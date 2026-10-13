package n64

// RDP texture memory: the load commands that fill the 4 KB TMEM from RDRAM,
// and texel fetching, decoding and filtering for the pixel pipeline.
//
// TMEM layout: byte addresses 0x000-0xfff. Rows of a tile start every
// Line*8 bytes; on odd rows the two 32-bit halves of every 64-bit word are
// swapped. 32-bit RGBA texels are split, red/green in the low half and
// blue/alpha at the same offset in the high half (0x800). YUV texels are
// split the same way one byte each: U/V alternating in the low half, Y in
// the high half. Palettes (TLUT) live in the high half, each 16-bit entry
// repeated in all four 16-bit lanes of its 64-bit word.

// texel decoders
const (
	tkRGBA16 = iota
	tkRGBA32
	tkYUV
	tkIA16
	tkIA8
	tkIA4
	tkI8
	tkI4
	tkCI8 // index shown as intensity (TLUT off)
	tkCI4
	tkPal8 // 8-bit index through the palette
	tkPal4
)

// rdpTileX is a tile descriptor turned into what the sampler needs.
type rdpTileX struct {
	kind           int
	base, stride   uint32 // TMEM byte address and row stride
	pal            uint32 // palette number << 4
	sl, tl         int32  // tile origin in 10.5
	maxS, maxT     int32  // clamp limits (texels)
	clampS, clampT bool
	maskS, maskT   int32 // wrap masks, -1 for none
	mirS, mirT     uint32
	mbitS, mbitT   uint32 // mask width (mirror test bit)
	shS, shT       uint32
}

func (t *rdpTile) prepare(x *rdpTileX, tlut bool) {
	switch t.Size {
	case 0:
		switch {
		case t.Fmt == 3:
			x.kind = tkIA4
		case t.Fmt == 4:
			x.kind = tkI4
		default:
			x.kind = tkCI4
		}
		if tlut {
			x.kind = tkPal4
		}
	case 1:
		switch {
		case t.Fmt == 3:
			x.kind = tkIA8
		case t.Fmt == 4:
			x.kind = tkI8
		default:
			x.kind = tkCI8
		}
		if tlut {
			x.kind = tkPal8
		}
	case 2:
		switch t.Fmt {
		case 1:
			x.kind = tkYUV
		case 3, 4:
			x.kind = tkIA16
		default:
			x.kind = tkRGBA16
		}
	default:
		x.kind = tkRGBA32
	}
	x.base = t.Tmem * 8
	x.stride = t.Line * 8
	x.pal = t.Pal << 4
	x.sl = int32(t.SL) << 3
	x.tl = int32(t.TL) << 3
	x.maxS = int32(t.SH>>2) - int32(t.SL>>2)
	x.maxT = int32(t.TH>>2) - int32(t.TL>>2)
	x.clampS = t.ClampS != 0 || t.MaskS == 0
	x.clampT = t.ClampT != 0 || t.MaskT == 0
	ms, mt := t.MaskS, t.MaskT
	if ms > 10 {
		ms = 10
	}
	if mt > 10 {
		mt = 10
	}
	x.maskS, x.maskT = -1, -1
	if ms != 0 {
		x.maskS = 1<<ms - 1
	}
	if mt != 0 {
		x.maskT = 1<<mt - 1
	}
	x.mbitS, x.mbitT = ms, mt
	x.mirS, x.mirT = t.MirrorS, t.MirrorT
	x.shS, x.shT = t.ShiftS, t.ShiftT
}

// coord applies a tile's shift and origin to a 10.5 coordinate.
func texShift(v int32, sh uint32, origin int32) int32 {
	if sh != 0 {
		if sh <= 10 {
			v >>= sh
		} else {
			v <<= 16 - sh
		}
	}
	return v - origin
}

// wrap applies mirroring and masking to an integer texel coordinate.
func texWrap(i int32, mask int32, mir, bit uint32) int32 {
	if mask < 0 {
		return i
	}
	if mir != 0 && (i>>bit)&1 != 0 {
		i = ^i
	}
	return i & mask
}

