package n64

// RDP pixel pipeline: texture coordinates and LOD, texture sampling, the
// colour combiner, alpha compare, the blender, the z buffer and frame
// buffer writes. setupPipe decodes the modes once per primitive so the span
// loops only follow precomputed pointers.

import "encoding/binary"

type rdpPipe struct {
	cycle uint32

	persp, lodEn, detail, sharpen    bool
	tlut, tlutIA, filter, mid        bool
	bil0, bil1, convOne              bool
	forceBlend, alphaCvgSel, cvgXA   bool
	colorOnCvg, imageRead            bool
	zUpdate, zCompare, aa, zPrim     bool
	alphaCompare, ditherAlpha        bool
	zMode, cvgDest, rgbDither, bpp   uint32
	useTex0, useTex1, useShade, useZ bool
	useNoise, useLod, useTexN        bool

	// combiner inputs per cycle and channel: (a - b) * c + d
	ca, cb, cc, cd [2][4]*int32
	// blender: p * a + m * b
	bp, bm [2]*[4]int32
	ba, bb [2]*int32
	binv   [2]bool // b is 1 - a

	// registers the pointers above select from
	tex0, tex1, texN, shade, comb, cyc1 [4]int32 // texN: next pixel's TEX0
	mem, prim, env, blend, fog          [4]int32
	keyC, keyS, raw                     [4]int32
	alpha, memA                         int32
	noise, lodFrac, primLod             int32
	k4, k5, zero, one, full             int32
	k                                   [4]int32

	primZ, dz, dzEnc int32
	tx               [8]rdpTileX
}

var ditherMagic = [16]int32{0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0}
var ditherBayer = [16]int32{0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2}

func unpackColor(v uint32, c *[4]int32) {
	c[0], c[1], c[2], c[3] = int32(v>>24), int32(v>>16&0xff), int32(v>>8&0xff), int32(v&0xff)
}

func sext9(v uint64) int32 { return int32(uint32(v&0x1ff)<<23) >> 23 }

