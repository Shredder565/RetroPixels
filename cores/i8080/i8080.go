// Package i8080 is RetroPixels' Intel 8080 processor, a Go port of
// web/emu/i8080.js (used by the Space Invaders arcade board). It behaves
// exactly like the JS core: all documented instructions, the undocumented
// duplicate opcodes, the 8080's own flag rules (half carry on AND/compare,
// bit 1 of the flags always set) and its timings.
//
//	c := i8080.New(bus)
//	t := c.Step()            // runs one instruction; returns clock cycles
//	t = c.Interrupt(0xcf)    // a device puts RST 1 on the bus; 0 if interrupts are off
package i8080

import (
	"encoding/binary"
	"errors"
)

// Bus is the memory and I/O the processor sees.
type Bus interface {
	Read(addr uint16) uint8
	Write(addr uint16, v uint8)
	In(port uint8) uint8
	Out(port, v uint8)
}

const (
	fC = 1
	fP = 4
	fA = 16
	fZ = 64
	fS = 128
)

var szp [256]int
var cyc [256]int

func init() {
	for i := 0; i < 256; i++ {
		p := i
		p ^= p >> 4
		p ^= p >> 2
		p ^= p >> 1
		v := i & fS
		if i == 0 {
			v |= fZ
		}
		if p&1 == 0 {
			v |= fP
		}
		szp[i] = v
	}
	for op := 0; op < 256; op++ {
		t := 4
		switch {
		case op < 0x40:
			switch op & 7 {
			case 0:
				t = 4
			case 1:
				t = 10
			case 2:
				t = [8]int{7, 7, 7, 7, 16, 16, 13, 13}[(op>>3)&7]
			case 3:
				t = 5
			case 4, 5:
				t = 5
				if (op>>3)&7 == 6 {
					t = 10
				}
			case 6:
				t = 7
				if (op>>3)&7 == 6 {
					t = 10
				}
			case 7:
				t = 4
			}
		case op < 0x80:
			if op == 0x76 || op&7 == 6 || (op>>3)&7 == 6 {
				t = 7
			} else {
				t = 5
			}
		case op < 0xc0:
			if op&7 == 6 {
				t = 7
			} else {
				t = 4
			}
		default:
			t = [8]int{5, 10, 10, 10, 11, 11, 7, 11}[op&7]
		}
		cyc[op] = t
	}
	cyc[0xc9], cyc[0xd9] = 10, 10
	cyc[0xe9], cyc[0xf9] = 5, 5
	cyc[0xe3], cyc[0xeb], cyc[0xf3], cyc[0xfb] = 18, 4, 4, 4
	cyc[0xcd], cyc[0xdd], cyc[0xed], cyc[0xfd] = 17, 17, 17, 17
}

// CPU is one 8080.
type CPU struct {
	bus                    Bus
	A, F, B, C, D, E, H, L int
	SP, PC                 int
	Inte, Halted, EIDelay  bool
}

// New makes an 8080 (reset) on a bus.
func New(bus Bus) *CPU {
	c := &CPU{bus: bus}
	c.Reset()
	return c
}

// Reset clears the registers.
func (c *CPU) Reset() {
	c.A, c.B, c.C, c.D, c.E, c.H, c.L = 0, 0, 0, 0, 0, 0, 0
	c.F, c.SP, c.PC = 2, 0, 0
	c.Inte, c.Halted, c.EIDelay = false, false, false
}

func (c *CPU) rd(ad int) int    { return int(c.bus.Read(uint16(ad))) }
func (c *CPU) wr(ad int, v int) { c.bus.Write(uint16(ad), uint8(v)) }
func (c *CPU) fetch() int {
	v := c.rd(c.PC)
	c.PC = (c.PC + 1) & 0xffff
	return v
}
func (c *CPU) fetch16() int { lo := c.fetch(); return lo | c.fetch()<<8 }
func (c *CPU) push(v int) {
	c.SP = (c.SP - 1) & 0xffff
	c.wr(c.SP, v>>8)
	c.SP = (c.SP - 1) & 0xffff
	c.wr(c.SP, v)
}
func (c *CPU) pop() int {
	lo := c.rd(c.SP)
	c.SP = (c.SP + 1) & 0xffff
	hi := c.rd(c.SP)
	c.SP = (c.SP + 1) & 0xffff
	return lo | hi<<8
}

func (c *CPU) getR(r int) int {
	switch r {
	case 0:
		return c.B
	case 1:
		return c.C
	case 2:
		return c.D
	case 3:
		return c.E
	case 4:
		return c.H
	case 5:
		return c.L
	case 6:
		return c.rd(c.H<<8 | c.L)
	}
	return c.A
}

