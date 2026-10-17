package wiiu

// Floating point and paired singles.
//
// Each FPR holds a double (FPR) and a second single (PS1). As on the
// Broadway/Espresso with paired singles on: single-precision loads and
// arithmetic write both halves; double instructions write only the first.

import (
	"math"
	"math/big"
)

// exact single <-> double conversions that keep NaN payloads (programs copy
// plain data through the FPRs)
func f32to64(b uint32) float64 {
	if b&0x7f800000 == 0x7f800000 && b&0x7fffff != 0 {
		return math.Float64frombits(uint64(b>>31)<<63 | 0x7ff<<52 | uint64(b&0x7fffff)<<29)
	}
	return float64(math.Float32frombits(b))
}

func f64to32(v float64) uint32 {
	b := math.Float64bits(v)
	if b&0x7ff0000000000000 == 0x7ff0000000000000 && b&0xfffffffffffff != 0 {
		return uint32(b>>63)<<31 | 0x7f800000 | uint32(b>>29)&0x7fffff
	}
	return math.Float32bits(float32(v))
}

// fmas is a*c+b rounded once to single precision (rounding the double FMA
// to single again would be off by one in the last place when the double
// lands exactly halfway between two singles)
func fmas(a, c, b float64) float64 {
	r := math.FMA(a, c, b)
	bits := math.Float64bits(r)
	if bits&0x1fffffff != 0x10000000 || math.IsInf(r, 0) || math.IsNaN(r) {
		return rs(r)
	}
	x := new(big.Float).SetPrec(2000).SetFloat64(a)
	x.Mul(x, new(big.Float).SetPrec(2000).SetFloat64(c))
	x.Add(x, new(big.Float).SetPrec(2000).SetFloat64(b))
	f, _ := x.Float32()
	return float64(f)
}

// round a double to single precision
func rs(v float64) float64 { return float64(float32(v)) }

// the Broadway rounds the multiplier of single-precision multiplies to 25
// bits of mantissa
func force25(v float64) float64 {
	b := math.Float64bits(v)
	if b&0x7ff0000000000000 == 0x7ff0000000000000 {
		return v
	}
	b = (b & 0xfffffffff8000000) + (b & 0x8000000)
	return math.Float64frombits(b)
}

func (c *CPU) setFPRSingle(d uint32, v float64) {
	v = rs(v)
	c.FPR[d] = v
	c.PS1[d] = v
}

func (c *CPU) fpCR1() {
	c.CR = c.CR&^0x0f000000 | (c.FPSCR>>28)<<24
}

func (c *CPU) fcmp(crf uint32, a, b float64) {
	var f uint32
	switch {
	case math.IsNaN(a) || math.IsNaN(b):
		f = 1
	case a < b:
		f = 8
	case a > b:
		f = 4
	default:
		f = 2
	}
	c.setCRField(crf, f)
	c.FPSCR = c.FPSCR&^0xf000 | f<<12
}

// fctiw: to a 32-bit integer with the FPSCR rounding mode (or toward zero)
func (c *CPU) toInt(v float64, trunc bool) uint32 {
	if math.IsNaN(v) {
		return 0x80000000
	}
	var r float64
	mode := c.FPSCR & 3
	if trunc {
		mode = 1
	}
	switch mode {
	case 0:
		r = math.RoundToEven(v)
	case 1:
		r = math.Trunc(v)
	case 2:
		r = math.Ceil(v)
	default:
		r = math.Floor(v)
	}
	if r >= 2147483647 {
		return 0x7fffffff
	}
	if r <= -2147483648 {
		return 0x80000000
	}
	return uint32(int32(r))
}

func (c *CPU) exec59(inst uint32) {
	d := inst >> 21 & 31
	a := inst >> 16 & 31
	b := inst >> 11 & 31
	cc := inst >> 6 & 31
	fa, fb, fc := c.FPR[a], c.FPR[b], c.FPR[cc]
	switch inst >> 1 & 31 {
	case 18:
		c.setFPRSingle(d, fa/fb)
	case 20:
		c.setFPRSingle(d, fa-fb)
	case 21:
		c.setFPRSingle(d, fa+fb)
	case 22:
		c.setFPRSingle(d, math.Sqrt(fb))
	case 24: // fres
		c.setFPRSingle(d, 1/fb)
	case 25:
		c.setFPRSingle(d, fa*force25(fc))
	case 28:
		c.setFPRSingle(d, fmas(fa, force25(fc), -fb))
	case 29:
		c.setFPRSingle(d, fmas(fa, force25(fc), fb))
	case 30:
		c.setFPRSingle(d, -fmas(fa, force25(fc), -fb))
	case 31:
		c.setFPRSingle(d, -fmas(fa, force25(fc), fb))
	default:
		c.fail(inst, "unknown instruction")
		return
	}
	if inst&1 != 0 {
		c.fpCR1()
	}
}

