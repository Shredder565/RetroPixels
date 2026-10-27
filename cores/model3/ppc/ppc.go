// Package ppc is the PowerPC 603e processor of the Sega Model 3: RetroPixels'
// own interpreter, written for the Model 3 board (cores/model3).
//
// It runs the 32-bit PowerPC instruction set the way the 603e does with its
// memory management off (the Model 3 games never turn it on): integer,
// branch, condition register, load/store and floating point instructions,
// the supervisor instructions the games' start-up code uses, the external
// and decrementer interrupts, and the time base.
//
// Timing is one cycle per instruction. The time base and the decrementer
// count once every TimerRatio cycles (4 bus clocks on the 603e).
//
// Instructions are decoded once and kept per address (RAM and the fixed
// program ROM), so running code is a table lookup and a call. A store to RAM
// forgets the decoded instruction at that address.
package ppc

import (
	"encoding/binary"
	"fmt"
	"math"
)

// Bus is the machine the processor is in: everything that isn't RAM or the
// program ROM. Addresses are physical. Multi-byte values are big endian.
type Bus interface {
	Read8(a uint32) uint8
	Read16(a uint32) uint16
	Read32(a uint32) uint32
	Write8(a uint32, v uint8)
	Write16(a uint32, v uint16)
	Write32(a uint32, v uint32)
}

// MSR bits
const (
	MsrPOW = 0x00040000
	MsrILE = 0x00010000
	MsrEE  = 0x00008000
	MsrPR  = 0x00004000
	MsrFP  = 0x00002000
	MsrME  = 0x00001000
	MsrFE0 = 0x00000800
	MsrSE  = 0x00000400
	MsrBE  = 0x00000200
	MsrFE1 = 0x00000100
	MsrIP  = 0x00000040
	MsrIR  = 0x00000020
	MsrDR  = 0x00000010
	MsrRI  = 0x00000002
	MsrLE  = 0x00000001
)

const (
	xerSO = 0x80000000
	xerOV = 0x40000000
	xerCA = 0x20000000
)

// Processor version numbers
const (
	PVR603E  = 0x00060103
	PVR603R  = 0x00071202 // the 603e "R" (Goldeneye) of the Step 1.0 and 2.x boards
	PVR603EV = 0x00070000
)

// SPR numbers
const (
	sprXER   = 1
	sprLR    = 8
	sprCTR   = 9
	sprDSISR = 18
	sprDAR   = 19
	sprDEC   = 22
	sprSDR1  = 25
	sprSRR0  = 26
	sprSRR1  = 27
	sprSPRG0 = 272
	sprEAR   = 282
	sprTBLw  = 284
	sprTBUw  = 285
	sprPVR   = 287
	sprTBLr  = 268
	sprTBUr  = 269
	sprHID0  = 1008
	sprHID1  = 1009
)

type entry struct {
	f  func(c *CPU, op uint32)
	op uint32
}

// CPU is one PowerPC 603e.
type CPU struct {
	GPR [32]uint32
	FPR [32]float64
	CR  uint32
	LR  uint32
	CTR uint32
	XER uint32
	PC  uint32 // the next instruction (while an instruction runs: the one after it)
	MSR uint32

	FPSCR      uint32
	SRR0, SRR1 uint32
	SPRG       [4]uint32
	HID0, HID1 uint32
	SR         [16]uint32
	PVR        uint32
	spr        map[uint32]uint32 // the other SPRs (BATs, DAR, ...): read back what was written

	// Time base and decrementer
	TimerRatio int    // cycles per time base tick
	tb         uint64 // time base at the start of the current Run
	dec        uint32 // decrementer at the start of the current Run
	frac       int    // cycles towards the next tick, carried between Runs

	// Interrupts
	irqLine    bool // the external interrupt input
	decPending bool

	// Run state
	icount    int // cycles left in this Run
	runLen    int // cycles asked for in this Run (+frac)
	decTrig   int // icount at which the decrementer passes zero (-1 = not in this Run)
	Cycles    int64
	Halted    error // set when the processor stopped on something it can't run
	stopEarly bool

	// Memory the processor reaches directly
	RAM     []byte // at 0, big endian
	ROM     []byte // the fixed program ROM, mapped so it ends at 0xFFFFFFFF
	romBase uint32
	ramOps  []entry
	romOps  []entry

	Bus Bus

	// Trace, when set, is called before each instruction (for debugging).
	Trace func(c *CPU, pc, op uint32)
}

