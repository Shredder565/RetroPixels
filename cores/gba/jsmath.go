package gba

// Math.pow, Math.atan and Math.atan2 as V8 (the web core's JavaScript
// engine) computes them, so the BIOS calls MidiKey2Freq, ArcTan and ArcTan2
// give the web core's answers to the last bit: Go's math.Pow, Atan and Atan2
// are a different algorithm and differ in the last place for a quarter of the
// inputs. These are fdlibm's __ieee754_pow, atan and __ieee754_atan2, with
// V8's grouping of one division in pow (V8 divides by (t1-2)-(w+z*w) where
// fdlibm subtracts after dividing); math_test.go checks them against Node.
//
// fdlibm: Copyright (C) 1993-2004 by Sun Microsystems, Inc. All rights reserved.
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this software is freely
// granted, provided that this notice is preserved.

import "math"

func fhi(x float64) int32  { return int32(math.Float64bits(x) >> 32) }
func flo(x float64) uint32 { return uint32(math.Float64bits(x)) }
func fsetHi(x float64, h int32) float64 {
	return math.Float64frombits(uint64(uint32(h))<<32 | uint64(flo(x)))
}
func fsetLo(x float64, l uint32) float64 {
	return math.Float64frombits(math.Float64bits(x)&^0xffffffff | uint64(l))
}

var (
	huge = 1.0e300 // variables, so huge*huge overflows at run time as in C
	tiny = 1.0e-300
	bp   = [2]float64{1.0, 1.5}
	dpH  = [2]float64{0.0, 5.84962487220764160156e-01}
	dpL  = [2]float64{0.0, 1.35003920212974897128e-08}
)

const (
	two53 = 9007199254740992.0

	pL1    = 5.99999999999994648725e-01
	pL2    = 4.28571428578550184252e-01
	pL3    = 3.33333329818377432918e-01
	pL4    = 2.72728123808534006489e-01
	pL5    = 2.30660745775561754067e-01
	pL6    = 2.06975017800338417784e-01
	pP1    = 1.66666666666666019037e-01
	pP2    = -2.77777777770155933842e-03
	pP3    = 6.61375632143793436117e-05
	pP4    = -1.65339022054652515390e-06
	pP5    = 4.13813679705723846039e-08
	lg2    = 6.93147180559945286227e-01
	lg2H   = 6.93147182464599609375e-01
	lg2L   = -1.90465429995776804525e-09
	ovt    = 8.0085662595372944372e-17
	cp     = 9.61796693925975554329e-01
	cpH    = 9.61796700954437255859e-01
	cpL    = -7.02846165095275826516e-09
	ivln2  = 1.44269504088896338700e+00
	ivln2H = 1.44269502162933349609e+00
	ivln2L = 1.92596299112661746887e-08
)

