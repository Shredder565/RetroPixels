package ppc

import (
	"math/bits"
)

// Instruction fields
func rD(op uint32) uint32   { return op >> 21 & 31 }
func rA(op uint32) uint32   { return op >> 16 & 31 }
func rB(op uint32) uint32   { return op >> 11 & 31 }
func rC(op uint32) uint32   { return op >> 6 & 31 }
func simm(op uint32) uint32 { return uint32(int32(int16(op))) }
func uimm(op uint32) uint32 { return op & 0xFFFF }

type opfn = func(c *CPU, op uint32)

var (
	primary [64]opfn
	ext19   [1024]opfn
	ext31   [1024]opfn
	ext59   [32]opfn
	ext63a  [32]opfn
	ext63x  [1024]opfn
)

func decode(op uint32) opfn {
	switch op >> 26 {
	case 19:
		if f := ext19[op>>1&0x3FF]; f != nil {
			return f
		}
	case 31:
		if f := ext31[op>>1&0x3FF]; f != nil {
			return f
		}
	case 59:
		if f := ext59[op>>1&31]; f != nil {
			return f
		}
	case 63:
		if f := ext63x[op>>1&0x3FF]; f != nil {
			return f
		}
		if f := ext63a[op>>1&31]; f != nil {
			return f
		}
	default:
		if f := primary[op>>26]; f != nil {
			return f
		}
	}
	return opIllegal
}

func opIllegal(c *CPU, op uint32) {
	c.halt(c.PC-4, op, "unknown instruction")
}

// ea: rA|0 + d
func (c *CPU) eaD(op uint32) uint32 {
	if a := rA(op); a != 0 {
		return c.GPR[a] + simm(op)
	}
	return simm(op)
}

// ea: rA|0 + rB
func (c *CPU) eaX(op uint32) uint32 {
	if a := rA(op); a != 0 {
		return c.GPR[a] + c.GPR[rB(op)]
	}
	return c.GPR[rB(op)]
}

// rotate mask from bit mb to bit me (bit 0 is the most significant)
func mask(mb, me uint32) uint32 {
	m1 := uint32(0xFFFFFFFF) >> mb
	m2 := uint32(0xFFFFFFFF) << (31 - me)
	if mb <= me {
		return m1 & m2
	}
	return m1 | m2
}

