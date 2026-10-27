package ps3

// The PPU's floating point unit. Registers hold doubles (as bits); single
// precision instructions round their result to a float.

import (
	"math"
)

func (c *PPU) f(r uint32) float64       { return math.Float64frombits(c.FPR[r]) }
func (c *PPU) setF(r uint32, v float64) { c.FPR[r] = math.Float64bits(v) }

// f32to64 widens a float's bits exactly (NaNs keep their payload).
func f32to64(b uint32) uint64 {
	if b&0x7f800000 == 0x7f800000 && b&0x7fffff != 0 {
		return uint64(b&0x80000000)<<32 | 0x7ff0000000000000 | uint64(b&0x7fffff)<<29
	}
	return math.Float64bits(float64(math.Float32frombits(b)))
}

// f64to32 narrows a double's bits (NaNs keep the top of their payload).
func f64to32(b uint64) uint32 {
	if b&0x7ff0000000000000 == 0x7ff0000000000000 && b&0xfffffffffffff != 0 {
		return uint32(b>>32)&0x80000000 | 0x7f800000 | uint32(b>>29)&0x7fffff | 0x400000&uint32(b>>29)
	}
	return math.Float32bits(float32(math.Float64frombits(b)))
}

func round32(v float64) float64 { return float64(float32(v)) }

func (c *PPU) execFPMem(i uint32) {
	d := rD(i)
	op := i >> 26
	var ea uint32
	if op&1 != 0 { // update forms
		ea = uint32(c.GPR[rA(i)] + simm(i))
	} else {
		ea = c.eaD(i)
	}
	switch op {
	case 48, 49: // lfs(u)
		c.FPR[d] = f32to64(c.Mem.Read32(ea))
	case 50, 51: // lfd(u)
		c.FPR[d] = c.Mem.Read64(ea)
	case 52, 53: // stfs(u)
		c.Mem.Write32(ea, f64to32(c.FPR[d]))
	case 54, 55: // stfd(u)
		c.Mem.Write64(ea, c.FPR[d])
	}
	if op&1 != 0 {
		c.GPR[rA(i)] = uint64(ea)
	}
}

func (c *PPU) execFPMemX(i uint32, xo uint32) {
	d := rD(i)
	update := xo == 567 || xo == 631 || xo == 695 || xo == 759
	var ea uint32
	if update {
		ea = uint32(c.GPR[rA(i)] + c.GPR[rB(i)])
	} else {
		ea = c.eaX(i)
	}
	switch xo {
	case 535, 567: // lfsx lfsux
		c.FPR[d] = f32to64(c.Mem.Read32(ea))
	case 599, 631: // lfdx lfdux
		c.FPR[d] = c.Mem.Read64(ea)
	case 663, 695: // stfsx stfsux
		c.Mem.Write32(ea, f64to32(c.FPR[d]))
	case 727, 759: // stfdx stfdux
		c.Mem.Write64(ea, c.FPR[d])
	case 983: // stfiwx
		c.Mem.Write32(ea, uint32(c.FPR[d]))
	}
	if update {
		c.GPR[rA(i)] = uint64(ea)
	}
}

func (c *PPU) fpCR1(i uint32) {
	if i&1 != 0 {
		c.setCRField(1, c.FPSCR>>28)
	}
}

// setFPRF sets the FPSCR result flags for v.
func (c *PPU) setFPRF(v float64) {
	var f uint32
	switch {
	case math.IsNaN(v):
		f = 0x11
	case math.IsInf(v, 1):
		f = 0x05
	case math.IsInf(v, -1):
		f = 0x09
	case v == 0:
		if math.Signbit(v) {
			f = 0x12
		} else {
			f = 0x02
		}
	case v > 0:
		if math.Abs(v) < 2.2250738585072014e-308 {
			f = 0x14
		} else {
			f = 0x04
		}
	default:
		if math.Abs(v) < 2.2250738585072014e-308 {
			f = 0x18
		} else {
			f = 0x08
		}
	}
	c.FPSCR = c.FPSCR&^(0x1f<<12) | f<<12
}

func (c *PPU) exec59(i uint32) {
	d, a, b, cc := rD(i), rA(i), rB(i), rC(i)
	var v float64
	switch i >> 1 & 31 {
	case 18:
		v = c.f(a) / c.f(b)
	case 20:
		v = c.f(a) - c.f(b)
	case 21:
		v = c.f(a) + c.f(b)
	case 22:
		v = math.Sqrt(c.f(b))
	case 24: // fres
		v = 1 / c.f(b)
	case 25:
		v = c.f(a) * c.f(cc)
	case 28:
		v = math.FMA(c.f(a), c.f(cc), -c.f(b))
	case 29:
		v = math.FMA(c.f(a), c.f(cc), c.f(b))
	case 30:
		v = -math.FMA(c.f(a), c.f(cc), -c.f(b))
	case 31:
		v = -math.FMA(c.f(a), c.f(cc), c.f(b))
	default:
		c.fail(i, "unknown instruction")
		return
	}
	v = round32(v)
	c.setF(d, v)
	c.setFPRF(v)
	c.fpCR1(i)
}

