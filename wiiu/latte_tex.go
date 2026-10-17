package wiiu

// Latte textures: decoding texture levels (with their tiling and compressed
// formats) into a cache, sampling them the way the shaders ask (filtering,
// mipmaps, wrapping, depth compares), and vertex fetches.

import (
	"math"
	"math/bits"
)

// texDesc is a texture resource (SQ_TEX_RESOURCE_WORD0..6).
type texDesc struct {
	dim, mode, pitch, width, height, depth, format uint32
	base, mipBase, swizzle                         uint32
	kind                                           numKind
	dstSel                                         [4]uint32
	baseLevel, lastLevel, baseArray, lastArray     uint32
	endian                                         uint32
	valid                                          bool
	w                                              [7]uint32
}

func (g *Latte) resource(i uint32) [7]uint32 {
	var w [7]uint32
	a := regSQ_FETCH_RESOURCE + i*28
	for k := range w {
		w[k] = g.reg(a + uint32(k)*4)
	}
	return w
}

func texFromWords(w [7]uint32) texDesc {
	t := texDesc{w: w}
	t.dim = w[0] & 7
	t.mode = w[0] >> 3 & 0xf
	t.pitch = (w[0]>>8&0x7ff + 1) * 8
	t.width = w[0]>>19&0x1fff + 1
	t.height = w[1]&0x3fff + 1
	t.depth = w[1]>>14&0x1fff + 1
	t.format = w[1] >> 26 & 0x3f
	t.base, t.swizzle = splitSwizzle(w[2]<<8, t.mode)
	t.mipBase, _ = splitSwizzle(w[3]<<8, t.mode)
	signed := w[4]&3 == 1
	t.kind = numKindFromFetch(w[4]>>8&3, signed, w[4]>>11&1 != 0)
	t.endian = w[4] >> 12 & 3
	t.dstSel = [4]uint32{w[4] >> 16 & 7, w[4] >> 19 & 7, w[4] >> 22 & 7, w[4] >> 25 & 7}
	t.baseLevel = w[4] >> 28 & 0xf
	t.lastLevel = w[5] & 0xf
	t.baseArray = w[5] >> 4 & 0x1fff
	t.lastArray = w[5] >> 17 & 0x1fff
	t.valid = w[6]>>30 == 2 && t.base != 0 && t.format != 0
	return t
}

// ---- the decoded-level cache ----

type texKey struct {
	w0, w1, w2, w3, w4, w5 uint32
	level                  uint32
}

// texLevel is one decoded mip level: either packed RGBA8 (for formats of up
// to 8 bits a component) or four 32-bit shader values per texel.
type texLevel struct {
	w, h, d  uint32
	rgba8    []uint32
	vals     []uint32
	lo, hi   uint32 // the memory it came from
	srgb     bool
	lastUsed uint64
}

type texCache struct {
	m     map[texKey]*texLevel
	bytes int
	tick  uint64
}

func (c *texCache) init() { c.m = map[texKey]*texLevel{} }

// invalidate drops cached levels decoded from memory in [lo, hi).
func (c *texCache) invalidate(lo, hi uint32) {
	for k, l := range c.m {
		if l.lo < hi && lo < l.hi {
			c.bytes -= l.size()
			delete(c.m, k)
		}
	}
}

func (c *texCache) clear() {
	c.m = map[texKey]*texLevel{}
	c.bytes = 0
}

func (l *texLevel) size() int { return len(l.rgba8)*4 + len(l.vals)*4 }

const texCacheMax = 512 << 20

// fits8 reports whether a format decodes exactly into 8 bits a component.
func fits8(f uint32, k numKind) bool {
	if k != kUnorm && k != kSrgb {
		return false
	}
	switch f {
	case fmt8, fmt4_4, fmt3_3_2, fmt8_8, fmt5_6_5, fmt6_5_5, fmt1_5_5_5, fmt4_4_4_4, fmt5_5_5_1, fmt8_8_8_8,
		fmtBC1, fmtBC2, fmtBC3, fmtBC4, fmtBC5, fmt1:
		return true
	}
	return false
}

