package m68k

// The instruction table: each of the 65,536 opcodes gets a small function
// with its operands already decoded (buildTable in m68k.js).

// eaOk: which addressing modes an instruction accepts.
//
//	mode 0 Dn, 1 An, 2 (An), 3 (An)+, 4 -(An), 5 d16(An), 6 d8(An,Xn),
//	7.0 abs.W, 7.1 abs.L, 7.2 d16(PC), 7.3 d8(PC,Xn), 7.4 #imm
type eaClass int

const (
	eaAll eaClass = iota
	eaData
	eaMem
	eaAlt
	eaDataAlt
	eaMemAlt
	eaCtrl
	eaCtrlAlt
	eaCtrlPostinc
)

func eaOk(mode, reg int, cls eaClass) bool {
	if mode == 7 && reg > 4 {
		return false
	}
	isPC := mode == 7 && (reg == 2 || reg == 3)
	isImm := mode == 7 && reg == 4
	switch cls {
	case eaAll:
		return true
	case eaData:
		return mode != 1
	case eaMem:
		return mode >= 2
	case eaAlt:
		return !isPC && !isImm
	case eaDataAlt:
		return mode != 1 && !isPC && !isImm
	case eaMemAlt:
		return mode >= 2 && !isPC && !isImm
	case eaCtrl:
		return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3)
	case eaCtrlAlt:
		return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1)
	case eaCtrlPostinc:
		return mode == 2 || mode == 3 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3)
	}
	return false
}

func buildTable() *[65536]opFunc {
	t := new([65536]opFunc)
	illegal := func(c *CPU) {
		if c.BranchHooks && (c.op>>12 == 0xa || c.op>>12 == 0xf) && c.op>>8&0xf != 1 {
			if d := int8(c.op); d < 0 && d != -1 {
				c.op = 0x6000 | c.op&0x0fff
				c.table[c.op](c)
				return
			}
		}
		switch c.op >> 12 {
		case 0xa:
			c.illegal(10)
		case 0xf:
			c.illegal(11)
		default:
			c.illegal(4)
		}
	}
	for op := 0; op < 65536; op++ {
		if f := decode(op); f != nil {
			t[op] = f
		} else {
			t[op] = illegal
		}
	}
	return t
}

var size2 = [3]int{1, 2, 4} // size field 00/01/10

// longExtra: internal cycles for long ops on a data register vs memory source.
func longExtra(mode, reg int) int64 {
	if mode == 0 || mode == 1 || (mode == 7 && reg == 4) {
		return 4
	}
	return 2
}

func decode(op int) opFunc {
	hi := op >> 12
	rx, opm, mode, reg := (op>>9)&7, (op>>6)&7, (op>>3)&7, op&7
	szf := (op >> 6) & 3
	switch hi {
	case 0x0:
		return decode0(op, rx, mode, reg, szf)
	case 0x1, 0x2, 0x3:
		return decodeMove(hi, rx, opm, mode, reg)
	case 0x4:
		return decode4(op, rx, opm, mode, reg, szf)
	case 0x5:
		return decode5(op, rx, mode, reg, szf)
	case 0x6:
		return decodeBranch(op)
	case 0x7:
		if op&0x100 != 0 {
			return nil
		}
		v := sx8(uint32(op & 0xff))
		return func(c *CPU) { c.D[rx] = v; c.logic(v, 4) }
	case 0x8:
		return decodeOrDiv(rx, opm, mode, reg)
	case 0x9:
		return decodeAddSub(rx, opm, mode, reg, false)
	case 0xb:
		return decodeCmpEor(rx, opm, mode, reg)
	case 0xc:
		return decodeAndMul(rx, opm, mode, reg)
	case 0xd:
		return decodeAddSub(rx, opm, mode, reg, true)
	case 0xe:
		return decodeShift(op, rx, mode, reg, szf)
	}
	return nil // A-line / F-line: the illegal handler
}

// ---------------- group 0: bit operations, immediates, MOVEP ----------------

