package gb

import "math/bits"

// The SM83 CPU (the CPU part of the GB class in gb.js). Every memory access
// goes through rd / wr, which advance the rest of the machine one M-cycle.

func (g *GB) getBC() int  { return g.b<<8 | g.c }
func (g *GB) getDE() int  { return g.d<<8 | g.e }
func (g *GB) getHL() int  { return g.h<<8 | g.l }
func (g *GB) setBC(v int) { g.b, g.c = (v>>8)&255, v&255 }
func (g *GB) setDE(v int) { g.d, g.e = (v>>8)&255, v&255 }
func (g *GB) setHL(v int) { g.h, g.l = (v>>8)&255, v&255 }

func (g *GB) fetch() int {
	v := g.rd(g.pc)
	g.pc = (g.pc + 1) & 0xffff
	return v
}

func (g *GB) fetch16() int {
	lo := g.fetch()
	return lo | g.fetch()<<8
}

func (g *GB) push16(v int) {
	g.idle()
	g.oamBug(g.sp, 0)
	g.sp = (g.sp - 1) & 0xffff
	g.wr(g.sp, v>>8)
	g.sp = (g.sp - 1) & 0xffff
	g.wr(g.sp, v&255)
}

func (g *GB) pop16() int {
	g.iduRead = true
	lo := g.rd(g.sp)
	g.sp = (g.sp + 1) & 0xffff
	hi := g.rd(g.sp)
	g.sp = (g.sp + 1) & 0xffff
	g.iduRead = false
	return lo | hi<<8
}

func (g *GB) getR(i int) int {
	switch i {
	case 0:
		return g.b
	case 1:
		return g.c
	case 2:
		return g.d
	case 3:
		return g.e
	case 4:
		return g.h
	case 5:
		return g.l
	case 6:
		return g.rd(g.getHL())
	}
	return g.a
}

func (g *GB) setR(i, v int) {
	v &= 255
	switch i {
	case 0:
		g.b = v
	case 1:
		g.c = v
	case 2:
		g.d = v
	case 3:
		g.e = v
	case 4:
		g.h = v
	case 5:
		g.l = v
	case 6:
		g.wr(g.getHL(), v)
	default:
		g.a = v
	}
}

func (g *GB) flags(z, n, h, c bool) {
	g.f = b2i(z)<<7 | b2i(n)<<6 | b2i(h)<<5 | b2i(c)<<4
}

func (g *GB) fz() bool { return g.f&0x80 != 0 }
func (g *GB) fc() bool { return g.f&0x10 != 0 }

func (g *GB) alu(op, v int) {
	a, c := g.a, b2i(g.fc())
	switch op {
	case 0: // ADD
		r := a + v
		g.flags(r&255 == 0, false, (a&15)+(v&15) > 15, r > 255)
		g.a = r & 255
	case 1: // ADC
		r := a + v + c
		g.flags(r&255 == 0, false, (a&15)+(v&15)+c > 15, r > 255)
		g.a = r & 255
	case 2: // SUB
		r := a - v
		g.flags(r&255 == 0, true, a&15 < v&15, r < 0)
		g.a = r & 255
	case 3: // SBC
		r := a - v - c
		g.flags(r&255 == 0, true, a&15 < (v&15)+c, r < 0)
		g.a = r & 255
	case 4: // AND
		g.a = a & v
		g.flags(g.a == 0, false, true, false)
	case 5: // XOR
		g.a = a ^ v
		g.flags(g.a == 0, false, false, false)
	case 6: // OR
		g.a = a | v
		g.flags(g.a == 0, false, false, false)
	case 7: // CP
		r := a - v
		g.flags(r&255 == 0, true, a&15 < v&15, r < 0)
	}
}

func (g *GB) cond(i int) bool {
	switch i {
	case 0:
		return !g.fz()
	case 1:
		return g.fz()
	case 2:
		return !g.fc()
	}
	return g.fc()
}

func (g *GB) getRR(i int) int {
	switch i {
	case 0:
		return g.getBC()
	case 1:
		return g.getDE()
	case 2:
		return g.getHL()
	}
	return g.sp
}

func (g *GB) setRR(i, v int) {
	v &= 0xffff
	switch i {
	case 0:
		g.setBC(v)
	case 1:
		g.setDE(v)
	case 2:
		g.setHL(v)
	default:
		g.sp = v
	}
}

// interrupts: idles is the wait before the pushes (2 when waking from HALT,
// 1 after a dropped opcode fetch); mask: requests too recent to be seen yet.
func (g *GB) interrupts(idles, mask int) bool {
	pending := g.ie & g.ifl & 0x1f &^ mask
	if pending == 0 {
		return false
	}
	if g.halted {
		g.halted = false
	}
	if !g.ime {
		return false
	}
	g.ime = false
	// "EI, HALT" with an interrupt already waiting: the handler returns to the HALT
	if g.haltBug {
		g.haltBug = false
		g.pc = (g.pc - 1) & 0xffff
	}
	for i := 0; i < idles; i++ {
		g.idle()
	}
	g.sp = (g.sp - 1) & 0xffff
	g.wr(g.sp, g.pc>>8)
	// Which interrupt wins is decided after the high byte is pushed.
	p2 := g.ie & g.ifl & 0x1f
	g.sp = (g.sp - 1) & 0xffff
	g.wr(g.sp, g.pc&255)
	if p2 == 0 {
		g.pc = 0
		return true
	}
	bit := p2 & -p2
	g.ifl &^= bit
	g.pc = 0x40 + 8*bits.TrailingZeros(uint(bit))
	g.idle()
	return true
}