// level returns texture level lvl (relative to level 0), decoding it if
// needed.
func (g *Latte) texLevel(t *texDesc, lvl uint32) *texLevel {
	c := &g.tex
	key := texKey{t.w[0], t.w[1], t.w[2], t.w[3], t.w[4] &^ (0x7ff << 16), t.w[5], lvl}
	c.tick++
	if l := c.m[key]; l != nil {
		l.lastUsed = c.tick
		return l
	}
	if c.bytes > texCacheMax {
		// drop the older half
		for k, l := range c.m {
			if l.lastUsed < c.tick-uint64(len(c.m))/2 || c.bytes > texCacheMax {
				c.bytes -= l.size()
				delete(c.m, k)
			}
		}
	}
	l := g.decodeLevel(t, lvl)
	l.lastUsed = c.tick
	c.m[key] = l
	c.bytes += l.size()
	return l
}

// texSlices: how many slices (array layers, cube faces, depth) level 0 has
func (t *texDesc) slices() uint32 {
	switch t.dim {
	case 2, 3, 4, 5, 7: // 3D, cube, arrays
		return t.depth
	}
	return 1
}

func (g *Latte) decodeLevel(t *texDesc, lvl uint32) *texLevel {
	f := t.format
	comp := formatCompressed(f)
	w0, h0 := t.width, t.height
	if t.dim == 4 { // 1D array: height is the layer count
		h0 = 1
	}
	// the GPU works out the mip chain the same way GX2 lays it out
	info := calcSurfInfo(t.dim, w0, h0, t.depth, t.lastLevel+1, f, 0, 1, t.mode)
	if int(lvl) >= len(info.Levels) {
		lvl = uint32(len(info.Levels) - 1)
	}
	li := info.Levels[lvl]
	base := t.base
	if lvl > 0 {
		base = t.mipBase + li.Offset
		if t.mipBase == 0 {
			base = t.base + alignUp(info.Size, info.Align) + li.Offset
		}
	}
	pitch := li.Pitch
	if lvl == 0 {
		// the register's pitch (in texels) wins for level 0
		pitch = t.pitch
		if comp {
			pitch = (pitch + 3) / 4
		}
	}
	lw, lh := max(w0>>lvl, 1), max(h0>>lvl, 1)
	ld := t.slices()
	if t.dim == 2 {
		ld = max(t.depth>>lvl, 1)
	}
	lay := surfLayout{Base: base, Pitch: pitch, Height: li.Height, Slices: ld, BPP: formatBits(f), Mode: li.Mode,
		Samples: 1, Swizzle: t.swizzle}
	if t.dim == 4 {
		ld = t.depth
	}
	l := &texLevel{w: lw, h: lh, d: ld, srgb: t.kind == kSrgb}
	ew, eh := lw, lh
	if comp {
		ew, eh = (lw+3)/4, (lh+3)/4
	}
	use8 := fits8(f, t.kind) || comp
	n := int(lw * lh * ld)
	if use8 {
		l.rgba8 = make([]uint32, n)
	} else {
		l.vals = make([]uint32, n*4)
	}
	eb := lay.BPP / 8
	buf := make([]byte, 16)
	addrs := make([]uint32, ew)
	l.lo, l.hi = ^uint32(0), 0
	kind := t.kind
	if kind == kSrgb {
		kind = kUnorm // sRGB is undone when sampling, after the 8-bit lookup
	}
	// 8-bit RGBA: the bytes are the values
	fast4 := use8 && f == fmt8_8_8_8 && eb == 4 && t.endian != 3
	for z := uint32(0); z < ld; z++ {
		for ey := uint32(0); ey < eh; ey++ {
			lay.rowAddrs(0, ey, z, addrs)
			for ex := uint32(0); ex < ew; ex++ {
				a := addrs[ex]
				if a < l.lo {
					l.lo = a
				}
				if a+eb > l.hi {
					l.hi = a + eb
				}
				if fast4 {
					v := g.mem.Read32(a) // bytes R, G, B, A
					switch t.endian {
					case 1:
						v = v>>8&0x00ff00ff | v<<8&0xff00ff00
					case 2:
						v = bits.ReverseBytes32(v)
					}
					if ex < lw && ey < lh {
						l.rgba8[(z*lh+ey)*lw+ex] = bits.ReverseBytes32(v)
					}
					continue
				}
				g.mem.ReadBytes(a, buf[:eb])
				swapBytes(buf[:eb], t.endian)
				if comp {
					var px [16]uint32
					decodeBC(f, buf[:eb], &px, t.kind == kSnorm)
					for py := uint32(0); py < 4; py++ {
						y := ey*4 + py
						if y >= lh {
							break
						}
						for pxx := uint32(0); pxx < 4; pxx++ {
							x := ex*4 + pxx
							if x >= lw {
								break
							}
							l.rgba8[(z*lh+y)*lw+x] = px[py*4+pxx]
						}
					}
					continue
				}
				if ex >= lw || ey >= lh {
					continue
				}
				i := (z*lh+ey)*lw + ex
				if use8 {
					v := decodeElem(f, kind, buf[:eb])
					l.rgba8[i] = q8(v[0]) | q8(v[1])<<8 | q8(v[2])<<16 | q8(v[3])<<24
					continue
				}
				v := decodeElem(f, kind, buf[:eb])
				copy(l.vals[i*4:], v[:])
			}
		}
	}
	if l.lo > l.hi {
		l.lo, l.hi = 0, 0
	}
	return l
}

