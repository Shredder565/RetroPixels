// Package m6801 is RetroPixels' Motorola 6801 family processor: the MC6801 /
// MC6803 and Hitachi's HD6301 / HD63701 (V and Y models), single-chip
// microcomputers built on the 6800.
//
// The processor has accumulators A and B (together D), index register X,
// stack pointer S and the condition codes; the 6801 adds 16-bit loads,
// stores and arithmetic on D, ABX, MUL, PSHX / PULX and LSRD / ASLD. The
// Hitachi parts add XGDX, SLP and the bit-manipulation AIM / OIM / EIM /
// TIM, take fewer cycles for most instructions, and trap illegal opcodes.
//
// On chip are the I/O ports (with data-direction registers), the 16-bit
// free-running timer with output compare and input capture, and RAM; the
// package handles their registers at $00-$1F ($00-$27 on the Y models) and
// the RAM, and passes everything else to the Bus. Port pins reach the
// machine through Ports. The serial port only keeps its registers.
//
//	cpu := m6801.New(bus, m6801.HD63701Y)
//	cpu.Reset()
//	n := cpu.Step()    // one instruction (or interrupt); returns cycles
//	cpu.IRQ = ...      // IRQ1 level
//	cpu.NMI()          // edge
package m6801

import (
	"errors"
)

// Bus is the memory outside the chip (16-bit addresses).
type Bus interface {
	Read(addr uint16) uint8
	Write(addr uint16, v uint8)
}

// Ports is implemented by buses wired to the chip's I/O ports (1-7).
// PortRead gives the pins' levels; PortWrite gets the port's output latch
// and its data-direction register (1 bits are outputs).
type Ports interface {
	PortRead(port int) uint8
	PortWrite(port int, v, ddr uint8)
}

// Model is the chip.
type Model int

const (
	MC6801   Model = iota // Motorola MC6801 / MC6803: 128 bytes of RAM at $80
	HD6301V               // Hitachi HD6301V / HD63701V: 128 bytes at $80
	HD63701Y              // Hitachi HD6301Y / HD63701Y: 256 bytes at $40, ports 5 and 6
)

// Condition code bits (bits 6 and 7 always read 1).
const (
	FlagC = 0x01
	FlagV = 0x02
	FlagZ = 0x04
	FlagN = 0x08
	FlagI = 0x10
	FlagH = 0x20
)

// Vectors.
const (
	VecTrap   = 0xffee // Hitachi: illegal opcode / address error
	VecSCI    = 0xfff0
	VecTOF    = 0xfff2 // timer overflow
	VecOCF    = 0xfff4 // output compare
	VecICF    = 0xfff6 // input capture
	VecIRQ    = 0xfff8 // IRQ1
	VecSWI    = 0xfffa
	VecNMI    = 0xfffc
	VecReset  = 0xfffe
	timerTOF  = 0x20
	timerOCF  = 0x40
	timerICF  = 0x80
	timerETOI = 0x04
	timerEOCI = 0x08
	timerEICI = 0x10
)

// CPU is one 6801-family chip.
type CPU struct {
	bus   Bus
	ports Ports
	model Model

	A, B, CC uint8
	X, S, PC uint16

	// IRQ is the level of the IRQ1 line.
	IRQ bool

	nmiPending bool
	nmiLine    bool
	wait       bool // in WAI or SLP
	stacked    bool // WAI stacked the state already

	ram     []uint8
	ramBase uint16
	regs    [0x28]uint8 // internal registers as last written
	frc     uint16      // free-running counter
	ocr     uint16
	icr     uint16
	tcsr    uint8
	pending uint8 // timer flags the program hasn't yet cleared (read TCSR first)
	tmpFRC  uint8 // latched low byte of the counter
}

// New makes a processor of the given model on bus, reset. A bus that is also
// Ports gets the port pins.
func New(bus Bus, model Model) *CPU {
	c := &CPU{bus: bus, model: model}
	c.ports, _ = bus.(Ports)
	if model == HD63701Y {
		c.ram, c.ramBase = make([]uint8, 256), 0x40
	} else {
		c.ram, c.ramBase = make([]uint8, 128), 0x80
	}
	c.Reset()
	return c
}

func (c *CPU) hitachi() bool { return c.model != MC6801 }

