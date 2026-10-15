// Package z80 is RetroPixels' Zilog Z80 processor, a Go port of web/emu/z80.js
// (the web version's Z80, used by the Master System, as the Genesis sound
// processor and by arcade boards). It behaves exactly like the JS core: the
// documented instructions plus the undocumented ones, the undocumented flag
// bits 3 and 5, MEMPTR (WZ) and the Q latch used by SCF/CCF.
//
//	cpu := z80.New(bus)
//	t := cpu.Step() // runs one instruction (or takes an interrupt); returns T-states
//	cpu.IRQ = true  // the maskable interrupt line (level)
//	cpu.NMI()       // an edge on the non-maskable interrupt line
//
// The machine around the processor is a Bus. A bus that also implements
// Vectorer supplies the data-bus byte read in interrupt mode 2 (0xff without
// it), and one that implements Acker is told whenever a maskable interrupt is
// taken (for boards that hold the line until then).
package z80

import (
	"encoding/binary"
	"errors"
)

// Bus is the memory and I/O the processor sees. Addresses and ports are 16
// bits (the high byte of a port is the A or B register, as on the chip).
type Bus interface {
	Read(addr uint16) uint8
	Write(addr uint16, v uint8)
	In(port uint16) uint8
	Out(port uint16, v uint8)
}

// Vectorer is implemented by buses that put a byte on the data bus when an
// interrupt is acknowledged (used in interrupt mode 2).
type Vectorer interface {
	Vector() uint8
}

// Acker is implemented by buses that want to know when the processor takes
// a maskable interrupt.
type Acker interface {
	Ack()
}

const (
	fC = 1
	fN = 2
	fP = 4
	fX = 8
	fH = 16
	fY = 32
	fZ = 64
	fS = 128
)

var sz, szp [256]int

func init() {
	for i := 0; i < 256; i++ {
		v := i & (fS | fY | fX)
		if i == 0 {
			v |= fZ
		}
		sz[i] = v
		p := i
		p ^= p >> 4
		p ^= p >> 2
		p ^= p >> 1
		szp[i] = v
		if p&1 == 0 {
			szp[i] |= fP
		}
	}
}

var imMode = [8]int{0, 0, 1, 2, 0, 0, 1, 2}

// sx sign-extends a byte.
func sx(v int) int { return int(int8(uint8(v))) }

// CPU is one Z80. Registers are kept as ints holding 8- or 16-bit values, the
// way the JS core keeps them; Regs and SetRegs give typed access.
type CPU struct {
	bus Bus
	vec Vectorer
	ack Acker

	a, f, b, c, d, e, h, l int
	ix, iy, sp, pc         int
	iv, rv, wz             int
	af2, bc2, de2, hl2     int
	iff1, iff2, im         int
	halted, eiBlock        bool
	q, qn                  int
	nmiPending             bool

	// IRQ is the level of the maskable interrupt line.
	IRQ bool
}

// New makes a processor on bus, reset.
func New(bus Bus) *CPU {
	c := &CPU{bus: bus}
	c.vec, _ = bus.(Vectorer)
	c.ack, _ = bus.(Acker)
	c.Reset()
	return c
}

// Reset puts the processor in its power-on state. The IRQ line is the
// machine's, so it is left as it is.
func (c *CPU) Reset() {
	c.a, c.f = 0xff, 0xff
	c.b, c.c, c.d, c.e, c.h, c.l = 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	c.ix, c.iy, c.sp = 0xffff, 0xffff, 0xffff
	c.pc, c.iv, c.rv, c.wz = 0, 0, 0, 0
	c.af2, c.bc2, c.de2, c.hl2 = 0xffff, 0xffff, 0xffff, 0xffff
	c.iff1, c.iff2, c.im = 0, 0, 0
	c.halted, c.eiBlock = false, false
	c.q, c.qn = 0, 0
	c.nmiPending = false
}

// NMI makes a non-maskable interrupt happen before the next instruction.
func (c *CPU) NMI() { c.nmiPending = true }

// Halted tells whether the processor is stopped in a HALT instruction.
func (c *CPU) Halted() bool { return c.halted }

// PC is the program counter.
func (c *CPU) PC() uint16 { return uint16(c.pc) }