func (c *CPU) setR(r, v int) {
	switch r {
	case 0:
		c.B = v
	case 1:
		c.C = v
	case 2:
		c.D = v
	case 3:
		c.E = v
	case 4:
		c.H = v
	case 5:
		c.L = v
	case 6:
		c.wr(c.H<<8|c.L, v)
	default:
		c.A = v
	}
}

func (c *CPU) getRP(p int) int {
	switch p {
	case 0:
		return c.B<<8 | c.C
	case 1:
		return c.D<<8 | c.E
	case 2:
		return c.H<<8 | c.L
	}
	return c.SP
}

func (c *CPU) setRP(p, v int) {
	v &= 0xffff
	switch p {
	case 0:
		c.B, c.C = v>>8, v&0xff
	case 1:
		c.D, c.E = v>>8, v&0xff
	case 2:
		c.H, c.L = v>>8, v&0xff
	default:
		c.SP = v
	}
}

func (c *CPU) cond(cc int) bool {
	switch cc {
	case 0:
		return c.F&fZ == 0
	case 1:
		return c.F&fZ != 0
	case 2:
		return c.F&fC == 0
	case 3:
		return c.F&fC != 0
	case 4:
		return c.F&fP == 0
	case 5:
		return c.F&fP != 0
	case 6:
		return c.F&fS == 0
	}
	return c.F&fS != 0
}

func b2i(b bool, v int) int {
	if b {
		return v
	}
	return 0
}

// alu: ADD ADC SUB SBB ANA XRA ORA CMP
func (c *CPU) alu(op, v int) {
	switch op {
	case 0, 1:
		cy := 0
		if op == 1 {
			cy = c.F & fC
		}
		r := c.A + v + cy
		c.F = szp[r&0xff] | b2i(r > 0xff, fC) | ((c.A ^ v ^ r) & fA)
		c.A = r & 0xff
	case 2, 3, 7:
		cy := 0
		if op == 3 {
			cy = c.F & fC
		}
		r := c.A - v - cy
		c.F = szp[r&0xff] | b2i(r < 0, fC) | (^(c.A ^ v ^ r) & fA)
		if op != 7 {
			c.A = r & 0xff
		}
	case 4:
		r := c.A & v
		c.F = szp[r] | b2i((c.A|v)&8 != 0, fA)
		c.A = r
	case 5:
		c.A ^= v
		c.F = szp[c.A]
	default:
		c.A |= v
		c.F = szp[c.A]
	}
}

func (c *CPU) inr(v int) int {
	v = (v + 1) & 0xff
	c.F = (c.F & fC) | szp[v] | b2i(v&0xf == 0, fA)
	return v
}

func (c *CPU) dcr(v int) int {
	v = (v - 1) & 0xff
	c.F = (c.F & fC) | szp[v] | b2i(v&0xf != 0xf, fA)
	return v
}

func (c *CPU) daa() {
	add, cy := 0, c.F&fC
	lo, hi := c.A&0xf, c.A>>4
	if c.F&fA != 0 || lo > 9 {
		add = 6
	}
	if cy != 0 || hi > 9 || (hi >= 9 && lo > 9) {
		add |= 0x60
		cy = fC
	}
	r := c.A + add
	c.F = szp[r&0xff] | cy | ((c.A ^ add ^ r) & fA)
	c.A = r & 0xff
}

