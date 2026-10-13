package n64

import "math"

// COP1: the VR4300's floating-point unit.

func (c *CPU) fr() bool { return c.cop0[c0Status]&statusFR != 0 }

func (c *CPU) getFPR32(n uint32) uint32 {
	if c.fr() {
		return uint32(c.fpr[n])
	}
	if n&1 != 0 {
		return uint32(c.fpr[n&^1] >> 32)
	}
	return uint32(c.fpr[n])
}

func (c *CPU) setFPR32(n uint32, v uint32) {
	if c.fr() {
		c.fpr[n] = (c.fpr[n] &^ 0xffffffff) | uint64(v)
		return
	}
	if n&1 != 0 {
		e := n &^ 1
		c.fpr[e] = (c.fpr[e] & 0xffffffff) | uint64(v)<<32
		return
	}
	c.fpr[n] = (c.fpr[n] &^ 0xffffffff) | uint64(v)
}

func (c *CPU) getFPR64(n uint32) uint64 {
	if c.fr() {
		return c.fpr[n]
	}
	return c.fpr[n&^1]
}

func (c *CPU) setFPR64(n uint32, v uint64) {
	if c.fr() {
		c.fpr[n] = v
		return
	}
	c.fpr[n&^1] = v
}

func (c *CPU) fS(n uint32) float32      { return math.Float32frombits(c.getFPR32(n)) }
func (c *CPU) fD(n uint32) float64      { return math.Float64frombits(c.getFPR64(n)) }
func (c *CPU) setS(n uint32, v float32) { c.setFPR32(n, math.Float32bits(v)) }
func (c *CPU) setD(n uint32, v float64) { c.setFPR64(n, math.Float64bits(v)) }

// FCR31 bits
const (
	fcrI    = 1 << 0 // inexact (shifted by 2 for flags, 7 for enables, 12 for cause)
	fcrU    = 1 << 1 // underflow
	fcrO    = 1 << 2 // overflow
	fcrZ    = 1 << 3 // divide by zero
	fcrV    = 1 << 4 // invalid operation
	fcrE    = 1 << 5 // unimplemented operation (cause only)
	fcrCond = 1 << 23
	fcrFS   = 1 << 24 // flush denormal results to zero
)

// MIPS default NaNs (the quiet bit clear means quiet on this CPU)
const (
	nan32 = 0x7fbfffff
	nan64 = 0x7ff7ffffffffffff
)

func isSNaN32(b uint32) bool { return b&0x7f800000 == 0x7f800000 && b&0x400000 != 0 }
func isSNaN64(b uint64) bool {
	return b&0x7ff0000000000000 == 0x7ff0000000000000 && b&0x8000000000000 != 0
}

// fpBegin clears the cause field for a new FPU operation.
func (c *CPU) fpBegin() { c.fcr31 &^= 0x3f << 12 }

// fpEnd records the exceptions an operation raised. It returns false (and
// enters the exception handler) when one of them is enabled, in which case
// the result must not be written.
func (c *CPU) fpEnd(cause uint32) bool {
	if cause == 0 {
		return true
	}
	c.fcr31 |= cause << 12
	if cause&(c.fcr31>>7&0x1f|fcrE) != 0 {
		c.exception(excFPE, 0x180)
		return false
	}
	c.fcr31 |= (cause & 0x1f) << 2
	return true
}

