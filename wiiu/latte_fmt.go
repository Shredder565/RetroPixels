package wiiu

// Latte pixel formats: reading and writing one element of a colour buffer,
// texture or vertex attribute.
//
// The GPU stores elements little-endian, with the first component (X, which
// is usually red) in the lowest bits. Values travel through the shaders as
// 32-bit words: floats for normalized and float formats, plain integers for
// the integer ones.

import "math"

// number kinds
type numKind uint8

const (
	kUnorm numKind = iota
	kSnorm
	kUscaled
	kSscaled
	kUint
	kSint
	kSrgb
	kFloat
)

// CB_COLOR*_INFO.NUMBER_TYPE is the same list
func numKindFromCB(t uint32) numKind { return numKind(t & 7) }

// numKindFromFetch: the texture and vertex fetch fields (NUM_FORMAT: 0 norm,
// 1 int, 2 scaled; signed; degamma)
func numKindFromFetch(numFormat uint32, signed, degamma bool) numKind {
	switch numFormat {
	case 1:
		if signed {
			return kSint
		}
		return kUint
	case 2:
		if signed {
			return kSscaled
		}
		return kUscaled
	}
	if signed {
		return kSnorm
	}
	if degamma {
		return kSrgb
	}
	return kUnorm
}

type compLayout struct{ shift, n uint8 }

type fmtDesc struct {
	bytes uint32 // element size
	nc    int    // components
	comp  [4]compLayout
	float bool // float components (16 and 32 bits)
	small bool // 11/11/10-bit floats
}

func cl(s, n uint8) compLayout { return compLayout{s, n} }

