// Package m6809 is RetroPixels' Motorola 6809 processor, with the Hitachi
// HD6309's extensions as an option.
//
// The 6809 has two 8-bit accumulators A and B (together D), index registers
// X and Y, two stack pointers U and S, a direct-page register DP and the
// condition codes CC. Interrupts are NMI (edge), FIRQ and IRQ (levels), with
// their vectors at the top of memory; SWI, SWI2 and SWI3 are software ones.
//
// The HD6309 adds E and F (together W; D and W together Q), V, the zero
// register and the mode register MD. In emulation mode (MD bit 0 clear, as
// after reset) it runs 6809 code with the 6809's timing and also knows its
// new instructions (register-to-register arithmetic, TFM block moves, bit
// operations, 32-bit loads, multiply and divide...). In native mode (MD bit 0
// set) most instructions take fewer cycles and interrupts also stack E and F.
// Illegal opcodes and division by zero trap through the vector at $FFF0.
//
//	cpu := m6809.New(bus, m6809.MC6809) // or m6809.HD6309
//	cpu.Reset()
//	n := cpu.Step()           // one instruction (or interrupt); returns cycles
//	cpu.IRQ, cpu.FIRQ = ...   // level-sensitive lines
//	cpu.NMI()                 // edge
package m6809

import (
	"encoding/binary"
	"errors"
)

// Bus is the memory the processor sees (16-bit addresses).
type Bus interface {
	Read(addr uint16) uint8
	Write(addr uint16, v uint8)
}

// Interrupt kinds for Acker.
const (
	AckNMI = iota
	AckFIRQ
	AckIRQ
)

// Acker is implemented by buses that want to know when an interrupt is taken.
type Acker interface {
	Ack(kind int)
}

// Model is the chip.
type Model int

const (
	MC6809 Model = iota // Motorola 6809 (and 68B09, 6809E)
	HD6309              // Hitachi HD6309 (and 63C09, 63C09E)
)

// Condition code bits.
const (
	FlagC = 0x01 // carry
	FlagV = 0x02 // overflow
	FlagZ = 0x04 // zero
	FlagN = 0x08 // negative
	FlagI = 0x10 // IRQ masked
	FlagH = 0x20 // half carry
	FlagF = 0x40 // FIRQ masked
	FlagE = 0x80 // entire state on the stack
)

// MD (HD6309 mode register) bits.
const (
	MDNative  = 0x01 // native mode
	MDFIRQAll = 0x02 // FIRQ stacks the entire state, like IRQ
	MDIllegal = 0x40 // set by an illegal-opcode trap
	MDDivZero = 0x80 // set by a division-by-zero trap
)

// Vectors.
const (
	VecTrap  = 0xfff0 // HD6309 illegal opcode / division by zero
	VecSWI3  = 0xfff2
	VecSWI2  = 0xfff4
	VecFIRQ  = 0xfff6
	VecIRQ   = 0xfff8
	VecSWI   = 0xfffa
	VecNMI   = 0xfffc
	VecReset = 0xfffe
)

// wait states
const (
	running = iota
	inCWAI
	inSYNC
)

// CPU is one 6809 or 6309.
type CPU struct {
	bus   Bus
	ack   Acker
	model Model

	A, B, DP, CC uint8
	X, Y, U, S   uint16
	PC           uint16
	E, F, MD     uint8  // HD6309 only
	V            uint16 // HD6309 only

	// IRQ and FIRQ are the levels of the maskable interrupt lines.
	IRQ, FIRQ bool

	nmiPending bool
	nmiArmed   bool // NMI is ignored until S is first loaded
	nmiLine    bool
	wait       int
	extra      int // cycles added by the addressing mode
	native     int // 1 in HD6309 native mode (cycle table index)
}

// New makes a processor of the given model on bus, reset.
func New(bus Bus, model Model) *CPU {
	c := &CPU{bus: bus, model: model}
	c.ack, _ = bus.(Acker)
	c.Reset()
	return c
}

// Model is the chip this processor is.
func (c *CPU) Model() Model { return c.model }

// Reset is the RESET line: interrupts masked, DP = 0, emulation mode and
// the program counter from $FFFE. The interrupt lines are the machine's and
// are left as they are.
func (c *CPU) Reset() {
	c.CC |= FlagI | FlagF
	c.DP = 0
	c.MD = 0
	c.native = 0
	c.wait = running
	c.nmiPending, c.nmiArmed = false, false
	c.PC = c.rd16(VecReset)
}

// NMI makes a non-maskable interrupt (an edge on the NMI line).
func (c *CPU) NMI() {
	if c.nmiArmed {
		c.nmiPending = true
	}
}

// SetNMI sets the level of the NMI line; a falling-to-asserted edge makes an
// interrupt.
func (c *CPU) SetNMI(on bool) {
	if on && !c.nmiLine {
		c.NMI()
	}
	c.nmiLine = on
}

// D is A:B.
func (c *CPU) D() uint16 { return uint16(c.A)<<8 | uint16(c.B) }

// SetD sets A:B.
func (c *CPU) SetD(v uint16) { c.A, c.B = uint8(v>>8), uint8(v) }

// W is E:F (HD6309).
func (c *CPU) W() uint16 { return uint16(c.E)<<8 | uint16(c.F) }

// SetW sets E:F.
func (c *CPU) SetW(v uint16) { c.E, c.F = uint8(v>>8), uint8(v) }

// Q is D:W (HD6309).
func (c *CPU) Q() uint32 { return uint32(c.D())<<16 | uint32(c.W()) }

// SetQ sets D:W.
func (c *CPU) SetQ(v uint32) { c.SetD(uint16(v >> 16)); c.SetW(uint16(v)) }

// Native tells whether an HD6309 is in native mode.
func (c *CPU) Native() bool { return c.native != 0 }

// Waiting tells whether the processor is stopped in CWAI or SYNC.
func (c *CPU) Waiting() bool { return c.wait != running }

func (c *CPU) is6309() bool { return c.model == HD6309 }

// setMD writes the mode register's two writable bits.
func (c *CPU) setMD(v uint8) {
	c.MD = c.MD&0xc0 | v&0x03
	c.native = int(c.MD & MDNative)
}

// ---------------------------------------------------------------- memory

func (c *CPU) rd(a uint16) uint8       { return c.bus.Read(a) }
func (c *CPU) wr(a uint16, v uint8)    { c.bus.Write(a, v) }
func (c *CPU) rd16(a uint16) uint16    { return uint16(c.rd(a))<<8 | uint16(c.rd(a+1)) }
func (c *CPU) wr16(a uint16, v uint16) { c.wr(a, uint8(v>>8)); c.wr(a+1, uint8(v)) }
func (c *CPU) fetch() uint8            { v := c.rd(c.PC); c.PC++; return v }
func (c *CPU) fetch16() uint16         { v := c.rd16(c.PC); c.PC += 2; return v }

func (c *CPU) push8(sp *uint16, v uint8) { *sp--; c.wr(*sp, v) }
func (c *CPU) push16(sp *uint16, v uint16) {
	c.push8(sp, uint8(v))
	c.push8(sp, uint8(v>>8))
}
func (c *CPU) pull8(sp *uint16) uint8 { v := c.rd(*sp); *sp++; return v }
func (c *CPU) pull16(sp *uint16) uint16 {
	hi := c.pull8(sp)
	return uint16(hi)<<8 | uint16(c.pull8(sp))
}

