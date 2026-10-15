// Package arm7 is an ARM7TDMI processor (ARM and Thumb instruction sets): a Go
// port of web/emu/arm7.js, which RetroPixels uses for the Atari 2600's DPC+
// cartridges (and, in the web app, the Game Boy Advance and the SNES ST018).
// It behaves exactly like the JS core, instruction for instruction.
package arm7

// Bus is the processor's memory. Addresses are passed through unaligned; the
// bus aligns. Reads return the value in the low bits (8, 16 or 32).
type Bus interface {
	Read8(a uint32) uint32
	Read16(a uint32) uint32
	Read32(a uint32) uint32
	Write8(a, v uint32)
	Write16(a, v uint32)
	Write32(a, v uint32)
	Fetch16(a uint32) uint32 // instruction fetch
	Fetch32(a uint32) uint32
}

// Brancher is optionally implemented by a Bus that wants to know about
// pipeline refills after a jump (timing only).
type Brancher interface{ Branched(addr uint32, thumb bool) }

// SWIHandler is optionally implemented by a Bus that handles software
// interrupts itself (a high-level BIOS); it returns true if it did.
type SWIHandler interface{ SWI(n uint32) bool }

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

var bankOf = func() (b [32]int) {
	b[ModeFIQ], b[ModeIRQ], b[ModeSVC], b[ModeABT], b[ModeUND] = 1, 2, 3, 4, 5
	return
}()

func ror(v, n uint32) uint32 {
	n &= 31
	if n == 0 {
		return v
	}
	return v>>n | v<<(32-n)
}

// mulCycles: internal cycles of a multiply by rs
func mulCycles(rs uint32, signed bool) int64 {
	if signed {
		s := int32(rs)
		switch {
		case s>>8 == 0 || s>>8 == -1:
			return 1
		case s>>16 == 0 || s>>16 == -1:
			return 2
		case s>>24 == 0 || s>>24 == -1:
			return 3
		}
		return 4
	}
	switch {
	case rs < 0x100:
		return 1
	case rs < 0x10000:
		return 2
	case rs < 0x1000000:
		return 3
	}
	return 4
}

// CPU is one ARM7TDMI.
type CPU struct {
	bus      Bus
	brancher Brancher
	swiH     SWIHandler

	R                        [16]uint32
	bank13, bank14, spsrBank [6]uint32
	fiqR, usrR               [5]uint32
	N, Z, C, V               uint32 // flags, 0 or 1
	I, F, T                  uint32 // IRQ / FIQ disable, Thumb state
	Mode                     uint32
	PC                       uint32 // address of the next instruction to run
	Cycles                   int64
	Halted                   bool
	IRQLine                  bool
	sc                       uint32 // shifter carry out
	pipe0, pipe1             uint32 // fetched instructions waiting to run
}

// New makes a processor on bus (call Reset before running it).
func New(bus Bus) *CPU {
	c := &CPU{bus: bus, I: 1, F: 1, Mode: ModeSVC}
	c.brancher, _ = bus.(Brancher)
	c.swiH, _ = bus.(SWIHandler)
	return c
}

// ---- status registers ----

// CPSR is the current program status register.
func (c *CPU) CPSR() uint32 {
	return c.N<<31 | c.Z<<30 | c.C<<29 | c.V<<28 | c.I<<7 | c.F<<6 | c.T<<5 | c.Mode
}

// SetCPSR sets the status register (switching register banks for the mode).
func (c *CPU) SetCPSR(v uint32) {
	c.N, c.Z, c.C, c.V = v>>31&1, v>>30&1, v>>29&1, v>>28&1
	c.I, c.F, c.T = v>>7&1, v>>6&1, v>>5&1
	c.switchMode(v & 0x1f)
}

func (c *CPU) spsr() uint32 {
	if b := bankOf[c.Mode&31]; b != 0 {
		return c.spsrBank[b]
	}
	return c.CPSR()
}

func (c *CPU) setSPSR(v uint32) {
	if b := bankOf[c.Mode&31]; b != 0 {
		c.spsrBank[b] = v
	}
}

func (c *CPU) switchMode(m uint32) {
	old := c.Mode
	if old == m {
		return
	}
	r := &c.R
	ob, nb := bankOf[old&31], bankOf[m&31]
	c.bank13[ob], c.bank14[ob] = r[13], r[14]
	if old == ModeFIQ {
		for k := 0; k < 5; k++ {
			c.fiqR[k] = r[8+k]
			r[8+k] = c.usrR[k]
		}
	}
	if m == ModeFIQ {
		for k := 0; k < 5; k++ {
			c.usrR[k] = r[8+k]
			r[8+k] = c.fiqR[k]
		}
	}
	r[13], r[14] = c.bank13[nb], c.bank14[nb]
	c.Mode = m
}

