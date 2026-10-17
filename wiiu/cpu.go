package wiiu

// The Espresso CPU: three PowerPC 750CL cores (the Wii's Broadway, faster and
// with more cache). This is an interpreter of the instruction set user
// programs run: the integer, branch and load/store instructions here, and the
// floating point and paired-single instructions in fpu.go.
//
// The CPU state lives in a Context (one per OS thread), so switching threads
// is switching which Context the core runs.
//
// System library calls: RetroPixels writes primary opcode 1 (unused by the
// PowerPC) at each library function's entry point, with the function's number
// in the low 26 bits. Running it calls the Go version of the function, which
// returns to LR like a real function would.

import (
	"fmt"
	"math/bits"
)

// Context is one thread's CPU registers.
type Context struct {
	GPR   [32]uint32
	FPR   [32]float64 // ps0 (the whole register for double instructions)
	PS1   [32]float64 // the second paired single
	CR    uint32
	LR    uint32
	CTR   uint32
	XER   uint32 // SO 0x80000000, OV 0x40000000, CA 0x20000000, byte count 0x7f
	FPSCR uint32
	PC    uint32
	GQR   [8]uint32

	ResAddr  uint32 // lwarx reservation
	ResValid bool
}

const (
	xerSO = 0x80000000
	xerOV = 0x40000000
	xerCA = 0x20000000
)

// CPU is one core.
type CPU struct {
	*Context
	Mem  *Memory
	Core int // 0..2 (what PIR / OSGetCoreId says)

	// HLE runs library function n (set by the machine).
	HLE func(c *CPU, n uint32)
	// TB is the time base (set by the machine).
	TB func() uint64

	Ran  uint64 // instructions run
	stop bool   // leave Run after this instruction
	Err  error  // why the program stopped (an instruction the CPU can't run)
	spr  [1024]uint32
}

// Stop makes Run return after the current instruction.
func (c *CPU) Stop() { c.stop = true }

// CPUError is a stop on an instruction the core can't run.
type CPUError struct {
	PC   uint32
	Inst uint32
	Msg  string
}

func (e *CPUError) Error() string {
	return fmt.Sprintf("%s at %08X (instruction %08X)", e.Msg, e.PC, e.Inst)
}

func (c *CPU) fail(inst uint32, msg string) {
	if c.Err == nil {
		c.Err = &CPUError{c.PC, inst, msg}
	}
	c.stop = true
}

// Run runs up to n instructions; it returns early when stopped.
func (c *CPU) Run(n int) {
	c.stop = false
	m := c.Mem
	for i := 0; i < n; i++ {
		pc := c.PC
		var inst uint32
		if p := m.pages[pc>>pageBits]; p != nil {
			o := pc & pageMask
			inst = uint32(p[o])<<24 | uint32(p[o+1])<<16 | uint32(p[o+2])<<8 | uint32(p[o+3])
		} else {
			inst = m.Read32(pc)
		}
		c.PC = pc + 4
		c.exec(inst)
		if c.stop {
			c.Ran += uint64(i + 1)
			return
		}
	}
	c.Ran += uint64(n)
}

// Step runs one instruction.
func (c *CPU) Step() {
	c.stop = false
	inst := c.Mem.Read32(c.PC)
	c.PC += 4
	c.exec(inst)
	c.Ran++
}

func (c *CPU) setCR0(v uint32) {
	var f uint32
	switch {
	case int32(v) < 0:
		f = 8
	case v != 0:
		f = 4
	default:
		f = 2
	}
	if c.XER&xerSO != 0 {
		f |= 1
	}
	c.CR = c.CR&0x0fffffff | f<<28
}

func (c *CPU) setCRField(n uint32, f uint32) {
	sh := (7 - n) * 4
	c.CR = c.CR&^(0xf<<sh) | (f&0xf)<<sh
}

func (c *CPU) crBit(b uint32) bool { return c.CR>>(31-b)&1 != 0 }

