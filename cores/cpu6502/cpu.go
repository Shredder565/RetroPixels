// Package cpu6502 is the 6502 processor shared by the native NES and Atari
// cores. It is a Go port of web/emu/cpu6502.js and behaves the same way, cycle
// for cycle.
//
// Every memory access goes through Bus.Read / Bus.Write and costs one cycle,
// including the "dummy" accesses real hardware makes, so devices that count
// cycles (video chips, timers) see exactly the same timing as on the console.
package cpu6502

import "errors"

// Status flag bits.
const (
	FlagC = 0x01
	FlagZ = 0x02
	FlagI = 0x04
	FlagD = 0x08
	FlagB = 0x10
	FlagU = 0x20
	FlagV = 0x40
	FlagN = 0x80
)

// Bus is the machine's memory map; each call is one CPU cycle.
type Bus interface {
	Read(addr uint16) uint8
	Write(addr uint16, v uint8)
}

// Options select chip variants.
type Options struct {
	// Decimal: whether the D flag enables decimal arithmetic (off on the NES).
	Decimal bool
	// MachinePolls: the machine samples the interrupt lines itself by calling
	// Poll at the right point within each cycle (the NES does); otherwise the
	// CPU samples them at the start of every cycle.
	MachinePolls bool
}

// CPU is one 6502.
type CPU struct {
	A, X, Y, S, P uint8
	PC            uint16
	Cycles        int64 // total cycles run

	NMIPending bool // edge-triggered, set by the machine
	IRQLine    bool // level-triggered, set by the machine
	Jammed     bool

	decimal, machinePolls bool
	bus                   Bus

	prevIrq, prevNmi bool // interrupt polling happens before the last cycle
	pollNmi, pollIrq bool
	branchPolled     bool
	branchNmi        bool
	branchIrq        bool
}

// New makes a CPU on bus. Call Reset before the first Step.
func New(bus Bus, opts Options) *CPU {
	return &CPU{bus: bus, decimal: opts.Decimal, machinePolls: opts.MachinePolls, S: 0xfd, P: FlagI | FlagU}
}

// SetBus replaces the bus (for machines that are rebuilt around a CPU).
func (c *CPU) SetBus(b Bus) { c.bus = b }

// Poll notes the interrupt lines as they are now. Before each cycle (or when
// the machine calls it, with MachinePolls), the state seen at the end of the
// previous cycle is what decides whether an interrupt is taken next.
func (c *CPU) Poll() {
	c.pollNmi = c.NMIPending
	c.pollIrq = c.IRQLine && c.P&FlagI == 0
}

func (c *CPU) rd(a uint16) uint8 {
	if !c.machinePolls {
		c.Poll()
	}
	c.Cycles++
	return c.bus.Read(a)
}

func (c *CPU) wr(a uint16, v uint8) {
	if !c.machinePolls {
		c.Poll()
	}
	c.Cycles++
	c.bus.Write(a, v)
}

func (c *CPU) push(v uint8) { c.wr(0x100|uint16(c.S), v); c.S-- }
func (c *CPU) pull() uint8  { c.S++; return c.rd(0x100 | uint16(c.S)) }
func (c *CPU) fetch() uint8 { v := c.rd(c.PC); c.PC++; return v }
func (c *CPU) fetch16() uint16 {
	lo := uint16(c.fetch())
	return lo | uint16(c.fetch())<<8
}

func (c *CPU) setZN(v uint8) uint8 {
	c.P &^= FlagZ | FlagN
	if v == 0 {
		c.P |= FlagZ
	}
	c.P |= v & FlagN
	return v
}

// Reset runs the reset sequence (7 cycles) and loads PC from $FFFC.
func (c *CPU) Reset() {
	c.rd(c.PC)
	c.rd(c.PC)
	c.rd(0x100 | uint16(c.S))
	c.S--
	c.rd(0x100 | uint16(c.S))
	c.S--
	c.rd(0x100 | uint16(c.S))
	c.S--
	c.P |= FlagI
	lo := uint16(c.rd(0xfffc))
	c.PC = lo | uint16(c.rd(0xfffd))<<8
	c.Jammed = false
}

func (c *CPU) interrupt(vector uint16, brk bool) {
	c.push(uint8(c.PC >> 8))
	c.push(uint8(c.PC))
	if brk {
		c.push(c.P | FlagU | FlagB)
	} else {
		c.push((c.P | FlagU) &^ FlagB)
	}
	c.P |= FlagI
	// An NMI arriving during a BRK/IRQ takes over its vector.
	// (With machine polling, only an NMI already sampled by this point counts.)
	nmi := c.NMIPending
	if c.machinePolls {
		nmi = c.pollNmi
	}
	if vector == 0xfffe && nmi {
		vector = 0xfffa
		c.NMIPending = false
	}
	lo := uint16(c.rd(vector))
	c.PC = lo | uint16(c.rd(vector+1))<<8
}