// ---------------------------------------------------------------- flags

func (c *CPU) flag(f uint8, on bool) {
	if on {
		c.CC |= f
	} else {
		c.CC &^= f
	}
}

func (c *CPU) nz8(v uint8) {
	c.CC &^= FlagN | FlagZ
	c.CC |= v & 0x80 >> 4
	if v == 0 {
		c.CC |= FlagZ
	}
}

func (c *CPU) nz16(v uint16) {
	c.CC &^= FlagN | FlagZ
	c.CC |= uint8(v>>12) & FlagN
	if v == 0 {
		c.CC |= FlagZ
	}
}

func (c *CPU) nz32(v uint32) {
	c.CC &^= FlagN | FlagZ
	c.CC |= uint8(v>>28) & FlagN
	if v == 0 {
		c.CC |= FlagZ
	}
}

// ld8 / ld16: N and Z of a loaded value, V clear.
func (c *CPU) ld8(v uint8) uint8 {
	c.nz8(v)
	c.CC &^= FlagV
	return v
}

func (c *CPU) ld16(v uint16) uint16 {
	c.nz16(v)
	c.CC &^= FlagV
	return v
}

// ---------------------------------------------------------------- arithmetic

func (c *CPU) add8(a, b, cy uint8) uint8 {
	r := uint16(a) + uint16(b) + uint16(cy)
	c.CC &^= FlagH | FlagN | FlagZ | FlagV | FlagC
	c.CC |= uint8((uint16(a)^uint16(b)^r)&0x10) << 1
	c.CC |= uint8(r>>8) & FlagC
	c.CC |= uint8((uint16(a)^r)&(uint16(b)^r)&0x80) >> 6
	c.nz8(uint8(r))
	return uint8(r)
}

func (c *CPU) sub8(a, b, cy uint8) uint8 {
	r := uint16(a) - uint16(b) - uint16(cy)
	c.CC &^= FlagN | FlagZ | FlagV | FlagC
	c.CC |= uint8(r>>8) & FlagC
	c.CC |= uint8((uint16(a)^uint16(b))&(uint16(a)^r)&0x80) >> 6
	c.nz8(uint8(r))
	return uint8(r)
}

func (c *CPU) add16(a, b, cy uint16) uint16 {
	r := uint32(a) + uint32(b) + uint32(cy)
	c.CC &^= FlagN | FlagZ | FlagV | FlagC
	c.CC |= uint8(r>>16) & FlagC
	c.CC |= uint8(((uint32(a)^r)&(uint32(b)^r)&0x8000)>>14) & FlagV
	c.nz16(uint16(r))
	return uint16(r)
}

func (c *CPU) sub16(a, b, cy uint16) uint16 {
	r := uint32(a) - uint32(b) - uint32(cy)
	c.CC &^= FlagN | FlagZ | FlagV | FlagC
	c.CC |= uint8(r>>16) & FlagC
	c.CC |= uint8(((uint32(a)^uint32(b))&(uint32(a)^r)&0x8000)>>14) & FlagV
	c.nz16(uint16(r))
	return uint16(r)
}

func (c *CPU) carry() uint8 { return c.CC & FlagC }

// rmw8 is the read-modify-write group (NEG, COM, LSR... by the opcode's low
// nibble) on one byte.
func (c *CPU) rmw8(k int, v uint8) uint8 {
	switch k {
	case 0x0: // NEG
		return c.sub8(0, v, 0)
	case 0x3: // COM
		v = ^v
		c.ld8(v)
		c.CC |= FlagC
		return v
	case 0x4: // LSR
		c.flag(FlagC, v&1 != 0)
		v >>= 1
		c.nz8(v)
		return v
	case 0x6: // ROR
		r := v>>1 | c.carry()<<7
		c.flag(FlagC, v&1 != 0)
		c.nz8(r)
		return r
	case 0x7: // ASR
		c.flag(FlagC, v&1 != 0)
		v = v&0x80 | v>>1
		c.nz8(v)
		return v
	case 0x8: // ASL / LSL
		r := v << 1
		c.flag(FlagC, v&0x80 != 0)
		c.flag(FlagV, (v^r)&0x80 != 0)
		c.nz8(r)
		return r
	case 0x9: // ROL
		r := v<<1 | c.carry()
		c.flag(FlagC, v&0x80 != 0)
		c.flag(FlagV, (v^r)&0x80 != 0)
		c.nz8(r)
		return r
	case 0xa: // DEC
		c.flag(FlagV, v == 0x80)
		v--
		c.nz8(v)
		return v
	case 0xc: // INC
		c.flag(FlagV, v == 0x7f)
		v++
		c.nz8(v)
		return v
	case 0xd: // TST
		return c.ld8(v)
	case 0xf: // CLR
		c.CC &^= FlagN | FlagV | FlagC
		c.CC |= FlagZ
		return 0
	}
	return v
}

// rmw16 is the same group on D or W (HD6309).
func (c *CPU) rmw16(k int, v uint16) uint16 {
	switch k {
	case 0x0:
		return c.sub16(0, v, 0)
	case 0x3:
		v = ^v
		c.ld16(v)
		c.CC |= FlagC
		return v
	case 0x4:
		c.flag(FlagC, v&1 != 0)
		v >>= 1
		c.nz16(v)
		return v
	case 0x6:
		r := v>>1 | uint16(c.carry())<<15
		c.flag(FlagC, v&1 != 0)
		c.nz16(r)
		return r
	case 0x7:
		c.flag(FlagC, v&1 != 0)
		v = v&0x8000 | v>>1
		c.nz16(v)
		return v
	case 0x8:
		r := v << 1
		c.flag(FlagC, v&0x8000 != 0)
		c.flag(FlagV, (v^r)&0x8000 != 0)
		c.nz16(r)
		return r
	case 0x9:
		r := v<<1 | uint16(c.carry())
		c.flag(FlagC, v&0x8000 != 0)
		c.flag(FlagV, (v^r)&0x8000 != 0)
		c.nz16(r)
		return r
	case 0xa:
		c.flag(FlagV, v == 0x8000)
		v--
		c.nz16(v)
		return v
	case 0xc:
		c.flag(FlagV, v == 0x7fff)
		v++
		c.nz16(v)
		return v
	case 0xd:
		return c.ld16(v)
	case 0xf:
		c.CC &^= FlagN | FlagV | FlagC
		c.CC |= FlagZ
		return 0
	}
	return v
}

// alu8 is the accumulator group (SUB, CMP, SBC, AND, BIT, LD, EOR, ADC, OR,
// ADD by the opcode's low nibble): the new value of the register.
func (c *CPU) alu8(k int, r, m uint8) uint8 {
	switch k {
	case 0x0:
		return c.sub8(r, m, 0)
	case 0x1:
		c.sub8(r, m, 0)
	case 0x2:
		return c.sub8(r, m, c.carry())
	case 0x4:
		return c.ld8(r & m)
	case 0x5:
		c.ld8(r & m)
	case 0x6:
		return c.ld8(m)
	case 0x8:
		return c.ld8(r ^ m)
	case 0x9:
		return c.add8(r, m, c.carry())
	case 0xa:
		return c.ld8(r | m)
	case 0xb:
		return c.add8(r, m, 0)
	}
	return r
}