// setupPipe prepares the pixel pipeline for the next primitive.
func (d *RDP) setupPipe() {
	p := &d.pp
	mo := d.Modes
	bit := func(n uint) bool { return mo>>n&1 != 0 }
	p.cycle = uint32(mo>>52) & 3
	p.persp, p.detail, p.sharpen, p.lodEn = bit(51), bit(50), bit(49), bit(48)
	p.tlut, p.tlutIA, p.filter, p.mid = bit(47), bit(46), bit(45), bit(44)
	p.bil0, p.bil1, p.convOne = bit(43), bit(42), bit(41)
	p.rgbDither = uint32(mo>>38) & 3
	p.forceBlend, p.alphaCvgSel, p.cvgXA = bit(14), bit(13), bit(12)
	p.zMode, p.cvgDest = uint32(mo>>10)&3, uint32(mo>>8)&3
	p.colorOnCvg, p.imageRead, p.zUpdate, p.zCompare = bit(7), bit(6), bit(5), bit(4)
	p.aa, p.zPrim, p.ditherAlpha, p.alphaCompare = bit(3), bit(2), bit(1), bit(0)
	if d.sp.rect {
		p.persp = false
	}
	switch d.Color.Size {
	case 3:
		p.bpp = 4
	case 2:
		p.bpp = 2
	default:
		p.bpp = 1
	}

	unpackColor(d.Prim, &p.prim)
	unpackColor(d.Env, &p.env)
	unpackColor(d.Blend, &p.blend)
	unpackColor(d.Fog, &p.fog)
	p.primLod = int32(d.PrimLodFrac)
	cv := d.Convert
	p.k = [4]int32{sext9(cv >> 45), sext9(cv >> 36), sext9(cv >> 27), sext9(cv >> 18)}
	p.k4, p.k5 = sext9(cv>>9), sext9(cv)
	p.keyC = [4]int32{int32(d.KeyR >> 8 & 0xff), int32(d.KeyGB >> 24 & 0xff), int32(d.KeyGB >> 8 & 0xff), 0}
	p.keyS = [4]int32{int32(d.KeyR & 0xff), int32(d.KeyGB >> 16 & 0xff), int32(d.KeyGB & 0xff), 0}
	p.zero, p.one, p.full = 0, 256, 255
	p.primZ = int32(d.PrimZ&0x7fff) << 3
	p.lodFrac = 0xff // what LOD_FRACTION reads with LOD off

	for i := range d.Tiles {
		d.Tiles[i].prepare(&p.tx[i], p.tlut)
	}

	// z slope for decal/compare
	sp := &d.sp
	if p.zPrim {
		p.dz = int32(d.PrimDZ)
	} else {
		dzdx, dzdy := sp.dx[atZ], sp.dy[atZ]
		if dzdx < 0 {
			dzdx = -dzdx
		}
		if dzdy < 0 {
			dzdy = -dzdy
		}
		p.dz = int32((int64(dzdx) + int64(dzdy)) >> 16)
		if p.dz > 0xffff {
			p.dz = 0xffff
		}
	}
	p.dzEnc = 0
	for v := p.dz; v > 1 && p.dzEnc < 15; v >>= 1 {
		p.dzEnc++
	}

	if !d.dirty && p.ca[0][0] != nil {
		d.pipeUses()
		return
	}
	d.dirty = false
	cm := d.Combine
	f := func(sh uint, n uint) uint32 { return uint32(cm>>sh) & (1<<n - 1) }
	sa := [2]uint32{f(52, 4), f(37, 4)}
	mr := [2]uint32{f(47, 5), f(32, 5)}
	saA := [2]uint32{f(44, 3), f(21, 3)}
	mA := [2]uint32{f(41, 3), f(18, 3)}
	sb := [2]uint32{f(28, 4), f(24, 4)}
	ad := [2]uint32{f(15, 3), f(6, 3)}
	sbA := [2]uint32{f(12, 3), f(3, 3)}
	adA := [2]uint32{f(9, 3), f(0, 3)}
	for c := 0; c < 2; c++ {
		for ch := 0; ch < 3; ch++ {
			p.ca[c][ch] = p.ccInput(0, sa[c], ch)
			p.cb[c][ch] = p.ccInput(1, sb[c], ch)
			p.cc[c][ch] = p.ccInput(2, mr[c], ch)
			p.cd[c][ch] = p.ccInput(3, ad[c], ch)
		}
		p.ca[c][3] = p.ccAlpha(saA[c], false)
		p.cb[c][3] = p.ccAlpha(sbA[c], false)
		p.cc[c][3] = p.ccAlpha(mA[c], true)
		p.cd[c][3] = p.ccAlpha(adA[c], false)
	}
	// In the second cycle of 2-cycle mode TEX0 reads the texel fetched for
	// that cycle (the next tile) and TEX1 the next pixel's first texel.
	if p.cycle == 1 {
		for _, q := range []*[4]*int32{&p.ca[1], &p.cb[1], &p.cc[1], &p.cd[1]} {
			for ch := range q {
				q[ch] = p.swapTex(q[ch])
			}
		}
	}
	for c := 0; c < 2; c++ {
		sh := uint(2 - 2*c) // cycle 0 settings are the upper bit pair
		m1a := uint32(mo>>(28+sh)) & 3
		m1b := uint32(mo>>(24+sh)) & 3
		m2a := uint32(mo>>(20+sh)) & 3
		m2b := uint32(mo>>(16+sh)) & 3
		pix := &p.comb
		if c == 1 {
			pix = &p.cyc1
		}
		sel := func(v uint32) *[4]int32 {
			switch v {
			case 0:
				return pix
			case 1:
				return &p.mem
			case 2:
				return &p.blend
			}
			return &p.fog
		}
		p.bp[c], p.bm[c] = sel(m1a), sel(m2a)
		switch m1b {
		case 0:
			p.ba[c] = &p.alpha
		case 1:
			p.ba[c] = &p.fog[3]
		case 2:
			p.ba[c] = &p.shade[3]
		default:
			p.ba[c] = &p.zero
		}
		p.binv[c] = m2b == 0
		switch m2b {
		case 1:
			p.bb[c] = &p.memA
		case 2:
			p.bb[c] = &p.full
		default:
			p.bb[c] = &p.zero
		}
	}
	d.pipeUses()
}