func (c *CPU) setCRBit(b uint32, v bool) {
	if v {
		c.CR |= 1 << (31 - b)
	} else {
		c.CR &^= 1 << (31 - b)
	}
}

func (c *CPU) cmpS(crf uint32, a, b int32) {
	var f uint32
	switch {
	case a < b:
		f = 8
	case a > b:
		f = 4
	default:
		f = 2
	}
	if c.XER&xerSO != 0 {
		f |= 1
	}
	c.setCRField(crf, f)
}

func (c *CPU) cmpU(crf uint32, a, b uint32) {
	var f uint32
	switch {
	case a < b:
		f = 8
	case a > b:
		f = 4
	default:
		f = 2
	}
	if c.XER&xerSO != 0 {
		f |= 1
	}
	c.setCRField(crf, f)
}

func (c *CPU) setCA(v bool) {
	if v {
		c.XER |= xerCA
	} else {
		c.XER &^= xerCA
	}
}

func (c *CPU) ca() uint32 { return c.XER >> 29 & 1 }

func (c *CPU) setOV(v bool) {
	if v {
		c.XER |= xerOV | xerSO
	} else {
		c.XER &^= xerOV
	}
}

// rotation mask for rlw* (bits numbered from the top)
func rotMask(mb, me uint32) uint32 {
	m1 := uint32(0xffffffff) >> mb
	m2 := uint32(0xffffffff) << (31 - me)
	if mb <= me {
		return m1 & m2
	}
	return m1 | m2
}

// branch condition for bc / bclr / bcctr
func (c *CPU) cond(bo, bi uint32, useCTR bool) bool {
	ctrOK := true
	if useCTR && bo&4 == 0 {
		c.CTR--
		ctrOK = (c.CTR != 0) != (bo&2 != 0)
	}
	condOK := bo&0x10 != 0 || c.crBit(bi) == (bo&8 != 0)
	return ctrOK && condOK
}

// effective address helpers
func (c *CPU) eaD(inst uint32) uint32 {
	ra := inst >> 16 & 31
	d := uint32(int32(int16(inst)))
	if ra == 0 {
		return d
	}
	return c.GPR[ra] + d
}

func (c *CPU) eaX(inst uint32) uint32 {
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	if ra == 0 {
		return c.GPR[rb]
	}
	return c.GPR[ra] + c.GPR[rb]
}