func (c *CPU) rd(ad int) int    { return int(c.bus.Read(uint16(ad))) }
func (c *CPU) wr(ad, v int)     { c.bus.Write(uint16(ad), uint8(v)) }
func (c *CPU) pin(port int) int { return int(c.bus.In(uint16(port))) }
func (c *CPU) pout(port, v int) { c.bus.Out(uint16(port), uint8(v)) }
func (c *CPU) setF(v int)       { c.f = v; c.qn = 1 }
func (c *CPU) incR()            { c.rv = (c.rv & 0x80) | ((c.rv + 1) & 0x7f) }
func (c *CPU) fetch() int       { v := c.rd(c.pc); c.pc = (c.pc + 1) & 0xffff; return v }
func (c *CPU) fetch16() int     { lo := c.fetch(); return lo | c.fetch()<<8 }
func (c *CPU) m1() int          { c.incR(); return c.fetch() }
func (c *CPU) hl() int          { return c.h<<8 | c.l }
func (c *CPU) setHLreg(v int)   { c.h = v >> 8 & 0xff; c.l = v & 0xff }
func (c *CPU) push(v int) {
	c.sp = (c.sp - 1) & 0xffff
	c.wr(c.sp, v>>8)
	c.sp = (c.sp - 1) & 0xffff
	c.wr(c.sp, v)
}
func (c *CPU) pop() int {
	lo := c.rd(c.sp)
	c.sp = (c.sp + 1) & 0xffff
	hi := c.rd(c.sp)
	c.sp = (c.sp + 1) & 0xffff
	return lo | hi<<8
}

func (c *CPU) getR(r int) int {
	switch r {
	case 0:
		return c.b
	case 1:
		return c.c
	case 2:
		return c.d
	case 3:
		return c.e
	case 4:
		return c.h
	case 5:
		return c.l
	}
	return c.a
}

func (c *CPU) setR(r, v int) {
	switch r {
	case 0:
		c.b = v
	case 1:
		c.c = v
	case 2:
		c.d = v
	case 3:
		c.e = v
	case 4:
		c.h = v
	case 5:
		c.l = v
	default:
		c.a = v
	}
}

func (c *CPU) getRX(r, x int) int {
	if x != 0 && (r == 4 || r == 5) {
		v := c.iy
		if x == 1 {
			v = c.ix
		}
		if r == 4 {
			return v >> 8
		}
		return v & 0xff
	}
	return c.getR(r)
}

func (c *CPU) setRX(r, v, x int) {
	if x != 0 && (r == 4 || r == 5) {
		w := c.iy
		if x == 1 {
			w = c.ix
		}
		if r == 4 {
			w = (w & 0xff) | v<<8
		} else {
			w = (w & 0xff00) | v
		}
		if x == 1 {
			c.ix = w
		} else {
			c.iy = w
		}
		return
	}
	c.setR(r, v)
}

func (c *CPU) getHL(x int) int {
	switch x {
	case 0:
		return c.h<<8 | c.l
	case 1:
		return c.ix
	}
	return c.iy
}

func (c *CPU) setHL(x, v int) {
	switch x {
	case 0:
		c.h = v >> 8
		c.l = v & 0xff
	case 1:
		c.ix = v
	default:
		c.iy = v
	}
}

func (c *CPU) getRP(p, x int) int {
	switch p {
	case 0:
		return c.b<<8 | c.c
	case 1:
		return c.d<<8 | c.e
	case 2:
		return c.getHL(x)
	}
	return c.sp
}

func (c *CPU) setRP(p, v, x int) {
	switch p {
	case 0:
		c.b = v >> 8
		c.c = v & 0xff
	case 1:
		c.d = v >> 8
		c.e = v & 0xff
	case 2:
		c.setHL(x, v)
	default:
		c.sp = v
	}
}

func (c *CPU) memAddr(x int) int {
	if x == 0 {
		return c.h<<8 | c.l
	}
	base := c.iy
	if x == 1 {
		base = c.ix
	}
	ad := (base + sx(c.fetch())) & 0xffff
	c.wz = ad
	return ad
}

func (c *CPU) cond(y int) bool {
	switch y {
	case 0:
		return c.f&fZ == 0
	case 1:
		return c.f&fZ != 0
	case 2:
		return c.f&fC == 0
	case 3:
		return c.f&fC != 0
	case 4:
		return c.f&fP == 0
	case 5:
		return c.f&fP != 0
	case 6:
		return c.f&fS == 0
	}
	return c.f&fS != 0
}

// ---- 8-bit arithmetic ----

func (c *CPU) add8(v, cy int) {
	a := c.a
	r := a + v + cy
	c.setF(sz[r&0xff] | ((r >> 8) & 1) | ((a ^ v ^ r) & fH) | (((a ^ ^v) & (a ^ r) & 0x80) >> 5))
	c.a = r & 0xff
}

