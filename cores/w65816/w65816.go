// Package w65816 is the WDC 65C816 processor (the Super Nintendo's main CPU,
// and the SA-1 chip's): a Go port of web/emu/w65816.js that behaves exactly
// like it, instruction for instruction and bus access for bus access.
package w65816

// Bus is the processor's memory. Timing is counted by the bus: every Read and
// Write is one memory cycle, Idle one internal cycle.
type Bus interface {
	Read(addr uint32) uint8 // addr is 24 bits
	Write(addr uint32, v uint8)
	Idle()
}

// CPU is one 65C816.
type CPU struct {
	bus Bus

	a, x, y, s, d, db, pb, pc      int
	fn, fv, fm, fx, fd, fi, fz, fc int
	e                              int
	waiting, stopped               bool

	NMIPending bool
	IRQLine    bool
}

// New makes a processor on bus (call Reset before running it).
func New(bus Bus) *CPU {
	return &CPU{bus: bus, s: 0x1ff, fm: 1, fx: 1, fi: 1, e: 1}
}

const b0 = 0x1000000 // "bank 0" marker on effective addresses (direct page / stack)

func (c *CPU) rd(ad int) int    { return int(c.bus.Read(uint32(ad & 0xffffff))) }
func (c *CPU) wr(ad int, v int) { c.bus.Write(uint32(ad&0xffffff), uint8(v)) }
func (c *CPU) io()              { c.bus.Idle() }
func (c *CPU) fetch() int       { v := c.rd(c.pb<<16 | c.pc); c.pc = (c.pc + 1) & 0xffff; return v }
func (c *CPU) fetch16() int     { l := c.fetch(); return l | c.fetch()<<8 }
func (c *CPU) fetch24() int     { l := c.fetch16(); return l | c.fetch()<<16 }
func (c *CPU) getP() int {
	return c.fn<<7 | c.fv<<6 | c.fm<<5 | c.fx<<4 | c.fd<<3 | c.fi<<2 | c.fz<<1 | c.fc
}

func (c *CPU) setP(v int) {
	c.fn, c.fv, c.fd, c.fi, c.fz, c.fc = v>>7&1, v>>6&1, v>>3&1, v>>2&1, v>>1&1, v&1
	if c.e != 0 {
		c.fm, c.fx = 1, 1
	} else {
		c.fm, c.fx = v>>5&1, v>>4&1
	}
	if c.fx != 0 {
		c.x &= 0xff
		c.y &= 0xff
	}
}

// ---- stack ----

func (c *CPU) push(v int) {
	c.wr(c.s, v)
	if c.e != 0 {
		c.s = 0x100 | (c.s-1)&0xff
	} else {
		c.s = (c.s - 1) & 0xffff
	}
}

func (c *CPU) pull() int {
	if c.e != 0 {
		c.s = 0x100 | (c.s+1)&0xff
	} else {
		c.s = (c.s + 1) & 0xffff
	}
	return c.rd(c.s)
}

