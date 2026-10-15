package arm7

type opFn func(c *CPU, op uint32)

var (
	armTable   [4096]opFn
	thumbTable [1024]opFn
)

func init() { buildTables() }

// sign-extension helpers
func sx8(v uint32) uint32  { return uint32(int32(int8(v))) }
func sx16(v uint32) uint32 { return uint32(int32(int16(v))) }

// =====================================================================
// ARM instructions
// =====================================================================

func armUndefined(c *CPU, _ uint32) { c.undefinedInstr() }

// armDP: data processing (the operation, S bit, immediate / register-shift
// forms all come from the instruction's bits, as in the table's index).
func armDP(c *CPU, op uint32) {
	opc := op >> 21 & 15
	S := op>>20&1 != 0
	imm := op>>25&1 != 0
	regShift := !imm && op>>4&1 != 0
	test := opc >= 8 && opc <= 11
	r := &c.R
	rn, rd := op>>16&15, op>>12&15
	var b uint32
	if imm {
		rot := op >> 7 & 0x1e
		b = ror(op&0xff, rot)
		if rot != 0 {
			c.sc = b >> 31
		} else {
			c.sc = c.C
		}
	} else {
		rm, typ := op&15, op>>5&3
		if regShift {
			s := r[op>>8&15] & 0xff
			c.Cycles++
			v := r[rm]
			if rm == 15 {
				v += 4
			}
			b = c.shiftReg(v, typ, s)
		} else {
			b = c.shiftImm(r[rm], typ, op>>7&31)
		}
	}
	a := r[rn]
	if rn == 15 && regShift {
		a += 4
	}
	setf := S && (rd != 15 || test || bankOf[c.Mode&31] == 0)
	var res uint32
	switch opc {
	case 0:
		res = c.logic(a&b, setf)
	case 1:
		res = c.logic(a^b, setf)
	case 2:
		res = c.sub(a, b, 1, setf)
	case 3:
		res = c.sub(b, a, 1, setf)
	case 4:
		res = c.add(a, b, 0, setf)
	case 5:
		res = c.add(a, b, c.C, setf)
	case 6:
		res = c.sub(a, b, c.C, setf)
	case 7:
		res = c.sub(b, a, c.C, setf)
	case 8:
		c.logic(a&b, true)
	case 9:
		c.logic(a^b, true)
	case 10:
		c.sub(a, b, 1, true)
	case 11:
		c.add(a, b, 0, true)
	case 12:
		res = c.logic(a|b, setf)
	case 13:
		res = c.logic(b, setf)
	case 14:
		res = c.logic(a&^b, setf)
	default:
		res = c.logic(^b, setf)
	}
	if test {
		if rd == 15 && S && bankOf[c.Mode&31] != 0 {
			c.SetCPSR(c.spsr())
		}
		return
	}
	if rd == 15 {
		if S && bankOf[c.Mode&31] != 0 {
			c.SetCPSR(c.spsr())
		}
		c.Branch(res)
	} else {
		r[rd] = res
	}
}

func armMul(c *CPU, op uint32) {
	r := &c.R
	rd, rn, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	acc, S := op>>21&1 != 0, op>>20&1 != 0
	res := r[rm] * r[rs]
	c.Cycles += mulCycles(r[rs], true)
	if acc {
		v := r[rn]
		if rn == 15 {
			v += 4
		}
		res += v
		c.Cycles++
	}
	r[rd] = res
	if S {
		c.N = res >> 31
		c.Z = b2u(res == 0)
	}
	if rd == 15 {
		c.Branch(res)
	}
}

func armMull(c *CPU, op uint32) {
	r := &c.R
	hi, lo, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	signed, acc, S := op>>22&1 != 0, op>>21&1 != 0, op>>20&1 != 0
	var p uint64
	if signed {
		p = uint64(int64(int32(r[rm])) * int64(int32(r[rs])))
	} else {
		p = uint64(r[rm]) * uint64(r[rs])
	}
	c.Cycles += mulCycles(r[rs], signed) + 1
	if acc {
		p += uint64(r[hi])<<32 | uint64(r[lo])
		c.Cycles++
	}
	r[lo], r[hi] = uint32(p), uint32(p>>32)
	if S {
		c.N = uint32(p >> 63)
		c.Z = b2u(p == 0)
	}
}