// alu16 is the same group on 16 bits (HD6309 D and W instructions).
func (c *CPU) alu16(k int, r, m uint16) uint16 {
	switch k {
	case 0x0:
		return c.sub16(r, m, 0)
	case 0x1:
		c.sub16(r, m, 0)
	case 0x2:
		return c.sub16(r, m, uint16(c.carry()))
	case 0x4:
		return c.ld16(r & m)
	case 0x5:
		c.ld16(r & m)
	case 0x6:
		return c.ld16(m)
	case 0x8:
		return c.ld16(r ^ m)
	case 0x9:
		return c.add16(r, m, uint16(c.carry()))
	case 0xa:
		return c.ld16(r | m)
	case 0xb:
		return c.add16(r, m, 0)
	}
	return r
}

// ---------------------------------------------------------------- addressing

func (c *CPU) direct() uint16 { return uint16(c.DP)<<8 | uint16(c.fetch()) }

// idxCycles: extra cycles of each indexed form (low nibble of the postbyte)
// in emulation and native mode; indirect forms add 3 more.
var idxCycles = [2][16]int{
	{2, 3, 2, 3, 0, 1, 1, 1, 1, 4, 1, 4, 1, 5, 4, 0},
	{1, 2, 1, 2, 0, 1, 1, 1, 1, 3, 1, 2, 1, 3, 2, 0},
}

// indexed decodes an indexed postbyte (and its offset bytes) into an address.
func (c *CPU) indexed() uint16 {
	pb := c.fetch()
	var reg *uint16
	switch pb >> 5 & 3 {
	case 0:
		reg = &c.X
	case 1:
		reg = &c.Y
	case 2:
		reg = &c.U
	default:
		reg = &c.S
	}
	if pb&0x80 == 0 { // 5-bit offset
		c.extra += 1
		return *reg + uint16(int16(int8(pb<<3))>>3)
	}
	k := pb & 0x0f
	ind := pb&0x10 != 0
	if c.is6309() && (k == 0xf && !ind || k == 0x0 && ind) {
		return c.wIndexed(pb)
	}
	c.extra += idxCycles[c.native][k]
	var ea uint16
	switch k {
	case 0x0: // ,R+
		ea = *reg
		*reg++
	case 0x1:
		ea = *reg
		*reg += 2
	case 0x2:
		*reg--
		ea = *reg
	case 0x3:
		*reg -= 2
		ea = *reg
	case 0x4:
		ea = *reg
	case 0x5:
		ea = *reg + uint16(int16(int8(c.B)))
	case 0x6:
		ea = *reg + uint16(int16(int8(c.A)))
	case 0x7: // E,R
		ea = *reg + uint16(int16(int8(c.E)))
	case 0x8:
		ea = *reg + uint16(int16(int8(c.fetch())))
	case 0x9:
		ea = *reg + c.fetch16()
	case 0xa: // F,R
		ea = *reg + uint16(int16(int8(c.F)))
	case 0xb:
		ea = *reg + c.D()
	case 0xc:
		off := uint16(int16(int8(c.fetch())))
		ea = c.PC + off
	case 0xd:
		off := c.fetch16()
		ea = c.PC + off
	case 0xe: // W,R
		ea = *reg + c.W()
	case 0xf:
		if ind { // [n16]
			ea = c.fetch16()
			c.extra += 2 // [n16] is 5 in all
			break
		}
		ea = c.fetch16() // not a 6809 mode: taken as n16
	}
	if ind {
		c.extra += 3
		ea = c.rd16(ea)
	}
	return ea
}

// wIndexed is the HD6309's indexing on W: postbytes $8F ,W  $AF n16,W
// $CF ,W++  $EF ,--W and their indirect forms $90 $B0 $D0 $F0.
func (c *CPU) wIndexed(pb uint8) uint16 {
	var ea uint16
	switch pb >> 5 & 3 {
	case 0:
		ea = c.W()
	case 1:
		ea = c.W() + c.fetch16()
		c.extra += 2
	case 2:
		ea = c.W()
		c.SetW(ea + 2)
		c.extra++
	case 3:
		c.SetW(c.W() - 2)
		ea = c.W()
		c.extra++
	}
	if pb&0x10 != 0 {
		c.extra += 3
		ea = c.rd16(ea)
	}
	return ea
}

// ea is the address of a memory operand: mode 1 direct, 2 indexed, 3
// extended (the opcode's bits 4-5).
func (c *CPU) ea(mode int) uint16 {
	switch mode {
	case 1:
		return c.direct()
	case 2:
		return c.indexed()
	}
	return c.fetch16()
}

func (c *CPU) operand8(mode int) uint8 {
	if mode == 0 {
		return c.fetch()
	}
	return c.rd(c.ea(mode))
}

func (c *CPU) operand16(mode int) uint16 {
	if mode == 0 {
		return c.fetch16()
	}
	return c.rd16(c.ea(mode))
}

// ---------------------------------------------------------------- registers by number

// Register numbers in TFR / EXG and the HD6309's register instructions.
const (
	rD, rX, rY, rU, rS, rPC, rW, rV, rA, rB, rCC, rDP, rZ0, rZ1, rE, rF = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
)

func (c *CPU) validReg(r int) bool {
	if c.is6309() {
		return true
	}
	return r <= rPC || (r >= rA && r <= rDP)
}

// getReg reads register r as the destination size wants it (dst16: a 16-bit
// destination).
func (c *CPU) getReg(r int, dst16 bool) uint16 {
	if !c.validReg(r) {
		return 0xffff
	}
	var v uint16
	switch r {
	case rD:
		v = c.D()
	case rX:
		v = c.X
	case rY:
		v = c.Y
	case rU:
		v = c.U
	case rS:
		v = c.S
	case rPC:
		v = c.PC
	case rW:
		v = c.W()
	case rV:
		v = c.V
	case rZ0, rZ1:
		return 0
	}
	if r < rA {
		return v // 16-bit source: an 8-bit destination takes the low byte
	}
	var b uint8
	switch r {
	case rA:
		b = c.A
	case rB:
		b = c.B
	case rCC:
		b = c.CC
	case rDP:
		b = c.DP
	case rE:
		b = c.E
	case rF:
		b = c.F
	}
	if !dst16 {
		return uint16(b)
	}
	if !c.is6309() {
		return 0xff00 | uint16(b) // the 6809 fills the high byte with ones
	}
	switch r {
	case rA, rB:
		return c.D()
	case rE, rF:
		return c.W()
	}
	return uint16(b)<<8 | uint16(b)
}

func (c *CPU) setReg(r int, v uint16) {
	if !c.validReg(r) {
		return
	}
	switch r {
	case rD:
		c.SetD(v)
	case rX:
		c.X = v
	case rY:
		c.Y = v
	case rU:
		c.U = v
	case rS:
		c.S = v
		c.nmiArmed = true
	case rPC:
		c.PC = v
	case rW:
		c.SetW(v)
	case rV:
		c.V = v
	case rA:
		c.A = uint8(v)
	case rB:
		c.B = uint8(v)
	case rCC:
		c.CC = uint8(v)
	case rDP:
		c.DP = uint8(v)
	case rE:
		c.E = uint8(v)
	case rF:
		c.F = uint8(v)
	}
}