var fmtDescs = func() [64]fmtDesc {
	var t [64]fmtDesc
	t[fmt8] = fmtDesc{bytes: 1, nc: 1, comp: [4]compLayout{cl(0, 8)}}
	t[fmt4_4] = fmtDesc{bytes: 1, nc: 2, comp: [4]compLayout{cl(0, 4), cl(4, 4)}}
	t[fmt3_3_2] = fmtDesc{bytes: 1, nc: 3, comp: [4]compLayout{cl(0, 2), cl(2, 3), cl(5, 3)}}
	t[fmt16] = fmtDesc{bytes: 2, nc: 1, comp: [4]compLayout{cl(0, 16)}}
	t[fmt16F] = fmtDesc{bytes: 2, nc: 1, comp: [4]compLayout{cl(0, 16)}, float: true}
	t[fmt8_8] = fmtDesc{bytes: 2, nc: 2, comp: [4]compLayout{cl(0, 8), cl(8, 8)}}
	t[fmt5_6_5] = fmtDesc{bytes: 2, nc: 3, comp: [4]compLayout{cl(0, 5), cl(5, 6), cl(11, 5)}}
	t[fmt6_5_5] = fmtDesc{bytes: 2, nc: 3, comp: [4]compLayout{cl(0, 5), cl(5, 5), cl(10, 6)}}
	t[fmt1_5_5_5] = fmtDesc{bytes: 2, nc: 4, comp: [4]compLayout{cl(0, 5), cl(5, 5), cl(10, 5), cl(15, 1)}}
	t[fmt4_4_4_4] = fmtDesc{bytes: 2, nc: 4, comp: [4]compLayout{cl(0, 4), cl(4, 4), cl(8, 4), cl(12, 4)}}
	t[fmt5_5_5_1] = fmtDesc{bytes: 2, nc: 4, comp: [4]compLayout{cl(11, 5), cl(6, 5), cl(1, 5), cl(0, 1)}}
	t[fmt32] = fmtDesc{bytes: 4, nc: 1, comp: [4]compLayout{cl(0, 32)}}
	t[fmt32F] = fmtDesc{bytes: 4, nc: 1, comp: [4]compLayout{cl(0, 32)}, float: true}
	t[fmt16_16] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 16), cl(16, 16)}}
	t[fmt16_16F] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 16), cl(16, 16)}, float: true}
	t[fmt8_24] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 24), cl(24, 8)}}
	t[fmt8_24F] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 24), cl(24, 8)}}
	t[fmt24_8] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 8), cl(8, 24)}}
	t[fmt24_8F] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 8), cl(8, 24)}}
	t[fmt10_11_11] = fmtDesc{bytes: 4, nc: 3, comp: [4]compLayout{cl(0, 11), cl(11, 11), cl(22, 10)}}
	t[fmt10_11_11F] = fmtDesc{bytes: 4, nc: 3, comp: [4]compLayout{cl(0, 11), cl(11, 11), cl(22, 10)}, small: true}
	t[fmt11_11_10] = fmtDesc{bytes: 4, nc: 3, comp: [4]compLayout{cl(0, 10), cl(10, 11), cl(21, 11)}}
	t[fmt11_11_10F] = fmtDesc{bytes: 4, nc: 3, comp: [4]compLayout{cl(0, 10), cl(10, 11), cl(21, 11)}, small: true}
	t[fmt2_10_10_10] = fmtDesc{bytes: 4, nc: 4, comp: [4]compLayout{cl(0, 10), cl(10, 10), cl(20, 10), cl(30, 2)}}
	t[fmt8_8_8_8] = fmtDesc{bytes: 4, nc: 4, comp: [4]compLayout{cl(0, 8), cl(8, 8), cl(16, 8), cl(24, 8)}}
	t[fmt10_10_10_2] = fmtDesc{bytes: 4, nc: 4, comp: [4]compLayout{cl(0, 2), cl(2, 10), cl(12, 10), cl(22, 10)}}
	t[fmtX24_8_32F] = fmtDesc{bytes: 8, nc: 2, comp: [4]compLayout{cl(0, 32), cl(32, 8)}, float: true}
	t[fmt32_32] = fmtDesc{bytes: 8, nc: 2, comp: [4]compLayout{cl(0, 32), cl(32, 32)}}
	t[fmt32_32F] = fmtDesc{bytes: 8, nc: 2, comp: [4]compLayout{cl(0, 32), cl(32, 32)}, float: true}
	t[fmt16x4] = fmtDesc{bytes: 8, nc: 4, comp: [4]compLayout{cl(0, 16), cl(16, 16), cl(32, 16), cl(48, 16)}}
	t[fmt16x4F] = fmtDesc{bytes: 8, nc: 4, comp: [4]compLayout{cl(0, 16), cl(16, 16), cl(32, 16), cl(48, 16)}, float: true}
	t[fmt32x4] = fmtDesc{bytes: 16, nc: 4, comp: [4]compLayout{cl(0, 32), cl(32, 32), cl(64, 32), cl(96, 32)}}
	t[fmt32x4F] = fmtDesc{bytes: 16, nc: 4, comp: [4]compLayout{cl(0, 32), cl(32, 32), cl(64, 32), cl(96, 32)}, float: true}
	t[fmt1] = fmtDesc{bytes: 1, nc: 1, comp: [4]compLayout{cl(0, 1)}}
	t[fmt32_AS_8] = fmtDesc{bytes: 4, nc: 1, comp: [4]compLayout{cl(0, 8)}}
	t[fmt32_AS_8_8] = fmtDesc{bytes: 4, nc: 2, comp: [4]compLayout{cl(0, 8), cl(8, 8)}}
	t[fmt8_8_8] = fmtDesc{bytes: 3, nc: 3, comp: [4]compLayout{cl(0, 8), cl(8, 8), cl(16, 8)}}
	t[fmt16_16_16] = fmtDesc{bytes: 6, nc: 3, comp: [4]compLayout{cl(0, 16), cl(16, 16), cl(32, 16)}}
	t[fmt16_16_16F] = fmtDesc{bytes: 6, nc: 3, comp: [4]compLayout{cl(0, 16), cl(16, 16), cl(32, 16)}, float: true}
	t[fmt32_32_32] = fmtDesc{bytes: 12, nc: 3, comp: [4]compLayout{cl(0, 32), cl(32, 32), cl(64, 32)}}
	t[fmt32_32_32F] = fmtDesc{bytes: 12, nc: 3, comp: [4]compLayout{cl(0, 32), cl(32, 32), cl(64, 32)}, float: true}
	t[fmt5_9_9_9] = fmtDesc{bytes: 4, nc: 3}
	t[fmtGB_GR] = fmtDesc{bytes: 4, nc: 4, comp: [4]compLayout{cl(0, 8), cl(8, 8), cl(16, 8), cl(24, 8)}}
	t[fmtBG_RG] = fmtDesc{bytes: 4, nc: 4, comp: [4]compLayout{cl(0, 8), cl(8, 8), cl(16, 8), cl(24, 8)}}
	return t
}()