func decode0(op, rx, mode, reg, szf int) opFunc {
	// MOVEP
	if op&0x0138 == 0x0108 {
		toMem, long := (op>>7)&1 != 0, (op>>6)&1 != 0
		return func(c *CPU) {
			a := c.A[reg] + sx16(c.fetch())
			if toMem {
				v := c.D[rx]
				if long {
					c.w8(a, v>>24)
					c.w8(a+2, v>>16)
					a += 4
				}
				c.w8(a, v>>8)
				c.w8(a+2, v)
			} else if long {
				b3 := c.r8(a)
				b2 := c.r8(a + 2)
				b1 := c.r8(a + 4)
				b0 := c.r8(a + 6)
				c.D[rx] = b3<<24 | b2<<16 | b1<<8 | b0
			} else {
				b1 := c.r8(a)
				b0 := c.r8(a + 2)
				c.setD(rx, b1<<8|b0, 2)
			}
		}
	}
	// dynamic / static bit ops
	if op&0x100 != 0 || op&0x0f00 == 0x0800 {
		kind := (op >> 6) & 3 // 0 BTST, 1 BCHG, 2 BCLR, 3 BSET
		dynamic := op&0x100 != 0
		if mode == 1 {
			return nil
		}
		if kind == 0 {
			if !eaOk(mode, reg, eaData) || (!dynamic && mode == 7 && reg == 4) {
				return nil
			}
		} else if !eaOk(mode, reg, eaDataAlt) {
			return nil
		}
		return func(c *CPU) {
			var bit uint32
			if dynamic {
				bit = c.D[rx]
			} else {
				bit = c.fetch() & 0xff
			}
			if mode == 0 {
				bit &= 31
				m := uint32(1) << bit
				v := c.D[reg]
				c.z = b2u(v&m == 0)
				if kind == 0 {
					c.Cycles += 2
					return
				}
				switch kind {
				case 1:
					c.D[reg] = v ^ m
				case 2:
					c.D[reg] = v &^ m
				default:
					c.D[reg] = v | m
				}
				if kind == 2 {
					c.Cycles += 4
				} else {
					c.Cycles += 2
				}
				if bit >= 16 {
					c.Cycles += 2
				}
				return
			}
			bit &= 7
			m := uint32(1) << bit
			if kind == 0 {
				v := c.read(mode, reg, 1)
				c.z = b2u(v&m == 0)
				if mode == 7 && reg == 4 {
					c.Cycles += 2
				}
				return
			}
			a, v := c.rmwRead(mode, reg, 1)
			c.z = b2u(v&m == 0)
			switch kind {
			case 1:
				v ^= m
			case 2:
				v &^= m
			default:
				v |= m
			}
			c.rmwWrite(mode, reg, 1, a, v)
		}
	}
	kind := (op >> 9) & 7 // 0 ORI 1 ANDI 2 SUBI 3 ADDI 5 EORI 6 CMPI
	// to CCR / SR
	if op&0xff == 0x3c || op&0xff == 0x7c {
		if kind != 0 && kind != 1 && kind != 5 {
			return nil
		}
		toSR := op&0xff == 0x7c
		return func(c *CPU) {
			if toSR && c.s == 0 {
				c.privilege()
				return
			}
			imm := c.fetch()
			cur := c.getCCR()
			if toSR {
				cur = c.SR()
			}
			var v uint32
			switch kind {
			case 0:
				v = cur | imm
			case 1:
				v = cur & imm
			default:
				v = cur ^ imm
			}
			if toSR {
				c.SetSR(v & 0xa71f)
			} else {
				c.setCCR(v & 0x1f)
			}
			c.Cycles += 12
		}
	}
	if szf == 3 || kind == 4 || kind == 7 {
		return nil
	}
	sz := size2[szf]
	if !eaOk(mode, reg, eaDataAlt) {
		return nil
	}
	return func(c *CPU) {
		var imm uint32
		switch sz {
		case 4:
			imm = c.fetch32()
		case 2:
			imm = c.fetch()
		default:
			imm = c.fetch() & 0xff
		}
		if kind == 6 { // CMPI
			v := c.read(mode, reg, sz)
			r := c.sub(v, imm, sz, 0)
			c.z = b2u(r == 0)
			if sz == 4 && mode == 0 {
				c.Cycles += 2
			}
			return
		}
		if mode == 0 && sz == 4 {
			c.Cycles += 4
		}
		a, v := c.rmwRead(mode, reg, sz)
		var r uint32
		switch kind {
		case 0:
			r = clip(v|imm, sz)
			c.logic(r, sz)
		case 1:
			r = clip(v&imm, sz)
			c.logic(r, sz)
		case 5:
			r = clip(v^imm, sz)
			c.logic(r, sz)
		case 2:
			r = c.sub(v, imm, sz, 0)
			c.x = c.c
			c.z = b2u(r == 0)
		case 3:
			r = c.add(v, imm, sz, 0)
			c.x = c.c
			c.z = b2u(r == 0)
		}
		c.rmwWrite(mode, reg, sz, a, r)
	}
}

// ---------------- MOVE / MOVEA ----------------

func decodeMove(hi, rx, dmode, smode, sreg int) opFunc {
	sz := 4
	switch hi {
	case 1:
		sz = 1
	case 3:
		sz = 2
	}
	if !eaOk(smode, sreg, eaAll) || (sz == 1 && smode == 1) {
		return nil
	}
	if dmode == 1 { // MOVEA
		if sz == 1 {
			return nil
		}
		return func(c *CPU) {
			v := c.read(smode, sreg, sz)
			if sz == 2 {
				v = sx16(v)
			}
			c.A[rx] = v
		}
	}
	if !eaOk(dmode, rx, eaDataAlt) {
		return nil
	}
	return func(c *CPU) {
		v := c.read(smode, sreg, sz)
		if dmode == 0 {
			c.setD(rx, v, sz)
			c.logic(v, sz)
			return
		}
		var a uint32
		if dmode == 4 { // no extra time as a MOVE destination
			c.A[rx] -= step(sz, rx)
			a = c.A[rx]
		} else {
			a = c.addr(dmode, rx, sz)
		}
		c.logic(v, sz)
		c.wm(a, sz, v)
	}
}

// ---------------- group 4: miscellaneous ----------------