// round applies the current rounding mode.
func (c *CPU) round(v float64) float64 {
	switch c.fcr31 & 3 {
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

// fix64 finishes a double result r whose exact value differs from it by an
// amount with the sign of err (0 when exact): it applies directed rounding,
// overflow and underflow, and returns the result and the exceptions raised.
func (c *CPU) fix64(r float64, err float64, finiteIn bool) (float64, uint32) {
	var cause uint32
	if math.IsNaN(r) {
		return math.Float64frombits(nan64), fcrV
	}
	mode := c.fcr31 & 3
	if math.IsInf(r, 0) {
		if !finiteIn {
			return r, 0
		}
		cause = fcrO | fcrI
		if (mode == 1) || (mode == 2 && r < 0) || (mode == 3 && r > 0) {
			r = math.Copysign(math.MaxFloat64, r)
		}
		return r, cause
	}
	if err != 0 {
		cause |= fcrI
		switch mode {
		case 1:
			if (err < 0) != (r < 0) && r != 0 {
				r = math.Nextafter(r, 0)
			}
		case 2:
			if err > 0 {
				r = math.Nextafter(r, math.Inf(1))
			}
		case 3:
			if err < 0 {
				r = math.Nextafter(r, math.Inf(-1))
			}
		}
	}
	if r != 0 && math.Abs(r) < 0x1p-1022 {
		cause |= fcrU | fcrI
		if c.fcr31&fcrFS != 0 {
			r = math.Copysign(0, r)
		}
	}
	return r, cause
}

// fix32 is fix64 for single results; x is the (nearly) exact value in double.
func (c *CPU) fix32(x float64, finiteIn bool) (float32, uint32) {
	var cause uint32
	r := float32(x)
	if x != x {
		return math.Float32frombits(nan32), fcrV
	}
	mode := c.fcr31 & 3
	if math.IsInf(float64(r), 0) {
		if !finiteIn {
			return r, 0
		}
		cause = fcrO | fcrI
		if (mode == 1) || (mode == 2 && r < 0) || (mode == 3 && r > 0) {
			r = float32(math.Copysign(math.MaxFloat32, float64(r)))
		}
		return r, cause
	}
	err := x - float64(r)
	if err != 0 {
		cause |= fcrI
		switch mode {
		case 1:
			if (err < 0) != (r < 0) && r != 0 {
				r = math.Nextafter32(r, 0)
			}
		case 2:
			if err > 0 {
				r = math.Nextafter32(r, float32(math.Inf(1)))
			}
		case 3:
			if err < 0 {
				r = math.Nextafter32(r, float32(math.Inf(-1)))
			}
		}
	}
	if r != 0 && math.Abs(float64(r)) < 0x1p-126 {
		cause |= fcrU | fcrI
		if c.fcr31&fcrFS != 0 {
			r = float32(math.Copysign(0, float64(r)))
		}
	}
	return r, cause
}

func finite(a, b float64) bool {
	return math.Float64bits(a)>>52&0x7ff != 0x7ff && math.Float64bits(b)>>52&0x7ff != 0x7ff
}

// adjust64 applies a directed rounding mode to a normal result r whose exact
// value differs from it with the sign of err.
func adjust64(r, err float64, mode uint32) float64 {
	switch mode {
	case 1:
		if (err < 0) != (r < 0) {
			return math.Nextafter(r, 0)
		}
	case 2:
		if err > 0 {
			return math.Nextafter(r, math.Inf(1))
		}
	case 3:
		if err < 0 {
			return math.Nextafter(r, math.Inf(-1))
		}
	}
	return r
}

// arith32 runs a single-precision operation.
func (c *CPU) arith32(fn uint32, fs, ft, fd uint32) {
	ab, bb := c.getFPR32(fs), c.getFPR32(ft)
	a, b := float64(math.Float32frombits(ab)), float64(math.Float32frombits(bb))
	var cause uint32
	if isSNaN32(ab) || (fn < 4 && isSNaN32(bb)) {
		cause |= fcrV
	}
	var x float64
	switch fn {
	case 0:
		x = a + b
	case 1:
		x = a - b
	case 2:
		x = a * b
	case 3:
		if b == 0 && a == a && a != 0 && !math.IsInf(a, 0) {
			cause |= fcrZ
		}
		x = a / b
	case 4:
		x = math.Sqrt(a)
	}
	if e := math.Float32bits(float32(x)) >> 23 & 0xff; e != 0 && e != 0xff && c.fcr31&3 == 0 {
		// the common case: a normal result in round-to-nearest
		r := float32(x)
		if x != float64(r) {
			cause |= fcrI
		}
		if cause == 0 || c.fpEnd(cause) {
			c.setFPR32(fd, math.Float32bits(r))
		}
		return
	}
	r, cs := c.fix32(x, (finite(a, b) || fn == 4 && finite(a, 0)) && cause&fcrZ == 0)
	if r != r && (a != a || (fn < 4 && b != b)) {
		cs &^= fcrV // a quiet NaN operand gives a NaN without complaint
	}
	if c.fpEnd(cause | cs) {
		c.setFPR32(fd, math.Float32bits(r))
	}
}

// arith64 runs a double-precision operation.
func (c *CPU) arith64(fn uint32, fs, ft, fd uint32) {
	ab, bb := c.getFPR64(fs), c.getFPR64(ft)
	a, b := math.Float64frombits(ab), math.Float64frombits(bb)
	var cause uint32
	if isSNaN64(ab) || (fn < 4 && isSNaN64(bb)) {
		cause |= fcrV
	}
	var r, err float64
	switch fn {
	case 0, 1:
		if fn == 1 {
			b = -b
		}
		r = a + b
		bv := r - a
		err = (a - (r - bv)) + (b - bv)
	case 2:
		r = a * b
		err = math.FMA(a, b, -r)
	case 3:
		if b == 0 && a == a && a != 0 && !math.IsInf(a, 0) {
			cause |= fcrZ
		}
		r = a / b
		if b != 0 && r != 0 {
			err = math.FMA(-r, b, a)
			if b < 0 {
				err = -err
			}
		}
	case 4:
		r = math.Sqrt(a)
		err = math.FMA(-r, r, a)
	}
	if e := math.Float64bits(r) >> 52 & 0x7ff; e != 0 && e != 0x7ff {
		// the common case: a normal result
		if err != 0 {
			cause |= fcrI
			if mode := c.fcr31 & 3; mode != 0 {
				r = adjust64(r, err, mode)
			}
		}
		if cause == 0 || c.fpEnd(cause) {
			c.setFPR64(fd, math.Float64bits(r))
		}
		return
	}
	r, cs := c.fix64(r, err, (finite(a, b) || fn == 4 && finite(a, 0)) && cause&fcrZ == 0)
	if math.IsNaN(r) && (a != a || (fn < 4 && b != b)) {
		cs &^= fcrV
	}
	if c.fpEnd(cause | cs) {
		c.setFPR64(fd, math.Float64bits(r))
	}
}

// toInt converts with the given rounding for CVT/ROUND/TRUNC/CEIL/FLOOR;
// values that don't fit raise the unimplemented-operation cause.
func (c *CPU) toInt(v float64, rv float64, long bool) (uint64, bool) {
	var cause uint32
	if rv != v {
		cause |= fcrI
	}
	var out uint64
	if long {
		if v != v || rv >= 0x1p63 || rv < -0x1p63 {
			c.fcr31 |= fcrE << 12
			return 0x7fffffffffffffff, true
		}
		out = uint64(int64(rv))
	} else {
		if v != v || rv >= 0x1p31 || rv < -0x1p31 {
			c.fcr31 |= fcrE << 12
			return 0x7fffffff, true
		}
		out = uint64(uint32(int32(rv)))
	}
	return out, c.fpEnd(cause)
}

func (c *CPU) cop1op(op uint32) {
	fmt := (op >> 21) & 31
	ft := (op >> 16) & 31
	fs := (op >> 11) & 31
	fd := (op >> 6) & 31
	switch fmt {
	case 0x00: // MFC1
		c.gpr[ft] = sx32(c.getFPR32(fs))
		return
	case 0x01: // DMFC1
		c.gpr[ft] = c.getFPR64(fs)
		return
	case 0x02: // CFC1
		switch fs {
		case 0:
			c.gpr[ft] = 0xa00
		case 31:
			c.gpr[ft] = sx32(c.fcr31)
		default:
			c.gpr[ft] = 0
		}
		return
	case 0x04:
		c.setFPR32(fs, uint32(c.gpr[ft]))
		return
	case 0x05:
		c.setFPR64(fs, c.gpr[ft])
		return
	case 0x06: // CTC1
		if fs == 31 {
			c.fcr31 = uint32(c.gpr[ft]) & 0x0183ffff
			// a cause bit that is enabled traps right away
			if (c.fcr31>>12)&(c.fcr31>>7&0x1f|fcrE) != 0 {
				c.exception(excFPE, 0x180)
			}
		}
		return
	case 0x08: // BC1
		cond := c.fcr31&fcrCond != 0
		target := c.curPC + 4 + uint64(int64(int16(op)))<<2
		switch ft & 3 {
		case 0:
			c.branch(!cond, target)
		case 1:
			c.branch(cond, target)
		case 2:
			c.branchLikely(!cond, target)
		default:
			c.branchLikely(cond, target)
		}
		return
	}
	fn := op & 63
	switch fmt {
	case 0x10: // single
		if fn <= 4 {
			c.fpBegin()
			c.arith32(fn, fs, ft, fd)
			return
		}
		a := c.fS(fs)
		switch fn {
		case 0x05:
			c.fpBegin()
			if c.fpEnd(c.nanCause32(c.getFPR32(fs))) {
				c.setFPR32(fd, c.getFPR32(fs)&0x7fffffff)
			}
		case 0x06:
			c.setFPR32(fd, c.getFPR32(fs))
		case 0x07:
			c.fpBegin()
			if c.fpEnd(c.nanCause32(c.getFPR32(fs))) {
				c.setFPR32(fd, c.getFPR32(fs)^0x80000000)
			}
		case 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x24, 0x25:
			c.cvtInt(fn, float64(a), fd)
		case 0x21:
			c.fpBegin()
			b := c.getFPR32(fs)
			v := float64(a)
			if v != v {
				v = math.Float64frombits(nan64)
			}
			if c.fpEnd(c.nanCause32(b)) {
				c.setD(fd, v)
			}
		default:
			if fn >= 0x30 {
				c.compare(fn, float64(a), float64(c.fS(ft)), isSNaN32(c.getFPR32(fs)) || isSNaN32(c.getFPR32(ft)))
			} else {
				c.fpBegin()
				c.fpEnd(fcrE)
			}
		}
	case 0x11: // double
		if fn <= 4 {
			c.fpBegin()
			c.arith64(fn, fs, ft, fd)
			return
		}
		a := c.fD(fs)
		switch fn {
		case 0x05:
			c.fpBegin()
			if c.fpEnd(c.nanCause64(c.getFPR64(fs))) {
				c.setFPR64(fd, c.getFPR64(fs)&^(1<<63))
			}
		case 0x06:
			c.setFPR64(fd, c.getFPR64(fs))
		case 0x07:
			c.fpBegin()
			if c.fpEnd(c.nanCause64(c.getFPR64(fs))) {
				c.setFPR64(fd, c.getFPR64(fs)^(1<<63))
			}
		case 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x24, 0x25:
			c.cvtInt(fn, a, fd)
		case 0x20:
			c.fpBegin()
			r, cause := c.fix32(a, finite(a, 0))
			if a != a {
				cause = c.nanCause64(c.getFPR64(fs))
			}
			if c.fpEnd(cause) {
				c.setS(fd, r)
			}
		default:
			if fn >= 0x30 {
				c.compare(fn, a, c.fD(ft), isSNaN64(c.getFPR64(fs)) || isSNaN64(c.getFPR64(ft)))
			} else {
				c.fpBegin()
				c.fpEnd(fcrE)
			}
		}
	case 0x14: // word
		v := int32(c.getFPR32(fs))
		c.fpBegin()
		switch fn {
		case 0x20:
			r, cause := c.fix32(float64(v), true)
			if c.fpEnd(cause) {
				c.setS(fd, r)
			}
		case 0x21:
			c.setD(fd, float64(v))
		default:
			c.fpEnd(fcrE)
		}
	case 0x15: // long
		v := int64(c.getFPR64(fs))
		c.fpBegin()
		switch fn {
		case 0x20:
			// exact in two steps: the high part is exact in double
			hi := float64(v &^ 0x7ff)
			lo := float64(v & 0x7ff)
			r, cause := c.fix32(hi+lo, true)
			if c.fpEnd(cause) {
				c.setS(fd, r)
			}
		case 0x21:
			r := float64(v)
			err := float64(v - int64(r)) // exact unless r is out of int64 range
			if r >= 0x1p63 {
				err = -1
			}
			r, cause := c.fix64(r, err, true)
			if c.fpEnd(cause) {
				c.setD(fd, r)
			}
		default:
			c.fpEnd(fcrE)
		}
	default:
		c.fpBegin()
		c.fpEnd(fcrE)
	}
}

func (c *CPU) nanCause32(b uint32) uint32 {
	if isSNaN32(b) {
		return fcrV
	}
	return 0
}

func (c *CPU) nanCause64(b uint64) uint32 {
	if isSNaN64(b) {
		return fcrV
	}
	return 0
}

// cvtInt handles the float to integer conversions.
func (c *CPU) cvtInt(fn uint32, v float64, fd uint32) {
	c.fpBegin()
	var rv float64
	switch fn & 3 {
	case 0:
		rv = math.RoundToEven(v)
	case 1:
		rv = math.Trunc(v)
	case 2:
		rv = math.Ceil(v)
	case 3:
		rv = math.Floor(v)
	}
	if fn >= 0x24 {
		rv = c.round(v)
	}
	long := fn == 0x25 || (fn >= 8 && fn <= 0x0b)
	out, ok := c.toInt(v, rv, long)
	if !ok {
		return
	}
	if long {
		c.setFPR64(fd, out)
	} else {
		c.setFPR32(fd, uint32(out))
	}
}

func (c *CPU) compare(fn uint32, a, b float64, snan bool) {
	cond := fn & 15
	unordered := math.IsNaN(a) || math.IsNaN(b)
	res := false
	if unordered {
		res = cond&1 != 0
	} else {
		if cond&2 != 0 && a == b {
			res = true
		}
		if cond&4 != 0 && a < b {
			res = true
		}
	}
	c.fpBegin()
	// the signaling compares (and any compare of a signaling NaN) flag
	// unordered operands as invalid
	if unordered && (cond&8 != 0 || snan) {
		if !c.fpEnd(fcrV) {
			return
		}
	}
	if res {
		c.fcr31 |= fcrCond
	} else {
		c.fcr31 &^= fcrCond
	}
}
