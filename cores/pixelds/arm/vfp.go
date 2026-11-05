package arm

import (
	"math"
	"math/big"
)

// The ARM11's VFPv2 floating-point unit (coprocessors 10 and 11):
// 32 single registers that pair up as 16 doubles, FPSCR with its flags,
// flush-to-zero, default NaN and rounding mode, and every VFPv2
// instruction. Short vectors (FPSCR LEN) aren't used by 3DS compilers and
// run as scalars.

type vfp struct {
	S     [32]uint32
	FPSCR uint32
	FPEXC uint32
}

const (
	fpscrFZ = 1 << 24
	fpscrDN = 1 << 25
)

func (c *CPU) sreg(i uint32) float32 { return c.flushIn32(math.Float32frombits(c.fp.S[i])) }
func (c *CPU) dreg(i uint32) float64 {
	return c.flushIn64(math.Float64frombits(uint64(c.fp.S[i*2]) | uint64(c.fp.S[i*2+1])<<32))
}

func (c *CPU) setS(i uint32, v float32) {
	v = c.flushOut32(v)
	b := math.Float32bits(v)
	if v != v && c.fp.FPSCR&fpscrDN != 0 {
		b = 0x7fc00000
	}
	c.fp.S[i] = b
}

func (c *CPU) setD(i uint32, v float64) {
	v = c.flushOut64(v)
	b := math.Float64bits(v)
	if v != v && c.fp.FPSCR&fpscrDN != 0 {
		b = 0x7ff8000000000000
	}
	c.fp.S[i*2], c.fp.S[i*2+1] = uint32(b), uint32(b>>32)
}

func (c *CPU) flushIn32(v float32) float32 {
	if c.fp.FPSCR&fpscrFZ != 0 && v != 0 && math.Abs(float64(v)) < 0x1p-126 {
		return float32(math.Copysign(0, float64(v)))
	}
	return v
}

func (c *CPU) flushOut32(v float32) float32 { return c.flushIn32(v) }

func (c *CPU) flushIn64(v float64) float64 {
	if c.fp.FPSCR&fpscrFZ != 0 && v != 0 && math.Abs(v) < 0x1p-1022 {
		return math.Copysign(0, v)
	}
	return v
}

func (c *CPU) flushOut64(v float64) float64 { return c.flushIn64(v) }

func isVFP(op uint32) bool { cp := op >> 8 & 15; return cp == 10 || cp == 11 }

