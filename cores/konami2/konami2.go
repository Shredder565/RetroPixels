// Package konami2 is RetroPixels' Konami 053248 ("Konami-2") processor, a Go
// port of web/emu/konami2.js (used by The Simpsons arcade board). It behaves
// exactly like the JS core: the same instructions, flags and cycle counts.
//
// The 053248 is Konami's own member of the Motorola 6809 family: the same
// registers (A, B / D, X, Y, U, S, PC, DP, CC) and interrupts (NMI, FIRQ, IRQ
// with the 6809's vectors), a different opcode map where every memory operand
// uses one "index" postbyte, and extra block, 16-bit and multiply / divide
// instructions, plus SETLINES, which puts a byte on the chip's spare output
// lines (boards use them to switch ROM banks).
//
//	cpu := konami2.New(bus)
//	cpu.Reset()
//	n := cpu.Step()          // one instruction (or interrupt); returns cycles
//	cpu.IRQ, cpu.FIRQ = ...  // level-sensitive lines
//	cpu.NMI()                // edge
package konami2

import (
	"encoding/binary"
	"errors"
)

// Bus is the memory the processor sees (16-bit addresses).
type Bus interface {
	Read(addr uint16) uint8
	Write(addr uint16, v uint8)
}

// LineSetter is implemented by buses wired to SETLINES' output lines.
type LineSetter interface {
	SetLines(v uint8)
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

const (
	fC = 0x01
	fV = 0x02
	fZ = 0x04
	fN = 0x08
	fI = 0x10
	fH = 0x20
	fF = 0x40
	fE = 0x80
)

func s8(v int) int  { return int(int8(uint8(v))) }
func s16(v int) int { return int(int16(uint16(v))) }

// base cycles of each opcode (indexed forms add their addressing time)
var cyc [256]int

func init() {
	for i := range cyc {
		cyc[i] = 2
	}
	set := func(n int, ops ...int) {
		for _, o := range ops {
			cyc[o] = n
		}
	}
	for o := 0x08; o <= 0x0b; o++ {
		cyc[o] = 4
	}
	set(5, 0x0c, 0x0d, 0x0e, 0x0f)
	for o := 0x10; o <= 0x3b; o++ {
		if o&2 != 0 {
			cyc[o] = 4
		} else {
			cyc[o] = 2
		}
	}
	set(2, 0x38)
	set(4, 0x39)
	set(3, 0x3c, 0x3d)
	set(8, 0x3e)
	set(6, 0x3f)
	for o := 0x40; o <= 0x53; o++ {
		if o&1 != 0 {
			cyc[o] = 5
		} else {
			cyc[o] = 3
		}
	}
	set(4, 0x54, 0x56)
	set(6, 0x55, 0x57)
	for o := 0x58; o <= 0x5c; o++ {
		cyc[o] = 5
	}
	for o := 0x60; o <= 0x67; o++ {
		cyc[o], cyc[o+0x10], cyc[o+8], cyc[o+0x18] = 3, 3, 5, 5
	}
	for o := 0x80; o <= 0xa7; o++ {
		cyc[o] = 2
	}
	set(6, 0x82, 0x85, 0x88, 0x8b, 0x8e, 0x92, 0x95, 0x98, 0x9b, 0x9e, 0xa2)
	set(8, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7)
	set(5, 0x8f)
	set(6, 0x9f)
	set(3, 0xa8)
	set(7, 0xa9)
	set(7, 0xaa)
	set(9, 0xab)
	set(3, 0xac, 0xad)
	set(3, 0xb0)
	set(11, 0xb3)
	set(18, 0xb4)
	set(20, 0xb5)
	set(2, 0xb6)
	set(2, 0xb7)
	for o := 0xb8; o <= 0xc1; o++ {
		if o&1 != 0 {
			cyc[o] = 6
		} else {
			cyc[o] = 4
		}
	}
	set(3, 0xc2, 0xc4, 0xc6, 0xc8, 0xca)
	set(7, 0xc3, 0xc5, 0xc7, 0xc9, 0xcb)
	set(3, 0xcc, 0xcd, 0xce)
	set(2, 0xcf, 0xd0)
}

// CPU is one 053248.
type CPU struct {
	bus   Bus
	lines LineSetter
	ack   Acker

	A, B, X, Y, U, S, PC, DP, CC int

	IRQ, FIRQ  bool // the interrupt lines (level)
	nmiPending bool
	nmiArmed   bool
	cycles     int // extra cycles of the instruction being run
	Lines      int // last SETLINES value
}

// New makes a processor on bus. Call Reset to start it.
func New(bus Bus) *CPU {
	c := &CPU{bus: bus}
	c.lines, _ = bus.(LineSetter)
	c.ack, _ = bus.(Acker)
	return c
}

func (c *CPU) rd(a int) int   { return int(c.bus.Read(uint16(a))) }
func (c *CPU) wr(a, v int)    { c.bus.Write(uint16(a), uint8(v)) }
func (c *CPU) rd16(a int) int { return c.rd(a)<<8 | c.rd(a+1) }
func (c *CPU) wr16(a, v int)  { c.wr(a, v>>8); c.wr(a+1, v) }
func (c *CPU) imm8() int      { v := c.rd(c.PC); c.PC = (c.PC + 1) & 0xffff; return v }
func (c *CPU) imm16() int     { v := c.rd16(c.PC); c.PC = (c.PC + 2) & 0xffff; return v }
func (c *CPU) d() int         { return c.A<<8 | c.B }
func (c *CPU) setD(v int)     { c.A = (v >> 8) & 0xff; c.B = v & 0xff }
func (c *CPU) push8S(v int)   { c.S = (c.S - 1) & 0xffff; c.wr(c.S, v) }
func (c *CPU) push16S(v int)  { c.push8S(v); c.push8S(v >> 8) }
func (c *CPU) pull8S() int    { v := c.rd(c.S); c.S = (c.S + 1) & 0xffff; return v }
func (c *CPU) pull16S() int   { h := c.pull8S(); return h<<8 | c.pull8S() }
func (c *CPU) op8(ix bool) int {
	if ix {
		return c.rd(c.ea())
	}
	return c.imm8()
}
func (c *CPU) op16(ix bool) int {
	if ix {
		return c.rd16(c.ea())
	}
	return c.imm16()
}

// Reset is the processor's reset: interrupts masked, PC from the reset vector.
func (c *CPU) Reset() {
	c.DP = 0
	c.CC = fI | fF
	c.nmiPending, c.nmiArmed = false, false
	c.PC = c.rd16(0xfffe)
	c.Lines = 0
}

// NMI makes a non-maskable interrupt happen before the next instruction.
func (c *CPU) NMI() { c.nmiPending = true }

func (c *CPU) ireg(r int) int {
	switch r {
	case 2:
		return c.X
	case 3:
		return c.Y
	case 5:
		return c.U
	case 6:
		return c.S
	}
	return c.PC
}

func (c *CPU) setIreg(r, v int) {
	v &= 0xffff
	switch r {
	case 2:
		c.X = v
	case 3:
		c.Y = v
	case 5:
		c.U = v
	case 6:
		c.S = v
	default:
		c.PC = v
	}
}

func (c *CPU) ea() int {
	pb := c.imm8()
	var addr int
	if pb == 0x07 || pb == 0x0f {
		addr = c.imm16()
		c.cycles += 2
	} else if pb == 0xc4 || pb == 0xcc {
		addr = c.DP<<8 | c.imm8()
		c.cycles++
	} else {
		r, t := (pb>>4)&7, pb&7
		if pb&0x80 != 0 {
			off := 0
			switch t {
			case 0:
				off = s8(c.A)
			case 1:
				off = s8(c.B)
			case 7:
				off = s16(c.d())
			}
			addr = c.ireg(r) + off
			if t == 7 {
				c.cycles += 4
			} else {
				c.cycles++
			}
		} else {
			switch t {
			case 0:
				addr = c.ireg(r)
				c.setIreg(r, addr+1)
				c.cycles += 2
			case 1:
				addr = c.ireg(r)
				c.setIreg(r, addr+2)
				c.cycles += 3
			case 2:
				addr = (c.ireg(r) - 1) & 0xffff
				c.setIreg(r, addr)
				c.cycles += 2
			case 3:
				addr = (c.ireg(r) - 2) & 0xffff
				c.setIreg(r, addr)
				c.cycles += 3
			case 4:
				o := s8(c.imm8())
				addr = c.ireg(r) + o
				c.cycles++
			case 5:
				o := s16(c.imm16())
				addr = c.ireg(r) + o
				c.cycles += 4
			default:
				addr = c.ireg(r)
			}
		}
	}
	addr &= 0xffff
	if pb&0x08 != 0 {
		addr = c.rd16(addr)
		c.cycles += 3
	}
	return addr
}

func b2i(b bool, v int) int {
	if b {
		return v
	}
	return 0
}

func (c *CPU) nz8(r int) { c.CC = (c.CC &^ (fN | fZ)) | b2i(r&0x80 != 0, fN) | b2i(r&0xff == 0, fZ) }
func (c *CPU) nz16(r int) {
	c.CC = (c.CC &^ (fN | fZ)) | b2i(r&0x8000 != 0, fN) | b2i(r&0xffff == 0, fZ)
}

func (c *CPU) add8(a, b, cy int) int {
	r := a + b + cy
	c.CC = (c.CC &^ (fH | fN | fZ | fV | fC)) | b2i((a^b^r)&0x10 != 0, fH) | b2i(r&0x80 != 0, fN) |
		b2i(r&0xff == 0, fZ) | b2i((a^r)&(b^r)&0x80 != 0, fV) | b2i(r&0x100 != 0, fC)
	return r & 0xff
}

func (c *CPU) sub8(a, b, cy int) int {
	r := a - b - cy
	c.CC = (c.CC &^ (fN | fZ | fV | fC)) | b2i(r&0x80 != 0, fN) | b2i(r&0xff == 0, fZ) |
		b2i((a^b)&(a^r)&0x80 != 0, fV) | b2i(r&0x100 != 0, fC)
	return r & 0xff
}

func (c *CPU) add16(a, b int) int {
	r := a + b
	c.CC = (c.CC &^ (fN | fZ | fV | fC)) | b2i(r&0x8000 != 0, fN) | b2i(r&0xffff == 0, fZ) |
		b2i((a^r)&(b^r)&0x8000 != 0, fV) | b2i(r&0x10000 != 0, fC)
	return r & 0xffff
}

func (c *CPU) sub16(a, b int) int {
	r := a - b
	c.CC = (c.CC &^ (fN | fZ | fV | fC)) | b2i(r&0x8000 != 0, fN) | b2i(r&0xffff == 0, fZ) |
		b2i((a^b)&(a^r)&0x8000 != 0, fV) | b2i(r&0x10000 != 0, fC)
	return r & 0xffff
}

func (c *CPU) logic8(r int) int  { c.nz8(r); c.CC &^= fV; return r & 0xff }
func (c *CPU) logic16(r int) int { c.nz16(r); c.CC &^= fV; return r & 0xffff }

// one-operand operations
const (
	uClr = iota
	uCom
	uNeg
	uInc
	uDec
	uTst
	uLsr
	uRor
	uAsr
	uAsl
	uRol
	uAbs
)

// unary: one-operand ops; w16 for 16 bits. Values are non-negative here.
func (c *CPU) unary(kind, v int, w16 bool) int {
	top, mask := 0x80, 0xff
	if w16 {
		top, mask = 0x8000, 0xffff
	}
	cy := c.CC & fC
	r := 0
	cc := c.CC &^ (fN | fZ | fV)
	switch kind {
	case uClr:
		r = 0
		cc &^= fC
	case uCom:
		r = ^v & mask
		cc |= fC
	case uNeg:
		r = (-v) & mask
		if v == top {
			cc |= fV
		}
		if v != 0 {
			cc |= fC
		} else {
			cc &^= fC
		}
	case uInc:
		r = (v + 1) & mask
		if r == top {
			cc |= fV
		}
	case uDec:
		r = (v - 1) & mask
		if v == top {
			cc |= fV
		}
	case uTst:
		r = v
	case uLsr:
		r = v >> 1
		cc = (cc &^ fC) | (v & 1)
	case uRor:
		r = v>>1 | b2i(cy != 0, top)
		cc = (cc &^ fC) | (v & 1)
	case uAsr:
		r = v>>1 | (v & top)
		cc = (cc &^ fC) | (v & 1)
	case uAsl:
		r = (v << 1) & mask
		cc = (cc &^ fC) | b2i(v&top != 0, fC)
		if ((v<<1)^v)&top != 0 {
			cc |= fV
		}
	case uRol:
		r = (v<<1 | cy) & mask
		cc = (cc &^ fC) | b2i(v&top != 0, fC)
		if ((v<<1)^v)&top != 0 {
			cc |= fV
		}
	case uAbs:
		if v&top != 0 {
			r = (-v) & mask
			if v == top {
				cc |= fV
			}
		} else {
			r = v
		}
	}
	cc |= b2i(r&top != 0, fN) | b2i(r == 0, fZ)
	c.CC = cc
	return r
}

func (c *CPU) rmw8(kind int) { a := c.ea(); c.wr(a, c.unary(kind, c.rd(a), false)) }
func (c *CPU) rmw16(kind int) {
	a := c.ea()
	r := c.unary(kind, c.rd16(a), true)
	if kind != uTst {
		c.wr16(a, r)
	}
}

func (c *CPU) shiftD(kind, n int) {
	v := c.d()
	for i := 0; i < n; i++ {
		v = c.unary(kind, v, true)
	}
	if n == 0 {
		c.nz16(v)
	}
	c.setD(v)
	c.cycles += n
}

func (c *CPU) cond(k int) bool {
	cc := c.CC
	n, z, v, cy := (cc>>3)&1, (cc>>2)&1, (cc>>1)&1, cc&1
	switch k {
	case 0:
		return true
	case 1:
		return cy|z == 0
	case 2:
		return cy == 0
	case 3:
		return z == 0
	case 4:
		return v == 0
	case 5:
		return n == 0
	case 6:
		return n == v
	}
	return z == 0 && n == v
}

func (c *CPU) regGet(r int) int {
	switch r & 7 {
	case 0:
		return c.A
	case 1:
		return c.B
	case 2:
		return c.X
	case 3:
		return c.Y
	case 4:
		return c.S
	case 5:
		return c.U
	}
	return 0xff
}

func (c *CPU) regSet(r, v int) {
	switch r & 7 {
	case 0:
		c.A = v & 0xff
	case 1:
		c.B = v & 0xff
	case 2:
		c.X = v & 0xffff
	case 3:
		c.Y = v & 0xffff
	case 4:
		c.S = v & 0xffff
	case 5:
		c.U = v & 0xffff
	}
}

func (c *CPU) pushRegs(useU bool, mask int) {
	sp := c.S
	if useU {
		sp = c.U
	}
	p8 := func(v int) { sp = (sp - 1) & 0xffff; c.wr(sp, v); c.cycles++ }
	p16 := func(v int) { p8(v); p8(v >> 8) }
	if mask&0x80 != 0 {
		p16(c.PC)
	}
	if mask&0x40 != 0 {
		if useU {
			p16(c.S)
		} else {
			p16(c.U)
		}
	}
	if mask&0x20 != 0 {
		p16(c.Y)
	}
	if mask&0x10 != 0 {
		p16(c.X)
	}
	if mask&0x08 != 0 {
		p8(c.DP)
	}
	if mask&0x04 != 0 {
		p8(c.B)
	}
	if mask&0x02 != 0 {
		p8(c.A)
	}
	if mask&0x01 != 0 {
		p8(c.CC)
	}
	if useU {
		c.U = sp
	} else {
		c.S = sp
	}
}

func (c *CPU) pullRegs(useU bool, mask int) {
	sp := c.S
	if useU {
		sp = c.U
	}
	q8 := func() int { v := c.rd(sp); sp = (sp + 1) & 0xffff; c.cycles++; return v }
	q16 := func() int { h := q8(); return h<<8 | q8() }
	if mask&0x01 != 0 {
		c.CC = q8()
	}
	if mask&0x02 != 0 {
		c.A = q8()
	}
	if mask&0x04 != 0 {
		c.B = q8()
	}
	if mask&0x08 != 0 {
		c.DP = q8()
	}
	if mask&0x10 != 0 {
		c.X = q16()
	}
	if mask&0x20 != 0 {
		c.Y = q16()
	}
	if mask&0x40 != 0 {
		v := q16()
		if useU {
			c.S = v
		} else {
			c.U = v
		}
	}
	if mask&0x80 != 0 {
		c.PC = q16()
	}
	if useU {
		c.U = sp
	} else {
		c.S = sp
	}
}

func (c *CPU) interrupt(vec int, full bool, mask int) {
	if full {
		c.CC |= fE
		c.pushRegs(false, 0xff)
	} else {
		c.CC &^= fE
		c.pushRegs(false, 0x81)
	}
	c.CC |= mask
	c.PC = c.rd16(vec)
}

// Step runs one instruction (or takes an interrupt) and returns the clock
// cycles used.
func (c *CPU) Step() int {
	c.cycles = 0
	if c.nmiPending {
		c.nmiPending = false
		c.interrupt(0xfffc, true, fI|fF)
		if c.ack != nil {
			c.ack.Ack(AckNMI)
		}
		return c.cycles + 7
	}
	if c.FIRQ && c.CC&fF == 0 {
		c.interrupt(0xfff6, false, fI|fF)
		if c.ack != nil {
			c.ack.Ack(AckFIRQ)
		}
		return c.cycles + 7
	}
	if c.IRQ && c.CC&fI == 0 {
		c.interrupt(0xfff8, true, fI)
		if c.ack != nil {
			c.ack.Ack(AckIRQ)
		}
		return c.cycles + 7
	}
	op := c.imm8()
	c.exec(op)
	return cyc[op] + c.cycles
}

func (c *CPU) branch(off int) { c.PC = (c.PC + off) & 0xffff }

func (c *CPU) exec(op int) {
	// 0x10-0x37: 8-bit A/B operations: A imm, B imm, A idx, B idx
	if op >= 0x10 && op <= 0x37 {
		ix, isB, g := op&2 != 0, op&1 != 0, (op-0x10)>>2
		v := c.op8(ix)
		r := c.A
		if isB {
			r = c.B
		}
		out, keep := r, true
		switch g {
		case 0:
			out = v
			c.logic8(v)
		case 1:
			out = c.add8(r, v, 0)
		case 2:
			out = c.add8(r, v, c.CC&fC)
		case 3:
			out = c.sub8(r, v, 0)
		case 4:
			out = c.sub8(r, v, c.CC&fC)
		case 5:
			out = c.logic8(r & v)
		case 6:
			c.logic8(r & v)
			keep = false
		case 7:
			out = c.logic8(r ^ v)
		case 8:
			out = c.logic8(r | v)
		case 9:
			c.sub8(r, v, 0)
			keep = false
		}
		if keep {
			if isB {
				c.B = out
			} else {
				c.A = out
			}
		}
		return
	}
	// 0x40-0x53: 16-bit loads and compares
	if op >= 0x40 && op <= 0x53 {
		v := c.op16(op&1 != 0)
		switch (op - 0x40) >> 1 {
		case 0:
			c.setD(c.logic16(v))
		case 1:
			c.X = c.logic16(v)
		case 2:
			c.Y = c.logic16(v)
		case 3:
			c.U = c.logic16(v)
		case 4:
			c.S = c.logic16(v)
			c.nmiArmed = true
		case 5:
			c.sub16(c.d(), v)
		case 6:
			c.sub16(c.X, v)
		case 7:
			c.sub16(c.Y, v)
		case 8:
			c.sub16(c.U, v)
		case 9:
			c.sub16(c.S, v)
		}
		return
	}
	// 0x60-0x7f: short and long branches
	if op >= 0x60 && op <= 0x7f {
		long := op&8 != 0
		var off int
		if long {
			off = s16(c.imm16())
		} else {
			off = s8(c.imm8())
		}
		t := c.cond(op & 7)
		if op&0x10 != 0 {
			t = !t
		}
		if t {
			c.branch(off)
			if long {
				c.cycles++
			}
		}
		return
	}
	switch op {
	case 0x08: // LEAX
		c.X = c.ea()
		c.CC = (c.CC &^ fZ) | b2i(c.X == 0, fZ)
	case 0x09: // LEAY
		c.Y = c.ea()
		c.CC = (c.CC &^ fZ) | b2i(c.Y == 0, fZ)
	case 0x0a: // LEAU
		c.U = c.ea()
	case 0x0b: // LEAS
		c.S = c.ea()
		c.nmiArmed = true
	case 0x0c:
		c.pushRegs(false, c.imm8())
	case 0x0d:
		c.pushRegs(true, c.imm8())
	case 0x0e:
		c.pullRegs(false, c.imm8())
	case 0x0f:
		c.pullRegs(true, c.imm8())
	case 0x38, 0x39: // SETLINES
		v := c.op8(op&1 != 0)
		c.Lines = v
		if c.lines != nil {
			c.lines.SetLines(uint8(v))
		}
	case 0x3a: // STA
		c.logic8(c.A)
		c.wr(c.ea(), c.A)
	case 0x3b: // STB
		c.logic8(c.B)
		c.wr(c.ea(), c.B)
	case 0x3c: // ANDCC
		c.CC &= c.imm8()
	case 0x3d: // ORCC
		c.CC |= c.imm8()
	case 0x3e: // EXG
		p := c.imm8()
		r1, r2 := p>>4, p&15
		v1, v2 := c.regGet(r1), c.regGet(r2)
		c.regSet(r1, v2)
		c.regSet(r2, v1)
	case 0x3f: // TFR: low nibble is the source, high the destination
		p := c.imm8()
		c.regSet(p>>4, c.regGet(p&15))
	case 0x54, 0x55: // ADDD
		c.setD(c.add16(c.d(), c.op16(op&1 != 0)))
	case 0x56, 0x57: // SUBD
		c.setD(c.sub16(c.d(), c.op16(op&1 != 0)))
	case 0x58:
		a := c.ea()
		c.logic16(c.d())
		c.wr16(a, c.d())
	case 0x59:
		a := c.ea()
		c.logic16(c.X)
		c.wr16(a, c.X)
	case 0x5a:
		a := c.ea()
		c.logic16(c.Y)
		c.wr16(a, c.Y)
	case 0x5b:
		a := c.ea()
		c.logic16(c.U)
		c.wr16(a, c.U)
	case 0x5c:
		a := c.ea()
		c.logic16(c.S)
		c.wr16(a, c.S)

	case 0x80:
		c.A = c.unary(uClr, c.A, false)
	case 0x81:
		c.B = c.unary(uClr, c.B, false)
	case 0x82:
		c.rmw8(uClr)
	case 0x83:
		c.A = c.unary(uCom, c.A, false)
	case 0x84:
		c.B = c.unary(uCom, c.B, false)
	case 0x85:
		c.rmw8(uCom)
	case 0x86:
		c.A = c.unary(uNeg, c.A, false)
	case 0x87:
		c.B = c.unary(uNeg, c.B, false)
	case 0x88:
		c.rmw8(uNeg)
	case 0x89:
		c.A = c.unary(uInc, c.A, false)
	case 0x8a:
		c.B = c.unary(uInc, c.B, false)
	case 0x8b:
		c.rmw8(uInc)
	case 0x8c:
		c.A = c.unary(uDec, c.A, false)
	case 0x8d:
		c.B = c.unary(uDec, c.B, false)
	case 0x8e:
		c.rmw8(uDec)
	case 0x8f: // RTS
		c.PC = c.pull16S()
	case 0x90:
		c.unary(uTst, c.A, false)
	case 0x91:
		c.unary(uTst, c.B, false)
	case 0x92:
		c.unary(uTst, c.rd(c.ea()), false)
	case 0x93:
		c.A = c.unary(uLsr, c.A, false)
	case 0x94:
		c.B = c.unary(uLsr, c.B, false)
	case 0x95:
		c.rmw8(uLsr)
	case 0x96:
		c.A = c.unary(uRor, c.A, false)
	case 0x97:
		c.B = c.unary(uRor, c.B, false)
	case 0x98:
		c.rmw8(uRor)
	case 0x99:
		c.A = c.unary(uAsr, c.A, false)
	case 0x9a:
		c.B = c.unary(uAsr, c.B, false)
	case 0x9b:
		c.rmw8(uAsr)
	case 0x9c:
		c.A = c.unary(uAsl, c.A, false)
	case 0x9d:
		c.B = c.unary(uAsl, c.B, false)
	case 0x9e:
		c.rmw8(uAsl)
	case 0x9f: // RTI
		c.CC = c.pull8S()
		if c.CC&fE != 0 {
			c.pullRegs(false, 0xfe)
		} else {
			c.PC = c.pull16S()
		}
	case 0xa0:
		c.A = c.unary(uRol, c.A, false)
	case 0xa1:
		c.B = c.unary(uRol, c.B, false)
	case 0xa2:
		c.rmw8(uRol)
	case 0xa3:
		c.rmw16(uLsr)
	case 0xa4:
		c.rmw16(uRor)
	case 0xa5:
		c.rmw16(uAsr)
	case 0xa6:
		c.rmw16(uAsl)
	case 0xa7:
		c.rmw16(uRol)
	case 0xa8: // JMP
		c.PC = c.ea()
	case 0xa9: // JSR
		a := c.ea()
		c.push16S(c.PC)
		c.PC = a
	case 0xaa: // BSR
		o := s8(c.imm8())
		c.push16S(c.PC)
		c.branch(o)
	case 0xab: // LBSR
		o := s16(c.imm16())
		c.push16S(c.PC)
		c.branch(o)
	case 0xac: // DECB,JNZ
		o := s8(c.imm8())
		c.B = c.unary(uDec, c.B, false)
		if c.B != 0 {
			c.branch(o)
		}
	case 0xad: // DECX,JNZ
		o := s8(c.imm8())
		c.X = (c.X - 1) & 0xffff
		c.nz16(c.X)
		if c.X != 0 {
			c.branch(o)
		}
	case 0xae: // NOP
	case 0xb0: // ABX
		c.X = (c.X + c.B) & 0xffff
	case 0xb1: // DAA
		a, cf := c.A, c.CC&fC
		adj, cy := 0, cf
		if c.CC&fH != 0 || a&0x0f > 9 {
			adj |= 0x06
		}
		if cf != 0 || a>>4 > 9 || (a>>4 > 8 && a&0x0f > 9) {
			adj |= 0x60
			cy = 1
		}
		r := a + adj
		c.A = r & 0xff
		c.nz8(c.A)
		c.CC = (c.CC &^ (fC | fV)) | b2i(cy != 0 || r&0x100 != 0, fC)
	case 0xb2: // SEX
		if c.B&0x80 != 0 {
			c.A = 0xff
		} else {
			c.A = 0
		}
		c.nz16(c.d())
	case 0xb3: // MUL
		r := c.A * c.B
		c.setD(r)
		c.CC = (c.CC &^ (fZ | fC)) | b2i(r == 0, fZ) | b2i(r&0x80 != 0, fC)
	case 0xb4: // LMUL: X:Y = X * Y
		r := c.X * c.Y
		c.X = (r >> 16) & 0xffff
		c.Y = r & 0xffff
		c.CC = (c.CC &^ (fZ | fC)) | b2i(r == 0, fZ) | b2i(r&0x8000 != 0, fC)
	case 0xb5: // DIVX: X = X / B, B = X % B
		if d := c.B; d != 0 {
			q, r := c.X/d, c.X%d
			c.X = q & 0xffff
			c.B = r
		} else {
			c.X, c.B = 0, 0
		}
		c.CC = (c.CC &^ (fZ | fC)) | b2i(c.X == 0, fZ) | b2i(c.X&0x80 != 0, fC)
	case 0xb6: // BMOVE
		for c.U != 0 {
			c.wr(c.X, c.rd(c.Y))
			c.X = (c.X + 1) & 0xffff
			c.Y = (c.Y + 1) & 0xffff
			c.U = (c.U - 1) & 0xffff
			c.cycles += 2
		}
	case 0xb7: // MOVE
		c.wr(c.X, c.rd(c.Y))
		c.X = (c.X + 1) & 0xffff
		c.Y = (c.Y + 1) & 0xffff
		c.U = (c.U - 1) & 0xffff
	case 0xb8, 0xb9:
		c.shiftD(uLsr, c.op8(op&1 != 0))
	case 0xba, 0xbb:
		c.shiftD(uRor, c.op8(op&1 != 0))
	case 0xbc, 0xbd:
		c.shiftD(uAsr, c.op8(op&1 != 0))
	case 0xbe, 0xbf:
		c.shiftD(uAsl, c.op8(op&1 != 0))
	case 0xc0, 0xc1:
		c.shiftD(uRol, c.op8(op&1 != 0))
	case 0xc2:
		c.setD(c.unary(uClr, c.d(), true))
	case 0xc3:
		c.rmw16(uClr)
	case 0xc4:
		c.setD(c.unary(uNeg, c.d(), true))
	case 0xc5:
		c.rmw16(uNeg)
	case 0xc6:
		c.setD(c.unary(uInc, c.d(), true))
	case 0xc7:
		c.rmw16(uInc)
	case 0xc8:
		c.setD(c.unary(uDec, c.d(), true))
	case 0xc9:
		c.rmw16(uDec)
	case 0xca:
		c.unary(uTst, c.d(), true)
	case 0xcb:
		c.rmw16(uTst)
	case 0xcc:
		c.A = c.unary(uAbs, c.A, false)
	case 0xcd:
		c.B = c.unary(uAbs, c.B, false)
	case 0xce:
		c.setD(c.unary(uAbs, c.d(), true))
	case 0xcf: // BSET
		for c.U != 0 {
			c.wr(c.X, c.A)
			c.X = (c.X + 1) & 0xffff
			c.U = (c.U - 1) & 0xffff
			c.cycles += 2
		}
	case 0xd0: // BSETW
		for c.U != 0 {
			c.wr16(c.X, c.d())
			c.X = (c.X + 2) & 0xffff
			c.U = (c.U - 1) & 0xffff
			c.cycles += 3
		}
	}
}

// StateSize is the length of a saved processor state.
const StateSize = 1 + 9*2 + 1 + 1

var errState = errors.New("konami2: damaged save state")

// AppendState appends the processor's state (StateSize bytes) to b.
func (c *CPU) AppendState(b []byte) []byte {
	b = append(b, 1)
	for _, v := range []int{c.A, c.B, c.X, c.Y, c.U, c.S, c.PC, c.DP, c.CC} {
		b = binary.LittleEndian.AppendUint16(b, uint16(v))
	}
	fl := byte(0)
	for i, v := range []bool{c.IRQ, c.FIRQ, c.nmiPending, c.nmiArmed} {
		if v {
			fl |= 1 << i
		}
	}
	return append(b, fl, byte(c.Lines))
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != 1 {
		return b, errState
	}
	r := []*int{&c.A, &c.B, &c.X, &c.Y, &c.U, &c.S, &c.PC, &c.DP, &c.CC}
	for i, p := range r {
		*p = int(binary.LittleEndian.Uint16(b[1+i*2:]))
	}
	fl := b[19]
	c.IRQ, c.FIRQ, c.nmiPending, c.nmiArmed = fl&1 != 0, fl&2 != 0, fl&4 != 0, fl&8 != 0
	c.Lines = int(b[20])
	return b[StateSize:], nil
}