func q8(v uint32) uint32 { return uint32(clamp01(u2f(v))*255 + 0.5) }

// swapBytes undoes an ENDIAN_SWAP setting (1: 8 in 16, 2: 8 in 32)
func swapBytes(b []byte, mode uint32) {
	switch mode {
	case 1:
		for i := 0; i+1 < len(b); i += 2 {
			b[i], b[i+1] = b[i+1], b[i]
		}
	case 2:
		for i := 0; i+3 < len(b); i += 4 {
			b[i], b[i+1], b[i+2], b[i+3] = b[i+3], b[i+2], b[i+1], b[i]
		}
	case 3:
		for i := 0; i+7 < len(b); i += 8 {
			for j := 0; j < 4; j++ {
				b[i+j], b[i+7-j] = b[i+7-j], b[i+j]
			}
		}
	}
}

// ---- block-compressed formats ----

func rgb565(c uint16) (r, g, b uint32) {
	r = uint32(c>>11) & 31
	g = uint32(c>>5) & 63
	b = uint32(c) & 31
	return r<<3 | r>>2, g<<2 | g>>4, b<<3 | b>>2
}

// bc1 colours: the colour part of BC1-BC3 blocks
func bcColors(b []byte, out *[16]uint32, bc1 bool) {
	c0 := uint16(b[0]) | uint16(b[1])<<8
	c1 := uint16(b[2]) | uint16(b[3])<<8
	r0, g0, b0 := rgb565(c0)
	r1, g1, b1 := rgb565(c1)
	var pal [4]uint32
	pk := func(r, g, b, a uint32) uint32 { return r | g<<8 | b<<16 | a<<24 }
	pal[0] = pk(r0, g0, b0, 255)
	pal[1] = pk(r1, g1, b1, 255)
	if c0 > c1 || !bc1 {
		pal[2] = pk((2*r0+r1)/3, (2*g0+g1)/3, (2*b0+b1)/3, 255)
		pal[3] = pk((r0+2*r1)/3, (g0+2*g1)/3, (b0+2*b1)/3, 255)
	} else {
		pal[2] = pk((r0+r1)/2, (g0+g1)/2, (b0+b1)/2, 255)
		pal[3] = 0
	}
	idx := uint32(b[4]) | uint32(b[5])<<8 | uint32(b[6])<<16 | uint32(b[7])<<24
	for i := 0; i < 16; i++ {
		out[i] = pal[idx>>(2*i)&3]
	}
}

// bc4 channel: 8 bytes, 8 interpolated values
func bcChannel(b []byte, signed bool) [16]uint32 {
	var out [16]uint32
	var pal [8]int32
	a0, a1 := int32(b[0]), int32(b[1])
	if signed {
		a0, a1 = int32(int8(b[0])), int32(int8(b[1]))
	}
	pal[0], pal[1] = a0, a1
	if a0 > a1 {
		for i := int32(1); i < 7; i++ {
			pal[1+i] = ((7-i)*a0 + i*a1) / 7
		}
	} else {
		for i := int32(1); i < 5; i++ {
			pal[1+i] = ((5-i)*a0 + i*a1) / 5
		}
		if signed {
			pal[6], pal[7] = -127, 127
		} else {
			pal[6], pal[7] = 0, 255
		}
	}
	var bits uint64
	for i := 0; i < 6; i++ {
		bits |= uint64(b[2+i]) << (8 * i)
	}
	for i := 0; i < 16; i++ {
		v := pal[bits>>(3*i)&7]
		if signed { // to 0..255 for the 8-bit store (decoded as snorm later)
			v = (max(v, -127) + 127) * 255 / 254
		}
		out[i] = uint32(v)
	}
	return out
}

