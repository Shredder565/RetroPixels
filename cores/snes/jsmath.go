package snes

// Math.sin, cos, tan, exp, log2, hypot and round as the web core's JavaScript
// engine computes them, so tables and chip results (DSP-1, Cx4, the sound
// DSP's interpolation kernel) come out the same to the last bit. Go's math
// package uses other algorithms that differ in the last place for a good part
// of the inputs. V8 (Node 22) uses fdlibm's: these are __kernel_sin /
// __kernel_cos / __kernel_tan with __ieee754_rem_pio2 (arguments up to
// 2^19 * pi/2, all this code needs), __ieee754_exp and FreeBSD's log2; hypot
// is V8's own (scaled Kahan sum). jsmath_test.go checks them against Node.
//
// fdlibm: Copyright (C) 1993-2004 by Sun Microsystems, Inc. All rights reserved.
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this software is freely
// granted, provided that this notice is preserved.

import "math"

func fdHi(x float64) int32  { return int32(math.Float64bits(x) >> 32) }
func fdLo(x float64) uint32 { return uint32(math.Float64bits(x)) }
func fdWords(hi int32, lo uint32) float64 {
	return math.Float64frombits(uint64(uint32(hi))<<32 | uint64(lo))
}
func fdSetLo(x float64, lo uint32) float64 { return fdWords(fdHi(x), lo) }
func fdSetHi(x float64, hi int32) float64  { return fdWords(hi, fdLo(x)) }

const (
	kS1 = -1.66666666666666324348e-01
	kS2 = 8.33333333332248946124e-03
	kS3 = -1.98412698298579493134e-04
	kS4 = 2.75573137070700676789e-06
	kS5 = -2.50507602534068634195e-08
	kS6 = 1.58969099521155010221e-10

	kC1 = 4.16666666666666019037e-02
	kC2 = -1.38888888888741095749e-03
	kC3 = 2.48015872894767294178e-05
	kC4 = -2.75573143513906633035e-07
	kC5 = 2.08757232129817482790e-09
	kC6 = -1.13596475577881948265e-11
)

func kernelSin(x, y float64, iy int) float64 {
	ix := fdHi(x) & 0x7fffffff
	if ix < 0x3e400000 && int(x) == 0 {
		return x
	}
	z := x * x
	v := z * x
	r := kS2 + z*(kS3+z*(kS4+z*(kS5+z*kS6)))
	if iy == 0 {
		return x + v*(kS1+z*r)
	}
	return x - ((z*(0.5*y-v*r) - y) - v*kS1)
}

func kernelCos(x, y float64) float64 {
	ix := fdHi(x) & 0x7fffffff
	if ix < 0x3e400000 && int(x) == 0 {
		return 1
	}
	z := x * x
	r := z * (kC1 + z*(kC2+z*(kC3+z*(kC4+z*(kC5+z*kC6)))))
	if ix < 0x3fd33333 {
		return 1 - (0.5*z - (z*r - x*y))
	}
	var qx float64
	if ix > 0x3fe90000 {
		qx = 0.28125
	} else {
		qx = fdWords(ix-0x00200000, 0)
	}
	hz := 0.5*z - qx
	a := 1 - qx
	return a - (hz - (z*r - x*y))
}

var kT = [13]float64{
	3.33333333333334091986e-01,
	1.33333333333201242699e-01,
	5.39682539762260521377e-02,
	2.18694882948595424599e-02,
	8.86323982359930005737e-03,
	3.59207910759131235356e-03,
	1.45620945432529025516e-03,
	5.88041240820264096874e-04,
	2.46463134818469906812e-04,
	7.81794442939557092300e-05,
	7.14072491382608190305e-05,
	-1.85586374855275456654e-05,
	2.59073051863633712884e-05,
}

const (
	kPio4   = 7.85398163397448278999e-01
	kPio4lo = 3.06161699786838301793e-17
)

func kernelTan(x, y float64, iy int) float64 {
	hx := fdHi(x)
	ix := hx & 0x7fffffff
	if ix < 0x3e300000 && int(x) == 0 {
		if (uint32(ix)|fdLo(x))|uint32(iy+1) == 0 {
			return 1 / math.Abs(x)
		}
		if iy == 1 {
			return x
		}
		w := x + y
		z := fdSetLo(w, 0)
		v := y - (z - x)
		a := -1 / w
		t := fdSetLo(a, 0)
		s := 1 + t*z
		return t + a*(s+t*v)
	}
	if ix >= 0x3fe59428 {
		if hx < 0 {
			x, y = -x, -y
		}
		z := kPio4 - x
		w := kPio4lo - y
		x = z + w
		y = 0
	}
	z := x * x
	w := z * z
	r := kT[1] + w*(kT[3]+w*(kT[5]+w*(kT[7]+w*(kT[9]+w*kT[11]))))
	v := z * (kT[2] + w*(kT[4]+w*(kT[6]+w*(kT[8]+w*(kT[10]+w*kT[12])))))
	s := z * x
	r = y + z*(s*(r+v)+y)
	r += kT[0] * s
	w = x + r
	if ix >= 0x3fe59428 {
		v = float64(iy)
		return float64(1-((hx>>30)&2)) * (v - 2.0*(x-(w*w/(w+v)-r)))
	}
	if iy == 1 {
		return w
	}
	z = fdSetLo(w, 0)
	v = r - (z - x)
	a := -1.0 / w
	t := fdSetLo(a, 0)
	s = 1.0 + t*z
	return t + a*(s+t*v)
}