func (g *GB) step() {
	if g.hdmaPending {
		g.hdmaPending = false
		if g.hdmaActive && !g.halted {
			g.hdmaBlock()
		}
	}
	// EI takes effect after the instruction that follows it
	if g.eiDelay != 0 {
		g.eiDelay--
		if g.eiDelay == 0 {
			g.ime = true
		}
	}
	if g.halted || g.stopped {
		if g.interrupts(2, 0) {
			return
		}
		if g.halted || g.stopped {
			g.idle()
			if g.stopped && g.keys&0xff != 0 {
				g.stopped = false
			}
			return
		}
	}
	// Opcode fetch. An interrupt that arrives during it is still taken (the
	// fetched byte is dropped), except the video chip's, which come too late
	// in the M-cycle.
	op := g.rd(g.pc)
	if g.interrupts(1, g.ppuNewIf) {
		return
	}
	if g.haltBug {
		g.haltBug = false
	} else {
		g.pc = (g.pc + 1) & 0xffff
	}
	g.exec(op)
}

func (g *GB) illegal() {
	g.locked = true // the CPU stops
	g.pc = (g.pc - 1) & 0xffff
}

func (g *GB) exec(op int) {
	if op >= 0x40 && op < 0x80 {
		if op == 0x76 { // HALT
			if !g.ime && g.ie&g.ifl&0x1f != 0 {
				g.haltBug = true
			} else {
				g.halted = true
			}
			return
		}
		g.setR((op>>3)&7, g.getR(op&7))
		return
	}
	if op >= 0x80 && op < 0xc0 {
		g.alu((op>>3)&7, g.getR(op&7))
		return
	}
	x, y, z := op>>6, (op>>3)&7, op&7
	p, q := y>>1, y&1
	if x == 0 {
		switch z {
		case 0:
			switch {
			case y == 0: // NOP
			case y == 1: // LD (nn),SP
				a := g.fetch16()
				g.wr(a, g.sp&255)
				g.wr((a+1)&0xffff, g.sp>>8)
			case y == 2: // STOP
				g.fetch()
				if g.cgb && g.key1&1 != 0 {
					g.double = !g.double
					g.key1 = 0
					g.div = 0
				} else {
					g.stopped = true
				}
			default: // JR
				e := int(int8(g.fetch()))
				if y == 3 || g.cond(y-4) {
					g.idle()
					g.pc = (g.pc + e) & 0xffff
				}
			}
		case 1:
			if q == 0 {
				g.setRR(p, g.fetch16())
				return
			}
			hl, v := g.getHL(), g.getRR(p)
			r := hl + v
			g.idle()
			g.f = g.f&0x80 | b2i((hl&0xfff)+(v&0xfff) > 0xfff)<<5 | b2i(r > 0xffff)<<4
			g.setHL(r & 0xffff)
		case 2:
			var addr int
			switch p {
			case 0:
				addr = g.getBC()
			case 1:
				addr = g.getDE()
			default:
				addr = g.getHL()
			}
			if q == 0 {
				g.wr(addr, g.a)
			} else {
				g.iduRead = p >= 2
				g.a = g.rd(addr)
				g.iduRead = false
			}
			if p == 2 {
				g.setHL((g.getHL() + 1) & 0xffff)
			} else if p == 3 {
				g.setHL((g.getHL() - 1) & 0xffff)
			}
		case 3:
			v := g.getRR(p)
			g.idle()
			g.oamBug(v, 0)
			if q != 0 {
				g.setRR(p, v-1)
			} else {
				g.setRR(p, v+1)
			}
		case 4:
			v := g.getR(y)
			r := (v + 1) & 255
			g.f = g.f&0x10 | b2i(r == 0)<<7 | b2i(v&15 == 15)<<5
			g.setR(y, r)
		case 5:
			v := g.getR(y)
			r := (v - 1) & 255
			g.f = g.f&0x10 | 0x40 | b2i(r == 0)<<7 | b2i(v&15 == 0)<<5
			g.setR(y, r)
		case 6:
			g.setR(y, g.fetch())
		case 7:
			a, c := g.a, b2i(g.fc())
			switch y {
			case 0: // RLCA
				g.a = ((a << 1) | (a >> 7)) & 255
				g.flags(false, false, false, a&0x80 != 0)
			case 1: // RRCA
				g.a = ((a >> 1) | (a << 7)) & 255
				g.flags(false, false, false, a&1 != 0)
			case 2: // RLA
				g.a = ((a << 1) | c) & 255
				g.flags(false, false, false, a&0x80 != 0)
			case 3: // RRA
				g.a = ((a >> 1) | (c << 7)) & 255
				g.flags(false, false, false, a&1 != 0)
			case 4: // DAA
				r, carry := a, g.fc()
				if g.f&0x40 == 0 {
					if carry || r > 0x99 {
						r += 0x60
						carry = true
					}
					if g.f&0x20 != 0 || r&15 > 9 {
						r += 6
					}
				} else {
					if carry {
						r -= 0x60
					}
					if g.f&0x20 != 0 {
						r -= 6
					}
				}
				r &= 255
				g.f = b2i(r == 0)<<7 | g.f&0x40 | b2i(carry)<<4
				g.a = r
			case 5: // CPL
				g.a = a ^ 255
				g.f |= 0x60
			case 6: // SCF
				g.f = g.f&0x80 | 0x10
			case 7: // CCF
				g.f = (g.f & 0x90) ^ 0x10
			}
		}
		return
	}
	// x == 3
	switch z {
	case 0:
		switch {
		case y < 4: // RET cc
			g.idle()
			if g.cond(y) {
				g.pc = g.pop16()
				g.idle()
			}
		case y == 4:
			g.wr(0xff00|g.fetch(), g.a)
		case y == 6:
			g.a = g.rd(0xff00 | g.fetch())
		default: // ADD SP,e / LD HL,SP+e
			e := int(int8(g.fetch()))
			sp := g.sp
			r := (sp + e) & 0xffff
			g.flags(false, false, (sp&15)+(e&15) > 15, (sp&255)+(e&255) > 255)
			if y == 5 {
				g.idle()
				g.idle()
				g.sp = r
			} else {
				g.idle()
				g.setHL(r)
			}
		}
	case 1:
		if q == 0 {
			v := g.pop16()
			if p == 3 {
				g.a = v >> 8
				g.f = v & 0xf0
			} else {
				g.setRR(p, v)
			}
			return
		}
		switch p {
		case 0: // RET
			g.pc = g.pop16()
			g.idle()
		case 1: // RETI
			g.pc = g.pop16()
			g.idle()
			g.ime = true
		case 2: // JP HL
			g.pc = g.getHL()
		default: // LD SP,HL
			g.idle()
			g.sp = g.getHL()
		}
	case 2:
		switch {
		case y < 4:
			a := g.fetch16()
			if g.cond(y) {
				g.idle()
				g.pc = a
			}
		case y == 4:
			g.wr(0xff00|g.c, g.a)
		case y == 5:
			g.wr(g.fetch16(), g.a)
		case y == 6:
			g.a = g.rd(0xff00 | g.c)
		default:
			g.a = g.rd(g.fetch16())
		}
	case 3:
		switch y {
		case 0: // JP nn
			a := g.fetch16()
			g.idle()
			g.pc = a
		case 1:
			g.execCB(g.fetch())
		case 6: // DI
			g.ime = false
			g.eiDelay = 0
		case 7: // EI (takes effect after the next instruction)
			if !g.ime && g.eiDelay == 0 {
				g.eiDelay = 2
			}
		default:
			g.illegal()
		}
	case 4:
		if y < 4 { // CALL cc
			a := g.fetch16()
			if g.cond(y) {
				g.push16(g.pc)
				g.pc = a
			}
			return
		}
		g.illegal()
	case 5:
		if q == 0 {
			v := g.getRR(p)
			if p == 3 {
				v = g.a<<8 | g.f
			}
			g.push16(v)
			return
		}
		if p == 0 { // CALL nn
			a := g.fetch16()
			g.push16(g.pc)
			g.pc = a
			return
		}
		g.illegal()
	case 6:
		g.alu(y, g.fetch())
	case 7: // RST
		g.push16(g.pc)
		g.pc = y * 8
	}
}

func (g *GB) execCB(op int) {
	r, y, x := op&7, (op>>3)&7, op>>6
	v := g.getR(r)
	switch x {
	case 1: // BIT
		g.f = g.f&0x10 | 0x20 | b2i((v>>y)&1 == 0)<<7
		return
	case 2: // RES
		g.setR(r, v&^(1<<y))
		return
	case 3: // SET
		g.setR(r, v|(1<<y))
		return
	}
	var res, c int
	cin := b2i(g.fc())
	switch y {
	case 0: // RLC
		c = v >> 7
		res = (v << 1) | c
	case 1: // RRC
		c = v & 1
		res = (v >> 1) | (c << 7)
	case 2: // RL
		c = v >> 7
		res = (v << 1) | cin
	case 3: // RR
		c = v & 1
		res = (v >> 1) | (cin << 7)
	case 4: // SLA
		c = v >> 7
		res = v << 1
	case 5: // SRA
		c = v & 1
		res = (v >> 1) | (v & 0x80)
	case 6: // SWAP
		res = ((v & 15) << 4) | (v >> 4)
	default: // SRL
		c = v & 1
		res = v >> 1
	}
	res &= 255
	g.flags(res == 0, false, false, c != 0)
	g.setR(r, res)
}