func armSwp(c *CPU, op uint32) {
	r := &c.R
	rn, rd, rm, byteOp := op>>16&15, op>>12&15, op&15, op>>22&1 != 0
	addr := r[rn]
	val := r[rm]
	if rm == 15 {
		val += 4
	}
	var t uint32
	if byteOp {
		t = c.bus.Read8(addr)
		c.bus.Write8(addr, val&0xff)
	} else {
		t = ror(c.bus.Read32(addr), (addr&3)*8)
		c.bus.Write32(addr, val)
	}
	c.Cycles++
	if rd == 15 {
		c.Branch(t)
	} else {
		r[rd] = t
	}
}

func armBX(c *CPU, op uint32) {
	v := c.R[op&15]
	c.T = v & 1
	c.Branch(v)
}

func armMRS(c *CPU, op uint32) {
	rd := op >> 12 & 15
	var v uint32
	if op>>22&1 != 0 {
		v = c.spsr()
	} else {
		v = c.CPSR()
	}
	if rd == 15 {
		c.Branch(v)
	} else {
		c.R[rd] = v
	}
}

func armMSR(c *CPU, op uint32) {
	var v uint32
	if op&0x02000000 != 0 {
		v = ror(op&0xff, op>>7&0x1e)
	} else {
		v = c.R[op&15]
	}
	var mask uint32
	if op&0x80000 != 0 {
		mask |= 0xff000000
	}
	if op&0x40000 != 0 {
		mask |= 0x00ff0000
	}
	if op&0x20000 != 0 {
		mask |= 0x0000ff00
	}
	if op&0x10000 != 0 {
		mask |= 0x000000ff
	}
	if op>>22&1 != 0 {
		if bankOf[c.Mode&31] == 0 {
			return
		}
		c.setSPSR(c.spsr()&^mask | v&mask)
	} else {
		if c.Mode == ModeUSR {
			mask &= 0xff000000
		}
		nv := c.CPSR()&^mask | v&mask
		c.SetCPSR(nv | 0x10)
	}
}

func armHalf(c *CPU, op uint32) {
	r := &c.R
	P, U, I, W, L := op>>24&1 != 0, op>>23&1 != 0, op>>22&1 != 0, op>>21&1 != 0, op>>20&1 != 0
	rn, rd, sh := op>>16&15, op>>12&15, op>>5&3
	var off uint32
	if I {
		off = op>>4&0xf0 | op&15
	} else {
		off = r[op&15]
	}
	base := r[rn]
	moved := base - off
	if U {
		moved = base + off
	}
	addr := base
	if P {
		addr = moved
	}
	wb := moved
	bus := c.bus
	storeVal := func() uint32 {
		if rd == 15 {
			return r[15] + 4
		}
		return r[rd]
	}
	if L {
		var v uint32
		switch sh {
		case 1:
			v = ror(bus.Read16(addr), (addr&1)*8)
		case 2:
			v = sx8(bus.Read8(addr))
		default:
			if addr&1 != 0 {
				v = sx8(bus.Read16(addr) >> 8)
			} else {
				v = sx16(bus.Read16(addr))
			}
		}
		c.Cycles++
		if !P || W {
			r[rn] = wb
		}
		if rd == 15 {
			c.Branch(v)
		} else {
			r[rd] = v
		}
		return
	}
	if sh == 1 {
		bus.Write16(addr, storeVal()&0xffff)
	} else {
		// doubleword forms don't exist on this processor: treat as a load / store of halfwords
		if sh == 2 {
			v := ror(bus.Read16(addr), (addr&1)*8)
			c.Cycles++
			if !P || W {
				r[rn] = wb
			}
			if rd == 15 {
				c.Branch(v)
			} else {
				r[rd] = v
			}
			return
		}
		bus.Write16(addr, storeVal()&0xffff)
	}
	if !P || W {
		r[rn] = wb
	}
}

