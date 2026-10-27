package ppc

import (
	"math"
)

// FPSCR bits
const (
	fpscrFX     = 0x80000000
	fpscrFEX    = 0x40000000
	fpscrVX     = 0x20000000
	fpscrOX     = 0x10000000
	fpscrZX     = 0x04000000
	fpscrVXSNAN = 0x01000000
)

func (c *CPU) fpRc(op uint32) {
	if op&1 != 0 {
		c.setCRField(1, c.FPSCR>>28)
	}
}

// setFPRF sets the result class and condition bits of FPSCR for r.
func (c *CPU) setFPRF(r float64) {
	var f uint32
	switch {
	case math.IsNaN(r):
		f = 0x11
	case math.IsInf(r, 1):
		f = 0x05
	case math.IsInf(r, -1):
		f = 0x09
	case r == 0:
		if math.Signbit(r) {
			f = 0x12
		} else {
			f = 0x02
		}
	default:
		ar := math.Abs(r)
		den := ar < 2.2250738585072014e-308
		switch {
		case r < 0 && den:
			f = 0x18
		case r < 0:
			f = 0x08
		case den:
			f = 0x14
		default:
			f = 0x04
		}
	}
	c.FPSCR = c.FPSCR&^(0x1F<<12) | f<<12
}

func (c *CPU) fpResult(op uint32, r float64) {
	c.FPR[rD(op)] = r
	c.setFPRF(r)
	c.fpRc(op)
}

// round converts f to an integer the way FPSCR[RN] says (or toward zero).
func (c *CPU) toInt32(f float64, truncate bool) uint32 {
	if math.IsNaN(f) {
		c.FPSCR |= fpscrVX | fpscrFX
		return 0x80000000
	}
	var r float64
	mode := c.FPSCR & 3
	if truncate {
		mode = 1
	}
	switch mode {
	case 0:
		r = math.RoundToEven(f)
	case 1:
		r = math.Trunc(f)
	case 2:
		r = math.Ceil(f)
	default:
		r = math.Floor(f)
	}
	if r > 2147483647 {
		c.FPSCR |= fpscrVX | fpscrFX
		return 0x7FFFFFFF
	}
	if r < -2147483648 {
		c.FPSCR |= fpscrVX | fpscrFX
		return 0x80000000
	}
	return uint32(int32(r))
}

func (c *CPU) fcmp(op uint32) {
	a, b := c.FPR[rA(op)], c.FPR[rB(op)]
	var v uint32
	switch {
	case math.IsNaN(a) || math.IsNaN(b):
		v = 1
	case a < b:
		v = 8
	case a > b:
		v = 4
	default:
		v = 2
	}
	c.FPSCR = c.FPSCR&^0xF000 | v<<12
	c.setCRField(op>>23&7, v)
}