// "new" instructions use the full 16-bit stack pointer even in emulation mode
func (c *CPU) pushN(v int) { c.wr(c.s, v); c.s = (c.s - 1) & 0xffff }
func (c *CPU) pullN() int  { c.s = (c.s + 1) & 0xffff; return c.rd(c.s) }
func (c *CPU) fixS() {
	if c.e != 0 {
		c.s = 0x100 | c.s&0xff
	}
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func (c *CPU) nz8(v int)  { c.fn = v >> 7 & 1; c.fz = b2i(v&0xff == 0) }
func (c *CPU) nz16(v int) { c.fn = v >> 15 & 1; c.fz = b2i(v&0xffff == 0) }
func (c *CPU) nzM(v int) {
	if c.fm != 0 {
		c.nz8(v)
	} else {
		c.nz16(v)
	}
}
func (c *CPU) nzX(v int) {
	if c.fx != 0 {
		c.nz8(v)
	} else {
		c.nz16(v)
	}
}

// ---- addressing ----

func (c *CPU) dpBase(off int) int {
	if c.d&0xff != 0 {
		c.io()
	}
	return (c.d + off) & 0xffff
}

func (c *CPU) dpIdx(off, idx int) int {
	if c.d&0xff != 0 {
		c.io()
	}
	c.io()
	if c.e != 0 && c.d&0xff == 0 {
		return c.d&0xff00 | (off+idx)&0xff
	}
	return (c.d + off + idx) & 0xffff
}

func (c *CPU) dpPtr(ad int) int {
	lo := c.rd(ad)
	var hiAd int
	if c.e != 0 && c.d&0xff == 0 {
		hiAd = ad&0xff00 | (ad+1)&0xff
	} else {
		hiAd = (ad + 1) & 0xffff
	}
	return lo | c.rd(hiAd)<<8
}

func (c *CPU) dpPtrLong(ad int) int {
	return c.rd(ad) | c.rd((ad+1)&0xffff)<<8 | c.rd((ad+2)&0xffff)<<16
}

func (c *CPU) rdW(ad int, w8 int) int {
	l := c.rd(ad)
	if w8 != 0 {
		return l
	}
	return l | c.rd((ad+1)&0xffffff)<<8
}

func (c *CPU) wrW(ad, v, w8 int) {
	c.wr(ad, v)
	if w8 == 0 {
		c.wr((ad+1)&0xffffff, v>>8)
	}
}

func (c *CPU) rdW0(ad int, w8 int) int {
	l := c.rd(ad)
	if w8 != 0 {
		return l
	}
	return l | c.rd((ad+1)&0xffff)<<8
}

func (c *CPU) wrW0(ad, v, w8 int) {
	c.wr(ad, v)
	if w8 == 0 {
		c.wr((ad+1)&0xffff, v>>8)
	}
}

// addr returns the effective address of an operand (kind: 0 read, 1 write,
// 2 read-modify-write), with b0 set for direct page / stack addresses.
func (c *CPU) addr(mode, kind int) int {
	switch mode {
	case 0: // dp
		return b0 | c.dpBase(c.fetch())
	case 1: // dp,X
		return b0 | c.dpIdx(c.fetch(), c.x)
	case 2: // dp,Y
		return b0 | c.dpIdx(c.fetch(), c.y)
	case 3: // abs
		return c.db<<16 | c.fetch16()
	case 4: // abs,X
		b := c.fetch16()
		ad := ((c.db<<16 | b) + c.x) & 0xffffff
		if kind != 0 || c.fx == 0 || b&0xff00 != (b+c.x)&0xff00 {
			c.io()
		}
		return ad
	case 5: // abs,Y
		b := c.fetch16()
		ad := ((c.db<<16 | b) + c.y) & 0xffffff
		if kind != 0 || c.fx == 0 || b&0xff00 != (b+c.y)&0xff00 {
			c.io()
		}
		return ad
	case 6: // long
		return c.fetch24()
	case 7: // long,X
		return (c.fetch24() + c.x) & 0xffffff
	case 8: // (dp)
		p := c.dpBase(c.fetch())
		return c.db<<16 | c.dpPtr(p)
	case 9: // (dp,X)
		p := c.dpIdx(c.fetch(), c.x)
		return c.db<<16 | c.dpPtr(p)
	case 10: // (dp),Y
		p := c.dpBase(c.fetch())
		b := c.dpPtr(p)
		ad := ((c.db<<16 | b) + c.y) & 0xffffff
		if kind != 0 || c.fx == 0 || b&0xff00 != (b+c.y)&0xff00 {
			c.io()
		}
		return ad
	case 11: // [dp]
		p := c.dpBase(c.fetch())
		return c.dpPtrLong(p)
	case 12: // [dp],Y
		p := c.dpBase(c.fetch())
		return (c.dpPtrLong(p) + c.y) & 0xffffff
	case 13: // sr,S
		o := c.fetch()
		c.io()
		return b0 | (c.s+o)&0xffff
	}
	// (sr,S),Y
	o := c.fetch()
	c.io()
	p := (c.s + o) & 0xffff
	b := c.rd(p) | c.rd((p+1)&0xffff)<<8
	c.io()
	return ((c.db<<16 | b) + c.y) & 0xffffff
}

func (c *CPU) rdOp(ea, w8 int) int {
	if ea&b0 != 0 {
		return c.rdW0(ea&0xffffff, w8)
	}
	return c.rdW(ea, w8)
}

func (c *CPU) wrOp(ea, v, w8 int) {
	if ea&b0 != 0 {
		c.wrW0(ea&0xffffff, v, w8)
	} else {
		c.wrW(ea, v, w8)
	}
}

// ---- ALU ----

func (c *CPU) adc(v int) {
	w8 := c.fm
	var r int
	acc := c.a
	if w8 != 0 {
		acc = c.a & 0xff
	}
	if c.fd == 0 {
		r = acc + v + c.fc
		if w8 != 0 {
			c.fv = b2i(^(acc^v)&(acc^r)&0x80 != 0)
			c.fc = b2i(r > 0xff)
			r &= 0xff
		} else {
			c.fv = b2i(^(acc^v)&(acc^r)&0x8000 != 0)
			c.fc = b2i(r > 0xffff)
			r &= 0xffff
		}
	} else {
		cy, res := c.fc, 0
		digits := 4
		if w8 != 0 {
			digits = 2
		}
		binNoCarry := 0
		for k := 0; k < digits; k++ {
			t := (acc>>(k*4))&15 + (v>>(k*4))&15 + cy
			if k == digits-1 {
				binNoCarry = res | (t&15)<<(k*4)
			}
			cy = 0
			if t > 9 {
				t += 6
				cy = 1
			}
			res |= (t & 15) << (k * 4)
		}
		top := 0x8000
		if w8 != 0 {
			top = 0x80
		}
		c.fv = b2i(^(acc^v)&(acc^binNoCarry)&top != 0)
		c.fc = cy
		r = res
	}
	if w8 != 0 {
		c.a = c.a&0xff00 | r
		c.nz8(r)
	} else {
		c.a = r
		c.nz16(r)
	}
}

func (c *CPU) sbc(v int) {
	w8 := c.fm
	if c.fd == 0 {
		if w8 != 0 {
			c.adc(v ^ 0xff)
		} else {
			c.adc(v ^ 0xffff)
		}
		return
	}
	acc, digits, nv, top := c.a, 4, v^0xffff, 0x8000
	if w8 != 0 {
		acc, digits, nv, top = c.a&0xff, 2, v^0xff, 0x80
	}
	cy, res, binNoBorrow := c.fc, 0, 0
	for k := 0; k < digits; k++ {
		t := (acc>>(k*4))&15 + (nv>>(k*4))&15 + cy
		if k == digits-1 {
			binNoBorrow = res | (t&15)<<(k*4)
		}
		cy = b2i(t > 15)
		if cy == 0 {
			t -= 6
		}
		res |= (t & 15) << (k * 4)
	}
	c.fv = b2i(^(acc^nv)&(acc^binNoBorrow)&top != 0)
	c.fc = cy
	if w8 != 0 {
		c.a = c.a&0xff00 | res&0xff
		c.nz8(res)
	} else {
		c.a = res & 0xffff
		c.nz16(res)
	}
}

func (c *CPU) cmp(reg, v, w8 int) {
	if w8 != 0 {
		reg &= 0xff
	}
	r := reg - v
	c.fc = b2i(r >= 0)
	if w8 != 0 {
		c.nz8(r)
	} else {
		c.nz16(r)
	}
}

func (c *CPU) setA(v int) {
	if c.fm != 0 {
		c.a = c.a&0xff00 | v&0xff
	} else {
		c.a = v & 0xffff
	}
}

func (c *CPU) accM() int {
	if c.fm != 0 {
		return c.a & 0xff
	}
	return c.a
}

func (c *CPU) aluOp(g, v int) {
	switch g {
	case 0:
		c.setA(c.accM() | v)
		c.nzM(c.accM())
	case 1:
		c.setA(c.accM() & v)
		c.nzM(c.accM())
	case 2:
		c.setA(c.accM() ^ v)
		c.nzM(c.accM())
	case 3:
		c.adc(v)
	case 5:
		c.setA(v)
		c.nzM(v)
	case 6:
		c.cmp(c.a, v, c.fm)
	case 7:
		c.sbc(v)
	}
}

func (c *CPU) rmw(op, v, w8 int) int {
	top, mask := 0x8000, 0xffff
	if w8 != 0 {
		top, mask = 0x80, 0xff
	}
	var r int
	switch op {
	case 0: // ASL
		c.fc = b2i(v&top != 0)
		r = (v << 1) & mask
	case 1: // ROL
		r = (v<<1 | c.fc) & mask
		c.fc = b2i(v&top != 0)
	case 2: // LSR
		c.fc = v & 1
		r = v >> 1
	case 3: // ROR
		r = v >> 1
		if c.fc != 0 {
			r |= top
		}
		c.fc = v & 1
	case 4: // DEC
		r = (v - 1) & mask
	default: // INC
		r = (v + 1) & mask
	}
	if w8 != 0 {
		c.nz8(r)
	} else {
		c.nz16(r)
	}
	return r
}

func (c *CPU) branch(cond bool) {
	o := c.fetch()
	if !cond {
		return
	}
	c.io()
	np := (c.pc + int(int8(o))) & 0xffff
	if c.e != 0 && np&0xff00 != c.pc&0xff00 {
		c.io()
	}
	c.pc = np
}

func (c *CPU) interrupt(vecNative, vecEmu int, isBrk bool) {
	if c.e == 0 {
		c.push(c.pb)
	}
	c.push(c.pc >> 8)
	c.push(c.pc & 0xff)
	if c.e != 0 {
		p := c.getP()&^0x10 | 0x20
		if isBrk {
			p |= 0x10
		}
		c.push(p)
	} else {
		c.push(c.getP())
	}
	c.fi, c.fd, c.pb = 1, 0, 0
	v := vecNative
	if c.e != 0 {
		v = vecEmu
	}
	c.pc = c.rd(v) | c.rd(v+1)<<8
}

// operand modes of the eight "group one" instructions, by the low five opcode bits
var g1mode = func() (t [32]int8) {
	for i := range t {
		t[i] = -1
	}
	for _, p := range [][2]int{{0x01, 9}, {0x03, 13}, {0x05, 0}, {0x07, 11}, {0x0d, 3}, {0x0f, 6}, {0x11, 10}, {0x12, 8}, {0x13, 14}, {0x15, 1}, {0x17, 12}, {0x19, 5}, {0x1d, 4}, {0x1f, 7}} {
		t[p[0]] = int8(p[1])
	}
	return
}()

var rmwKind = [8]int{0, 1, 2, 3, -1, -1, 4, 5}
var mode0314 = [4]int{0, 3, 1, 4}

// a 16-bit read-modify-write result goes high byte first
func (c *CPU) rmwWrite(ea, r int) {
	if c.fm != 0 {
		c.wrOp(ea, r, 1)
		return
	}
	base := ea & 0xffffff
	var hiA int
	if ea&b0 != 0 {
		hiA = (base + 1) & 0xffff
	} else {
		hiA = (base + 1) & 0xffffff
	}
	c.wr(hiA, r>>8)
	c.wr(base, r)
}

func (c *CPU) immX() int {
	if c.fx != 0 {
		return c.fetch()
	}
	return c.fetch16()
}

func (c *CPU) maskX() int {
	if c.fx != 0 {
		return 0xff
	}
	return 0xffff
}

func (c *CPU) exec(op int) {
	low, g := op&0x1f, op>>5
	if m := g1mode[low]; m >= 0 && op != 0x89 {
		if g == 4 { // STA
			ea := c.addr(int(m), 1)
			c.wrOp(ea, c.a, c.fm)
			return
		}
		ea := c.addr(int(m), 0)
		c.aluOp(g, c.rdOp(ea, c.fm))
		return
	}
	if low == 0x09 && op != 0x89 {
		var v int
		if c.fm != 0 {
			v = c.fetch()
		} else {
			v = c.fetch16()
		}
		c.aluOp(g, v)
		return
	}
	switch op {
	// ---- read-modify-write ----
	case 0x06, 0x26, 0x46, 0x66, 0xc6, 0xe6,
		0x0e, 0x2e, 0x4e, 0x6e, 0xce, 0xee,
		0x16, 0x36, 0x56, 0x76, 0xd6, 0xf6,
		0x1e, 0x3e, 0x5e, 0x7e, 0xde, 0xfe:
		kind := rmwKind[op>>5]
		ea := c.addr(mode0314[(op>>3)&3], 2)
		v := c.rdOp(ea, c.fm)
		c.io()
		c.rmwWrite(ea, c.rmw(kind, v, c.fm))
	case 0x0a, 0x2a, 0x4a, 0x6a, 0x3a, 0x1a:
		c.io()
		var kind int
		switch op {
		case 0x0a:
			kind = 0
		case 0x2a:
			kind = 1
		case 0x4a:
			kind = 2
		case 0x6a:
			kind = 3
		case 0x3a:
			kind = 4
		default:
			kind = 5
		}
		c.setA(c.rmw(kind, c.accM(), c.fm))
	case 0x04, 0x0c, 0x14, 0x1c: // TSB / TRB
		m := 0
		if op&8 != 0 {
			m = 3
		}
		ea := c.addr(m, 2)
		v := c.rdOp(ea, c.fm)
		c.io()
		acc := c.accM()
		c.fz = b2i(v&acc == 0)
		var r int
		if op&0x10 != 0 {
			r = v &^ acc
		} else {
			r = v | acc
		}
		c.rmwWrite(ea, r)
	// ---- BIT ----
	case 0x89:
		var v int
		if c.fm != 0 {
			v = c.fetch()
		} else {
			v = c.fetch16()
		}
		c.fz = b2i(c.accM()&v == 0)
	case 0x24, 0x2c, 0x34, 0x3c:
		ea := c.addr(mode0314[(op>>3)&3], 0)
		v := c.rdOp(ea, c.fm)
		c.fz = b2i(c.accM()&v == 0)
		if c.fm != 0 {
			c.fn, c.fv = v>>7&1, v>>6&1
		} else {
			c.fn, c.fv = v>>15&1, v>>14&1
		}
	// ---- index loads / stores / compares ----
	case 0xa2:
		c.x = c.immX()
		c.nzX(c.x)
	case 0xa0:
		c.y = c.immX()
		c.nzX(c.y)
	case 0xa6, 0xae, 0xb6, 0xbe:
		ea := c.addr([4]int{0, 3, 2, 5}[(op>>3)&3], 0)
		c.x = c.rdOp(ea, c.fx)
		c.nzX(c.x)
	case 0xa4, 0xac, 0xb4, 0xbc:
		ea := c.addr(mode0314[(op>>3)&3], 0)
		c.y = c.rdOp(ea, c.fx)
		c.nzX(c.y)
	case 0x86, 0x8e, 0x96:
		m := 2
		if op == 0x86 {
			m = 0
		} else if op == 0x8e {
			m = 3
		}
		ea := c.addr(m, 1)
		c.wrOp(ea, c.x, c.fx)
	case 0x84, 0x8c, 0x94:
		m := 1
		if op == 0x84 {
			m = 0
		} else if op == 0x8c {
			m = 3
		}
		ea := c.addr(m, 1)
		c.wrOp(ea, c.y, c.fx)
	case 0x64, 0x74, 0x9c, 0x9e:
		var m int
		switch op {
		case 0x64:
			m = 0
		case 0x74:
			m = 1
		case 0x9c:
			m = 3
		default:
			m = 4
		}
		ea := c.addr(m, 1)
		c.wrOp(ea, 0, c.fm)
	case 0xe0:
		c.cmp(c.x, c.immX(), c.fx)
	case 0xc0:
		c.cmp(c.y, c.immX(), c.fx)
	case 0xe4, 0xec:
		m := 3
		if op == 0xe4 {
			m = 0
		}
		ea := c.addr(m, 0)
		c.cmp(c.x, c.rdOp(ea, c.fx), c.fx)
	case 0xc4, 0xcc:
		m := 3
		if op == 0xc4 {
			m = 0
		}
		ea := c.addr(m, 0)
		c.cmp(c.y, c.rdOp(ea, c.fx), c.fx)
	// ---- inc/dec index ----
	case 0xe8:
		c.io()
		c.x = (c.x + 1) & c.maskX()
		c.nzX(c.x)
	case 0xc8:
		c.io()
		c.y = (c.y + 1) & c.maskX()
		c.nzX(c.y)
	case 0xca:
		c.io()
		c.x = (c.x - 1) & c.maskX()
		c.nzX(c.x)
	case 0x88:
		c.io()
		c.y = (c.y - 1) & c.maskX()
		c.nzX(c.y)
	// ---- branches ----
	case 0x10:
		c.branch(c.fn == 0)
	case 0x30:
		c.branch(c.fn != 0)
	case 0x50:
		c.branch(c.fv == 0)
	case 0x70:
		c.branch(c.fv != 0)
	case 0x90:
		c.branch(c.fc == 0)
	case 0xb0:
		c.branch(c.fc != 0)
	case 0xd0:
		c.branch(c.fz == 0)
	case 0xf0:
		c.branch(c.fz != 0)
	case 0x80:
		c.branch(true)
	case 0x82:
		o := c.fetch16()
		c.io()
		c.pc = (c.pc + o) & 0xffff
	// ---- jumps / calls ----
	case 0x4c:
		c.pc = c.fetch16()
	case 0x5c:
		t := c.fetch24()
		c.pc = t & 0xffff
		c.pb = t >> 16
	case 0x6c:
		p := c.fetch16()
		c.pc = c.rd(p) | c.rd((p+1)&0xffff)<<8
	case 0x7c:
		p := c.fetch16()
		c.io()
		q := c.pb<<16 | (p+c.x)&0xffff
		c.pc = c.rd(q) | c.rd(c.pb<<16|(p+c.x+1)&0xffff)<<8
	case 0xdc:
		p := c.fetch16()
		c.pc = c.rd(p) | c.rd((p+1)&0xffff)<<8
		c.pb = c.rd((p + 2) & 0xffff)
	case 0x20:
		t := c.fetch16()
		c.io()
		r := (c.pc - 1) & 0xffff
		c.push(r >> 8)
		c.push(r & 0xff)
		c.pc = t
	case 0xfc:
		l := c.fetch()
		r := c.pc
		c.pushN(r >> 8)
		c.pushN(r & 0xff)
		h := c.fetch()
		c.io()
		p := l | h<<8
		q := (p + c.x) & 0xffff
		c.pc = c.rd(c.pb<<16|q) | c.rd(c.pb<<16|(q+1)&0xffff)<<8
		c.fixS()
	case 0x22:
		l := c.fetch16()
		c.pushN(c.pb)
		c.io()
		b := c.fetch()
		r := (c.pc - 1) & 0xffff
		c.pushN(r >> 8)
		c.pushN(r & 0xff)
		c.pc = l
		c.pb = b
		c.fixS()
	case 0x60:
		c.io()
		c.io()
		l := c.pull()
		c.pc = ((l | c.pull()<<8) + 1) & 0xffff
		c.io()
	case 0x6b:
		c.io()
		c.io()
		l := c.pullN()
		h := c.pullN()
		c.pb = c.pullN()
		c.pc = ((l | h<<8) + 1) & 0xffff
		c.fixS()
	case 0x40:
		c.io()
		c.io()
		c.setP(c.pull())
		l := c.pull()
		c.pc = l | c.pull()<<8
		if c.e == 0 {
			c.pb = c.pull()
		}
	// ---- stack ----
	case 0x48:
		c.io()
		if c.fm != 0 {
			c.push(c.a & 0xff)
		} else {
			c.push(c.a >> 8)
			c.push(c.a & 0xff)
		}
	case 0xda:
		c.io()
		if c.fx != 0 {
			c.push(c.x & 0xff)
		} else {
			c.push(c.x >> 8)
			c.push(c.x & 0xff)
		}
	case 0x5a:
		c.io()
		if c.fx != 0 {
			c.push(c.y & 0xff)
		} else {
			c.push(c.y >> 8)
			c.push(c.y & 0xff)
		}
	case 0x68:
		c.io()
		c.io()
		if c.fm != 0 {
			v := c.pull()
			c.a = c.a&0xff00 | v
			c.nz8(v)
		} else {
			l := c.pull()
			c.a = l | c.pull()<<8
			c.nz16(c.a)
		}
	case 0xfa:
		c.io()
		c.io()
		if c.fx != 0 {
			c.x = c.pull()
			c.nz8(c.x)
		} else {
			l := c.pull()
			c.x = l | c.pull()<<8
			c.nz16(c.x)
		}
	case 0x7a:
		c.io()
		c.io()
		if c.fx != 0 {
			c.y = c.pull()
			c.nz8(c.y)
		} else {
			l := c.pull()
			c.y = l | c.pull()<<8
			c.nz16(c.y)
		}
	case 0x08:
		c.io()
		if c.e != 0 {
			c.push(c.getP() | 0x30)
		} else {
			c.push(c.getP())
		}
	case 0x28:
		c.io()
		c.io()
		c.setP(c.pull())
	case 0x8b:
		c.io()
		c.push(c.db)
	case 0xab:
		c.io()
		c.io()
		c.db = c.pullN()
		c.nz8(c.db)
		c.fixS()
	case 0x0b:
		c.io()
		c.pushN(c.d >> 8)
		c.pushN(c.d & 0xff)
		c.fixS()
	case 0x2b:
		c.io()
		c.io()
		l := c.pullN()
		c.d = l | c.pullN()<<8
		c.nz16(c.d)
		c.fixS()
	case 0x4b:
		c.io()
		c.push(c.pb)
	case 0xf4:
		v := c.fetch16()
		c.pushN(v >> 8)
		c.pushN(v & 0xff)
		c.fixS()
	case 0xd4:
		p := c.dpBase(c.fetch())
		v := c.rd(p) | c.rd((p+1)&0xffff)<<8
		c.pushN(v >> 8)
		c.pushN(v & 0xff)
		c.fixS()
	case 0x62:
		o := c.fetch16()
		c.io()
		v := (c.pc + o) & 0xffff
		c.pushN(v >> 8)
		c.pushN(v & 0xff)
		c.fixS()
	// ---- transfers ----
	case 0xaa:
		c.io()
		if c.fx != 0 {
			c.x = c.a & 0xff
		} else {
			c.x = c.a
		}
		c.nzX(c.x)
	case 0xa8:
		c.io()
		if c.fx != 0 {
			c.y = c.a & 0xff
		} else {
			c.y = c.a
		}
		c.nzX(c.y)
	case 0x8a:
		c.io()
		if c.fm != 0 {
			c.a = c.a&0xff00 | c.x&0xff
			c.nz8(c.a)
		} else {
			c.a = c.x
			c.nz16(c.a)
		}
	case 0x98:
		c.io()
		if c.fm != 0 {
			c.a = c.a&0xff00 | c.y&0xff
			c.nz8(c.a)
		} else {
			c.a = c.y
			c.nz16(c.a)
		}
	case 0xba:
		c.io()
		if c.fx != 0 {
			c.x = c.s & 0xff
		} else {
			c.x = c.s
		}
		c.nzX(c.x)
	case 0x9a:
		c.io()
		if c.e != 0 {
			c.s = 0x100 | c.x&0xff
		} else {
			c.s = c.x
		}
	case 0x9b:
		c.io()
		c.y = c.x
		c.nzX(c.y)
	case 0xbb:
		c.io()
		c.x = c.y
		c.nzX(c.x)
	case 0x5b:
		c.io()
		c.d = c.a
		c.nz16(c.d)
	case 0x7b:
		c.io()
		c.a = c.d
		c.nz16(c.a)
	case 0x1b:
		c.io()
		if c.e != 0 {
			c.s = 0x100 | c.a&0xff
		} else {
			c.s = c.a
		}
	case 0x3b:
		c.io()
		c.a = c.s
		c.nz16(c.a)
	case 0xeb:
		c.io()
		c.io()
		c.a = (c.a>>8 | c.a<<8) & 0xffff
		c.nz8(c.a & 0xff)
	case 0xfb:
		c.io()
		t := c.fc
		c.fc = c.e
		c.e = t
		if c.e != 0 {
			c.fm, c.fx = 1, 1
			c.x &= 0xff
			c.y &= 0xff
			c.s = 0x100 | c.s&0xff
		}
	// ---- flags ----
	case 0x18:
		c.io()
		c.fc = 0
	case 0x38:
		c.io()
		c.fc = 1
	case 0x58:
		c.io()
		c.fi = 0
	case 0x78:
		c.io()
		c.fi = 1
	case 0xb8:
		c.io()
		c.fv = 0
	case 0xd8:
		c.io()
		c.fd = 0
	case 0xf8:
		c.io()
		c.fd = 1
	case 0xc2:
		v := c.fetch()
		c.io()
		c.setP(c.getP() &^ v)
	case 0xe2:
		v := c.fetch()
		c.io()
		c.setP(c.getP() | v)
	// ---- misc ----
	case 0xea:
		c.io()
	case 0x42:
		c.fetch()
	case 0x00:
		c.fetch()
		c.interrupt(0xffe6, 0xfffe, true)
	case 0x02:
		c.fetch()
		c.interrupt(0xffe4, 0xfff4, true)
	case 0xcb:
		c.io()
		c.io()
		c.io()
		c.waiting = true
	case 0xdb:
		c.io()
		c.io()
		c.io()
		c.stopped = true
	case 0x54, 0x44: // MVN / MVP
		dst := c.fetch()
		src := c.fetch()
		c.db = dst
		v := c.rd(src<<16 | c.x)
		c.wr(dst<<16|c.y, v)
		c.io()
		c.io()
		step := -1
		if op == 0x54 {
			step = 1
		}
		im := c.maskX()
		c.x = (c.x + step) & im
		c.y = (c.y + step) & im
		c.a = (c.a - 1) & 0xffff
		if c.a != 0xffff {
			c.pc = (c.pc - 3) & 0xffff
		}
	}
}

// Step runs one instruction (or takes an interrupt).
func (c *CPU) Step() {
	if c.stopped {
		c.io()
		return
	}
	if c.NMIPending {
		c.NMIPending = false
		c.waiting = false
		c.io()
		c.io()
		c.interrupt(0xffea, 0xfffa, false)
		return
	}
	if c.IRQLine && c.fi == 0 {
		c.waiting = false
		c.io()
		c.io()
		c.interrupt(0xffee, 0xfffe, false)
		return
	}
	if c.waiting {
		if c.IRQLine {
			c.waiting = false
		} else {
			c.io()
			return
		}
	}
	c.exec(c.fetch())
}

// Reset starts the processor from its reset vector, in emulation mode.
func (c *CPU) Reset() {
	c.e, c.fm, c.fx, c.fi, c.fd = 1, 1, 1, 1, 0
	c.db, c.pb, c.d, c.s = 0, 0, 0, 0x1ff
	c.x &= 0xff
	c.y &= 0xff
	c.waiting, c.stopped = false, false
	c.pc = c.rd(0xfffc) | c.rd(0xfffd)<<8
}

// Waiting reports whether the processor sleeps in WAI.
func (c *CPU) Waiting() bool { return c.waiting }

// PC is the 24-bit address of the next instruction.
func (c *CPU) PC() int { return c.pb<<16 | c.pc }

// State is the processor's registers, as the web core's getState lists them.
type State [14]int

// GetState returns the registers.
func (c *CPU) GetState() State {
	return State{c.a, c.x, c.y, c.s, c.d, c.db, c.pb, c.pc, c.getP(), c.e, b2i(c.waiting), b2i(c.stopped), b2i(c.NMIPending), b2i(c.IRQLine)}
}

// SetState restores the registers.
func (c *CPU) SetState(st State) {
	c.a, c.x, c.y, c.s, c.d, c.db, c.pb, c.pc = st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]
	p := st[8]
	c.e = st[9]
	c.fn, c.fv, c.fm, c.fx, c.fd, c.fi, c.fz, c.fc = p>>7&1, p>>6&1, p>>5&1, p>>4&1, p>>3&1, p>>2&1, p>>1&1, p&1
	if c.e != 0 {
		c.s = 0x100 | c.s&0xff
		c.fm, c.fx = 1, 1
	}
	if c.fx != 0 {
		c.x &= 0xff
		c.y &= 0xff
	}
	c.waiting, c.stopped, c.NMIPending, c.IRQLine = st[10] != 0, st[11] != 0, st[12] != 0, st[13] != 0
}