func armSDT(c *CPU, op uint32) {
	I, P, U, B, W, L := op>>25&1 != 0, op>>24&1 != 0, op>>23&1 != 0, op>>22&1 != 0, op>>21&1 != 0, op>>20&1 != 0
	r := &c.R
	rn, rd := op>>16&15, op>>12&15
	var off uint32
	if I {
		off = c.shiftImm(r[op&15], op>>5&3, op>>7&31)
	} else {
		off = op & 0xfff
	}
	base := r[rn]
	moved := base - off
	if U {
		moved = base + off
	}
	addr := base
	if P {
		addr = moved
	}
	bus := c.bus
	if L {
		var v uint32
		if B {
			v = bus.Read8(addr)
		} else {
			v = ror(bus.Read32(addr), (addr&3)*8)
		}
		c.Cycles++
		if !P || W {
			r[rn] = moved
			if rn == 15 {
				c.Branch(moved)
			}
		}
		if rd == 15 {
			c.Branch(v)
		} else {
			r[rd] = v
		}
		return
	}
	v := r[rd]
	if rd == 15 {
		v += 4
	}
	if B {
		bus.Write8(addr, v&0xff)
	} else {
		bus.Write32(addr, v)
	}
	if !P || W {
		r[rn] = moved
		if rn == 15 {
			c.Branch(moved)
		}
	}
}

func popCount16(v uint32) int {
	n := 0
	for k := v; k != 0; k &= k - 1 {
		n++
	}
	return n
}

func armBlock(c *CPU, op uint32) {
	r := &c.R
	P, U, S, W, L := op>>24&1 != 0, op>>23&1 != 0, op>>22&1 != 0, op>>21&1 != 0, op>>20&1 != 0
	rn := op >> 16 & 15
	list := op & 0xffff
	bus := c.bus
	n := popCount16(list)
	base := r[rn]
	bytes, empty := uint32(n*4), false
	if n == 0 {
		list, bytes, empty = 0x8000, 0x40, true
	}
	var addr uint32
	if U {
		addr = base
		if P {
			addr = base + 4
		}
	} else {
		addr = base - bytes + 4
		if P {
			addr = base - bytes
		}
	}
	newBase := base - bytes
	if U {
		newBase = base + bytes
	}
	userBank := S && !(L && list&0x8000 != 0)
	if L {
		if W {
			r[rn] = newBase
		}
		for k := 0; k < 16; k++ {
			if list&(1<<k) == 0 {
				continue
			}
			v := bus.Read32(addr)
			addr += 4
			if k == 15 {
				if S {
					c.SetCPSR(c.spsr())
				}
				c.Branch(v)
			} else if userBank {
				c.setUser(k, v)
			} else {
				r[k] = v
			}
		}
		c.Cycles++
		return
	}
	first := true
	for k := 0; k < 16; k++ {
		if list&(1<<k) == 0 {
			continue
		}
		var v uint32
		switch {
		case k == 15:
			v = r[15] + 4
		case userBank:
			v = c.getUser(k)
		default:
			v = r[k]
		}
		if uint32(k) == rn && !first && W && !empty {
			v = newBase
		}
		bus.Write32(addr, v)
		addr += 4
		first = false
	}
	if W {
		r[rn] = newBase
	}
}

func armBranch(c *CPU, op uint32) {
	off := uint32(int32(op<<8) >> 6)
	if op&0x01000000 != 0 {
		c.R[14] = c.PC
	}
	c.Branch(c.R[15] + off)
}

func armSWI(c *CPU, op uint32) { c.swi(op >> 16 & 0xff) }

// =====================================================================
// Thumb instructions
// =====================================================================

func (c *CPU) setNZ(v uint32) {
	c.N = v >> 31
	c.Z = b2u(v == 0)
}

func thShiftImm(c *CPU, op uint32) {
	typ, amt, rs, rd := op>>11&3, op>>6&31, op>>3&7, op&7
	v := c.shiftImm(c.R[rs], typ, amt)
	c.R[rd] = v
	c.setNZ(v)
	c.C = c.sc
}

