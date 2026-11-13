package arm

import "math/bits"

// media: the ARMv6 media instructions of the 3DS's ARM11 (ARM state, bits
// 27-25 = 011 with bit 4 set): parallel add and subtract, packing,
// saturation, byte reversal, selection, sign and zero extension, the dual
// 16-bit multiplies, the most-significant-word multiplies and USAD8.
func (c *CPU) media(op uint32) {
	rn, rd, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	op1 := op >> 20 & 0x1f
	op2 := op >> 5 & 7
	switch {
	case op1 < 8: // 0b00xxx: parallel add and subtract
		c.parallel(op, op1&7, op2)
	case op1 == 8 && op2&1 == 0: // PKHBT / PKHTB
		sh := op >> 7 & 31
		n, m := c.R[rn], c.R[rm]
		if op2&2 == 0 {
			c.R[rd] = n&0xffff | (m<<sh)&0xffff0000
		} else {
			if sh == 0 {
				sh = 32
			}
			c.R[rd] = n&0xffff0000 | uint32(asr(m, sh))&0xffff
		}
	case op1 == 8 && op2 == 5: // SEL
		ge := c.CPSR >> 16 & 15
		n, m := c.R[rn], c.R[rm]
		var r uint32
		for i := uint(0); i < 4; i++ {
			src := m
			if ge>>i&1 != 0 {
				src = n
			}
			r |= src & (0xff << (8 * i))
		}
		c.R[rd] = r
	case op1&0x1a == 0x0a && op2&1 == 0: // SSAT / USAT
		sh := op >> 7 & 31
		v := c.R[rm] << sh
		if op>>6&1 != 0 {
			if sh == 0 {
				sh = 32
			}
			v = uint32(asr(c.R[rm], sh))
		}
		sat := op >> 16 & 31
		var r int64
		var q bool
		if op1&4 == 0 {
			r, q = ssat(int64(int32(v)), sat+1)
		} else {
			r, q = usat(int64(int32(v)), sat)
		}
		c.R[rd] = uint32(r)
		if q {
			c.CPSR |= FlagQ
		}
	case (op1 == 0x0a || op1 == 0x0e) && op2 == 1: // SSAT16 / USAT16
		sat := op >> 16 & 15
		v := c.R[rm]
		var out uint32
		for i := uint(0); i < 2; i++ {
			x := int64(int16(v >> (16 * i)))
			var r int64
			var q bool
			if op1 == 0x0a {
				r, q = ssat(x, sat+1)
			} else {
				r, q = usat(x, sat)
			}
			out |= uint32(r) & 0xffff << (16 * i)
			if q {
				c.CPSR |= FlagQ
			}
		}
		c.R[rd] = out
	case op1 == 0x0b && op2 == 1: // REV
		c.R[rd] = bits.ReverseBytes32(c.R[rm])
	case op1 == 0x0b && op2 == 5: // REV16
		v := c.R[rm]
		c.R[rd] = v>>8&0x00ff00ff | v<<8&0xff00ff00
	case op1 == 0x0f && op2 == 5: // REVSH
		v := c.R[rm]
		c.R[rd] = uint32(int32(int16(uint16(v<<8&0xff00 | v>>8&0xff))))
	case op1 >= 8 && op1 < 16 && op2 == 3: // sign and zero extension
		rot := op >> 10 & 3 * 8
		v := bits.RotateLeft32(c.R[rm], -int(rot))
		var add uint32
		if rn != 15 {
			add = c.R[rn]
		}
		switch op1 & 7 {
		case 0: // SXTAB16
			lo := add + uint32(int32(int8(v)))
			hi := add>>16 + uint32(int32(int8(v>>16)))
			c.R[rd] = lo&0xffff | hi<<16
		case 2: // SXTAB
			c.R[rd] = add + uint32(int32(int8(v)))
		case 3: // SXTAH
			c.R[rd] = add + uint32(int32(int16(v)))
		case 4: // UXTAB16
			lo := add + v&0xff
			hi := add>>16 + v>>16&0xff
			c.R[rd] = lo&0xffff | hi<<16
		case 6: // UXTAB
			c.R[rd] = add + v&0xff
		case 7: // UXTAH
			c.R[rd] = add + v&0xffff
		default:
			c.undefined(op)
		}
	case op1 == 0x10 && op2 < 4: // SMLAD, SMUAD, SMLSD, SMUSD
		// here Rd is bits 19-16, Ra 15-12, Rm 11-8, Rn 3-0
		n, m := c.R[rm], c.R[rs]
		if op2&1 != 0 {
			m = m>>16 | m<<16
		}
		p1 := int64(int16(n)) * int64(int16(m))
		p2 := int64(int16(n>>16)) * int64(int16(m>>16))
		r := p1 + p2
		if op2&2 != 0 {
			r = p1 - p2
		}
		if r != int64(int32(r)) { // each 32-bit addition can overflow
			c.CPSR |= FlagQ
		}
		r = int64(int32(r))
		if rd != 15 { // Ra
			r += int64(int32(c.R[rd]))
			if r != int64(int32(r)) {
				c.CPSR |= FlagQ
			}
		}
		c.R[rn] = uint32(r)
		c.Cycles++
	case op1 == 0x14 && op2 < 4: // SMLALD, SMLSLD
		n, m := c.R[rm], c.R[rs]
		if op2&1 != 0 {
			m = m>>16 | m<<16
		}
		p1 := int64(int16(n)) * int64(int16(m))
		p2 := int64(int16(n>>16)) * int64(int16(m>>16))
		r := p1 + p2
		if op2&2 != 0 {
			r = p1 - p2
		}
		acc := int64(uint64(c.R[rn])<<32 | uint64(c.R[rd]))
		acc += r
		c.R[rd], c.R[rn] = uint32(acc), uint32(uint64(acc)>>32)
		c.Cycles += 2
	case op1 == 0x15 && (op2 < 2 || op2 >= 6): // SMMUL, SMMLA, SMMLS
		p := int64(int32(c.R[rm])) * int64(int32(c.R[rs]))
		var acc int64
		if rd != 15 {
			acc = int64(int32(c.R[rd])) << 32
		}
		if op2 >= 6 {
			acc -= p
		} else {
			acc += p
		}
		if op2&1 != 0 {
			acc += 0x80000000
		}
		c.R[rn] = uint32(uint64(acc) >> 32)
		c.Cycles++
	case op1 == 0x18 && op2 == 0: // USAD8, USADA8
		n, m := c.R[rm], c.R[rs]
		var sum uint32
		for i := uint(0); i < 32; i += 8 {
			a, b := int32(n>>i&0xff), int32(m>>i&0xff)
			d := a - b
			if d < 0 {
				d = -d
			}
			sum += uint32(d)
		}
		if rd != 15 {
			sum += c.R[rd]
		}
		c.R[rn] = sum
	default:
		c.undefined(op)
	}
}