func (c *CPU) sub8(v, cy int) int {
	a := c.a
	r := a - v - cy
	c.setF(sz[r&0xff] | fN | ((r >> 8) & 1) | ((a ^ v ^ r) & fH) | (((a ^ v) & (a ^ r) & 0x80) >> 5))
	return r & 0xff
}

func (c *CPU) alu(op, v int) {
	switch op {
	case 0:
		c.add8(v, 0)
	case 1:
		c.add8(v, c.f&fC)
	case 2:
		c.a = c.sub8(v, 0)
	case 3:
		c.a = c.sub8(v, c.f&fC)
	case 4:
		c.a &= v
		c.setF(szp[c.a] | fH)
	case 5:
		c.a ^= v
		c.setF(szp[c.a])
	case 6:
		c.a |= v
		c.setF(szp[c.a])
	default:
		c.sub8(v, 0)
		c.setF((c.f &^ (fX | fY)) | (v & (fX | fY)))
	}
}

func (c *CPU) inc8(v int) int {
	r := (v + 1) & 0xff
	fl := (c.f & fC) | sz[r]
	if r&0xf == 0 {
		fl |= fH
	}
	if v == 0x7f {
		fl |= fP
	}
	c.setF(fl)
	return r
}

func (c *CPU) dec8(v int) int {
	r := (v - 1) & 0xff
	fl := (c.f & fC) | fN | sz[r]
	if v&0xf == 0 {
		fl |= fH
	}
	if v == 0x80 {
		fl |= fP
	}
	c.setF(fl)
	return r
}

func (c *CPU) rot(y, v int) int {
	var r, cy int
	switch y {
	case 0:
		cy = v >> 7
		r = ((v << 1) | cy) & 0xff
	case 1:
		cy = v & 1
		r = (v >> 1) | (cy << 7)
	case 2:
		cy = v >> 7
		r = ((v << 1) | (c.f & fC)) & 0xff
	case 3:
		cy = v & 1
		r = (v >> 1) | ((c.f & fC) << 7)
	case 4:
		cy = v >> 7
		r = (v << 1) & 0xff
	case 5:
		cy = v & 1
		r = (v >> 1) | (v & 0x80)
	case 6:
		cy = v >> 7
		r = ((v << 1) | 1) & 0xff
	default:
		cy = v & 1
		r = v >> 1
	}
	c.setF(szp[r] | cy)
	return r
}

func (c *CPU) bit(y, v, src35 int) {
	r := v & (1 << y)
	fl := (c.f & fC) | fH | (r & fS) | (src35 & (fX | fY))
	if r == 0 {
		fl |= fZ | fP
	}
	c.setF(fl)
}

func (c *CPU) addHL(x, v int) {
	hv := c.getHL(x)
	r := hv + v
	c.wz = (hv + 1) & 0xffff
	c.setF((c.f & (fS | fZ | fP)) | ((r >> 16) & 1) | (((hv ^ v ^ r) >> 8) & fH) | ((r >> 8) & (fX | fY)))
	c.setHL(x, r&0xffff)
}

func (c *CPU) adcHL(v int) {
	hv := c.hl()
	r := hv + v + (c.f & fC)
	c.wz = (hv + 1) & 0xffff
	fl := ((r >> 8) & (fS | fX | fY)) | (((hv ^ v ^ r) >> 8) & fH) |
		(((^(hv ^ v) & (hv ^ r)) & 0x8000) >> 13) | ((r >> 16) & 1)
	if r&0xffff == 0 {
		fl |= fZ
	}
	c.setF(fl)
	c.h = (r >> 8) & 0xff
	c.l = r & 0xff
}

func (c *CPU) sbcHL(v int) {
	hv := c.hl()
	r := hv - v - (c.f & fC)
	c.wz = (hv + 1) & 0xffff
	fl := fN | ((r >> 8) & (fS | fX | fY)) | (((hv ^ v ^ r) >> 8) & fH) |
		((((hv ^ v) & (hv ^ r)) & 0x8000) >> 13) | ((r >> 16) & 1)
	if r&0xffff == 0 {
		fl |= fZ
	}
	c.setF(fl)
	c.h = (r >> 8) & 0xff
	c.l = r & 0xff
}