func is16(r int) bool { return r < rA }

// ---------------------------------------------------------------- stacking

// pushAll stacks the entire state (E set by the caller first).
func (c *CPU) pushAll() {
	c.push16(&c.S, c.PC)
	c.push16(&c.S, c.U)
	c.push16(&c.S, c.Y)
	c.push16(&c.S, c.X)
	c.push8(&c.S, c.DP)
	if c.native != 0 {
		c.push8(&c.S, c.F)
		c.push8(&c.S, c.E)
	}
	c.push8(&c.S, c.B)
	c.push8(&c.S, c.A)
	c.push8(&c.S, c.CC)
}

// psh is PSHS / PSHU: sp is the stack, other the other stack pointer.
func (c *CPU) psh(sp *uint16, other uint16, pb uint8) {
	if pb&0x80 != 0 {
		c.push16(sp, c.PC)
		c.extra += 2
	}
	if pb&0x40 != 0 {
		c.push16(sp, other)
		c.extra += 2
	}
	if pb&0x20 != 0 {
		c.push16(sp, c.Y)
		c.extra += 2
	}
	if pb&0x10 != 0 {
		c.push16(sp, c.X)
		c.extra += 2
	}
	if pb&0x08 != 0 {
		c.push8(sp, c.DP)
		c.extra++
	}
	if pb&0x04 != 0 {
		c.push8(sp, c.B)
		c.extra++
	}
	if pb&0x02 != 0 {
		c.push8(sp, c.A)
		c.extra++
	}
	if pb&0x01 != 0 {
		c.push8(sp, c.CC)
		c.extra++
	}
}

// pul is PULS / PULU: other points at the other stack pointer.
func (c *CPU) pul(sp *uint16, other *uint16, pb uint8) {
	if pb&0x01 != 0 {
		c.CC = c.pull8(sp)
		c.extra++
	}
	if pb&0x02 != 0 {
		c.A = c.pull8(sp)
		c.extra++
	}
	if pb&0x04 != 0 {
		c.B = c.pull8(sp)
		c.extra++
	}
	if pb&0x08 != 0 {
		c.DP = c.pull8(sp)
		c.extra++
	}
	if pb&0x10 != 0 {
		c.X = c.pull16(sp)
		c.extra += 2
	}
	if pb&0x20 != 0 {
		c.Y = c.pull16(sp)
		c.extra += 2
	}
	if pb&0x40 != 0 {
		*other = c.pull16(sp)
		c.extra += 2
	}
	if pb&0x80 != 0 {
		c.PC = c.pull16(sp)
		c.extra += 2
	}
}

// ---------------------------------------------------------------- interrupts

// interrupt stacks the state (all of it when entire) and jumps through vec.
func (c *CPU) interrupt(vec uint16, entire bool, mask uint8) {
	if c.wait != inCWAI { // CWAI stacked everything already
		if entire {
			c.CC |= FlagE
			c.pushAll()
		} else {
			c.CC &^= FlagE
			c.push16(&c.S, c.PC)
			c.push8(&c.S, c.CC)
		}
	}
	c.wait = running
	c.CC |= mask
	c.PC = c.rd16(vec)
}

// trap is the HD6309's illegal-opcode / division-by-zero exception.
func (c *CPU) trap(md uint8) int {
	c.MD |= md
	c.CC |= FlagE
	c.pushAll()
	c.CC |= FlagI | FlagF
	c.PC = c.rd16(VecTrap)
	return 20 + 2*c.native
}

// illegal runs an opcode the chip doesn't have: the HD6309 traps; the 6809
// treats it as a 1-byte do-nothing.
func (c *CPU) illegal() int {
	if c.is6309() {
		return c.trap(MDIllegal)
	}
	return 2
}

// Step runs one instruction, or takes an interrupt, or waits one cycle in
// CWAI / SYNC. It returns the cycles used.
func (c *CPU) Step() int {
	firq := c.FIRQ && c.CC&FlagF == 0
	irq := c.IRQ && c.CC&FlagI == 0
	if c.wait == inSYNC {
		if !c.nmiPending && !c.FIRQ && !c.IRQ {
			return 1
		}
		c.wait = running // a masked line just ends the SYNC
	}
	switch {
	case c.nmiPending:
		c.nmiPending = false
		cyc := 19
		if c.wait == inCWAI {
			cyc = 7
		}
		c.interrupt(VecNMI, true, FlagI|FlagF)
		if c.ack != nil {
			c.ack.Ack(AckNMI)
		}
		return cyc + 2*c.native
	case firq:
		entire := c.MD&MDFIRQAll != 0
		cyc := 10
		if entire {
			cyc = 19 + 2*c.native
		}
		if c.wait == inCWAI {
			cyc = 7
		}
		c.interrupt(VecFIRQ, entire, FlagI|FlagF)
		if c.ack != nil {
			c.ack.Ack(AckFIRQ)
		}
		return cyc
	case irq:
		cyc := 19 + 2*c.native
		if c.wait == inCWAI {
			cyc = 7
		}
		c.interrupt(VecIRQ, true, FlagI)
		if c.ack != nil {
			c.ack.Ack(AckIRQ)
		}
		return cyc
	}
	if c.wait == inCWAI {
		return 1
	}
	c.extra = 0
	op := c.fetch()
	var n int
	switch op {
	case 0x10:
		n = c.exec10(c.fetch())
	case 0x11:
		n = c.exec11(c.fetch())
	default:
		n = c.exec(op)
	}
	return n + c.extra
}

// ---------------------------------------------------------------- page 1

func (c *CPU) branch(cond bool, long bool) {
	var off uint16
	if long {
		off = c.fetch16()
	} else {
		off = uint16(int16(int8(c.fetch())))
	}
	if cond {
		c.PC += off
		if long {
			c.extra++
		}
	}
}

func (c *CPU) cond(op uint8) bool {
	cc := c.CC
	n, z, v, cy := cc&FlagN != 0, cc&FlagZ != 0, cc&FlagV != 0, cc&FlagC != 0
	var r bool
	switch op >> 1 & 7 {
	case 0: // BRA / BRN
		r = true
	case 1: // BHI / BLS
		r = !cy && !z
	case 2: // BCC / BCS
		r = !cy
	case 3: // BNE / BEQ
		r = !z
	case 4: // BVC / BVS
		r = !v
	case 5: // BPL / BMI
		r = !n
	case 6: // BGE / BLT
		r = n == v
	case 7: // BGT / BLE
		r = n == v && !z
	}
	if op&1 != 0 {
		r = !r
	}
	return r
}

func (c *CPU) cyc(page int, op uint8) int { return cycles[page][c.native][op] }