func (c *CPU) exec(inst uint32) {
	op := inst >> 26
	rd := inst >> 21 & 31
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	simm := uint32(int32(int16(inst)))
	uimm := inst & 0xffff
	m := c.Mem

	switch op {
	case 1: // a system library call
		c.HLE(c, inst&0x3ffffff)
	case 3: // twi
		if c.trap(rd, c.GPR[ra], simm) {
			c.fail(inst, "trap (twi)")
		}
	case 4:
		c.execPS(inst)
	case 7: // mulli
		c.GPR[rd] = uint32(int32(c.GPR[ra]) * int32(simm))
	case 8: // subfic
		a := c.GPR[ra]
		c.GPR[rd] = simm - a
		c.setCA(uint64(^a)+uint64(simm)+1 > 0xffffffff)
	case 10: // cmpli
		c.cmpU(rd>>2, c.GPR[ra], uimm)
	case 11: // cmpi
		c.cmpS(rd>>2, int32(c.GPR[ra]), int32(simm))
	case 12, 13: // addic, addic.
		a := c.GPR[ra]
		r := a + simm
		c.GPR[rd] = r
		c.setCA(r < a)
		if op == 13 {
			c.setCR0(r)
		}
	case 14: // addi
		if ra == 0 {
			c.GPR[rd] = simm
		} else {
			c.GPR[rd] = c.GPR[ra] + simm
		}
	case 15: // addis
		if ra == 0 {
			c.GPR[rd] = uimm << 16
		} else {
			c.GPR[rd] = c.GPR[ra] + uimm<<16
		}
	case 16: // bc
		bd := uint32(int32(int16(inst & 0xfffc)))
		if c.cond(rd, ra, true) {
			if inst&1 != 0 {
				c.LR = c.PC
			}
			if inst&2 != 0 {
				c.PC = bd
			} else {
				c.PC = c.PC - 4 + bd
			}
		}
	case 17: // sc
		c.fail(inst, "system call (sc)")
	case 18: // b
		li := inst & 0x03fffffc
		if li&0x02000000 != 0 {
			li |= 0xfc000000
		}
		if inst&1 != 0 {
			c.LR = c.PC
		}
		if inst&2 != 0 {
			c.PC = li
		} else {
			c.PC = c.PC - 4 + li
		}
	case 19:
		c.exec19(inst)
	case 20: // rlwimi
		sh, mb, me := rb, inst>>6&31, inst>>1&31
		mask := rotMask(mb, me)
		r := bits.RotateLeft32(c.GPR[rd], int(sh))&mask | c.GPR[ra]&^mask
		c.GPR[ra] = r
		if inst&1 != 0 {
			c.setCR0(r)
		}
	case 21: // rlwinm
		sh, mb, me := rb, inst>>6&31, inst>>1&31
		r := bits.RotateLeft32(c.GPR[rd], int(sh)) & rotMask(mb, me)
		c.GPR[ra] = r
		if inst&1 != 0 {
			c.setCR0(r)
		}
	case 23: // rlwnm
		mb, me := inst>>6&31, inst>>1&31
		r := bits.RotateLeft32(c.GPR[rd], int(c.GPR[rb]&31)) & rotMask(mb, me)
		c.GPR[ra] = r
		if inst&1 != 0 {
			c.setCR0(r)
		}
	case 24: // ori
		c.GPR[ra] = c.GPR[rd] | uimm
	case 25: // oris
		c.GPR[ra] = c.GPR[rd] | uimm<<16
	case 26: // xori
		c.GPR[ra] = c.GPR[rd] ^ uimm
	case 27: // xoris
		c.GPR[ra] = c.GPR[rd] ^ uimm<<16
	case 28: // andi.
		r := c.GPR[rd] & uimm
		c.GPR[ra] = r
		c.setCR0(r)
	case 29: // andis.
		r := c.GPR[rd] & (uimm << 16)
		c.GPR[ra] = r
		c.setCR0(r)
	case 31:
		c.exec31(inst)
	case 32: // lwz
		c.GPR[rd] = m.Read32(c.eaD(inst))
	case 33: // lwzu
		ea := c.GPR[ra] + simm
		c.GPR[rd] = m.Read32(ea)
		c.GPR[ra] = ea
	case 34: // lbz
		c.GPR[rd] = uint32(m.Read8(c.eaD(inst)))
	case 35: // lbzu
		ea := c.GPR[ra] + simm
		c.GPR[rd] = uint32(m.Read8(ea))
		c.GPR[ra] = ea
	case 36: // stw
		m.Write32(c.eaD(inst), c.GPR[rd])
	case 37: // stwu
		ea := c.GPR[ra] + simm
		m.Write32(ea, c.GPR[rd])
		c.GPR[ra] = ea
	case 38: // stb
		m.Write8(c.eaD(inst), uint8(c.GPR[rd]))
	case 39: // stbu
		ea := c.GPR[ra] + simm
		m.Write8(ea, uint8(c.GPR[rd]))
		c.GPR[ra] = ea
	case 40: // lhz
		c.GPR[rd] = uint32(m.Read16(c.eaD(inst)))
	case 41: // lhzu
		ea := c.GPR[ra] + simm
		c.GPR[rd] = uint32(m.Read16(ea))
		c.GPR[ra] = ea
	case 42: // lha
		c.GPR[rd] = uint32(int32(int16(m.Read16(c.eaD(inst)))))
	case 43: // lhau
		ea := c.GPR[ra] + simm
		c.GPR[rd] = uint32(int32(int16(m.Read16(ea))))
		c.GPR[ra] = ea
	case 44: // sth
		m.Write16(c.eaD(inst), uint16(c.GPR[rd]))
	case 45: // sthu
		ea := c.GPR[ra] + simm
		m.Write16(ea, uint16(c.GPR[rd]))
		c.GPR[ra] = ea
	case 46: // lmw
		ea := c.eaD(inst)
		for r := rd; r < 32; r++ {
			c.GPR[r] = m.Read32(ea)
			ea += 4
		}
	case 47: // stmw
		ea := c.eaD(inst)
		for r := rd; r < 32; r++ {
			m.Write32(ea, c.GPR[r])
			ea += 4
		}
	case 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61:
		c.execFPMem(inst)
	case 59:
		c.exec59(inst)
	case 63:
		c.exec63(inst)
	default:
		c.fail(inst, "unknown instruction")
	}
}