func decode4(op, rx, opm, mode, reg, szf int) opFunc {
	// LEA / CHK
	if opm == 7 {
		if !eaOk(mode, reg, eaCtrl) {
			return nil
		}
		return func(c *CPU) {
			a := c.addr(mode, reg, 4)
			if mode == 6 || (mode == 7 && reg == 3) {
				c.Cycles += 2
			}
			c.A[rx] = a
		}
	}
	if opm == 6 {
		if !eaOk(mode, reg, eaData) {
			return nil
		}
		return func(c *CPU) {
			bound := int32(int16(c.read(mode, reg, 2)))
			v := int32(int16(c.D[rx]))
			c.Cycles += 6
			c.z, c.v, c.c, c.n = b2u(v == 0), 0, 0, 0
			if v < 0 {
				c.n = 1
				c.trap(6, 10)
			} else if v > bound {
				c.trap(6, 8)
			}
		}
	}
	switch op & 0x0f00 {
	case 0x0000: // NEGX / MOVE from SR
		if szf == 3 {
			if !eaOk(mode, reg, eaDataAlt) {
				return nil
			}
			return func(c *CPU) {
				sr := c.SR()
				if mode == 0 {
					c.setD(reg, sr, 2)
					c.Cycles += 2
					return
				}
				a, _ := c.rmwRead(mode, reg, 2)
				c.rmwWrite(mode, reg, 2, a, sr)
			}
		}
		if !eaOk(mode, reg, eaDataAlt) {
			return nil
		}
		return unary(mode, reg, size2[szf], func(c *CPU, v uint32, sz int) uint32 {
			z := c.z
			r := c.sub(0, v, sz, c.x)
			c.x = c.c
			if r != 0 {
				z = 0
			}
			c.z = z
			return r
		})
	case 0x0200: // CLR
		if szf == 3 || !eaOk(mode, reg, eaDataAlt) {
			return nil
		}
		return unary(mode, reg, size2[szf], func(c *CPU, v uint32, sz int) uint32 {
			c.n, c.z, c.v, c.c = 0, 1, 0, 0
			return 0
		})
	case 0x0400: // NEG / MOVE to CCR
		if szf == 3 {
			if !eaOk(mode, reg, eaData) {
				return nil
			}
			return func(c *CPU) { c.setCCR(c.read(mode, reg, 2) & 0x1f); c.Cycles += 8 }
		}
		if !eaOk(mode, reg, eaDataAlt) {
			return nil
		}
		return unary(mode, reg, size2[szf], func(c *CPU, v uint32, sz int) uint32 {
			r := c.sub(0, v, sz, 0)
			c.x = c.c
			c.z = b2u(r == 0)
			return r
		})
	case 0x0600: // NOT / MOVE to SR
		if szf == 3 {
			if !eaOk(mode, reg, eaData) {
				return nil
			}
			return func(c *CPU) {
				if c.s == 0 {
					c.privilege()
					return
				}
				c.SetSR(c.read(mode, reg, 2) & 0xa71f)
				c.Cycles += 8
			}
		}
		if !eaOk(mode, reg, eaDataAlt) {
			return nil
		}
		return unary(mode, reg, size2[szf], func(c *CPU, v uint32, sz int) uint32 {
			r := clip(^v, sz)
			c.logic(r, sz)
			return r
		})
	case 0x0800:
		if szf == 0 { // NBCD
			if !eaOk(mode, reg, eaDataAlt) {
				return nil
			}
			return func(c *CPU) {
				if mode == 0 {
					c.Cycles += 2
				}
				a, v := c.rmwRead(mode, reg, 1)
				c.rmwWrite(mode, reg, 1, a, bcdSub(c, 0, v))
			}
		}
		if szf == 1 {
			if mode == 0 { // SWAP
				return func(c *CPU) {
					v := c.D[reg]
					r := v>>16 | v<<16
					c.D[reg] = r
					c.logic(r, 4)
				}
			}
			if !eaOk(mode, reg, eaCtrl) {
				return nil
			}
			return func(c *CPU) { // PEA
				a := c.addr(mode, reg, 4)
				if mode == 6 || (mode == 7 && reg == 3) {
					c.Cycles += 2
				}
				c.push32(a)
			}
		}
		if mode == 0 { // EXT
			if szf == 2 {
				return func(c *CPU) {
					r := sx8(c.D[reg]&0xff) & 0xffff
					c.setD(reg, r, 2)
					c.logic(r, 2)
				}
			}
			return func(c *CPU) {
				r := sx16(c.D[reg] & 0xffff)
				c.D[reg] = r
				c.logic(r, 4)
			}
		}
		if !eaOk(mode, reg, eaCtrlAlt) && mode != 4 {
			return nil
		}
		if szf == 3 {
			return movemToMem(mode, reg, 4)
		}
		return movemToMem(mode, reg, 2)
	case 0x0a00: // TST / TAS / ILLEGAL
		if op == 0x4afc {
			return nil
		}
		if szf == 3 {
			if !eaOk(mode, reg, eaDataAlt) {
				return nil
			}
			return func(c *CPU) {
				if mode == 0 {
					v := c.D[reg] & 0xff
					c.logic(v, 1)
					c.D[reg] |= 0x80
					return
				}
				a := c.addr(mode, reg, 1)
				v := c.r8(a)
				c.logic(v, 1)
				c.Cycles += 2
				if c.TASWrites {
					c.w8(a, v|0x80)
				} else {
					c.Cycles += 4
				}
			}
		}
		if !eaOk(mode, reg, eaAll) || (szf == 0 && mode == 1) {
			return nil
		}
		sz := size2[szf]
		return func(c *CPU) { c.logic(c.read(mode, reg, sz), sz) }
	case 0x0c00: // MOVEM to registers
		if szf < 2 {
			return nil
		}
		if !eaOk(mode, reg, eaCtrlPostinc) {
			return nil
		}
		if szf == 3 {
			return movemToReg(mode, reg, 4)
		}
		return movemToReg(mode, reg, 2)
	case 0x0e00:
		if op&0xfff0 == 0x4e40 { // TRAP
			v := uint32(op & 15)
			return func(c *CPU) { c.trap(32+v, 10) }
		}
		if op&0xfff8 == 0x4e50 { // LINK
			return func(c *CPU) {
				disp := sx16(c.fetch())
				c.push32(c.A[reg]) // (LINK A7 stores A7's value from before the push)
				c.A[reg] = c.A[7]
				c.A[7] += disp
			}
		}
		if op&0xfff8 == 0x4e58 { // UNLK
			return func(c *CPU) {
				c.A[7] = c.A[reg]
				v := c.pop32()
				c.A[reg] = v
			}
		}
		if op&0xfff8 == 0x4e60 {
			return func(c *CPU) {
				if c.s == 0 {
					c.privilege()
					return
				}
				c.USP = c.A[reg]
			}
		}
		if op&0xfff8 == 0x4e68 {
			return func(c *CPU) {
				if c.s == 0 {
					c.privilege()
					return
				}
				c.A[reg] = c.USP
			}
		}
		switch op {
		case 0x4e70: // RESET
			return func(c *CPU) {
				if c.s == 0 {
					c.privilege()
					return
				}
				if c.reset != nil {
					c.reset.Reset()
				}
				c.Cycles += 128
			}
		case 0x4e71: // NOP
			return func(c *CPU) {}
		case 0x4e72: // STOP
			return func(c *CPU) {
				if c.s == 0 {
					c.privilege()
					return
				}
				v := c.fetch()
				c.SetSR(v & 0xa71f)
				c.Stopped = true
			}
		case 0x4e73: // RTE
			return func(c *CPU) {
				if c.s == 0 {
					c.privilege()
					return
				}
				sr := c.pop16()
				pc := c.pop32()
				c.SetSR(sr & 0xa71f)
				c.PC = pc
				c.Cycles += 4
			}
		case 0x4e75: // RTS
			return func(c *CPU) { c.PC = c.pop32(); c.Cycles += 4 }
		case 0x4e76: // TRAPV
			return func(c *CPU) {
				if c.v != 0 {
					c.trap(7, 10)
				}
			}
		case 0x4e77: // RTR
			return func(c *CPU) {
				ccr := c.pop16()
				c.setCCR(ccr & 0x1f)
				c.PC = c.pop32()
				c.Cycles += 4
			}
		}
		if op&0xffc0 == 0x4e80 { // JSR
			if !eaOk(mode, reg, eaCtrl) {
				return nil
			}
			extra := jumpExtra(mode, reg)
			return func(c *CPU) {
				a := c.addr(mode, reg, 4)
				c.Cycles += extra
				c.push32(c.PC)
				c.PC = a
			}
		}
		if op&0xffc0 == 0x4ec0 { // JMP
			if !eaOk(mode, reg, eaCtrl) {
				return nil
			}
			extra := jumpExtra(mode, reg)
			return func(c *CPU) {
				a := c.addr(mode, reg, 4)
				c.Cycles += extra
				c.PC = a
			}
		}
		return nil
	}
	return nil
}