// exec runs an unprefixed instruction, or one after DD (x=1) / FD (x=2).
func (c *CPU) exec(op, x int) int {
	y, z := (op>>3)&7, op&7
	p, odd := y>>1, y&1
	switch op >> 6 {
	case 0:
		switch z {
		case 0:
			switch y {
			case 0:
				return 4
			case 1:
				t := c.a<<8 | c.f
				c.a = c.af2 >> 8
				c.f = c.af2 & 0xff
				c.af2 = t
				return 4
			case 2:
				dd := c.fetch()
				c.b = (c.b - 1) & 0xff
				if c.b != 0 {
					c.pc = (c.pc + sx(dd)) & 0xffff
					c.wz = c.pc
					return 13
				}
				return 8
			case 3:
				dd := c.fetch()
				c.pc = (c.pc + sx(dd)) & 0xffff
				c.wz = c.pc
				return 12
			default:
				dd := c.fetch()
				if c.cond(y - 4) {
					c.pc = (c.pc + sx(dd)) & 0xffff
					c.wz = c.pc
					return 12
				}
				return 7
			}
		case 1:
			if odd == 0 {
				c.setRP(p, c.fetch16(), x)
				return 10
			}
			c.addHL(x, c.getRP(p, x))
			return 11
		case 2:
			switch y {
			case 0:
				ad := c.b<<8 | c.c
				c.wr(ad, c.a)
				c.wz = ((ad + 1) & 0xff) | c.a<<8
				return 7
			case 1:
				ad := c.b<<8 | c.c
				c.a = c.rd(ad)
				c.wz = (ad + 1) & 0xffff
				return 7
			case 2:
				ad := c.d<<8 | c.e
				c.wr(ad, c.a)
				c.wz = ((ad + 1) & 0xff) | c.a<<8
				return 7
			case 3:
				ad := c.d<<8 | c.e
				c.a = c.rd(ad)
				c.wz = (ad + 1) & 0xffff
				return 7
			case 4:
				ad := c.fetch16()
				v := c.getHL(x)
				c.wr(ad, v)
				c.wr(ad+1, v>>8)
				c.wz = (ad + 1) & 0xffff
				return 16
			case 5:
				ad := c.fetch16()
				c.setHL(x, c.rd(ad)|c.rd((ad+1)&0xffff)<<8)
				c.wz = (ad + 1) & 0xffff
				return 16
			case 6:
				ad := c.fetch16()
				c.wr(ad, c.a)
				c.wz = ((ad + 1) & 0xff) | c.a<<8
				return 13
			default:
				ad := c.fetch16()
				c.a = c.rd(ad)
				c.wz = (ad + 1) & 0xffff
				return 13
			}
		case 3:
			d := 1
			if odd != 0 {
				d = 0xffff
			}
			c.setRP(p, (c.getRP(p, x)+d)&0xffff, x)
			return 6
		case 4:
			if y == 6 {
				ad := c.memAddr(x)
				c.wr(ad, c.inc8(c.rd(ad)))
				if x != 0 {
					return 19
				}
				return 11
			}
			c.setRX(y, c.inc8(c.getRX(y, x)), x)
			return 4
		case 5:
			if y == 6 {
				ad := c.memAddr(x)
				c.wr(ad, c.dec8(c.rd(ad)))
				if x != 0 {
					return 19
				}
				return 11
			}
			c.setRX(y, c.dec8(c.getRX(y, x)), x)
			return 4
		case 6:
			if y == 6 {
				ad := c.memAddr(x)
				c.wr(ad, c.fetch())
				if x != 0 {
					return 15
				}
				return 10
			}
			c.setRX(y, c.fetch(), x)
			return 7
		default:
			f := c.f
			switch y {
			case 0:
				c.a = ((c.a << 1) | (c.a >> 7)) & 0xff
				c.setF((f & (fS | fZ | fP)) | (c.a & (fX | fY | fC)))
			case 1:
				cy := c.a & 1
				c.a = (c.a >> 1) | (cy << 7)
				c.setF((f & (fS | fZ | fP)) | (c.a & (fX | fY)) | cy)
			case 2:
				cy := c.a >> 7
				c.a = ((c.a << 1) | (f & fC)) & 0xff
				c.setF((f & (fS | fZ | fP)) | (c.a & (fX | fY)) | cy)
			case 3:
				cy := c.a & 1
				c.a = (c.a >> 1) | ((f & fC) << 7)
				c.setF((f & (fS | fZ | fP)) | (c.a & (fX | fY)) | cy)
			case 4:
				t, cy := 0, f&fC
				if f&fH != 0 || c.a&0xf > 9 {
					t = 6
				}
				if cy != 0 || c.a > 0x99 {
					t |= 0x60
					cy = 1
				}
				var r int
				if f&fN != 0 {
					r = (c.a - t) & 0xff
				} else {
					r = (c.a + t) & 0xff
				}
				c.setF(szp[r] | cy | (f & fN) | ((c.a ^ r) & fH))
				c.a = r
			case 5:
				c.a ^= 0xff
				c.setF((f & (fS | fZ | fP | fC)) | fH | fN | (c.a & (fX | fY)))
			case 6:
				c.setF((f & (fS | fZ | fP)) | fC | (((c.q ^ f) | c.a) & (fX | fY)))
			default:
				c.setF(((f & (fS | fZ | fP | fC)) | ((f & fC) << 4) | (((c.q ^ f) | c.a) & (fX | fY))) ^ fC)
			}
			return 4
		}
	case 1:
		if op == 0x76 {
			c.halted = true
			return 4
		}
		if z == 6 {
			c.setR(y, c.rd(c.memAddr(x)))
			if x != 0 {
				return 15
			}
			return 7
		}
		if y == 6 {
			ad := c.memAddr(x)
			c.wr(ad, c.getR(z))
			if x != 0 {
				return 15
			}
			return 7
		}
		c.setRX(y, c.getRX(z, x), x)
		return 4
	case 2:
		if z == 6 {
			c.alu(y, c.rd(c.memAddr(x)))
			if x != 0 {
				return 15
			}
			return 7
		}
		c.alu(y, c.getRX(z, x))
		return 4
	}
	// op >> 6 == 3
	switch z {
	case 0:
		if c.cond(y) {
			c.pc = c.pop()
			c.wz = c.pc
			return 11
		}
		return 5
	case 1:
		if odd == 0 {
			v := c.pop()
			if p == 3 {
				c.a = v >> 8
				c.f = v & 0xff
			} else {
				c.setRP(p, v, x)
			}
			return 10
		}
		switch p {
		case 0:
			c.pc = c.pop()
			c.wz = c.pc
			return 10
		case 1:
			t := c.b<<8 | c.c
			c.b = c.bc2 >> 8
			c.c = c.bc2 & 0xff
			c.bc2 = t
			t = c.d<<8 | c.e
			c.d = c.de2 >> 8
			c.e = c.de2 & 0xff
			c.de2 = t
			t = c.h<<8 | c.l
			c.h = c.hl2 >> 8
			c.l = c.hl2 & 0xff
			c.hl2 = t
			return 4
		case 2:
			c.pc = c.getHL(x)
			return 4
		default:
			c.sp = c.getHL(x)
			return 6
		}
	case 2:
		ad := c.fetch16()
		c.wz = ad
		if c.cond(y) {
			c.pc = ad
		}
		return 10
	case 3:
		switch y {
		case 0:
			c.pc = c.fetch16()
			c.wz = c.pc
			return 10
		case 1:
			if x != 0 {
				return c.execDDCB(x)
			}
			return c.execCB()
		case 2:
			n := c.fetch()
			c.pout(c.a<<8|n, c.a)
			c.wz = ((n + 1) & 0xff) | c.a<<8
			return 11
		case 3:
			port := c.a<<8 | c.fetch()
			c.a = c.pin(port)
			c.wz = (port + 1) & 0xffff
			return 11
		case 4:
			v := c.rd(c.sp) | c.rd((c.sp+1)&0xffff)<<8
			hv := c.getHL(x)
			c.wr(c.sp+1, hv>>8)
			c.wr(c.sp, hv)
			c.setHL(x, v)
			c.wz = v
			return 19
		case 5:
			c.d, c.h = c.h, c.d
			c.e, c.l = c.l, c.e
			return 4
		case 6:
			c.iff1, c.iff2 = 0, 0
			return 4
		default:
			c.iff1, c.iff2 = 1, 1
			c.eiBlock = true
			return 4
		}
	case 4:
		ad := c.fetch16()
		c.wz = ad
		if c.cond(y) {
			c.push(c.pc)
			c.pc = ad
			return 17
		}
		return 10
	case 5:
		if odd == 0 {
			if p == 3 {
				c.push(c.a<<8 | c.f)
			} else {
				c.push(c.getRP(p, x))
			}
			return 11
		}
		// p == 0: CALL nn (the prefixes are handled in Step)
		ad := c.fetch16()
		c.wz = ad
		c.push(c.pc)
		c.pc = ad
		return 17
	case 6:
		c.alu(y, c.fetch())
		return 7
	}
	c.push(c.pc)
	c.pc = y << 3
	c.wz = c.pc
	return 11
}

