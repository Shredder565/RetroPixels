// Package mcs48 is RetroPixels' Intel MCS-48 processor (8035 / 8039 / 8048
// family), a Go port of web/emu/mcs48.js, used as the Donkey Kong sound
// processor. It behaves exactly like the JS core: the instruction set, the
// two register banks, the 8-level stack in internal RAM, the two program
// memory banks, the timer/counter and its interrupt, the external interrupt,
// the T0/T1 test pins and the ports. The 8243 port expander (MOVD/ANLD/ORLD)
// goes to the bus if it implements Expander.
//
//	c := mcs48.New(bus, 64)
//	n := c.Step() // machine cycles used (one machine cycle = 15 clock periods)
package mcs48

import (
	"encoding/binary"
	"errors"
)

// Bus is what the processor is wired to.
type Bus interface {
	ROM(addr int) uint8     // program memory (12-bit address)
	ReadX(a uint8) uint8    // MOVX A,@Rr
	WriteX(a, v uint8)      // MOVX @Rr,A
	OutP(port int, v uint8) // port 1 / 2 / bus (0) written
	InP(port int) uint8     // port 1 / 2 / bus (0) read
	T0() bool               // test pins (true = high)
	T1() bool
	IntLine() bool // /INT: true = interrupt requested (pin low)
}

// Expander is implemented by buses with an 8243 port expander.
// op: 0 MOVD A,Pp; 1 MOVD Pp,A; 2 ANLD; 3 ORLD.
type Expander interface {
	Expander(op, port int, v uint8) uint8
}

const (
	fCY = 0x80
	fAC = 0x40
	fF0 = 0x20
	fBS = 0x10
)

// CPU is one MCS-48.
type CPU struct {
	bus Bus
	exp Expander
	RAM []uint8
	rm  int

	A, PC, PSW           int
	F1                   bool
	DBF, A11, T          int
	TF, Tirq             bool
	TimerOn, CounterOn   bool
	Prescale             int
	LastT1               bool
	IntEn, TcntEn, InIrq bool
	P1, P2, BusLatch     int
}

// New makes a processor with ramSize bytes of internal RAM (64 if 0).
func New(bus Bus, ramSize int) *CPU {
	if ramSize <= 0 {
		ramSize = 64
	}
	c := &CPU{bus: bus, RAM: make([]uint8, ramSize), rm: ramSize - 1}
	c.exp, _ = bus.(Expander)
	c.Reset()
	return c
}

// Reset is the processor's reset.
func (c *CPU) Reset() {
	c.A, c.PC, c.PSW = 0, 0, 0
	c.F1 = false
	c.DBF, c.A11, c.T = 0, 0, 0
	c.TF, c.Tirq = false, false
	c.TimerOn, c.CounterOn = false, false
	c.Prescale = 0
	c.LastT1 = false
	c.IntEn, c.TcntEn, c.InIrq = false, false, false
	c.P1, c.P2, c.BusLatch = 0xff, 0xff, 0xff
}

// Port is a port's output latch (1, 2, or 0 for the bus).
func (c *CPU) Port(n int) int {
	switch n {
	case 1:
		return c.P1
	case 2:
		return c.P2
	}
	return c.BusLatch
}

func (c *CPU) rom(ad int) int { return int(c.bus.ROM(ad & 0xfff)) }
func (c *CPU) fetch() int {
	v := c.rom(c.PC)
	c.PC = (c.PC & 0x800) | ((c.PC + 1) & 0x7ff)
	return v
}
func (c *CPU) regBase() int {
	if c.PSW&fBS != 0 {
		return 24
	}
	return 0
}
func (c *CPU) getR(r int) int    { return int(c.RAM[(c.regBase()+r)&c.rm]) }
func (c *CPU) setR(r int, v int) { c.RAM[(c.regBase()+r)&c.rm] = uint8(v) }
func (c *CPU) ind(r int) int     { return c.getR(r) & c.rm }

func (c *CPU) push() {
	sp := c.PSW & 7
	ad := 8 + sp*2
	c.RAM[ad&c.rm] = uint8(c.PC)
	c.RAM[(ad+1)&c.rm] = uint8(((c.PC >> 8) & 0x0f) | (c.PSW & 0xf0))
	c.PSW = (c.PSW & 0xf8) | ((sp + 1) & 7)
}

func (c *CPU) pop(restorePsw bool) {
	sp := (c.PSW - 1) & 7
	ad := 8 + sp*2
	lo, hi := int(c.RAM[ad&c.rm]), int(c.RAM[(ad+1)&c.rm])
	c.PC = (hi&0x0f)<<8 | lo
	c.PSW = (c.PSW & 0xf8) | sp
	if restorePsw {
		c.PSW = (hi & 0xf0) | sp
	}
}