// exec runs a page-1 (unprefixed) opcode.
func (c *CPU) exec(op uint8) int {
	n := c.cyc(0, op)
	k := int(op & 0x0f)
	switch {
	case op < 0x10 || (op >= 0x60 && op < 0x80):
		var ea uint16
		if op < 0x10 {
			if k == 1 || k == 2 || k == 5 || k == 0xb {
				if c.is6309() {
					return n + c.imm(k, 1)
				}
			}
			ea = c.direct()
		} else if c.is6309() && (k == 1 || k == 2 || k == 5 || k == 0xb) {
			return n + c.imm(k, int(op>>4&3))
		} else if op < 0x70 {
			ea = c.indexed()
		} else {
			ea = c.fetch16()
		}
		switch k {
		case 0xe:
			c.PC = ea
		case 0x1, 0x2, 0x5, 0xb: // 6809 undocumented aliases
			alias := [16]int{5: 4, 0xb: 0xa}[k]
			if k == 2 && c.CC&FlagC != 0 {
				alias = 3
			}
			c.wr(ea, c.rmw8(alias, c.rd(ea)))
		case 0xd:
			c.rmw8(k, c.rd(ea))
		case 0xf:
			c.rd(ea) // CLR reads before it writes
			c.wr(ea, c.rmw8(k, 0))
		default:
			c.wr(ea, c.rmw8(k, c.rd(ea)))
		}
		return n
	case op >= 0x40 && op < 0x60:
		r := &c.A
		if op >= 0x50 {
			r = &c.B
		}
		switch k {
		case 0x1, 0x2, 0x5, 0xb, 0xe:
			if c.is6309() {
				return c.illegal()
			}
			alias := [16]int{5: 4, 0xb: 0xa, 0xe: 0xf}[k]
			if k == 2 && c.CC&FlagC != 0 {
				alias = 3
			}
			*r = c.rmw8(alias, *r)
		default:
			*r = c.rmw8(k, *r)
		}
		return n
	case op >= 0x20 && op < 0x30:
		c.branch(c.cond(op), false)
		return n
	case op >= 0x80:
		return n + c.execAcc(op)
	}
	switch op {
	case 0x12: // NOP
	case 0x13: // SYNC
		c.wait = inSYNC
	case 0x14: // SEXW
		if !c.is6309() {
			return c.illegal()
		}
		if c.E&0x80 != 0 {
			c.SetD(0xffff)
		} else {
			c.SetD(0)
		}
		c.nz32(c.Q())
	case 0x16: // LBRA
		c.PC += c.fetch16()
	case 0x17: // LBSR
		off := c.fetch16()
		c.push16(&c.S, c.PC)
		c.PC += off
	case 0x19: // DAA
		var cf uint16
		lo, hi := c.A&0x0f, c.A>>4
		if lo > 9 || c.CC&FlagH != 0 {
			cf |= 0x06
		}
		if hi > 9 || c.CC&FlagC != 0 || (hi > 8 && lo > 9) {
			cf |= 0x60
		}
		r := uint16(c.A) + cf
		c.A = uint8(r)
		c.CC &^= FlagV
		if r > 0xff {
			c.CC |= FlagC
		}
		c.nz8(c.A)
	case 0x1a: // ORCC
		c.CC |= c.fetch()
	case 0x1c: // ANDCC
		c.CC &= c.fetch()
	case 0x1d: // SEX
		if c.B&0x80 != 0 {
			c.A = 0xff
		} else {
			c.A = 0
		}
		c.nz16(c.D())
	case 0x1e: // EXG
		pb := c.fetch()
		r1, r2 := int(pb>>4), int(pb&0x0f)
		v1, v2 := c.getReg(r1, is16(r2)), c.getReg(r2, is16(r1))
		c.setReg(r2, v1)
		c.setReg(r1, v2)
	case 0x1f: // TFR
		pb := c.fetch()
		r1, r2 := int(pb>>4), int(pb&0x0f)
		c.setReg(r2, c.getReg(r1, is16(r2)))
	case 0x30, 0x31: // LEAX, LEAY
		ea := c.indexed()
		if op == 0x30 {
			c.X = ea
		} else {
			c.Y = ea
		}
		c.flag(FlagZ, ea == 0)
	case 0x32: // LEAS
		c.S = c.indexed()
		c.nmiArmed = true
	case 0x33: // LEAU
		c.U = c.indexed()
	case 0x34: // PSHS
		c.psh(&c.S, c.U, c.fetch())
	case 0x35: // PULS
		c.pul(&c.S, &c.U, c.fetch())
	case 0x36: // PSHU
		c.psh(&c.U, c.S, c.fetch())
	case 0x37: // PULU
		c.pul(&c.U, &c.S, c.fetch())
	case 0x39: // RTS
		c.PC = c.pull16(&c.S)
	case 0x3a: // ABX
		c.X += uint16(c.B)
	case 0x3b: // RTI
		c.CC = c.pull8(&c.S)
		if c.CC&FlagE != 0 {
			c.A = c.pull8(&c.S)
			c.B = c.pull8(&c.S)
			if c.native != 0 {
				c.E = c.pull8(&c.S)
				c.F = c.pull8(&c.S)
				n += 2
			}
			c.DP = c.pull8(&c.S)
			c.X = c.pull16(&c.S)
			c.Y = c.pull16(&c.S)
			c.U = c.pull16(&c.S)
			n += 9
		}
		c.PC = c.pull16(&c.S)
	case 0x3c: // CWAI
		c.CC &= c.fetch()
		c.CC |= FlagE
		c.pushAll()
		c.wait = inCWAI
	case 0x3d: // MUL
		c.SetD(uint16(c.A) * uint16(c.B))
		c.flag(FlagZ, c.D() == 0)
		c.flag(FlagC, c.B&0x80 != 0)
	case 0x3e: // 6809 undocumented RESET
		if c.is6309() {
			return c.illegal()
		}
		c.CC |= FlagE
		c.pushAll()
		c.CC |= FlagI | FlagF
		c.PC = c.rd16(VecReset)
	case 0x3f: // SWI
		c.CC |= FlagE
		c.pushAll()
		c.CC |= FlagI | FlagF
		c.PC = c.rd16(VecSWI)
	default:
		return c.illegal()
	}
	return n
}

// imm is the HD6309's OIM / AIM / EIM / TIM: an immediate byte, then the
// memory operand (mode 1 direct, 2 indexed, 3 extended).
func (c *CPU) imm(k int, mode int) int {
	m := c.fetch()
	ea := c.ea(mode)
	v := c.rd(ea)
	switch k {
	case 0x1:
		v |= m
	case 0x2:
		v &= m
	case 0x5:
		v ^= m
	case 0xb:
		c.ld8(v & m)
		return 0
	}
	c.wr(ea, c.ld8(v))
	return 0
}