// jumpExtra: extra internal time JMP/JSR take for each addressing mode.
func jumpExtra(mode, reg int) int64 {
	switch mode {
	case 2:
		return 4
	case 5:
		return 2
	case 6:
		return 4
	case 7:
		switch reg {
		case 0:
			return 2
		case 1:
			return 0
		case 2:
			return 2
		}
		return 4
	}
	return 0
}

// unary: single-operand instructions (CLR, NEG, NEGX, NOT); registers take 2
// extra for long.
func unary(mode, reg, sz int, fn func(c *CPU, v uint32, sz int) uint32) opFunc {
	return func(c *CPU) {
		if mode == 0 {
			if sz == 4 {
				c.Cycles += 2
			}
			c.setD(reg, fn(c, clip(c.D[reg], sz), sz), sz)
			return
		}
		a, v := c.rmwRead(mode, reg, sz)
		c.rmwWrite(mode, reg, sz, a, fn(c, v, sz))
	}
}

func movemToMem(mode, reg, sz int) opFunc {
	return func(c *CPU) {
		m := c.fetch()
		if mode == 4 {
			a := c.A[reg]
			start := a
			for i := 0; i < 16; i++ {
				if m&(1<<i) == 0 {
					continue
				}
				r := 15 - i
				a -= uint32(sz)
				// the address register being decremented is stored with its original value
				var v uint32
				switch {
				case r >= 8 && r-8 == reg:
					v = start
				case r >= 8:
					v = c.A[r-8]
				default:
					v = c.D[r]
				}
				if sz == 4 {
					c.w16(a+2, v&0xffff)
					c.w16(a, v>>16)
				} else {
					c.w16(a, v)
				}
			}
			c.A[reg] = a
			return
		}
		a := c.addr(mode, reg, sz)
		for i := 0; i < 16; i++ {
			if m&(1<<i) == 0 {
				continue
			}
			var v uint32
			if i >= 8 {
				v = c.A[i-8]
			} else {
				v = c.D[i]
			}
			c.wm(a, sz, v)
			a += uint32(sz)
		}
	}
}

