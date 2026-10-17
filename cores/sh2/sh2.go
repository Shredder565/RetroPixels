// Package sh2 is RetroPixels' Hitachi SH-2 processor (the SH7604 as used in
// arcade boards and the Saturn): the SH-2 instruction set with its delayed
// branches, the interrupt controller with IRL autovectors, and the on-chip
// modules games use: the free-running timer, watchdog interval timer,
// division unit, DMA controller and the cache's data-array RAM.
//
//	cpu := sh2.New(bus)
//	cpu.Map(0x06000000, ram, true) // memory the CPU reads directly (fast)
//	cpu.Reset()                    // PC and SP from vectors 0 and 1
//	cpu.Run(until)                 // runs until Cycles >= until
//	cpu.SetIRL(4)                  // the external interrupt level (0 = none)
//
// Addresses: 0x00000000-0x1fffffff go to the board (cached area); the
// cache-through mirror at 0x20000000 is the same space. 0x40000000 (cache
// purge) and 0x60000000 (cache address array) are accepted and ignored,
// 0xc0000000 is the cache's 4 KB data array used as RAM, and 0xfffffe00-
// 0xffffffff are the on-chip registers. The cache itself is not emulated.
//
// Timing is one cycle per instruction plus the extra cycles the SH-2 manual
// gives for branches, multiplies and the like; memory wait states are not
// counted.
package sh2

import "encoding/binary"

// Bus is the board's memory: addresses are 29 bits (the area bits are
// removed), and 16/32-bit accesses are aligned.
type Bus interface {
	Read8(a uint32) uint8
	Read16(a uint32) uint16
	Read32(a uint32) uint32
	Write8(a uint32, v uint8)
	Write16(a uint32, v uint16)
	Write32(a uint32, v uint32)
}

// Vectorer is implemented by buses that supply the vector of an external
// interrupt when the interrupt controller is in external-vector mode (ICR
// VECMD set); otherwise the autovector 64 + level/2 is used.
type Vectorer interface {
	IRQVector(level int) int
}

// Acker is implemented by buses that want to know when an external
// interrupt is taken (the acknowledge cycle).
type Acker interface {
	IRQAck(level int)
}

// SR bits.
const (
	srT = 0x001
	srS = 0x002
	srI = 0x0f0
	srQ = 0x100
	srM = 0x200
)

const srMask = 0x3f3

// page is one 64 KB page of directly mapped memory.
type page struct {
	mem []byte // nil: ask the bus
	rw  bool
}

// CPU is one SH-2.
type CPU struct {
	R    [16]uint32
	PC   uint32
	SR   uint32
	GBR  uint32
	VBR  uint32
	MACH uint32
	MACL uint32
	PR   uint32

	// Cycles counts the cycles run; Run stops once it reaches its target.
	Cycles int64

	bus   Bus
	vec   Vectorer
	acker Acker
	pages [0x2000]page

	// DelaySlotPC selects what PC-relative instructions in a delay slot
	// use as "PC": false = the slot's own address + 4 (as every other
	// instruction), true = the branch target + 2.
	DelaySlotPC bool

	irl       int    // external interrupt level
	sleeping  bool   // SLEEP: waiting for an interrupt
	irqCheck  bool   // something may need an interrupt taken
	slotPC    uint32 // during a delay slot: what PC-relative reads use
	inSlot    bool
	slotFault bool // the slot's instruction raised an exception

	on onchip

	// Trace, when set, is called before each instruction (for debugging).
	Trace func(pc uint32, op uint16)
}

// New makes an SH-2 on bus.
func New(bus Bus) *CPU {
	c := &CPU{bus: bus}
	c.vec, _ = bus.(Vectorer)
	c.acker, _ = bus.(Acker)
	c.on.c = c
	c.on.reset()
	return c
}

// Map makes the CPU read (and, if rw, write) mem directly at addresses
// start..start+len(mem)-1, without asking the bus. start and len(mem) must be
// multiples of 64 KB.
func (c *CPU) Map(start uint32, mem []byte, rw bool) {
	start &= 0x1fffffff
	if len(mem)%0x10000 != 0 || start%0x10000 != 0 {
		panic("sh2: Map needs whole 64 KB pages")
	}
	for o := 0; o+0x10000 <= len(mem); o += 0x10000 {
		c.pages[(start+uint32(o))>>16] = page{mem: mem[o : o+0x10000 : o+0x10000], rw: rw}
	}
}

// Unmap returns a range of pages to the bus.
func (c *CPU) Unmap(start, size uint32) {
	start &= 0x1fffffff
	for o := uint32(0); o < size; o += 0x10000 {
		c.pages[(start+o)>>16] = page{}
	}
}

