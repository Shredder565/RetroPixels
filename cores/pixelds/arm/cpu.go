// Package arm is the ARM processor of PixelDS, RetroPixels' own DS and 3DS
// emulator: an interpreter for the ARM and Thumb instruction sets, written
// for RetroPixels from ARM's architecture documents. One CPU type covers the
// three cores the two handhelds use:
//
//	V4T   the ARM7TDMI (the DS's sound and I/O processor)
//	V5TE  the ARM946E-S (the DS's main processor), with its coprocessor 15
//	V6K   the ARM11 MPCore (the 3DS's application processor; added in stages)
//
// The architecture decides the few places where the cores behave
// differently: Thumb interworking on loads, BLX, CLZ and the DSP
// instructions, unaligned halfword loads, and the LDM/STM corner cases.
package arm

// Arch is the architecture version of a CPU.
type Arch int

const (
	V4T Arch = iota
	V5TE
	V6K
)

// Bus is a CPU's view of memory. Addresses come in unaligned; the bus aligns
// them. Reads return the value in the low bits.
type Bus interface {
	Read8(a uint32) uint32
	Read16(a uint32) uint32
	Read32(a uint32) uint32
	Write8(a, v uint32)
	Write16(a, v uint32)
	Write32(a, v uint32)
	Fetch16(a uint32) uint32
	Fetch32(a uint32) uint32
}

// Coprocessor is a system control coprocessor (CP15) for MRC and MCR.
type Coprocessor interface {
	MRC(op1, cn, cm, op2 uint32) uint32
	MCR(op1, cn, cm, op2, v uint32)
}

// Processor modes.
const (
	ModeUSR = 0x10
	ModeFIQ = 0x11
	ModeIRQ = 0x12
	ModeSVC = 0x13
	ModeABT = 0x17
	ModeUND = 0x1b
	ModeSYS = 0x1f
)

// CPSR bits.
const (
	FlagN = 1 << 31
	FlagZ = 1 << 30
	FlagC = 1 << 29
	FlagV = 1 << 28
	FlagQ = 1 << 27
	FlagI = 1 << 7
	FlagF = 1 << 6
	FlagT = 1 << 5
)

func bankIndex(mode uint32) int {
	switch mode {
	case ModeFIQ:
		return 1
	case ModeIRQ:
		return 2
	case ModeSVC:
		return 3
	case ModeABT:
		return 4
	case ModeUND:
		return 5
	}
	return 0
}

// CPU is one ARM processor.
type CPU struct {
	R    [16]uint32
	CPSR uint32
	SPSR uint32

	bankSP   [6]uint32
	bankLR   [6]uint32
	bankSPSR [6]uint32
	fiqRegs  [5]uint32 // r8-r12 of FIQ mode while another mode runs
	usrRegs  [5]uint32 // r8-r12 of the other modes while FIQ mode runs

	Bus  Bus
	CP15 Coprocessor // nil on CPUs without one
	Arch Arch

	// SWI, when set, is offered every software interrupt first (a BIOS
	// written in Go); it returns true when it handled the call.
	SWI func(n uint32) bool
	// Undefined, when set, is offered undefined instructions (the ARM11's
	// VFP and media instructions while they're added); true = handled.
	Undefined func(op uint32, thumb bool) bool

	VectorBase uint32 // 0 or 0xFFFF0000
	IRQLine    bool   // the interrupt controller's output
	Halted     bool   // waiting for an interrupt; the machine wakes it

	Cycles int64 // cycles run so far
	// Cost: cycles each instruction takes before its extra cycles (1; the
	// DS's ARM9 uses 2, its cached core running at twice the bus clock)
	Cost int64

	exclusive     bool // an LDREX's reservation (ARM11)
	exclusiveAddr uint32
	next          uint32 // address of the next instruction
}

// New makes a CPU on bus.
func New(bus Bus, arch Arch) *CPU {
	c := &CPU{Bus: bus, Arch: arch, Cost: 1}
	c.Reset()
	return c
}