func initFP() {
	// ---------------------------------------------------------- loads and stores
	lfs := func(c *CPU, a uint32) float64 { return float64(math.Float32frombits(c.read32(a))) }
	primary[48] = func(c *CPU, op uint32) { c.FPR[rD(op)] = lfs(c, c.eaD(op)) } // lfs
	primary[49] = func(c *CPU, op uint32) {                                     // lfsu
		a := c.GPR[rA(op)] + simm(op)
		c.FPR[rD(op)] = lfs(c, a)
		c.GPR[rA(op)] = a
	}
	primary[50] = func(c *CPU, op uint32) { c.FPR[rD(op)] = u2f(c.read64(c.eaD(op))) } // lfd
	primary[51] = func(c *CPU, op uint32) {                                            // lfdu
		a := c.GPR[rA(op)] + simm(op)
		c.FPR[rD(op)] = u2f(c.read64(a))
		c.GPR[rA(op)] = a
	}
	stfs := func(c *CPU, a uint32, f float64) { c.write32(a, math.Float32bits(float32(f))) }
	primary[52] = func(c *CPU, op uint32) { stfs(c, c.eaD(op), c.FPR[rD(op)]) } // stfs
	primary[53] = func(c *CPU, op uint32) {                                     // stfsu
		a := c.GPR[rA(op)] + simm(op)
		stfs(c, a, c.FPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	primary[54] = func(c *CPU, op uint32) { c.write64(c.eaD(op), f2u(c.FPR[rD(op)])) } // stfd
	primary[55] = func(c *CPU, op uint32) {                                            // stfdu
		a := c.GPR[rA(op)] + simm(op)
		c.write64(a, f2u(c.FPR[rD(op)]))
		c.GPR[rA(op)] = a
	}
	ext31[535] = func(c *CPU, op uint32) { c.FPR[rD(op)] = lfs(c, c.eaX(op)) } // lfsx
	ext31[567] = func(c *CPU, op uint32) {                                     // lfsux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.FPR[rD(op)] = lfs(c, a)
		c.GPR[rA(op)] = a
	}
	ext31[599] = func(c *CPU, op uint32) { c.FPR[rD(op)] = u2f(c.read64(c.eaX(op))) } // lfdx
	ext31[631] = func(c *CPU, op uint32) {                                            // lfdux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.FPR[rD(op)] = u2f(c.read64(a))
		c.GPR[rA(op)] = a
	}
	ext31[663] = func(c *CPU, op uint32) { stfs(c, c.eaX(op), c.FPR[rD(op)]) } // stfsx
	ext31[695] = func(c *CPU, op uint32) {                                     // stfsux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		stfs(c, a, c.FPR[rD(op)])
		c.GPR[rA(op)] = a
	}
	ext31[727] = func(c *CPU, op uint32) { c.write64(c.eaX(op), f2u(c.FPR[rD(op)])) } // stfdx
	ext31[759] = func(c *CPU, op uint32) {                                            // stfdux
		a := c.GPR[rA(op)] + c.GPR[rB(op)]
		c.write64(a, f2u(c.FPR[rD(op)]))
		c.GPR[rA(op)] = a
	}
	ext31[983] = func(c *CPU, op uint32) { c.write32(c.eaX(op), uint32(f2u(c.FPR[rD(op)]))) } // stfiwx

	// ---------------------------------------------------------- arithmetic
	type ar = func(a, b, cc float64) float64
	single := func(n uint32, f ar) {
		ext59[n] = func(c *CPU, op uint32) {
			c.fpResult(op, toSingle(f(c.FPR[rA(op)], c.FPR[rB(op)], c.FPR[rC(op)])))
		}
	}
	double := func(n uint32, f ar) {
		ext63a[n] = func(c *CPU, op uint32) {
			c.fpResult(op, f(c.FPR[rA(op)], c.FPR[rB(op)], c.FPR[rC(op)]))
		}
	}
	both := func(n uint32, f ar) { single(n, f); double(n, f) }
	both(18, func(a, b, _ float64) float64 { return a / b })
	both(20, func(a, b, _ float64) float64 { return a - b })
	both(21, func(a, b, _ float64) float64 { return a + b })
	both(22, func(_, b, _ float64) float64 { return math.Sqrt(b) })
	both(25, func(a, _, cc float64) float64 { return a * cc })
	both(28, func(a, b, cc float64) float64 { return math.FMA(a, cc, -b) })  // fmsub
	both(29, func(a, b, cc float64) float64 { return math.FMA(a, cc, b) })   // fmadd
	both(30, func(a, b, cc float64) float64 { return -math.FMA(a, cc, -b) }) // fnmsub
	both(31, func(a, b, cc float64) float64 { return -math.FMA(a, cc, b) })  // fnmadd
	single(24, func(_, b, _ float64) float64 { return 1 / b })               // fres
	double(26, func(_, b, _ float64) float64 { return 1 / math.Sqrt(b) })    // frsqrte
	ext63a[23] = func(c *CPU, op uint32) {                                   // fsel
		a := c.FPR[rA(op)]
		if a >= 0 {
			c.FPR[rD(op)] = c.FPR[rC(op)]
		} else {
			c.FPR[rD(op)] = c.FPR[rB(op)]
		}
		c.fpRc(op)
	}

	// ---------------------------------------------------------- moves, conversions, compares
	ext63x[72] = func(c *CPU, op uint32) { c.FPR[rD(op)] = c.FPR[rB(op)]; c.fpRc(op) }                         // fmr
	ext63x[40] = func(c *CPU, op uint32) { c.FPR[rD(op)] = u2f(f2u(c.FPR[rB(op)]) ^ 1<<63); c.fpRc(op) }       // fneg
	ext63x[264] = func(c *CPU, op uint32) { c.FPR[rD(op)] = u2f(f2u(c.FPR[rB(op)]) &^ (1 << 63)); c.fpRc(op) } // fabs
	ext63x[136] = func(c *CPU, op uint32) { c.FPR[rD(op)] = u2f(f2u(c.FPR[rB(op)]) | 1<<63); c.fpRc(op) }      // fnabs
	ext63x[12] = func(c *CPU, op uint32) { c.fpResult(op, toSingle(c.FPR[rB(op)])) }                           // frsp
	ext63x[14] = func(c *CPU, op uint32) {                                                                     // fctiw
		c.FPR[rD(op)] = u2f(0xFFF8000000000000 | uint64(c.toInt32(c.FPR[rB(op)], false)))
		c.fpRc(op)
	}
	ext63x[15] = func(c *CPU, op uint32) { // fctiwz
		c.FPR[rD(op)] = u2f(0xFFF8000000000000 | uint64(c.toInt32(c.FPR[rB(op)], true)))
		c.fpRc(op)
	}
	ext63x[0] = func(c *CPU, op uint32) { c.fcmp(op) }  // fcmpu
	ext63x[32] = func(c *CPU, op uint32) { c.fcmp(op) } // fcmpo

	// ---------------------------------------------------------- FPSCR
	ext63x[583] = func(c *CPU, op uint32) { c.FPR[rD(op)] = u2f(uint64(c.FPSCR)); c.fpRc(op) } // mffs
	ext63x[711] = func(c *CPU, op uint32) {                                                    // mtfsf
		fm := op >> 17 & 0xFF
		var m uint32
		for i := uint32(0); i < 8; i++ {
			if fm&(0x80>>i) != 0 {
				m |= 0xF0000000 >> (4 * i)
			}
		}
		c.FPSCR = c.FPSCR&^m | uint32(f2u(c.FPR[rB(op)]))&m
		c.fpRc(op)
	}
	ext63x[134] = func(c *CPU, op uint32) { // mtfsfi
		f := op >> 23 & 7
		sh := 28 - 4*f
		c.FPSCR = c.FPSCR&^(0xF<<sh) | (op>>12&0xF)<<sh
		c.fpRc(op)
	}
	ext63x[38] = func(c *CPU, op uint32) { c.FPSCR |= 0x80000000 >> rD(op); c.fpRc(op) }  // mtfsb1
	ext63x[70] = func(c *CPU, op uint32) { c.FPSCR &^= 0x80000000 >> rD(op); c.fpRc(op) } // mtfsb0
	ext63x[64] = func(c *CPU, op uint32) {                                                // mcrfs
		s := op >> 18 & 7
		v := c.FPSCR >> (28 - 4*s) & 0xF
		c.setCRField(op>>23&7, v)
		// the exception bits copied are cleared
		c.FPSCR &^= (0xF << (28 - 4*s)) & 0x9FF80700
	}
}