func movemToReg(mode, reg, sz int) opFunc {
	return func(c *CPU) {
		m := c.fetch()
		var a uint32
		if mode == 3 {
			a = c.A[reg]
		} else {
			a = c.addr(mode, reg, sz)
		}
		for i := 0; i < 16; i++ {
			if m&(1<<i) == 0 {
				continue
			}
			var v uint32
			if sz == 4 {
				v = c.r32(a)
			} else {
				v = sx16(c.r16(a))
			}
			if i >= 8 {
				c.A[i-8] = v
			} else {
				c.D[i] = v
			}
			a += uint32(sz)
		}
		c.r16(a) // one extra read at the end
		if mode == 3 {
			c.A[reg] = a
		}
	}
}

// ---------------- group 5: ADDQ / SUBQ / Scc / DBcc ----------------

func decode5(op, rx, mode, reg, szf int) opFunc {
	cc := uint32(op>>8) & 15
	if szf == 3 {
		if mode == 1 { // DBcc
			return func(c *CPU) {
				base := c.PC
				disp := sx16(c.fetch())
				if c.cond(cc) {
					c.Cycles += 4
					return
				}
				v := (c.D[reg] - 1) & 0xffff
				c.setD(reg, v, 2)
				if v != 0xffff {
					c.PC = base + disp
					c.Cycles += 2
				} else {
					c.Cycles += 6
				}
			}
		}
		if !eaOk(mode, reg, eaDataAlt) {
			return nil
		}
		return func(c *CPU) { // Scc
			t := c.cond(cc)
			v := uint32(0)
			if t {
				v = 0xff
			}
			if mode == 0 {
				c.setD(reg, v, 1)
				if t {
					c.Cycles += 2
				}
				return
			}
			a, _ := c.rmwRead(mode, reg, 1)
			c.rmwWrite(mode, reg, 1, a, v)
		}
	}
	q := uint32(rx)
	if q == 0 {
		q = 8
	}
	sub, sz := op&0x100 != 0, size2[szf]
	if !eaOk(mode, reg, eaAlt) || (mode == 1 && sz == 1) {
		return nil
	}
	if mode == 1 {
		return func(c *CPU) {
			if sub {
				c.A[reg] -= q
			} else {
				c.A[reg] += q
			}
			c.Cycles += 4
		}
	}
	return func(c *CPU) {
		if mode == 0 && sz == 4 {
			c.Cycles += 4
		}
		a, v := c.rmwRead(mode, reg, sz)
		var r uint32
		if sub {
			r = c.sub(v, q, sz, 0)
		} else {
			r = c.add(v, q, sz, 0)
		}
		c.x = c.c
		c.z = b2u(r == 0)
		c.rmwWrite(mode, reg, sz, a, r)
	}
}

// ---------------- group 6: Bcc / BRA / BSR ----------------

func decodeBranch(op int) opFunc {
	cc, d8 := uint32(op>>8)&15, uint32(op&0xff)
	return func(c *CPU) {
		base := c.PC
		var disp uint32
		if d8 == 0 {
			disp = sx16(c.fetch())
		} else {
			disp = sx8(d8)
		}
		if cc == 1 { // BSR
			if d8 == 0 {
				c.Cycles += 2
			} else {
				c.Cycles += 6
			}
			c.push32(c.PC)
			c.PC = base + disp
			return
		}
		if c.cond(cc) {
			c.PC = base + disp
			if d8 == 0 {
				c.Cycles += 2
			} else {
				c.Cycles += 6
			}
		} else {
			c.Cycles += 4
		}
	}
}

// ---------------- OR / DIVU / DIVS / SBCD ----------------

func decodeOrDiv(rx, opm, mode, reg int) opFunc {
	if opm == 3 || opm == 7 {
		if !eaOk(mode, reg, eaData) {
			return nil
		}
		signed := opm == 7
		return func(c *CPU) {
			src := c.read(mode, reg, 2)
			divide(c, rx, src, signed)
		}
	}
	if opm == 4 && mode <= 1 {
		return bcdOp(rx, mode, reg, false)
	}
	return logicOp(rx, opm, mode, reg, 0)
}