func thAddSub(c *CPU, op uint32) {
	immF, sub, rn, rs, rd := op>>10&1 != 0, op>>9&1 != 0, op>>6&7, op>>3&7, op&7
	b := rn
	if !immF {
		b = c.R[rn]
	}
	if sub {
		c.R[rd] = c.sub(c.R[rs], b, 1, true)
	} else {
		c.R[rd] = c.add(c.R[rs], b, 0, true)
	}
}

func thImm(c *CPU, op uint32) {
	o, rd, imm := op>>11&3, op>>8&7, op&0xff
	r := &c.R
	switch o {
	case 0:
		r[rd] = imm
		c.N = 0
		c.Z = b2u(imm == 0)
	case 1:
		c.sub(r[rd], imm, 1, true)
	case 2:
		r[rd] = c.add(r[rd], imm, 0, true)
	default:
		r[rd] = c.sub(r[rd], imm, 1, true)
	}
}

func thAlu(c *CPU, op uint32) {
	o, rs, rd := op>>6&15, op>>3&7, op&7
	r := &c.R
	a, b := r[rd], r[rs]
	var v uint32
	switch o {
	case 0:
		v = a & b
	case 1:
		v = a ^ b
	case 2:
		v = c.shiftReg(a, 0, b&0xff)
		c.C = c.sc
		c.Cycles++
	case 3:
		v = c.shiftReg(a, 1, b&0xff)
		c.C = c.sc
		c.Cycles++
	case 4:
		v = c.shiftReg(a, 2, b&0xff)
		c.C = c.sc
		c.Cycles++
	case 5:
		r[rd] = c.add(a, b, c.C, true)
		return
	case 6:
		r[rd] = c.sub(a, b, c.C, true)
		return
	case 7:
		v = c.shiftReg(a, 3, b&0xff)
		c.C = c.sc
		c.Cycles++
	case 8:
		c.setNZ(a & b)
		return
	case 9:
		r[rd] = c.sub(0, b, 1, true)
		return
	case 10:
		c.sub(a, b, 1, true)
		return
	case 11:
		c.add(a, b, 0, true)
		return
	case 12:
		v = a | b
	case 13:
		v = a * b
		c.Cycles += mulCycles(a, true)
	case 14:
		v = a &^ b
	default:
		v = ^b
	}
	r[rd] = v
	c.setNZ(v)
}

func thHi(c *CPU, op uint32) {
	o, rs, rd := op>>8&3, op>>3&15, op&7|op>>4&8
	r := &c.R
	b := r[rs]
	switch o {
	case 0:
		v := r[rd] + b
		if rd == 15 {
			c.Branch(v)
		} else {
			r[rd] = v
		}
	case 1:
		c.sub(r[rd], b, 1, true)
	case 2:
		if rd == 15 {
			c.Branch(b)
		} else {
			r[rd] = b
		}
	default:
		c.T = b & 1
		c.Branch(b)
	}
}

func thLdrPc(c *CPU, op uint32) {
	rd := op >> 8 & 7
	addr := c.R[15]&^2 + (op&0xff)*4
	c.R[rd] = c.bus.Read32(addr)
	c.Cycles++
}

func thLdrReg(c *CPU, op uint32) {
	o, ro, rb, rd := op>>10&3, op>>6&7, op>>3&7, op&7
	r := &c.R
	addr := r[rb] + r[ro]
	bus := c.bus
	switch o {
	case 0:
		bus.Write32(addr, r[rd])
	case 1:
		bus.Write8(addr, r[rd]&0xff)
	case 2:
		r[rd] = ror(bus.Read32(addr), (addr&3)*8)
		c.Cycles++
	default:
		r[rd] = bus.Read8(addr)
		c.Cycles++
	}
}