// New makes a processor with ram at address 0 and rom ending at 0xFFFFFFFF.
func New(bus Bus, ram, rom []byte, pvr uint32, timerRatio int) *CPU {
	c := &CPU{Bus: bus, RAM: ram, ROM: rom, PVR: pvr, TimerRatio: timerRatio}
	c.romBase = uint32(0x100000000 - uint64(len(rom)))
	c.ramOps = make([]entry, len(ram)/4)
	c.romOps = make([]entry, len(rom)/4)
	c.spr = map[uint32]uint32{}
	if c.TimerRatio <= 0 {
		c.TimerRatio = 1
	}
	c.Reset()
	return c
}

// Reset starts the processor at the reset vector (0xFFF00100).
func (c *CPU) Reset() {
	c.GPR = [32]uint32{}
	c.FPR = [32]float64{}
	c.CR, c.LR, c.CTR, c.XER = 0, 0, 0, 0
	c.MSR = MsrIP
	c.PC = 0xFFF00100
	c.HID0 = 1
	c.HID1 = 0
	c.FPSCR = 0
	c.tb = 0
	c.dec = 0xFFFFFFFF
	c.frac = 0
	c.irqLine = false
	c.decPending = false
	c.Halted = nil
	c.Cycles = 0
	for k := range c.spr {
		delete(c.spr, k)
	}
	c.FlushCode()
}

// FlushCode forgets every decoded instruction.
func (c *CPU) FlushCode() {
	clear(c.ramOps)
	clear(c.romOps)
}

// InvalidateRAM forgets the decoded instructions in RAM[a:a+n] (call it when
// something other than the processor writes RAM).
func (c *CPU) InvalidateRAM(a, n uint32) {
	if a >= uint32(len(c.RAM)) {
		return
	}
	end := uint64(a) + uint64(n)
	if end > uint64(len(c.RAM)) {
		end = uint64(len(c.RAM))
	}
	for i := a >> 2; uint64(i)<<2 < end; i++ {
		c.ramOps[i].f = nil
	}
}

// SetIRQ sets the external interrupt input. Raising it takes the interrupt
// at once if MSR[EE] allows.
func (c *CPU) SetIRQ(on bool) {
	c.irqLine = on
	if on {
		c.checkInterrupts()
	}
}

// IRQ is the state of the external interrupt input.
func (c *CPU) IRQ() bool { return c.irqLine }

// ---------------------------------------------------------------- timers

// elapsed: cycles run so far in the current Run (including the carried frac)
func (c *CPU) elapsed() int { return c.runLen - c.icount }

// TB is the time base now.
func (c *CPU) TB() uint64 { return c.tb + uint64(c.elapsed()/c.TimerRatio) }

// DEC is the decrementer now.
func (c *CPU) DEC() uint32 { return c.dec - uint32(c.elapsed()/c.TimerRatio) }

func (c *CPU) setTB(v uint64) {
	e := c.elapsed()
	c.tb = v - uint64(e/c.TimerRatio)
}

func (c *CPU) setDEC(v uint32) {
	e := c.elapsed()
	c.dec = v + uint32(e/c.TimerRatio)
	c.armDec()
}

// armDec works out at which icount the decrementer passes through zero in
// this Run.
func (c *CPU) armDec() {
	c.decTrig = -1
	if c.runLen == 0 {
		return
	}
	// the decrementer is c.dec - t/ratio at elapsed t; it passes zero when
	// t/ratio == dec+1
	ticks := uint64(c.dec) + 1
	t := ticks * uint64(c.TimerRatio)
	if t <= uint64(c.elapsed()) || t > uint64(c.runLen) {
		return
	}
	c.decTrig = c.runLen - int(t)
}

// ---------------------------------------------------------------- run