func (c *CPU) execCB() int {
	op := c.m1()
	y, z := (op>>3)&7, op&7
	mem := z == 6
	ad, v := 0, 0
	if mem {
		ad = c.hl()
		v = c.rd(ad)
	} else {
		v = c.getR(z)
	}
	var r int
	switch op >> 6 {
	case 0:
		r = c.rot(y, v)
	case 1:
		if mem {
			c.bit(y, v, c.wz>>8)
			return 12
		}
		c.bit(y, v, v)
		return 8
	case 2:
		r = v &^ (1 << y)
	default:
		r = v | (1 << y)
	}
	if mem {
		c.wr(ad, r)
		return 15
	}
	c.setR(z, r)
	return 8
}

func (c *CPU) execDDCB(x int) int {
	base := c.iy
	if x == 1 {
		base = c.ix
	}
	ad := (base + sx(c.fetch())) & 0xffff
	c.wz = ad
	op := c.fetch()
	y, z := (op>>3)&7, op&7
	v := c.rd(ad)
	var r int
	switch op >> 6 {
	case 0:
		r = c.rot(y, v)
	case 1:
		c.bit(y, v, ad>>8)
		return 16
	case 2:
		r = v &^ (1 << y)
	default:
		r = v | (1 << y)
	}
	c.wr(ad, r)
	if z != 6 {
		c.setR(z, r)
	}
	return 19
}