func (c *CPU) exec63(inst uint32) {
	d := inst >> 21 & 31
	a := inst >> 16 & 31
	b := inst >> 11 & 31
	cc := inst >> 6 & 31
	fa, fb, fc := c.FPR[a], c.FPR[b], c.FPR[cc]
	switch inst >> 1 & 31 {
	case 18:
		c.FPR[d] = fa / fb
		goto done
	case 20:
		c.FPR[d] = fa - fb
		goto done
	case 21:
		c.FPR[d] = fa + fb
		goto done
	case 22:
		c.FPR[d] = math.Sqrt(fb)
		goto done
	case 23: // fsel
		if fa >= 0 {
			c.FPR[d] = fc
		} else {
			c.FPR[d] = fb
		}
		goto done
	case 25:
		c.FPR[d] = fa * fc
		goto done
	case 26: // frsqrte
		c.FPR[d] = 1 / math.Sqrt(fb)
		goto done
	case 28:
		c.FPR[d] = math.FMA(fa, fc, -fb)
		goto done
	case 29:
		c.FPR[d] = math.FMA(fa, fc, fb)
		goto done
	case 30:
		c.FPR[d] = -math.FMA(fa, fc, -fb)
		goto done
	case 31:
		c.FPR[d] = -math.FMA(fa, fc, fb)
		goto done
	}
	switch inst >> 1 & 0x3ff {
	case 0, 32: // fcmpu, fcmpo
		c.fcmp(d>>2, fa, fb)
		return
	case 12: // frsp
		c.setFPRSingle(d, fb)
	case 14: // fctiw
		c.FPR[d] = math.Float64frombits(0xfff8000000000000 | uint64(c.toInt(fb, false)))
	case 15: // fctiwz
		c.FPR[d] = math.Float64frombits(0xfff8000000000000 | uint64(c.toInt(fb, true)))
	case 38: // mtfsb1
		c.FPSCR |= 0x80000000 >> d
	case 40: // fneg
		c.FPR[d] = math.Float64frombits(math.Float64bits(fb) ^ 1<<63)
	case 64: // mcrfs
		f := c.FPSCR >> ((7 - a>>2) * 4) & 0xf
		c.setCRField(d>>2, f)
	case 70: // mtfsb0
		c.FPSCR &^= 0x80000000 >> d
	case 72: // fmr
		c.FPR[d] = fb
	case 134: // mtfsfi
		sh := (7 - d>>2) * 4
		c.FPSCR = c.FPSCR&^(0xf<<sh) | (inst>>12&0xf)<<sh
	case 136: // fnabs
		c.FPR[d] = math.Float64frombits(math.Float64bits(fb) | 1<<63)
	case 264: // fabs
		c.FPR[d] = math.Float64frombits(math.Float64bits(fb) &^ (1 << 63))
	case 583: // mffs
		c.FPR[d] = math.Float64frombits(0xfff8000000000000 | uint64(c.FPSCR))
	case 711: // mtfsf
		fm := inst >> 17 & 0xff
		var mask uint32
		for i := uint32(0); i < 8; i++ {
			if fm&(0x80>>i) != 0 {
				mask |= 0xf0000000 >> (i * 4)
			}
		}
		c.FPSCR = c.FPSCR&^mask | uint32(math.Float64bits(fb))&mask
	default:
		c.fail(inst, "unknown instruction")
		return
	}
done:
	if inst&1 != 0 {
		c.fpCR1()
	}
}

// quantized loads and stores (psq_l / psq_st)

func gqrScale(s uint32) float64 {
	n := int(s & 0x3f)
	if n >= 32 {
		n -= 64
	}
	return math.Ldexp(1, n)
}

func (c *CPU) qload(ea uint32, typ, scale uint32) (float64, uint32) {
	m := c.Mem
	switch typ {
	case 4: // u8
		return float64(m.Read8(ea)) / gqrScale(scale), 1
	case 5: // u16
		return float64(m.Read16(ea)) / gqrScale(scale), 2
	case 6: // s8
		return float64(int8(m.Read8(ea))) / gqrScale(scale), 1
	case 7: // s16
		return float64(int16(m.Read16(ea))) / gqrScale(scale), 2
	}
	return f32to64(m.Read32(ea)), 4
}