// field reads n bits at bit shift of a little-endian element.
func field(b []byte, shift, n uint8) uint32 {
	if n == 32 && shift%8 == 0 {
		o := shift / 8
		return uint32(b[o]) | uint32(b[o+1])<<8 | uint32(b[o+2])<<16 | uint32(b[o+3])<<24
	}
	var v uint64
	o := shift / 8
	for i := 0; i < 5 && int(o)+i < len(b); i++ {
		v |= uint64(b[int(o)+i]) << (8 * i)
	}
	return uint32(v>>(shift%8)) & (1<<n - 1)
}

func setField(b []byte, shift, n uint8, val uint32) {
	o := int(shift / 8)
	s := shift % 8
	mask := uint64(1)<<n - 1
	var v uint64
	for i := 0; i < 5 && o+i < len(b); i++ {
		v |= uint64(b[o+i]) << (8 * i)
	}
	v = v&^(mask<<s) | (uint64(val)&mask)<<s
	for i := 0; i < 5 && o+i < len(b); i++ {
		if (8*i) < int(s)+int(n) {
			b[o+i] = byte(v >> (8 * i))
		}
	}
}

func halfToF32(h uint16) float32 {
	s := uint32(h>>15) << 31
	e := uint32(h >> 10 & 0x1f)
	m := uint32(h & 0x3ff)
	switch {
	case e == 0:
		if m == 0 {
			return u2f(s)
		}
		f := float32(m) / 1024 / 16384
		if s != 0 {
			f = -f
		}
		return f
	case e == 31:
		return u2f(s | 0x7f800000 | m<<13)
	}
	return u2f(s | (e+112)<<23 | m<<13)
}

func f32ToHalf(f float32) uint16 {
	b := f2u(f)
	s := uint16(b>>16) & 0x8000
	e := int(b>>23&0xff) - 127 + 15
	m := b & 0x7fffff
	switch {
	case b&0x7fffffff > 0x7f800000: // NaN
		return s | 0x7e00
	case e >= 31:
		return s | 0x7c00
	case e <= 0:
		if e < -10 {
			return s
		}
		m |= 0x800000
		return s | uint16(m>>uint(14-e))
	}
	h := s | uint16(e)<<10 | uint16(m>>13)
	if m&0x1000 != 0 && (m&0x2fff) != 0 { // round to nearest even
		h++
	}
	return h
}

// small floats: 5-bit exponent, 6 or 5-bit mantissa, no sign
func smallToF32(v uint32, mbits uint) float32 {
	e := v >> mbits & 0x1f
	m := v & (1<<mbits - 1)
	switch e {
	case 0:
		return float32(m) / float32(uint32(1)<<mbits) / 16384
	case 31:
		if m == 0 {
			return float32(math.Inf(1))
		}
		return float32(math.NaN())
	}
	return u2f((e+112)<<23 | m<<(23-mbits))
}

func f32ToSmall(f float32, mbits uint) uint32 {
	if !(f > 0) {
		return 0
	}
	b := f2u(f)
	e := int(b>>23&0xff) - 127 + 15
	if e >= 31 {
		return 0x1f << mbits
	}
	if e <= 0 {
		return 0
	}
	return uint32(e)<<mbits | b&0x7fffff>>(23-mbits)
}

func srgbToLinear(v float32) float32 {
	if v <= 0.04045 {
		return v / 12.92
	}
	return float32(math.Pow((float64(v)+0.055)/1.055, 2.4))
}

func linearToSRGB(v float32) float32 {
	if v <= 0.0031308 {
		return v * 12.92
	}
	return float32(1.055*math.Pow(float64(v), 1/2.4) - 0.055)
}

func clamp01(v float32) float32 {
	if v > 1 {
		return 1
	}
	if !(v > 0) {
		return 0
	}
	return v
}

