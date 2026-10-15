package snes

// The Sony SPC700 sound processor: a port of web/emu/spc700.js.

var spcCycles = [256]uint8{
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,
	3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
	3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,
	2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,
	2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,
}

const (
	pN = 0x80
	pV = 0x40
	pP = 0x20
	pB = 0x10
	pH = 0x08
	pI = 0x04
	pZ = 0x02
	pC = 0x01
)

type spc700 struct {
	apu                       *apu
	a, x, y, sp, pc, psw, ext int
	stopped                   bool
}

func (s *spc700) rd(ad int) int    { return int(s.apu.read(ad & 0xffff)) }
func (s *spc700) wr(ad int, v int) { s.apu.write(ad&0xffff, uint8(v)) }
func (s *spc700) dp(o int) int     { return (s.psw&pP)<<3 | o&0xff }
func (s *spc700) imm() int         { v := s.rd(s.pc); s.pc = (s.pc + 1) & 0xffff; return v }
func (s *spc700) imm16() int       { l := s.imm(); return l | s.imm()<<8 }
func (s *spc700) rd16dp(o int) int { return s.rd(s.dp(o)) | s.rd(s.dp(o+1))<<8 }
func (s *spc700) push(v int)       { s.wr(0x100|s.sp, v); s.sp = (s.sp - 1) & 0xff }
func (s *spc700) pop() int         { s.sp = (s.sp + 1) & 0xff; return s.rd(0x100 | s.sp) }

func (s *spc700) nz(v int) int {
	s.psw = s.psw&^(pN|pZ) | v&pN
	if v&0xff == 0 {
		s.psw |= pZ
	}
	return v & 0xff
}

func (s *spc700) nz16(v int) {
	s.psw = s.psw&^(pN|pZ) | (v>>8)&pN
	if v&0xffff == 0 {
		s.psw |= pZ
	}
}

func (s *spc700) adc(p, q int) int {
	r := p + q + s.psw&pC
	s.psw &^= pN | pV | pH | pZ | pC
	if r > 0xff {
		s.psw |= pC
	}
	if (p^q^r)&0x10 != 0 {
		s.psw |= pH
	}
	if ^(p^q)&(p^r)&0x80 != 0 {
		s.psw |= pV
	}
	return s.nz(r)
}

func (s *spc700) cmp(p, q int) {
	r := p - q
	s.psw &^= pC
	if r >= 0 {
		s.psw |= pC
	}
	s.nz(r)
}

func (s *spc700) alu(op, p, q int) int {
	switch op {
	case 0:
		return s.nz(p | q)
	case 1:
		return s.nz(p & q)
	case 2:
		return s.nz(p ^ q)
	case 3:
		s.cmp(p, q)
		return p
	case 4:
		return s.adc(p, q)
	}
	return s.adc(p, q^0xff)
}

func (s *spc700) asl(v int) int { s.psw = s.psw&^pC | v>>7; return s.nz(v << 1) }
func (s *spc700) rol(v int) int { c := s.psw & pC; s.psw = s.psw&^pC | v>>7; return s.nz(v<<1 | c) }
func (s *spc700) lsr(v int) int { s.psw = s.psw&^pC | v&1; return s.nz(v >> 1) }
func (s *spc700) ror(v int) int { c := s.psw & pC; s.psw = s.psw&^pC | v&1; return s.nz(v>>1 | c<<7) }

func (s *spc700) branch(cond bool) {
	o := s.imm()
	if cond {
		s.pc = (s.pc + int(int8(o))) & 0xffff
		s.ext += 2
	}
}

func (s *spc700) mbit() (int, int) { w := s.imm16(); return w & 0x1fff, w >> 13 }

func (s *spc700) aluOperand(col int) int {
	switch col {
	case 0x04:
		return s.rd(s.dp(s.imm()))
	case 0x05:
		return s.rd(s.imm16())
	case 0x06:
		return s.rd(s.dp(s.x))
	case 0x07:
		o := (s.imm() + s.x) & 0xff
		return s.rd(s.rd16dp(o))
	case 0x08:
		return s.imm()
	case 0x14:
		return s.rd(s.dp(s.imm() + s.x))
	case 0x15:
		return s.rd((s.imm16() + s.x) & 0xffff)
	case 0x16:
		return s.rd((s.imm16() + s.y) & 0xffff)
	}
	o := s.imm()
	return s.rd((s.rd16dp(o) + s.y) & 0xffff)
}