// vfpDataProc: CDP-form VFP instructions (arithmetic, compares,
// conversions).
func (c *CPU) vfpDataProc(op uint32) {
	dbl := op>>8&15 == 11
	fd, fn, fm := op>>12&15, op>>16&15, op&15
	D, N, M := op>>22&1, op>>7&1, op>>5&1
	sd, sn, sm := fd<<1|D, fn<<1|N, fm<<1|M
	opc := op>>20&8 | op>>19&4 | op>>19&2 | op>>6&1 // p q r s
	c.Cycles++
	if opc != 15 {
		if dbl {
			c.setDBits(fd, c.arith64(opc, c.dbits(fd), c.dbits(fn), c.dbits(fm)))
		} else {
			c.setSBits(sd, uint64(c.arith32(opc, c.fp.S[sd], c.fp.S[sn], c.fp.S[sm])))
		}
		if opc == 8 {
			c.Cycles += 14
		}
		return
	}
	ext := fn<<1 | N
	switch ext {
	case 0, 1, 2, 3: // FCPY, FABS, FNEG, FSQRT
		if dbl {
			if ext < 3 { // bit operations: no flushing
				b := uint64(c.fp.S[fm*2]) | uint64(c.fp.S[fm*2+1])<<32
				switch ext {
				case 1:
					b &^= 1 << 63
				case 2:
					b ^= 1 << 63
				}
				c.fp.S[fd*2], c.fp.S[fd*2+1] = uint32(b), uint32(b>>32)
				return
			}
			b := c.dbits(fm)
			if r, ok := c.pickNaN(&f64, b); ok {
				c.setDBits(fd, r)
			} else {
				x := c.fin64(b)
				q := math.Sqrt(x)
				c.setDBits(fd, c.out64(round64(q, c.sqrtSign(x, q), c.rmode())))
			}
			c.Cycles += 28
			return
		}
		if ext < 3 {
			b := c.fp.S[sm]
			switch ext {
			case 1:
				b &^= 1 << 31
			case 2:
				b ^= 1 << 31
			}
			c.fp.S[sd] = b
			return
		}
		b := uint64(c.fp.S[sm])
		if r, ok := c.pickNaN(&f32, b); ok {
			c.fp.S[sd] = uint32(r)
		} else {
			x := float64(c.fin32(b))
			q := math.Sqrt(x)
			c.fp.S[sd] = uint32(c.out32(round32(q, c.sqrtSign(x, q), c.rmode())))
		}
		c.Cycles += 14
	case 8, 9, 10, 11: // FCMP, FCMPE, FCMPZ, FCMPEZ
		var a, b float64
		if dbl {
			a = c.dreg(fd)
			if ext < 10 {
				b = c.dreg(fm)
			}
		} else {
			a = float64(c.sreg(sd))
			if ext < 10 {
				b = float64(c.sreg(sm))
			}
		}
		var f uint32
		switch {
		case a != a || b != b:
			f = 0x3 // C V
		case a == b:
			f = 0x6 // Z C
		case a < b:
			f = 0x8 // N
		default:
			f = 0x2 // C
		}
		c.fp.FPSCR = c.fp.FPSCR&0x0fffffff | f<<28
	case 15: // FCVTDS / FCVTSD
		if dbl { // double to single: Sd from Dm
			b := c.dbits(fm)
			if r, ok := c.pickNaN(&f64, b); ok {
				if r != f64.defNaN {
					r = r>>32&0x80000000 | 0x7fc00000 | r>>29&0x3fffff
				} else {
					r = f32.defNaN
				}
				c.fp.S[sd] = uint32(r)
			} else {
				c.fp.S[sd] = uint32(c.out32(round32(c.fin64(b), 0, c.rmode())))
			}
		} else { // single to double: Dd from Sm
			b := uint64(c.fp.S[sm])
			if r, ok := c.pickNaN(&f32, b); ok {
				if r != f32.defNaN {
					r = r>>31<<63 | 0x7ff8000000000000 | (r&0x3fffff)<<29
				} else {
					r = f64.defNaN
				}
				c.setDBits(fd, r)
			} else {
				c.setDBits(fd, c.out64(float64(c.fin32(b))))
			}
		}
	case 16, 17: // FUITO, FSITO: from integer Sm
		v := c.fp.S[sm]
		var f float64
		if ext == 17 {
			f = float64(int32(v))
		} else {
			f = float64(v)
		}
		if dbl {
			c.setD(fd, f)
		} else {
			c.setS(sd, round32(f, 0, c.rmode()))
		}
	case 24, 25, 26, 27: // FTOUI, FTOUIZ, FTOSI, FTOSIZ: to integer Sd
		var f float64
		if dbl {
			f = c.dreg(fm)
		} else {
			f = float64(c.sreg(sm))
		}
		mode := c.fp.FPSCR >> 22 & 3
		if ext&1 != 0 {
			mode = 3
		}
		switch mode {
		case 0:
			f = math.RoundToEven(f)
		case 1:
			f = math.Ceil(f)
		case 2:
			f = math.Floor(f)
		default:
			f = math.Trunc(f)
		}
		var r uint32
		if ext >= 26 {
			switch {
			case f != f:
				r = 0
			case f >= 2147483647:
				r = 0x7fffffff
			case f <= -2147483648:
				r = 0x80000000
			default:
				r = uint32(int32(f))
			}
		} else {
			switch {
			case f != f, f <= 0:
				r = 0
			case f >= 4294967295:
				r = 0xffffffff
			default:
				r = uint32(f)
			}
		}
		c.fp.S[sd] = r
	default:
		c.undefined(op)
	}
}

// vfpTransfer: MRC/MCR-form VFP instructions (FMSR, FMRS, FMDLR, FMRDL,
// FMDHR, FMRDH, FMXR, FMRX and FMSTAT).
func (c *CPU) vfpTransfer(op uint32) {
	dbl := op>>8&15 == 11
	opc := op >> 21 & 7
	load := op&(1<<20) != 0 // to an ARM register
	rd := op >> 12 & 15
	fn := op >> 16 & 15
	N := op >> 7 & 1
	switch {
	case !dbl && opc == 0:
		s := fn<<1 | N
		if load {
			c.R[rd] = c.fp.S[s]
		} else {
			c.fp.S[s] = c.R[rd]
		}
	case dbl && opc < 2:
		i := fn*2 + opc
		if load {
			c.R[rd] = c.fp.S[i]
		} else {
			c.fp.S[i] = c.R[rd]
		}
	case !dbl && opc == 7:
		if load {
			var v uint32
			switch fn {
			case 0:
				v = 0x410120b4 // FPSID: ARM VFP11
			case 1:
				v = c.fp.FPSCR
			case 8:
				v = c.fp.FPEXC
			}
			if rd == 15 { // FMSTAT
				c.CPSR = c.CPSR&0x0fffffff | v&0xf0000000
			} else {
				c.R[rd] = v
			}
		} else {
			switch fn {
			case 1:
				c.fp.FPSCR = c.R[rd] & 0xf3ffff9f
			case 8:
				c.fp.FPEXC = c.R[rd] & 0xc0000000
			}
		}
	default:
		c.undefined(op)
	}
}