// Run runs about n cycles (instructions) and returns how many ran.
func (c *CPU) Run(n int) int {
	if c.Halted != nil || n <= 0 {
		return 0
	}
	c.runLen = n + c.frac
	c.icount = c.runLen
	c.icount -= c.frac // the carried cycles count as already run
	c.armDec()
	c.stopEarly = false
	c.checkInterrupts()
	for c.icount > 0 {
		pc := c.PC
		var e *entry
		if pc < uint32(len(c.RAM)) {
			e = &c.ramOps[pc>>2]
			if e.f == nil {
				e.op = binary.BigEndian.Uint32(c.RAM[pc&^3:])
				e.f = decode(e.op)
			}
		} else if pc >= c.romBase {
			e = &c.romOps[(pc-c.romBase)>>2]
			if e.f == nil {
				e.op = binary.BigEndian.Uint32(c.ROM[(pc-c.romBase)&^3:])
				e.f = decode(e.op)
			}
		} else {
			c.halt(pc, 0, "the program jumped outside RAM and ROM")
			break
		}
		if c.Trace != nil {
			c.Trace(c, pc, e.op)
		}
		c.PC = pc + 4
		e.f(c, e.op)
		c.icount--
		if c.icount == c.decTrig {
			c.decPending = true
			c.checkInterrupts()
		}
		if c.stopEarly {
			break
		}
	}
	ran := c.runLen - c.icount
	ticks := ran / c.TimerRatio
	c.tb += uint64(ticks)
	c.dec -= uint32(ticks)
	c.frac = ran % c.TimerRatio
	done := ran - (c.runLen - n) // without the carried frac
	c.Cycles += int64(done)
	c.runLen, c.icount, c.decTrig = 0, 0, -1
	return done
}

// StopRun makes Run return after the current instruction.
func (c *CPU) StopRun() { c.stopEarly = true }

// Halt reason
type HaltError struct {
	PC, Op uint32
	Msg    string
}

func (e *HaltError) Error() string {
	return fmt.Sprintf("PowerPC: %s at %08X (instruction %08X)", e.Msg, e.PC, e.Op)
}

func (c *CPU) halt(pc, op uint32, msg string) {
	if c.Halted == nil {
		c.Halted = &HaltError{pc, op, msg}
	}
	c.icount = 0
}

// ---------------------------------------------------------------- exceptions

const (
	excDSI     = 0x300
	excISI     = 0x400
	excIRQ     = 0x500
	excProgram = 0x700
	excFPU     = 0x800
	excDEC     = 0x900
	excSC      = 0xC00
)

// exception enters the handler at vector; srr0 is where to return to.
func (c *CPU) exception(vector, srr0, srr1extra uint32) {
	c.SRR0 = srr0
	c.SRR1 = c.MSR&0xFF73 | srr1extra
	msr := c.MSR &^ (MsrPOW | MsrEE | MsrPR | MsrFP | MsrFE0 | MsrSE | MsrBE | MsrFE1 | MsrIR | MsrDR | MsrRI)
	if msr&MsrILE != 0 {
		msr |= MsrLE
	} else {
		msr &^= MsrLE
	}
	c.MSR = msr
	if msr&MsrIP != 0 {
		c.PC = 0xFFF00000 | vector
	} else {
		c.PC = vector
	}
}

func (c *CPU) checkInterrupts() {
	if c.MSR&MsrEE == 0 {
		return
	}
	if c.irqLine {
		c.exception(excIRQ, c.PC, 0)
	} else if c.decPending {
		c.decPending = false
		c.exception(excDEC, c.PC, 0)
	}
}

func (c *CPU) setMSR(v uint32) {
	c.MSR = v
	c.checkInterrupts()
}

// ---------------------------------------------------------------- memory

func (c *CPU) read8(a uint32) uint32 {
	if a < uint32(len(c.RAM)) {
		return uint32(c.RAM[a])
	}
	if a >= c.romBase {
		return uint32(c.ROM[a-c.romBase])
	}
	return uint32(c.Bus.Read8(a))
}

func (c *CPU) read16(a uint32) uint32 {
	if a < uint32(len(c.RAM))-1 {
		return uint32(binary.BigEndian.Uint16(c.RAM[a:]))
	}
	if a >= c.romBase && a <= 0xFFFFFFFE {
		return uint32(binary.BigEndian.Uint16(c.ROM[a-c.romBase:]))
	}
	if a&1 != 0 {
		return c.read8(a)<<8 | c.read8(a+1)
	}
	return uint32(c.Bus.Read16(a))
}

func (c *CPU) read32(a uint32) uint32 {
	if a < uint32(len(c.RAM))-3 {
		return binary.BigEndian.Uint32(c.RAM[a:])
	}
	if a >= c.romBase && a <= 0xFFFFFFFC {
		return binary.BigEndian.Uint32(c.ROM[a-c.romBase:])
	}
	if a&3 != 0 {
		return c.read16(a)<<16 | c.read16(a+2)
	}
	return c.Bus.Read32(a)
}

func (c *CPU) read64(a uint32) uint64 {
	return uint64(c.read32(a))<<32 | uint64(c.read32(a+4))
}

