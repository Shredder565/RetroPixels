package ps3

// RSX textures: formats, swizzled (Morton order) and linear layouts, DXT,
// wrap modes, channel remapping and point/bilinear filtering. Sampling
// reads a snapshot of the texture taken when a draw starts.

import "math"

type rsxTexture struct {
	ok         bool
	data       []byte
	fmt        uint32 // format without LN/UN
	linear     bool
	unnorm     bool
	w, h       int
	pitch      int
	log2w      uint
	log2h      uint
	wrapS      uint32
	wrapT      uint32
	magLinear  bool
	minLinear  bool
	remap      uint32
	border     vec4
	bpp        int // bytes per texel (0 for DXT)
	cube, dim3 bool
	pix        []uint32 // decoded A8R8G8B8, w*h (for 8-bit formats)
	ea         uint32   // where the texels are
	size       int      // and how many bytes
}

// decode8 turns 8-bit-per-channel formats into plain ARGB pixels.
func (t *rsxTexture) decode8() {
	switch t.fmt {
	case texW16F, texW32F, texX32F, texY16X16F, texY16X16, texX16, texD24S8, texD24S8F, texD16, texD16F:
		return
	}
	t.pix = make([]uint32, t.w*t.h)
	if t.fmt == texA8R8G8B8 && t.remap == 0xAAE4 {
		for y := 0; y < t.h; y++ {
			for x := 0; x < t.w; x++ {
				var o int
				if t.linear {
					o = y*t.pitch + x*4
				} else {
					o = swizzleOffset(x, y, t.log2w, t.log2h) * 4
				}
				if o+4 <= len(t.data) {
					t.pix[y*t.w+x] = be32(t.data[o:])
				}
			}
		}
		return
	}
	for y := 0; y < t.h; y++ {
		for x := 0; x < t.w; x++ {
			c := t.texel(x, y)
			if t.remap != 0xAAE4 && t.remap != 0 {
				var out [4]float32
				for ch := 0; ch < 4; ch++ {
					switch t.remap >> (8 + 2*ch) & 3 {
					case 0:
						out[ch] = 0
					case 1:
						out[ch] = 1
					default:
						out[ch] = c[t.remap>>(2*ch)&3]
					}
				}
				c = out
			}
			t.pix[y*t.w+x] = u8c(c[0])<<24 | u8c(c[1])<<16 | u8c(c[2])<<8 | u8c(c[3])
		}
	}
}

func (t *rsxTexture) fetch8(x, y int) uint32 {
	xx, ok1 := wrapCoord(x, t.w, t.wrapS)
	yy, ok2 := wrapCoord(y, t.h, t.wrapT)
	if !ok1 || !ok2 {
		b := t.border
		return u8c(b[3])<<24 | u8c(b[0])<<16 | u8c(b[1])<<8 | u8c(b[2])
	}
	return t.pix[yy*t.w+xx]
}