// jsPow is Math.pow.
func jsPow(x, y float64) float64 {
	var z, ax, zH, zL, pH, pL float64
	var y1, t1, t2, r, s, t, u, v, w float64
	var i, j, k, yisint, n int32
	hx, lx := fhi(x), flo(x)
	hy, ly := fhi(y), flo(y)
	ix, iy := hx&0x7fffffff, hy&0x7fffffff
	if uint32(iy)|ly == 0 {
		return 1
	}
	if ix > 0x7ff00000 || (ix == 0x7ff00000 && lx != 0) || iy > 0x7ff00000 || (iy == 0x7ff00000 && ly != 0) {
		return x + y
	}
	yisint = 0
	if hx < 0 {
		if iy >= 0x43400000 {
			yisint = 2
		} else if iy >= 0x3ff00000 {
			k = (iy >> 20) - 0x3ff
			if k > 20 {
				jj := ly >> uint(52-k)
				if jj<<uint(52-k) == ly {
					yisint = 2 - int32(jj&1)
				}
			} else if ly == 0 {
				j = iy >> uint(20-k)
				if j<<uint(20-k) == iy {
					yisint = 2 - (j & 1)
				}
			}
		}
	}
	if ly == 0 {
		if iy == 0x7ff00000 {
			if (ix-0x3ff00000)|int32(lx) == 0 {
				return y - y
			} else if ix >= 0x3ff00000 {
				if hy >= 0 {
					return y
				}
				return 0
			} else {
				if hy < 0 {
					return -y
				}
				return 0
			}
		}
		if iy == 0x3ff00000 {
			if hy < 0 {
				return 1 / x
			}
			return x
		}
		if hy == 0x40000000 {
			return x * x
		}
		if hy == 0x3fe00000 && hx >= 0 {
			return math.Sqrt(x)
		}
	}
	ax = math.Abs(x)
	if lx == 0 {
		if ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000 {
			z = ax
			if hy < 0 {
				z = 1 / z
			}
			if hx < 0 {
				if (ix-0x3ff00000)|yisint == 0 {
					z = (z - z) / (z - z)
				} else if yisint == 1 {
					z = -z
				}
			}
			return z
		}
	}
	n = (hx >> 31) + 1
	if n|yisint == 0 {
		return (x - x) / (x - x)
	}
	s = 1
	if n|(yisint-1) == 0 {
		s = -1
	}
	if iy > 0x41e00000 {
		if iy > 0x43f00000 {
			if ix <= 0x3fefffff {
				if hy < 0 {
					return huge * huge
				}
				return tiny * tiny
			}
			if ix >= 0x3ff00000 {
				if hy > 0 {
					return huge * huge
				}
				return tiny * tiny
			}
		}
		if ix < 0x3fefffff {
			if hy < 0 {
				return s * huge * huge
			}
			return s * tiny * tiny
		}
		if ix > 0x3ff00000 {
			if hy > 0 {
				return s * huge * huge
			}
			return s * tiny * tiny
		}
		t = ax - 1
		w = (t * t) * (0.5 - t*(0.3333333333333333333333-t*0.25))
		u = ivln2H * t
		v = t*ivln2L - w*ivln2
		t1 = fsetLo(u+v, 0)
		t2 = v - (t1 - u)
	} else {
		var ss, s2, sH, sL, tH, tL float64
		n = 0
		if ix < 0x00100000 {
			ax *= two53
			n -= 53
			ix = fhi(ax)
		}
		n += (ix >> 20) - 0x3ff
		j = ix & 0x000fffff
		ix = j | 0x3ff00000
		if j <= 0x3988E {
			k = 0
		} else if j < 0xBB67A {
			k = 1
		} else {
			k = 0
			n++
			ix -= 0x00100000
		}
		ax = fsetHi(ax, ix)
		u = ax - bp[k]
		v = 1 / (ax + bp[k])
		ss = u * v
		sH = fsetLo(ss, 0)
		tH = fsetHi(0, ((ix>>1)|0x20000000)+0x00080000+(k<<18))
		tL = ax - (tH - bp[k])
		sL = v * ((u - sH*tH) - sH*tL)
		s2 = ss * ss
		r = s2 * s2 * (pL1 + s2*(pL2+s2*(pL3+s2*(pL4+s2*(pL5+s2*pL6)))))
		r += sL * (sH + ss)
		s2 = sH * sH
		tH = fsetLo(3.0+s2+r, 0)
		tL = r - ((tH - 3.0) - s2)
		u = sH * tH
		v = sL*tH + tL*ss
		pH = fsetLo(u+v, 0)
		pL = v - (pH - u)
		zH = cpH * pH
		zL = cpL*pH + pL*cp + dpL[k]
		t = float64(n)
		t1 = fsetLo(((zH+zL)+dpH[k])+t, 0)
		t2 = zL - (((t1 - t) - dpH[k]) - zH)
	}
	y1 = fsetLo(y, 0)
	pL = (y-y1)*t1 + y*t2
	pH = y1 * t1
	z = pL + pH
	j = fhi(z)
	i = int32(flo(z))
	if j >= 0x40900000 {
		if (j-0x40900000)|i != 0 {
			return s * huge * huge
		}
		if pL+ovt > z-pH {
			return s * huge * huge
		}
	} else if j&0x7fffffff >= 0x4090cc00 {
		if (j-int32(-0x3f6f3400))|i != 0 { // 0xc090cc00
			return s * tiny * tiny
		}
		if pL <= z-pH {
			return s * tiny * tiny
		}
	}
	i = j & 0x7fffffff
	k = (i >> 20) - 0x3ff
	n = 0
	if i > 0x3fe00000 {
		n = j + (0x00100000 >> uint(k+1))
		k = ((n & 0x7fffffff) >> 20) - 0x3ff
		t = fsetHi(0, n&^(0x000fffff>>uint(k)))
		n = ((n & 0x000fffff) | 0x00100000) >> uint(20-k)
		if j < 0 {
			n = -n
		}
		pH -= t
	}
	t = fsetLo(pL+pH, 0)
	u = t * lg2H
	v = (pL-(t-pH))*lg2 + t*lg2L
	z = u + v
	w = v - (z - u)
	t = z * z
	t1 = z - t*(pP1+t*(pP2+t*(pP3+t*(pP4+t*pP5))))
	r = (z * t1) / ((t1 - 2) - (w + z*w))
	z = 1 - (r - z)
	j = fhi(z)
	j += n << 20
	if j>>20 <= 0 {
		z = math.Ldexp(z, int(n))
	} else {
		z = fsetHi(z, fhi(z)+(n<<20))
	}
	return s * z
}