func (c *CPU) blockIOFlags(v, k int) {
	fl := sz[c.b] | (szp[(k&7)^c.b] & fP)
	if v&0x80 != 0 {
		fl |= fN
	}
	if k > 0xff {
		fl |= fH | fC
	}
	c.setF(fl)
}

func (c *CPU) repeatFlags() { c.f = (c.f &^ (fX | fY)) | ((c.pc >> 8) & (fX | fY)) }

func (c *CPU) repeatIOFlags(v int) {
	c.repeatFlags()
	if c.f&fC != 0 {
		c.f &^= fH
		if v&0x80 != 0 {
			c.f ^= (szp[(c.b-1)&7] ^ fP) & fP
			if c.b&0xf == 0 {
				c.f |= fH
			}
		} else {
			c.f ^= (szp[(c.b+1)&7] ^ fP) & fP
			if c.b&0xf == 0xf {
				c.f |= fH
			}
		}
	} else {
		c.f ^= (szp[c.b&7] ^ fP) & fP
	}
}

func (c *CPU) execED() int {
	op := c.m1()
	y, z := (op>>3)&7, op&7
	p, odd := y>>1, y&1
	if op>>6 == 1 {
		switch z {
		case 0:
			port := c.b<<8 | c.c
			v := c.pin(port)
			c.wz = (port + 1) & 0xffff
			c.setF((c.f & fC) | szp[v])
			if y != 6 {
				c.setR(y, v)
			}
			return 12
		case 1:
			port := c.b<<8 | c.c
			v := 0
			if y != 6 {
				v = c.getR(y)
			}
			c.pout(port, v)
			c.wz = (port + 1) & 0xffff
			return 12
		case 2:
			if odd != 0 {
				c.adcHL(c.getRP(p, 0))
			} else {
				c.sbcHL(c.getRP(p, 0))
			}
			return 15
		case 3:
			ad := c.fetch16()
			if odd != 0 {
				c.setRP(p, c.rd(ad)|c.rd((ad+1)&0xffff)<<8, 0)
			} else {
				v := c.getRP(p, 0)
				c.wr(ad, v)
				c.wr(ad+1, v>>8)
			}
			c.wz = (ad + 1) & 0xffff
			return 20
		case 4:
			v := c.a
			c.a = 0
			c.a = c.sub8(v, 0)
			return 8
		case 5:
			c.iff1 = c.iff2
			c.pc = c.pop()
			c.wz = c.pc
			return 14
		case 6:
			c.im = imMode[y]
			return 8
		}
		switch y {
		case 0:
			c.iv = c.a
			return 9
		case 1:
			c.rv = c.a
			return 9
		case 2, 3:
			if y == 2 {
				c.a = c.iv
			} else {
				c.a = c.rv
			}
			fl := (c.f & fC) | sz[c.a]
			if c.iff2 != 0 {
				fl |= fP
			}
			c.setF(fl)
			return 9
		case 4:
			ad := c.hl()
			v := c.rd(ad)
			c.wr(ad, (c.a<<4)|(v>>4))
			c.a = (c.a & 0xf0) | (v & 0xf)
			c.setF((c.f & fC) | szp[c.a])
			c.wz = (ad + 1) & 0xffff
			return 18
		case 5:
			ad := c.hl()
			v := c.rd(ad)
			c.wr(ad, (v<<4)|(c.a&0xf))
			c.a = (c.a & 0xf0) | (v >> 4)
			c.setF((c.f & fC) | szp[c.a])
			c.wz = (ad + 1) & 0xffff
			return 18
		}
		return 8
	}
	if op>>6 == 2 && y >= 4 && z <= 3 {
		rep := y >= 6
		step := 1
		if odd != 0 {
			step = 0xffff
		}
		hl := c.hl()
		switch z {
		case 0: // LDI/LDD/LDIR/LDDR
			v := c.rd(hl)
			de := c.d<<8 | c.e
			c.wr(de, v)
			hl = (hl + step) & 0xffff
			c.setHLreg(hl)
			de = (de + step) & 0xffff
			c.d, c.e = de>>8, de&0xff
			bc := ((c.b<<8 | c.c) - 1) & 0xffff
			c.b, c.c = bc>>8, bc&0xff
			n := (v + c.a) & 0xff
			fl := (c.f & (fS | fZ | fC)) | (n & fX) | ((n & 2) << 4)
			if bc != 0 {
				fl |= fP
			}
			c.setF(fl)
			if rep && bc != 0 {
				c.pc = (c.pc - 2) & 0xffff
				c.wz = (c.pc + 1) & 0xffff
				c.repeatFlags()
				return 21
			}
			return 16
		case 1: // CPI/CPD/CPIR/CPDR
			v := c.rd(hl)
			r := (c.a - v) & 0xff
			hl = (hl + step) & 0xffff
			c.setHLreg(hl)
			bc := ((c.b<<8 | c.c) - 1) & 0xffff
			c.b, c.c = bc>>8, bc&0xff
			hf := (c.a ^ v ^ r) & fH
			n := r
			if hf != 0 {
				n = (r - 1) & 0xff
			}
			fl := (c.f & fC) | fN | (sz[r] & (fS | fZ)) | hf | (n & fX) | ((n & 2) << 4)
			if bc != 0 {
				fl |= fP
			}
			c.setF(fl)
			c.wz = (c.wz + step) & 0xffff
			if rep && bc != 0 && r != 0 {
				c.pc = (c.pc - 2) & 0xffff
				c.wz = (c.pc + 1) & 0xffff
				c.repeatFlags()
				return 21
			}
			return 16
		case 2: // INI/IND/INIR/INDR
			port := c.b<<8 | c.c
			v := c.pin(port)
			c.wz = (port + step) & 0xffff
			c.wr(hl, v)
			c.b = (c.b - 1) & 0xff
			hl = (hl + step) & 0xffff
			c.setHLreg(hl)
			d := 1
			if odd != 0 {
				d = -1
			}
			c.blockIOFlags(v, v+((c.c+d)&0xff))
			if rep && c.b != 0 {
				c.pc = (c.pc - 2) & 0xffff
				c.wz = (c.pc + 1) & 0xffff
				c.repeatIOFlags(v)
				return 21
			}
			return 16
		default: // OUTI/OUTD/OTIR/OTDR
			v := c.rd(hl)
			c.b = (c.b - 1) & 0xff
			port := c.b<<8 | c.c
			c.wz = (port + step) & 0xffff
			c.pout(port, v)
			hl = (hl + step) & 0xffff
			c.setHLreg(hl)
			c.blockIOFlags(v, v+c.l)
			if rep && c.b != 0 {
				c.pc = (c.pc - 2) & 0xffff
				c.wz = (c.pc + 1) & 0xffff
				c.repeatIOFlags(v)
				return 21
			}
			return 16
		}
	}
	return 8
}

