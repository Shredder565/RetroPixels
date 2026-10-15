package nes

import (
	"math"
	"math/big"
)

// Math functions with JavaScript's results, so the tables computed from them
// (the palette and the sound chip tables) match the web version.
//
// Current browsers (V8 with LLVM's libc) return correctly rounded results for
// Math.sin, Math.cos, Math.pow and Math.log2, so these are computed the same
// way: in high precision, then rounded to the nearest double. (Older engines,
// such as Node 22's, can be one unit in the last place away; see
// TestTablesMatchJS.) They are slow, so they are only used to build tables.

const jsPrec = 160

func newF(x float64) *big.Float { return new(big.Float).SetPrec(jsPrec).SetFloat64(x) }

func rounded(f *big.Float) float64 { v, _ := f.Float64(); return v }

// bigExp returns e**x.
func bigExp(x *big.Float) *big.Float {
	// x = k*ln2 + r, then exp(r) = exp(r/2^10)^(2^10)
	ln2 := bigLn2()
	kf := new(big.Float).SetPrec(jsPrec).Quo(x, ln2)
	k, _ := kf.Float64()
	ki := int(math.Round(k))
	r := new(big.Float).SetPrec(jsPrec).Sub(x, new(big.Float).SetPrec(jsPrec).Mul(ln2, newF(float64(ki))))
	r.SetMantExp(r, -10)
	sum, term := newF(1), newF(1)
	for i := 1; i < 60; i++ {
		term.Mul(term, r)
		term.Quo(term, newF(float64(i)))
		sum.Add(sum, term)
		if term.Sign() == 0 || term.MantExp(nil)-sum.MantExp(nil) < -jsPrec {
			break
		}
	}
	for i := 0; i < 10; i++ {
		sum.Mul(sum, sum)
	}
	return sum.SetMantExp(sum, ki)
}

// bigLnM returns ln(m) for m in [1, 2]: 2*atanh((m-1)/(m+1)).
func bigLnM(m *big.Float) *big.Float {
	one := newF(1)
	z := new(big.Float).SetPrec(jsPrec).Quo(new(big.Float).SetPrec(jsPrec).Sub(m, one), new(big.Float).SetPrec(jsPrec).Add(m, one))
	z2 := new(big.Float).SetPrec(jsPrec).Mul(z, z)
	sum := new(big.Float).SetPrec(jsPrec).Set(z)
	p := new(big.Float).SetPrec(jsPrec).Set(z)
	for k := 3; k < 2000; k += 2 {
		p.Mul(p, z2)
		t := new(big.Float).SetPrec(jsPrec).Quo(p, newF(float64(k)))
		sum.Add(sum, t)
		if t.Sign() == 0 || t.MantExp(nil)-sum.MantExp(nil) < -jsPrec {
			break
		}
	}
	return sum.Mul(sum, newF(2))
}

var (
	ln2Cache *big.Float
	lnCache  = map[float64]*big.Float{} // ln of whole-number bases (10, 2), which are used many times
)

func bigLn2() *big.Float {
	if ln2Cache == nil {
		ln2Cache = bigLnM(newF(2))
	}
	return ln2Cache
}

// bigLn returns ln(x) for x > 0.
func bigLn(x float64) *big.Float {
	frac, exp := math.Frexp(x) // x = frac * 2^exp, frac in [0.5, 1)
	m := newF(frac * 2)
	l := bigLnM(m)
	return l.Add(l, new(big.Float).SetPrec(jsPrec).Mul(bigLn2(), newF(float64(exp-1))))
}

// bigSinCos returns sin(x) (cos = false) or cos(x) for a moderate x (|x| < 100).
func bigSinCos(x float64, cos bool) float64 {
	xf := newF(x)
	x2 := new(big.Float).SetPrec(jsPrec).Mul(xf, xf)
	var term *big.Float
	n := 1
	if cos {
		term, n = newF(1), 0
	} else {
		term = new(big.Float).SetPrec(jsPrec).Set(xf)
	}
	sum := new(big.Float).SetPrec(jsPrec).Set(term)
	for i := 0; i < 200; i++ {
		term.Mul(term, x2)
		term.Quo(term, newF(float64((n+1)*(n+2))))
		term.Neg(term)
		n += 2
		sum.Add(sum, term)
		if term.Sign() == 0 || (sum.Sign() != 0 && term.MantExp(nil)-sum.MantExp(nil) < -jsPrec) {
			break
		}
	}
	return rounded(sum)
}

func jsRound(x float64) float64 {
	r := math.Floor(x)
	if x-r >= 0.5 {
		r++
	}
	return r
}

func jsSin(x float64) float64 { return bigSinCos(x, false) }
func jsCos(x float64) float64 { return bigSinCos(x, true) }

func jsPow(x, y float64) float64 {
	switch {
	case y == 0:
		return 1
	case x == 0:
		if y > 0 {
			return 0
		}
		return math.Inf(1)
	case x < 0 || math.IsNaN(x) || math.IsNaN(y) || math.IsInf(x, 0) || math.IsInf(y, 0):
		return math.Pow(x, y)
	}
	if y == math.Trunc(y) && math.Abs(y) < 64 && x == math.Trunc(x) && math.Abs(x) <= 16 && y > 0 {
		return math.Pow(x, y) // exact
	}
	l, ok := lnCache[x]
	if !ok {
		l = bigLn(x)
		if x == math.Trunc(x) {
			lnCache[x] = l
		}
	}
	return rounded(bigExp(new(big.Float).SetPrec(jsPrec).Mul(l, newF(y))))
}

func jsLog2(x float64) float64 {
	if x <= 0 || math.IsNaN(x) || math.IsInf(x, 0) {
		return math.Log2(x)
	}
	if f, e := math.Frexp(x); f == 0.5 {
		return float64(e - 1)
	}
	l := bigLn(x)
	return rounded(l.Quo(l, bigLn2()))
}