// Step runs one instruction (or services an interrupt) and returns the cycles used.
func (c *CPU) Step() int {
	start := c.Cycles
	if c.Jammed {
		c.rd(c.PC)
		return int(c.Cycles - start)
	}
	if c.prevNmi || c.prevIrq {
		c.rd(c.PC)
		c.rd(c.PC)
		if c.prevNmi {
			c.NMIPending = false
			c.interrupt(0xfffa, false)
		} else {
			c.interrupt(0xfffe, false)
		}
		// The handler's first instruction always runs before another interrupt.
		c.prevNmi, c.prevIrq = false, false
		return int(c.Cycles - start)
	}
	c.branchPolled = false
	op := c.fetch()
	c.exec(op)
	if c.branchPolled {
		c.pollNmi, c.pollIrq = c.branchNmi, c.branchIrq
	}
	if op == 0x00 { // like any interrupt, BRK's handler runs one instruction first
		c.prevNmi, c.prevIrq = false, false
	} else {
		c.prevNmi, c.prevIrq = c.pollNmi, c.pollIrq
	}
	return int(c.Cycles - start)
}

// ---- addressing (return effective address; cycle-exact dummy reads) ----

func (c *CPU) zp() uint16 { return uint16(c.fetch()) }
func (c *CPU) zpx() uint16 {
	a := c.fetch()
	c.rd(uint16(a))
	return uint16(a + c.X)
}
func (c *CPU) zpy() uint16 {
	a := c.fetch()
	c.rd(uint16(a))
	return uint16(a + c.Y)
}
func (c *CPU) abs() uint16 { return c.fetch16() }

// absIdx: with read, the extra cycle is only taken when a page is crossed.
func (c *CPU) absIdx(idx uint8, read bool) uint16 {
	base := c.fetch16()
	ea := base + uint16(idx)
	if !read || (base^ea)&0xff00 != 0 {
		c.rd(base&0xff00 | ea&0xff)
	}
	return ea
}
func (c *CPU) izx() uint16 {
	z := c.fetch()
	c.rd(uint16(z))
	p := z + c.X
	lo := uint16(c.rd(uint16(p)))
	return lo | uint16(c.rd(uint16(p+1)))<<8
}
func (c *CPU) izy(read bool) uint16 {
	z := c.fetch()
	lo := uint16(c.rd(uint16(z)))
	base := lo | uint16(c.rd(uint16(z+1)))<<8
	ea := base + uint16(c.Y)
	if !read || (base^ea)&0xff00 != 0 {
		c.rd(base&0xff00 | ea&0xff)
	}
	return ea
}

// ---- arithmetic ----

func (c *CPU) adc(v uint8) {
	if c.decimal && c.P&FlagD != 0 {
		a, vv, cy := int(c.A), int(v), int(c.P&FlagC)
		lo := a&0x0f + vv&0x0f + cy
		hi := a&0xf0 + vv&0xf0
		if lo > 9 {
			lo += 6
			hi += 0x10
		}
		zTest := (a + vv + cy) & 0xff
		c.P &^= FlagZ | FlagN | FlagV | FlagC
		if zTest == 0 {
			c.P |= FlagZ
		}
		if hi&0x80 != 0 {
			c.P |= FlagN
		}
		if ^(a^vv)&(a^hi)&0x80 != 0 {
			c.P |= FlagV
		}
		if hi > 0x90 {
			hi += 0x60
		}
		if hi > 0xff {
			c.P |= FlagC
		}
		c.A = uint8(hi&0xf0 | lo&0x0f)
		return
	}
	c.adcBinary(v)
}

func (c *CPU) sbc(v uint8) {
	if c.decimal && c.P&FlagD != 0 {
		a, vv := int(c.A), int(v)
		cy := 1
		if c.P&FlagC != 0 {
			cy = 0
		}
		bin := a - vv - cy
		lo := a&0x0f - vv&0x0f - cy
		hi := a&0xf0 - vv&0xf0
		if lo&0x10 != 0 {
			lo -= 6
			hi -= 0x10
		}
		if hi&0x100 != 0 {
			hi -= 0x60
		}
		c.P &^= FlagC | FlagV | FlagZ | FlagN
		if bin >= 0 {
			c.P |= FlagC
		}
		if (a^vv)&(a^bin)&0x80 != 0 {
			c.P |= FlagV
		}
		if bin&0xff == 0 {
			c.P |= FlagZ
		}
		if bin&0x80 != 0 {
			c.P |= FlagN
		}
		c.A = uint8(hi&0xf0 | lo&0x0f)
		return
	}
	c.adcBinary(v ^ 0xff)
}

func (c *CPU) adcBinary(v uint8) {
	r := uint16(c.A) + uint16(v) + uint16(c.P&FlagC)
	c.P &^= FlagC | FlagV
	if r > 0xff {
		c.P |= FlagC
	}
	if ^(c.A^v)&(c.A^uint8(r))&0x80 != 0 {
		c.P |= FlagV
	}
	c.A = c.setZN(uint8(r))
}