func (c *CPU) add(v int, withCarry bool) {
	cy := 0
	if withCarry && c.PSW&fCY != 0 {
		cy = 1
	}
	r := c.A + v + cy
	c.PSW &^= fCY | fAC
	if r > 0xff {
		c.PSW |= fCY
	}
	if (c.A&0xf)+(v&0xf)+cy > 0xf {
		c.PSW |= fAC
	}
	c.A = r & 0xff
}

// jcc: conditional jump within the page of the address byte.
func (c *CPU) jcc(cond bool) int {
	page := c.PC & 0xf00
	lo := c.fetch()
	if cond {
		c.PC = page | lo
	}
	return 2
}

func (c *CPU) outP(port, v int) { c.bus.OutP(port, uint8(v)) }
func (c *CPU) inP(port int) int { return int(c.bus.InP(port)) }
func (c *CPU) expander(op, port, v int) int {
	if c.exp == nil {
		return 0xf
	}
	return int(c.exp.Expander(op, port, uint8(v))) & 0xf
}

func (c *CPU) exec(op int) int {
	switch op {
	case 0x00:
		return 1
	case 0x02:
		c.BusLatch = c.A
		c.outP(0, c.A)
		return 2
	case 0x03:
		c.add(c.fetch(), false)
		return 2
	case 0x05:
		c.IntEn = true
		return 1
	case 0x07:
		c.A = (c.A - 1) & 0xff
		return 1
	case 0x08:
		c.A = c.inP(0)
		return 2
	case 0x09:
		c.A = c.inP(1)
		return 2
	case 0x0a:
		c.A = c.inP(2)
		return 2
	case 0x0c, 0x0d, 0x0e, 0x0f:
		c.A = c.expander(0, op&3, 0)
		return 2
	case 0x10, 0x11:
		ad := c.ind(op & 1)
		c.RAM[ad]++
		return 1
	case 0x13:
		c.add(c.fetch(), true)
		return 2
	case 0x15:
		c.IntEn = false
		return 1
	case 0x16:
		t := c.TF
		c.TF = false
		return c.jcc(t)
	case 0x17:
		c.A = (c.A + 1) & 0xff
		return 1
	case 0x20, 0x21:
		ad := c.ind(op & 1)
		v := int(c.RAM[ad])
		c.RAM[ad] = uint8(c.A)
		c.A = v
		return 1
	case 0x23:
		c.A = c.fetch()
		return 2
	case 0x25:
		c.TcntEn = true
		return 1
	case 0x26:
		return c.jcc(!c.bus.T0())
	case 0x27:
		c.A = 0
		return 1
	case 0x30, 0x31:
		ad := c.ind(op & 1)
		v := int(c.RAM[ad])
		c.RAM[ad] = uint8((v & 0xf0) | (c.A & 0x0f))
		c.A = (c.A & 0xf0) | (v & 0x0f)
		return 1
	case 0x35:
		c.TcntEn = false
		c.Tirq = false
		return 1
	case 0x36:
		return c.jcc(c.bus.T0())
	case 0x37:
		c.A ^= 0xff
		return 1
	case 0x39:
		c.P1 = c.A
		c.outP(1, c.A)
		return 2
	case 0x3a:
		c.P2 = c.A
		c.outP(2, c.A)
		return 2
	case 0x3c, 0x3d, 0x3e, 0x3f:
		c.expander(1, op&3, c.A)
		return 2
	case 0x40, 0x41:
		c.A |= int(c.RAM[c.ind(op&1)])
		return 1
	case 0x42:
		c.A = c.T
		return 1
	case 0x43:
		c.A |= c.fetch()
		return 2
	case 0x45:
		c.CounterOn = true
		c.TimerOn = false
		return 1
	case 0x46:
		return c.jcc(!c.bus.T1())
	case 0x47:
		c.A = ((c.A << 4) | (c.A >> 4)) & 0xff
		return 1
	case 0x50, 0x51:
		c.A &= int(c.RAM[c.ind(op&1)])
		return 1
	case 0x53:
		c.A &= c.fetch()
		return 2
	case 0x55:
		c.TimerOn = true
		c.CounterOn = false
		c.Prescale = 0
		return 1
	case 0x56:
		return c.jcc(c.bus.T1())
	case 0x57: // DA A
		if (c.A&0x0f) > 9 || c.PSW&fAC != 0 {
			if c.A > 0xf9 {
				c.PSW |= fCY
			}
			c.A = (c.A + 6) & 0xff
		}
		if (c.A&0xf0) > 0x90 || c.PSW&fCY != 0 {
			c.A = (c.A + 0x60) & 0xff
			c.PSW |= fCY
		}
		return 1
	case 0x60, 0x61:
		c.add(int(c.RAM[c.ind(op&1)]), false)
		return 1
	case 0x62:
		c.T = c.A
		return 1
	case 0x65:
		c.TimerOn, c.CounterOn = false, false
		return 1
	case 0x67: // RRC A
		cy := c.PSW & fCY
		c.PSW &^= fCY
		if c.A&1 != 0 {
			c.PSW |= fCY
		}
		c.A >>= 1
		if cy != 0 {
			c.A |= 0x80
		}
		return 1
	case 0x70, 0x71:
		c.add(int(c.RAM[c.ind(op&1)]), true)
		return 1
	case 0x75:
		return 1
	case 0x76:
		return c.jcc(c.F1)
	case 0x77:
		c.A = ((c.A >> 1) | (c.A << 7)) & 0xff
		return 1
	case 0x80, 0x81:
		c.A = int(c.bus.ReadX(uint8(c.getR(op & 1))))
		return 2
	case 0x83:
		c.pop(false)
		return 2
	case 0x85:
		c.PSW &^= fF0
		return 1
	case 0x86:
		return c.jcc(c.bus.IntLine())
	case 0x88:
		c.BusLatch |= c.fetch()
		c.outP(0, c.BusLatch)
		return 2
	case 0x89:
		c.P1 |= c.fetch()
		c.outP(1, c.P1)
		return 2
	case 0x8a:
		c.P2 |= c.fetch()
		c.outP(2, c.P2)
		return 2
	case 0x8c, 0x8d, 0x8e, 0x8f:
		c.expander(3, op&3, c.A)
		return 2
	case 0x90, 0x91:
		c.bus.WriteX(uint8(c.getR(op&1)), uint8(c.A))
		return 2
	case 0x93:
		c.pop(true)
		c.InIrq = false
		c.A11 = c.DBF
		return 2
	case 0x95:
		c.PSW ^= fF0
		return 1
	case 0x96:
		return c.jcc(c.A != 0)
	case 0x97:
		c.PSW &^= fCY
		return 1
	case 0x98:
		c.BusLatch &= c.fetch()
		c.outP(0, c.BusLatch)
		return 2
	case 0x99:
		c.P1 &= c.fetch()
		c.outP(1, c.P1)
		return 2
	case 0x9a:
		c.P2 &= c.fetch()
		c.outP(2, c.P2)
		return 2
	case 0x9c, 0x9d, 0x9e, 0x9f:
		c.expander(2, op&3, c.A)
		return 2
	case 0xa0, 0xa1:
		c.RAM[c.ind(op&1)] = uint8(c.A)
		return 1
	case 0xa3:
		c.A = c.rom((c.PC & 0xf00) | c.A)
		return 2
	case 0xa5:
		c.F1 = false
		return 1
	case 0xa7:
		c.PSW ^= fCY
		return 1
	case 0xb0, 0xb1:
		ad := c.ind(op & 1)
		c.RAM[ad] = uint8(c.fetch())
		return 2
	case 0xb3:
		c.PC = (c.PC & 0xf00) | c.rom((c.PC&0xf00)|c.A)
		return 2
	case 0xb5:
		c.F1 = !c.F1
		return 1
	case 0xb6:
		return c.jcc(c.PSW&fF0 != 0)
	case 0xc5:
		c.PSW &^= fBS
		return 1
	case 0xc6:
		return c.jcc(c.A == 0)
	case 0xc7:
		c.A = c.PSW | 0x08
		return 1
	case 0xd0, 0xd1:
		c.A ^= int(c.RAM[c.ind(op&1)])
		return 1
	case 0xd3:
		c.A ^= c.fetch()
		return 2
	case 0xd5:
		c.PSW |= fBS
		return 1
	case 0xd7:
		c.PSW = c.A & 0xf7
		return 1
	case 0xe3:
		c.A = c.rom(0x300 | c.A)
		return 2
	case 0xe5:
		c.DBF = 0
		if !c.InIrq {
			c.A11 = 0
		}
		return 1
	case 0xe6:
		return c.jcc(c.PSW&fCY == 0)
	case 0xe7:
		c.A = ((c.A << 1) | (c.A >> 7)) & 0xff
		return 1
	case 0xf0, 0xf1:
		c.A = int(c.RAM[c.ind(op&1)])
		return 1
	case 0xf5:
		c.DBF = 0x800
		if !c.InIrq {
			c.A11 = 0x800
		}
		return 1
	case 0xf6:
		return c.jcc(c.PSW&fCY != 0)
	case 0xf7: // RLC A
		cy := c.PSW & fCY
		c.PSW &^= fCY
		if c.A&0x80 != 0 {
			c.PSW |= fCY
		}
		c.A = (c.A << 1) & 0xff
		if cy != 0 {
			c.A |= 1
		}
		return 1
	}
	lo3 := op & 7
	if op >= 0x08 {
		switch op & 0xf8 {
		case 0x18:
			c.setR(lo3, c.getR(lo3)+1)
			return 1
		case 0x28:
			v := c.getR(lo3)
			c.setR(lo3, c.A)
			c.A = v
			return 1
		case 0x48:
			c.A |= c.getR(lo3)
			return 1
		case 0x58:
			c.A &= c.getR(lo3)
			return 1
		case 0x68:
			c.add(c.getR(lo3), false)
			return 1
		case 0x78:
			c.add(c.getR(lo3), true)
			return 1
		case 0xa8:
			c.setR(lo3, c.A)
			return 1
		case 0xb8:
			c.setR(lo3, c.fetch())
			return 2
		case 0xc8:
			c.setR(lo3, c.getR(lo3)-1)
			return 1
		case 0xd8:
			c.A ^= c.getR(lo3)
			return 1
		case 0xe8: // DJNZ
			page := c.PC & 0xf00
			lo := c.fetch()
			v := (c.getR(lo3) - 1) & 0xff
			c.setR(lo3, v)
			if v != 0 {
				c.PC = page | lo
			}
			return 2
		case 0xf8:
			c.A = c.getR(lo3)
			return 1
		}
	}
	switch op & 0x1f {
	case 0x04: // JMP
		lo := c.fetch()
		c.PC = c.A11 | (op&0xe0)<<3 | lo
		return 2
	case 0x14: // CALL
		lo := c.fetch()
		c.push()
		c.PC = c.A11 | (op&0xe0)<<3 | lo
		return 2
	case 0x12: // JBb
		return c.jcc(c.A&(1<<(op>>5)) != 0)
	}
	return 1
}