// pipeUses works out which inputs the current modes read.
func (d *RDP) pipeUses() {
	p := &d.pp
	p.useTex0, p.useTex1, p.useShade, p.useNoise, p.useLod, p.useTexN = false, false, false, false, false, false
	first := 1
	if p.cycle == 1 {
		first = 0
	}
	for c := first; c < 2; c++ {
		for ch := 0; ch < 4; ch++ {
			for _, q := range [4]*int32{p.ca[c][ch], p.cb[c][ch], p.cc[c][ch], p.cd[c][ch]} {
				switch {
				case q == &p.tex0[0] || q == &p.tex0[1] || q == &p.tex0[2] || q == &p.tex0[3]:
					p.useTex0 = true
				case q == &p.tex1[0] || q == &p.tex1[1] || q == &p.tex1[2] || q == &p.tex1[3]:
					p.useTex1 = true
				case q == &p.texN[0] || q == &p.texN[1] || q == &p.texN[2] || q == &p.texN[3]:
					p.useTexN = true
				case q == &p.shade[0] || q == &p.shade[1] || q == &p.shade[2] || q == &p.shade[3]:
					p.useShade = true
				case q == &p.noise:
					p.useNoise = true
				case q == &p.lodFrac:
					p.useLod = true
				}
			}
		}
		if p.ba[c] == &p.shade[3] {
			p.useShade = true
		}
	}
	if p.useTex1 && p.cycle != 1 {
		p.useTex0 = true
	}
	p.useZ = p.zCompare || p.zUpdate
}

// ccInput picks an RGB combiner input; kind 0 = A, 1 = B, 2 = C, 3 = D.
func (p *rdpPipe) ccInput(kind int, v uint32, ch int) *int32 {
	switch v {
	case 0:
		return &p.comb[ch]
	case 1:
		return &p.tex0[ch]
	case 2:
		return &p.tex1[ch]
	case 3:
		return &p.prim[ch]
	case 4:
		return &p.shade[ch]
	case 5:
		return &p.env[ch]
	}
	switch kind {
	case 0:
		if v == 6 {
			return &p.one
		}
		if v == 7 {
			return &p.noise
		}
	case 1:
		if v == 6 {
			return &p.keyC[ch]
		}
		if v == 7 {
			return &p.k4
		}
	case 2:
		switch v {
		case 6:
			return &p.keyS[ch]
		case 7:
			return &p.comb[3]
		case 8:
			return &p.tex0[3]
		case 9:
			return &p.tex1[3]
		case 10:
			return &p.prim[3]
		case 11:
			return &p.shade[3]
		case 12:
			return &p.env[3]
		case 13:
			return &p.lodFrac
		case 14:
			return &p.primLod
		case 15:
			return &p.k5
		}
	case 3:
		if v == 6 {
			return &p.one
		}
	}
	return &p.zero
}

func (p *rdpPipe) swapTex(q *int32) *int32 {
	for i := range p.tex0 {
		if q == &p.tex0[i] {
			return &p.tex1[i]
		}
		if q == &p.tex1[i] {
			return &p.texN[i]
		}
	}
	return q
}

func (p *rdpPipe) ccAlpha(v uint32, mul bool) *int32 {
	switch v {
	case 0:
		if mul {
			return &p.lodFrac
		}
		return &p.comb[3]
	case 1:
		return &p.tex0[3]
	case 2:
		return &p.tex1[3]
	case 3:
		return &p.prim[3]
	case 4:
		return &p.shade[3]
	case 5:
		return &p.env[3]
	case 6:
		if mul {
			return &p.primLod
		}
		return &p.one
	}
	return &p.zero
}

// ccClamp folds a combiner result to 0-255 the way the 9-bit hardware does:
// small overflows saturate, large ones (and negatives) give 0.
func ccClamp(v int32) int32 {
	v &= 0x1ff
	if v&0x100 != 0 {
		if v&0x80 != 0 {
			return 0
		}
		return 255
	}
	return v
}

func (p *rdpPipe) combine(c int) {
	var o [4]int32
	a, b, m, ad := &p.ca[c], &p.cb[c], &p.cc[c], &p.cd[c]
	for i := 0; i < 4; i++ {
		o[i] = ccClamp(((*a[i]-*b[i])**m[i] + *ad[i]<<8 + 0x80) >> 8)
	}
	p.comb = o
}

// blendCycle runs one blender cycle into out.
func (p *rdpPipe) blendCycle(c int, out *[4]int32, divide bool) {
	P, M := p.bp[c], p.bm[c]
	a := *p.ba[c] >> 3
	var b int32
	if p.binv[c] {
		b = ^a & 31
	} else {
		b = *p.bb[c] >> 3
	}
	if divide {
		s := a + b + 1
		for i := 0; i < 3; i++ {
			out[i] = (P[i]*a + M[i]*(b+1)) / s
		}
		return
	}
	for i := 0; i < 3; i++ {
		v := (P[i]*a + M[i]*(b+1)) >> 5
		if v > 255 {
			v = 255
		}
		out[i] = v
	}
}