func (c *CPU) cmp(r, v uint8) {
	c.P &^= FlagC
	if r >= v {
		c.P |= FlagC
	}
	c.setZN(r - v)
}
func (c *CPU) asl(v uint8) uint8 { c.P = c.P&^FlagC | v>>7; return c.setZN(v << 1) }
func (c *CPU) lsr(v uint8) uint8 { c.P = c.P&^FlagC | v&1; return c.setZN(v >> 1) }
func (c *CPU) rol(v uint8) uint8 {
	cy := c.P & FlagC
	c.P = c.P&^FlagC | v>>7
	return c.setZN(v<<1 | cy)
}
func (c *CPU) ror(v uint8) uint8 {
	cy := c.P & FlagC
	c.P = c.P&^FlagC | v&1
	return c.setZN(v>>1 | cy<<7)
}

func (c *CPU) branch(cond bool) {
	off := c.fetch()
	if !cond {
		return
	}
	earlyNmi, earlyIrq := c.pollNmi, c.pollIrq // captured just before the operand cycle
	// A taken branch delays interrupt polling unless it crosses a page.
	c.rd(c.PC)
	target := c.PC + uint16(int8(off))
	// A taken branch that stays on the same page doesn't look at interrupts
	// on its last cycle, so the earlier check counts.
	if (target^c.PC)&0xff00 != 0 {
		c.rd(c.PC&0xff00 | target&0xff)
	} else {
		c.branchPolled, c.branchNmi, c.branchIrq = true, earlyNmi, earlyIrq
	}
	c.PC = target
}

// Read-modify-write: read, dummy write of the old value, write the new value.
func (c *CPU) rmw(ea uint16, fn func(*CPU, uint8) uint8) {
	v := c.rd(ea)
	c.wr(ea, v)
	c.wr(ea, fn(c, v))
}

// readMode returns the operand for the read addressing mode m.
func (c *CPU) readMode(m mode) uint8 {
	switch m {
	case mImm:
		return c.fetch()
	case mZp:
		return c.rd(c.zp())
	case mZpx:
		return c.rd(c.zpx())
	case mZpy:
		return c.rd(c.zpy())
	case mAbs:
		return c.rd(c.abs())
	case mAbx:
		return c.rd(c.absIdx(c.X, true))
	case mAby:
		return c.rd(c.absIdx(c.Y, true))
	case mIzx:
		return c.rd(c.izx())
	default: // mIzy
		return c.rd(c.izy(true))
	}
}

// addrMode returns the effective address for writes and read-modify-writes.
func (c *CPU) addrMode(m mode) uint16 {
	switch m {
	case mZp:
		return c.zp()
	case mZpx:
		return c.zpx()
	case mZpy:
		return c.zpy()
	case mAbs:
		return c.abs()
	case mAbx:
		return c.absIdx(c.X, false)
	case mAby:
		return c.absIdx(c.Y, false)
	case mIzx:
		return c.izx()
	default: // mIzy
		return c.izy(false)
	}
}

// shStore is SHY/SHX/SHA abs,X/Y: the unstable stores with the high-byte AND quirk.
func (c *CPU) shStore(idx, reg uint8) {
	base := c.fetch16()
	ea := base + uint16(idx)
	c.rd(base&0xff00 | ea&0xff)
	val := reg & uint8(base>>8+1)
	addr := ea
	if (base^ea)&0xff00 != 0 {
		addr = uint16(val)<<8 | ea&0xff
	}
	c.wr(addr, val)
}

// State is the CPU's saved state (StateSize bytes).
const StateSize = 17

// SaveState returns the CPU's state as StateSize bytes.
func (c *CPU) SaveState() []byte {
	b := make([]byte, StateSize)
	b[0] = 1 // version
	b[1], b[2], b[3], b[4], b[5] = c.A, c.X, c.Y, c.S, c.P
	b[6], b[7] = uint8(c.PC), uint8(c.PC>>8)
	cy := uint64(c.Cycles)
	for i := 0; i < 8; i++ {
		b[8+i] = uint8(cy >> (8 * i))
	}
	var f uint8
	for i, v := range []bool{c.NMIPending, c.IRQLine, c.Jammed, c.prevIrq, c.prevNmi, c.pollNmi, c.pollIrq} {
		if v {
			f |= 1 << i
		}
	}
	b[16] = f
	return b
}

// LoadState restores a state made by SaveState.
func (c *CPU) LoadState(b []byte) error {
	if len(b) != StateSize || b[0] != 1 {
		return errors.New("cpu6502: bad saved state")
	}
	c.A, c.X, c.Y, c.S, c.P = b[1], b[2], b[3], b[4], b[5]
	c.PC = uint16(b[6]) | uint16(b[7])<<8
	var cy uint64
	for i := 0; i < 8; i++ {
		cy |= uint64(b[8+i]) << (8 * i)
	}
	c.Cycles = int64(cy)
	f := b[16]
	c.NMIPending, c.IRQLine, c.Jammed = f&1 != 0, f&2 != 0, f&4 != 0
	c.prevIrq, c.prevNmi, c.pollNmi, c.pollIrq = f&8 != 0, f&16 != 0, f&32 != 0, f&64 != 0
	return nil
}