// Step runs one instruction, or starts an interrupt, and returns the
// T-states it took.
func (c *CPU) Step() int {
	if c.nmiPending {
		c.nmiPending = false
		c.halted = false
		c.incR()
		c.iff1 = 0
		c.push(c.pc)
		c.pc = 0x66
		c.wz = c.pc
		c.q = 0
		return 11
	}
	if c.IRQ && c.iff1 != 0 && !c.eiBlock {
		c.halted = false
		c.iff1, c.iff2 = 0, 0
		if c.ack != nil {
			c.ack.Ack()
		}
		c.incR()
		c.push(c.pc)
		c.q = 0
		if c.im == 2 {
			v := 0xff
			if c.vec != nil {
				v = int(c.vec.Vector())
			}
			vec := c.iv<<8 | v
			c.pc = c.rd(vec) | c.rd((vec+1)&0xffff)<<8
			c.wz = c.pc
			return 19
		}
		c.pc = 0x38
		c.wz = c.pc
		return 13
	}
	c.eiBlock = false
	if c.halted {
		c.incR()
		c.q = 0
		return 4
	}
	c.qn = 0
	op, t, x := c.m1(), 0, 0
	for op == 0xdd || op == 0xfd {
		if op == 0xdd {
			x = 1
		} else {
			x = 2
		}
		t += 4
		c.q = 0
		op = c.m1()
	}
	if op == 0xed {
		t += c.execED()
	} else {
		t += c.exec(op, x)
	}
	if c.qn != 0 {
		c.q = c.f
	} else {
		c.q = 0
	}
	return t
}