// Reset is the RESET line: interrupts masked, ports as inputs, the timer
// cleared and the program counter from $FFFE. RAM keeps its contents.
func (c *CPU) Reset() {
	c.CC |= 0xc0 | FlagI
	c.wait = false
	c.nmiPending = false
	for i := range c.regs {
		c.regs[i] = 0
	}
	c.frc, c.ocr, c.icr = 0, 0xffff, 0
	c.tcsr, c.pending = 0, 0
	c.PC = c.rd16(VecReset)
}

// NMI makes a non-maskable interrupt (an edge on the NMI line).
func (c *CPU) NMI() { c.nmiPending = true }

// SetNMI sets the level of the NMI line; an edge to asserted makes an
// interrupt.
func (c *CPU) SetNMI(on bool) {
	if on && !c.nmiLine {
		c.nmiPending = true
	}
	c.nmiLine = on
}

// D is A:B.
func (c *CPU) D() uint16 { return uint16(c.A)<<8 | uint16(c.B) }

// SetD sets A:B.
func (c *CPU) SetD(v uint16) { c.A, c.B = uint8(v>>8), uint8(v) }

// Waiting tells whether the processor is stopped in WAI or SLP.
func (c *CPU) Waiting() bool { return c.wait }

// RAM is the chip's internal RAM.
func (c *CPU) RAM() []uint8 { return c.ram }

// ---------------------------------------------------------------- on-chip registers

func (c *CPU) nregs() uint16 {
	if c.model == HD63701Y {
		return 0x28
	}
	return 0x20
}

// ddrs: each port's DDR and data registers.
var ddrs = []struct {
	ddr, data uint16
	port      int
}{{0x00, 0x02, 1}, {0x01, 0x03, 2}, {0x04, 0x06, 3}, {0x05, 0x07, 4}, {0x20, 0x15, 5}, {0x16, 0x17, 6}}

// port registers: data register -> (port number, DDR register)
var portRegs = map[uint16][2]int{0x02: {1, 0x00}, 0x03: {2, 0x01}, 0x06: {3, 0x04}, 0x07: {4, 0x05}}
var portRegsY = map[uint16][2]int{0x15: {5, 0x20}, 0x17: {6, 0x16}, 0x18: {7, -1}}

func (c *CPU) portOf(a uint16) (port, ddr int, ok bool) {
	if p, ok := portRegs[a]; ok {
		return p[0], p[1], true
	}
	if c.model == HD63701Y {
		if p, ok := portRegsY[a]; ok {
			return p[0], p[1], true
		}
	}
	return 0, 0, false
}

func (c *CPU) readReg(a uint16) uint8 {
	if port, ddr, ok := c.portOf(a); ok {
		if ddr < 0 {
			return c.regs[a] // port 7 is output only
		}
		d := c.regs[ddr]
		in := uint8(0xff)
		if c.ports != nil {
			in = c.ports.PortRead(port)
		}
		return c.regs[a]&d | in&^d
	}
	switch a {
	case 0x00, 0x01, 0x04, 0x05, 0x16, 0x20:
		return c.regs[a] // DDRs read back (the 6801's are write-only; harmless)
	case 0x08:
		c.pending = c.tcsr & 0xe0 // flags seen: the next counter/OCR access clears them
		return c.tcsr
	case 0x09:
		if c.pending&timerTOF != 0 {
			c.tcsr &^= timerTOF
		}
		c.tmpFRC = uint8(c.frc)
		return uint8(c.frc >> 8)
	case 0x0a:
		return c.tmpFRC
	case 0x0b:
		return uint8(c.ocr >> 8)
	case 0x0c:
		return uint8(c.ocr)
	case 0x0d:
		if c.pending&timerICF != 0 {
			c.tcsr &^= timerICF
		}
		return uint8(c.icr >> 8)
	case 0x0e:
		return uint8(c.icr)
	case 0x11:
		return c.regs[a] | 0x20 // transmit register empty
	}
	return c.regs[a]
}