// tick: the timer counts every 32 machine cycles; the counter counts falling
// edges of T1.
func (c *CPU) tick(cycles int) {
	if c.TimerOn {
		c.Prescale += cycles
		for c.Prescale >= 32 {
			c.Prescale -= 32
			c.T = (c.T + 1) & 0xff
			if c.T == 0 {
				c.TF = true
				if c.TcntEn {
					c.Tirq = true
				}
			}
		}
	} else if c.CounterOn {
		now := c.bus.T1()
		if c.LastT1 && !now {
			c.T = (c.T + 1) & 0xff
			if c.T == 0 {
				c.TF = true
				if c.TcntEn {
					c.Tirq = true
				}
			}
		}
		c.LastT1 = now
	}
}

// Step runs one instruction (or takes an interrupt) and returns the machine
// cycles used.
func (c *CPU) Step() int {
	var cycles int
	if !c.InIrq && c.IntEn && c.bus.IntLine() {
		c.push()
		c.PC, c.InIrq, c.A11, cycles = 3, true, 0, 2
	} else if !c.InIrq && c.Tirq && c.TcntEn {
		c.Tirq = false
		c.push()
		c.PC, c.InIrq, c.A11, cycles = 7, true, 0, 2
	} else {
		cycles = c.exec(c.fetch())
	}
	c.tick(cycles)
	return cycles
}