func (c *CPU) write8(a uint32, v uint32) {
	if a < uint32(len(c.RAM)) {
		c.RAM[a] = byte(v)
		c.ramOps[a>>2].f = nil
		return
	}
	c.Bus.Write8(a, uint8(v))
}

func (c *CPU) write16(a uint32, v uint32) {
	if a < uint32(len(c.RAM))-1 {
		binary.BigEndian.PutUint16(c.RAM[a:], uint16(v))
		c.ramOps[a>>2].f = nil
		c.ramOps[(a+1)>>2].f = nil
		return
	}
	if a&1 != 0 {
		c.write8(a, v>>8)
		c.write8(a+1, v)
		return
	}
	c.Bus.Write16(a, uint16(v))
}

func (c *CPU) write32(a uint32, v uint32) {
	if a < uint32(len(c.RAM))-3 {
		binary.BigEndian.PutUint32(c.RAM[a:], v)
		c.ramOps[a>>2].f = nil
		c.ramOps[(a+3)>>2].f = nil
		return
	}
	if a&3 != 0 {
		c.write16(a, v>>16)
		c.write16(a+2, v)
		return
	}
	c.Bus.Write32(a, v)
}

func (c *CPU) write64(a uint32, v uint64) {
	c.write32(a, uint32(v>>32))
	c.write32(a+4, uint32(v))
}

// Read32 reads memory as the processor would (for debugging and tests).
func (c *CPU) Read32(a uint32) uint32 { return c.read32(a) }

// ---------------------------------------------------------------- CR helpers

func (c *CPU) crField(n uint32) uint32 { return (c.CR >> (28 - 4*n)) & 0xF }
func (c *CPU) setCRField(n, v uint32) {
	sh := 28 - 4*n
	c.CR = c.CR&^(0xF<<sh) | (v&0xF)<<sh
}
func (c *CPU) crBit(b uint32) uint32 { return (c.CR >> (31 - b)) & 1 }
func (c *CPU) setCRBit(b, v uint32) {
	m := uint32(1) << (31 - b)
	if v != 0 {
		c.CR |= m
	} else {
		c.CR &^= m
	}
}

func (c *CPU) so() uint32 { return c.XER >> 31 }

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
	c.CR = c.CR&0x0FFFFFFF | (f|c.so())<<28
}

// ---------------------------------------------------------------- SPRs

func (c *CPU) mfspr(n uint32, pc, op uint32) uint32 {
	switch n {
	case sprXER:
		return c.XER
	case sprLR:
		return c.LR
	case sprCTR:
		return c.CTR
	case sprDEC:
		return c.DEC()
	case sprSRR0:
		return c.SRR0
	case sprSRR1:
		return c.SRR1
	case sprSPRG0, sprSPRG0 + 1, sprSPRG0 + 2, sprSPRG0 + 3:
		return c.SPRG[n-sprSPRG0]
	case sprPVR:
		return c.PVR
	case sprHID0:
		return c.HID0
	case sprHID1:
		return c.HID1
	case sprTBLr, sprTBLw:
		return uint32(c.TB())
	case sprTBUr, sprTBUw:
		return uint32(c.TB() >> 32)
	}
	return c.spr[n]
}

func (c *CPU) mtspr(n, v uint32) {
	switch n {
	case sprXER:
		c.XER = v & 0xE000007F
	case sprLR:
		c.LR = v
	case sprCTR:
		c.CTR = v
	case sprDEC:
		// the 603e takes a decrementer exception when bit 0 goes from 0 to 1
		c.setDEC(v)
	case sprSRR0:
		c.SRR0 = v
	case sprSRR1:
		c.SRR1 = v
	case sprSPRG0, sprSPRG0 + 1, sprSPRG0 + 2, sprSPRG0 + 3:
		c.SPRG[n-sprSPRG0] = v
	case sprPVR:
	case sprHID0:
		c.HID0 = v
	case sprHID1:
		c.HID1 = v
	case sprTBLw, sprTBLr:
		c.setTB(c.TB()&^0xFFFFFFFF | uint64(v))
	case sprTBUw, sprTBUr:
		c.setTB(c.TB()&0xFFFFFFFF | uint64(v)<<32)
	default:
		c.spr[n] = v
	}
}

// ---------------------------------------------------------------- floats

func f2u(f float64) uint64 { return math.Float64bits(f) }
func u2f(u uint64) float64 { return math.Float64frombits(u) }

// round a double to single precision, keeping it a double
func toSingle(f float64) float64 { return float64(float32(f)) }