// texelAt fetches and decodes one texel at integer texel coordinates
// (already clamped and wrapped) into r, g, b, a.
func (d *RDP) texelAt(x *rdpTileX, s, t int32, tlutIA bool, out *[4]int32) {
	tm := &d.Tmem
	row := x.base + uint32(t)*x.stride
	odd := uint32(t&1) << 2
	switch x.kind {
	case tkRGBA16:
		a := (row + uint32(s)*2) ^ odd
		v := uint32(tm[a&0xffe])<<8 | uint32(tm[(a+1)&0xfff])
		rgba16(v, out)
	case tkRGBA32:
		a := ((row + uint32(s)*2) ^ odd) & 0x7fe
		out[0], out[1] = int32(tm[a]), int32(tm[a+1])
		out[2], out[3] = int32(tm[a|0x800]), int32(tm[(a+1)|0x800])
	case tkYUV:
		a := ((row + uint32(s)) ^ odd) & 0x7ff
		out[0] = int32(tm[a&^1])
		out[1] = int32(tm[a|1])
		out[2] = int32(tm[a|0x800])
		out[3] = out[2]
	case tkIA16:
		a := (row + uint32(s)*2) ^ odd
		i := int32(tm[a&0xffe])
		out[0], out[1], out[2], out[3] = i, i, i, int32(tm[(a+1)&0xfff])
	case tkIA8:
		v := int32(tm[((row+uint32(s))^odd)&0xfff])
		i := (v >> 4) * 17
		out[0], out[1], out[2], out[3] = i, i, i, (v&15)*17
	case tkIA4:
		v := int32(tm[((row+uint32(s)>>1)^odd)&0xfff])
		if s&1 == 0 {
			v >>= 4
		}
		i := (v >> 1) & 7
		i = i<<5 | i<<2 | i>>1
		out[0], out[1], out[2], out[3] = i, i, i, -(v&1)&0xff
	case tkI8, tkCI8:
		v := int32(tm[((row+uint32(s))^odd)&0xfff])
		out[0], out[1], out[2], out[3] = v, v, v, v
	case tkI4:
		v := int32(tm[((row+uint32(s)>>1)^odd)&0xfff])
		if s&1 == 0 {
			v >>= 4
		}
		v = (v & 15) * 17
		out[0], out[1], out[2], out[3] = v, v, v, v
	case tkCI4:
		v := uint32(tm[((row+uint32(s)>>1)^odd)&0xfff])
		if s&1 == 0 {
			v >>= 4
		}
		i := int32(x.pal | v&15)
		out[0], out[1], out[2], out[3] = i, i, i, i
	case tkPal8:
		i := uint32(tm[((row+uint32(s))^odd)&0x7ff])
		d.palette(i, tlutIA, out)
	case tkPal4:
		v := uint32(tm[((row+uint32(s)>>1)^odd)&0x7ff])
		if s&1 == 0 {
			v >>= 4
		}
		d.palette(x.pal|v&15, tlutIA, out)
	}
}

func (d *RDP) palette(i uint32, ia bool, out *[4]int32) {
	a := 0x800 | i<<3
	v := uint32(d.Tmem[a])<<8 | uint32(d.Tmem[a+1])
	if ia {
		c := int32(v >> 8)
		out[0], out[1], out[2], out[3] = c, c, c, int32(v&0xff)
		return
	}
	rgba16(v, out)
}

func rgba16(v uint32, out *[4]int32) {
	r, g, b := int32(v>>11)&31, int32(v>>6)&31, int32(v>>1)&31
	out[0] = r<<3 | r>>2
	out[1] = g<<3 | g>>2
	out[2] = b<<3 | b>>2
	out[3] = -int32(v&1) & 0xff
}