// execAcc runs $80-$FF: the A and B instructions and the 16-bit ones that
// share their columns.
func (c *CPU) execAcc(op uint8) int {
	k := int(op & 0x0f)
	mode := int(op >> 4 & 3)
	isB := op >= 0xc0
	r := &c.A
	if isB {
		r = &c.B
	}
	switch k {
	case 0x7: // STA / STB
		if mode == 0 {
			return c.illegal()
		}
		c.wr(c.ea(mode), c.ld8(*r))
	case 0x3: // SUBD / ADDD
		m := c.operand16(mode)
		if isB {
			c.SetD(c.add16(c.D(), m, 0))
		} else {
			c.SetD(c.sub16(c.D(), m, 0))
		}
	case 0xc: // CMPX / LDD
		if isB {
			c.SetD(c.ld16(c.operand16(mode)))
		} else {
			c.sub16(c.X, c.operand16(mode), 0)
		}
	case 0xd: // BSR, JSR / STD (LDQ #)
		if isB {
			if mode == 0 {
				if !c.is6309() {
					return c.illegal()
				}
				q := uint32(c.fetch16())<<16 | uint32(c.fetch16())
				c.SetQ(q)
				c.nz32(q)
				c.CC &^= FlagV
				return 0
			}
			c.wr16(c.ea(mode), c.ld16(c.D()))
			return 0
		}
		var ea uint16
		if mode == 0 {
			ea = c.PC + 1 + uint16(int16(int8(c.rd(c.PC))))
			c.PC++
		} else {
			ea = c.ea(mode)
		}
		c.push16(&c.S, c.PC)
		c.PC = ea
	case 0xe: // LDX / LDU
		v := c.ld16(c.operand16(mode))
		if isB {
			c.U = v
		} else {
			c.X = v
		}
	case 0xf: // STX / STU
		if mode == 0 {
			return c.illegal()
		}
		v := c.X
		if isB {
			v = c.U
		}
		c.wr16(c.ea(mode), c.ld16(v))
	default:
		*r = c.alu8(k, *r, c.operand8(mode))
	}
	return 0
}

// ---------------------------------------------------------------- page 2 ($10)

func (c *CPU) exec10(op uint8) int {
	n := c.cyc(1, op)
	k := int(op & 0x0f)
	mode := int(op >> 4 & 3)
	switch {
	case op > 0x20 && op < 0x30:
		c.branch(c.cond(op), true)
		return n
	case op == 0x3f: // SWI2
		c.CC |= FlagE
		c.pushAll()
		c.PC = c.rd16(VecSWI2)
		return n
	case op >= 0x80 && op < 0xc0:
		switch k {
		case 0x3: // CMPD
			c.sub16(c.D(), c.operand16(mode), 0)
			return n
		case 0xc: // CMPY
			c.sub16(c.Y, c.operand16(mode), 0)
			return n
		case 0xe: // LDY
			c.Y = c.ld16(c.operand16(mode))
			return n
		case 0xf: // STY
			if mode == 0 {
				return c.illegal()
			}
			c.wr16(c.ea(mode), c.ld16(c.Y))
			return n
		}
		if !c.is6309() || k == 0xd {
			return c.illegal()
		}
		switch k {
		case 0x7: // STW
			if mode == 0 {
				return c.illegal()
			}
			c.wr16(c.ea(mode), c.ld16(c.W()))
		case 0x0, 0x1, 0x6, 0xb: // SUBW CMPW LDW ADDW
			c.SetW(c.alu16(k, c.W(), c.operand16(mode)))
		default: // SBCD ANDD BITD EORD ADCD ORD
			c.SetD(c.alu16(k, c.D(), c.operand16(mode)))
		}
		return n
	case op >= 0xc0:
		switch k {
		case 0xe: // LDS
			c.S = c.ld16(c.operand16(mode))
			c.nmiArmed = true
			return n
		case 0xf: // STS
			if mode == 0 {
				return c.illegal()
			}
			c.wr16(c.ea(mode), c.ld16(c.S))
			return n
		case 0xc: // LDQ
			if c.is6309() && mode != 0 {
				ea := c.ea(mode)
				q := uint32(c.rd16(ea))<<16 | uint32(c.rd16(ea+2))
				c.SetQ(q)
				c.nz32(q)
				c.CC &^= FlagV
				return n
			}
		case 0xd: // STQ
			if c.is6309() && mode != 0 {
				ea := c.ea(mode)
				q := c.Q()
				c.wr16(ea, uint16(q>>16))
				c.wr16(ea+2, uint16(q))
				c.nz32(q)
				c.CC &^= FlagV
				return n
			}
		}
		return c.illegal()
	}
	if !c.is6309() {
		return c.illegal()
	}
	switch {
	case op >= 0x30 && op <= 0x37: // ADDR ADCR SUBR SBCR ANDR ORR EORR CMPR
		pb := c.fetch()
		src, dst := int(pb>>4), int(pb&0x0f)
		alu := [8]int{0xb, 0x9, 0x0, 0x2, 0x4, 0xa, 0x8, 0x1}[op&7]
		if is16(dst) {
			v := c.alu16(alu, c.getReg(dst, true), c.getReg(src, true))
			if op != 0x37 {
				c.setReg(dst, v)
			}
		} else {
			v := c.alu8(alu, uint8(c.getReg(dst, false)), uint8(c.getReg(src, false)))
			if op != 0x37 {
				c.setReg(dst, uint16(v))
			}
		}
		return n
	case op == 0x38: // PSHSW
		c.push8(&c.S, c.F)
		c.push8(&c.S, c.E)
		return n
	case op == 0x39: // PULSW
		c.E = c.pull8(&c.S)
		c.F = c.pull8(&c.S)
		return n
	case op == 0x3a: // PSHUW
		c.push8(&c.U, c.F)
		c.push8(&c.U, c.E)
		return n
	case op == 0x3b: // PULUW
		c.E = c.pull8(&c.U)
		c.F = c.pull8(&c.U)
		return n
	case op >= 0x40 && op < 0x50: // D: NEGD COMD LSRD RORD ASRD ASLD ROLD DECD INCD TSTD CLRD
		switch k {
		case 0x0, 0x3, 0x4, 0x6, 0x7, 0x8, 0x9, 0xa, 0xc, 0xd, 0xf:
			v := c.rmw16(k, c.D())
			if k != 0xd {
				c.SetD(v)
			}
			return n
		}
	case op >= 0x50 && op < 0x60: // W: COMW LSRW RORW ROLW DECW INCW TSTW CLRW
		switch k {
		case 0x3, 0x4, 0x6, 0x9, 0xa, 0xc, 0xd, 0xf:
			v := c.rmw16(k, c.W())
			if k != 0xd {
				c.SetW(v)
			}
			return n
		}
	}
	return c.illegal()
}

// ---------------------------------------------------------------- page 3 ($11)