func (c *CPU) writeReg(a uint16, v uint8) {
	c.regs[a] = v
	if port, ddr, ok := c.portOf(a); ok {
		d := uint8(0xff)
		if ddr >= 0 {
			d = c.regs[ddr]
		}
		if c.ports != nil {
			c.ports.PortWrite(port, v, d)
		}
		return
	}
	switch a {
	case 0x00, 0x01, 0x04, 0x05, 0x16, 0x20: // a DDR change changes the pins
		if c.ports != nil {
			for _, d := range ddrs {
				if d.ddr == a && (d.port < 5 || c.model == HD63701Y) {
					c.ports.PortWrite(d.port, c.regs[d.data], v)
				}
			}
		}
	case 0x08:
		c.tcsr = c.tcsr&0xe0 | v&0x1f
	case 0x09:
		c.frc = 0xfff8 // a write to the counter presets it
		c.tmpFRC = v
	case 0x0a:
		c.frc = uint16(c.tmpFRC)<<8 | uint16(v)
	case 0x0b:
		if c.pending&timerOCF != 0 {
			c.tcsr &^= timerOCF
		}
		c.ocr = c.ocr&0xff | uint16(v)<<8
	case 0x0c:
		if c.pending&timerOCF != 0 {
			c.tcsr &^= timerOCF
		}
		c.ocr = c.ocr&0xff00 | uint16(v)
	}
}

// ---------------------------------------------------------------- memory

func (c *CPU) rd(a uint16) uint8 {
	if a < c.nregs() {
		return c.readReg(a)
	}
	if a-c.ramBase < uint16(len(c.ram)) {
		return c.ram[a-c.ramBase]
	}
	return c.bus.Read(a)
}

func (c *CPU) wr(a uint16, v uint8) {
	if a < c.nregs() {
		c.writeReg(a, v)
		return
	}
	if a-c.ramBase < uint16(len(c.ram)) {
		c.ram[a-c.ramBase] = v
		return
	}
	c.bus.Write(a, v)
}

func (c *CPU) rd16(a uint16) uint16    { return uint16(c.rd(a))<<8 | uint16(c.rd(a+1)) }
func (c *CPU) wr16(a uint16, v uint16) { c.wr(a, uint8(v>>8)); c.wr(a+1, uint8(v)) }
func (c *CPU) fetch() uint8            { v := c.rd(c.PC); c.PC++; return v }
func (c *CPU) fetch16() uint16         { v := c.rd16(c.PC); c.PC += 2; return v }