func (c *CPU) exec(op int) int {
	t := cyc[op]
	if op >= 0x40 && op < 0x80 {
		if op == 0x76 {
			c.Halted = true
			return t
		}
		c.setR((op>>3)&7, c.getR(op&7))
		return t
	}
	if op >= 0x80 && op < 0xc0 {
		c.alu((op>>3)&7, c.getR(op&7))
		return t
	}
	y := (op >> 3) & 7
	p := y >> 1
	if op < 0x40 {
		switch op & 7 {
		case 0:
			return t
		case 1:
			if op&8 != 0 { // DAD
				r := (c.H<<8 | c.L) + c.getRP(p)
				c.F = (c.F &^ fC) | b2i(r > 0xffff, fC)
				c.H, c.L = (r>>8)&0xff, r&0xff
			} else {
				c.setRP(p, c.fetch16())
			}
			return t
		case 2:
			switch y {
			case 0:
				c.wr(c.B<<8|c.C, c.A)
			case 1:
				c.A = c.rd(c.B<<8 | c.C)
			case 2:
				c.wr(c.D<<8|c.E, c.A)
			case 3:
				c.A = c.rd(c.D<<8 | c.E)
			case 4:
				ad := c.fetch16()
				c.wr(ad, c.L)
				c.wr(ad+1, c.H)
			case 5:
				ad := c.fetch16()
				c.L = c.rd(ad)
				c.H = c.rd(ad + 1)
			case 6:
				c.wr(c.fetch16(), c.A)
			default:
				c.A = c.rd(c.fetch16())
			}
			return t
		case 3:
			if op&8 != 0 {
				c.setRP(p, c.getRP(p)-1)
			} else {
				c.setRP(p, c.getRP(p)+1)
			}
			return t
		case 4:
			c.setR(y, c.inr(c.getR(y)))
			return t
		case 5:
			c.setR(y, c.dcr(c.getR(y)))
			return t
		case 6:
			c.setR(y, c.fetch())
			return t
		default:
			switch y {
			case 0: // RLC
				c.A = ((c.A << 1) | (c.A >> 7)) & 0xff
				c.F = (c.F &^ fC) | (c.A & 1)
			case 1: // RRC
				c.F = (c.F &^ fC) | (c.A & 1)
				c.A = ((c.A >> 1) | (c.A << 7)) & 0xff
			case 2: // RAL
				cy := c.F & fC
				c.F = (c.F &^ fC) | (c.A >> 7)
				c.A = ((c.A << 1) | cy) & 0xff
			case 3: // RAR
				cy := c.F & fC
				c.F = (c.F &^ fC) | (c.A & 1)
				c.A = (c.A >> 1) | (cy << 7)
			case 4:
				c.daa()
			case 5:
				c.A ^= 0xff
			case 6:
				c.F |= fC
			default:
				c.F ^= fC
			}
			return t
		}
	}
	switch op & 7 {
	case 0:
		if c.cond(y) {
			c.PC = c.pop()
			t += 6
		}
		return t
	case 1:
		switch y {
		case 1, 3:
			c.PC = c.pop()
		case 5:
			c.PC = c.H<<8 | c.L
		case 7:
			c.SP = c.H<<8 | c.L
		case 6:
			v := c.pop()
			c.A = v >> 8
			c.F = (v & 0xd7) | 2
		default:
			c.setRP(p, c.pop())
		}
		return t
	case 2:
		ad := c.fetch16()
		if c.cond(y) {
			c.PC = ad
		}
		return t
	case 3:
		switch y {
		case 0, 1:
			c.PC = c.fetch16()
		case 2:
			port := c.fetch()
			c.bus.Out(uint8(port), uint8(c.A))
		case 3:
			port := c.fetch()
			c.A = int(c.bus.In(uint8(port)))
		case 4: // XTHL
			v := c.rd(c.SP) | c.rd((c.SP+1)&0xffff)<<8
			c.wr(c.SP, c.L)
			c.wr((c.SP+1)&0xffff, c.H)
			c.H, c.L = v>>8, v&0xff
		case 5: // XCHG
			c.D, c.H = c.H, c.D
			c.E, c.L = c.L, c.E
		case 6:
			c.Inte = false
		default:
			c.Inte = true
			c.EIDelay = true
		}
		return t
	case 4:
		ad := c.fetch16()
		if c.cond(y) {
			c.push(c.PC)
			c.PC = ad
			t += 6
		}
		return t
	case 5:
		if op&8 != 0 {
			ad := c.fetch16()
			c.push(c.PC)
			c.PC = ad
			return t
		}
		if p == 3 {
			c.push(c.A<<8 | ((c.F & 0xd7) | 2))
		} else {
			c.push(c.getRP(p))
		}
		return t
	case 6:
		c.alu(y, c.fetch())
		return t
	}
	c.push(c.PC)
	c.PC = y << 3
	return t
}

// Step runs one instruction and returns the clock cycles it took.
func (c *CPU) Step() int {
	c.EIDelay = false
	if c.Halted {
		return 4
	}
	return c.exec(c.fetch())
}

// Interrupt runs the instruction a device puts on the bus (normally RST n);
// it returns the cycles used, or 0 when interrupts are off.
func (c *CPU) Interrupt(op uint8) int {
	if !c.Inte || c.EIDelay {
		return 0
	}
	c.Inte, c.Halted = false, false
	return c.exec(int(op))
}

var errState = errors.New("i8080: damaged save state")

// AppendState adds the processor's state to b.
func (c *CPU) AppendState(b []byte) []byte {
	for _, v := range []int{c.A, c.F, c.B, c.C, c.D, c.E, c.H, c.L, c.SP, c.PC} {
		b = binary.LittleEndian.AppendUint16(b, uint16(v))
	}
	for _, v := range []bool{c.Inte, c.Halted, c.EIDelay} {
		if v {
			b = append(b, 1)
		} else {
			b = append(b, 0)
		}
	}
	return b
}

// LoadState reads what AppendState wrote and returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	if len(b) < 23 {
		return nil, errState
	}
	r := make([]int, 10)
	for i := range r {
		r[i] = int(binary.LittleEndian.Uint16(b[i*2:]))
	}
	c.A, c.F, c.B, c.C, c.D, c.E, c.H, c.L, c.SP, c.PC = r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]
	c.Inte, c.Halted, c.EIDelay = b[20] != 0, b[21] != 0, b[22] != 0
	return b[23:], nil
}
