// Package m68k is RetroPixels' Motorola 68000 processor, a Go port of
// web/emu/m68k.js (the web version's 68000, used by the Sega Genesis and,
// later, arcade boards). It behaves exactly like the JS core: the same
// instructions, flags, exceptions and cycle counts (every bus access costs 4
// cycles and each instruction adds its internal cycles).
//
//	cpu := m68k.New(bus)
//	cpu.Reset()          // reads the stack pointer and PC from vectors 0 and 1
//	n := cpu.Step()      // one instruction (or an interrupt); returns cycles
//	cpu.Run(until)       // runs until Cycles >= until
//	cpu.SetIRQ(level)    // the interrupt level being requested (0 = none, 7 = NMI)
//
// The machine around the processor is a Bus. A bus that also implements Acker
// is asked for the vector when an interrupt is taken (autovectors otherwise),
// and one that implements Resetter is told when the RESET instruction runs.
package m68k

import (
	"encoding/binary"
	"errors"
	"sync"
)

// Bus is the memory the processor sees. Addresses are 24 bits (word accesses
// are always at even addresses).
type Bus interface {
	Read8(addr uint32) uint8
	Read16(addr uint32) uint16
	Write8(addr uint32, v uint8)
	Write16(addr uint32, v uint16)
}

// Acker is implemented by buses that answer the interrupt acknowledge cycle:
// Ack returns the vector number, or a negative number for the autovector
// (24 + level).
type Acker interface {
	Ack(level int) int
}

// Resetter is implemented by buses that want to see the RESET instruction.
type Resetter interface {
	Reset()
}

var mask = [5]uint32{0, 0xff, 0xffff, 0, 0xffffffff}
var bitsOf = [5]uint32{0, 8, 16, 0, 32}

func sx8(v uint32) uint32  { return uint32(int32(int8(v))) }
func sx16(v uint32) uint32 { return uint32(int32(int16(v))) }
func clip(v uint32, sz int) uint32 {
	return v & mask[sz]
}
func msb(v uint32, sz int) uint32 { return (v >> (bitsOf[sz] - 1)) & 1 }
func b2u(b bool) uint32 {
	if b {
		return 1
	}
	return 0
}
func popcount16(v uint32) uint32 {
	n := uint32(0)
	for v != 0 {
		n += v & 1
		v >>= 1
	}
	return n
}

// CPU is one 68000.
type CPU struct {
	bus   Bus
	acker Acker
	reset Resetter

	D        [8]uint32
	A        [8]uint32
	PC       uint32
	USP, SSP uint32

	s, t, ipl     uint32
	x, n, z, v, c uint32

	// Cycles counts processor cycles; the machine compares it with its own clock.
	Cycles  int64
	Stopped bool
	irq     int  // interrupt level being requested
	nmiSeen bool // level 7 is edge-triggered
	// TASWrites: whether TAS writes its result back (the Genesis bus ignores the write).
	TASWrites bool
	op        uint32 // opcode being executed
	opPC      uint32 // where it started

	table *[65536]opFunc
}

type opFunc func(c *CPU)

var (
	tableOnce sync.Once
	theTable  *[65536]opFunc
)

// New makes a processor on bus. Call Reset to start it.
func New(bus Bus) *CPU {
	tableOnce.Do(func() { theTable = buildTable() })
	c := &CPU{bus: bus, table: theTable}
	c.acker, _ = bus.(Acker)
	c.reset, _ = bus.(Resetter)
	c.s, c.ipl = 1, 7
	c.TASWrites = true
	return c
}

// ---- status register ----

func (c *CPU) getCCR() uint32 { return c.x<<4 | c.n<<3 | c.z<<2 | c.v<<1 | c.c }
func (c *CPU) setCCR(v uint32) {
	c.x, c.n, c.z, c.v, c.c = (v>>4)&1, (v>>3)&1, (v>>2)&1, (v>>1)&1, v&1
}

// SR is the status register.
func (c *CPU) SR() uint32 { return c.t<<15 | c.s<<13 | c.ipl<<8 | c.getCCR() }