var atanhi = [4]float64{4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00}
var atanlo = [4]float64{2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17}
var aT = [11]float64{
	3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01, -1.11111104054623557880e-01,
	9.09088713343650656196e-02, -7.69187620504482999495e-02, 6.66107313738753120669e-02, -5.83357013379057348645e-02,
	4.97687799461593236017e-02, -3.65315727442169155270e-02, 1.62858201153657823623e-02,
}

// jsAtan is Math.atan.
func jsAtan(x float64) float64 {
	hx := int32(math.Float64bits(x) >> 32)
	ix := hx & 0x7fffffff
	var id int
	if ix >= 0x44100000 {
		if math.IsNaN(x) {
			return x + x
		}
		if hx > 0 {
			return atanhi[3] + atanlo[3]
		}
		return -atanhi[3] - atanlo[3]
	}
	if ix < 0x3fdc0000 {
		if ix < 0x3e200000 {
			return x
		}
		id = -1
	} else {
		x = math.Abs(x)
		if ix < 0x3ff30000 {
			if ix < 0x3fe60000 {
				id = 0
				x = (2.0*x - 1) / (2.0 + x)
			} else {
				id = 1
				x = (x - 1) / (x + 1)
			}
		} else {
			if ix < 0x40038000 {
				id = 2
				x = (x - 1.5) / (1 + 1.5*x)
			} else {
				id = 3
				x = -1.0 / x
			}
		}
	}
	z := x * x
	w := z * z
	s1 := z * (aT[0] + w*(aT[2]+w*(aT[4]+w*(aT[6]+w*(aT[8]+w*aT[10])))))
	s2 := w * (aT[1] + w*(aT[3]+w*(aT[5]+w*(aT[7]+w*aT[9]))))
	if id < 0 {
		return x - x*(s1+s2)
	}
	z = atanhi[id] - ((x*(s1+s2) - atanlo[id]) - x)
	if hx < 0 {
		return -z
	}
	return z
}

const (
	piO4 = 7.8539816339744827900e-01
	piO2 = 1.5707963267948965580e+00
	piD  = 3.1415926535897931160e+00
	piLo = 1.2246467991473531772e-16
)

// jsAtan2 is Math.atan2.
func jsAtan2(y, x float64) float64 {
	if math.IsNaN(x) || math.IsNaN(y) {
		return x + y
	}
	bx, by := math.Float64bits(x), math.Float64bits(y)
	hx, hy := int32(bx>>32), int32(by>>32)
	lx, ly := uint32(bx), uint32(by)
	ix, iy := hx&0x7fffffff, hy&0x7fffffff
	if (hx-0x3ff00000)|int32(lx) == 0 {
		return jsAtan(y)
	}
	m := (hy>>31)&1 | (hx>>30)&2
	if uint32(iy)|ly == 0 {
		switch m {
		case 0, 1:
			return y
		case 2:
			return piD + tiny
		default:
			return -piD - tiny
		}
	}
	if uint32(ix)|lx == 0 {
		if hy < 0 {
			return -piO2 - tiny
		}
		return piO2 + tiny
	}
	if ix == 0x7ff00000 {
		if iy == 0x7ff00000 {
			switch m {
			case 0:
				return piO4 + tiny
			case 1:
				return -piO4 - tiny
			case 2:
				return 3.0*piO4 + tiny
			default:
				return -3.0*piO4 - tiny
			}
		}
		switch m {
		case 0:
			return 0
		case 1:
			return math.Copysign(0, -1)
		case 2:
			return piD + tiny
		default:
			return -piD - tiny
		}
	}
	if iy == 0x7ff00000 {
		if hy < 0 {
			return -piO2 - tiny
		}
		return piO2 + tiny
	}
	k := (iy - ix) >> 20
	var z float64
	switch {
	case k > 60:
		z = piO2 + 0.5*piLo
		m &= 1
	case hx < 0 && k < -60:
		z = 0.0
	default:
		z = jsAtan(math.Abs(y / x))
	}
	switch m {
	case 0:
		return z
	case 1:
		return -z
	case 2:
		return piD - (z - piLo)
	}
	return (z - piLo) - piD
}