func (c *PPU) exec63(i uint32) {
	d, a, b, cc := rD(i), rA(i), rB(i), rC(i)
	if i&0x20 != 0 { // A-form
		var v float64
		switch i >> 1 & 31 {
		case 18:
			v = c.f(a) / c.f(b)
		case 20:
			v = c.f(a) - c.f(b)
		case 21:
			v = c.f(a) + c.f(b)
		case 22:
			v = math.Sqrt(c.f(b))
		case 23: // fsel
			if c.f(a) >= 0 {
				c.FPR[d] = c.FPR[cc]
			} else {
				c.FPR[d] = c.FPR[b]
			}
			c.fpCR1(i)
			return
		case 25:
			v = c.f(a) * c.f(cc)
		case 26: // frsqrte
			v = 1 / math.Sqrt(c.f(b))
		case 28:
			v = math.FMA(c.f(a), c.f(cc), -c.f(b))
		case 29:
			v = math.FMA(c.f(a), c.f(cc), c.f(b))
		case 30:
			v = -math.FMA(c.f(a), c.f(cc), -c.f(b))
		case 31:
			v = -math.FMA(c.f(a), c.f(cc), c.f(b))
		default:
			c.fail(i, "unknown instruction")
			return
		}
		c.setF(d, v)
		c.setFPRF(v)
		c.fpCR1(i)
		return
	}
	switch i >> 1 & 0x3ff {
	case 0, 32: // fcmpu, fcmpo
		x, y := c.f(a), c.f(b)
		var f uint32
		switch {
		case math.IsNaN(x) || math.IsNaN(y):
			f = 1
		case x < y:
			f = 8
		case x > y:
			f = 4
		default:
			f = 2
		}
		c.setCRField(d>>2, f)
		c.FPSCR = c.FPSCR&^(0xf<<12) | f<<12
		return
	case 12: // frsp
		v := c.FPR[b]
		c.FPR[d] = f32to64(f64to32(v))
		c.setFPRF(c.f(d))
	case 14, 15: // fctiw, fctiwz
		v := c.f(b)
		var r int64
		switch {
		case math.IsNaN(v):
			r = math.MinInt32
		default:
			v = c.fpRound(v, i>>1&1 != 0)
			if v >= 2147483647 {
				r = math.MaxInt32
			} else if v <= -2147483648 {
				r = math.MinInt32
			} else {
				r = int64(v)
			}
		}
		c.FPR[d] = 0xfff8000000000000 | uint64(uint32(r))
	case 814, 815: // fctid, fctidz
		v := c.f(b)
		var r int64
		switch {
		case math.IsNaN(v):
			r = math.MinInt64
		default:
			v = c.fpRound(v, i>>1&1 != 0)
			if v >= 9.223372036854775807e18 {
				r = math.MaxInt64
			} else if v <= -9.223372036854775808e18 {
				r = math.MinInt64
			} else {
				r = int64(v)
			}
		}
		c.FPR[d] = uint64(r)
	case 846: // fcfid
		v := float64(int64(c.FPR[b]))
		c.setF(d, v)
		c.setFPRF(v)
	case 40: // fneg
		c.FPR[d] = c.FPR[b] ^ 1<<63
	case 72: // fmr
		c.FPR[d] = c.FPR[b]
	case 136: // fnabs
		c.FPR[d] = c.FPR[b] | 1<<63
	case 264: // fabs
		c.FPR[d] = c.FPR[b] &^ (1 << 63)
	case 38: // mtfsb1
		c.FPSCR |= 1 << (31 - d)
	case 70: // mtfsb0
		c.FPSCR &^= 1 << (31 - d)
	case 64: // mcrfs
		sh := 28 - 4*(a>>2)
		c.setCRField(d>>2, c.FPSCR>>sh)
		c.FPSCR &^= (0x9ff80700 & (0xf << sh)) // clear the exception bits copied
		return
	case 134: // mtfsfi
		sh := 28 - 4*(d>>2)
		c.FPSCR = c.FPSCR&^(0xf<<sh) | (i>>12&0xf)<<sh
	case 583: // mffs
		c.FPR[d] = 0xfff8000000000000 | uint64(c.FPSCR)
	case 711: // mtfsf
		fm := i >> 17 & 0xff
		var m uint32
		for k := 0; k < 8; k++ {
			if fm&(0x80>>k) != 0 {
				m |= 0xf0000000 >> (4 * k)
			}
		}
		c.FPSCR = c.FPSCR&^m | uint32(c.FPR[b])&m
	default:
		c.fail(i, "unknown instruction")
		return
	}
	c.fpCR1(i)
}

// fpRound rounds to an integer by FPSCR[RN] (or toward zero).
func (c *PPU) fpRound(v float64, zero bool) float64 {
	if zero {
		return math.Trunc(v)
	}
	switch c.FPSCR & 3 {
	case 0:
		return math.RoundToEven(v)
	case 1:
		return math.Trunc(v)
	case 2:
		return math.Ceil(v)
	default:
		return math.Floor(v)
	}
}