func (c *CPU) trap(to, a, b uint32) bool {
	sa, sb := int32(a), int32(b)
	return (to&16 != 0 && sa < sb) || (to&8 != 0 && sa > sb) || (to&4 != 0 && a == b) ||
		(to&2 != 0 && a < b) || (to&1 != 0 && a > b)
}

func (c *CPU) exec19(inst uint32) {
	bt := inst >> 21 & 31
	ba := inst >> 16 & 31
	bb := inst >> 11 & 31
	switch inst >> 1 & 0x3ff {
	case 0: // mcrf
		f := c.CR >> ((7 - ba>>2) * 4) & 0xf
		c.setCRField(bt>>2, f)
	case 16: // bclr
		target := c.LR &^ 3
		if c.cond(bt, ba, true) {
			if inst&1 != 0 {
				c.LR = c.PC
			}
			c.PC = target
		}
	case 33: // crnor
		c.setCRBit(bt, !(c.crBit(ba) || c.crBit(bb)))
	case 50: // rfi
		c.fail(inst, "rfi in a user program")
	case 129: // crandc
		c.setCRBit(bt, c.crBit(ba) && !c.crBit(bb))
	case 150: // isync
	case 193: // crxor
		c.setCRBit(bt, c.crBit(ba) != c.crBit(bb))
	case 225: // crnand
		c.setCRBit(bt, !(c.crBit(ba) && c.crBit(bb)))
	case 257: // crand
		c.setCRBit(bt, c.crBit(ba) && c.crBit(bb))
	case 289: // creqv
		c.setCRBit(bt, c.crBit(ba) == c.crBit(bb))
	case 417: // crorc
		c.setCRBit(bt, c.crBit(ba) || !c.crBit(bb))
	case 449: // cror
		c.setCRBit(bt, c.crBit(ba) || c.crBit(bb))
	case 528: // bcctr
		if c.cond(bt, ba, false) {
			if inst&1 != 0 {
				c.LR = c.PC
			}
			c.PC = c.CTR &^ 3
		}
	default:
		c.fail(inst, "unknown instruction")
	}
}