// sample runs a 10.5 texture coordinate pair through a tile and the texture
// filter. filter selects bilinear (3-point) filtering; mid enables the
// "middle of four texels" average.
func (d *RDP) sample(x *rdpTileX, s, t int32, filter, mid, tlutIA bool, out *[4]int32) {
	s = texShift(s, x.shS, x.sl)
	t = texShift(t, x.shT, x.tl)
	si, ti := s>>5, t>>5
	fs, ft := s&31, t&31
	if x.clampS {
		if si >= x.maxS {
			si, fs = x.maxS, 0
		} else if s < 0 {
			si, fs = 0, 0
		}
	}
	if x.clampT {
		if ti >= x.maxT {
			ti, ft = x.maxT, 0
		} else if t < 0 {
			ti, ft = 0, 0
		}
	}
	if !filter || (fs == 0 && ft == 0) {
		d.texelAt(x, texWrap(si, x.maskS, x.mirS, x.mbitS), texWrap(ti, x.maskT, x.mirT, x.mbitT), tlutIA, out)
		return
	}
	s0 := texWrap(si, x.maskS, x.mirS, x.mbitS)
	s1 := texWrap(si+1, x.maskS, x.mirS, x.mbitS)
	t0 := texWrap(ti, x.maskT, x.mirT, x.mbitT)
	t1 := texWrap(ti+1, x.maskT, x.mirT, x.mbitT)
	var a, b, c [4]int32
	if mid && fs == 16 && ft == 16 {
		var e [4]int32
		d.texelAt(x, s0, t0, tlutIA, &a)
		d.texelAt(x, s1, t0, tlutIA, &b)
		d.texelAt(x, s0, t1, tlutIA, &c)
		d.texelAt(x, s1, t1, tlutIA, &e)
		for i := range out {
			out[i] = (a[i] + b[i] + c[i] + e[i] + 2) >> 2
		}
		return
	}
	if fs+ft < 32 {
		d.texelAt(x, s0, t0, tlutIA, &a)
		d.texelAt(x, s1, t0, tlutIA, &b)
		d.texelAt(x, s0, t1, tlutIA, &c)
	} else {
		d.texelAt(x, s1, t1, tlutIA, &a)
		d.texelAt(x, s0, t1, tlutIA, &b)
		d.texelAt(x, s1, t0, tlutIA, &c)
		fs, ft = 32-fs, 32-ft
	}
	for i := range out {
		out[i] = a[i] + ((b[i]-a[i])*fs+(c[i]-a[i])*ft+16)>>5
	}
}

// convert is the texture filter's YUV to RGB step: in is (U, V, Y, Y).
func (d *RDP) convert(in, out *[4]int32) {
	u, v, y := int32(int8(in[0]^0x80)), int32(int8(in[1]^0x80)), in[2]
	k := &d.pp.k
	out[0] = clamp255(y + (k[0]*v)>>7)
	out[1] = clamp255(y + (k[1]*u+k[2]*v)>>7)
	out[2] = clamp255(y + (k[3]*u)>>7)
	out[3] = y
}

func clamp255(v int32) int32 {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return v
}

// ---------------------------------------------------------------------
// loads
// ---------------------------------------------------------------------

func (d *RDP) rd8(a uint32) byte { return d.m.rdram[a&(rdramSize-1)] }

// texel bytes per Set Texture Image size, in half bytes
func texHalfBytes(size uint32) uint32 { return 1 << size }