func (s *spc700) exec(op int) {
	hi, lo := op>>4, op&15
	if lo >= 4 && lo <= 9 && hi <= 0xb {
		opn, row := hi>>1, (hi&1)<<4
		col := row | lo
		switch col {
		case 0x09:
			src := s.rd(s.dp(s.imm()))
			d := s.dp(s.imm())
			r := s.alu(opn, s.rd(d), src)
			if opn != 3 {
				s.wr(d, r)
			}
			return
		case 0x18:
			src := s.imm()
			d := s.dp(s.imm())
			r := s.alu(opn, s.rd(d), src)
			if opn != 3 {
				s.wr(d, r)
			}
			return
		case 0x19:
			src := s.rd(s.dp(s.y))
			d := s.dp(s.x)
			r := s.alu(opn, s.rd(d), src)
			if opn != 3 {
				s.wr(d, r)
			}
			return
		}
		v := s.aluOperand(col)
		r := s.alu(opn, s.a, v)
		if opn != 3 {
			s.a = r
		}
		return
	}
	if lo == 1 { // TCALL
		s.push(s.pc >> 8)
		s.push(s.pc & 0xff)
		vec := 0xffde - hi*2
		s.pc = s.rd(vec) | s.rd(vec+1)<<8
		return
	}
	if lo == 2 { // SET1 / CLR1
		d, bit := s.dp(s.imm()), 1<<(hi>>1)
		v := s.rd(d)
		if hi&1 != 0 {
			s.wr(d, v&^bit)
		} else {
			s.wr(d, v|bit)
		}
		return
	}
	if lo == 3 { // BBS / BBC
		d, bit := s.dp(s.imm()), 1<<(hi>>1)
		v := s.rd(d)
		if hi&1 != 0 {
			s.branch(v&bit == 0)
		} else {
			s.branch(v&bit != 0)
		}
		return
	}
	switch op {
	case 0x00:
	case 0x0a:
		ad, b := s.mbit()
		if (s.rd(ad)>>b)&1 != 0 {
			s.psw |= pC
		}
	case 0x2a:
		ad, b := s.mbit()
		if (s.rd(ad)>>b)&1 == 0 {
			s.psw |= pC
		}
	case 0x4a:
		ad, b := s.mbit()
		if (s.rd(ad)>>b)&1 == 0 {
			s.psw &^= pC
		}
	case 0x6a:
		ad, b := s.mbit()
		if (s.rd(ad)>>b)&1 != 0 {
			s.psw &^= pC
		}
	case 0x8a:
		ad, b := s.mbit()
		s.psw ^= (s.rd(ad) >> b) & 1
	case 0xaa:
		ad, b := s.mbit()
		s.psw = s.psw&^pC | (s.rd(ad)>>b)&1
	case 0xca:
		ad, b := s.mbit()
		v := s.rd(ad)
		s.wr(ad, v&^(1<<b)|(s.psw&pC)<<b)
	case 0xea:
		ad, b := s.mbit()
		s.wr(ad, s.rd(ad)^(1<<b))
	case 0x0b:
		d := s.dp(s.imm())
		s.wr(d, s.asl(s.rd(d)))
	case 0x0c:
		d := s.imm16()
		s.wr(d, s.asl(s.rd(d)))
	case 0x1b:
		d := s.dp(s.imm() + s.x)
		s.wr(d, s.asl(s.rd(d)))
	case 0x1c:
		s.a = s.asl(s.a)
	case 0x2b:
		d := s.dp(s.imm())
		s.wr(d, s.rol(s.rd(d)))
	case 0x2c:
		d := s.imm16()
		s.wr(d, s.rol(s.rd(d)))
	case 0x3b:
		d := s.dp(s.imm() + s.x)
		s.wr(d, s.rol(s.rd(d)))
	case 0x3c:
		s.a = s.rol(s.a)
	case 0x4b:
		d := s.dp(s.imm())
		s.wr(d, s.lsr(s.rd(d)))
	case 0x4c:
		d := s.imm16()
		s.wr(d, s.lsr(s.rd(d)))
	case 0x5b:
		d := s.dp(s.imm() + s.x)
		s.wr(d, s.lsr(s.rd(d)))
	case 0x5c:
		s.a = s.lsr(s.a)
	case 0x6b:
		d := s.dp(s.imm())
		s.wr(d, s.ror(s.rd(d)))
	case 0x6c:
		d := s.imm16()
		s.wr(d, s.ror(s.rd(d)))
	case 0x7b:
		d := s.dp(s.imm() + s.x)
		s.wr(d, s.ror(s.rd(d)))
	case 0x7c:
		s.a = s.ror(s.a)
	case 0x8b:
		d := s.dp(s.imm())
		s.wr(d, s.nz(s.rd(d)-1))
	case 0x8c:
		d := s.imm16()
		s.wr(d, s.nz(s.rd(d)-1))
	case 0x9b:
		d := s.dp(s.imm() + s.x)
		s.wr(d, s.nz(s.rd(d)-1))
	case 0x9c:
		s.a = s.nz(s.a - 1)
	case 0xab:
		d := s.dp(s.imm())
		s.wr(d, s.nz(s.rd(d)+1))
	case 0xac:
		d := s.imm16()
		s.wr(d, s.nz(s.rd(d)+1))
	case 0xbb:
		d := s.dp(s.imm() + s.x)
		s.wr(d, s.nz(s.rd(d)+1))
	case 0xbc:
		s.a = s.nz(s.a + 1)
	case 0x1d:
		s.x = s.nz(s.x - 1)
	case 0x3d:
		s.x = s.nz(s.x + 1)
	case 0xdc:
		s.y = s.nz(s.y - 1)
	case 0xfc:
		s.y = s.nz(s.y + 1)
	case 0x0d:
		s.push(s.psw)
	case 0x2d:
		s.push(s.a)
	case 0x4d:
		s.push(s.x)
	case 0x6d:
		s.push(s.y)
	case 0x8e:
		s.psw = s.pop()
	case 0xae:
		s.a = s.pop()
	case 0xce:
		s.x = s.pop()
	case 0xee:
		s.y = s.pop()
	case 0x0e, 0x4e:
		d := s.imm16()
		v := s.rd(d)
		s.nz(s.a - v)
		if op == 0x0e {
			s.wr(d, v|s.a)
		} else {
			s.wr(d, v&^s.a)
		}
	case 0x0f:
		s.push(s.pc >> 8)
		s.push(s.pc & 0xff)
		s.push(s.psw)
		s.psw = (s.psw | pB) &^ pI
		s.pc = s.rd(0xffde) | s.rd(0xffdf)<<8
	case 0x10:
		s.branch(s.psw&pN == 0)
	case 0x30:
		s.branch(s.psw&pN != 0)
	case 0x50:
		s.branch(s.psw&pV == 0)
	case 0x70:
		s.branch(s.psw&pV != 0)
	case 0x90:
		s.branch(s.psw&pC == 0)
	case 0xb0:
		s.branch(s.psw&pC != 0)
	case 0xd0:
		s.branch(s.psw&pZ == 0)
	case 0xf0:
		s.branch(s.psw&pZ != 0)
	case 0x2f:
		s.branch(true)
		s.ext -= 2
	case 0x1a, 0x3a:
		o := s.imm()
		add := 0xffff
		if op == 0x3a {
			add = 1
		}
		v := (s.rd16dp(o) + add) & 0xffff
		s.wr(s.dp(o), v&0xff)
		s.wr(s.dp(o+1), v>>8)
		s.nz16(v)
	case 0x5a:
		w := s.rd16dp(s.imm())
		ya := s.y<<8 | s.a
		r := ya - w
		s.psw &^= pC
		if r >= 0 {
			s.psw |= pC
		}
		s.nz16(r & 0xffff)
	case 0x7a, 0x9a:
		w := s.rd16dp(s.imm())
		ya := s.y<<8 | s.a
		var r int
		s.psw &^= pN | pV | pH | pZ | pC
		if op == 0x7a {
			r = ya + w
			if r > 0xffff {
				s.psw |= pC
			}
			if (ya^w^r)&0x1000 != 0 {
				s.psw |= pH
			}
			if ^(ya^w)&(ya^r)&0x8000 != 0 {
				s.psw |= pV
			}
		} else {
			r = ya - w
			if r >= 0 {
				s.psw |= pC
			}
			if (ya^w^r)&0x1000 == 0 {
				s.psw |= pH
			}
			if (ya^w)&(ya^r)&0x8000 != 0 {
				s.psw |= pV
			}
		}
		r &= 0xffff
		s.nz16(r)
		s.a = r & 0xff
		s.y = r >> 8
	case 0xba:
		o := s.imm()
		v := s.rd16dp(o)
		s.a = v & 0xff
		s.y = v >> 8
		s.nz16(v)
	case 0xda:
		o := s.imm()
		s.wr(s.dp(o), s.a)
		s.wr(s.dp(o+1), s.y)
	case 0x1e:
		s.cmp(s.x, s.rd(s.imm16()))
	case 0x3e:
		s.cmp(s.x, s.rd(s.dp(s.imm())))
	case 0x5e:
		s.cmp(s.y, s.rd(s.imm16()))
	case 0x7e:
		s.cmp(s.y, s.rd(s.dp(s.imm())))
	case 0xad:
		s.cmp(s.y, s.imm())
	case 0xc8:
		s.cmp(s.x, s.imm())
	case 0x1f:
		t := (s.imm16() + s.x) & 0xffff
		s.pc = s.rd(t) | s.rd((t+1)&0xffff)<<8
	case 0x5f:
		s.pc = s.imm16()
	case 0x3f:
		t := s.imm16()
		s.push(s.pc >> 8)
		s.push(s.pc & 0xff)
		s.pc = t
	case 0x4f:
		t := 0xff00 | s.imm()
		s.push(s.pc >> 8)
		s.push(s.pc & 0xff)
		s.pc = t
	case 0x6f:
		l := s.pop()
		s.pc = l | s.pop()<<8
	case 0x7f:
		s.psw = s.pop()
		l := s.pop()
		s.pc = l | s.pop()<<8
	case 0x20:
		s.psw &^= pP
	case 0x40:
		s.psw |= pP
	case 0x60:
		s.psw &^= pC
	case 0x80:
		s.psw |= pC
	case 0xa0:
		s.psw |= pI
	case 0xc0:
		s.psw &^= pI
	case 0xe0:
		s.psw &^= pV | pH
	case 0xed:
		s.psw ^= pC
	case 0x2e:
		v := s.rd(s.dp(s.imm()))
		s.branch(s.a != v)
	case 0xde:
		v := s.rd(s.dp(s.imm() + s.x))
		s.branch(s.a != v)
	case 0x6e:
		d := s.dp(s.imm())
		v := (s.rd(d) - 1) & 0xff
		s.wr(d, v)
		s.branch(v != 0)
	case 0xfe:
		s.y = (s.y - 1) & 0xff
		s.branch(s.y != 0)
	case 0x5d:
		s.x = s.nz(s.a)
	case 0x7d:
		s.a = s.nz(s.x)
	case 0x9d:
		s.x = s.nz(s.sp)
	case 0xbd:
		s.sp = s.x
	case 0xdd:
		s.a = s.nz(s.y)
	case 0xfd:
		s.y = s.nz(s.a)
	case 0x8d:
		s.y = s.nz(s.imm())
	case 0xcd:
		s.x = s.nz(s.imm())
	case 0xe8:
		s.a = s.nz(s.imm())
	case 0x8f:
		v := s.imm()
		d := s.dp(s.imm())
		s.rd(d)
		s.wr(d, v)
	case 0xfa:
		v := s.rd(s.dp(s.imm()))
		s.wr(s.dp(s.imm()), v)
	case 0x9e: // DIV YA, X
		ya := s.y<<8 | s.a
		s.psw &^= pV | pH
		if s.x&15 <= s.y&15 {
			s.psw |= pH
		}
		if s.y >= s.x {
			s.psw |= pV
		}
		if s.y < s.x<<1 {
			s.a = (ya / s.x) & 0xff
			s.y = (ya % s.x) & 0xff
		} else {
			t := ya - s.x<<9
			s.a = (255 - t/(256-s.x)) & 0xff
			s.y = (s.x + t%(256-s.x)) & 0xff
		}
		s.nz(s.a)
	case 0xcf:
		r := s.y * s.a
		s.a = r & 0xff
		s.y = r >> 8
		s.nz(s.y)
	case 0x9f:
		s.a = s.nz(s.a>>4 | s.a<<4)
	case 0xbe: // DAS
		if s.psw&pC == 0 || s.a > 0x99 {
			s.a = (s.a - 0x60) & 0xff
			s.psw &^= pC
		}
		if s.psw&pH == 0 || s.a&15 > 9 {
			s.a = (s.a - 6) & 0xff
		}
		s.nz(s.a)
	case 0xdf: // DAA
		if s.psw&pC != 0 || s.a > 0x99 {
			s.a = (s.a + 0x60) & 0xff
			s.psw |= pC
		}
		if s.psw&pH != 0 || s.a&15 > 9 {
			s.a = (s.a + 6) & 0xff
		}
		s.nz(s.a)
	case 0xaf:
		s.wr(s.dp(s.x), s.a)
		s.x = (s.x + 1) & 0xff
	case 0xbf:
		s.a = s.nz(s.rd(s.dp(s.x)))
		s.x = (s.x + 1) & 0xff
	case 0xc4:
		s.wr(s.dp(s.imm()), s.a)
	case 0xc5:
		s.wr(s.imm16(), s.a)
	case 0xc6:
		s.wr(s.dp(s.x), s.a)
	case 0xc7:
		o := (s.imm() + s.x) & 0xff
		s.wr(s.rd16dp(o), s.a)
	case 0xc9:
		s.wr(s.imm16(), s.x)
	case 0xcb:
		s.wr(s.dp(s.imm()), s.y)
	case 0xcc:
		s.wr(s.imm16(), s.y)
	case 0xd4:
		s.wr(s.dp(s.imm()+s.x), s.a)
	case 0xd5:
		s.wr((s.imm16()+s.x)&0xffff, s.a)
	case 0xd6:
		s.wr((s.imm16()+s.y)&0xffff, s.a)
	case 0xd7:
		o := s.imm()
		s.wr((s.rd16dp(o)+s.y)&0xffff, s.a)
	case 0xd8:
		s.wr(s.dp(s.imm()), s.x)
	case 0xd9:
		s.wr(s.dp(s.imm()+s.y), s.x)
	case 0xdb:
		s.wr(s.dp(s.imm()+s.x), s.y)
	case 0xe4:
		s.a = s.nz(s.rd(s.dp(s.imm())))
	case 0xe5:
		s.a = s.nz(s.rd(s.imm16()))
	case 0xe6:
		s.a = s.nz(s.rd(s.dp(s.x)))
	case 0xe7:
		o := (s.imm() + s.x) & 0xff
		s.a = s.nz(s.rd(s.rd16dp(o)))
	case 0xe9:
		s.x = s.nz(s.rd(s.imm16()))
	case 0xeb:
		s.y = s.nz(s.rd(s.dp(s.imm())))
	case 0xec:
		s.y = s.nz(s.rd(s.imm16()))
	case 0xf4:
		s.a = s.nz(s.rd(s.dp(s.imm() + s.x)))
	case 0xf5:
		s.a = s.nz(s.rd((s.imm16() + s.x) & 0xffff))
	case 0xf6:
		s.a = s.nz(s.rd((s.imm16() + s.y) & 0xffff))
	case 0xf7:
		o := s.imm()
		s.a = s.nz(s.rd((s.rd16dp(o) + s.y) & 0xffff))
	case 0xf8:
		s.x = s.nz(s.rd(s.dp(s.imm())))
	case 0xf9:
		s.x = s.nz(s.rd(s.dp(s.imm() + s.y)))
	case 0xfb:
		s.y = s.nz(s.rd(s.dp(s.imm() + s.x)))
	case 0xef, 0xff:
		s.stopped = true
	}
}

func (s *spc700) step() int {
	if s.stopped {
		return 2
	}
	op := s.imm()
	s.ext = 0
	s.exec(op)
	return int(spcCycles[op]) + s.ext
}

func (s *spc700) reset() {
	s.a, s.x, s.y = 0, 0, 0
	s.sp, s.psw, s.stopped, s.pc = 0xef, 0x02, false, 0xffc0
}

func (s *spc700) state() [7]int {
	return [7]int{s.a, s.x, s.y, s.sp, s.pc, s.psw, b2i(s.stopped)}
}

func (s *spc700) setState(st [7]int) {
	s.a, s.x, s.y, s.sp, s.pc, s.psw = st[0], st[1], st[2], st[3], st[4], st[5]
	s.stopped = st[6] != 0
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}