func (c *CPU) push8(v uint8) { c.wr(c.S, v); c.S-- }
func (c *CPU) push16(v uint16) {
	c.push8(uint8(v))
	c.push8(uint8(v >> 8))
}
func (c *CPU) pull8() uint8 { c.S++; return c.rd(c.S) }
func (c *CPU) pull16() uint16 {
	hi := c.pull8()
	return uint16(hi)<<8 | uint16(c.pull8())
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

func (c *CPU) ld8(v uint8) uint8    { c.nz8(v); c.CC &^= FlagV; return v }
func (c *CPU) ld16(v uint16) uint16 { c.nz16(v); c.CC &^= FlagV; return v }

func (c *CPU) add8(a, b, cy uint8) uint8 {
	r := uint16(a) + uint16(b) + uint16(cy)
	c.CC &^= FlagH | FlagV | FlagC
	c.CC |= uint8((uint16(a)^uint16(b)^r)&0x10) << 1
	c.CC |= uint8(r>>8) & FlagC
	c.CC |= uint8((uint16(a)^r)&(uint16(b)^r)&0x80) >> 6
	c.nz8(uint8(r))
	return uint8(r)
}

func (c *CPU) sub8(a, b, cy uint8) uint8 {
	r := uint16(a) - uint16(b) - uint16(cy)
	c.CC &^= FlagV | FlagC
	c.CC |= uint8(r>>8) & FlagC
	c.CC |= uint8((uint16(a)^uint16(b))&(uint16(a)^r)&0x80) >> 6
	c.nz8(uint8(r))
	return uint8(r)
}

func (c *CPU) add16(a, b uint16) uint16 {
	r := uint32(a) + uint32(b)
	c.CC &^= FlagV | FlagC
	c.CC |= uint8(r>>16) & FlagC
	c.CC |= uint8(((uint32(a)^r)&(uint32(b)^r)&0x8000)>>14) & FlagV
	c.nz16(uint16(r))
	return uint16(r)
}

func (c *CPU) sub16(a, b uint16) uint16 {
	r := uint32(a) - uint32(b)
	c.CC &^= FlagV | FlagC
	c.CC |= uint8(r>>16) & FlagC
	c.CC |= uint8(((uint32(a)^uint32(b))&(uint32(a)^r)&0x8000)>>14) & FlagV
	c.nz16(uint16(r))
	return uint16(r)
}

// shiftV: the 6800's shifts and rotates set V to N xor C.
func (c *CPU) shiftV() {
	n, cy := c.CC&FlagN != 0, c.CC&FlagC != 0
	c.flag(FlagV, n != cy)
}

// rmw is the read-modify-write group by the opcode's low nibble.
func (c *CPU) rmw(k int, v uint8) uint8 {
	switch k {
	case 0x0: // NEG
		r := c.sub8(0, v, 0)
		return r
	case 0x3: // COM
		v = ^v
		c.ld8(v)
		c.CC |= FlagC
		return v
	case 0x4: // LSR
		c.flag(FlagC, v&1 != 0)
		v >>= 1
		c.nz8(v)
		c.shiftV()
		return v
	case 0x6: // ROR
		r := v>>1 | (c.CC&FlagC)<<7
		c.flag(FlagC, v&1 != 0)
		c.nz8(r)
		c.shiftV()
		return r
	case 0x7: // ASR
		c.flag(FlagC, v&1 != 0)
		v = v&0x80 | v>>1
		c.nz8(v)
		c.shiftV()
		return v
	case 0x8: // ASL
		r := v << 1
		c.flag(FlagC, v&0x80 != 0)
		c.nz8(r)
		c.shiftV()
		return r
	case 0x9: // ROL
		r := v<<1 | c.CC&FlagC
		c.flag(FlagC, v&0x80 != 0)
		c.nz8(r)
		c.shiftV()
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
		c.ld8(v)
		c.CC &^= FlagC
		return v
	case 0xf: // CLR
		c.CC &^= FlagN | FlagV | FlagC
		c.CC |= FlagZ
		return 0
	}
	return v
}

// alu is the accumulator group by the opcode's low nibble.
func (c *CPU) alu(k int, r, m uint8) uint8 {
	switch k {
	case 0x0:
		return c.sub8(r, m, 0)
	case 0x1:
		c.sub8(r, m, 0)
	case 0x2:
		return c.sub8(r, m, c.CC&FlagC)
	case 0x4:
		return c.ld8(r & m)
	case 0x5:
		c.ld8(r & m)
	case 0x6:
		return c.ld8(m)
	case 0x8:
		return c.ld8(r ^ m)
	case 0x9:
		return c.add8(r, m, c.CC&FlagC)
	case 0xa:
		return c.ld8(r | m)
	case 0xb:
		return c.add8(r, m, 0)
	}
	return r
}

// ---------------------------------------------------------------- interrupts

func (c *CPU) pushAll() {
	c.push16(c.PC)
	c.push16(c.X)
	c.push8(c.A)
	c.push8(c.B)
	c.push8(c.CC)
}

func (c *CPU) vector(v uint16) {
	c.CC |= FlagI
	c.PC = c.rd16(v)
}

// interrupt takes an interrupt (the state is already stacked after WAI).
func (c *CPU) interrupt(v uint16) int {
	n := 12
	if c.wait && c.stacked {
		n = 4
	} else {
		c.pushAll()
	}
	c.wait, c.stacked = false, false
	c.vector(v)
	return n
}

// timer advances the free-running counter by n cycles.
func (c *CPU) timer(n int) {
	before := c.frc
	c.frc += uint16(n)
	// compare match somewhere in (before, frc]
	if uint16(c.ocr-before-1) < uint16(n) {
		c.tcsr |= timerOCF
	}
	if c.frc < before {
		c.tcsr |= timerTOF
	}
}

// Step runs one instruction, or takes an interrupt, or waits in WAI / SLP.
// It returns the cycles used.
func (c *CPU) Step() int {
	n := c.step()
	c.timer(n)
	return n
}

func (c *CPU) step() int {
	if c.nmiPending {
		c.nmiPending = false
		return c.interrupt(VecNMI)
	}
	if c.CC&FlagI == 0 {
		switch {
		case c.IRQ:
			return c.interrupt(VecIRQ)
		case c.tcsr&timerICF != 0 && c.tcsr&timerEICI != 0:
			return c.interrupt(VecICF)
		case c.tcsr&timerOCF != 0 && c.tcsr&timerEOCI != 0:
			return c.interrupt(VecOCF)
		case c.tcsr&timerTOF != 0 && c.tcsr&timerETOI != 0:
			return c.interrupt(VecTOF)
		}
	}
	if c.wait {
		return 1
	}
	op := c.fetch()
	return c.exec(op)
}

// ---------------------------------------------------------------- instructions

func (c *CPU) cond(op uint8) bool {
	cc := c.CC
	n, z, v, cy := cc&FlagN != 0, cc&FlagZ != 0, cc&FlagV != 0, cc&FlagC != 0
	var r bool
	switch op >> 1 & 7 {
	case 0:
		r = true
	case 1:
		r = !cy && !z
	case 2:
		r = !cy
	case 3:
		r = !z
	case 4:
		r = !v
	case 5:
		r = !n
	case 6:
		r = n == v
	case 7:
		r = n == v && !z
	}
	if op&1 != 0 {
		r = !r
	}
	return r
}

// ea is a memory operand's address: mode 1 direct, 2 indexed, 3 extended.
func (c *CPU) ea(mode int) uint16 {
	switch mode {
	case 1:
		return uint16(c.fetch())
	case 2:
		return c.X + uint16(c.fetch())
	}
	return c.fetch16()
}

func (c *CPU) illegal() int {
	if c.hitachi() {
		c.PC--
		c.pushAll()
		c.vector(VecTrap)
		return 12
	}
	return 2
}

func (c *CPU) exec(op uint8) int {
	n := cycles[c.hitachi01()][op]
	k := int(op & 0x0f)
	switch {
	case op >= 0x20 && op < 0x30:
		off := uint16(int16(int8(c.fetch())))
		if c.cond(op) {
			c.PC += off
		}
		return n
	case op >= 0x40 && op < 0x60:
		r := &c.A
		if op >= 0x50 {
			r = &c.B
		}
		switch k {
		case 0x1, 0x2, 0x5, 0xb, 0xe:
			return c.illegal()
		}
		*r = c.rmw(k, *r)
		return n
	case op >= 0x60 && op < 0x80:
		mode := 2
		if op >= 0x70 {
			mode = 3
		}
		switch k {
		case 0x1, 0x2, 0x5, 0xb: // AIM OIM EIM TIM (Hitachi): 6x indexed, 7x direct
			if !c.hitachi() {
				return c.illegal()
			}
			m := c.fetch()
			if mode == 3 {
				mode = 1
			}
			ea := c.ea(mode)
			v := c.rd(ea)
			switch k {
			case 0x1:
				c.wr(ea, c.ld8(v&m))
			case 0x2:
				c.wr(ea, c.ld8(v|m))
			case 0x5:
				c.wr(ea, c.ld8(v^m))
			default:
				c.ld8(v & m)
			}
			return n
		}
		ea := c.ea(mode)
		switch k {
		case 0xe:
			c.PC = ea
		case 0xd:
			c.rmw(k, c.rd(ea))
		case 0xf:
			c.wr(ea, c.rmw(k, 0))
		default:
			c.wr(ea, c.rmw(k, c.rd(ea)))
		}
		return n
	case op >= 0x80:
		return n + c.execAcc(op)
	}
	switch op {
	case 0x01: // NOP
	case 0x04: // LSRD
		d := c.D()
		c.flag(FlagC, d&1 != 0)
		d >>= 1
		c.SetD(d)
		c.nz16(d)
		c.shiftV()
	case 0x05: // ASLD
		d := c.D()
		c.flag(FlagC, d&0x8000 != 0)
		d <<= 1
		c.SetD(d)
		c.nz16(d)
		c.shiftV()
	case 0x06: // TAP
		c.CC = c.A | 0xc0
	case 0x07: // TPA
		c.A = c.CC | 0xc0
	case 0x08: // INX
		c.X++
		c.flag(FlagZ, c.X == 0)
	case 0x09: // DEX
		c.X--
		c.flag(FlagZ, c.X == 0)
	case 0x0a:
		c.CC &^= FlagV
	case 0x0b:
		c.CC |= FlagV
	case 0x0c:
		c.CC &^= FlagC
	case 0x0d:
		c.CC |= FlagC
	case 0x0e:
		c.CC &^= FlagI
	case 0x0f:
		c.CC |= FlagI
	case 0x10: // SBA
		c.A = c.sub8(c.A, c.B, 0)
	case 0x11: // CBA
		c.sub8(c.A, c.B, 0)
	case 0x16: // TAB
		c.B = c.ld8(c.A)
	case 0x17: // TBA
		c.A = c.ld8(c.B)
	case 0x18: // XGDX
		if !c.hitachi() {
			return c.illegal()
		}
		d := c.D()
		c.SetD(c.X)
		c.X = d
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
		if r > 0xff {
			c.CC |= FlagC
		}
		c.nz8(c.A)
	case 0x1a: // SLP
		if !c.hitachi() {
			return c.illegal()
		}
		c.wait, c.stacked = true, false
	case 0x1b: // ABA
		c.A = c.add8(c.A, c.B, 0)
	case 0x30: // TSX
		c.X = c.S + 1
	case 0x31: // INS
		c.S++
	case 0x32:
		c.A = c.pull8()
	case 0x33:
		c.B = c.pull8()
	case 0x34: // DES
		c.S--
	case 0x35: // TXS
		c.S = c.X - 1
	case 0x36:
		c.push8(c.A)
	case 0x37:
		c.push8(c.B)
	case 0x38: // PULX
		c.X = c.pull16()
	case 0x39: // RTS
		c.PC = c.pull16()
	case 0x3a: // ABX
		c.X += uint16(c.B)
	case 0x3b: // RTI
		c.CC = c.pull8() | 0xc0
		c.B = c.pull8()
		c.A = c.pull8()
		c.X = c.pull16()
		c.PC = c.pull16()
	case 0x3c: // PSHX
		c.push16(c.X)
	case 0x3d: // MUL
		c.SetD(uint16(c.A) * uint16(c.B))
		c.flag(FlagC, c.B&0x80 != 0)
	case 0x3e: // WAI
		c.pushAll()
		c.wait, c.stacked = true, true
	case 0x3f: // SWI
		c.pushAll()
		c.vector(VecSWI)
	default:
		return c.illegal()
	}
	return n
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
	operand16 := func() uint16 {
		if mode == 0 {
			return c.fetch16()
		}
		return c.rd16(c.ea(mode))
	}
	switch k {
	case 0x7: // STA / STB
		if mode == 0 {
			return c.illegal()
		}
		c.wr(c.ea(mode), c.ld8(*r))
	case 0x3: // SUBD / ADDD
		if isB {
			c.SetD(c.add16(c.D(), operand16()))
		} else {
			c.SetD(c.sub16(c.D(), operand16()))
		}
	case 0xc: // CPX / LDD
		if isB {
			c.SetD(c.ld16(operand16()))
		} else {
			c.sub16(c.X, operand16())
		}
	case 0xd: // BSR, JSR / STD
		if isB {
			if mode == 0 {
				return c.illegal()
			}
			c.wr16(c.ea(mode), c.ld16(c.D()))
			return 0
		}
		var ea uint16
		if mode == 0 {
			off := uint16(int16(int8(c.fetch())))
			ea = c.PC + off
		} else {
			ea = c.ea(mode)
		}
		c.push16(c.PC)
		c.PC = ea
	case 0xe: // LDS / LDX
		v := c.ld16(operand16())
		if isB {
			c.X = v
		} else {
			c.S = v
		}
	case 0xf: // STS / STX
		if mode == 0 {
			return c.illegal()
		}
		v := c.S
		if isB {
			v = c.X
		}
		c.wr16(c.ea(mode), c.ld16(v))
	default:
		m := uint8(0)
		if mode == 0 {
			m = c.fetch()
		} else {
			m = c.rd(c.ea(mode))
		}
		*r = c.alu(k, *r, m)
	}
	return 0
}

// ---------------------------------------------------------------- cycle tables

func (c *CPU) hitachi01() int {
	if c.hitachi() {
		return 1
	}
	return 0
}

// cycles[0] is the MC6801's timing, cycles[1] the HD6301's.
var cycles [2][256]int

func init() {
	set := func(t *[256]int, n int, ops ...int) {
		for _, o := range ops {
			t[o] = n
		}
	}
	fill := func(t *[256]int, lo, hi, n int) {
		for i := lo; i <= hi; i++ {
			t[i] = n
		}
	}
	m := &cycles[0]
	fill(m, 0, 255, 2)
	set(m, 3, 0x04, 0x05, 0x08, 0x09, 0x30, 0x31, 0x34, 0x35, 0x36, 0x37, 0x3a)
	set(m, 4, 0x32, 0x33, 0x3c)
	set(m, 5, 0x38, 0x39)
	set(m, 10, 0x3b, 0x3d)
	set(m, 9, 0x3e)
	set(m, 12, 0x3f)
	fill(m, 0x20, 0x2f, 3)
	fill(m, 0x60, 0x7f, 6)
	set(m, 3, 0x6e, 0x7e)
	for _, b := range []int{0x80, 0xc0} {
		fill(m, b+0x10, b+0x1f, 3)
		fill(m, b+0x20, b+0x3f, 4)
		set(m, 4, b+0x03, b+0x0c)
		set(m, 5, b+0x13, b+0x1c)
		set(m, 6, b+0x23, b+0x2c, b+0x33, b+0x3c)
		set(m, 3, b+0x0e)
		set(m, 4, b+0x1e, b+0x1f)
		set(m, 5, b+0x2e, b+0x2f, b+0x3e, b+0x3f)
	}
	set(m, 3, 0xcc)
	set(m, 4, 0xdc, 0xdd)
	set(m, 5, 0xec, 0xed, 0xfc, 0xfd)
	set(m, 6, 0x8d)
	set(m, 5, 0x9d)
	set(m, 6, 0xad, 0xbd)

	h := &cycles[1]
	fill(h, 0, 255, 1)
	set(h, 2, 0x18, 0x19)
	set(h, 4, 0x1a)
	set(h, 3, 0x32, 0x33)
	set(h, 4, 0x36, 0x37, 0x38)
	set(h, 5, 0x39, 0x3c)
	set(h, 10, 0x3b)
	set(h, 7, 0x3d)
	set(h, 9, 0x3e)
	set(h, 12, 0x3f)
	fill(h, 0x20, 0x2f, 3)
	fill(h, 0x60, 0x7f, 6)
	set(h, 4, 0x6d, 0x7d)
	set(h, 5, 0x6f, 0x7f)
	set(h, 3, 0x6e, 0x7e)
	set(h, 7, 0x61, 0x62, 0x65)
	set(h, 5, 0x6b)
	set(h, 6, 0x71, 0x72, 0x75)
	set(h, 4, 0x7b)
	for _, b := range []int{0x80, 0xc0} {
		fill(h, b+0x00, b+0x0f, 2)
		fill(h, b+0x10, b+0x1f, 3)
		fill(h, b+0x20, b+0x3f, 4)
		set(h, 3, b+0x03, b+0x0c, b+0x0e)
		set(h, 4, b+0x13, b+0x1c, b+0x1e, b+0x1f)
		set(h, 5, b+0x23, b+0x2c, b+0x2e, b+0x2f, b+0x33, b+0x3c, b+0x3e, b+0x3f)
	}
	set(h, 3, 0xcc)
	set(h, 4, 0xdc, 0xdd)
	set(h, 5, 0xec, 0xed, 0xfc, 0xfd)
	set(h, 5, 0x8d, 0x9d, 0xad)
	set(h, 6, 0xbd)
}

// ---------------------------------------------------------------- state

var errState = errors.New("m6801: damaged save state")

// AppendState appends the processor's state (registers, on-chip registers,
// timer and RAM) to b.
func (c *CPU) AppendState(b []byte) []byte {
	b = append(b, 1, c.A, c.B, c.CC)
	for _, v := range []uint16{c.X, c.S, c.PC, c.frc, c.ocr, c.icr} {
		b = append(b, uint8(v>>8), uint8(v))
	}
	var fl uint8
	for i, v := range []bool{c.IRQ, c.nmiPending, c.nmiLine, c.wait, c.stacked} {
		if v {
			fl |= 1 << i
		}
	}
	b = append(b, fl, c.tcsr, c.pending, c.tmpFRC)
	b = append(b, c.regs[:]...)
	return append(b, c.ram...)
}

// StateSize is the length of a saved state for this processor.
func (c *CPU) StateSize() int { return 4 + 12 + 4 + len(c.regs) + len(c.ram) }

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	if len(b) < c.StateSize() || b[0] != 1 {
		return b, errState
	}
	c.A, c.B, c.CC = b[1], b[2], b[3]
	w := func(i int) uint16 { return uint16(b[4+i*2])<<8 | uint16(b[5+i*2]) }
	c.X, c.S, c.PC, c.frc, c.ocr, c.icr = w(0), w(1), w(2), w(3), w(4), w(5)
	fl := b[16]
	c.IRQ, c.nmiPending, c.nmiLine, c.wait, c.stacked = fl&1 != 0, fl&2 != 0, fl&4 != 0, fl&8 != 0, fl&16 != 0
	c.tcsr, c.pending, c.tmpFRC = b[17], b[18], b[19]
	copy(c.regs[:], b[20:])
	copy(c.ram, b[20+len(c.regs):])
	return b[c.StateSize():], nil
}