// registers of user mode (for LDM/STM with the S bit)
func (c *CPU) getUser(k int) uint32 {
	if k >= 8 && k <= 12 && c.Mode == ModeFIQ {
		return c.usrR[k-8]
	}
	if (k == 13 || k == 14) && bankOf[c.Mode&31] != 0 {
		if k == 13 {
			return c.bank13[0]
		}
		return c.bank14[0]
	}
	return c.R[k]
}

func (c *CPU) setUser(k int, v uint32) {
	if k >= 8 && k <= 12 && c.Mode == ModeFIQ {
		c.usrR[k-8] = v
		return
	}
	if (k == 13 || k == 14) && bankOf[c.Mode&31] != 0 {
		if k == 13 {
			c.bank13[0] = v
		} else {
			c.bank14[0] = v
		}
		return
	}
	c.R[k] = v
}

// ---- flow ----

// Branch jumps to addr in the current state (ARM or Thumb) and refills the pipeline.
func (c *CPU) Branch(addr uint32) {
	if c.T != 0 {
		addr &^= 1
	}
	c.PC = addr
	if c.T != 0 {
		c.R[15] = addr + 4
		c.pipe0 = c.bus.Fetch16(c.PC &^ 1)
		c.pipe1 = c.bus.Fetch16((c.PC + 2) &^ 1)
	} else {
		c.R[15] = addr + 8
		c.pipe0 = c.bus.Fetch32(c.PC &^ 3)
		c.pipe1 = c.bus.Fetch32((c.PC + 4) &^ 3)
	}
	if c.brancher != nil {
		c.brancher.Branched(c.PC, c.T != 0)
	}
}

func (c *CPU) exception(vector, mode, lr uint32) {
	cpsr := c.CPSR()
	c.switchMode(mode)
	c.setSPSR(cpsr)
	c.R[14] = lr
	c.T = 0
	c.I = 1
	if mode == ModeFIQ {
		c.F = 1
	}
	c.Branch(vector)
}

func (c *CPU) undefinedInstr() { c.exception(0x04, ModeUND, c.PC) }

func (c *CPU) swi(n uint32) {
	if c.swiH != nil && c.swiH.SWI(n) {
		return
	}
	c.exception(0x08, ModeSVC, c.PC)
}

func (c *CPU) irq() {
	c.Halted = false
	c.exception(0x18, ModeIRQ, c.PC+4)
}

// Reset clears the registers and starts at address 0 in ARM state, supervisor mode.
func (c *CPU) Reset() {
	c.R = [16]uint32{}
	c.bank13, c.bank14, c.spsrBank = [6]uint32{}, [6]uint32{}, [6]uint32{}
	c.fiqR, c.usrR = [5]uint32{}, [5]uint32{}
	c.Mode = ModeSVC
	c.I, c.F, c.T = 1, 1, 0
	c.N, c.Z, c.C, c.V = 0, 0, 0, 0
	c.Halted = false
	c.Branch(0)
}

// Step runs one instruction (or takes an interrupt).
func (c *CPU) Step() {
	if c.IRQLine && c.I == 0 {
		c.irq()
		return
	}
	if c.Halted {
		return
	}
	a := c.PC
	if c.T != 0 {
		op := c.pipe0
		c.pipe0 = c.pipe1
		c.PC = a + 2
		c.R[15] = a + 4
		c.pipe1 = c.bus.Fetch16((a + 4) &^ 1)
		thumbTable[(op>>6)&1023](c, op)
	} else {
		op := c.pipe0
		c.pipe0 = c.pipe1
		c.PC = a + 4
		c.R[15] = a + 8
		c.pipe1 = c.bus.Fetch32((a + 8) &^ 3)
		if c.condPass(op >> 28) {
			armTable[(op>>16)&0xff0|(op>>4)&0xf](c, op)
		}
	}
}

// ---- shifter ----

func (c *CPU) shiftImm(v, typ, amt uint32) uint32 {
	switch typ {
	case 0:
		if amt == 0 {
			c.sc = c.C
			return v
		}
		c.sc = v >> (32 - amt) & 1
		return v << amt
	case 1:
		if amt == 0 {
			c.sc = v >> 31
			return 0
		}
		c.sc = v >> (amt - 1) & 1
		return v >> amt
	case 2:
		if amt == 0 {
			c.sc = v >> 31
			return uint32(int32(v) >> 31)
		}
		c.sc = uint32(int32(v)>>(amt-1)) & 1
		return uint32(int32(v) >> amt)
	default:
		if amt == 0 {
			c.sc = v & 1
			return v>>1 | c.C<<31
		}
		c.sc = v >> (amt - 1) & 1
		return ror(v, amt)
	}
}