func divide(c *CPU, rx int, src uint32, signed bool) {
	dividend := c.D[rx]
	if src == 0 {
		c.Cycles += 4
		c.v, c.c, c.n, c.z = 0, 0, 0, 0
		c.trap(5, 10)
		return
	}
	if !signed {
		q, r := dividend/src, dividend%src
		c.Cycles += divuTime(dividend, src)
		if q > 0xffff {
			c.v, c.n, c.z, c.c = 1, 1, 0, 0
			return
		}
		c.D[rx] = r<<16 | q
		c.n, c.z, c.v, c.c = (q>>15)&1, b2u(q == 0), 0, 0
		return
	}
	dv, sv := int64(int32(dividend)), int64(int16(src))
	q := dv / sv
	r := dv - q*sv
	c.Cycles += divsTime(dv, sv)
	if q > 32767 || q < -32768 {
		c.v, c.n, c.z, c.c = 1, 1, 0, 0
		return
	}
	c.D[rx] = (uint32(r)&0xffff)<<16 | uint32(q)&0xffff
	c.n, c.z, c.v, c.c = uint32(q>>15)&1, b2u(q == 0), 0, 0
}

// Division takes a data-dependent time: the chip works out the quotient one
// bit at a time, and how long a bit takes depends on that bit and on the
// partial remainder before it. These return the instruction's time with a
// register operand, minus the 4-cycle opcode fetch.
//
// DIVU: 72 cycles, plus for each quotient bit 15..1: nothing when the partial
// remainder going in had its top bit set, otherwise 2 for a 1 bit, 4 for a 0
// bit. An overflow (quotient wider than 16 bits) is caught after 6 cycles.
func divuTime(dividend, divisor uint32) int64 {
	if dividend>>16 >= divisor {
		return 6
	}
	q := dividend / divisor
	t := int64(72)
	for bit := 15; bit >= 1; bit-- {
		partial := (dividend >> (bit + 1)) % divisor
		if partial < 0x8000 {
			if (q>>bit)&1 != 0 {
				t += 2
			} else {
				t += 4
			}
		}
	}
	return t
}

// DIVS: a base time set by the operands' signs, plus 2 cycles for every 0
// among bits 15..1 of the unsigned quotient. Overflow: 12 cycles (14 for a
// negative dividend).
func divsTime(dividend, divisor int64) int64 {
	negN, negD := 0, 0
	n, d := dividend, divisor
	if n < 0 {
		negN, n = 1, -n
	}
	if d < 0 {
		negD, d = 1, -d
	}
	if n>>16 >= d {
		return 12 + 2*int64(negN)
	}
	q := (n / d >> 1) & 0x7fff
	ones := int64(0)
	for ; q != 0; q &= q - 1 {
		ones++
	}
	base := [2][2]int64{{116, 118}, {122, 120}}[negN][negD]
	return base + 2*(15-ones)
}

// logicOp: OR (kind 0) or AND (kind 1) between Dn and <ea>.
func logicOp(rx, opm, mode, reg, kind int) opFunc {
	sz, toMem := size2[opm&3], opm >= 4
	fn := func(a, b uint32) uint32 {
		if kind == 0 {
			return a | b
		}
		return a & b
	}
	if toMem {
		if !eaOk(mode, reg, eaMemAlt) {
			return nil
		}
		return func(c *CPU) {
			a, v := c.rmwRead(mode, reg, sz)
			r := clip(fn(v, c.D[rx]), sz)
			c.logic(r, sz)
			c.rmwWrite(mode, reg, sz, a, r)
		}
	}
	if !eaOk(mode, reg, eaData) {
		return nil
	}
	extra := longExtra(mode, reg)
	return func(c *CPU) {
		v := c.read(mode, reg, sz)
		r := clip(fn(c.D[rx], v), sz)
		c.setD(rx, r, sz)
		c.logic(r, sz)
		if sz == 4 {
			c.Cycles += extra
		}
	}
}

// ---------------- ADD / SUB / ADDA / SUBA / ADDX / SUBX ----------------

func decodeAddSub(rx, opm, mode, reg int, isAdd bool) opFunc {
	if opm == 3 || opm == 7 { // ADDA / SUBA
		if !eaOk(mode, reg, eaAll) {
			return nil
		}
		sz := 2
		if opm == 7 {
			sz = 4
		}
		extra := int64(4)
		if sz == 4 {
			extra = longExtra(mode, reg)
		}
		return func(c *CPU) {
			v := c.read(mode, reg, sz)
			if sz == 2 {
				v = sx16(v)
			}
			if isAdd {
				c.A[rx] += v
			} else {
				c.A[rx] -= v
			}
			c.Cycles += extra
		}
	}
	sz := size2[opm&3]
	if opm >= 4 && mode <= 1 { // ADDX / SUBX
		return func(c *CPU) {
			var s, d, a uint32
			if mode == 0 {
				s, d = clip(c.D[reg], sz), clip(c.D[rx], sz)
				if sz == 4 {
					c.Cycles += 4
				}
			} else {
				c.A[reg] -= step(sz, reg)
				s = c.rm(c.A[reg], sz)
				c.A[rx] -= step(sz, rx)
				a = c.A[rx]
				d = c.rm(a, sz)
				c.Cycles += 2
			}
			z := c.z
			var r uint32
			if isAdd {
				r = c.add(d, s, sz, c.x)
			} else {
				r = c.sub(d, s, sz, c.x)
			}
			c.x = c.c
			if r != 0 {
				z = 0
			}
			c.z = z
			if mode == 0 {
				c.setD(rx, r, sz)
			} else {
				c.wm(a, sz, r)
			}
		}
	}
	if opm >= 4 { // Dn op <ea> -> <ea>
		if !eaOk(mode, reg, eaMemAlt) {
			return nil
		}
		return func(c *CPU) {
			a, v := c.rmwRead(mode, reg, sz)
			var r uint32
			if isAdd {
				r = c.add(v, clip(c.D[rx], sz), sz, 0)
			} else {
				r = c.sub(v, clip(c.D[rx], sz), sz, 0)
			}
			c.x = c.c
			c.z = b2u(r == 0)
			c.rmwWrite(mode, reg, sz, a, r)
		}
	}
	if !eaOk(mode, reg, eaAll) || (sz == 1 && mode == 1) {
		return nil
	}
	extra := longExtra(mode, reg)
	return func(c *CPU) {
		v, d := c.read(mode, reg, sz), clip(c.D[rx], sz)
		var r uint32
		if isAdd {
			r = c.add(d, v, sz, 0)
		} else {
			r = c.sub(d, v, sz, 0)
		}
		c.x = c.c
		c.z = b2u(r == 0)
		c.setD(rx, r, sz)
		if sz == 4 {
			c.Cycles += extra
		}
	}
}

