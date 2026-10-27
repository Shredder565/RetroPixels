package ps3

// The Cell's PPU: a 64-bit PowerPC with the AltiVec (VMX) vector unit. This
// is an interpreter of the instructions user programs run: the integer,
// branch and load/store instructions here, floating point in ppu_fpu.go and
// the vector unit in ppu_vmx.go.
//
// Each PPU thread has its own registers (PPUState); the machine switches
// threads by switching which state the interpreter runs.
//
// System functions: RetroPixels puts primary opcode 1 (unused on the PPU)
// with the function's number in the low 26 bits at each system function's
// entry point. Running it calls the Go version of the function, which
// returns to LR like a real function would.

import (
	"fmt"
	"math/bits"
)

// Vec is a vector register: four words, word 0 first in memory (big-endian).
type Vec [4]uint32

// PPUState is one thread's registers.
type PPUState struct {
	GPR    [32]uint64
	FPR    [32]uint64 // the bits of each double
	VR     [32]Vec
	CR     uint32
	LR     uint64
	CTR    uint64
	XER    uint64 // SO 0x80000000, OV 0x40000000, CA 0x20000000, byte count 0x7f
	FPSCR  uint32
	VSCR   uint32 // NJ 0x10000, SAT 1
	VRSAVE uint32
	PC     uint32

	// the FPSCR result flags (FPRF) are worked out when FPSCR is read,
	// from the last result (fpLast) if fpDirty
	fpLast  uint64
	fpDirty uint32

	resAddr  uint32 // lwarx/ldarx reservation (128-byte line)
	resVal   uint64
	resValid bool
	resSize  int
}

const (
	xerSO = 0x80000000
	xerOV = 0x40000000
	xerCA = 0x20000000
)

// PPU runs one thread at a time.
type PPU struct {
	*PPUState
	Mem *Memory

	// HLE runs system function n.
	HLE func(c *PPU, n uint32)
	// Syscall runs LV2 system call r11.
	Syscall func(c *PPU)
	// TB is the time base (79.8 MHz).
	TB func() uint64

	Ran  uint64
	stop bool
	Err  error

	// Watch: debugging, functions called when the PC reaches an address
	Watch map[uint32]func(c *PPU)

	jit *JIT // the recompiler, or nil
}

// Stop makes Run return after the current instruction.
func (c *PPU) Stop() { c.stop = true }

// CPUError is a stop on an instruction the PPU can't run.
type CPUError struct {
	PC   uint32
	Inst uint32
	Msg  string
}

func (e *CPUError) Error() string {
	return fmt.Sprintf("%s at %08X (instruction %08X)", e.Msg, e.PC, e.Inst)
}

func (c *PPU) fail(inst uint32, msg string) {
	if c.Err == nil {
		c.Err = &CPUError{c.PC - 4, inst, msg}
	}
	c.stop = true
}