// SetSR sets the status register (switching stacks as the S bit says).
func (c *CPU) SetSR(v uint32) {
	s := (v >> 13) & 1
	if s != c.s {
		if c.s != 0 {
			c.SSP = c.A[7]
			c.A[7] = c.USP
		} else {
			c.USP = c.A[7]
			c.A[7] = c.SSP
		}
		c.s = s
	}
	c.t = (v >> 15) & 1
	c.ipl = (v >> 8) & 7
	c.setCCR(v & 0x1f)
}

// ---- bus (each access 4 cycles) ----

func (c *CPU) r8(a uint32) uint32  { c.Cycles += 4; return uint32(c.bus.Read8(a & 0xffffff)) }
func (c *CPU) r16(a uint32) uint32 { c.Cycles += 4; return uint32(c.bus.Read16(a & 0xfffffe)) }
func (c *CPU) r32(a uint32) uint32 {
	hi := c.r16(a)
	return hi<<16 | c.r16(a+2)
}
func (c *CPU) w8(a, v uint32)  { c.Cycles += 4; c.bus.Write8(a&0xffffff, uint8(v)) }
func (c *CPU) w16(a, v uint32) { c.Cycles += 4; c.bus.Write16(a&0xfffffe, uint16(v)) }
func (c *CPU) w32(a, v uint32) { c.w16(a, v>>16); c.w16(a+2, v&0xffff) }
func (c *CPU) rm(a uint32, sz int) uint32 {
	switch sz {
	case 1:
		return c.r8(a)
	case 2:
		return c.r16(a)
	}
	return c.r32(a)
}
func (c *CPU) wm(a uint32, sz int, v uint32) {
	switch sz {
	case 1:
		c.w8(a, v)
	case 2:
		c.w16(a, v)
	default:
		c.w32(a, v)
	}
}
func (c *CPU) fetch() uint32 { v := c.r16(c.PC); c.PC += 2; return v }
func (c *CPU) fetch32() uint32 {
	hi := c.fetch()
	return hi<<16 | c.fetch()
}
func (c *CPU) push16(v uint32) { c.A[7] -= 2; c.w16(c.A[7], v) }
func (c *CPU) push32(v uint32) { c.A[7] -= 4; c.w32(c.A[7], v) }
func (c *CPU) pop16() uint32   { v := c.r16(c.A[7]); c.A[7] += 2; return v }
func (c *CPU) pop32() uint32   { v := c.r32(c.A[7]); c.A[7] += 4; return v }

// ---- effective addresses ----

func (c *CPU) index(base uint32) uint32 {
	ext := c.fetch()
	c.Cycles += 2
	r := (ext >> 12) & 7
	var idx uint32
	if ext&0x8000 != 0 {
		idx = c.A[r]
	} else {
		idx = c.D[r]
	}
	if ext&0x800 == 0 {
		idx = sx16(idx & 0xffff)
	}
	return base + idx + sx8(ext&0xff)
}

func step(sz, reg int) uint32 {
	if sz == 1 && reg == 7 {
		return 2
	}
	return uint32(sz)
}

// addr is the memory address of an operand (performs (An)+ / -(An) and reads
// extension words).
func (c *CPU) addr(mode, reg, sz int) uint32 {
	switch mode {
	case 2:
		return c.A[reg]
	case 3:
		a := c.A[reg]
		c.A[reg] += step(sz, reg)
		return a
	case 4:
		c.Cycles += 2
		c.A[reg] -= step(sz, reg)
		return c.A[reg]
	case 5:
		base := c.A[reg]
		return base + sx16(c.fetch())
	case 6:
		return c.index(c.A[reg])
	case 7:
		switch reg {
		case 0:
			return sx16(c.fetch())
		case 1:
			return c.fetch32()
		case 2:
			base := c.PC
			return base + sx16(c.fetch())
		case 3:
			return c.index(c.PC)
		}
	}
	return 0
}