func (c *CPU) exec31(inst uint32) {
	rd := inst >> 21 & 31
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	rc := inst&1 != 0
	oe := inst&0x400 != 0
	m := c.Mem
	xo := inst >> 1 & 0x3ff

	// the arithmetic group has a 9-bit opcode with OE in front
	switch xo & 0x1ff {
	case 266: // add
		a, b := c.GPR[ra], c.GPR[rb]
		r := a + b
		if oe {
			c.setOV((a^r)&(b^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 10: // addc
		a, b := c.GPR[ra], c.GPR[rb]
		r := a + b
		c.setCA(r < a)
		if oe {
			c.setOV((a^r)&(b^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 138: // adde
		a, b := c.GPR[ra], c.GPR[rb]
		s := uint64(a) + uint64(b) + uint64(c.ca())
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV((a^r)&(b^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 202: // addze
		a := c.GPR[ra]
		s := uint64(a) + uint64(c.ca())
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV(^a&r>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 234: // addme
		a := c.GPR[ra]
		s := uint64(a) + uint64(c.ca()) + 0xffffffff
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV((a^r)&(0xffffffff^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 40: // subf
		a, b := c.GPR[ra], c.GPR[rb]
		r := b - a
		if oe {
			c.setOV((^a^r)&(b^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 8: // subfc
		a, b := c.GPR[ra], c.GPR[rb]
		s := uint64(^a) + uint64(b) + 1
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV((^a^r)&(b^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 136: // subfe
		a, b := c.GPR[ra], c.GPR[rb]
		s := uint64(^a) + uint64(b) + uint64(c.ca())
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV((^a^r)&(b^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 200: // subfze
		a := c.GPR[ra]
		s := uint64(^a) + uint64(c.ca())
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV(a&r>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 232: // subfme
		a := c.GPR[ra]
		s := uint64(^a) + uint64(c.ca()) + 0xffffffff
		r := uint32(s)
		c.setCA(s > 0xffffffff)
		if oe {
			c.setOV((^a^r)&(0xffffffff^r)>>31 != 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 104: // neg
		a := c.GPR[ra]
		r := -a
		if oe {
			c.setOV(a == 0x80000000)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 235: // mullw
		p := int64(int32(c.GPR[ra])) * int64(int32(c.GPR[rb]))
		r := uint32(p)
		if oe {
			c.setOV(p != int64(int32(r)))
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 75: // mulhw
		r := uint32(int64(int32(c.GPR[ra])) * int64(int32(c.GPR[rb])) >> 32)
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 11: // mulhwu
		r := uint32(uint64(c.GPR[ra]) * uint64(c.GPR[rb]) >> 32)
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 491: // divw
		a, b := int32(c.GPR[ra]), int32(c.GPR[rb])
		var r uint32
		bad := b == 0 || (a == -0x80000000 && b == -1)
		if bad {
			if a < 0 && b == 0 {
				r = 0xffffffff
			} else if b == 0 {
				r = 0
			} else {
				r = 0xffffffff // 0x80000000 / -1 on the 750
			}
		} else {
			r = uint32(a / b)
		}
		if oe {
			c.setOV(bad)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 459: // divwu
		a, b := c.GPR[ra], c.GPR[rb]
		var r uint32
		if b != 0 {
			r = a / b
		}
		if oe {
			c.setOV(b == 0)
		}
		c.GPR[rd] = r
		if rc {
			c.setCR0(r)
		}
		return
	}

	switch xo {
	case 0: // cmp
		c.cmpS(rd>>2, int32(c.GPR[ra]), int32(c.GPR[rb]))
	case 32: // cmpl
		c.cmpU(rd>>2, c.GPR[ra], c.GPR[rb])
	case 4: // tw
		if c.trap(rd, c.GPR[ra], c.GPR[rb]) {
			c.fail(inst, "trap (tw)")
		}
	case 19: // mfcr
		c.GPR[rd] = c.CR
	case 20: // lwarx
		ea := c.eaX(inst)
		c.GPR[rd] = m.Read32(ea)
		c.ResAddr, c.ResValid = ea, true
	case 23: // lwzx
		c.GPR[rd] = m.Read32(c.eaX(inst))
	case 24: // slw
		s := c.GPR[rb] & 63
		var r uint32
		if s < 32 {
			r = c.GPR[rd] << s
		}
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 26: // cntlzw
		r := uint32(bits.LeadingZeros32(c.GPR[rd]))
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 28: // and
		r := c.GPR[rd] & c.GPR[rb]
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 54: // dcbst
	case 55: // lwzux
		ea := c.GPR[ra] + c.GPR[rb]
		c.GPR[rd] = m.Read32(ea)
		c.GPR[ra] = ea
	case 60: // andc
		r := c.GPR[rd] &^ c.GPR[rb]
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 83: // mfmsr
		c.GPR[rd] = 0x0000b032
	case 86: // dcbf
	case 87: // lbzx
		c.GPR[rd] = uint32(m.Read8(c.eaX(inst)))
	case 119: // lbzux
		ea := c.GPR[ra] + c.GPR[rb]
		c.GPR[rd] = uint32(m.Read8(ea))
		c.GPR[ra] = ea
	case 124: // nor
		r := ^(c.GPR[rd] | c.GPR[rb])
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 144: // mtcrf
		crm := inst >> 12 & 0xff
		var mask uint32
		for i := uint32(0); i < 8; i++ {
			if crm&(0x80>>i) != 0 {
				mask |= 0xf0000000 >> (i * 4)
			}
		}
		c.CR = c.CR&^mask | c.GPR[rd]&mask
	case 146: // mtmsr
	case 150: // stwcx.
		ea := c.eaX(inst)
		ok := c.ResValid && c.ResAddr == ea
		c.ResValid = false
		f := uint32(0)
		if ok {
			m.Write32(ea, c.GPR[rd])
			f = 2
		}
		if c.XER&xerSO != 0 {
			f |= 1
		}
		c.setCRField(0, f)
	case 151: // stwx
		m.Write32(c.eaX(inst), c.GPR[rd])
	case 183: // stwux
		ea := c.GPR[ra] + c.GPR[rb]
		m.Write32(ea, c.GPR[rd])
		c.GPR[ra] = ea
	case 215: // stbx
		m.Write8(c.eaX(inst), uint8(c.GPR[rd]))
	case 246: // dcbtst
	case 247: // stbux
		ea := c.GPR[ra] + c.GPR[rb]
		m.Write8(ea, uint8(c.GPR[rd]))
		c.GPR[ra] = ea
	case 278: // dcbt
	case 279: // lhzx
		c.GPR[rd] = uint32(m.Read16(c.eaX(inst)))
	case 284: // eqv
		r := ^(c.GPR[rd] ^ c.GPR[rb])
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 311: // lhzux
		ea := c.GPR[ra] + c.GPR[rb]
		c.GPR[rd] = uint32(m.Read16(ea))
		c.GPR[ra] = ea
	case 316: // xor
		r := c.GPR[rd] ^ c.GPR[rb]
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 339: // mfspr
		c.GPR[rd] = c.mfspr(inst>>16&31 | (inst>>11&31)<<5)
	case 343: // lhax
		c.GPR[rd] = uint32(int32(int16(m.Read16(c.eaX(inst)))))
	case 371: // mftb
		tb := c.TB()
		if inst>>16&31|(inst>>11&31)<<5 == 269 {
			c.GPR[rd] = uint32(tb >> 32)
		} else {
			c.GPR[rd] = uint32(tb)
		}
	case 375: // lhaux
		ea := c.GPR[ra] + c.GPR[rb]
		c.GPR[rd] = uint32(int32(int16(m.Read16(ea))))
		c.GPR[ra] = ea
	case 407: // sthx
		m.Write16(c.eaX(inst), uint16(c.GPR[rd]))
	case 412: // orc
		r := c.GPR[rd] | ^c.GPR[rb]
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 439: // sthux
		ea := c.GPR[ra] + c.GPR[rb]
		m.Write16(ea, uint16(c.GPR[rd]))
		c.GPR[ra] = ea
	case 444: // or
		r := c.GPR[rd] | c.GPR[rb]
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 467: // mtspr
		c.mtspr(inst>>16&31|(inst>>11&31)<<5, c.GPR[rd])
	case 470: // dcbi
	case 476: // nand
		r := ^(c.GPR[rd] & c.GPR[rb])
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 512: // mcrxr
		c.setCRField(rd>>2, c.XER>>28)
		c.XER &^= 0xf0000000
	case 533: // lswx
		c.loadString(c.eaX(inst), rd, c.XER&0x7f)
	case 534: // lwbrx
		c.GPR[rd] = bits.ReverseBytes32(m.Read32(c.eaX(inst)))
	case 535, 567, 599, 631, 663, 695, 727, 759, 983:
		c.execFPMemX(inst)
	case 536: // srw
		s := c.GPR[rb] & 63
		var r uint32
		if s < 32 {
			r = c.GPR[rd] >> s
		}
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 566: // tlbsync
	case 597: // lswi
		n := rb
		if n == 0 {
			n = 32
		}
		ea := uint32(0)
		if ra != 0 {
			ea = c.GPR[ra]
		}
		c.loadString(ea, rd, n)
	case 598: // sync
	case 661: // stswx
		c.storeString(c.eaX(inst), rd, c.XER&0x7f)
	case 662: // stwbrx
		m.Write32(c.eaX(inst), bits.ReverseBytes32(c.GPR[rd]))
	case 725: // stswi
		n := rb
		if n == 0 {
			n = 32
		}
		ea := uint32(0)
		if ra != 0 {
			ea = c.GPR[ra]
		}
		c.storeString(ea, rd, n)
	case 790: // lhbrx
		c.GPR[rd] = uint32(bits.ReverseBytes16(m.Read16(c.eaX(inst))))
	case 792: // sraw
		s := c.GPR[rb] & 63
		v := int32(c.GPR[rd])
		var r int32
		var ca bool
		if s >= 32 {
			r = v >> 31
			ca = v < 0
		} else {
			r = v >> s
			ca = v < 0 && uint32(v)<<(32-s) != 0 && s != 0
		}
		c.GPR[ra] = uint32(r)
		c.setCA(ca)
		if rc {
			c.setCR0(uint32(r))
		}
	case 824: // srawi
		s := rb
		v := int32(c.GPR[rd])
		r := v >> s
		c.GPR[ra] = uint32(r)
		c.setCA(v < 0 && s != 0 && uint32(v)<<(32-s) != 0)
		if rc {
			c.setCR0(uint32(r))
		}
	case 854: // eieio
	case 918: // sthbrx
		m.Write16(c.eaX(inst), bits.ReverseBytes16(uint16(c.GPR[rd])))
	case 922: // extsh
		r := uint32(int32(int16(c.GPR[rd])))
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 954: // extsb
		r := uint32(int32(int8(c.GPR[rd])))
		c.GPR[ra] = r
		if rc {
			c.setCR0(r)
		}
	case 982: // icbi
	case 1014: // dcbz
		ea := c.eaX(inst) &^ 31
		m.Fill(ea, 0, 32)
	default:
		c.fail(inst, "unknown instruction")
	}
}

func (c *CPU) loadString(ea, rd, n uint32) {
	r := rd
	for i := uint32(0); i < n; i++ {
		if i%4 == 0 {
			c.GPR[r] = 0
		}
		c.GPR[r] |= uint32(c.Mem.Read8(ea+i)) << (24 - 8*(i%4))
		if i%4 == 3 {
			r = (r + 1) & 31
		}
	}
}

func (c *CPU) storeString(ea, rs, n uint32) {
	r := rs
	for i := uint32(0); i < n; i++ {
		c.Mem.Write8(ea+i, uint8(c.GPR[r]>>(24-8*(i%4))))
		if i%4 == 3 {
			r = (r + 1) & 31
		}
	}
}

func (c *CPU) mfspr(n uint32) uint32 {
	switch n {
	case 1:
		return c.XER
	case 8:
		return c.LR
	case 9:
		return c.CTR
	case 268, 284:
		return uint32(c.TB())
	case 269, 285:
		return uint32(c.TB() >> 32)
	case 912, 913, 914, 915, 916, 917, 918, 919:
		return c.GQR[n-912]
	case 896, 897, 898, 899, 900, 901, 902, 903: // UGQR (user-mode copies)
		return c.GQR[n-896]
	case 1007, 1023: // UPIR / PIR: which core
		return uint32(c.Core)
	case 920: // HID2: paired singles on, write-gather, locked cache
		return 0xa0000000
	}
	return c.spr[n]
}

func (c *CPU) mtspr(n, v uint32) {
	switch n {
	case 1:
		c.XER = v
	case 8:
		c.LR = v
	case 9:
		c.CTR = v
	case 912, 913, 914, 915, 916, 917, 918, 919:
		c.GQR[n-912] = v
	case 896, 897, 898, 899, 900, 901, 902, 903:
		c.GQR[n-896] = v
	default:
		c.spr[n] = v
	}
}