func init() {
	// ---------------------------------------------------------- integer immediates
	primary[3] = func(c *CPU, op uint32) { // twi
		if c.trapCond(rD(op), c.GPR[rA(op)], simm(op)) {
			c.exception(excProgram, c.PC-4, 0x20000)
		}
	}
	primary[7] = func(c *CPU, op uint32) { // mulli
		c.GPR[rD(op)] = uint32(int32(c.GPR[rA(op)]) * int32(simm(op)))
	}
	primary[8] = func(c *CPU, op uint32) { // subfic
		a := c.GPR[rA(op)]
		i := simm(op)
		c.GPR[rD(op)] = i - a
		c.setCA(i >= a)
	}
	primary[10] = func(c *CPU, op uint32) { // cmpli
		c.cmpU(op>>23&7, c.GPR[rA(op)], uimm(op))
	}
	primary[11] = func(c *CPU, op uint32) { // cmpi
		c.cmpS(op>>23&7, c.GPR[rA(op)], simm(op))
	}
	primary[12] = func(c *CPU, op uint32) { // addic
		a := c.GPR[rA(op)]
		r := a + simm(op)
		c.GPR[rD(op)] = r
		c.setCA(r < a)
	}
	primary[13] = func(c *CPU, op uint32) { // addic.
		a := c.GPR[rA(op)]
		r := a + simm(op)
		c.GPR[rD(op)] = r
		c.setCA(r < a)
		c.setCR0(r)
	}
	primary[14] = func(c *CPU, op uint32) { // addi
		if a := rA(op); a != 0 {
			c.GPR[rD(op)] = c.GPR[a] + simm(op)
		} else {
			c.GPR[rD(op)] = simm(op)
		}
	}
	primary[15] = func(c *CPU, op uint32) { // addis
		if a := rA(op); a != 0 {
			c.GPR[rD(op)] = c.GPR[a] + op<<16
		} else {
			c.GPR[rD(op)] = op << 16
		}
	}
	primary[24] = func(c *CPU, op uint32) { c.GPR[rA(op)] = c.GPR[rD(op)] | uimm(op) }                        // ori
	primary[25] = func(c *CPU, op uint32) { c.GPR[rA(op)] = c.GPR[rD(op)] | op<<16 }                          // oris
	primary[26] = func(c *CPU, op uint32) { c.GPR[rA(op)] = c.GPR[rD(op)] ^ uimm(op) }                        // xori
	primary[27] = func(c *CPU, op uint32) { c.GPR[rA(op)] = c.GPR[rD(op)] ^ op<<16 }                          // xoris
	primary[28] = func(c *CPU, op uint32) { r := c.GPR[rD(op)] & uimm(op); c.GPR[rA(op)] = r; c.setCR0(r) }   // andi.
	primary[29] = func(c *CPU, op uint32) { r := c.GPR[rD(op)] & (op << 16); c.GPR[rA(op)] = r; c.setCR0(r) } // andis.

	// ---------------------------------------------------------- rotates
	primary[20] = func(c *CPU, op uint32) { // rlwimi
		m := mask(op>>6&31, op>>1&31)
		r := bits.RotateLeft32(c.GPR[rD(op)], int(op>>11&31))
		v := r&m | c.GPR[rA(op)]&^m
		c.GPR[rA(op)] = v
		if op&1 != 0 {
			c.setCR0(v)
		}
	}
	primary[21] = func(c *CPU, op uint32) { // rlwinm
		v := bits.RotateLeft32(c.GPR[rD(op)], int(op>>11&31)) & mask(op>>6&31, op>>1&31)
		c.GPR[rA(op)] = v
		if op&1 != 0 {
			c.setCR0(v)
		}
	}
	primary[23] = func(c *CPU, op uint32) { // rlwnm
		v := bits.RotateLeft32(c.GPR[rD(op)], int(c.GPR[rB(op)]&31)) & mask(op>>6&31, op>>1&31)
		c.GPR[rA(op)] = v
		if op&1 != 0 {
			c.setCR0(v)
		}
	}

	// ---------------------------------------------------------- branches
	primary[16] = func(c *CPU, op uint32) { // bc
		if c.bcCond(op) {
			t := uint32(int32(int16(op & 0xFFFC)))
			if op&2 == 0 {
				t += c.PC - 4
			}
			if op&1 != 0 {
				c.LR = c.PC
			}
			c.PC = t
		} else if op&1 != 0 {
			c.LR = c.PC
		}
	}
	primary[17] = func(c *CPU, op uint32) { // sc
		c.exception(excSC, c.PC, 0)
	}
	primary[18] = func(c *CPU, op uint32) { // b
		t := uint32(int32(op<<6)>>6) &^ 3
		if op&2 == 0 {
			t += c.PC - 4
		}
		if op&1 != 0 {
			c.LR = c.PC
		}
		c.PC = t
	}
	ext19[16] = func(c *CPU, op uint32) { // bclr
		t := c.LR &^ 3
		ok := c.bcCond(op)
		if op&1 != 0 {
			c.LR = c.PC
		}
		if ok {
			c.PC = t
		}
	}
	ext19[528] = func(c *CPU, op uint32) { // bcctr
		t := c.CTR &^ 3
		bo := rD(op)
		ok := bo&16 != 0 || c.crBit(rA(op)) == bo>>3&1
		if op&1 != 0 {
			c.LR = c.PC
		}
		if ok {
			c.PC = t
		}
	}
	ext19[0] = func(c *CPU, op uint32) { c.setCRField(op>>23&7, c.crField(op>>18&7)) } // mcrf
	crop := func(f func(a, b uint32) uint32) opfn {
		return func(c *CPU, op uint32) {
			c.setCRBit(rD(op), f(c.crBit(rA(op)), c.crBit(rB(op)))&1)
		}
	}
	ext19[33] = crop(func(a, b uint32) uint32 { return ^(a | b) })  // crnor
	ext19[129] = crop(func(a, b uint32) uint32 { return a &^ b })   // crandc
	ext19[193] = crop(func(a, b uint32) uint32 { return a ^ b })    // crxor
	ext19[225] = crop(func(a, b uint32) uint32 { return ^(a & b) }) // crnand
	ext19[257] = crop(func(a, b uint32) uint32 { return a & b })    // crand
	ext19[289] = crop(func(a, b uint32) uint32 { return ^(a ^ b) }) // creqv
	ext19[417] = crop(func(a, b uint32) uint32 { return a | ^b })   // crorc
	ext19[449] = crop(func(a, b uint32) uint32 { return a | b })    // cror
	ext19[150] = func(c *CPU, op uint32) {}                         // isync
	ext19[50] = func(c *CPU, op uint32) {                           // rfi
		const m = 0x87C0FF73
		c.MSR = c.MSR&^m | c.SRR1&m
		c.PC = c.SRR0 &^ 3
		c.checkInterrupts()
	}

	// ---------------------------------------------------------- loads and stores
	primary[32] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read32(c.eaD(op)) } // lwz
	primary[33] = func(c *CPU, op uint32) {                                       // lwzu
		a := c.GPR[rA(op)] + simm(op)
		c.GPR[rD(op)] = c.read32(a)
		c.GPR[rA(op)] = a
	}
	primary[34] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read8(c.eaD(op)) } // lbz
	primary[35] = func(c *CPU, op uint32) {                                      // lbzu
		a := c.GPR[rA(op)] + simm(op)
		c.GPR[rD(op)] = c.read8(a)
		c.GPR[rA(op)] = a
	}
	primary[36] = func(c *CPU, op uint32) { c.write32(c.eaD(op), c.GPR[rD(op)]) } // stw
	primary[37] = func(c *CPU, op uint32) {                                       // stwu
		a := c.GPR[rA(op)] + simm(op)
		c.write32(a, c.GPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	primary[38] = func(c *CPU, op uint32) { c.write8(c.eaD(op), c.GPR[rD(op)]) } // stb
	primary[39] = func(c *CPU, op uint32) {                                      // stbu
		a := c.GPR[rA(op)] + simm(op)
		c.write8(a, c.GPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	primary[40] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read16(c.eaD(op)) } // lhz
	primary[41] = func(c *CPU, op uint32) {                                       // lhzu
		a := c.GPR[rA(op)] + simm(op)
		c.GPR[rD(op)] = c.read16(a)
		c.GPR[rA(op)] = a
	}
	primary[42] = func(c *CPU, op uint32) { c.GPR[rD(op)] = uint32(int32(int16(c.read16(c.eaD(op))))) } // lha
	primary[43] = func(c *CPU, op uint32) {                                                             // lhau
		a := c.GPR[rA(op)] + simm(op)
		c.GPR[rD(op)] = uint32(int32(int16(c.read16(a))))
		c.GPR[rA(op)] = a
	}
	primary[44] = func(c *CPU, op uint32) { c.write16(c.eaD(op), c.GPR[rD(op)]) } // sth
	primary[45] = func(c *CPU, op uint32) {                                       // sthu
		a := c.GPR[rA(op)] + simm(op)
		c.write16(a, c.GPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	primary[46] = func(c *CPU, op uint32) { // lmw
		a := c.eaD(op)
		for r := rD(op); r < 32; r++ {
			c.GPR[r] = c.read32(a)
			a += 4
		}
	}
	primary[47] = func(c *CPU, op uint32) { // stmw
		a := c.eaD(op)
		for r := rD(op); r < 32; r++ {
			c.write32(a, c.GPR[r])
			a += 4
		}
	}

	// indexed
	ext31[23] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read32(c.eaX(op)) } // lwzx
	ext31[55] = func(c *CPU, op uint32) {                                       // lwzux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.GPR[rD(op)] = c.read32(a)
		c.GPR[rA(op)] = a
	}
	ext31[87] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read8(c.eaX(op)) } // lbzx
	ext31[119] = func(c *CPU, op uint32) {                                     // lbzux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.GPR[rD(op)] = c.read8(a)
		c.GPR[rA(op)] = a
	}
	ext31[279] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read16(c.eaX(op)) } // lhzx
	ext31[311] = func(c *CPU, op uint32) {                                       // lhzux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.GPR[rD(op)] = c.read16(a)
		c.GPR[rA(op)] = a
	}
	ext31[343] = func(c *CPU, op uint32) { c.GPR[rD(op)] = uint32(int32(int16(c.read16(c.eaX(op))))) } // lhax
	ext31[375] = func(c *CPU, op uint32) {                                                             // lhaux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.GPR[rD(op)] = uint32(int32(int16(c.read16(a))))
		c.GPR[rA(op)] = a
	}
	ext31[151] = func(c *CPU, op uint32) { c.write32(c.eaX(op), c.GPR[rD(op)]) } // stwx
	ext31[183] = func(c *CPU, op uint32) {                                       // stwux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.write32(a, c.GPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	ext31[215] = func(c *CPU, op uint32) { c.write8(c.eaX(op), c.GPR[rD(op)]) } // stbx
	ext31[247] = func(c *CPU, op uint32) {                                      // stbux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.write8(a, c.GPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	ext31[407] = func(c *CPU, op uint32) { c.write16(c.eaX(op), c.GPR[rD(op)]) } // sthx
	ext31[439] = func(c *CPU, op uint32) {                                       // sthux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.write16(a, c.GPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	ext31[534] = func(c *CPU, op uint32) { c.GPR[rD(op)] = bits.ReverseBytes32(c.read32(c.eaX(op))) }                 // lwbrx
	ext31[790] = func(c *CPU, op uint32) { c.GPR[rD(op)] = uint32(bits.ReverseBytes16(uint16(c.read16(c.eaX(op))))) } // lhbrx
	ext31[662] = func(c *CPU, op uint32) { c.write32(c.eaX(op), bits.ReverseBytes32(c.GPR[rD(op)])) }                 // stwbrx
	ext31[918] = func(c *CPU, op uint32) { c.write16(c.eaX(op), uint32(bits.ReverseBytes16(uint16(c.GPR[rD(op)])))) } // sthbrx
	ext31[20] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read32(c.eaX(op)) }                                       // lwarx
	ext31[150] = func(c *CPU, op uint32) {                                                                            // stwcx.
		c.write32(c.eaX(op), c.GPR[rD(op)])
		c.setCRField(0, 2|c.so())
	}
	ext31[597] = func(c *CPU, op uint32) { // lswi
		n := rB(op)
		if n == 0 {
			n = 32
		}
		var a uint32
		if rA(op) != 0 {
			a = c.GPR[rA(op)]
		}
		c.loadString(rD(op), a, n)
	}
	ext31[533] = func(c *CPU, op uint32) { c.loadString(rD(op), c.eaX(op), c.XER&0x7F) } // lswx
	ext31[725] = func(c *CPU, op uint32) {                                               // stswi
		n := rB(op)
		if n == 0 {
			n = 32
		}
		var a uint32
		if rA(op) != 0 {
			a = c.GPR[rA(op)]
		}
		c.storeString(rD(op), a, n)
	}
	ext31[661] = func(c *CPU, op uint32) { c.storeString(rD(op), c.eaX(op), c.XER&0x7F) } // stswx

	// caches, synchronisation, MMU: nothing to do without caches or an MMU
	nop := func(c *CPU, op uint32) {}
	for _, x := range []uint32{54, 86, 246, 278, 470, 982, 598, 854, 306, 370, 566, 978, 1010} {
		ext31[x] = nop
	}
	ext31[1014] = func(c *CPU, op uint32) { // dcbz
		a := c.eaX(op) &^ 31
		for i := uint32(0); i < 32; i += 4 {
			c.write32(a+i, 0)
		}
	}
	ext31[310] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.read32(c.eaX(op)) } // eciwx
	ext31[438] = func(c *CPU, op uint32) { c.write32(c.eaX(op), c.GPR[rD(op)]) } // ecowx

	// ---------------------------------------------------------- integer arithmetic (XO form, OE in bit 10)
	xo := func(n uint32, f opfn) { ext31[n] = f; ext31[n|0x200] = f }
	xo(266, func(c *CPU, op uint32) { // add
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		r := a + b
		c.GPR[rD(op)] = r
		c.arithDone(op, r, addOV(r, a, b))
	})
	xo(10, func(c *CPU, op uint32) { // addc
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		r := a + b
		c.GPR[rD(op)] = r
		c.setCA(r < a)
		c.arithDone(op, r, addOV(r, a, b))
	})
	xo(138, func(c *CPU, op uint32) { // adde
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		r, ca := bits.Add32(a, b, c.ca())
		c.GPR[rD(op)] = r
		c.setCA(ca != 0)
		c.arithDone(op, r, addOV(r, a, b))
	})
	xo(234, func(c *CPU, op uint32) { // addme
		a := c.GPR[rA(op)]
		r, ca := bits.Add32(a, 0xFFFFFFFF, c.ca())
		c.GPR[rD(op)] = r
		c.setCA(ca != 0)
		c.arithDone(op, r, addOV(r, a, 0xFFFFFFFF))
	})
	xo(202, func(c *CPU, op uint32) { // addze
		a := c.GPR[rA(op)]
		r, ca := bits.Add32(a, 0, c.ca())
		c.GPR[rD(op)] = r
		c.setCA(ca != 0)
		c.arithDone(op, r, addOV(r, a, 0))
	})
	xo(40, func(c *CPU, op uint32) { // subf
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		r := b - a
		c.GPR[rD(op)] = r
		c.arithDone(op, r, subOV(r, b, a))
	})
	xo(8, func(c *CPU, op uint32) { // subfc
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		r := b - a
		c.GPR[rD(op)] = r
		c.setCA(b >= a)
		c.arithDone(op, r, subOV(r, b, a))
	})
	xo(136, func(c *CPU, op uint32) { // subfe
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		r, ca := bits.Add32(^a, b, c.ca())
		c.GPR[rD(op)] = r
		c.setCA(ca != 0)
		c.arithDone(op, r, addOV(r, ^a, b))
	})
	xo(232, func(c *CPU, op uint32) { // subfme
		a := c.GPR[rA(op)]
		r, ca := bits.Add32(^a, 0xFFFFFFFF, c.ca())
		c.GPR[rD(op)] = r
		c.setCA(ca != 0)
		c.arithDone(op, r, addOV(r, ^a, 0xFFFFFFFF))
	})
	xo(200, func(c *CPU, op uint32) { // subfze
		a := c.GPR[rA(op)]
		r, ca := bits.Add32(^a, 0, c.ca())
		c.GPR[rD(op)] = r
		c.setCA(ca != 0)
		c.arithDone(op, r, addOV(r, ^a, 0))
	})
	xo(104, func(c *CPU, op uint32) { // neg
		a := c.GPR[rA(op)]
		r := -a
		c.GPR[rD(op)] = r
		c.arithDone(op, r, a == 0x80000000)
	})
	xo(235, func(c *CPU, op uint32) { // mullw
		p := int64(int32(c.GPR[rA(op)])) * int64(int32(c.GPR[rB(op)]))
		r := uint32(p)
		c.GPR[rD(op)] = r
		c.arithDone(op, r, p != int64(int32(r)))
	})
	xo(75, func(c *CPU, op uint32) { // mulhw
		p := int64(int32(c.GPR[rA(op)])) * int64(int32(c.GPR[rB(op)]))
		r := uint32(p >> 32)
		c.GPR[rD(op)] = r
		if op&1 != 0 {
			c.setCR0(r)
		}
	})
	xo(11, func(c *CPU, op uint32) { // mulhwu
		hi, _ := bits.Mul32(c.GPR[rA(op)], c.GPR[rB(op)])
		c.GPR[rD(op)] = hi
		if op&1 != 0 {
			c.setCR0(hi)
		}
	})
	xo(491, func(c *CPU, op uint32) { // divw
		a, b := int32(c.GPR[rA(op)]), int32(c.GPR[rB(op)])
		var r uint32
		ov := false
		if b == 0 || a == -0x80000000 && b == -1 {
			ov = true
			if a < 0 {
				r = 0xFFFFFFFF
			}
		} else {
			r = uint32(a / b)
		}
		c.GPR[rD(op)] = r
		c.arithDone(op, r, ov)
	})
	xo(459, func(c *CPU, op uint32) { // divwu
		a, b := c.GPR[rA(op)], c.GPR[rB(op)]
		var r uint32
		ov := b == 0
		if !ov {
			r = a / b
		}
		c.GPR[rD(op)] = r
		c.arithDone(op, r, ov)
	})

	// ---------------------------------------------------------- compares, logic, shifts
	ext31[0] = func(c *CPU, op uint32) { c.cmpS(op>>23&7, c.GPR[rA(op)], c.GPR[rB(op)]) }  // cmp
	ext31[32] = func(c *CPU, op uint32) { c.cmpU(op>>23&7, c.GPR[rA(op)], c.GPR[rB(op)]) } // cmpl
	logic := func(n uint32, f func(s, b uint32) uint32) {
		ext31[n] = func(c *CPU, op uint32) {
			r := f(c.GPR[rD(op)], c.GPR[rB(op)])
			c.GPR[rA(op)] = r
			if op&1 != 0 {
				c.setCR0(r)
			}
		}
	}
	logic(28, func(s, b uint32) uint32 { return s & b })     // and
	logic(60, func(s, b uint32) uint32 { return s &^ b })    // andc
	logic(124, func(s, b uint32) uint32 { return ^(s | b) }) // nor
	logic(284, func(s, b uint32) uint32 { return ^(s ^ b) }) // eqv
	logic(316, func(s, b uint32) uint32 { return s ^ b })    // xor
	logic(412, func(s, b uint32) uint32 { return s | ^b })   // orc
	logic(444, func(s, b uint32) uint32 { return s | b })    // or
	logic(476, func(s, b uint32) uint32 { return ^(s & b) }) // nand
	logic(24, func(s, b uint32) uint32 {                     // slw
		if b&0x20 != 0 {
			return 0
		}
		return s << (b & 31)
	})
	logic(536, func(s, b uint32) uint32 { // srw
		if b&0x20 != 0 {
			return 0
		}
		return s >> (b & 31)
	})
	unary := func(n uint32, f func(s uint32) uint32) {
		ext31[n] = func(c *CPU, op uint32) {
			r := f(c.GPR[rD(op)])
			c.GPR[rA(op)] = r
			if op&1 != 0 {
				c.setCR0(r)
			}
		}
	}
	unary(26, func(s uint32) uint32 { return uint32(bits.LeadingZeros32(s)) }) // cntlzw
	unary(922, func(s uint32) uint32 { return uint32(int32(int16(s))) })       // extsh
	unary(954, func(s uint32) uint32 { return uint32(int32(int8(s))) })        // extsb
	ext31[792] = func(c *CPU, op uint32) {                                     // sraw
		s := int32(c.GPR[rD(op)])
		n := c.GPR[rB(op)] & 0x3F
		var r int32
		ca := false
		if n >= 32 {
			if s < 0 {
				r, ca = -1, true
			}
		} else {
			r = s >> n
			ca = s < 0 && uint32(s)<<(32-n) != 0 && n != 0
		}
		c.GPR[rA(op)] = uint32(r)
		c.setCA(ca)
		if op&1 != 0 {
			c.setCR0(uint32(r))
		}
	}
	ext31[824] = func(c *CPU, op uint32) { // srawi
		s := int32(c.GPR[rD(op)])
		n := rB(op)
		r := s >> n
		c.GPR[rA(op)] = uint32(r)
		c.setCA(n != 0 && s < 0 && uint32(s)<<(32-n) != 0)
		if op&1 != 0 {
			c.setCR0(uint32(r))
		}
	}
	ext31[4] = func(c *CPU, op uint32) { // tw
		if c.trapCond(rD(op), c.GPR[rA(op)], c.GPR[rB(op)]) {
			c.exception(excProgram, c.PC-4, 0x20000)
		}
	}

	// ---------------------------------------------------------- special registers
	ext31[19] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.CR } // mfcr
	ext31[144] = func(c *CPU, op uint32) {                       // mtcrf
		fxm := op >> 12 & 0xFF
		var m uint32
		for i := uint32(0); i < 8; i++ {
			if fxm&(0x80>>i) != 0 {
				m |= 0xF0000000 >> (4 * i)
			}
		}
		c.CR = c.CR&^m | c.GPR[rD(op)]&m
	}
	ext31[512] = func(c *CPU, op uint32) { // mcrxr
		c.setCRField(op>>23&7, c.XER>>28)
		c.XER &^= 0xF0000000
	}
	ext31[83] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.MSR }    // mfmsr
	ext31[146] = func(c *CPU, op uint32) { c.setMSR(c.GPR[rD(op)]) } // mtmsr
	ext31[339] = func(c *CPU, op uint32) {                           // mfspr
		n := op>>16&31 | op>>6&0x3E0
		c.GPR[rD(op)] = c.mfspr(n, c.PC-4, op)
	}
	ext31[371] = func(c *CPU, op uint32) { // mftb
		n := op>>16&31 | op>>6&0x3E0
		if n == sprTBUr {
			c.GPR[rD(op)] = uint32(c.TB() >> 32)
		} else {
			c.GPR[rD(op)] = uint32(c.TB())
		}
	}
	ext31[467] = func(c *CPU, op uint32) { // mtspr
		n := op>>16&31 | op>>6&0x3E0
		c.mtspr(n, c.GPR[rD(op)])
	}
	ext31[595] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.SR[op>>16&15] }         // mfsr
	ext31[659] = func(c *CPU, op uint32) { c.GPR[rD(op)] = c.SR[c.GPR[rB(op)]>>28] } // mfsrin
	ext31[210] = func(c *CPU, op uint32) { c.SR[op>>16&15] = c.GPR[rD(op)] }         // mtsr
	ext31[242] = func(c *CPU, op uint32) { c.SR[c.GPR[rB(op)]>>28] = c.GPR[rD(op)] } // mtsrin

	initFP()
}