func (c *CPU) read(mode, reg, sz int) uint32 {
	switch {
	case mode == 0:
		return clip(c.D[reg], sz)
	case mode == 1:
		return clip(c.A[reg], sz)
	case mode == 7 && reg == 4:
		switch sz {
		case 1:
			return c.fetch() & 0xff
		case 2:
			return c.fetch()
		}
		return c.fetch32()
	}
	return c.rm(c.addr(mode, reg, sz), sz)
}

func (c *CPU) setD(r int, v uint32, sz int) {
	switch sz {
	case 4:
		c.D[r] = v
	case 2:
		c.D[r] = c.D[r]&0xffff0000 | v&0xffff
	default:
		c.D[r] = c.D[r]&0xffffff00 | v&0xff
	}
}

// rmwRead / rmwWrite: a read-modify-write of an operand.
func (c *CPU) rmwRead(mode, reg, sz int) (a, v uint32) {
	if mode == 0 {
		return 0, clip(c.D[reg], sz)
	}
	a = c.addr(mode, reg, sz)
	return a, c.rm(a, sz)
}
func (c *CPU) rmwWrite(mode, reg, sz int, a, v uint32) {
	if mode == 0 {
		c.setD(reg, v, sz)
		return
	}
	c.wm(a, sz, v)
}

// ---- flags ----

func (c *CPU) nz(v uint32, sz int) {
	c.n = msb(v, sz)
	c.z = b2u(clip(v, sz) == 0)
}
func (c *CPU) logic(v uint32, sz int) { c.nz(v, sz); c.v = 0; c.c = 0 }
func (c *CPU) add(a, b uint32, sz int, x uint32) uint32 {
	r := uint64(a) + uint64(b) + uint64(x)
	m := clip(uint32(r), sz)
	sa, sb, sr := msb(a, sz), msb(b, sz), msb(m, sz)
	c.c = b2u(r > uint64(mask[sz]))
	c.v = b2u(sa == sb && sr != sa)
	c.n = sr
	return m
}
func (c *CPU) sub(a, b uint32, sz int, x uint32) uint32 { // a - b - x
	r := int64(a) - int64(b) - int64(x)
	m := clip(uint32(r), sz)
	sa, sb, sr := msb(a, sz), msb(b, sz), msb(m, sz)
	c.c = b2u(r < 0)
	c.v = b2u(sa != sb && sr != sa)
	c.n = sr
	return m
}
func (c *CPU) cond(cc uint32) bool {
	switch cc {
	case 0:
		return true
	case 1:
		return false
	case 2:
		return c.c == 0 && c.z == 0
	case 3:
		return c.c != 0 || c.z != 0
	case 4:
		return c.c == 0
	case 5:
		return c.c != 0
	case 6:
		return c.z == 0
	case 7:
		return c.z != 0
	case 8:
		return c.v == 0
	case 9:
		return c.v != 0
	case 10:
		return c.n == 0
	case 11:
		return c.n != 0
	case 12:
		return c.n == c.v
	case 13:
		return c.n != c.v
	case 14:
		return c.z == 0 && c.n == c.v
	}
	return c.z != 0 || c.n != c.v
}

// ---- exceptions ----

func (c *CPU) exception(vec, pc uint32) {
	sr := c.SR()
	if c.s == 0 {
		c.USP = c.A[7]
		c.A[7] = c.SSP
		c.s = 1
	}
	c.t = 0
	c.push32(pc)
	c.push16(sr)
	c.PC = c.r32(vec * 4)
}
func (c *CPU) trap(vec uint32, extra int64) { c.Cycles += extra; c.exception(vec, c.PC) }
func (c *CPU) privilege()                   { c.Cycles += 10; c.exception(8, c.opPC) }
func (c *CPU) illegal(vec uint32)           { c.Cycles += 10; c.exception(vec, c.opPC) }

// SetIRQ sets the interrupt level being requested (0 = none). Level 7 is
// taken once each time it is raised.
func (c *CPU) SetIRQ(level int) {
	c.irq = level
	if level != 7 {
		c.nmiSeen = false
	}
}

// IRQ is the interrupt level being requested.
func (c *CPU) IRQ() int { return c.irq }