// Reset puts the CPU in supervisor mode at the reset vector.
func (c *CPU) Reset() {
	c.R = [16]uint32{}
	c.bankSP, c.bankLR, c.bankSPSR = [6]uint32{}, [6]uint32{}, [6]uint32{}
	c.CPSR = ModeSVC | FlagI | FlagF
	c.SPSR = 0
	c.Halted = false
	c.SetPC(c.VectorBase)
}

// Thumb reports whether the CPU runs Thumb code.
func (c *CPU) Thumb() bool { return c.CPSR&FlagT != 0 }

// PC is the address of the next instruction.
func (c *CPU) PC() uint32 { return c.next }

// SetPC jumps to addr in the current instruction set.
func (c *CPU) SetPC(addr uint32) {
	if c.CPSR&FlagT != 0 {
		c.next = addr &^ 1
	} else {
		c.next = addr &^ 3
	}
}

// JumpX jumps to addr, picking Thumb when bit 0 is set (BX).
func (c *CPU) JumpX(addr uint32) {
	if addr&1 != 0 {
		c.CPSR |= FlagT
		c.next = addr &^ 1
	} else {
		c.CPSR &^= FlagT
		c.next = addr &^ 3
	}
}

// SetMode switches the processor mode, banking registers.
func (c *CPU) SetMode(m uint32) {
	old := c.CPSR & 0x1f
	m &= 0x1f
	if old == m {
		return
	}
	ob, nb := bankIndex(old), bankIndex(m)
	if ob != nb {
		c.bankSP[ob], c.bankLR[ob], c.bankSPSR[ob] = c.R[13], c.R[14], c.SPSR
		if old == ModeFIQ {
			copy(c.fiqRegs[:], c.R[8:13])
			copy(c.R[8:13], c.usrRegs[:])
		}
		if m == ModeFIQ {
			copy(c.usrRegs[:], c.R[8:13])
			copy(c.R[8:13], c.fiqRegs[:])
		}
		c.R[13], c.R[14], c.SPSR = c.bankSP[nb], c.bankLR[nb], c.bankSPSR[nb]
	}
	c.CPSR = c.CPSR&^0x1f | m
}

// SetCPSR writes the whole CPSR, switching mode first.
func (c *CPU) SetCPSR(v uint32) {
	c.SetMode(v & 0x1f)
	c.CPSR = v
}

func (c *CPU) hasSPSR() bool {
	m := c.CPSR & 0x1f
	return m != ModeUSR && m != ModeSYS
}

// user-bank register access (LDM/STM with the S bit)
func (c *CPU) userReg(i int) uint32 {
	m := c.CPSR & 0x1f
	switch {
	case i >= 8 && i <= 12 && m == ModeFIQ:
		return c.usrRegs[i-8]
	case i >= 13 && i <= 14 && bankIndex(m) != 0:
		if i == 13 {
			return c.bankSP[0]
		}
		return c.bankLR[0]
	}
	return c.R[i]
}

func (c *CPU) setUserReg(i int, v uint32) {
	m := c.CPSR & 0x1f
	switch {
	case i >= 8 && i <= 12 && m == ModeFIQ:
		c.usrRegs[i-8] = v
	case i >= 13 && i <= 14 && bankIndex(m) != 0:
		if i == 13 {
			c.bankSP[0] = v
		} else {
			c.bankLR[0] = v
		}
	default:
		c.R[i] = v
	}
}

// Exception vectors.
const (
	VecReset     = 0x00
	VecUndefined = 0x04
	VecSWI       = 0x08
	VecPrefetch  = 0x0c
	VecData      = 0x10
	VecIRQ       = 0x18
	VecFIQ       = 0x1c
)

func (c *CPU) exception(vec, mode, lr uint32) {
	old := c.CPSR
	c.SetMode(mode)
	c.SPSR = old
	c.R[14] = lr
	c.CPSR = c.CPSR&^FlagT | FlagI
	if mode == ModeFIQ {
		c.CPSR |= FlagF
	}
	c.next = c.VectorBase + vec
	c.Cycles += 3
}