// vfpTwoReg: MCRR/MRRC-form VFP (FMSRR, FMRRS, FMDRR, FMRRD).
func (c *CPU) vfpTwoReg(op uint32) {
	rd, rn := op>>12&15, op>>16&15
	load := op&(1<<20) != 0
	var i uint32
	if op>>8&15 == 11 {
		i = (op & 15) * 2
	} else {
		i = (op&15)<<1 | op>>5&1
	}
	if load {
		c.R[rd], c.R[rn] = c.fp.S[i], c.fp.S[(i+1)&31]
	} else {
		c.fp.S[i], c.fp.S[(i+1)&31] = c.R[rd], c.R[rn]
	}
}

// vfpLoadStore: FLDS, FSTS, FLDD, FSTD and the multiple forms.
func (c *CPU) vfpLoadStore(op uint32) {
	dbl := op>>8&15 == 11
	p, u, w := op>>24&1, op>>23&1, op>>21&1
	load := op&(1<<20) != 0
	rn := op >> 16 & 15
	off := (op & 0xff) * 4
	base := c.R[rn]
	if rn == 15 {
		base &^= 3
	}
	first := op>>12&15<<1 | op>>22&1
	if dbl {
		first = (op >> 12 & 15) * 2
	}
	if p == 1 && w == 0 { // a single register
		a := base - off
		if u == 1 {
			a = base + off
		}
		n := uint32(1)
		if dbl {
			n = 2
		}
		for k := uint32(0); k < n; k++ {
			if load {
				c.fp.S[(first+k)&31] = c.Bus.Read32(a + 4*k)
			} else {
				c.Bus.Write32(a+4*k, c.fp.S[(first+k)&31])
			}
		}
		c.Cycles += int64(n)
		return
	}
	words := op & 0xff
	if dbl {
		words &^= 1 // FLDMX/FSTMX's extra word is skipped
	}
	a := base
	if p == 1 { // decrement before
		a = base - off
	}
	for k := uint32(0); k < words; k++ {
		if load {
			c.fp.S[(first+k)&31] = c.Bus.Read32(a + 4*k)
		} else {
			c.Bus.Write32(a+4*k, c.fp.S[(first+k)&31])
		}
	}
	if w == 1 {
		if u == 1 {
			c.R[rn] = base + off
		} else {
			c.R[rn] = base - off
		}
	}
	c.Cycles += int64(words)
}

// ---- arithmetic with ARM's NaN rules ----
//
// A NaN operand gives that NaN back (the first signalling one, quietened,
// else the first quiet one), or the default NaN with FPSCR.DN; an invalid
// operation (0 × ∞, ∞ − ∞, 0 / 0) gives the default NaN.

func (c *CPU) dbits(i uint32) uint64 { return uint64(c.fp.S[i*2]) | uint64(c.fp.S[i*2+1])<<32 }

func (c *CPU) setDBits(i uint32, b uint64) { c.fp.S[i*2], c.fp.S[i*2+1] = uint32(b), uint32(b>>32) }

func (c *CPU) setSBits(i uint32, b uint64) { c.fp.S[i] = uint32(b) }

type fpFmt struct {
	bits     uint
	sign     uint64
	quiet    uint64
	defNaN   uint64
	expMask  uint64
	fracMask uint64
}

var f32 = fpFmt{32, 1 << 31, 1 << 22, 0x7fc00000, 0x7f800000, 0x7fffff}
var f64 = fpFmt{64, 1 << 63, 1 << 51, 0x7ff8000000000000, 0x7ff0000000000000, 1<<52 - 1}

func (f *fpFmt) isNaN(b uint64) bool  { return b&f.expMask == f.expMask && b&f.fracMask != 0 }
func (f *fpFmt) isSNaN(b uint64) bool { return f.isNaN(b) && b&f.quiet == 0 }