func (c *CPU) exec11(op uint8) int {
	n := c.cyc(2, op)
	k := int(op & 0x0f)
	mode := int(op >> 4 & 3)
	switch {
	case op == 0x3f: // SWI3
		c.CC |= FlagE
		c.pushAll()
		c.PC = c.rd16(VecSWI3)
		return n
	case op >= 0x80 && op < 0xc0 && (k == 0x3 || k == 0xc): // CMPU CMPS
		r := c.U
		if k == 0xc {
			r = c.S
		}
		c.sub16(r, c.operand16(mode), 0)
		return n
	}
	if !c.is6309() {
		return c.illegal()
	}
	switch {
	case op >= 0x30 && op <= 0x37: // BAND BIAND BOR BIOR BEOR BIEOR LDBT STBT
		pb := c.fetch()
		ea := c.direct()
		var reg *uint8
		switch pb >> 6 {
		case 0:
			reg = &c.CC
		case 1:
			reg = &c.A
		case 2:
			reg = &c.B
		default:
			return c.illegal()
		}
		mbit, rbit := pb>>3&7, pb&7
		m := c.rd(ea) >> mbit & 1
		r := *reg >> rbit & 1
		switch op & 7 {
		case 0:
			r &= m
		case 1:
			r &= m ^ 1
		case 2:
			r |= m
		case 3:
			r |= m ^ 1
		case 4:
			r ^= m
		case 5:
			r ^= m ^ 1
		case 6:
			r = m
		case 7:
			v := c.rd(ea)&^(1<<mbit) | r<<mbit
			c.wr(ea, v)
			return n
		}
		*reg = *reg&^(1<<rbit) | r<<rbit
		return n
	case op >= 0x38 && op <= 0x3b: // TFM
		return c.tfm(op)
	case op == 0x3c: // BITMD
		m := c.fetch()
		c.flag(FlagZ, c.MD&m&0xc0 == 0)
		c.MD &^= m & 0xc0
		return n
	case op == 0x3d: // LDMD
		c.setMD(c.fetch())
		return n
	case op >= 0x40 && op < 0x60: // COME DECE INCE TSTE CLRE / F
		r := &c.E
		if op >= 0x50 {
			r = &c.F
		}
		switch k {
		case 0x3, 0xa, 0xc, 0xd, 0xf:
			v := c.rmw8(k, *r)
			if k != 0xd {
				*r = v
			}
			return n
		}
	case op >= 0x80:
		r := &c.E
		if op >= 0xc0 {
			r = &c.F
		}
		switch k {
		case 0x0, 0x1, 0x6, 0xb: // SUB CMP LD ADD
			*r = c.alu8(k, *r, c.operand8(mode))
			return n
		case 0x7: // STE / STF
			if mode != 0 {
				c.wr(c.ea(mode), c.ld8(*r))
				return n
			}
		case 0xd, 0xe, 0xf:
			if op < 0xc0 {
				switch k {
				case 0xd:
					return n + c.divd(c.operand8(mode))
				case 0xe:
					return n + c.divq(c.operand16(mode))
				default:
					return n + c.muld(c.operand16(mode))
				}
			}
		}
	}
	return c.illegal()
}

// tfm moves one byte of a TFM block move and repeats the instruction until
// W is zero (so interrupts can come between bytes, as on the chip).
func (c *CPU) tfm(op uint8) int {
	pb := c.rd(c.PC)
	src, dst := int(pb>>4), int(pb&0x0f)
	if src > rD+4 || dst > rD+4 { // only D X Y U S
		c.PC++
		return c.illegal()
	}
	if c.W() == 0 {
		c.PC++
		return 6
	}
	sv, dv := c.getReg(src, true), c.getReg(dst, true)
	c.wr(dv, c.rd(sv))
	switch op {
	case 0x38:
		sv++
		dv++
	case 0x39:
		sv--
		dv--
	case 0x3a:
		sv++
	case 0x3b:
		dv++
	}
	c.setReg(src, sv)
	c.setReg(dst, dv)
	c.SetW(c.W() - 1)
	c.PC -= 2 // back to the $11 prefix: run again for the next byte
	return 3
}

// divd: D / m (signed) -> quotient in B, remainder in A.
func (c *CPU) divd(m uint8) int {
	if m == 0 {
		return c.trap(MDDivZero) - 20
	}
	num := int32(int16(c.D()))
	den := int32(int8(m))
	q, r := num/den, num%den
	c.CC &^= FlagN | FlagZ | FlagV | FlagC
	if q > 255 || q < -256 { // too big: registers left alone
		c.CC |= FlagV
		return -12
	}
	c.A, c.B = uint8(r), uint8(q)
	if q > 127 || q < -128 {
		c.CC |= FlagV | FlagN
	} else {
		c.nz8(c.B)
	}
	c.CC |= c.B & FlagC
	return 0
}

// divq: Q / m (signed) -> quotient in W, remainder in D.
func (c *CPU) divq(m uint16) int {
	if m == 0 {
		return c.trap(MDDivZero) - 20
	}
	num := int64(int32(c.Q()))
	den := int64(int16(m))
	q, r := num/den, num%den
	c.CC &^= FlagN | FlagZ | FlagV | FlagC
	if q > 65535 || q < -65536 {
		c.CC |= FlagV
		return -21
	}
	c.SetD(uint16(r))
	c.SetW(uint16(q))
	if q > 32767 || q < -32768 {
		c.CC |= FlagV | FlagN
	} else {
		c.nz16(c.W())
	}
	c.CC |= c.F & FlagC
	return 0
}

// muld: D * m (signed) -> Q.
func (c *CPU) muld(m uint16) int {
	q := uint32(int32(int16(c.D())) * int32(int16(m)))
	c.SetQ(q)
	c.nz32(q)
	c.CC &^= FlagV | FlagC
	return 0
}

// ---------------------------------------------------------------- cycle tables

// cycles[page][native][opcode]: base cycles (addressing and stacking add theirs).
var cycles [3][2][256]int