// Reset is the processor's reset: supervisor mode, interrupts masked, and the
// stack pointer and PC read from vectors 0 and 1 (which takes bus cycles).
func (c *CPU) Reset() {
	c.s, c.t, c.ipl, c.Stopped = 1, 0, 7, false
	c.A[7] = c.r32(0)
	c.SSP = c.A[7]
	c.PC = c.r32(4)
}

func (c *CPU) checkInterrupt() bool {
	lv := c.irq
	if lv == 0 {
		return false
	}
	if lv == 7 {
		if c.nmiSeen {
			return false
		}
		c.nmiSeen = true
	} else if uint32(lv) <= c.ipl {
		return false
	}
	c.Stopped = false
	sr := c.SR()
	if c.s == 0 {
		c.USP = c.A[7]
		c.A[7] = c.SSP
		c.s = 1
	}
	c.t = 0
	c.ipl = uint32(lv)
	c.Cycles += 24
	vec := -1
	if c.acker != nil {
		vec = c.acker.Ack(lv)
	}
	if vec < 0 {
		vec = 24 + lv
	}
	c.push32(c.PC)
	c.push16(sr)
	c.PC = c.r32(uint32(vec) * 4)
	return true
}

// Step runs one instruction (or takes an interrupt, or idles 4 cycles when
// stopped) and returns the cycles it took.
func (c *CPU) Step() int {
	start := c.Cycles
	if c.checkInterrupt() {
		return int(c.Cycles - start)
	}
	if c.Stopped {
		c.Cycles += 4
		return 4
	}
	c.opPC = c.PC
	op := c.fetch()
	c.op = op
	c.table[op](c)
	return int(c.Cycles - start)
}

// Run runs instructions until Cycles reaches until.
func (c *CPU) Run(until int64) {
	for c.Cycles < until {
		if c.Stopped && !(c.irq != 0 && (uint32(c.irq) > c.ipl || c.irq == 7)) {
			c.Cycles = until
			break
		}
		c.Step()
	}
}

// ---- save states ----

// StateSize is the length of a saved processor state.
const StateSize = 1 + 16*4 + 4*4 + 8 + 2 + 4

var errState = errors.New("m68k: damaged save state")

// AppendState appends the processor's state (StateSize bytes) to b.
func (c *CPU) AppendState(b []byte) []byte {
	b = append(b, 1)
	for _, v := range c.D {
		b = binary.LittleEndian.AppendUint32(b, v)
	}
	for _, v := range c.A {
		b = binary.LittleEndian.AppendUint32(b, v)
	}
	b = binary.LittleEndian.AppendUint32(b, c.PC)
	b = binary.LittleEndian.AppendUint32(b, c.USP)
	b = binary.LittleEndian.AppendUint32(b, c.SSP)
	b = binary.LittleEndian.AppendUint32(b, c.SR())
	b = binary.LittleEndian.AppendUint64(b, uint64(c.Cycles))
	var fl uint8
	if c.Stopped {
		fl |= 1
	}
	if c.nmiSeen {
		fl |= 2
	}
	b = append(b, fl, uint8(c.irq))
	return append(b, 0, 0, 0, 0)
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != 1 || b[1+16*4+4*4+8+1] > 7 {
		return b, errState
	}
	o := 1
	u32 := func() uint32 { v := binary.LittleEndian.Uint32(b[o:]); o += 4; return v }
	for i := range c.D {
		c.D[i] = u32()
	}
	for i := range c.A {
		c.A[i] = u32()
	}
	c.PC, c.USP, c.SSP = u32(), u32(), u32()
	sr := u32()
	c.s, c.t, c.ipl = (sr>>13)&1, (sr>>15)&1, (sr>>8)&7
	c.setCCR(sr & 0x1f)
	c.Cycles = int64(binary.LittleEndian.Uint64(b[o:]))
	o += 8
	fl := b[o]
	c.Stopped, c.nmiSeen = fl&1 != 0, fl&2 != 0
	c.irq = int(b[o+1])
	return b[StateSize:], nil
}