func asr(v, sh uint32) int32 {
	if sh >= 32 {
		return int32(v) >> 31
	}
	return int32(v) >> sh
}

// ssat saturates v to a signed n-bit range.
func ssat(v int64, n uint32) (int64, bool) {
	max := int64(1)<<(n-1) - 1
	min := -int64(1) << (n - 1)
	if v > max {
		return max, true
	}
	if v < min {
		return min, true
	}
	return v, false
}

// usat saturates v to an unsigned n-bit range.
func usat(v int64, n uint32) (int64, bool) {
	max := int64(1)<<n - 1
	if v > max {
		return max, true
	}
	if v < 0 {
		return 0, true
	}
	return v, false
}

// parallel: SADD16, QADD8, UHSUBADDX and the rest. kind: 1 signed (sets
// GE), 2 signed saturating, 3 signed halving, 5 unsigned (sets GE),
// 6 unsigned saturating, 7 unsigned halving.
func (c *CPU) parallel(op, kind, sel uint32) {
	if kind == 0 || kind == 4 || sel == 5 || sel == 6 {
		c.undefined(op)
		return
	}
	n, m := c.R[op>>16&15], c.R[op&15]
	rd := op >> 12 & 15
	signed := kind < 4
	size := uint(16)
	if sel >= 4 {
		size = 8
	}
	lanes := 32 / size
	mask := uint32(1)<<size - 1
	lane := func(v uint32, i uint) int64 {
		x := v >> (i * size) & mask
		if signed {
			return int64(int32(x<<(32-size)) >> (32 - size))
		}
		return int64(x)
	}
	var out, ge uint32
	for i := uint(0); i < lanes; i++ {
		a := lane(n, i)
		var b int64
		sub := false
		switch sel {
		case 0, 4: // ADD16, ADD8
			b = lane(m, i)
		case 3, 7: // SUB16, SUB8
			b, sub = lane(m, i), true
		case 1: // ASX: low = n.lo - m.hi, high = n.hi + m.lo
			b, sub = lane(m, 1-i), i == 0
		case 2: // SAX: low = n.lo + m.hi, high = n.hi - m.lo
			b, sub = lane(m, 1-i), i == 1
		}
		r := a + b
		if sub {
			r = a - b
		}
		var g bool
		switch kind {
		case 1:
			g = r >= 0
		case 5:
			if sub {
				g = r >= 0
			} else {
				g = r >= int64(1)<<size
			}
		case 2:
			r, _ = ssat(r, uint32(size))
		case 6:
			r, _ = usat(r, uint32(size))
		case 3, 7:
			r >>= 1
		}
		out |= uint32(r) & mask << (i * size)
		if g {
			if size == 16 {
				ge |= 3 << (2 * i)
			} else {
				ge |= 1 << i
			}
		}
	}
	c.R[rd] = out
	if kind == 1 || kind == 5 {
		c.CPSR = c.CPSR&^(15<<16) | ge<<16
	}
}