func init() {
	fill := func(t *[256]int, lo, hi, n int) {
		for i := lo; i <= hi; i++ {
			t[i] = n
		}
	}
	set := func(t *[256]int, n int, ops ...int) {
		for _, o := range ops {
			t[o] = n
		}
	}
	// page 1, emulation mode (the 6809's own timing)
	p := &cycles[0][0]
	fill(p, 0, 255, 2)
	fill(p, 0x00, 0x0f, 6)
	set(p, 3, 0x0e)
	set(p, 4, 0x0b)
	set(p, 2, 0x12, 0x13, 0x19, 0x1d)
	set(p, 4, 0x14)
	set(p, 5, 0x16)
	set(p, 9, 0x17)
	set(p, 3, 0x1a, 0x1c)
	set(p, 8, 0x1e)
	set(p, 6, 0x1f)
	fill(p, 0x20, 0x2f, 3)
	fill(p, 0x30, 0x33, 4)
	fill(p, 0x34, 0x37, 5)
	set(p, 5, 0x39)
	set(p, 3, 0x3a)
	set(p, 6, 0x3b)
	set(p, 20, 0x3c)
	set(p, 11, 0x3d)
	set(p, 19, 0x3e, 0x3f)
	fill(p, 0x60, 0x6f, 6)
	set(p, 3, 0x6e)
	set(p, 7, 0x61, 0x62, 0x65)
	set(p, 5, 0x6b)
	fill(p, 0x70, 0x7f, 7)
	set(p, 4, 0x7e)
	set(p, 5, 0x7b)
	for _, b := range []int{0x80, 0xc0} {
		set(p, 4, b+0x03, b+0x0c)
		set(p, 3, b+0x0e)
		fill(p, b+0x10, b+0x1f, 4)
		set(p, 6, b+0x13, b+0x1c)
		set(p, 5, b+0x1e, b+0x1f)
		fill(p, b+0x20, b+0x2f, 4)
		set(p, 6, b+0x23, b+0x2c)
		set(p, 5, b+0x2e, b+0x2f)
		fill(p, b+0x30, b+0x3f, 5)
		set(p, 7, b+0x33, b+0x3c)
		set(p, 6, b+0x3e, b+0x3f)
	}
	set(p, 7, 0x8d, 0x9d, 0xad)
	set(p, 8, 0xbd)
	set(p, 3, 0xcc)
	set(p, 5, 0xcd)
	set(p, 5, 0xdc, 0xdd)
	set(p, 5, 0xec, 0xed)
	set(p, 6, 0xfc, 0xfd)
	set(p, 4, 0xc3)
	set(p, 6, 0xd3, 0xe3)
	set(p, 7, 0xf3)

	// page 1, native mode
	p = &cycles[0][1]
	*p = cycles[0][0]
	fill(p, 0x00, 0x0f, 5)
	set(p, 2, 0x0e)
	set(p, 6, 0x01, 0x02, 0x05)
	set(p, 4, 0x0b)
	set(p, 1, 0x12, 0x13, 0x19, 0x1d)
	set(p, 4, 0x16)
	set(p, 7, 0x17)
	set(p, 2, 0x1a)
	set(p, 5, 0x1e)
	set(p, 4, 0x1f)
	fill(p, 0x34, 0x37, 4)
	set(p, 4, 0x39)
	set(p, 1, 0x3a)
	set(p, 22, 0x3c)
	set(p, 10, 0x3d)
	set(p, 21, 0x3f)
	fill(p, 0x40, 0x5f, 1)
	fill(p, 0x70, 0x7f, 6)
	set(p, 3, 0x7e)
	set(p, 7, 0x71, 0x72, 0x75)
	set(p, 5, 0x7b)
	for _, b := range []int{0x80, 0xc0} {
		set(p, 3, b+0x03, b+0x0c)
		fill(p, b+0x10, b+0x1f, 3)
		set(p, 4, b+0x13, b+0x1c, b+0x1e, b+0x1f)
		set(p, 5, b+0x23, b+0x2c)
		fill(p, b+0x30, b+0x3f, 4)
		set(p, 5, b+0x33, b+0x3c, b+0x3e, b+0x3f)
	}
	set(p, 6, 0x8d, 0x9d, 0xad)
	set(p, 7, 0xbd)
	set(p, 3, 0xcc)
	set(p, 5, 0xcd)

	// page 2 ($10)
	p = &cycles[1][0]
	fill(p, 0, 255, 2)
	fill(p, 0x20, 0x2f, 5)
	fill(p, 0x30, 0x37, 4)
	fill(p, 0x38, 0x3b, 6)
	set(p, 20, 0x3f)
	fill(p, 0x40, 0x5f, 3)
	fill(p, 0x80, 0x8f, 5)
	set(p, 4, 0x86, 0x8e)
	fill(p, 0x90, 0x9f, 7)
	set(p, 6, 0x96, 0x97, 0x9e, 0x9f)
	fill(p, 0xa0, 0xaf, 7)
	set(p, 6, 0xa6, 0xa7, 0xae, 0xaf)
	fill(p, 0xb0, 0xbf, 8)
	set(p, 7, 0xb6, 0xb7, 0xbe, 0xbf)
	set(p, 4, 0xce)
	set(p, 8, 0xdc, 0xdd, 0xec, 0xed)
	set(p, 6, 0xde, 0xdf, 0xee, 0xef)
	set(p, 9, 0xfc, 0xfd)
	set(p, 7, 0xfe, 0xff)
	p = &cycles[1][1]
	*p = cycles[1][0]
	set(p, 22, 0x3f)
	fill(p, 0x40, 0x5f, 2)
	fill(p, 0x80, 0x8f, 4)
	fill(p, 0x90, 0x9f, 5)
	fill(p, 0xa0, 0xaf, 6)
	fill(p, 0xb0, 0xbf, 6)
	set(p, 7, 0xdc, 0xdd)
	set(p, 5, 0xde, 0xdf)
	set(p, 8, 0xfc, 0xfd)
	set(p, 6, 0xfe, 0xff)

	// page 3 ($11)
	p = &cycles[2][0]
	fill(p, 0, 255, 2)
	fill(p, 0x30, 0x36, 7)
	set(p, 8, 0x37)
	fill(p, 0x38, 0x3b, 6)
	set(p, 4, 0x3c)
	set(p, 5, 0x3d)
	set(p, 20, 0x3f)
	fill(p, 0x40, 0x5f, 3)
	fill(p, 0x80, 0x8f, 3)
	set(p, 5, 0x83, 0x8c)
	set(p, 25, 0x8d)
	set(p, 34, 0x8e)
	set(p, 28, 0x8f)
	fill(p, 0x90, 0x9f, 5)
	set(p, 7, 0x93, 0x9c)
	fill(p, 0xa0, 0xaf, 5)
	set(p, 7, 0xa3, 0xac)
	fill(p, 0xb0, 0xbf, 6)
	set(p, 8, 0xb3, 0xbc)
	set(p, 27, 0x9d, 0xad)
	set(p, 36, 0x9e, 0xae)
	set(p, 30, 0x9f, 0xaf)
	set(p, 28, 0xbd)
	set(p, 37, 0xbe)
	set(p, 31, 0xbf)
	fill(p, 0xc0, 0xcf, 3)
	fill(p, 0xd0, 0xdf, 5)
	fill(p, 0xe0, 0xef, 5)
	fill(p, 0xf0, 0xff, 6)
	p = &cycles[2][1]
	*p = cycles[2][0]
	fill(p, 0x30, 0x36, 6)
	set(p, 7, 0x37)
	set(p, 22, 0x3f)
	fill(p, 0x40, 0x5f, 2)
	set(p, 4, 0x83, 0x8c)
	fill(p, 0x90, 0x9f, 4)
	set(p, 5, 0x93, 0x9c)
	set(p, 6, 0xa3, 0xac)
	fill(p, 0xb0, 0xbf, 5)
	set(p, 6, 0xb3, 0xbc)
	set(p, 26, 0x9d)
	set(p, 35, 0x9e)
	set(p, 29, 0x9f)
	set(p, 27, 0xbd)
	set(p, 36, 0xbe)
	set(p, 30, 0xbf)
	fill(p, 0xd0, 0xdf, 4)
	fill(p, 0xf0, 0xff, 5)
}

// ---------------------------------------------------------------- state

// StateSize is the length of a saved processor state.
const StateSize = 28

var errState = errors.New("m6809: damaged save state")

// AppendState appends the processor's state (StateSize bytes) to b.
func (c *CPU) AppendState(b []byte) []byte {
	b = append(b, 1, c.A, c.B, c.DP, c.CC, c.E, c.F, c.MD)
	for _, v := range []uint16{c.X, c.Y, c.U, c.S, c.PC, c.V} {
		b = binary.LittleEndian.AppendUint16(b, v)
	}
	var fl uint8
	for i, v := range []bool{c.IRQ, c.FIRQ, c.nmiPending, c.nmiArmed, c.nmiLine} {
		if v {
			fl |= 1 << i
		}
	}
	return append(b, fl, uint8(c.wait), 0, 0, 0, 0, 0, 0)
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != 1 || b[21] > inSYNC {
		return b, errState
	}
	c.A, c.B, c.DP, c.CC, c.E, c.F = b[1], b[2], b[3], b[4], b[5], b[6]
	c.MD = b[7]
	c.native = int(c.MD & MDNative)
	w := make([]uint16, 6)
	for i := range w {
		w[i] = binary.LittleEndian.Uint16(b[8+i*2:])
	}
	c.X, c.Y, c.U, c.S, c.PC, c.V = w[0], w[1], w[2], w[3], w[4], w[5]
	fl := b[20]
	c.IRQ, c.FIRQ, c.nmiPending, c.nmiArmed, c.nmiLine = fl&1 != 0, fl&2 != 0, fl&4 != 0, fl&8 != 0, fl&16 != 0
	c.wait = int(b[21])
	return b[StateSize:], nil
}