var npio2hw = [32]int32{
	0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
	0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
	0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
	0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
	0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
	0x404858EB, 0x404921FB,
}

const (
	invpio2 = 6.36619772367581382433e-01
	pio2_1  = 1.57079632673412561417e+00
	pio2_1t = 6.07710050650619224932e-11
	pio2_2  = 6.07710050630396597660e-11
	pio2_2t = 2.02226624879595063154e-21
	pio2_3  = 2.02226624871116645580e-21
	pio2_3t = 8.47842766036889956997e-32
)

// remPio2 reduces x by multiples of pi/2; ok is false past 2^19 * pi/2.
func remPio2(x float64) (n int, y0, y1 float64, ok bool) {
	hx := fdHi(x)
	ix := hx & 0x7fffffff
	if ix <= 0x3fe921fb {
		return 0, x, 0, true
	}
	if ix < 0x4002d97c {
		if hx > 0 {
			z := x - pio2_1
			if ix != 0x3ff921fb {
				y0 = z - pio2_1t
				y1 = (z - y0) - pio2_1t
			} else {
				z -= pio2_2
				y0 = z - pio2_2t
				y1 = (z - y0) - pio2_2t
			}
			return 1, y0, y1, true
		}
		z := x + pio2_1
		if ix != 0x3ff921fb {
			y0 = z + pio2_1t
			y1 = (z - y0) + pio2_1t
		} else {
			z += pio2_2
			y0 = z + pio2_2t
			y1 = (z - y0) + pio2_2t
		}
		return -1, y0, y1, true
	}
	if ix > 0x413921fb {
		return 0, 0, 0, false
	}
	t := math.Abs(x)
	n = int(t*invpio2 + 0.5)
	fn := float64(n)
	r := t - fn*pio2_1
	w := fn * pio2_1t
	if n < 32 && ix != npio2hw[n-1] {
		y0 = r - w
	} else {
		j := ix >> 20
		y0 = r - w
		i := j - ((fdHi(y0) >> 20) & 0x7ff)
		if i > 16 {
			t = r
			w = fn * pio2_2
			r = t - w
			w = fn*pio2_2t - ((t - r) - w)
			y0 = r - w
			i = j - ((fdHi(y0) >> 20) & 0x7ff)
			if i > 49 {
				t = r
				w = fn * pio2_3
				r = t - w
				w = fn*pio2_3t - ((t - r) - w)
				y0 = r - w
			}
		}
	}
	y1 = (r - y0) - w
	if hx < 0 {
		return -n, -y0, -y1, true
	}
	return n, y0, y1, true
}

func jsSin(x float64) float64 {
	ix := fdHi(x) & 0x7fffffff
	if ix <= 0x3fe921fb {
		return kernelSin(x, 0, 0)
	}
	if ix >= 0x7ff00000 {
		return x - x
	}
	n, y0, y1, ok := remPio2(x)
	if !ok {
		return math.Sin(x)
	}
	switch n & 3 {
	case 0:
		return kernelSin(y0, y1, 1)
	case 1:
		return kernelCos(y0, y1)
	case 2:
		return -kernelSin(y0, y1, 1)
	}
	return -kernelCos(y0, y1)
}

func jsCos(x float64) float64 {
	ix := fdHi(x) & 0x7fffffff
	if ix <= 0x3fe921fb {
		return kernelCos(x, 0)
	}
	if ix >= 0x7ff00000 {
		return x - x
	}
	n, y0, y1, ok := remPio2(x)
	if !ok {
		return math.Cos(x)
	}
	switch n & 3 {
	case 0:
		return kernelCos(y0, y1)
	case 1:
		return -kernelSin(y0, y1, 1)
	case 2:
		return -kernelCos(y0, y1)
	}
	return kernelSin(y0, y1, 1)
}

func jsTan(x float64) float64 {
	ix := fdHi(x) & 0x7fffffff
	if ix <= 0x3fe921fb {
		return kernelTan(x, 0, 1)
	}
	if ix >= 0x7ff00000 {
		return x - x
	}
	n, y0, y1, ok := remPio2(x)
	if !ok {
		return math.Tan(x)
	}
	return kernelTan(y0, y1, 1-((n&1)<<1))
}

