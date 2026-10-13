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

func (c *CPU) fS(n uint32) float32 { return math.Float32frombits(c.getFPR32(n)) }
func (c *CPU) fD(n uint32) float64 { return math.Float64frombits(c.getFPR64(n)) }
func (c *CPU) setS(n uint32, v float32) { c.setFPR32(n, math.Float32bits(v)) }
func (c *CPU) setD(n uint32, v float64) { c.setFPR64(n, math.Float64bits(v)) }

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

func toW(v float64) uint32 {
	if math.IsNaN(v) || v >= 2147483648 || v < -2147483648 {
		return 0x7fffffff
	}
	return uint32(int32(v))
}
func toL(v float64) uint64 {
	if math.IsNaN(v) || v >= 9.223372036854775807e18 || v < -9.223372036854775807e18 {
		return 0x7fffffffffffffff
	}
	return uint64(int64(v))
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
		}
		return
	case 0x08: // BC1
		cond := c.fcr31&(1<<23) != 0
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
		a, b := c.fS(fs), c.fS(ft)
		switch fn {
		case 0x00:
			c.setS(fd, a+b)
		case 0x01:
			c.setS(fd, a-b)
		case 0x02:
			c.setS(fd, a*b)
		case 0x03:
			c.setS(fd, a/b)
		case 0x04:
			c.setS(fd, float32(math.Sqrt(float64(a))))
		case 0x05:
			c.setS(fd, float32(math.Abs(float64(a))))
		case 0x06:
			c.setFPR32(fd, c.getFPR32(fs))
		case 0x07:
			c.setS(fd, -a)
		case 0x08:
			c.setFPR64(fd, toL(math.RoundToEven(float64(a))))
		case 0x09:
			c.setFPR64(fd, toL(math.Trunc(float64(a))))
		case 0x0a:
			c.setFPR64(fd, toL(math.Ceil(float64(a))))
		case 0x0b:
			c.setFPR64(fd, toL(math.Floor(float64(a))))
		case 0x0c:
			c.setFPR32(fd, toW(math.RoundToEven(float64(a))))
		case 0x0d:
			c.setFPR32(fd, toW(math.Trunc(float64(a))))
		case 0x0e:
			c.setFPR32(fd, toW(math.Ceil(float64(a))))
		case 0x0f:
			c.setFPR32(fd, toW(math.Floor(float64(a))))
		case 0x21:
			c.setD(fd, float64(a))
		case 0x24:
			c.setFPR32(fd, toW(c.round(float64(a))))
		case 0x25:
			c.setFPR64(fd, toL(c.round(float64(a))))
		default:
			if fn >= 0x30 {
				c.compare(fn, float64(a), float64(b))
			}
		}
	case 0x11: // double
		a, b := c.fD(fs), c.fD(ft)
		switch fn {
		case 0x00:
			c.setD(fd, a+b)
		case 0x01:
			c.setD(fd, a-b)
		case 0x02:
			c.setD(fd, a*b)
		case 0x03:
			c.setD(fd, a/b)
		case 0x04:
			c.setD(fd, math.Sqrt(a))
		case 0x05:
			c.setD(fd, math.Abs(a))
		case 0x06:
			c.setFPR64(fd, c.getFPR64(fs))
		case 0x07:
			c.setD(fd, -a)
		case 0x08:
			c.setFPR64(fd, toL(math.RoundToEven(a)))
		case 0x09:
			c.setFPR64(fd, toL(math.Trunc(a)))
		case 0x0a:
			c.setFPR64(fd, toL(math.Ceil(a)))
		case 0x0b:
			c.setFPR64(fd, toL(math.Floor(a)))
		case 0x0c:
			c.setFPR32(fd, toW(math.RoundToEven(a)))
		case 0x0d:
			c.setFPR32(fd, toW(math.Trunc(a)))
		case 0x0e:
			c.setFPR32(fd, toW(math.Ceil(a)))
		case 0x0f:
			c.setFPR32(fd, toW(math.Floor(a)))
		case 0x20:
			c.setS(fd, c.roundS(a))
		case 0x24:
			c.setFPR32(fd, toW(c.round(a)))
		case 0x25:
			c.setFPR64(fd, toL(c.round(a)))
		default:
			if fn >= 0x30 {
				c.compare(fn, a, b)
			}
		}
	case 0x14: // word
		v := int32(c.getFPR32(fs))
		switch fn {
		case 0x20:
			c.setS(fd, float32(v))
		case 0x21:
			c.setD(fd, float64(v))
		}
	case 0x15: // long
		v := int64(c.getFPR64(fs))
		switch fn {
		case 0x20:
			c.setS(fd, float32(v))
		case 0x21:
			c.setD(fd, float64(v))
		}
	}
}

// roundS converts a double to single with the current rounding mode.
func (c *CPU) roundS(a float64) float32 {
	f := float32(a)
	mode := c.fcr31 & 3
	if mode == 0 || math.IsNaN(a) || math.IsInf(a, 0) {
		return f
	}
	back := float64(f)
	if back == a {
		return f
	}
	switch mode {
	case 1: // toward zero
		if math.Abs(back) > math.Abs(a) {
			return math.Nextafter32(f, 0)
		}
	case 2: // up
		if back < a {
			return math.Nextafter32(f, float32(math.Inf(1)))
		}
	case 3: // down
		if back > a {
			return math.Nextafter32(f, float32(math.Inf(-1)))
		}
	}
	return f
}

func (c *CPU) compare(fn uint32, a, b float64) {
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
	if res {
		c.fcr31 |= 1 << 23
	} else {
		c.fcr31 &^= 1 << 23
	}
}