func decodeBC(f uint32, b []byte, out *[16]uint32, signed bool) {
	switch f {
	case fmtBC1:
		bcColors(b, out, true)
	case fmtBC2:
		bcColors(b[8:], out, false)
		for i := 0; i < 16; i++ {
			a := uint32(b[i/2]>>(4*(i&1))) & 15
			out[i] = out[i]&0xffffff | (a*17)<<24
		}
	case fmtBC3:
		bcColors(b[8:], out, false)
		a := bcChannel(b, false)
		for i := 0; i < 16; i++ {
			out[i] = out[i]&0xffffff | a[i]<<24
		}
	case fmtBC4:
		r := bcChannel(b, signed)
		for i := 0; i < 16; i++ {
			out[i] = r[i] | 0xff000000
		}
	case fmtBC5:
		r := bcChannel(b, signed)
		gch := bcChannel(b[8:], signed)
		for i := 0; i < 16; i++ {
			out[i] = r[i] | gch[i]<<8 | 0xff000000
		}
	}
}

// ---- samplers ----

type samplerDesc struct {
	clamp           [3]uint32
	magFilter       uint32
	minFilter       uint32
	mipFilter       uint32
	compare         uint32
	borderType      uint32
	minLOD, maxLOD  float32
	lodBias         float32
	borderColor     [4]float32
	compareDisabled bool
}

func (g *Latte) sampler(i uint32) samplerDesc {
	a := regSQ_TEX_SAMPLER_WORD0_0 + i*12
	w0, w1 := g.reg(a), g.reg(a+4)
	s := samplerDesc{
		clamp:      [3]uint32{w0 & 7, w0 >> 3 & 7, w0 >> 6 & 7},
		magFilter:  w0 >> 9 & 7,
		minFilter:  w0 >> 12 & 7,
		mipFilter:  w0 >> 17 & 3,
		borderType: w0 >> 22 & 3,
		compare:    w0 >> 26 & 7,
		minLOD:     float32(w1&0x3ff) / 64,
		maxLOD:     float32(w1>>10&0x3ff) / 64,
		lodBias:    float32(int32(w1)>>20) / 64,
	}
	switch s.borderType {
	case 1:
		s.borderColor = [4]float32{0, 0, 0, 1}
	case 2:
		s.borderColor = [4]float32{1, 1, 1, 1}
	}
	return s
}

// wrap applies a clamp mode to texel coordinate i of a size-n axis; ok is
// false for border texels.
func wrap(i, n int32, mode uint32) (int32, bool) {
	switch mode {
	case 0: // wrap
		i %= n
		if i < 0 {
			i += n
		}
	case 1: // mirror
		p := 2 * n
		i %= p
		if i < 0 {
			i += p
		}
		if i >= n {
			i = p - 1 - i
		}
	case 2, 4: // clamp to the last texel (half border treated as clamp)
		i = min(max(i, 0), n-1)
	case 3, 5: // mirror once
		if i < 0 {
			i = -1 - i
		}
		i = min(i, n-1)
	case 6, 7: // border
		if i < 0 || i >= n {
			return 0, false
		}
	}
	return i, true
}

var srgbLUT = func() [256]float32 {
	var t [256]float32
	for i := range t {
		t[i] = srgbToLinear(float32(i) / 255)
	}
	return t
}()

// texel reads one texel as four floats-or-ints (shader values).
func (l *texLevel) texel(x, y, z int32) [4]uint32 {
	i := (uint32(z)*l.h+uint32(y))*l.w + uint32(x)
	if l.rgba8 != nil {
		v := l.rgba8[i]
		if l.srgb {
			return [4]uint32{f2u(srgbLUT[v&255]), f2u(srgbLUT[v>>8&255]), f2u(srgbLUT[v>>16&255]), f2u(float32(v>>24) / 255)}
		}
		return [4]uint32{f2u(float32(v&255) / 255), f2u(float32(v>>8&255) / 255), f2u(float32(v>>16&255) / 255), f2u(float32(v>>24) / 255)}
	}
	return [4]uint32(l.vals[i*4 : i*4+4])
}

type sampleReq struct {
	u, v, w   float32 // texel space is computed from these (0..1 when normalized)
	slice     int32
	lod       float32
	compare   bool
	ref       float32
	normalize [3]bool
	off       [3]float32
}