func (t *rsxTexture) sample8(u, v float32) vec4 {
	const k = 1.0 / 255
	if t.magLinear || t.minLinear {
		u -= 0.5
		v -= 0.5
		fu, fv := float32(math.Floor(float64(u))), float32(math.Floor(float64(v)))
		x0, y0 := int(fu), int(fv)
		fx, fy := u-fu, v-fv
		a, b, c, d := t.fetch8(x0, y0), t.fetch8(x0+1, y0), t.fetch8(x0, y0+1), t.fetch8(x0+1, y0+1)
		w00, w10, w01, w11 := (1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy
		ch := func(sh uint) float32 {
			return (float32(a>>sh&0xff)*w00 + float32(b>>sh&0xff)*w10 + float32(c>>sh&0xff)*w01 + float32(d>>sh&0xff)*w11) * k
		}
		return vec4{ch(16), ch(8), ch(0), ch(24)}
	}
	p := t.fetch8(int(math.Floor(float64(u))), int(math.Floor(float64(v))))
	return vec4{float32(p>>16&0xff) * k, float32(p>>8&0xff) * k, float32(p&0xff) * k, float32(p>>24) * k}
}

const (
	texB8       = 0x81
	texA1R5G5B5 = 0x82
	texA4R4G4B4 = 0x83
	texR5G6B5   = 0x84
	texA8R8G8B8 = 0x85
	texDXT1     = 0x86
	texDXT23    = 0x87
	texDXT45    = 0x88
	texG8B8     = 0x8B
	texR6G5B5   = 0x8F
	texD24S8    = 0x90
	texD24S8F   = 0x91
	texD16      = 0x92
	texD16F     = 0x93
	texX16      = 0x94
	texY16X16   = 0x95
	texR5G5B5A1 = 0x97
	texHILO8    = 0x98
	texHILOS8   = 0x99
	texW16F     = 0x9A
	texW32F     = 0x9B
	texX32F     = 0x9C
	texD1R5G5B5 = 0x9D
	texD8R8G8B8 = 0x9E
	texY16X16F  = 0x9F
	texB8R8G8R8 = 0xAD
	texR8B8R8G8 = 0xAE
)

func texBpp(f uint32) int {
	switch f {
	case texB8:
		return 1
	case texA1R5G5B5, texA4R4G4B4, texR5G6B5, texG8B8, texR6G5B5, texD16, texD16F, texX16, texR5G5B5A1, texHILO8, texHILOS8, texD1R5G5B5:
		return 2
	case texA8R8G8B8, texD24S8, texD24S8F, texY16X16, texX32F, texD8R8G8B8, texY16X16F, texB8R8G8R8, texR8B8R8G8:
		return 4
	case texW16F:
		return 8
	case texW32F:
		return 16
	}
	return 0
}

// loadTexture reads fragment texture unit i's registers and snapshots it.
func (m *Machine) loadTexture(r *rsxState, i int) rsxTexture {
	t := m.textureRegs(r, i)
	if t.size == 0 {
		return t
	}
	t.data = make([]byte, t.size)
	m.Mem.ReadBytes(t.ea, t.data)
	t.ok = true
	t.decode8()
	return t
}

// textureRegs reads unit i's registers: everything but the texels (size 0
// when there's nothing to read).
func (m *Machine) textureRegs(r *rsxState, i int) rsxTexture {
	base := uint32(0x1A00 + 0x20*i)
	reg := func(a uint32) uint32 { return r.regs[a/4] }
	format := reg(base + 4)
	t := rsxTexture{}
	off := reg(base) & 0x7fffffff
	loc := (format & 3) - 1
	raw := format >> 8 & 0xff
	t.fmt = raw &^ 0x60
	t.linear = raw&0x20 != 0
	t.unnorm = raw&0x40 != 0
	t.cube = format>>2&1 != 0
	t.dim3 = format>>4&0xf == 3
	rect := reg(base + 0x18)
	t.w = int(rect >> 16)
	t.h = int(rect & 0xffff)
	if t.w == 0 || t.h == 0 || t.w > 4096 || t.h > 4096 {
		return t
	}
	t.pitch = int(r.regs[(0x1840+4*uint32(i))/4] & 0xfffff)
	addr := reg(base + 8)
	t.wrapS = addr & 0xf
	t.wrapT = addr >> 8 & 0xf
	filt := reg(base + 0x14)
	minf := filt >> 16 & 7
	t.minLinear = minf == 2 || minf == 4 || minf == 6
	t.magLinear = filt>>24&7 == 2
	t.remap = reg(base+0x10) & 0xffff
	bc := reg(base + 0x1c)
	t.border = vec4{float32(bc>>16&0xff) / 255, float32(bc>>8&0xff) / 255, float32(bc&0xff) / 255, float32(bc>>24) / 255}
	t.bpp = texBpp(t.fmt)
	for t.log2w = 0; 1<<t.log2w < t.w; t.log2w++ {
	}
	for t.log2h = 0; 1<<t.log2h < t.h; t.log2h++ {
	}
	var size int
	switch {
	case t.fmt == texDXT1:
		size = ((t.w + 3) / 4) * ((t.h + 3) / 4) * 8
	case t.fmt == texDXT23 || t.fmt == texDXT45:
		size = ((t.w + 3) / 4) * ((t.h + 3) / 4) * 16
	case t.bpp == 0:
		return t
	case t.linear:
		if t.pitch == 0 {
			t.pitch = t.w * t.bpp
		}
		size = t.pitch*(t.h-1) + t.w*t.bpp
	default:
		size = (1 << t.log2w) * (1 << t.log2h) * t.bpp
	}
	ea := m.gcm.offsetToAddr(off, loc)
	if ea == 0 || !m.Mem.Mapped(ea) || !m.Mem.Mapped(ea+uint32(size)-1) {
		return t
	}
	t.ea, t.size = ea, size
	return t
}

// swizzleOffset is the Morton-order index of texel (x, y).
func swizzleOffset(x, y int, lw, lh uint) int {
	off, shift := 0, uint(0)
	for lw > 0 || lh > 0 {
		if lw > 0 {
			off |= (x & 1) << shift
			shift++
			x >>= 1
			lw--
		}
		if lh > 0 {
			off |= (y & 1) << shift
			shift++
			y >>= 1
			lh--
		}
	}
	return off
}

func be16(b []byte) uint16 { return uint16(b[0])<<8 | uint16(b[1]) }

// texel returns texel (x, y) as A, R, G, B in 0..1 (x, y already wrapped).
func (t *rsxTexture) texel(x, y int) [4]float32 {
	switch t.fmt {
	case texDXT1, texDXT23, texDXT45:
		return t.dxtTexel(x, y)
	}
	var p []byte
	if t.linear {
		o := y*t.pitch + x*t.bpp
		if o+t.bpp > len(t.data) {
			return [4]float32{}
		}
		p = t.data[o:]
	} else {
		o := swizzleOffset(x, y, t.log2w, t.log2h) * t.bpp
		if o+t.bpp > len(t.data) {
			return [4]float32{}
		}
		p = t.data[o:]
	}
	const k8 = 1.0 / 255
	switch t.fmt {
	case texB8:
		v := float32(p[0]) * k8
		return [4]float32{v, v, v, v}
	case texA8R8G8B8, texD8R8G8B8:
		a := float32(p[0]) * k8
		if t.fmt == texD8R8G8B8 {
			a = 1
		}
		return [4]float32{a, float32(p[1]) * k8, float32(p[2]) * k8, float32(p[3]) * k8}
	case texA1R5G5B5, texD1R5G5B5:
		v := be16(p)
		a := float32(v >> 15)
		if t.fmt == texD1R5G5B5 {
			a = 1
		}
		return [4]float32{a, float32(v>>10&31) / 31, float32(v>>5&31) / 31, float32(v&31) / 31}
	case texR5G5B5A1:
		v := be16(p)
		return [4]float32{float32(v & 1), float32(v>>11&31) / 31, float32(v>>6&31) / 31, float32(v>>1&31) / 31}
	case texA4R4G4B4:
		v := be16(p)
		return [4]float32{float32(v>>12) / 15, float32(v>>8&15) / 15, float32(v>>4&15) / 15, float32(v&15) / 15}
	case texR5G6B5:
		v := be16(p)
		return [4]float32{1, float32(v>>11) / 31, float32(v>>5&63) / 63, float32(v&31) / 31}
	case texR6G5B5:
		v := be16(p)
		return [4]float32{1, float32(v>>10) / 63, float32(v>>5&31) / 31, float32(v&31) / 31}
	case texG8B8:
		g, b := float32(p[0])*k8, float32(p[1])*k8
		return [4]float32{g, b, g, b}
	case texX16, texD16:
		v := float32(be16(p)) / 65535
		return [4]float32{v, v, v, v}
	case texY16X16:
		y, x := float32(be16(p))/65535, float32(be16(p[2:]))/65535
		return [4]float32{y, x, y, x}
	case texHILO8:
		a, b := float32(p[0])*k8, float32(p[1])*k8
		return [4]float32{a, b, a, b}
	case texD24S8:
		v := float32(uint32(p[0])<<16|uint32(p[1])<<8|uint32(p[2])) / 0xffffff
		return [4]float32{v, v, v, v}
	case texX32F:
		v := math.Float32frombits(be32(p))
		return [4]float32{v, v, v, v}
	case texY16X16F:
		y, x := halfToF32(be16(p)), halfToF32(be16(p[2:]))
		return [4]float32{y, x, y, x}
	case texW16F:
		// stored R, G, B, A halves
		r, g, b, a := halfToF32(be16(p)), halfToF32(be16(p[2:])), halfToF32(be16(p[4:])), halfToF32(be16(p[6:]))
		return [4]float32{a, r, g, b}
	case texW32F:
		r, g, b, a := math.Float32frombits(be32(p)), math.Float32frombits(be32(p[4:])), math.Float32frombits(be32(p[8:])), math.Float32frombits(be32(p[12:]))
		return [4]float32{a, r, g, b}
	}
	return [4]float32{1, 1, 0, 1}
}

func rgb565(c uint16) [3]float32 {
	return [3]float32{float32(c>>11) / 31, float32(c>>5&63) / 63, float32(c&31) / 31}
}

func (t *rsxTexture) dxtTexel(x, y int) [4]float32 {
	bw := (t.w + 3) / 4
	bs := 16
	if t.fmt == texDXT1 {
		bs = 8
	}
	o := ((y/4)*bw + x/4) * bs
	if o+bs > len(t.data) {
		return [4]float32{}
	}
	b := t.data[o:]
	px, py := x&3, y&3
	alpha := float32(1)
	cb := b
	if t.fmt != texDXT1 {
		cb = b[8:]
		if t.fmt == texDXT23 {
			n := uint(py*4 + px)
			v := uint16(b[n/2]) >> (4 * (n & 1)) & 15
			alpha = float32(v) / 15
		} else {
			a0, a1 := float32(b[0])/255, float32(b[1])/255
			var bitsv uint64
			for i := 0; i < 6; i++ {
				bitsv |= uint64(b[2+i]) << (8 * i)
			}
			n := uint(py*4 + px)
			code := bitsv >> (3 * n) & 7
			switch {
			case code == 0:
				alpha = a0
			case code == 1:
				alpha = a1
			case a0 > a1:
				alpha = (float32(8-code)*a0 + float32(code-1)*a1) / 7
			case code == 6:
				alpha = 0
			case code == 7:
				alpha = 1
			default:
				alpha = (float32(6-code)*a0 + float32(code-1)*a1) / 5
			}
		}
	}
	c0 := uint16(cb[1])<<8 | uint16(cb[0])
	c1 := uint16(cb[3])<<8 | uint16(cb[2])
	idx := cb[4+py] >> (2 * px) & 3
	p0, p1 := rgb565(c0), rgb565(c1)
	var c [3]float32
	switch idx {
	case 0:
		c = p0
	case 1:
		c = p1
	case 2:
		for i := range c {
			if c0 > c1 || t.fmt != texDXT1 {
				c[i] = (2*p0[i] + p1[i]) / 3
			} else {
				c[i] = (p0[i] + p1[i]) / 2
			}
		}
	case 3:
		if c0 > c1 || t.fmt != texDXT1 {
			for i := range c {
				c[i] = (p0[i] + 2*p1[i]) / 3
			}
		} else if t.fmt == texDXT1 {
			alpha = 0
		}
	}
	return [4]float32{alpha, c[0], c[1], c[2]}
}

func wrapCoord(i, n int, mode uint32) (int, bool) {
	switch mode {
	case 1: // wrap
		i %= n
		if i < 0 {
			i += n
		}
	case 2: // mirror
		p := 2 * n
		i %= p
		if i < 0 {
			i += p
		}
		if i >= n {
			i = p - 1 - i
		}
	case 4, 7: // border
		if i < 0 || i >= n {
			return 0, false
		}
	case 6, 8: // mirror once
		if i < 0 {
			i = -i - 1
		}
		if i >= n {
			i = n - 1
		}
	default: // clamp
		if i < 0 {
			i = 0
		} else if i >= n {
			i = n - 1
		}
	}
	return i, true
}

func (t *rsxTexture) fetch(x, y int) [4]float32 {
	xx, ok1 := wrapCoord(x, t.w, t.wrapS)
	yy, ok2 := wrapCoord(y, t.h, t.wrapT)
	if !ok1 || !ok2 {
		b := t.border
		return [4]float32{b[3], b[0], b[1], b[2]}
	}
	return t.texel(xx, yy)
}

// sample returns RGBA for coordinates (s, t).
func (t *rsxTexture) sample(s, tt float32) vec4 {
	if !t.ok {
		return vec4{0, 0, 0, 0}
	}
	u, v := s, tt
	if !t.unnorm {
		u *= float32(t.w)
		v *= float32(t.h)
	}
	if t.pix != nil {
		return t.sample8(u, v)
	}
	var argb [4]float32
	if t.magLinear || t.minLinear {
		u -= 0.5
		v -= 0.5
		x0 := int(math.Floor(float64(u)))
		y0 := int(math.Floor(float64(v)))
		fx, fy := u-float32(x0), v-float32(y0)
		a, b, c, d := t.fetch(x0, y0), t.fetch(x0+1, y0), t.fetch(x0, y0+1), t.fetch(x0+1, y0+1)
		for i := range argb {
			top := a[i] + (b[i]-a[i])*fx
			bot := c[i] + (d[i]-c[i])*fx
			argb[i] = top + (bot-top)*fy
		}
	} else {
		argb = t.fetch(int(math.Floor(float64(u))), int(math.Floor(float64(v))))
	}
	// channel remap (A, R, G, B order)
	if t.remap != 0xAAE4 && t.remap != 0 {
		var out [4]float32
		for ch := 0; ch < 4; ch++ {
			switch t.remap >> (8 + 2*ch) & 3 {
			case 0:
				out[ch] = 0
			case 1:
				out[ch] = 1
			default:
				out[ch] = argb[t.remap>>(2*ch)&3]
			}
		}
		argb = out
	}
	return vec4{argb[1], argb[2], argb[3], argb[0]}
}