func thLdrH(c *CPU, op uint32) {
	o, ro, rb, rd := op>>10&3, op>>6&7, op>>3&7, op&7
	r := &c.R
	addr := r[rb] + r[ro]
	bus := c.bus
	switch o {
	case 0:
		bus.Write16(addr, r[rd]&0xffff)
	case 1:
		r[rd] = sx8(bus.Read8(addr))
		c.Cycles++
	case 2:
		r[rd] = ror(bus.Read16(addr), (addr&1)*8)
		c.Cycles++
	default:
		if addr&1 != 0 {
			r[rd] = sx8(bus.Read16(addr) >> 8)
		} else {
			r[rd] = sx16(bus.Read16(addr))
		}
		c.Cycles++
	}
}

func thLdrImm(c *CPU, op uint32) {
	B, L, off, rb, rd := op>>12&1 != 0, op>>11&1 != 0, op>>6&31, op>>3&7, op&7
	r := &c.R
	bus := c.bus
	if !B {
		off *= 4
	}
	addr := r[rb] + off
	if L {
		if B {
			r[rd] = bus.Read8(addr)
		} else {
			r[rd] = ror(bus.Read32(addr), (addr&3)*8)
		}
		c.Cycles++
	} else if B {
		bus.Write8(addr, r[rd]&0xff)
	} else {
		bus.Write32(addr, r[rd])
	}
}

func thLdrHImm(c *CPU, op uint32) {
	L, off, rb, rd := op>>11&1 != 0, (op>>6&31)*2, op>>3&7, op&7
	r := &c.R
	addr := r[rb] + off
	if L {
		r[rd] = ror(c.bus.Read16(addr), (addr&1)*8)
		c.Cycles++
	} else {
		c.bus.Write16(addr, r[rd]&0xffff)
	}
}

func thLdrSp(c *CPU, op uint32) {
	L, rd := op>>11&1 != 0, op>>8&7
	r := &c.R
	addr := r[13] + (op&0xff)*4
	if L {
		r[rd] = ror(c.bus.Read32(addr), (addr&3)*8)
		c.Cycles++
	} else {
		c.bus.Write32(addr, r[rd])
	}
}

func thAddr(c *CPU, op uint32) {
	sp, rd := op>>11&1 != 0, op>>8&7
	base := c.R[15] &^ 2
	if sp {
		base = c.R[13]
	}
	c.R[rd] = base + (op&0xff)*4
}

func thAddSp(c *CPU, op uint32) {
	v := (op & 0x7f) * 4
	if op&0x80 != 0 {
		c.R[13] -= v
	} else {
		c.R[13] += v
	}
}

func thPushPop(c *CPU, op uint32) {
	L, R := op>>11&1 != 0, op>>8&1 != 0
	r := &c.R
	bus := c.bus
	list := op & 0xff
	if L {
		if list == 0 && !R { // empty list quirk
			v := bus.Read32(r[13])
			r[13] += 0x40
			c.Branch(v)
			return
		}
		a := r[13]
		for k := 0; k < 8; k++ {
			if list&(1<<k) != 0 {
				r[k] = bus.Read32(a)
				a += 4
			}
		}
		if R {
			v := bus.Read32(a)
			a += 4
			r[13] = a
			c.Cycles++
			c.Branch(v)
			return
		}
		r[13] = a
		c.Cycles++
		return
	}
	if list == 0 && !R {
		r[13] -= 0x40
		bus.Write32(r[13], r[15]+2)
		return
	}
	n := popCount16(list)
	if R {
		n++
	}
	a := r[13] - uint32(n*4)
	r[13] = a
	for k := 0; k < 8; k++ {
		if list&(1<<k) != 0 {
			bus.Write32(a, r[k])
			a += 4
		}
	}
	if R {
		bus.Write32(a, r[14])
	}
}

func thBlock(c *CPU, op uint32) {
	L, rb, list := op>>11&1 != 0, op>>8&7, op&0xff
	r := &c.R
	bus := c.bus
	if list == 0 {
		if L {
			v := bus.Read32(r[rb])
			r[rb] += 0x40
			c.Branch(v)
		} else {
			bus.Write32(r[rb], r[15]+2)
			r[rb] += 0x40
		}
		return
	}
	n := popCount16(list)
	a := r[rb]
	newBase := r[rb] + uint32(n*4)
	if L {
		r[rb] = newBase
		for k := 0; k < 8; k++ {
			if list&(1<<k) != 0 {
				r[k] = bus.Read32(a)
				a += 4
			}
		}
		c.Cycles++
		return
	}
	first := true
	for k := 0; k < 8; k++ {
		if list&(1<<k) != 0 {
			v := r[k]
			if uint32(k) == rb && !first {
				v = newBase
			}
			bus.Write32(a, v)
			a += 4
			first = false
		}
	}
	r[rb] = newBase
}