var errState = errors.New("mcs48: damaged save state")

func bi(b bool) int {
	if b {
		return 1
	}
	return 0
}

func (c *CPU) regs() []int {
	return []int{c.A, c.PC, c.PSW, bi(c.F1), c.DBF, c.A11, c.T, bi(c.TF), bi(c.Tirq), bi(c.TimerOn), bi(c.CounterOn),
		c.Prescale, bi(c.LastT1), bi(c.IntEn), bi(c.TcntEn), bi(c.InIrq), c.P1, c.P2, c.BusLatch}
}

// AppendState adds the processor's state (registers and RAM) to b.
func (c *CPU) AppendState(b []byte) []byte {
	for _, v := range c.regs() {
		b = binary.LittleEndian.AppendUint32(b, uint32(int32(v)))
	}
	return append(b, c.RAM...)
}

// LoadState reads what AppendState wrote and returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	n := len(c.regs())
	if len(b) < n*4+len(c.RAM) {
		return nil, errState
	}
	r := make([]int, n)
	for i := range r {
		r[i] = int(int32(binary.LittleEndian.Uint32(b[i*4:])))
	}
	c.A, c.PC, c.PSW = r[0], r[1], r[2]
	c.F1, c.DBF, c.A11, c.T = r[3] != 0, r[4], r[5], r[6]
	c.TF, c.Tirq, c.TimerOn, c.CounterOn = r[7] != 0, r[8] != 0, r[9] != 0, r[10] != 0
	c.Prescale, c.LastT1 = r[11], r[12] != 0
	c.IntEn, c.TcntEn, c.InIrq = r[13] != 0, r[14] != 0, r[15] != 0
	c.P1, c.P2, c.BusLatch = r[16], r[17], r[18]
	copy(c.RAM, b[n*4:])
	return b[n*4+len(c.RAM):], nil
}