// ---------------- CMP / CMPA / CMPM / EOR ----------------

func decodeCmpEor(rx, opm, mode, reg int) opFunc {
	if opm == 3 || opm == 7 { // CMPA
		if !eaOk(mode, reg, eaAll) {
			return nil
		}
		sz := 2
		if opm == 7 {
			sz = 4
		}
		return func(c *CPU) {
			v := c.read(mode, reg, sz)
			if sz == 2 {
				v = sx16(v)
			}
			r := c.sub(c.A[rx], v, 4, 0)
			c.z = b2u(r == 0)
			c.Cycles += 2
		}
	}
	sz := size2[opm&3]
	if opm < 3 { // CMP
		if !eaOk(mode, reg, eaAll) || (sz == 1 && mode == 1) {
			return nil
		}
		return func(c *CPU) {
			v := c.read(mode, reg, sz)
			r := c.sub(clip(c.D[rx], sz), v, sz, 0)
			c.z = b2u(r == 0)
			if sz == 4 {
				c.Cycles += 2
			}
		}
	}
	if mode == 1 { // CMPM (Ay)+,(Ax)+
		return func(c *CPU) {
			s := c.rm(c.addr(3, reg, sz), sz)
			d := c.rm(c.addr(3, rx, sz), sz)
			r := c.sub(d, s, sz, 0)
			c.z = b2u(r == 0)
		}
	}
	if !eaOk(mode, reg, eaDataAlt) { // EOR Dn,<ea>
		return nil
	}
	if mode == 0 {
		return func(c *CPU) {
			r := clip(c.D[reg]^c.D[rx], sz)
			c.setD(reg, r, sz)
			c.logic(r, sz)
			if sz == 4 {
				c.Cycles += 4
			}
		}
	}
	return func(c *CPU) {
		a, v := c.rmwRead(mode, reg, sz)
		r := clip(v^c.D[rx], sz)
		c.logic(r, sz)
		c.rmwWrite(mode, reg, sz, a, r)
	}
}

// ---------------- AND / MULU / MULS / ABCD / EXG ----------------

func decodeAndMul(rx, opm, mode, reg int) opFunc {
	if opm == 3 || opm == 7 {
		if !eaOk(mode, reg, eaData) {
			return nil
		}
		signed := opm == 7
		return func(c *CPU) {
			s := c.read(mode, reg, 2)
			var r, bits uint32
			if signed {
				r = uint32(int32(int16(s)) * int32(int16(c.D[rx])))
				bits = popcount16((s ^ s<<1) & 0xffff)
			} else {
				r = s * (c.D[rx] & 0xffff)
				bits = popcount16(s)
			}
			c.D[rx] = r
			c.logic(r, 4)
			c.Cycles += 34 + 2*int64(bits)
		}
	}
	if opm == 4 && mode <= 1 {
		return bcdOp(rx, mode, reg, true)
	}
	if opm == 5 && mode == 0 {
		return func(c *CPU) { c.D[rx], c.D[reg] = c.D[reg], c.D[rx]; c.Cycles += 2 }
	}
	if opm == 5 && mode == 1 {
		return func(c *CPU) { c.A[rx], c.A[reg] = c.A[reg], c.A[rx]; c.Cycles += 2 }
	}
	if opm == 6 && mode == 1 {
		return func(c *CPU) { c.D[rx], c.A[reg] = c.A[reg], c.D[rx]; c.Cycles += 2 }
	}
	if opm == 6 && mode == 0 {
		return nil
	}
	return logicOp(rx, opm, mode, reg, 1)
}

// bcdOp: ABCD / SBCD.
func bcdOp(rx, mode, reg int, isAdd bool) opFunc {
	return func(c *CPU) {
		var s, d, a uint32
		if mode == 0 {
			s, d = c.D[reg]&0xff, c.D[rx]&0xff
			c.Cycles += 2
		} else {
			c.A[reg] -= step(1, reg)
			s = c.r8(c.A[reg])
			c.A[rx] -= step(1, rx)
			a = c.A[rx]
			d = c.r8(a)
			c.Cycles += 2
		}
		var r uint32
		if isAdd {
			r = bcdAdd(c, d, s)
		} else {
			r = bcdSub(c, d, s)
		}
		if mode == 0 {
			c.setD(rx, r, 1)
		} else {
			c.w8(a, r)
		}
	}
}