// decodeElem turns one element (little-endian bytes) into shader values;
// missing components read 0 (and alpha 1 for normalized formats).
func decodeElem(f uint32, k numKind, b []byte) [4]uint32 {
	d := &fmtDescs[f&0x3f]
	out := [4]uint32{0, 0, 0, f2u(1)}
	if k == kUint || k == kSint {
		out[3] = 1
	}
	if f&0x3f == fmt5_9_9_9 {
		v := field(b, 0, 32)
		e := int(v>>27) - 15 - 9
		sc := float32(math.Ldexp(1, e))
		out[0], out[1], out[2] = f2u(float32(v&0x1ff)*sc), f2u(float32(v>>9&0x1ff)*sc), f2u(float32(v>>18&0x1ff)*sc)
		return out
	}
	for c := 0; c < d.nc; c++ {
		l := d.comp[c]
		v := field(b, l.shift, l.n)
		switch {
		case d.small:
			out[c] = f2u(smallToF32(v, uint(l.n)-5))
		case d.float && l.n == 16:
			out[c] = f2u(halfToF32(uint16(v)))
		case d.float && l.n == 32:
			out[c] = v
		default:
			out[c] = convertIn(v, l.n, k, c)
		}
	}
	return out
}

func convertIn(v uint32, n uint8, k numKind, c int) uint32 {
	maxU := float32(uint64(1)<<n - 1)
	sext := func() int32 {
		if n >= 32 {
			return int32(v)
		}
		return int32(v<<(32-n)) >> (32 - n)
	}
	switch k {
	case kUnorm:
		return f2u(float32(v) / maxU)
	case kSrgb:
		f := float32(v) / maxU
		if c < 3 {
			f = srgbToLinear(f)
		}
		return f2u(f)
	case kSnorm:
		f := float32(sext()) / float32(uint64(1)<<(n-1)-1)
		if f < -1 {
			f = -1
		}
		return f2u(f)
	case kUscaled:
		return f2u(float32(v))
	case kSscaled:
		return f2u(float32(sext()))
	case kUint:
		return v
	case kSint:
		return uint32(sext())
	case kFloat:
		if n == 32 {
			return v
		}
		if n == 16 {
			return f2u(halfToF32(uint16(v)))
		}
		return f2u(float32(v) / maxU)
	}
	return v
}

func convertOut(v uint32, n uint8, k numKind, c int) uint32 {
	maxU := float64(uint64(1)<<n - 1)
	switch k {
	case kUnorm, kSrgb:
		f := clamp01(u2f(v))
		if k == kSrgb && c < 3 {
			f = linearToSRGB(f)
		}
		return uint32(math.Floor(float64(f)*maxU + 0.5))
	case kSnorm:
		f := float64(u2f(v))
		f = math.Max(-1, math.Min(1, f))
		h := float64(uint64(1)<<(n-1) - 1)
		return uint32(int32(math.Round(f*h))) & uint32(maxU)
	case kUscaled:
		return uint32(math.Max(0, math.Min(maxU, float64(u2f(v)))))
	case kSscaled:
		return uint32(int32(u2f(v))) & uint32(maxU)
	case kUint, kSint:
		if n >= 32 {
			return v
		}
		return v & uint32(maxU)
	case kFloat:
		if n == 32 {
			return v
		}
		if n == 16 {
			return uint32(f32ToHalf(u2f(v)))
		}
		return uint32(math.Floor(float64(clamp01(u2f(v)))*maxU + 0.5))
	}
	return v
}

// encodeElem writes shader values into one element.
func encodeElem(f uint32, k numKind, v [4]uint32, b []byte) {
	d := &fmtDescs[f&0x3f]
	for c := 0; c < d.nc; c++ {
		l := d.comp[c]
		var x uint32
		switch {
		case d.small:
			x = f32ToSmall(u2f(v[c]), uint(l.n)-5)
		case d.float && l.n == 16:
			x = uint32(f32ToHalf(u2f(v[c])))
		case d.float && l.n == 32:
			x = v[c]
		default:
			x = convertOut(v[c], l.n, k, c)
		}
		setField(b, l.shift, l.n, x)
	}
}

func fmtBytes(f uint32) uint32 {
	if n := fmtDescs[f&0x3f].bytes; n != 0 {
		return n
	}
	return formatBits(f) / 8
}