func (c *CPU) loadString(r, a, n uint32) {
	for n > 0 {
		var v uint32
		for i := uint32(0); i < 4; i++ {
			v <<= 8
			if n > 0 {
				v |= c.read8(a)
				a++
				n--
			}
		}
		c.GPR[r] = v
		r = (r + 1) & 31
	}
}

func (c *CPU) storeString(r, a, n uint32) {
	for n > 0 {
		v := c.GPR[r]
		for i := uint32(0); i < 4 && n > 0; i++ {
			c.write8(a, v>>24)
			v <<= 8
			a++
			n--
		}
		r = (r + 1) & 31
	}
}

func (c *CPU) ca() uint32 { return c.XER >> 29 & 1 }
func (c *CPU) setCA(on bool) {
	if on {
		c.XER |= xerCA
	} else {
		c.XER &^= xerCA
	}
}

func addOV(r, a, b uint32) bool { return (^(a^b)&(a^r))&0x80000000 != 0 }
func subOV(r, a, b uint32) bool { return ((a^b)&(a^r))&0x80000000 != 0 } // r = a - b

// arithDone sets XER[OV,SO] (when OE) and CR0 (when Rc).
func (c *CPU) arithDone(op, r uint32, ov bool) {
	if op&0x400 != 0 {
		if ov {
			c.XER |= xerOV | xerSO
		} else {
			c.XER &^= xerOV
		}
	}
	if op&1 != 0 {
		c.setCR0(r)
	}
}

func (c *CPU) cmpS(f, a, b uint32) {
	var v uint32
	switch {
	case int32(a) < int32(b):
		v = 8
	case int32(a) > int32(b):
		v = 4
	default:
		v = 2
	}
	c.setCRField(f, v|c.so())
}

func (c *CPU) cmpU(f, a, b uint32) {
	var v uint32
	switch {
	case a < b:
		v = 8
	case a > b:
		v = 4
	default:
		v = 2
	}
	c.setCRField(f, v|c.so())
}

func (c *CPU) trapCond(to, a, b uint32) bool {
	return to&16 != 0 && int32(a) < int32(b) || to&8 != 0 && int32(a) > int32(b) || to&4 != 0 && a == b ||
		to&2 != 0 && a < b || to&1 != 0 && a > b
}

// bcCond decrements CTR if BO says so and says whether the branch is taken.
func (c *CPU) bcCond(op uint32) bool {
	bo := rD(op)
	ctrOK := true
	if bo&4 == 0 {
		c.CTR--
		ctrOK = (c.CTR != 0) != (bo&2 != 0)
	}
	condOK := bo&16 != 0 || c.crBit(rA(op)) == bo>>3&1
	return ctrOK && condOK
}