// Decimal add/subtract, including what the chip does with invalid (non-BCD)
// digits and the officially undefined N and V flags: the binary result is
// corrected by 6 in each nibble that produced a binary or decimal carry.
func bcdAdd(c *CPU, d32, s32 uint32) uint32 {
	d, s := int(d32), int(s32)
	sum := d + s + int(c.x)
	carries := ((s & d) | (^sum & s) | (^sum & d)) & 0x88 // binary carry out of each nibble
	decimal := (((sum + 0x66) ^ sum) & 0x110) >> 1        // nibbles that went past 9
	both := carries | decimal
	res := sum + both - (both >> 2) // add 6 per flagged nibble
	c.c = uint32(((carries | (sum &^ res)) >> 7) & 1)
	c.x = c.c
	c.v = uint32(((^sum & res) >> 7) & 1)
	c.n = uint32((res >> 7) & 1)
	if res&0xff != 0 {
		c.z = 0
	}
	return uint32(res & 0xff)
}

func bcdSub(c *CPU, d32, s32 uint32) uint32 {
	d, s := int(d32), int(s32)
	diff := d - s - int(c.x)
	borrows := ((^d & s) | (diff &^ d) | (diff & s)) & 0x88 // binary borrow out of each nibble
	res := diff - (borrows - (borrows >> 2))                // subtract 6 per borrowing nibble
	c.c = uint32(((borrows | (^diff & res)) >> 7) & 1)
	c.x = c.c
	c.v = uint32(((diff &^ res) >> 7) & 1)
	c.n = uint32((res >> 7) & 1)
	if res&0xff != 0 {
		c.z = 0
	}
	return uint32(res & 0xff)
}

// ---------------- shifts and rotates ----------------

func decodeShift(op, rx, mode, reg, szf int) opFunc {
	if szf == 3 { // memory, one bit, word
		typ, left := (op>>9)&3, op&0x100 != 0
		if op&0x0800 != 0 || !eaOk(mode, reg, eaMemAlt) {
			return nil
		}
		return func(c *CPU) {
			a, v := c.rmwRead(mode, reg, 2)
			c.rmwWrite(mode, reg, 2, a, shift(c, typ, left, v, 1, 2))
		}
	}
	sz, typ, left, byReg := size2[szf], (op>>3)&3, op&0x100 != 0, op&0x20 != 0
	base := int64(2)
	if sz == 4 {
		base = 4
	}
	return func(c *CPU) {
		var n uint32
		if byReg {
			n = c.D[rx] & 63
		} else {
			n = uint32(rx)
			if n == 0 {
				n = 8
			}
		}
		c.Cycles += base + 2*int64(n)
		c.setD(reg, shift(c, typ, left, clip(c.D[reg], sz), n, sz), sz)
	}
}

func shift(c *CPU, typ int, left bool, v32, n uint32, sz int) uint32 {
	top := uint64(1) << (bitsOf[sz] - 1)
	m := uint64(mask[sz])
	v := uint64(v32)
	msb := func(v uint64) uint32 { return uint32(v>>(bitsOf[sz]-1)) & 1 }
	var carry, vf uint32
	if n == 0 {
		if typ == 2 {
			c.c = c.x
		} else {
			c.c = 0
		}
		c.v = 0
		c.nz(v32, sz)
		return v32
	}
	switch typ {
	case 0: // ASL / ASR
		if left {
			for i := uint32(0); i < n; i++ {
				carry = msb(v)
				nv := (v * 2) & m
				if msb(nv) != msb(v) {
					vf = 1
				}
				v = nv
			}
		} else {
			sign := msb(v)
			for i := uint32(0); i < n; i++ {
				carry = uint32(v & 1)
				v = v / 2
				if sign != 0 {
					v += top
				}
				v &= m
			}
		}
		c.x = carry
	case 1: // LSL / LSR
		for i := uint32(0); i < n; i++ {
			if left {
				carry = msb(v)
				v = (v * 2) & m
			} else {
				carry = uint32(v & 1)
				v = v / 2
			}
		}
		c.x = carry
	case 2: // ROXL / ROXR
		x := c.x
		for i := uint32(0); i < n; i++ {
			if left {
				carry = msb(v)
				v = (v*2 + uint64(x)) & m
			} else {
				carry = uint32(v & 1)
				v = v / 2
				if x != 0 {
					v += top
				}
			}
			x = carry
		}
		c.x = x
		carry = x
	case 3: // ROL / ROR
		for i := uint32(0); i < n; i++ {
			if left {
				carry = msb(v)
				v = (v*2 + uint64(carry)) & m
			} else {
				carry = uint32(v & 1)
				v = v / 2
				if carry != 0 {
					v += top
				}
			}
		}
	}
	r := uint32(v) & mask[sz]
	c.c = carry
	c.v = vf
	c.nz(r, sz)
	return r
}