// sample filters a texture at one point.
func (g *Latte) sample(t *texDesc, s *samplerDesc, r *sampleReq) [4]uint32 {
	lvlMax := float32(int32(t.lastLevel) - int32(t.baseLevel))
	lod := r.lod
	lod = max(lod, s.minLOD)
	lod = min(lod, s.maxLOD)
	lod = min(lod, lvlMax)
	lod = max(lod, 0)
	filter := s.magFilter
	if r.lod > 0 {
		filter = s.minFilter
	}
	switch s.mipFilter {
	case 0: // no mipmaps
		return g.sampleLevel(t, s, r, t.baseLevel, filter)
	case 1: // nearest level
		return g.sampleLevel(t, s, r, t.baseLevel+uint32(math.Floor(float64(lod)+0.5)), filter)
	}
	l0 := uint32(math.Floor(float64(lod)))
	fr := lod - float32(l0)
	a := g.sampleLevel(t, s, r, t.baseLevel+l0, filter)
	if fr == 0 || float32(l0) >= lvlMax {
		return a
	}
	b := g.sampleLevel(t, s, r, t.baseLevel+l0+1, filter)
	return lerp4(a, b, fr)
}

func lerp4(a, b [4]uint32, f float32) [4]uint32 {
	var o [4]uint32
	for c := range o {
		x, y := u2f(a[c]), u2f(b[c])
		o[c] = f2u(x + (y-x)*f)
	}
	return o
}

func (g *Latte) sampleLevel(t *texDesc, s *samplerDesc, r *sampleReq, lvl uint32, filter uint32) [4]uint32 {
	l := g.texLevel(t, lvl)
	fw, fh, fd := float32(l.w), float32(l.h), float32(l.d)
	u, v, w := r.u, r.v, r.w
	if r.normalize[0] {
		u *= fw
	}
	if r.normalize[1] {
		v *= fh
	}
	if r.normalize[2] && t.dim == 2 {
		w *= fd
	}
	u += r.off[0]
	v += r.off[1]
	z := r.slice
	if t.dim == 2 {
		z = int32(math.Floor(float64(w)))
	}
	if z < 0 {
		z = 0
	}
	if z >= int32(l.d) {
		z = int32(l.d) - 1
	}
	fetch := func(x, y int32) [4]uint32 {
		x, okx := wrap(x, int32(l.w), s.clamp[0])
		y, oky := wrap(y, int32(l.h), s.clamp[1])
		if !okx || !oky {
			b := s.borderColor
			return [4]uint32{f2u(b[0]), f2u(b[1]), f2u(b[2]), f2u(b[3])}
		}
		c := l.texel(x, y, z)
		if r.compare {
			d := u2f(c[0])
			p := f2u(0)
			if compareOK(s.compare, r.ref, d) {
				p = f2u(1)
			}
			return [4]uint32{p, p, p, p}
		}
		return c
	}
	if filter == 0 || filter == 2 || l.vals != nil && !isFloatish(t) {
		// point sampling (and integer textures, which can't be filtered)
		return fetch(int32(math.Floor(float64(u))), int32(math.Floor(float64(v))))
	}
	u -= 0.5
	v -= 0.5
	x0, y0 := math.Floor(float64(u)), math.Floor(float64(v))
	fx, fy := u-float32(x0), v-float32(y0)
	ix, iy := int32(x0), int32(y0)
	a := fetch(ix, iy)
	b := fetch(ix+1, iy)
	c := fetch(ix, iy+1)
	d := fetch(ix+1, iy+1)
	return lerp4(lerp4(a, b, fx), lerp4(c, d, fx), fy)
}

func isFloatish(t *texDesc) bool { return t.kind != kUint && t.kind != kSint }

func compareOK(f uint32, a, b float32) bool {
	switch f {
	case 0:
		return false
	case 1:
		return a < b
	case 2:
		return a == b
	case 3:
		return a <= b
	case 4:
		return a > b
	case 5:
		return a != b
	case 6:
		return a >= b
	}
	return true
}

func applySel(v [4]uint32, sel [4]uint32, intFmt bool) [4]uint32 {
	var o [4]uint32
	one := f2u(1)
	if intFmt {
		one = 1
	}
	for c := 0; c < 4; c++ {
		switch sel[c] {
		case 0, 1, 2, 3:
			o[c] = v[sel[c]]
		case 4:
			o[c] = 0
		default:
			o[c] = one
		}
	}
	return o
}