// pickNaN: the result when any operand is a NaN.
func (c *CPU) pickNaN(f *fpFmt, ops ...uint64) (uint64, bool) {
	any := false
	for _, o := range ops {
		if f.isNaN(o) {
			any = true
		}
	}
	if !any {
		return 0, false
	}
	if c.fp.FPSCR&fpscrDN != 0 {
		return f.defNaN, true
	}
	for _, o := range ops {
		if f.isSNaN(o) {
			return o | f.quiet, true
		}
	}
	for _, o := range ops {
		if f.isNaN(o) {
			return o, true
		}
	}
	return 0, false
}

func (c *CPU) fin32(b uint64) float32 { return c.flushIn32(math.Float32frombits(uint32(b))) }
func (c *CPU) fin64(b uint64) float64 { return c.flushIn64(math.Float64frombits(b)) }

func (c *CPU) out32(v float32) uint64 {
	if v != v {
		return f32.defNaN
	}
	return uint64(math.Float32bits(c.flushOut32(v)))
}

func (c *CPU) out64(v float64) uint64 {
	if v != v {
		return f64.defNaN
	}
	return math.Float64bits(c.flushOut64(v))
}

// exact computes x op y rounded to nearest, and the sign of the exact
// result minus the rounded one (0 when exact).
func exact(k int, x, y float64) (float64, int) {
	var r, e float64
	switch k {
	case 0, 1:
		if k == 1 {
			y = -y
		}
		r = x + y
		if math.IsInf(r, 0) || math.IsNaN(r) {
			return r, 0
		}
		bb := r - x
		e = (x - (r - bb)) + (y - bb)
	case 2:
		r = x * y
		if math.IsInf(r, 0) || math.IsNaN(r) {
			return r, 0
		}
		e = math.FMA(x, y, -r)
	default:
		r = x / y
		if math.IsInf(r, 0) || math.IsNaN(r) || r == 0 && x == 0 {
			return r, 0
		}
		e = math.FMA(-r, y, x) // x - r*y: same sign as (exact - r) when y > 0
		if y < 0 {
			e = -e
		}
	}
	switch {
	case e > 0:
		return r, 1
	case e < 0:
		return r, -1
	}
	return r, 0
}

// rmode is FPSCR's rounding mode: 0 nearest, 1 towards +∞, 2 towards −∞,
// 3 towards zero.
func (c *CPU) rmode() uint32 { return c.fp.FPSCR >> 22 & 3 }

// round64 adjusts a round-to-nearest result r (with es, the sign of exact
// − r) to the rounding mode.
func round64(r float64, es int, mode uint32) float64 {
	if es == 0 || mode == 0 || math.IsNaN(r) {
		return r
	}
	switch mode {
	case 1:
		if es > 0 {
			return math.Nextafter(r, math.Inf(1))
		}
	case 2:
		if es < 0 {
			return math.Nextafter(r, math.Inf(-1))
		}
	case 3:
		if r > 0 && es < 0 || r == math.Inf(1) {
			return math.Nextafter(r, math.Inf(-1))
		}
		if r < 0 && es > 0 || r == math.Inf(-1) {
			return math.Nextafter(r, math.Inf(1))
		}
	}
	return r
}

// round32 rounds v (with es, the sign of exact − v) to single precision
// in the rounding mode.
func round32(v float64, es int, mode uint32) float32 {
	f := float32(v)
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return f
	}
	d := float64(f)
	sign := es
	if d < v {
		sign = 1
	} else if d > v {
		sign = -1
	}
	if mode == 0 {
		if d != v && !math.IsInf(d, 0) && es != 0 {
			// a tie in v hides the exact value's side
			other := float64(math.Nextafter32(f, float32(math.Copysign(math.Inf(1), v-d))))
			if v-d == other-v || d-v == v-other {
				if (es > 0) == (other > d) {
					return float32(other)
				}
			}
		}
		return f
	}
	up := func() float32 { return math.Nextafter32(f, float32(math.Inf(1))) }
	down := func() float32 { return math.Nextafter32(f, float32(math.Inf(-1))) }
	switch mode {
	case 1:
		if sign > 0 || math.IsInf(d, -1) {
			return up()
		}
	case 2:
		if sign < 0 || math.IsInf(d, 1) {
			return down()
		}
	case 3:
		if f > 0 && (sign < 0 || math.IsInf(d, 1)) {
			return down()
		}
		if f < 0 && (sign > 0 || math.IsInf(d, -1)) {
			return up()
		}
	}
	return f
}