func (d *RDP) loadTile(w uint64) {
	t := &d.Tiles[(w>>24)&7]
	t.SL, t.TL = uint32(w>>44)&0xfff, uint32(w>>32)&0xfff
	t.SH, t.TH = uint32(w>>12)&0xfff, uint32(w)&0xfff
	s0, t0 := t.SL>>2, t.TL>>2
	s1, t1 := t.SH>>2, t.TH>>2
	if s1 < s0 || t1 < t0 {
		return
	}
	img := &d.Tex
	n := s1 - s0 + 1
	hb := texHalfBytes(img.Size)
	for row := t0; row <= t1; row++ {
		r := row - t0
		dst := t.Tmem*8 + r*t.Line*8
		odd := (r & 1) << 2
		src := img.Addr + ((row*img.Width+s0)*hb)/2
		switch {
		case img.Size == 3:
			for i := uint32(0); i < n; i++ {
				p := src + i*4
				a := ((dst + i*2) ^ odd) & 0x7fe
				d.Tmem[a], d.Tmem[a+1] = d.rd8(p), d.rd8(p+1)
				d.Tmem[a|0x800], d.Tmem[(a+1)|0x800] = d.rd8(p+2), d.rd8(p+3)
			}
		case t.Fmt == 1 && img.Size == 2:
			for i := uint32(0); i < n; i++ {
				p := src + i*2
				a := ((dst + i) ^ odd) & 0x7ff
				d.Tmem[a], d.Tmem[a|0x800] = d.rd8(p), d.rd8(p+1)
			}
		default:
			nb := (n*hb + 1) / 2
			nb = (nb + 7) &^ 7
			for i := uint32(0); i < nb; i++ {
				d.Tmem[((dst+i)^odd)&0xfff] = d.rd8(src + i)
			}
		}
	}
}

func (d *RDP) loadBlock(w uint64) {
	t := &d.Tiles[(w>>24)&7]
	sl, tl := uint32(w>>44)&0xfff, uint32(w>>32)&0xfff
	sh, dxt := uint32(w>>12)&0xfff, uint32(w)&0xfff
	t.SL, t.TL, t.SH, t.TH = sl, tl, sh, dxt
	if sh < sl {
		return
	}
	img := &d.Tex
	hb := texHalfBytes(img.Size)
	src := img.Addr + ((tl*img.Width+sl)*hb)/2
	words := ((sh-sl+1)*hb/2 + 7) / 8
	acc := uint32(0)
	for i := uint32(0); i < words; i++ {
		odd := (acc >> 9) & 4 // bit 11 of the line counter
		acc += dxt
		p := src + i*8
		switch {
		case img.Size == 3:
			for k := uint32(0); k < 2; k++ {
				a := ((t.Tmem*8 + (i*2+k)*2) ^ odd) & 0x7fe
				q := p + k*4
				d.Tmem[a], d.Tmem[a+1] = d.rd8(q), d.rd8(q+1)
				d.Tmem[a|0x800], d.Tmem[(a+1)|0x800] = d.rd8(q+2), d.rd8(q+3)
			}
		case t.Fmt == 1 && img.Size == 2:
			for k := uint32(0); k < 4; k++ {
				a := ((t.Tmem*8 + i*4 + k) ^ odd) & 0x7ff
				d.Tmem[a], d.Tmem[a|0x800] = d.rd8(p+k*2), d.rd8(p+k*2+1)
			}
		default:
			base := t.Tmem*8 + i*8
			for k := uint32(0); k < 8; k++ {
				d.Tmem[((base+k)^odd)&0xfff] = d.rd8(p + k)
			}
		}
	}
}

func (d *RDP) loadTLUT(w uint64) {
	t := &d.Tiles[(w>>24)&7]
	sl, tl := uint32(w>>44)&0xfff, uint32(w>>32)&0xfff
	sh, th := uint32(w>>12)&0xfff, uint32(w)&0xfff
	t.SL, t.TL, t.SH, t.TH = sl, tl, sh, th
	s0, s1 := sl>>2, sh>>2
	if s1 < s0 {
		return
	}
	img := &d.Tex
	src := img.Addr + (tl>>2)*img.Width*2 + s0*2
	for i := uint32(0); i <= s1-s0; i++ {
		hi, lo := d.rd8(src+i*2), d.rd8(src+i*2+1)
		a := ((t.Tmem + i) & 0x1ff) * 8
		for l := uint32(0); l < 8; l += 2 {
			d.Tmem[a+l], d.Tmem[a+l+1] = hi, lo
		}
	}
}