// texCoords turns interpolated S, T, W into 10.5 texture coordinates.
func (p *rdpPipe) texCoords(s, t, w int32) (int32, int32) {
	if !p.persp {
		return s >> 16, t >> 16
	}
	if w <= 0 {
		w = 1
	}
	inv := 32768.0 / float64(w)
	return perspClamp(float64(s) * inv), perspClamp(float64(t) * inv)
}

func perspClamp(v float64) int32 {
	if v >= 65535 {
		return 65535
	}
	if v <= -65536 {
		return -65536
	}
	i := int32(v)
	if float64(i) > v {
		i--
	}
	return i
}

// lod picks the tiles and LOD fraction for the pixel at v.
func (d *RDP) lod(v *[atN]int32, s, t int32) (uint32, uint32) {
	p, sp := &d.pp, &d.sp
	s1, t1 := p.texCoords(v[atS]+sp.dx[atS], v[atT]+sp.dx[atT], v[atW]+sp.dx[atW])
	s2, t2 := p.texCoords(v[atS]+sp.dy[atS], v[atT]+sp.dy[atT], v[atW]+sp.dy[atW])
	if p.cycle == 2 {
		s1, t1 = s+(s1-s)/4, t+(t1-t)/4
	}
	l := abs32(s1 - s)
	l = max(l, abs32(t1-t), abs32(s2-s), abs32(t2-t))
	if l > 0x7fff {
		l = 0x7fff
	}
	base := sp.tile
	if l < 32 {
		p.lodFrac = 0
		if p.sharpen || p.detail {
			p.lodFrac = (l << 3) & 0xff // magnified: fraction below one texel
			if p.sharpen {
				p.lodFrac = -p.lodFrac & 0x1ff
			}
		}
		if p.detail {
			return base, base + 1
		}
		return base, base + 1
	}
	lv := uint32(0)
	for x := l >> 5; x > 1; x >>= 1 {
		lv++
	}
	if lv >= sp.levels {
		lv = sp.levels
		p.lodFrac = 0xff
		if p.detail {
			return base + lv + 1, base + lv + 1
		}
		return base + lv, base + lv
	}
	p.lodFrac = ((l << 3) >> lv) & 0xff
	if p.detail {
		return base + lv + 1, base + lv + 2
	}
	return base + lv, base + lv + 1
}

func abs32(v int32) int32 {
	if v < 0 {
		return -v
	}
	return v
}

// z buffer format: 3-bit exponent, 11-bit mantissa, 2 bits of dz.
var zShift = [8]uint32{6, 5, 4, 3, 2, 1, 0, 0}
var zBase = [8]int32{0, 0x20000, 0x30000, 0x38000, 0x3c000, 0x3e000, 0x3f000, 0x3f800}

func zDecode(v uint32) int32 {
	e := v >> 13 & 7
	return int32((v>>2)&0x7ff)<<zShift[e] + zBase[e]
}

func zEncode(z int32) uint32 {
	e := uint32(0)
	for e < 7 && z >= zBase[e+1] {
		e++
	}
	return e<<13 | uint32((z-zBase[e])>>zShift[e])&0x7ff<<2
}

func (d *RDP) noise() int32 {
	n := d.Noise
	n ^= n << 13
	n ^= n >> 17
	n ^= n << 5
	d.Noise = n
	return int32(n)
}