// op32 runs a two-operand operation (0 add, 1 subtract, 2 multiply,
// 3 divide) in single precision.
func (c *CPU) op32(k int, a, b uint64) uint64 {
	if r, ok := c.pickNaN(&f32, a, b); ok {
		return r
	}
	x, y := float64(c.fin32(a)), float64(c.fin32(b))
	v, es := exact(k, x, y)
	if m := c.rmode(); m != 0 {
		es = bigSign(k, x, y, v)
		return c.out32(float32(zeroSign(k, x, y, float64(round32(v, es, m)), m)))
	}
	return c.out32(round32(v, es, 0))
}

func (c *CPU) op64(k int, a, b uint64) uint64 {
	if r, ok := c.pickNaN(&f64, a, b); ok {
		return r
	}
	x, y := c.fin64(a), c.fin64(b)
	v, es := exact(k, x, y)
	if m := c.rmode(); m != 0 {
		es = bigSign(k, x, y, v)
		return c.out64(zeroSign(k, x, y, round64(v, es, m), m))
	}
	return c.out64(v)
}

// sqrtErr: the sign of sqrt(x) − r, worked out exactly.
func sqrtErr(x, r float64) int {
	if math.IsInf(r, 0) || math.IsNaN(r) || r == 0 {
		return 0
	}
	R := new(big.Float).SetPrec(2200).SetFloat64(r)
	X := new(big.Float).SetPrec(2200).SetFloat64(x)
	return X.Cmp(R.Mul(R, R))
}

func (c *CPU) arith32(opc uint32, d, n, m uint32) uint32 {
	return uint32(c.arith(&f32, c.op32, opc, uint64(d), uint64(n), uint64(m)))
}

func (c *CPU) arith64(opc uint32, d, n, m uint64) uint64 {
	return c.arith(&f64, c.op64, opc, d, n, m)
}

// arith: FMAC, FNMAC, FMSC, FNMSC, FMUL, FNMUL, FADD, FSUB, FDIV.
func (c *CPU) arith(f *fpFmt, op func(int, uint64, uint64) uint64, opc uint32, d, n, m uint64) uint64 {
	neg := func(v uint64) uint64 { return v ^ f.sign }
	switch opc {
	case 0: // d + n*m
		return op(0, d, op(2, n, m))
	case 1: // d - n*m
		return op(0, d, neg(op(2, n, m)))
	case 2: // -d + n*m
		return op(0, neg(d), op(2, n, m))
	case 3: // -d - n*m
		return op(0, neg(d), neg(op(2, n, m)))
	case 4:
		return op(2, n, m)
	case 5:
		return neg(op(2, n, m))
	case 6:
		return op(0, n, m)
	case 7:
		return op(1, n, m)
	case 8:
		return op(3, n, m)
	}
	return d // opcodes 9-14 are undefined; left unchanged
}

// bigSign: the sign of (x op y) − r, worked out exactly.
func bigSign(k int, x, y, r float64) int {
	if math.IsNaN(r) || math.IsInf(x, 0) || math.IsInf(y, 0) {
		return 0
	}
	if k == 3 && y == 0 {
		return 0
	}
	R := new(big.Float).SetFloat64(r)
	X := new(big.Float).SetPrec(2200).SetFloat64(x)
	Y := new(big.Float).SetPrec(2200).SetFloat64(y)
	E := new(big.Float).SetPrec(2200)
	switch k {
	case 0:
		E.Add(X, Y)
	case 1:
		E.Sub(X, Y)
	case 2:
		E.Mul(X, Y)
	default:
		// exact − r has the sign of (x − r·y)·sign(y)
		if math.IsInf(r, 0) {
			return -int(math.Copysign(1, r))
		}
		P := new(big.Float).SetPrec(2200).Mul(R, Y)
		d := X.Cmp(P)
		if y < 0 {
			d = -d
		}
		return d
	}
	return E.Cmp(R)
}

// signedZero gives an exactly-zero sum its sign in round-towards-−∞
// mode (−0 unless both operands are +0).
func zeroSign(k int, x, y, v float64, mode uint32) float64 {
	if v != 0 || mode != 2 || k > 1 {
		return v
	}
	if k == 1 {
		y = -y
	}
	if math.Signbit(x) != math.Signbit(y) || x != 0 || y != 0 {
		return math.Copysign(0, -1)
	}
	return v
}

// sqrtSign: sqrtErr, needed only in the directed rounding modes.
func (c *CPU) sqrtSign(x, r float64) int {
	if c.rmode() == 0 {
		return 0
	}
	return sqrtErr(x, r)
}