func (c *CPU) undefined(op uint32) {
	if c.Undefined != nil && c.Undefined(op, c.CPSR&FlagT != 0) {
		return
	}
	c.exception(VecUndefined, ModeUND, c.next)
}

func (c *CPU) swi(n uint32) {
	if c.SWI != nil && c.SWI(n) {
		c.Cycles += 3
		return
	}
	c.exception(VecSWI, ModeSVC, c.next)
}

// condPass[cond<<4|nzcv]
var condPass = func() (t [256]bool) {
	for cond := 0; cond < 16; cond++ {
		for f := 0; f < 16; f++ {
			n, z, cc, v := f&8 != 0, f&4 != 0, f&2 != 0, f&1 != 0
			var p bool
			switch cond {
			case 0:
				p = z
			case 1:
				p = !z
			case 2:
				p = cc
			case 3:
				p = !cc
			case 4:
				p = n
			case 5:
				p = !n
			case 6:
				p = v
			case 7:
				p = !v
			case 8:
				p = cc && !z
			case 9:
				p = !cc || z
			case 10:
				p = n == v
			case 11:
				p = n != v
			case 12:
				p = !z && n == v
			case 13:
				p = z || n != v
			case 14:
				p = true
			case 15:
				p = true // the unconditional space; decoded separately
			}
			t[cond<<4|f] = p
		}
	}
	return
}()

// Run runs instructions until Cycles reaches until, the CPU halts, or Stop
// is called.
func (c *CPU) Run(until int64) {
	for c.Cycles < until {
		if c.Halted {
			c.Cycles = until
			return
		}
		if c.IRQLine && c.CPSR&FlagI == 0 {
			c.exception(VecIRQ, ModeIRQ, c.next+4)
		}
		c.Step()
	}
}

// Step runs one instruction.
func (c *CPU) Step() {
	pc := c.next
	if c.CPSR&FlagT != 0 {
		op := c.Bus.Fetch16(pc)
		c.next = pc + 2
		c.R[15] = pc + 4
		c.Cycles += c.Cost
		thumbTable[op>>6](c, op)
		return
	}
	op := c.Bus.Fetch32(pc)
	c.next = pc + 4
	c.R[15] = pc + 8
	c.Cycles += c.Cost
	cond := op >> 28
	if cond != 14 {
		if cond == 15 {
			c.armUncond(op)
			return
		}
		if !condPass[cond<<4|c.CPSR>>28] {
			return
		}
	}
	armTable[(op>>16)&0xff0|(op>>4)&0xf](c, op)
}

// Stop makes Run return after the current instruction.
func (c *CPU) Stop() { c.Cycles = 1<<62 - 1 }

// State is everything a save state needs.
type State struct {
	R                        [16]uint32
	CPSR, SPSR               uint32
	BankSP, BankLR, BankSPSR [6]uint32
	FIQRegs, UsrRegs         [5]uint32
	Next                     uint32
	Halted, IRQLine          bool
	Cycles                   int64
	VectorBase               uint32
}

func (c *CPU) SaveState() State {
	return State{c.R, c.CPSR, c.SPSR, c.bankSP, c.bankLR, c.bankSPSR, c.fiqRegs, c.usrRegs, c.next, c.Halted, c.IRQLine, c.Cycles, c.VectorBase}
}

func (c *CPU) LoadState(s State) {
	c.R, c.CPSR, c.SPSR = s.R, s.CPSR, s.SPSR
	c.bankSP, c.bankLR, c.bankSPSR, c.fiqRegs, c.usrRegs = s.BankSP, s.BankLR, s.BankSPSR, s.FIQRegs, s.UsrRegs
	c.next, c.Halted, c.IRQLine, c.Cycles, c.VectorBase = s.Next, s.Halted, s.IRQLine, s.Cycles, s.VectorBase
}

// EnterSWI takes the software interrupt exception but continues at addr (a
// BIOS routine) instead of the vector; the routine returns with MOVS PC, LR.
func (c *CPU) EnterSWI(addr uint32) {
	c.exception(VecSWI, ModeSVC, c.next)
	c.next = addr
}