// drawSpan draws pixels x0..x1 of line y; at holds the attributes at x0.
func (d *RDP) drawSpan(y, x0, x1 int32, at *[atN]int32) {
	p := &d.pp
	switch p.cycle {
	case 3:
		d.fillSpan(y, x0, x1)
		return
	case 2:
		d.copySpan(y, x0, x1, at)
		return
	}
	sp := &d.sp
	rd := d.m.rdram
	fl, fr := sp.fullRange()
	v := *at
	dx := sp.dx
	two := p.cycle == 1
	width := d.Color.Width
	row := uint32(y) * width
	dith := &ditherMagic
	if p.rgbDither == 1 {
		dith = &ditherBayer
	}
	dithRow := (y & 3) << 2
	for x := x0; x <= x1; x++ {
		if x != x0 {
			for i := range v {
				v[i] += dx[i]
			}
		}
		cvg := int32(8)
		if x < fl || x > fr {
			cvg = sp.coverage(x)
			if cvg == 0 {
				continue
			}
		}
		idx := row + uint32(x)

		// depth
		var z int32
		var za uint32
		if p.useZ {
			if p.zPrim {
				z = p.primZ
			} else {
				z = v[atZ] >> 13
				if z < 0 {
					z = 0
				} else if z > 0x3ffff {
					z = 0x3ffff
				}
			}
			za = (d.ZAddr + idx*2) & (rdramSize - 2)
			if p.zCompare {
				old := uint32(rd[za])<<8 | uint32(rd[za+1])
				oz := zDecode(old)
				if !d.zTest(z, oz, int32(old&3)<<2) {
					continue
				}
			}
		}

		// texture
		if p.useTex0 || p.useTex1 || p.useTexN || p.useLod {
			s, t := p.texCoords(v[atS], v[atT], v[atW])
			t0, t1 := sp.tile, sp.tile+1
			if p.lodEn {
				t0, t1 = d.lod(&v, s, t)
			}
			if p.useTex0 {
				d.texel(&p.tx[t0&7], s, t, p.bil0, &p.tex0)
			}
			if two && p.useTex1 {
				if !p.bil1 && p.convOne {
					d.convert(&p.raw, &p.tex1)
				} else {
					d.texel(&p.tx[t1&7], s, t, p.bil1, &p.tex1)
				}
			} else if !two && p.useTex1 {
				// 1-cycle TEX1 is the next pixel's TEX0
				s1, t1n := p.texCoords(v[atS]+dx[atS], v[atT]+dx[atT], v[atW]+dx[atW])
				d.texel(&p.tx[t0&7], s1, t1n, p.bil0, &p.tex1)
			}
			if two && p.useTexN {
				s1, t1n := p.texCoords(v[atS]+dx[atS], v[atT]+dx[atT], v[atW]+dx[atW])
				d.texel(&p.tx[t0&7], s1, t1n, p.bil0, &p.texN)
			}
		}
		if p.useShade {
			p.shade[0] = ccClamp(v[atR] >> 16)
			p.shade[1] = ccClamp(v[atG] >> 16)
			p.shade[2] = ccClamp(v[atB] >> 16)
			p.shade[3] = ccClamp(v[atA] >> 16)
		}
		if p.useNoise {
			p.noise = (d.noise()&7)<<6 | 0x20
		}

		// combiner
		if two {
			p.combine(0)
		}
		p.combine(1)

		// coverage and alpha
		alpha := p.comb[3]
		if p.cvgXA {
			cvg = (cvg * (alpha + 1)) >> 8
			if cvg == 0 {
				continue
			}
		}
		if p.alphaCvgSel {
			alpha = cvg << 5
			if alpha > 255 {
				alpha = 255
			}
		}
		p.alpha = alpha
		if p.alphaCompare {
			thr := p.blend[3]
			if p.ditherAlpha {
				thr = d.noise() & 0xff
			}
			if alpha < thr {
				continue
			}
		}

		// memory colour
		ca := (d.Color.Addr + idx*p.bpp) & (rdramSize - 1)
		memCvg := int32(7)
		if p.imageRead {
			switch p.bpp {
			case 2:
				ca &^= 1
				c := uint32(rd[ca])<<8 | uint32(rd[ca+1])
				rgba16(c, &p.mem)
				memCvg = (p.mem[3] >> 5) & 7
			case 4:
				ca &^= 3
				p.mem[0], p.mem[1], p.mem[2], p.mem[3] = int32(rd[ca]), int32(rd[ca+1]), int32(rd[ca+2]), int32(rd[ca+3])
				memCvg = p.mem[3] >> 5
			default:
				c := int32(rd[ca])
				p.mem = [4]int32{c, c, c, 0xe0}
			}
			p.memA = memCvg << 5
		}

		// blender
		var out [4]int32
		blend := p.forceBlend || (p.aa && cvg < 8)
		if two {
			p.blendCycle(0, &p.cyc1, !p.forceBlend)
			if blend {
				p.blendCycle(1, &out, !p.forceBlend)
			} else {
				out = *p.bp[1]
			}
		} else if blend {
			p.blendCycle(0, &out, !p.forceBlend)
		} else {
			out = *p.bp[0]
		}

		// coverage write
		sum := memCvg + cvg
		if !p.imageRead {
			sum = cvg - 1
		}
		var ncvg int32
		switch p.cvgDest {
		case 0:
			ncvg = min(sum, 7)
		case 1:
			ncvg = sum & 7
		case 2:
			ncvg = 7
		default:
			ncvg = memCvg
		}
		writeColor := !p.colorOnCvg || sum > 7

		switch p.bpp {
		case 2:
			ca &^= 1
			r, g, b := out[0], out[1], out[2]
			if p.rgbDither < 2 {
				dv := dith[dithRow|x&3]
				if r&7 > dv && r < 248 {
					r += 8
				}
				if g&7 > dv && g < 248 {
					g += 8
				}
				if b&7 > dv && b < 248 {
					b += 8
				}
			}
			c := uint32(ncvg>>2) & 1
			if writeColor {
				c |= uint32(r>>3)<<11 | uint32(g>>3)<<6 | uint32(b>>3)<<1
			} else {
				c |= uint32(rd[ca])<<8 | uint32(rd[ca+1])&0xfe
			}
			rd[ca], rd[ca+1] = byte(c>>8), byte(c)
		case 4:
			ca &^= 3
			if writeColor {
				rd[ca], rd[ca+1], rd[ca+2] = byte(out[0]), byte(out[1]), byte(out[2])
			}
			rd[ca+3] = byte(ncvg << 5)
		default:
			if writeColor {
				rd[ca] = byte(out[0])
			}
		}
		if p.zUpdate {
			zv := zEncode(z) | uint32(p.dzEnc>>2)
			rd[za], rd[za+1] = byte(zv>>8), byte(zv)
		}
	}
}