const (
	eLn2HI  = 6.93147180369123816490e-01
	eLn2LO  = 1.90821492927058770002e-10
	eInvln2 = 1.44269504088896338700e+00
	eP1     = 1.66666666666666019037e-01
	eP2     = -2.77777777770155933842e-03
	eP3     = 6.61375632143793436117e-05
	eP4     = -1.65339022054652515390e-06
	eP5     = 4.13813679705723846039e-08
)

// jsExp is fdlibm's __ieee754_exp (the overflow / underflow ends go to Go's,
// which agree there: +Inf, 0, or exact powers of two scaled).
func jsExp(x float64) float64 {
	hx := uint32(fdHi(x))
	xsb := int(hx >> 31 & 1)
	hx &= 0x7fffffff
	if hx >= 0x40862e42 || math.IsNaN(x) {
		return math.Exp(x)
	}
	var hi, lo float64
	k := 0
	if hx > 0x3fd62e42 {
		if hx < 0x3ff0a2b2 {
			if xsb == 0 {
				hi, lo = x-eLn2HI, eLn2LO
			} else {
				hi, lo = x+eLn2HI, -eLn2LO
			}
			k = 1 - xsb - xsb
		} else {
			half := 0.5
			if xsb != 0 {
				half = -0.5
			}
			k = int(eInvln2*x + half)
			t := float64(k)
			hi = x - t*eLn2HI
			lo = t * eLn2LO
		}
		x = hi - lo
	} else if hx < 0x3e300000 {
		return 1 + x
	}
	t := x * x
	c := x - t*(eP1+t*(eP2+t*(eP3+t*(eP4+t*eP5))))
	if k == 0 {
		return 1 - ((x*c)/(c-2.0) - x)
	}
	y := 1 - ((lo - (x*c)/(2.0-c)) - hi)
	if k >= -1021 {
		return fdSetHi(y, fdHi(y)+int32(k<<20))
	}
	return fdSetHi(y, fdHi(y)+int32((k+1000)<<20)) * 9.33263618503218878990e-302
}

const (
	lg1 = 6.666666666666735130e-01
	lg2 = 3.999999999940941908e-01
	lg3 = 2.857142874366239149e-01
	lg4 = 2.222219843214978396e-01
	lg5 = 1.818357216161805012e-01
	lg6 = 1.531383769920937332e-01
	lg7 = 1.479819860511658591e-01

	ivln2hi = 1.44269504072144627571e+00
	ivln2lo = 1.67517131648865118353e-10
)

func kLog1p(f float64) float64 {
	s := f / (2.0 + f)
	z := s * s
	w := z * z
	t1 := w * (lg2 + w*(lg4+w*lg6))
	t2 := z * (lg1 + w*(lg3+w*(lg5+w*lg7)))
	r := t2 + t1
	hfsq := 0.5 * f * f
	return s * (hfsq + r)
}

// jsLog2 is FreeBSD's e_log2.c, as V8 has it.
func jsLog2(x float64) float64 {
	hx, lx := fdHi(x), fdLo(x)
	k := int32(0)
	if hx < 0x00100000 {
		if (hx&0x7fffffff)|int32(lx) == 0 {
			return math.Inf(-1)
		}
		if hx < 0 {
			return math.NaN()
		}
		k -= 54
		x *= 1.80143985094819840000e+16
		hx = fdHi(x)
	}
	if hx >= 0x7ff00000 {
		return x + x
	}
	if hx == 0x3ff00000 && lx == 0 {
		return 0
	}
	k += (hx >> 20) - 1023
	hx &= 0x000fffff
	i := (hx + 0x95f64) & 0x100000
	x = fdSetHi(x, hx|(i^0x3ff00000))
	k += i >> 20
	y := float64(k)
	f := x - 1.0
	hfsq := 0.5 * f * f
	r := kLog1p(f)
	hi := fdSetLo(f-hfsq, 0)
	lo := (f - hi) - hfsq + r
	valHi := hi * ivln2hi
	valLo := (lo+hi)*ivln2lo + lo*ivln2hi
	w := y + valHi
	valLo += (y - w) + valHi
	valHi = w
	return valLo + valHi
}

// jsHypot is V8's Math.hypot for two numbers.
func jsHypot(a, b float64) float64 {
	a, b = math.Abs(a), math.Abs(b)
	m := max(a, b)
	if m == 0 {
		return 0
	}
	if math.IsInf(m, 0) {
		return m
	}
	sum, comp := 0.0, 0.0
	for _, v := range [2]float64{a, b} {
		n := v / m
		summand := n*n - comp
		pre := sum + summand
		comp = (pre - sum) - summand
		sum = pre
	}
	return math.Sqrt(sum) * m
}

// jsRound is Math.round: to the nearest integer, halves toward +Infinity.
func jsRound(x float64) float64 {
	c := math.Ceil(x)
	if c-0.5 > x {
		c--
	}
	return c
}