func clampQ(v, lo, hi float64) float64 {
	if v != v {
		return 0
	}
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func (c *CPU) qstore(ea uint32, v float64, typ, scale uint32) uint32 {
	m := c.Mem
	switch typ {
	case 4:
		m.Write8(ea, uint8(clampQ(v*gqrScale(scale), 0, 255)))
		return 1
	case 5:
		m.Write16(ea, uint16(clampQ(v*gqrScale(scale), 0, 65535)))
		return 2
	case 6:
		m.Write8(ea, uint8(int8(clampQ(v*gqrScale(scale), -128, 127))))
		return 1
	case 7:
		m.Write16(ea, uint16(int16(clampQ(v*gqrScale(scale), -32768, 32767))))
		return 2
	}
	m.Write32(ea, f64to32(v))
	return 4
}

func (c *CPU) psqLoad(d, ea, w, i uint32) {
	g := c.GQR[i]
	typ, scale := g>>16&7, g>>24&0x3f
	v0, n := c.qload(ea, typ, scale)
	c.FPR[d] = v0
	if w != 0 {
		c.PS1[d] = 1
	} else {
		c.PS1[d], _ = c.qload(ea+n, typ, scale)
	}
}

func (c *CPU) psqStore(s, ea, w, i uint32) {
	g := c.GQR[i]
	typ, scale := g&7, g>>8&0x3f
	n := c.qstore(ea, c.FPR[s], typ, scale)
	if w == 0 {
		c.qstore(ea+n, c.PS1[s], typ, scale)
	}
}

func (c *CPU) execFPMem(inst uint32) {
	op := inst >> 26
	d := inst >> 21 & 31
	ra := inst >> 16 & 31
	m := c.Mem
	switch op {
	case 48: // lfs
		v := f32to64(m.Read32(c.eaD(inst)))
		c.FPR[d], c.PS1[d] = v, v
	case 49: // lfsu
		ea := c.GPR[ra] + uint32(int32(int16(inst)))
		v := f32to64(m.Read32(ea))
		c.FPR[d], c.PS1[d] = v, v
		c.GPR[ra] = ea
	case 50: // lfd
		c.FPR[d] = math.Float64frombits(m.Read64(c.eaD(inst)))
	case 51: // lfdu
		ea := c.GPR[ra] + uint32(int32(int16(inst)))
		c.FPR[d] = math.Float64frombits(m.Read64(ea))
		c.GPR[ra] = ea
	case 52: // stfs
		m.Write32(c.eaD(inst), f64to32(c.FPR[d]))
	case 53: // stfsu
		ea := c.GPR[ra] + uint32(int32(int16(inst)))
		m.Write32(ea, f64to32(c.FPR[d]))
		c.GPR[ra] = ea
	case 54: // stfd
		m.Write64(c.eaD(inst), math.Float64bits(c.FPR[d]))
	case 55: // stfdu
		ea := c.GPR[ra] + uint32(int32(int16(inst)))
		m.Write64(ea, math.Float64bits(c.FPR[d]))
		c.GPR[ra] = ea
	case 56, 57, 60, 61: // psq_l, psq_lu, psq_st, psq_stu
		off := uint32(int32(inst<<20) >> 20)
		ea := off
		if ra != 0 || op == 57 || op == 61 {
			ea += c.GPR[ra]
		}
		w, i := inst>>15&1, inst>>12&7
		if op < 60 {
			c.psqLoad(d, ea, w, i)
		} else {
			c.psqStore(d, ea, w, i)
		}
		if op == 57 || op == 61 {
			c.GPR[ra] = ea
		}
	}
}

func (c *CPU) execFPMemX(inst uint32) {
	d := inst >> 21 & 31
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	m := c.Mem
	switch inst >> 1 & 0x3ff {
	case 535: // lfsx
		v := f32to64(m.Read32(c.eaX(inst)))
		c.FPR[d], c.PS1[d] = v, v
	case 567: // lfsux
		ea := c.GPR[ra] + c.GPR[rb]
		v := f32to64(m.Read32(ea))
		c.FPR[d], c.PS1[d] = v, v
		c.GPR[ra] = ea
	case 599: // lfdx
		c.FPR[d] = math.Float64frombits(m.Read64(c.eaX(inst)))
	case 631: // lfdux
		ea := c.GPR[ra] + c.GPR[rb]
		c.FPR[d] = math.Float64frombits(m.Read64(ea))
		c.GPR[ra] = ea
	case 663: // stfsx
		m.Write32(c.eaX(inst), f64to32(c.FPR[d]))
	case 695: // stfsux
		ea := c.GPR[ra] + c.GPR[rb]
		m.Write32(ea, f64to32(c.FPR[d]))
		c.GPR[ra] = ea
	case 727: // stfdx
		m.Write64(c.eaX(inst), math.Float64bits(c.FPR[d]))
	case 759: // stfdux
		ea := c.GPR[ra] + c.GPR[rb]
		m.Write64(ea, math.Float64bits(c.FPR[d]))
		c.GPR[ra] = ea
	case 983: // stfiwx
		m.Write32(c.eaX(inst), uint32(math.Float64bits(c.FPR[d])))
	}
}

// primary opcode 4: paired singles
func (c *CPU) execPS(inst uint32) {
	d := inst >> 21 & 31
	a := inst >> 16 & 31
	b := inst >> 11 & 31
	cc := inst >> 6 & 31

	switch inst >> 1 & 63 {
	case 6, 7, 38, 39: // psq_lx, psq_stx, psq_lux, psq_stux
		ea := c.GPR[b]
		x := inst >> 1 & 63
		if a != 0 || x >= 38 {
			ea += c.GPR[a]
		}
		w, i := inst>>10&1, inst>>7&7
		if x&1 == 0 {
			c.psqLoad(d, ea, w, i)
		} else {
			c.psqStore(d, ea, w, i)
		}
		if x >= 38 {
			c.GPR[a] = ea
		}
		return
	}

	a0, a1 := c.FPR[a], c.PS1[a]
	b0, b1 := c.FPR[b], c.PS1[b]
	c0, c1 := c.FPR[cc], c.PS1[cc]
	set := func(v0, v1 float64) {
		c.FPR[d], c.PS1[d] = rs(v0), rs(v1)
	}
	switch inst >> 1 & 31 {
	case 10: // ps_sum0
		set(a0+b1, c1)
		goto done
	case 11: // ps_sum1
		set(c0, a0+b1)
		goto done
	case 12: // ps_muls0
		s := force25(c0)
		set(a0*s, a1*s)
		goto done
	case 13: // ps_muls1
		s := force25(c1)
		set(a0*s, a1*s)
		goto done
	case 14: // ps_madds0
		s := force25(c0)
		set(fmas(a0, s, b0), fmas(a1, s, b1))
		goto done
	case 15: // ps_madds1
		s := force25(c1)
		set(fmas(a0, s, b0), fmas(a1, s, b1))
		goto done
	case 18: // ps_div
		set(a0/b0, a1/b1)
		goto done
	case 20: // ps_sub
		set(a0-b0, a1-b1)
		goto done
	case 21: // ps_add
		set(a0+b0, a1+b1)
		goto done
	case 23: // ps_sel
		r0, r1 := b0, b1
		if a0 >= 0 {
			r0 = c0
		}
		if a1 >= 0 {
			r1 = c1
		}
		c.FPR[d], c.PS1[d] = r0, r1
		goto done
	case 24: // ps_res
		set(1/b0, 1/b1)
		goto done
	case 25: // ps_mul
		set(a0*force25(c0), a1*force25(c1))
		goto done
	case 26: // ps_rsqrte
		c.FPR[d], c.PS1[d] = rs(1/math.Sqrt(b0)), rs(1/math.Sqrt(b1))
		goto done
	case 28: // ps_msub
		set(fmas(a0, force25(c0), -b0), fmas(a1, force25(c1), -b1))
		goto done
	case 29: // ps_madd
		set(fmas(a0, force25(c0), b0), fmas(a1, force25(c1), b1))
		goto done
	case 30: // ps_nmsub
		set(-fmas(a0, force25(c0), -b0), -fmas(a1, force25(c1), -b1))
		goto done
	case 31: // ps_nmadd
		set(-fmas(a0, force25(c0), b0), -fmas(a1, force25(c1), b1))
		goto done
	}
	switch inst >> 1 & 0x3ff {
	case 0, 32: // ps_cmpu0, ps_cmpo0
		c.fcmp(d>>2, a0, b0)
		return
	case 64, 96: // ps_cmpu1, ps_cmpo1
		c.fcmp(d>>2, a1, b1)
		return
	case 40: // ps_neg
		c.FPR[d] = math.Float64frombits(math.Float64bits(b0) ^ 1<<63)
		c.PS1[d] = math.Float64frombits(math.Float64bits(b1) ^ 1<<63)
	case 72: // ps_mr
		c.FPR[d], c.PS1[d] = b0, b1
	case 136: // ps_nabs
		c.FPR[d] = math.Float64frombits(math.Float64bits(b0) | 1<<63)
		c.PS1[d] = math.Float64frombits(math.Float64bits(b1) | 1<<63)
	case 264: // ps_abs
		c.FPR[d] = math.Float64frombits(math.Float64bits(b0) &^ (1 << 63))
		c.PS1[d] = math.Float64frombits(math.Float64bits(b1) &^ (1 << 63))
	case 528: // ps_merge00
		c.FPR[d], c.PS1[d] = a0, rs(b0)
	case 560: // ps_merge01
		c.FPR[d], c.PS1[d] = a0, b1
	case 592: // ps_merge10
		c.FPR[d], c.PS1[d] = a1, rs(b0)
	case 624: // ps_merge11
		c.FPR[d], c.PS1[d] = a1, b1
	case 1014: // dcbz_l
		ea := c.eaX(inst) &^ 31
		c.Mem.Fill(ea, 0, 32)
		return
	default:
		c.fail(inst, "unknown instruction")
		return
	}
done:
	if inst&1 != 0 {
		c.fpCR1()
	}
}