// Reset is a power-on reset: the on-chip modules are reset and PC and R15
// come from vectors 0 and 1.
func (c *CPU) Reset() {
	c.on.reset()
	c.SR = srI
	c.VBR = 0
	c.sleeping = false
	c.inSlot = false
	c.PC = c.read32(0)
	c.R[15] = c.read32(4)
	c.irqCheck = true
}

// SetIRL sets the level (0-15) the board's interrupt lines request.
func (c *CPU) SetIRL(level int) {
	if level != c.irl {
		c.irl = level
		c.irqCheck = true
	}
}

// IRL is the level being requested.
func (c *CPU) IRL() int { return c.irl }

// Sleeping reports whether the CPU is stopped by SLEEP.
func (c *CPU) Sleeping() bool { return c.sleeping }

// ---------------------------------------------------------------- memory

func (c *CPU) read8(a uint32) uint32 {
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			return uint32(p.mem[a&0xffff])
		}
		return uint32(c.bus.Read8(a & 0x1fffffff))
	}
	return c.readHigh(a, 1)
}

func (c *CPU) read16(a uint32) uint32 {
	a &^= 1
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			return uint32(binary.BigEndian.Uint16(p.mem[a&0xffff:]))
		}
		return uint32(c.bus.Read16(a & 0x1fffffff))
	}
	return c.readHigh(a, 2)
}

func (c *CPU) read32(a uint32) uint32 {
	a &^= 3
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			return binary.BigEndian.Uint32(p.mem[a&0xffff:])
		}
		return c.bus.Read32(a & 0x1fffffff)
	}
	return c.readHigh(a, 4)
}

func (c *CPU) write8(a, v uint32) {
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			if p.rw {
				p.mem[a&0xffff] = uint8(v)
			}
			return
		}
		c.bus.Write8(a&0x1fffffff, uint8(v))
		return
	}
	c.writeHigh(a, v, 1)
}

func (c *CPU) write16(a, v uint32) {
	a &^= 1
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			if p.rw {
				binary.BigEndian.PutUint16(p.mem[a&0xffff:], uint16(v))
			}
			return
		}
		c.bus.Write16(a&0x1fffffff, uint16(v))
		return
	}
	c.writeHigh(a, v, 2)
}

func (c *CPU) write32(a, v uint32) {
	a &^= 3
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			if p.rw {
				binary.BigEndian.PutUint32(p.mem[a&0xffff:], v)
			}
			return
		}
		c.bus.Write32(a&0x1fffffff, v)
		return
	}
	c.writeHigh(a, v, 4)
}

// readHigh: the areas above the board's space.
func (c *CPU) readHigh(a uint32, size int) uint32 {
	switch a >> 29 {
	case 6: // cache data array as RAM
		o := a & 0xfff
		switch size {
		case 1:
			return uint32(c.on.cacheRAM[o])
		case 2:
			return uint32(binary.BigEndian.Uint16(c.on.cacheRAM[o:]))
		}
		return binary.BigEndian.Uint32(c.on.cacheRAM[o:])
	case 7:
		if a >= 0xfffffe00 {
			return c.on.read(a, size)
		}
	case 4, 5: // these mirror the board's space on the SH7604's bus
		a &= 0x1fffffff
		switch size {
		case 1:
			return uint32(c.bus.Read8(a))
		case 2:
			return uint32(c.bus.Read16(a))
		}
		return c.bus.Read32(a)
	}
	return 0
}

func (c *CPU) writeHigh(a, v uint32, size int) {
	switch a >> 29 {
	case 6:
		o := a & 0xfff
		switch size {
		case 1:
			c.on.cacheRAM[o] = uint8(v)
		case 2:
			binary.BigEndian.PutUint16(c.on.cacheRAM[o:], uint16(v))
		default:
			binary.BigEndian.PutUint32(c.on.cacheRAM[o:], v)
		}
	case 7:
		if a >= 0xfffffe00 {
			c.on.write(a, v, size)
		}
	case 4, 5:
		a &= 0x1fffffff
		switch size {
		case 1:
			c.bus.Write8(a, uint8(v))
		case 2:
			c.bus.Write16(a, uint16(v))
		default:
			c.bus.Write32(a, v)
		}
	}
}

// Read8/16/32 and Write8/16/32 access memory as the CPU sees it (for
// debuggers, tests and DMA).
func (c *CPU) Read8(a uint32) uint8       { return uint8(c.read8(a)) }
func (c *CPU) Read16(a uint32) uint16     { return uint16(c.read16(a)) }
func (c *CPU) Read32(a uint32) uint32     { return c.read32(a) }
func (c *CPU) Write8(a uint32, v uint8)   { c.write8(a, uint32(v)) }
func (c *CPU) Write16(a uint32, v uint16) { c.write16(a, uint32(v)) }
func (c *CPU) Write32(a uint32, v uint32) { c.write32(a, v) }