// Regs is the processor's state, in the same order as the JS core's
// getState / setState.
type Regs struct {
	A, F, B, C, D, E, H, L uint8
	IX, IY, SP, PC         uint16
	I, R                   uint8
	WZ                     uint16
	AF_, BC_, DE_, HL_     uint16
	IFF1, IFF2             bool
	IM                     uint8
	Halted                 bool
	EI                     bool // an EI just ran: interrupts wait one more instruction
	Q                      uint8
	NMIPending             bool
	IRQ                    bool
}

// Regs returns the processor's state.
func (c *CPU) Regs() Regs {
	return Regs{
		A: uint8(c.a), F: uint8(c.f), B: uint8(c.b), C: uint8(c.c), D: uint8(c.d), E: uint8(c.e), H: uint8(c.h), L: uint8(c.l),
		IX: uint16(c.ix), IY: uint16(c.iy), SP: uint16(c.sp), PC: uint16(c.pc),
		I: uint8(c.iv), R: uint8(c.rv), WZ: uint16(c.wz),
		AF_: uint16(c.af2), BC_: uint16(c.bc2), DE_: uint16(c.de2), HL_: uint16(c.hl2),
		IFF1: c.iff1 != 0, IFF2: c.iff2 != 0, IM: uint8(c.im),
		Halted: c.halted, EI: c.eiBlock, Q: uint8(c.q), NMIPending: c.nmiPending, IRQ: c.IRQ,
	}
}

func b2i(v bool) int {
	if v {
		return 1
	}
	return 0
}

// SetRegs sets the processor's state.
func (c *CPU) SetRegs(r Regs) {
	c.a, c.f, c.b, c.c, c.d, c.e, c.h, c.l = int(r.A), int(r.F), int(r.B), int(r.C), int(r.D), int(r.E), int(r.H), int(r.L)
	c.ix, c.iy, c.sp, c.pc = int(r.IX), int(r.IY), int(r.SP), int(r.PC)
	c.iv, c.rv, c.wz = int(r.I), int(r.R), int(r.WZ)
	c.af2, c.bc2, c.de2, c.hl2 = int(r.AF_), int(r.BC_), int(r.DE_), int(r.HL_)
	c.iff1, c.iff2, c.im = b2i(r.IFF1), b2i(r.IFF2), int(r.IM)
	if c.im > 2 {
		c.im = 2
	}
	c.halted, c.eiBlock, c.q, c.nmiPending, c.IRQ = r.Halted, r.EI, int(r.Q), r.NMIPending, r.IRQ
}

// StateSize is the length of a saved processor state.
const StateSize = 36

var errState = errors.New("z80: damaged save state")

// AppendState appends the processor's state (StateSize bytes) to b.
func (c *CPU) AppendState(b []byte) []byte {
	r := c.Regs()
	b = append(b, 1, r.A, r.F, r.B, r.C, r.D, r.E, r.H, r.L)
	for _, v := range []uint16{r.IX, r.IY, r.SP, r.PC, r.WZ, r.AF_, r.BC_, r.DE_, r.HL_} {
		b = binary.LittleEndian.AppendUint16(b, v)
	}
	var fl uint8
	for i, v := range []bool{r.IFF1, r.IFF2, r.Halted, r.EI, r.NMIPending, r.IRQ} {
		if v {
			fl |= 1 << i
		}
	}
	return append(b, r.I, r.R, r.IM, r.Q, fl, 0, 0, 0, 0)
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != 1 {
		return b, errState
	}
	var r Regs
	r.A, r.F, r.B, r.C, r.D, r.E, r.H, r.L = b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]
	w := make([]uint16, 9)
	for i := range w {
		w[i] = binary.LittleEndian.Uint16(b[9+i*2:])
	}
	r.IX, r.IY, r.SP, r.PC, r.WZ, r.AF_, r.BC_, r.DE_, r.HL_ = w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]
	r.I, r.R, r.IM, r.Q = b[27], b[28], b[29], b[30]
	fl := b[31]
	r.IFF1, r.IFF2, r.Halted, r.EI, r.NMIPending, r.IRQ = fl&1 != 0, fl&2 != 0, fl&4 != 0, fl&8 != 0, fl&16 != 0, fl&32 != 0
	if r.IM > 2 {
		return b, errState
	}
	c.SetRegs(r)
	return b[StateSize:], nil
}