func thBcond(c *CPU, op uint32) {
	cond := op >> 8 & 15
	if cond == 15 {
		c.swi(op & 0xff)
		return
	}
	if c.condPass(cond) {
		c.Branch(c.R[15] + uint32(int32((op&0xff)<<24)>>23))
	}
}

func thB(c *CPU, op uint32) { c.Branch(c.R[15] + uint32(int32((op&0x7ff)<<21)>>20)) }

func thBLhi(c *CPU, op uint32) { c.R[14] = c.R[15] + uint32(int32((op&0x7ff)<<21)>>9) }

func thBLlo(c *CPU, op uint32) {
	target := c.R[14] + (op&0x7ff)<<1
	c.R[14] = c.PC | 1
	c.Branch(target)
}

func thUndef(c *CPU, _ uint32) { c.undefinedInstr() }

func buildTables() {
	for i := 0; i < 4096; i++ {
		hi, lo := i>>4, i&15 // bits 27-20, bits 7-4
		f := opFn(armUndefined)
		switch hi >> 5 {
		case 0:
			switch {
			case lo == 9:
				switch {
				case hi&0xfc == 0x00:
					f = armMul
				case hi&0xf8 == 0x08:
					f = armMull
				case hi&0xfb == 0x10:
					f = armSwp
				}
			case lo&9 == 9:
				f = armHalf
			case hi&0xf9 == 0x10: // TST/TEQ/CMP/CMN without S: PSR transfer / BX
				if hi == 0x12 && lo == 1 {
					f = armBX
				} else if lo == 0 {
					if hi&2 != 0 {
						f = armMSR
					} else {
						f = armMRS
					}
				}
			default:
				f = armDP
			}
		case 1:
			if hi&0xf9 == 0x30 {
				if hi&2 != 0 {
					f = armMSR
				}
			} else {
				f = armDP
			}
		case 2:
			f = armSDT
		case 3:
			if lo&1 == 0 {
				f = armSDT
			}
		case 4:
			f = armBlock
		case 5:
			f = armBranch
		case 6:
		default:
			if hi&0x10 != 0 {
				f = armSWI
			}
		}
		armTable[i] = f
	}
	for i := 0; i < 1024; i++ {
		op := i << 6
		f := opFn(thUndef)
		switch {
		case op&0xf800 == 0x1800:
			f = thAddSub
		case op&0xe000 == 0x0000:
			f = thShiftImm
		case op&0xe000 == 0x2000:
			f = thImm
		case op&0xfc00 == 0x4000:
			f = thAlu
		case op&0xfc00 == 0x4400:
			f = thHi
		case op&0xf800 == 0x4800:
			f = thLdrPc
		case op&0xf200 == 0x5000:
			f = thLdrReg
		case op&0xf200 == 0x5200:
			f = thLdrH
		case op&0xe000 == 0x6000:
			f = thLdrImm
		case op&0xf000 == 0x8000:
			f = thLdrHImm
		case op&0xf000 == 0x9000:
			f = thLdrSp
		case op&0xf000 == 0xa000:
			f = thAddr
		case op&0xff00 == 0xb000:
			f = thAddSp
		case op&0xf600 == 0xb400:
			f = thPushPop
		case op&0xf000 == 0xc000:
			f = thBlock
		case op&0xf000 == 0xd000:
			f = thBcond
		case op&0xf800 == 0xe000:
			f = thB
		case op&0xf800 == 0xf000:
			f = thBLhi
		case op&0xf800 == 0xf800:
			f = thBLlo
		}
		thumbTable[i] = f
	}
}