// fetch reads an instruction.
func (c *CPU) fetch(a uint32) uint16 {
	if a < 0x40000000 {
		p := &c.pages[(a>>16)&0x1fff]
		if p.mem != nil {
			o := a & 0xfffe
			return uint16(p.mem[o])<<8 | uint16(p.mem[o+1])
		}
	}
	return uint16(c.read16(a))
}

// ---------------------------------------------------------------- running

// Run runs until Cycles >= until (or returns at once if it already is).
func (c *CPU) Run(until int64) {
	for c.Cycles < until {
		if c.on.nextEvent <= c.Cycles {
			c.on.update()
		}
		if c.irqCheck {
			c.checkIRQ()
		}
		if c.sleeping {
			// skip to the next timer event or the target
			t := min(until, c.on.nextEvent)
			if t > c.Cycles {
				c.Cycles = t
			} else {
				c.Cycles++
			}
			continue
		}
		stop := min(until, c.on.nextEvent)
		if c.Trace != nil {
			for c.Cycles < stop && !c.irqCheck && !c.sleeping {
				op := c.fetch(c.PC)
				c.Trace(c.PC, op)
				c.PC += 2
				c.Cycles++
				opTable[op](c, op)
			}
			continue
		}
		for c.Cycles < stop && !c.irqCheck && !c.sleeping {
			op := c.fetch(c.PC)
			c.PC += 2
			c.Cycles++
			opTable[op](c, op)
		}
	}
}

// Step runs one instruction (or takes an interrupt) and returns its cycles.
func (c *CPU) Step() int {
	start := c.Cycles
	if c.on.nextEvent <= c.Cycles {
		c.on.update()
	}
	if c.irqCheck {
		c.checkIRQ()
		if c.Cycles != start {
			return int(c.Cycles - start)
		}
	}
	if c.sleeping {
		c.Cycles++
		return 1
	}
	op := c.fetch(c.PC)
	if c.Trace != nil {
		c.Trace(c.PC, op)
	}
	c.PC += 2
	c.Cycles++
	opTable[op](c, op)
	return int(c.Cycles - start)
}

// checkIRQ takes the highest pending interrupt above the SR mask.
func (c *CPU) checkIRQ() {
	c.irqCheck = false
	mask := int(c.SR>>4) & 15
	level, vec := c.on.pending()
	ext := false
	if c.irl > level {
		level, ext = c.irl, true
	}
	if level == 0 || level <= mask {
		return
	}
	if ext {
		if c.on.icr&1 != 0 && c.vec != nil {
			vec = c.vec.IRQVector(c.irl)
		} else {
			vec = 64 + c.irl>>1
		}
		if c.acker != nil {
			c.acker.IRQAck(c.irl)
		}
	} else {
		c.on.ack(vec)
	}
	c.sleeping = false
	c.exception(uint32(vec), c.PC)
	if level > 15 {
		level = 15
	}
	c.SR = c.SR&^srI | uint32(level)<<4
	c.Cycles += 13
	c.irqCheck = true
}

// NMI takes a non-maskable interrupt.
func (c *CPU) NMI() {
	c.sleeping = false
	c.exception(11, c.PC)
	c.SR |= srI
	c.Cycles += 13
}

// exception pushes SR and pc and jumps through vector vec.
func (c *CPU) exception(vec, pc uint32) {
	c.R[15] -= 4
	c.write32(c.R[15], c.SR)
	c.R[15] -= 4
	c.write32(c.R[15], pc)
	c.PC = c.read32(c.VBR + vec*4)
}

// srChanged: the interrupt mask may have dropped.
func (c *CPU) srChanged() { c.irqCheck = true }

// delay runs the instruction in a branch's delay slot, then jumps to target.
func (c *CPU) delay(target uint32) {
	slot := c.PC
	op := c.fetch(slot)
	if isBranch[op] {
		// a branch in a delay slot: slot illegal instruction
		c.exception(6, slot-2)
		return
	}
	if c.Trace != nil {
		c.Trace(slot, op)
	}
	c.inSlot = true
	if c.DelaySlotPC {
		c.slotPC = target + 2
	} else {
		c.slotPC = slot + 2
	}
	c.PC = slot + 2
	c.Cycles++
	opTable[op](c, op)
	c.inSlot = false
	if c.slotFault {
		c.slotFault = false
		return
	}
	c.PC = target
}

// pcRel is the PC that PC-relative instructions use (their address + 4).
func (c *CPU) pcRel() uint32 {
	if c.inSlot {
		return c.slotPC
	}
	return c.PC + 2
}