func (c *CPU) shiftReg(v, typ, s uint32) uint32 {
	if s == 0 {
		c.sc = c.C
		return v
	}
	switch typ {
	case 0:
		if s < 32 {
			c.sc = v >> (32 - s) & 1
			return v << s
		}
		if s == 32 {
			c.sc = v & 1
		} else {
			c.sc = 0
		}
		return 0
	case 1:
		if s < 32 {
			c.sc = v >> (s - 1) & 1
			return v >> s
		}
		if s == 32 {
			c.sc = v >> 31
		} else {
			c.sc = 0
		}
		return 0
	case 2:
		if s < 32 {
			c.sc = uint32(int32(v)>>(s-1)) & 1
			return uint32(int32(v) >> s)
		}
		c.sc = v >> 31
		return uint32(int32(v) >> 31)
	default:
		s &= 31
		if s == 0 {
			c.sc = v >> 31
			return v
		}
		c.sc = v >> (s - 1) & 1
		return ror(v, s)
	}
}

// ---- arithmetic with flags ----

func b2u(b bool) uint32 {
	if b {
		return 1
	}
	return 0
}

func (c *CPU) add(a, b, cin uint32, setf bool) uint32 {
	r := a + b + cin
	if setf {
		c.N = r >> 31
		c.Z = b2u(r == 0)
		c.C = b2u(uint64(a)+uint64(b)+uint64(cin) > 0xffffffff)
		c.V = (^(a ^ b) & (a ^ r)) >> 31
	}
	return r
}

// sub is a - b - (1 - cin).
func (c *CPU) sub(a, b, cin uint32, setf bool) uint32 {
	r := a - b - (1 - cin)
	if setf {
		c.N = r >> 31
		c.Z = b2u(r == 0)
		c.C = b2u(uint64(a) >= uint64(b)+uint64(1-cin))
		c.V = ((a ^ b) & (a ^ r)) >> 31
	}
	return r
}

func (c *CPU) logic(r uint32, setf bool) uint32 {
	if setf {
		c.N = r >> 31
		c.Z = b2u(r == 0)
		c.C = c.sc
	}
	return r
}

func (c *CPU) condPass(cond uint32) bool {
	switch cond {
	case 0:
		return c.Z == 1
	case 1:
		return c.Z == 0
	case 2:
		return c.C == 1
	case 3:
		return c.C == 0
	case 4:
		return c.N == 1
	case 5:
		return c.N == 0
	case 6:
		return c.V == 1
	case 7:
		return c.V == 0
	case 8:
		return c.C == 1 && c.Z == 0
	case 9:
		return c.C == 0 || c.Z == 1
	case 10:
		return c.N == c.V
	case 11:
		return c.N != c.V
	case 12:
		return c.Z == 0 && c.N == c.V
	case 13:
		return c.Z == 1 || c.N != c.V
	case 14:
		return true
	}
	return false
}

// SwitchMode changes the processor mode, swapping in that mode's banked
// registers (as the GBA's start-up code and BIOS calls do).
func (c *CPU) SwitchMode(m uint32) { c.switchMode(m) }

// State is everything about the processor that a save state needs.
type State struct {
	R                    [16]uint32
	Bank13, Bank14, SPSR [6]uint32
	FIQ, USR             [5]uint32
	CPSR, PC             uint32
	Cycles               int64
	Halted, IRQLine      bool
	Pipe                 [2]uint32
}

// SaveState returns the processor's state.
func (c *CPU) SaveState() State {
	return State{R: c.R, Bank13: c.bank13, Bank14: c.bank14, SPSR: c.spsrBank, FIQ: c.fiqR, USR: c.usrR,
		CPSR: c.CPSR(), PC: c.PC, Cycles: c.Cycles, Halted: c.Halted, IRQLine: c.IRQLine, Pipe: [2]uint32{c.pipe0, c.pipe1}}
}

// LoadState restores a state from SaveState (the registers are taken as they
// are, without switching banks).
func (c *CPU) LoadState(s State) {
	c.R, c.bank13, c.bank14, c.spsrBank, c.fiqR, c.usrR = s.R, s.Bank13, s.Bank14, s.SPSR, s.FIQ, s.USR
	v := s.CPSR
	c.N, c.Z, c.C, c.V = v>>31&1, v>>30&1, v>>29&1, v>>28&1
	c.I, c.F, c.T = v>>7&1, v>>6&1, v>>5&1
	c.Mode = v & 0x1f
	c.PC, c.Cycles, c.Halted, c.IRQLine = s.PC, s.Cycles, s.Halted, s.IRQLine
	c.pipe0, c.pipe1 = s.Pipe[0], s.Pipe[1]
}