// Run runs up to n instructions; it returns early when an instruction stops it.
func (c *PPU) Run(n int) {
	c.stop = false
	if c.jit != nil && c.Watch == nil {
		c.jit.run(c, n)
		return
	}
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
		if c.Watch != nil {
			if f := c.Watch[pc]; f != nil {
				f(c)
			}
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
func (c *PPU) Step() { c.Run(1) }

func rD(i uint32) uint32   { return i >> 21 & 31 }
func rA(i uint32) uint32   { return i >> 16 & 31 }
func rB(i uint32) uint32   { return i >> 11 & 31 }
func rC(i uint32) uint32   { return i >> 6 & 31 }
func simm(i uint32) uint64 { return uint64(int64(int16(i))) }

// ea forms: (rA|0) + d
func (c *PPU) eaD(i uint32) uint32 {
	a := rA(i)
	if a == 0 {
		return uint32(simm(i))
	}
	return uint32(c.GPR[a] + simm(i))
}

func (c *PPU) eaDS(i uint32) uint32 {
	d := uint64(int64(int16(i & 0xfffc)))
	a := rA(i)
	if a == 0 {
		return uint32(d)
	}
	return uint32(c.GPR[a] + d)
}

func (c *PPU) eaX(i uint32) uint32 {
	a := rA(i)
	if a == 0 {
		return uint32(c.GPR[rB(i)])
	}
	return uint32(c.GPR[a] + c.GPR[rB(i)])
}

// setCR0 sets CR0 from a 64-bit result.
func (c *PPU) setCR0(v uint64) {
	var f uint32
	switch {
	case int64(v) < 0:
		f = 8
	case int64(v) > 0:
		f = 4
	default:
		f = 2
	}
	if c.XER&xerSO != 0 {
		f |= 1
	}
	c.CR = c.CR&0x0fffffff | f<<28
}

func (c *PPU) setCRField(n uint32, f uint32) {
	sh := 28 - 4*n
	c.CR = c.CR&^(0xf<<sh) | (f&0xf)<<sh
}

func (c *PPU) cmpS(n uint32, a, b int64) {
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
	c.setCRField(n, f)
}

func (c *PPU) cmpU(n uint32, a, b uint64) {
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
	c.setCRField(n, f)
}

func (c *PPU) crBit(b uint32) bool { return c.CR>>(31-b)&1 != 0 }
func (c *PPU) setCRBit(b uint32, v bool) {
	if v {
		c.CR |= 1 << (31 - b)
	} else {
		c.CR &^= 1 << (31 - b)
	}
}

func (c *PPU) setCA(v bool) {
	if v {
		c.XER |= xerCA
	} else {
		c.XER &^= xerCA
	}
}

func (c *PPU) ca() uint64 { return c.XER >> 29 & 1 }

func (c *PPU) setOV(v bool) {
	if v {
		c.XER |= xerOV | xerSO
	} else {
		c.XER &^= xerOV
	}
}

// mask64 is the PowerPC MASK(mb, me) over 64 bits (bit 0 = the top bit).
func mask64(mb, me uint32) uint64 {
	a := ^uint64(0) >> mb
	b := ^uint64(0) << (63 - me)
	if mb <= me {
		return a & b
	}
	return a | b
}

func rotl32(v uint64, n uint32) uint64 {
	x := uint64(uint32(v))
	x |= x << 32
	return bits.RotateLeft64(x, int(n&31))
}

// condOK evaluates a bc's BO/BI (and decrements CTR when asked).
func (c *PPU) condOK(bo, bi uint32) bool {
	if bo&4 == 0 {
		c.CTR--
	}
	ctrOK := bo&4 != 0 || ((c.CTR != 0) != (bo&2 != 0))
	condOK := bo&16 != 0 || c.crBit(bi) == (bo&8 != 0)
	return ctrOK && condOK
}

func (c *PPU) exec(i uint32) {
	switch i >> 26 {
	case 1:
		// RetroPixels' system function call
		if c.HLE != nil {
			c.HLE(c, i&0x3ffffff)
		} else {
			c.fail(i, "system function call with no HLE")
		}
	case 2: // tdi
		if c.trap(i>>21&31, c.GPR[rA(i)], simm(i)) {
			c.fail(i, "tdi trap")
		}
	case 3: // twi
		if c.trap(i>>21&31, uint64(int64(int32(c.GPR[rA(i)]))), simm(i)) {
			c.fail(i, "twi trap")
		}
	case 4:
		c.execVMX(i)
	case 7: // mulli
		c.GPR[rD(i)] = uint64(int64(c.GPR[rA(i)]) * int64(simm(i)))
	case 8: // subfic
		r, cy := bits.Add64(^c.GPR[rA(i)], simm(i), 1)
		c.GPR[rD(i)] = r
		c.setCA(cy != 0)
	case 10: // cmpli
		if i&(1<<21) != 0 {
			c.cmpU(i>>23&7, c.GPR[rA(i)], uint64(i&0xffff))
		} else {
			c.cmpU(i>>23&7, uint64(uint32(c.GPR[rA(i)])), uint64(i&0xffff))
		}
	case 11: // cmpi
		if i&(1<<21) != 0 {
			c.cmpS(i>>23&7, int64(c.GPR[rA(i)]), int64(simm(i)))
		} else {
			c.cmpS(i>>23&7, int64(int32(c.GPR[rA(i)])), int64(simm(i)))
		}
	case 12, 13: // addic, addic.
		a := c.GPR[rA(i)]
		s, cy := bits.Add64(a, simm(i), 0)
		c.GPR[rD(i)] = s
		c.setCA(cy != 0)
		if i>>26 == 13 {
			c.setCR0(s)
		}
	case 14: // addi
		if rA(i) == 0 {
			c.GPR[rD(i)] = simm(i)
		} else {
			c.GPR[rD(i)] = c.GPR[rA(i)] + simm(i)
		}
	case 15: // addis
		v := uint64(int64(int32(i << 16)))
		if rA(i) == 0 {
			c.GPR[rD(i)] = v
		} else {
			c.GPR[rD(i)] = c.GPR[rA(i)] + v
		}
	case 16: // bc
		bo, bi := i>>21&31, i>>16&31
		pc := c.PC - 4
		if i&1 != 0 {
			c.LR = uint64(c.PC)
		}
		if c.condOK(bo, bi) {
			off := uint32(int32(int16(i & 0xfffc)))
			if i&2 != 0 {
				c.PC = off
			} else {
				c.PC = pc + off
			}
		}
	case 17: // sc
		if c.Syscall != nil {
			c.Syscall(c)
		} else {
			c.fail(i, "system call with no kernel")
		}
	case 18: // b
		off := i & 0x3fffffc
		if off&0x2000000 != 0 {
			off |= 0xfc000000
		}
		pc := c.PC - 4
		if i&1 != 0 {
			c.LR = uint64(c.PC)
		}
		if i&2 != 0 {
			c.PC = off
		} else {
			c.PC = pc + off
		}
	case 19:
		c.exec19(i)
	case 20: // rlwimi
		sh, mb, me := rB(i), i>>6&31, i>>1&31
		m := mask64(mb+32, me+32)
		r := rotl32(c.GPR[rD(i)], sh)
		v := r&m | c.GPR[rA(i)]&^m
		c.GPR[rA(i)] = v
		if i&1 != 0 {
			c.setCR0(v)
		}
	case 21: // rlwinm
		sh, mb, me := rB(i), i>>6&31, i>>1&31
		v := rotl32(c.GPR[rD(i)], sh) & mask64(mb+32, me+32)
		c.GPR[rA(i)] = v
		if i&1 != 0 {
			c.setCR0(v)
		}
	case 23: // rlwnm
		mb, me := i>>6&31, i>>1&31
		v := rotl32(c.GPR[rD(i)], uint32(c.GPR[rB(i)]&31)) & mask64(mb+32, me+32)
		c.GPR[rA(i)] = v
		if i&1 != 0 {
			c.setCR0(v)
		}
	case 24: // ori
		c.GPR[rA(i)] = c.GPR[rD(i)] | uint64(i&0xffff)
	case 25: // oris
		c.GPR[rA(i)] = c.GPR[rD(i)] | uint64(i&0xffff)<<16
	case 26: // xori
		c.GPR[rA(i)] = c.GPR[rD(i)] ^ uint64(i&0xffff)
	case 27: // xoris
		c.GPR[rA(i)] = c.GPR[rD(i)] ^ uint64(i&0xffff)<<16
	case 28: // andi.
		v := c.GPR[rD(i)] & uint64(i&0xffff)
		c.GPR[rA(i)] = v
		c.setCR0(v)
	case 29: // andis.
		v := c.GPR[rD(i)] & (uint64(i&0xffff) << 16)
		c.GPR[rA(i)] = v
		c.setCR0(v)
	case 30:
		c.exec30(i)
	case 31:
		c.exec31(i)
	case 32: // lwz
		c.GPR[rD(i)] = uint64(c.Mem.Read32(c.eaD(i)))
	case 33: // lwzu
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.GPR[rD(i)] = uint64(c.Mem.Read32(ea))
		c.GPR[rA(i)] = uint64(ea)
	case 34: // lbz
		c.GPR[rD(i)] = uint64(c.Mem.Read8(c.eaD(i)))
	case 35: // lbzu
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.GPR[rD(i)] = uint64(c.Mem.Read8(ea))
		c.GPR[rA(i)] = uint64(ea)
	case 36: // stw
		c.Mem.Write32(c.eaD(i), uint32(c.GPR[rD(i)]))
	case 37: // stwu
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.Mem.Write32(ea, uint32(c.GPR[rD(i)]))
		c.GPR[rA(i)] = uint64(ea)
	case 38: // stb
		c.Mem.Write8(c.eaD(i), uint8(c.GPR[rD(i)]))
	case 39: // stbu
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.Mem.Write8(ea, uint8(c.GPR[rD(i)]))
		c.GPR[rA(i)] = uint64(ea)
	case 40: // lhz
		c.GPR[rD(i)] = uint64(c.Mem.Read16(c.eaD(i)))
	case 41: // lhzu
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.GPR[rD(i)] = uint64(c.Mem.Read16(ea))
		c.GPR[rA(i)] = uint64(ea)
	case 42: // lha
		c.GPR[rD(i)] = uint64(int64(int16(c.Mem.Read16(c.eaD(i)))))
	case 43: // lhau
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.GPR[rD(i)] = uint64(int64(int16(c.Mem.Read16(ea))))
		c.GPR[rA(i)] = uint64(ea)
	case 44: // sth
		c.Mem.Write16(c.eaD(i), uint16(c.GPR[rD(i)]))
	case 45: // sthu
		ea := uint32(c.GPR[rA(i)] + simm(i))
		c.Mem.Write16(ea, uint16(c.GPR[rD(i)]))
		c.GPR[rA(i)] = uint64(ea)
	case 46: // lmw
		ea := c.eaD(i)
		for r := rD(i); r < 32; r++ {
			c.GPR[r] = uint64(c.Mem.Read32(ea))
			ea += 4
		}
	case 47: // stmw
		ea := c.eaD(i)
		for r := rD(i); r < 32; r++ {
			c.Mem.Write32(ea, uint32(c.GPR[r]))
			ea += 4
		}
	case 48, 49, 50, 51, 52, 53, 54, 55:
		c.execFPMem(i)
	case 58:
		ea := c.eaDS(i)
		switch i & 3 {
		case 0: // ld
			c.GPR[rD(i)] = c.Mem.Read64(ea)
		case 1: // ldu
			ea = uint32(c.GPR[rA(i)] + uint64(int64(int16(i&0xfffc))))
			c.GPR[rD(i)] = c.Mem.Read64(ea)
			c.GPR[rA(i)] = uint64(ea)
		case 2: // lwa
			c.GPR[rD(i)] = uint64(int64(int32(c.Mem.Read32(ea))))
		default:
			c.fail(i, "unknown instruction")
		}
	case 59:
		c.exec59(i)
	case 62:
		ea := c.eaDS(i)
		switch i & 3 {
		case 0: // std
			c.Mem.Write64(ea, c.GPR[rD(i)])
		case 1: // stdu
			ea = uint32(c.GPR[rA(i)] + uint64(int64(int16(i&0xfffc))))
			c.Mem.Write64(ea, c.GPR[rD(i)])
			c.GPR[rA(i)] = uint64(ea)
		default:
			c.fail(i, "unknown instruction")
		}
	case 63:
		c.exec63(i)
	default:
		c.fail(i, "unknown instruction")
	}
}

func (c *PPU) trap(to uint32, a, b uint64) bool {
	sa, sb := int64(a), int64(b)
	return (to&16 != 0 && sa < sb) || (to&8 != 0 && sa > sb) || (to&4 != 0 && a == b) ||
		(to&2 != 0 && a < b) || (to&1 != 0 && a > b)
}

func (c *PPU) exec19(i uint32) {
	switch i >> 1 & 0x3ff {
	case 0: // mcrf
		f := c.CR >> (28 - 4*(i>>18&7)) & 0xf
		c.setCRField(i>>23&7, f)
	case 16: // bclr
		bo, bi := i>>21&31, i>>16&31
		target := uint32(c.LR) &^ 3
		if c.condOK(bo, bi) {
			if i&1 != 0 {
				c.LR = uint64(c.PC)
			}
			c.PC = target
		} else if i&1 != 0 {
			c.LR = uint64(c.PC)
		}
	case 528: // bcctr
		bo, bi := i>>21&31, i>>16&31
		if i&1 != 0 {
			c.LR = uint64(c.PC)
		}
		if bo&16 != 0 || c.crBit(bi) == (bo&8 != 0) {
			c.PC = uint32(c.CTR) &^ 3
		}
	case 150: // isync
	case 33: // crnor
		c.setCRBit(rD(i), !(c.crBit(rA(i)) || c.crBit(rB(i))))
	case 129: // crandc
		c.setCRBit(rD(i), c.crBit(rA(i)) && !c.crBit(rB(i)))
	case 193: // crxor
		c.setCRBit(rD(i), c.crBit(rA(i)) != c.crBit(rB(i)))
	case 225: // crnand
		c.setCRBit(rD(i), !(c.crBit(rA(i)) && c.crBit(rB(i))))
	case 257: // crand
		c.setCRBit(rD(i), c.crBit(rA(i)) && c.crBit(rB(i)))
	case 289: // creqv
		c.setCRBit(rD(i), c.crBit(rA(i)) == c.crBit(rB(i)))
	case 417: // crorc
		c.setCRBit(rD(i), c.crBit(rA(i)) || !c.crBit(rB(i)))
	case 449: // cror
		c.setCRBit(rD(i), c.crBit(rA(i)) || c.crBit(rB(i)))
	default:
		c.fail(i, "unknown instruction")
	}
}

func (c *PPU) exec30(i uint32) {
	s, a := rD(i), rA(i)
	sh := rB(i) | (i>>1&1)<<5
	mb := i>>6&31 | (i>>5&1)<<5
	var v uint64
	switch i >> 2 & 7 {
	case 0: // rldicl
		v = bits.RotateLeft64(c.GPR[s], int(sh)) & mask64(mb, 63)
	case 1: // rldicr
		v = bits.RotateLeft64(c.GPR[s], int(sh)) & mask64(0, mb)
	case 2: // rldic
		v = bits.RotateLeft64(c.GPR[s], int(sh)) & mask64(mb, 63-sh)
	case 3: // rldimi
		m := mask64(mb, 63-sh)
		v = bits.RotateLeft64(c.GPR[s], int(sh))&m | c.GPR[a]&^m
	default:
		n := int(c.GPR[rB(i)] & 63)
		switch i >> 1 & 15 {
		case 8: // rldcl
			v = bits.RotateLeft64(c.GPR[s], n) & mask64(mb, 63)
		case 9: // rldcr
			v = bits.RotateLeft64(c.GPR[s], n) & mask64(0, mb)
		default:
			c.fail(i, "unknown instruction")
			return
		}
	}
	c.GPR[a] = v
	if i&1 != 0 {
		c.setCR0(v)
	}
}

func (c *PPU) exec31(i uint32) {
	d, a, b := rD(i), rA(i), rB(i)
	rc := i&1 != 0
	xo := i >> 1 & 0x3ff
	oe := false
	// XO-form arithmetic: the OE bit is the top bit of the 10-bit field
	switch xo & 0x1ff {
	case 8, 10, 11, 40, 104, 136, 138, 200, 202, 232, 233, 234, 235, 266, 457, 459, 489, 491, 9, 73, 75:
		oe = xo&0x200 != 0
		xo &= 0x1ff
	}
	switch xo {
	case 0: // cmp
		if i&(1<<21) != 0 {
			c.cmpS(d>>2, int64(c.GPR[a]), int64(c.GPR[b]))
		} else {
			c.cmpS(d>>2, int64(int32(c.GPR[a])), int64(int32(c.GPR[b])))
		}
		return
	case 32: // cmpl
		if i&(1<<21) != 0 {
			c.cmpU(d>>2, c.GPR[a], c.GPR[b])
		} else {
			c.cmpU(d>>2, uint64(uint32(c.GPR[a])), uint64(uint32(c.GPR[b])))
		}
		return
	case 4: // tw
		if c.trap(d, uint64(int64(int32(c.GPR[a]))), uint64(int64(int32(c.GPR[b])))) {
			c.fail(i, "tw trap")
		}
		return
	case 68: // td
		if c.trap(d, c.GPR[a], c.GPR[b]) {
			c.fail(i, "td trap")
		}
		return
	case 8: // subfc
		r, cy := bits.Add64(^c.GPR[a], c.GPR[b], 1)
		if oe {
			c.setOV(subOverflow(c.GPR[b], c.GPR[a], r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 10: // addc
		r, cy := bits.Add64(c.GPR[a], c.GPR[b], 0)
		if oe {
			c.setOV(addOverflow(c.GPR[a], c.GPR[b], r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 9: // mulhdu
		hi, _ := bits.Mul64(c.GPR[a], c.GPR[b])
		c.GPR[d] = hi
		if rc {
			c.setCR0(hi)
		}
		return
	case 73: // mulhd
		hi := mulhs64(int64(c.GPR[a]), int64(c.GPR[b]))
		c.GPR[d] = uint64(hi)
		if rc {
			c.setCR0(uint64(hi))
		}
		return
	case 11: // mulhwu
		v := uint64(uint32(c.GPR[a])) * uint64(uint32(c.GPR[b])) >> 32
		c.GPR[d] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 75: // mulhw
		v := uint64(int64(int32(c.GPR[a])) * int64(int32(c.GPR[b])) >> 32)
		c.GPR[d] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 19: // mfcr / mfocrf
		c.GPR[d] = uint64(c.CR)
		return
	case 144: // mtcrf / mtocrf
		fxm := i >> 12 & 0xff
		var m uint32
		for k := 0; k < 8; k++ {
			if fxm&(0x80>>k) != 0 {
				m |= 0xf0000000 >> (4 * k)
			}
		}
		c.CR = c.CR&^m | uint32(c.GPR[d])&m
		return
	case 20: // lwarx
		ea := c.eaX(i)
		v := c.Mem.Read32(ea)
		c.GPR[d] = uint64(v)
		c.resAddr, c.resVal, c.resValid, c.resSize = ea, uint64(v), true, 4
		return
	case 84: // ldarx
		ea := c.eaX(i)
		v := c.Mem.Read64(ea)
		c.GPR[d] = v
		c.resAddr, c.resVal, c.resValid, c.resSize = ea, v, true, 8
		return
	case 150: // stwcx.
		ea := c.eaX(i)
		ok := c.resValid && c.resAddr == ea && c.resSize == 4 && uint64(c.Mem.Read32(ea)) == c.resVal
		if ok {
			c.Mem.Write32(ea, uint32(c.GPR[d]))
		}
		c.resValid = false
		f := uint32(0)
		if ok {
			f = 2
		}
		if c.XER&xerSO != 0 {
			f |= 1
		}
		c.setCRField(0, f)
		return
	case 214: // stdcx.
		ea := c.eaX(i)
		ok := c.resValid && c.resAddr == ea && c.resSize == 8 && c.Mem.Read64(ea) == c.resVal
		if ok {
			c.Mem.Write64(ea, c.GPR[d])
		}
		c.resValid = false
		f := uint32(0)
		if ok {
			f = 2
		}
		if c.XER&xerSO != 0 {
			f |= 1
		}
		c.setCRField(0, f)
		return
	case 21: // ldx
		c.GPR[d] = c.Mem.Read64(c.eaX(i))
		return
	case 53: // ldux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.GPR[d] = c.Mem.Read64(ea)
		c.GPR[a] = uint64(ea)
		return
	case 23: // lwzx
		c.GPR[d] = uint64(c.Mem.Read32(c.eaX(i)))
		return
	case 55: // lwzux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.GPR[d] = uint64(c.Mem.Read32(ea))
		c.GPR[a] = uint64(ea)
		return
	case 341: // lwax
		c.GPR[d] = uint64(int64(int32(c.Mem.Read32(c.eaX(i)))))
		return
	case 373: // lwaux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.GPR[d] = uint64(int64(int32(c.Mem.Read32(ea))))
		c.GPR[a] = uint64(ea)
		return
	case 87: // lbzx
		c.GPR[d] = uint64(c.Mem.Read8(c.eaX(i)))
		return
	case 119: // lbzux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.GPR[d] = uint64(c.Mem.Read8(ea))
		c.GPR[a] = uint64(ea)
		return
	case 279: // lhzx
		c.GPR[d] = uint64(c.Mem.Read16(c.eaX(i)))
		return
	case 311: // lhzux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.GPR[d] = uint64(c.Mem.Read16(ea))
		c.GPR[a] = uint64(ea)
		return
	case 343: // lhax
		c.GPR[d] = uint64(int64(int16(c.Mem.Read16(c.eaX(i)))))
		return
	case 375: // lhaux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.GPR[d] = uint64(int64(int16(c.Mem.Read16(ea))))
		c.GPR[a] = uint64(ea)
		return
	case 532: // ldbrx
		c.GPR[d] = bits.ReverseBytes64(c.Mem.Read64(c.eaX(i)))
		return
	case 534: // lwbrx
		c.GPR[d] = uint64(bits.ReverseBytes32(c.Mem.Read32(c.eaX(i))))
		return
	case 790: // lhbrx
		c.GPR[d] = uint64(bits.ReverseBytes16(c.Mem.Read16(c.eaX(i))))
		return
	case 149: // stdx
		c.Mem.Write64(c.eaX(i), c.GPR[d])
		return
	case 181: // stdux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.Mem.Write64(ea, c.GPR[d])
		c.GPR[a] = uint64(ea)
		return
	case 151: // stwx
		c.Mem.Write32(c.eaX(i), uint32(c.GPR[d]))
		return
	case 183: // stwux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.Mem.Write32(ea, uint32(c.GPR[d]))
		c.GPR[a] = uint64(ea)
		return
	case 215: // stbx
		c.Mem.Write8(c.eaX(i), uint8(c.GPR[d]))
		return
	case 247: // stbux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.Mem.Write8(ea, uint8(c.GPR[d]))
		c.GPR[a] = uint64(ea)
		return
	case 407: // sthx
		c.Mem.Write16(c.eaX(i), uint16(c.GPR[d]))
		return
	case 439: // sthux
		ea := uint32(c.GPR[a] + c.GPR[b])
		c.Mem.Write16(ea, uint16(c.GPR[d]))
		c.GPR[a] = uint64(ea)
		return
	case 660: // stdbrx
		c.Mem.Write64(c.eaX(i), bits.ReverseBytes64(c.GPR[d]))
		return
	case 662: // stwbrx
		c.Mem.Write32(c.eaX(i), bits.ReverseBytes32(uint32(c.GPR[d])))
		return
	case 918: // sthbrx
		c.Mem.Write16(c.eaX(i), bits.ReverseBytes16(uint16(c.GPR[d])))
		return
	case 533: // lswx
		c.loadString(c.eaX(i), d, uint32(c.XER&0x7f))
		return
	case 597: // lswi
		n := b
		if n == 0 {
			n = 32
		}
		ea := uint32(0)
		if a != 0 {
			ea = uint32(c.GPR[a])
		}
		c.loadString(ea, d, n)
		return
	case 661: // stswx
		c.storeString(c.eaX(i), d, uint32(c.XER&0x7f))
		return
	case 725: // stswi
		n := b
		if n == 0 {
			n = 32
		}
		ea := uint32(0)
		if a != 0 {
			ea = uint32(c.GPR[a])
		}
		c.storeString(ea, d, n)
		return
	case 24: // slw
		n := c.GPR[b] & 63
		var v uint64
		if n < 32 {
			v = uint64(uint32(c.GPR[d]) << n)
		}
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 536: // srw
		n := c.GPR[b] & 63
		var v uint64
		if n < 32 {
			v = uint64(uint32(c.GPR[d]) >> n)
		}
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 27: // sld
		n := c.GPR[b] & 127
		var v uint64
		if n < 64 {
			v = c.GPR[d] << n
		}
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 539: // srd
		n := c.GPR[b] & 127
		var v uint64
		if n < 64 {
			v = c.GPR[d] >> n
		}
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 792: // sraw
		n := c.GPR[b] & 63
		s := int32(c.GPR[d])
		var v int64
		ca := false
		if n < 32 {
			v = int64(s >> n)
			ca = s < 0 && uint32(s)<<(32-n) != 0 && n != 0
		} else {
			v = int64(s >> 31)
			ca = s < 0
		}
		c.GPR[a] = uint64(v)
		c.setCA(ca)
		if rc {
			c.setCR0(uint64(v))
		}
		return
	case 824: // srawi
		n := b
		s := int32(c.GPR[d])
		v := int64(s >> n)
		c.GPR[a] = uint64(v)
		c.setCA(s < 0 && n != 0 && uint32(s)<<(32-n) != 0)
		if rc {
			c.setCR0(uint64(v))
		}
		return
	case 794: // srad
		n := c.GPR[b] & 127
		s := int64(c.GPR[d])
		var v int64
		ca := false
		if n < 64 {
			v = s >> n
			ca = s < 0 && n != 0 && uint64(s)<<(64-n) != 0
		} else {
			v = s >> 63
			ca = s < 0
		}
		c.GPR[a] = uint64(v)
		c.setCA(ca)
		if rc {
			c.setCR0(uint64(v))
		}
		return
	case 826, 827: // sradi
		n := b | (i>>1&1)<<5
		s := int64(c.GPR[d])
		v := s >> n
		c.GPR[a] = uint64(v)
		c.setCA(s < 0 && n != 0 && uint64(s)<<(64-n) != 0)
		if rc {
			c.setCR0(uint64(v))
		}
		return
	case 26: // cntlzw
		v := uint64(bits.LeadingZeros32(uint32(c.GPR[d])))
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 58: // cntlzd
		v := uint64(bits.LeadingZeros64(c.GPR[d]))
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 28, 60, 124, 284, 316, 412, 444, 476: // and andc nor eqv xor orc or nand
		s, t := c.GPR[d], c.GPR[b]
		var v uint64
		switch xo {
		case 28:
			v = s & t
		case 60:
			v = s &^ t
		case 124:
			v = ^(s | t)
		case 284:
			v = ^(s ^ t)
		case 316:
			v = s ^ t
		case 412:
			v = s | ^t
		case 444:
			v = s | t
		case 476:
			v = ^(s & t)
		}
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 922: // extsh
		v := uint64(int64(int16(c.GPR[d])))
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 954: // extsb
		v := uint64(int64(int8(c.GPR[d])))
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 986: // extsw
		v := uint64(int64(int32(c.GPR[d])))
		c.GPR[a] = v
		if rc {
			c.setCR0(v)
		}
		return
	case 40: // subf
		r := c.GPR[b] - c.GPR[a]
		if oe {
			c.setOV(subOverflow(c.GPR[b], c.GPR[a], r))
		}
		c.GPR[d] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 104: // neg
		r := -c.GPR[a]
		if oe {
			c.setOV(c.GPR[a] == 1<<63)
		}
		c.GPR[d] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 136: // subfe
		r, cy := bits.Add64(^c.GPR[a], c.GPR[b], c.ca())
		if oe {
			c.setOV(addOverflow(^c.GPR[a], c.GPR[b], r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 138: // adde
		r, cy := bits.Add64(c.GPR[a], c.GPR[b], c.ca())
		if oe {
			c.setOV(addOverflow(c.GPR[a], c.GPR[b], r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 200: // subfze
		r, cy := bits.Add64(^c.GPR[a], 0, c.ca())
		if oe {
			c.setOV(addOverflow(^c.GPR[a], 0, r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 202: // addze
		r, cy := bits.Add64(c.GPR[a], 0, c.ca())
		if oe {
			c.setOV(addOverflow(c.GPR[a], 0, r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 232: // subfme
		r, cy := bits.Add64(^c.GPR[a], ^uint64(0), c.ca())
		if oe {
			c.setOV(addOverflow(^c.GPR[a], ^uint64(0), r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 234: // addme
		r, cy := bits.Add64(c.GPR[a], ^uint64(0), c.ca())
		if oe {
			c.setOV(addOverflow(c.GPR[a], ^uint64(0), r))
		}
		c.GPR[d] = r
		c.setCA(cy != 0)
		if rc {
			c.setCR0(r)
		}
		return
	case 233: // mulld
		x, y := int64(c.GPR[a]), int64(c.GPR[b])
		r := uint64(x * y)
		if oe {
			hi := mulhs64(x, y)
			c.setOV(!(hi == 0 && int64(r) >= 0) && !(hi == -1 && int64(r) < 0))
		}
		c.GPR[d] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 235: // mullw
		p := int64(int32(c.GPR[a])) * int64(int32(c.GPR[b]))
		if oe {
			c.setOV(p != int64(int32(p)))
		}
		c.GPR[d] = uint64(p)
		if rc {
			c.setCR0(uint64(p))
		}
		return
	case 266: // add
		r := c.GPR[a] + c.GPR[b]
		if oe {
			c.setOV(addOverflow(c.GPR[a], c.GPR[b], r))
		}
		c.GPR[d] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 457: // divdu
		x, y := c.GPR[a], c.GPR[b]
		var r uint64
		if y != 0 {
			r = x / y
		}
		if oe {
			c.setOV(y == 0)
		}
		c.GPR[d] = r
		if rc {
			c.setCR0(r)
		}
		return
	case 489: // divd
		x, y := int64(c.GPR[a]), int64(c.GPR[b])
		var r int64
		bad := y == 0 || (x == -1<<63 && y == -1)
		if !bad {
			r = x / y
		}
		if oe {
			c.setOV(bad)
		}
		c.GPR[d] = uint64(r)
		if rc {
			c.setCR0(uint64(r))
		}
		return
	case 459: // divwu
		x, y := uint32(c.GPR[a]), uint32(c.GPR[b])
		var r uint32
		if y != 0 {
			r = x / y
		}
		if oe {
			c.setOV(y == 0)
		}
		c.GPR[d] = uint64(r)
		if rc {
			c.setCR0(uint64(r))
		}
		return
	case 491: // divw
		x, y := int32(c.GPR[a]), int32(c.GPR[b])
		var r int32
		bad := y == 0 || (x == -1<<31 && y == -1)
		if !bad {
			r = x / y
		}
		if oe {
			c.setOV(bad)
		}
		c.GPR[d] = uint64(uint32(r))
		if rc {
			c.setCR0(uint64(uint32(r)))
		}
		return
	case 339: // mfspr
		spr := (i>>16&31 | (i>>11&31)<<5)
		switch spr {
		case 1:
			c.GPR[d] = c.XER
		case 8:
			c.GPR[d] = c.LR
		case 9:
			c.GPR[d] = c.CTR
		case 256:
			c.GPR[d] = uint64(c.VRSAVE)
		case 268:
			c.GPR[d] = c.TB()
		case 269:
			c.GPR[d] = c.TB() >> 32
		default:
			c.fail(i, fmt.Sprintf("mfspr %d", spr))
		}
		return
	case 467: // mtspr
		spr := (i>>16&31 | (i>>11&31)<<5)
		switch spr {
		case 1:
			c.XER = c.GPR[d] & 0xe000007f
		case 8:
			c.LR = c.GPR[d]
		case 9:
			c.CTR = c.GPR[d]
		case 256:
			c.VRSAVE = uint32(c.GPR[d])
		default:
			c.fail(i, fmt.Sprintf("mtspr %d", spr))
		}
		return
	case 371: // mftb
		spr := (i>>16&31 | (i>>11&31)<<5)
		if spr == 269 {
			c.GPR[d] = c.TB() >> 32
		} else {
			c.GPR[d] = c.TB()
		}
		return
	case 54, 86, 246, 278, 470, 982, 598, 854, 342, 374, 822: // dcbst dcbf dcbtst dcbt dcbi icbi sync eieio dst dstst dss
		return
	case 1014: // dcbz (128-byte cache line on the Cell)
		ea := c.eaX(i) &^ 127
		c.Mem.Fill(ea, 0, 128)
		return
	case 535, 567, 599, 631, 663, 695, 727, 759, 983:
		c.execFPMemX(i, xo)
		return
	}
	if c.execVMXMem(i, xo) {
		return
	}
	c.fail(i, "unknown instruction")
}

func (c *PPU) loadString(ea, r, n uint32) {
	for k := uint32(0); k < n; k++ {
		if k%4 == 0 {
			c.GPR[r] = 0
		}
		c.GPR[r] |= uint64(c.Mem.Read8(ea+k)) << (24 - 8*(k%4))
		if k%4 == 3 {
			r = (r + 1) & 31
		}
	}
}

func (c *PPU) storeString(ea, r, n uint32) {
	for k := uint32(0); k < n; k++ {
		c.Mem.Write8(ea+k, uint8(c.GPR[r]>>(24-8*(k%4))))
		if k%4 == 3 {
			r = (r + 1) & 31
		}
	}
}

func addOverflow(a, b, r uint64) bool { return (^(a ^ b))&(a^r)>>63 != 0 }
func subOverflow(a, b, r uint64) bool { return ((a^b)&(a^r))>>63 != 0 } // a - b

func mulhs64(a, b int64) int64 {
	hi, _ := bits.Mul64(uint64(a), uint64(b))
	h := int64(hi)
	if a < 0 {
		h -= b
	}
	if b < 0 {
		h -= a
	}
	return h
}