// texel samples one tile for the pixel, with filtering or YUV conversion.
func (d *RDP) texel(x *rdpTileX, s, t int32, bilerp bool, out *[4]int32) {
	p := &d.pp
	if bilerp {
		d.sample(x, s, t, p.filter, p.mid, p.tlutIA, out)
		p.raw = *out
		return
	}
	d.sample(x, s, t, false, false, p.tlutIA, &p.raw)
	d.convert(&p.raw, out)
}

// zTest compares a new depth with the stored one for the current z mode.
func (d *RDP) zTest(z, oz, odz int32) bool {
	p := &d.pp
	if p.zMode == 3 { // decal
		dz := max(int32(1)<<p.dzEnc, int32(1)<<odz) << 3
		diff := z - oz
		return diff <= dz && diff >= -dz && oz != 0x3ffff
	}
	return z < oz || oz == 0x3ffff
}

func (d *RDP) fillSpan(y, x0, x1 int32) {
	rd := d.m.rdram
	row := uint32(y) * d.Color.Width
	f := d.Fill
	switch d.pp.bpp {
	case 2:
		for x := x0; x <= x1; x++ {
			a := (d.Color.Addr + (row+uint32(x))*2) & (rdramSize - 2)
			v := f >> (16 - 16*(uint32(x)&1))
			rd[a], rd[a+1] = byte(v>>8), byte(v)
		}
	case 4:
		for x := x0; x <= x1; x++ {
			a := (d.Color.Addr + (row+uint32(x))*4) & (rdramSize - 4)
			binary.BigEndian.PutUint32(rd[a:], f)
		}
	default:
		for x := x0; x <= x1; x++ {
			a := (d.Color.Addr + row + uint32(x)) & (rdramSize - 1)
			rd[a] = byte(f >> (24 - 8*(uint32(x)&3)))
		}
	}
}

func (d *RDP) copySpan(y, x0, x1 int32, at *[atN]int32) {
	p, sp := &d.pp, &d.sp
	rd := d.m.rdram
	row := uint32(y) * d.Color.Width
	tx := &p.tx[sp.tile]
	s, t := at[atS], at[atT]
	dsdx, dtdx := sp.dx[atS]>>2, sp.dx[atT]>>2
	var c [4]int32
	for x := x0; x <= x1; x, s, t = x+1, s+dsdx, t+dtdx {
		d.sample(tx, s>>16, t>>16, false, false, p.tlutIA, &c)
		if p.alphaCompare && c[3] == 0 {
			continue
		}
		switch p.bpp {
		case 2:
			a := (d.Color.Addr + (row+uint32(x))*2) & (rdramSize - 2)
			v := uint32(c[0]>>3)<<11 | uint32(c[1]>>3)<<6 | uint32(c[2]>>3)<<1 | uint32(c[3]>>7)
			rd[a], rd[a+1] = byte(v>>8), byte(v)
		case 4:
			a := (d.Color.Addr + (row+uint32(x))*4) & (rdramSize - 4)
			rd[a], rd[a+1], rd[a+2], rd[a+3] = byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3])
		default:
			a := (d.Color.Addr + row + uint32(x)) & (rdramSize - 1)
			rd[a] = byte(c[0])
		}
	}
}
