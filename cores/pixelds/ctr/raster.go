package ctr

import (
	"math"
	"runtime"
	"sync"
)

// The PICA200's rasterizer: clipping in clip space, the viewport,
// culling, triangle setup with perspective-correct interpolation, the
// three texture units, the six colour-combiner (TEV) stages, and the
// per-pixel alpha, stencil and depth tests, blending or logic ops and the
// write into the tiled colour and depth buffers.

// clip planes: a vertex is inside when the distance is ≥ 0.
var clipPlanes = [7]func(p *[24]float32) float32{
	func(p *[24]float32) float32 { return p[3] - p[0] },
	func(p *[24]float32) float32 { return p[3] + p[0] },
	func(p *[24]float32) float32 { return p[3] - p[1] },
	func(p *[24]float32) float32 { return p[3] + p[1] },
	func(p *[24]float32) float32 { return -p[2] },
	func(p *[24]float32) float32 { return p[3] + p[2] },
	func(p *[24]float32) float32 { return p[3] - 1e-5 },
}

// triangle clips a triangle from the assembler and draws what is left.
func (g *gpu) triangle(a, b, c *gpuVertex) {
	poly := make([]gpuVertex, 0, 12)
	poly = append(poly, *a, *b, *c)
	inside := true
	for _, p := range [3]*gpuVertex{a, b, c} {
		for _, f := range clipPlanes {
			if f(&p.a) < 0 {
				inside = false
			}
		}
	}
	if !inside {
		for _, f := range clipPlanes {
			if len(poly) < 3 {
				return
			}
			out := make([]gpuVertex, 0, len(poly)+2)
			for i := range poly {
				v0, v1 := &poly[i], &poly[(i+1)%len(poly)]
				d0, d1 := f(&v0.a), f(&v1.a)
				if d0 >= 0 {
					out = append(out, *v0)
				}
				if (d0 >= 0) != (d1 >= 0) {
					t := d0 / (d0 - d1)
					var n gpuVertex
					for k := range n.a {
						n.a[k] = v0.a[k] + (v1.a[k]-v0.a[k])*t
					}
					out = append(out, n)
				}
			}
			poly = out
		}
		if len(poly) < 3 {
			return
		}
	}
	vp := g.viewport()
	for i := range poly {
		vp.project(&poly[i])
	}
	st := g.drawState()
	if st == nil {
		return
	}
	for i := 1; i+1 < len(poly); i++ {
		g.raster(st, &poly[0], &poly[i], &poly[i+1])
	}
}

type viewport struct{ sx, sy, ox, oy float32 }

func (g *gpu) viewport() viewport {
	off := g.reg[0x068]
	return viewport{
		sx: f24(g.reg[0x041]), sy: f24(g.reg[0x043]),
		ox: float32(int32(off<<22) >> 22), oy: float32(int32(off<<6) >> 22),
	}
}

func (vp viewport) project(v *gpuVertex) {
	iw := 1 / v.a[3]
	v.invW = iw
	// 4 bits below the pixel, as the chip keeps
	v.sx = float32(math.Round(float64((v.a[0]*iw+1)*vp.sx+vp.ox)*16) / 16)
	v.sy = float32(math.Round(float64((v.a[1]*iw+1)*vp.sy+vp.oy)*16) / 16)
	v.sz = v.a[2] * iw
}

// ---------------- the draw state ----------------

type texUnit struct {
	on          bool
	data        []byte
	w, h        int
	format      uint32
	wrapS       uint32
	wrapT       uint32
	linear      bool
	border      [4]uint8
	typ         uint32    // unit 0: 0 2D, 1 cube, 2 shadow, 3 projected, 4 shadow cube
	faces       [6][]byte // cube maps: +x, -x, +y, -y, +z, -z
	shadowOrtho bool
	shadowBias  int32
}

type rasterState struct {
	color, depth  []byte
	fbW, fbH      int
	cFmt, dFmt    uint32
	cBpp, dBpp    int
	tex           [3]texUnit
	pt            *procTex
	tevPass       [6]bool
	tcUsed        uint32 // texture coordinates read: bit 0-2
	tevConst      [6][4]uint8
	light         *lightState
	fog, fogFlip  bool
	fogColor      [3]float32
	fogLUT        *[128]uint32
	tex2UsesTC1   bool
	tev           [6][5]uint32
	bufInit       [4]uint8
	bufUpdate     uint32
	cull          uint32
	scissorMode   uint32
	scX0, scY0    int
	scX1, scY1    int
	zScale, zOff  float32
	wBuffer       bool
	alphaTest     bool
	alphaFunc     uint32
	alphaRef      uint8
	stencil       bool
	stFunc        uint32
	stRef, stMask uint8
	stWMask       uint8
	stFail, stZF  uint32
	stZP          uint32
	depthTest     bool
	depthFunc     uint32
	depthWrite    bool
	colorMask     uint32
	colorWrite    bool
	colorRead     bool
	blend         bool
	blendCfg      uint32
	blendColor    [4]uint8
	logicOp       uint32
	stencilWrite  bool
	depthReadOK   bool
}

// drawState reads the registers the rasterizer needs, once per triangle.
func (g *gpu) drawState() *rasterState {
	r := &g.reg
	s := &rasterState{}
	dims := r[0x11e]
	s.fbW, s.fbH = int(dims&0x7ff), int(dims>>12&0x3ff)+1
	if s.fbW == 0 {
		return nil
	}
	s.cFmt = r[0x117] >> 16 & 7
	s.cBpp = [8]int{4, 3, 2, 2, 2, 2, 2, 2}[s.cFmt]
	s.dFmt = r[0x116] & 3
	s.dBpp = [4]int{2, 2, 3, 4}[s.dFmt]
	mem := g.m.mem
	s.color = mem.physSlice(r[0x11d]<<3, uint32(s.fbW*s.fbH*s.cBpp))
	s.depth = mem.physSlice(r[0x11c]<<3, uint32(s.fbW*s.fbH*s.dBpp))
	texCfg := r[0x080]
	s.tex2UsesTC1 = texCfg>>13&1 != 0
	for i, base := range [3]uint32{0x081, 0x091, 0x099} {
		t := &s.tex[i]
		t.on = texCfg>>i&1 != 0
		if !t.on {
			continue
		}
		d := r[base+1]
		t.w, t.h = int(d>>16&0x7ff), int(d&0x7ff)
		p := r[base+2]
		t.linear = p>>1&1 != 0
		t.wrapT, t.wrapS = p>>8&7, p>>12&7
		var tfmt uint32
		if i == 0 {
			tfmt = r[0x08e] & 0xf
			t.typ = p >> 28 & 7
			if t.typ == 5 { // off
				t.on = false
				continue
			}
			sh := r[0x08b]
			t.shadowOrtho, t.shadowBias = sh&1 != 0, int32(sh>>1&0x7fffff)
		} else {
			tfmt = r[base+5] & 0xf
		}
		t.format = tfmt
		bc := r[base]
		t.border = [4]uint8{uint8(bc), uint8(bc >> 8), uint8(bc >> 16), uint8(bc >> 24)}
		size := texBytes(tfmt, t.w, t.h)
		t.data = mem.physSlice(r[base+4]<<3, uint32(size))
		if t.data == nil || t.w == 0 || t.h == 0 {
			t.on = false
		}
		if i == 0 && (t.typ == 1 || t.typ == 4) {
			main := r[0x085]
			for f := range t.faces {
				a := main
				if f > 0 {
					a = main&^0x3fffff | r[0x085+uint32(f)]&0x3fffff
				}
				if t.faces[f] = mem.physSlice(a<<3, uint32(size)); t.faces[f] == nil {
					t.on = false
				}
			}
		}
	}
	s.light = g.lightState()
	if texCfg>>10&1 != 0 {
		s.pt = g.procTexState()
	}
	var texUsed uint32
	for i, base := range [6]uint32{0xc0, 0xc8, 0xd0, 0xd8, 0xf0, 0xf8} {
		t := &s.tev[i]
		copy(t[:], r[base:base+5])
		k := t[3]
		s.tevConst[i] = [4]uint8{uint8(k), uint8(k >> 8), uint8(k >> 16), uint8(k >> 24)}
		s.tevPass[i] = t[0]&0xf == 0xf && t[0]>>16&0xf == 0xf && t[1]&0xf == 0 && t[1]>>12&7 == 0 &&
			t[2]&0xf == 0 && t[2]>>16&0xf == 0 && t[4]&3 == 0 && t[4]>>16&3 == 0
		if !s.tevPass[i] {
			for k := 0; k < 3; k++ {
				texUsed |= 1 << (t[0] >> (4 * k) & 0xf)
				texUsed |= 1 << (t[0] >> (16 + 4*k) & 0xf)
			}
		}
	}
	if s.light != nil && s.light.bumpMode != 0 {
		texUsed |= 1 << (3 + s.light.bumpTex)
	}
	if s.pt != nil {
		if texUsed>>6&1 == 0 {
			s.pt = nil
		} else {
			s.tcUsed |= 1 << s.pt.coord
		}
	}
	for i := range s.tex {
		if texUsed>>(3+i)&1 == 0 {
			s.tex[i].on = false
		}
		if s.tex[i].on {
			if i == 2 && s.tex2UsesTC1 {
				s.tcUsed |= 2
			} else {
				s.tcUsed |= 1 << i
			}
		}
	}
	bc := r[0x0fd]
	s.bufInit = [4]uint8{uint8(bc), uint8(bc >> 8), uint8(bc >> 16), uint8(bc >> 24)}
	s.bufUpdate = r[0x0e0] >> 8 & 0xff
	switch r[0x0e0] & 7 {
	case 5:
		s.fog = true
		s.fogFlip = r[0x0e0]>>16&1 != 0
		fc := r[0x0e1]
		s.fogColor = [3]float32{float32(fc & 0xff), float32(fc >> 8 & 0xff), float32(fc >> 16 & 0xff)}
		s.fogLUT = &g.fogLUT
	case 7:
		g.warn("gas rendering")
	}
	s.cull = r[0x040] & 3
	s.scissorMode = r[0x065] & 3
	s.scX0, s.scY0 = int(r[0x066]&0x3ff), int(r[0x066]>>16&0x3ff)
	s.scX1, s.scY1 = int(r[0x067]&0x3ff), int(r[0x067]>>16&0x3ff)
	s.zScale, s.zOff = f24(r[0x04d]), f24(r[0x04e])
	s.wBuffer = r[0x06d]&1 != 0
	at := r[0x104]
	s.alphaTest, s.alphaFunc, s.alphaRef = at&1 != 0, at>>4&7, uint8(at>>8)
	st := r[0x105]
	s.stencil, s.stFunc = st&1 != 0, st>>4&7
	s.stWMask, s.stRef, s.stMask = uint8(st>>8), uint8(st>>16), uint8(st>>24)
	so := r[0x106]
	s.stFail, s.stZF, s.stZP = so&7, so>>4&7, so>>8&7
	dc := r[0x107]
	s.depthTest, s.depthFunc = dc&1 != 0, dc>>4&7
	s.colorMask = dc >> 8 & 0xf
	s.depthWrite = dc>>12&1 != 0 && r[0x115]&2 != 0
	s.stencilWrite = r[0x115]&1 != 0
	s.depthReadOK = r[0x114]&3 != 0
	s.colorWrite = r[0x113]&0xf != 0
	s.colorRead = r[0x112]&0xf != 0
	s.blend = r[0x100]>>8&1 != 0
	s.blendCfg = r[0x101]
	cc := r[0x103]
	s.blendColor = [4]uint8{uint8(cc), uint8(cc >> 8), uint8(cc >> 16), uint8(cc >> 24)}
	s.logicOp = r[0x102] & 0xf
	if s.color == nil {
		s.colorWrite = false
	}
	if s.depth == nil {
		s.depthWrite, s.depthTest, s.stencil, s.stencilWrite = false, false, false, false
	}
	return s
}

func texBytes(f uint32, w, h int) int {
	switch f {
	case 0:
		return w * h * 4
	case 1:
		return w * h * 3
	case 2, 3, 4, 5, 6:
		return w * h * 2
	case 7, 8, 9, 13:
		return w * h
	}
	return w * h / 2
}

// ---------------- triangles ----------------

func edge(ax, ay, bx, by, px, py float32) float32 {
	return (bx-ax)*(py-ay) - (by-ay)*(px-ax)
}

// topLeft: pixels exactly on the edge a→b belong to this triangle.
func topLeft(ax, ay, bx, by float32) bool {
	dy, dx := by-ay, bx-ax
	return dy < 0 || dy == 0 && dx > 0
}

func (g *gpu) raster(s *rasterState, v0, v1, v2 *gpuVertex) {
	area := edge(v0.sx, v0.sy, v1.sx, v1.sy, v2.sx, v2.sy)
	switch s.cull {
	case 1: // keep clockwise
		v1, v2 = v2, v1
		area = -area
		if area <= 0 {
			return
		}
	case 2: // keep counter-clockwise
		if area <= 0 {
			return
		}
	default:
		if area == 0 {
			return
		}
		if area < 0 {
			v1, v2 = v2, v1
			area = -area
		}
	}
	minX := int(math.Floor(float64(min(v0.sx, v1.sx, v2.sx))))
	maxX := int(math.Ceil(float64(max(v0.sx, v1.sx, v2.sx))))
	minY := int(math.Floor(float64(min(v0.sy, v1.sy, v2.sy))))
	maxY := int(math.Ceil(float64(max(v0.sy, v1.sy, v2.sy))))
	minX, minY = max(minX, 0), max(minY, 0)
	maxX, maxY = min(maxX, s.fbW-1), min(maxY, s.fbH-1)
	if s.scissorMode == 3 {
		minX, minY = max(minX, s.scX0), max(minY, s.scY0)
		maxX, maxY = min(maxX, s.scX1), min(maxY, s.scY1)
	}
	tl0 := topLeft(v1.sx, v1.sy, v2.sx, v2.sy)
	tl1 := topLeft(v2.sx, v2.sy, v0.sx, v0.sy)
	tl2 := topLeft(v0.sx, v0.sy, v1.sx, v1.sy)
	inv := 1 / area
	rows := func(y0, y1 int) {
		var f fragment
		for y := y0; y <= y1; y++ {
			py := float32(y) + 0.5
			// the span of this line inside all three edges (one pixel
			// wider each side; the exact test below decides)
			xs, xe := minX, maxX
			for _, e := range [3][4]float32{{v1.sx, v1.sy, v2.sx, v2.sy}, {v2.sx, v2.sy, v0.sx, v0.sy}, {v0.sx, v0.sy, v1.sx, v1.sy}} {
				a := -(e[3] - e[1])
				c := edge(e[0], e[1], e[2], e[3], float32(minX)+0.5, py)
				if a == 0 {
					if c < 0 {
						xs, xe = 1, 0
					}
					continue
				}
				root := float64(float32(minX) + 0.5 - c/a)
				if a > 0 {
					xs = max(xs, int(math.Ceil(root-0.5))-1)
				} else {
					xe = min(xe, int(math.Floor(root-0.5))+1)
				}
			}
			for x := xs; x <= xe; x++ {
				px := float32(x) + 0.5
				w0 := edge(v1.sx, v1.sy, v2.sx, v2.sy, px, py)
				w1 := edge(v2.sx, v2.sy, v0.sx, v0.sy, px, py)
				w2 := edge(v0.sx, v0.sy, v1.sx, v1.sy, px, py)
				if w0 < 0 || w1 < 0 || w2 < 0 || w0 == 0 && !tl0 || w1 == 0 && !tl1 || w2 == 0 && !tl2 {
					continue
				}
				if s.scissorMode == 1 && x >= s.scX0 && x <= s.scX1 && y >= s.scY0 && y <= s.scY1 {
					continue
				}
				b0, b1, b2 := w0*inv, w1*inv, w2*inv
				p0, p1, p2 := b0*v0.invW, b1*v1.invW, b2*v2.invW
				ps := 1 / (p0 + p1 + p2)
				p0, p1, p2 = p0*ps, p1*ps, p2*ps
				a0, a1, a2 := &v0.a, &v1.a, &v2.a
				z := v0.sz*b0 + v1.sz*b1 + v2.sz*b2
				depth := z*s.zScale + s.zOff
				if s.wBuffer {
					depth = (a0[2]*p0+a1[2]*p1+a2[2]*p2)*s.zScale + s.zOff*(a0[3]*p0+a1[3]*p1+a2[3]*p2)
				}
				f.depth = max(0, min(1, depth))
				for c := 0; c < 4; c++ {
					k := semColor + c
					f.color[c] = uint8(max(0, min(1, a0[k]*p0+a1[k]*p1+a2[k]*p2))*255 + 0.5)
				}
				for t, k := range [3]int{semTC0, semTC1, semTC2} {
					if s.tcUsed>>t&1 != 0 {
						f.tc[t] = [2]float32{a0[k]*p0 + a1[k]*p1 + a2[k]*p2, a0[k+1]*p0 + a1[k+1]*p1 + a2[k+1]*p2}
					}
				}
				if s.tex[0].on && s.tex[0].typ != 0 {
					f.tc0w = a0[semTC0W]*p0 + a1[semTC0W]*p1 + a2[semTC0W]*p2
				}
				if s.light != nil {
					for c := 0; c < 4; c++ {
						k := semQuat + c
						f.quat[c] = a0[k]*p0 + a1[k]*p1 + a2[k]*p2
					}
					for c := 0; c < 3; c++ {
						k := semView + c
						f.view[c] = a0[k]*p0 + a1[k]*p1 + a2[k]*p2
					}
				}
				g.fragment(s, x, y, &f)
			}
		}
	}
	// big triangles are drawn in bands of lines on several cores
	n := min(runtime.NumCPU(), 8)
	if h := maxY - minY + 1; n > 1 && h >= 2*n && h*(maxX-minX+1) >= 8192 {
		var wg sync.WaitGroup
		for b := 0; b < n; b++ {
			y0, y1 := minY+h*b/n, minY+h*(b+1)/n-1
			wg.Add(1)
			go func() {
				defer wg.Done()
				rows(y0, y1)
			}()
		}
		wg.Wait()
	} else {
		rows(minY, maxY)
	}
}

type fragment struct {
	color [4]uint8
	tc    [3][2]float32
	tc0w  float32
	depth float32
	quat  [4]float32
	view  v3
}

// ---------------- textures ----------------

func wrap(i, size int, mode uint32) (int, bool) {
	switch mode {
	case 1: // clamp to border
		if i < 0 || i >= size {
			return 0, false
		}
		return i, true
	case 2, 6: // repeat
		i %= size
		if i < 0 {
			i += size
		}
		return i, true
	case 3, 7: // mirrored repeat
		p := size * 2
		i %= p
		if i < 0 {
			i += p
		}
		if i >= size {
			i = p - 1 - i
		}
		return i, true
	}
	return max(0, min(size-1, i)), true
}

func (t *texUnit) texel(x, y int) [4]uint8 { return t.texelIn(t.data, x, y) }

func (t *texUnit) texelIn(d []byte, x, y int) [4]uint8 {
	xs, ok1 := wrap(x, t.w, t.wrapS)
	ys, ok2 := wrap(y, t.h, t.wrapT)
	if !ok1 || !ok2 {
		return t.border
	}
	ym := t.h - 1 - ys
	if t.format >= 12 {
		return etc1Texel(d, xs, ym, t.w, t.format == 13)
	}
	return decodeTexel(d, t.format, int(tiledOffset(uint32(xs), uint32(ym), uint32(t.w))))
}

func ex5(v uint32) uint8 { return uint8(v<<3 | v>>2) }
func ex6(v uint32) uint8 { return uint8(v<<2 | v>>4) }

// decodeTexel: texel number i of a texture in one of the plain formats.
func decodeTexel(d []byte, f uint32, i int) [4]uint8 {
	switch f {
	case 0:
		p := d[i*4:]
		return [4]uint8{p[3], p[2], p[1], p[0]}
	case 1:
		p := d[i*3:]
		return [4]uint8{p[2], p[1], p[0], 255}
	case 2, 3, 4:
		v := uint32(d[i*2]) | uint32(d[i*2+1])<<8
		switch f {
		case 2:
			return [4]uint8{ex5(v >> 11), ex5(v >> 6 & 31), ex5(v >> 1 & 31), uint8(v&1) * 255}
		case 3:
			return [4]uint8{ex5(v >> 11), ex6(v >> 5 & 63), ex5(v & 31), 255}
		}
		return [4]uint8{uint8(v>>12) * 17, uint8(v>>8&15) * 17, uint8(v>>4&15) * 17, uint8(v&15) * 17}
	case 5:
		return [4]uint8{d[i*2+1], d[i*2+1], d[i*2+1], d[i*2]}
	case 6:
		return [4]uint8{d[i*2+1], d[i*2], 0, 255}
	case 7:
		return [4]uint8{d[i], d[i], d[i], 255}
	case 8:
		return [4]uint8{0, 0, 0, d[i]}
	case 9:
		v := d[i]
		return [4]uint8{v >> 4 * 17, v >> 4 * 17, v >> 4 * 17, v & 15 * 17}
	}
	n := d[i/2] >> (4 * (i & 1)) & 15 * 17
	if f == 10 {
		return [4]uint8{n, n, n, 255}
	}
	return [4]uint8{0, 0, 0, n}
}

var etcTable = [8][4]int{
	{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
	{18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183},
}

func le64(b []byte) uint64 {
	return uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
		uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56
}

// etc1Texel decodes one texel of an ETC1 (or ETC1 with 4-bit alpha)
// texture: 8×8 tiles of four 4×4 blocks.
func etc1Texel(d []byte, x, y, w int, alpha bool) [4]uint8 {
	blk := 8
	if alpha {
		blk = 16
	}
	tile := ((y>>3)*(w>>3) + x>>3) * blk * 4
	sub := (x&7)>>2 + (y&7)>>2*2
	off := tile + sub*blk
	if off+blk > len(d) {
		return [4]uint8{}
	}
	bx, by := x&3, y&3
	a := uint8(255)
	if alpha {
		a = uint8(le64(d[off:])>>(4*(bx*4+by))&15) * 17
		off += 8
	}
	v := le64(d[off:])
	hi, lo := uint32(v>>32), uint32(v)
	flip, diff := hi&1 != 0, hi>>1&1 != 0
	second := bx >= 2
	if flip {
		second = by >= 2
	}
	var base [3]int
	if diff {
		for c, sh := range [3]uint{27, 19, 11} {
			b := hi >> sh & 31
			if second {
				d := int32(hi>>(sh-3)&7) << 29 >> 29
				b = uint32(int32(b)+d) & 31
			}
			base[c] = int(ex5(b))
		}
	} else {
		for c, sh := range [3]uint{28, 20, 12} {
			if second {
				sh -= 4
			}
			base[c] = int(hi>>sh&15) * 17
		}
	}
	tab := hi >> 5 & 7
	if second {
		tab = hi >> 2 & 7
	}
	pos := uint(bx*4 + by)
	idx := (lo>>(16+pos)&1)<<1 | lo>>pos&1
	m := etcTable[tab][idx]
	var out [4]uint8
	for c := 0; c < 3; c++ {
		out[c] = uint8(max(0, min(255, base[c]+m)))
	}
	out[3] = a
	return out
}

func (t *texUnit) sample(u, v float32) [4]uint8 { return t.sampleIn(t.data, u, v) }

func (t *texUnit) sampleIn(d []byte, u, v float32) [4]uint8 {
	s, tt := u*float32(t.w), v*float32(t.h)
	if !t.linear {
		return t.texelIn(d, int(math.Floor(float64(s))), int(math.Floor(float64(tt))))
	}
	s, tt = s-0.5, tt-0.5
	x0, y0 := int(math.Floor(float64(s))), int(math.Floor(float64(tt)))
	fx, fy := s-float32(x0), tt-float32(y0)
	c00, c10 := t.texelIn(d, x0, y0), t.texelIn(d, x0+1, y0)
	c01, c11 := t.texelIn(d, x0, y0+1), t.texelIn(d, x0+1, y0+1)
	var out [4]uint8
	for c := 0; c < 4; c++ {
		top := float32(c00[c])*(1-fx) + float32(c10[c])*fx
		bot := float32(c01[c])*(1-fx) + float32(c11[c])*fx
		out[c] = uint8(top*(1-fy) + bot*fy + 0.5)
	}
	return out
}

// ---------------- the colour combiners ----------------

func colorMod(op uint32, v [4]uint8) [3]int {
	switch op {
	case 1:
		return [3]int{255 - int(v[0]), 255 - int(v[1]), 255 - int(v[2])}
	case 2:
		return [3]int{int(v[3]), int(v[3]), int(v[3])}
	case 3:
		a := 255 - int(v[3])
		return [3]int{a, a, a}
	case 4:
		return [3]int{int(v[0]), int(v[0]), int(v[0])}
	case 5:
		r := 255 - int(v[0])
		return [3]int{r, r, r}
	case 8:
		return [3]int{int(v[1]), int(v[1]), int(v[1])}
	case 9:
		r := 255 - int(v[1])
		return [3]int{r, r, r}
	case 0xc:
		return [3]int{int(v[2]), int(v[2]), int(v[2])}
	case 0xd:
		r := 255 - int(v[2])
		return [3]int{r, r, r}
	}
	return [3]int{int(v[0]), int(v[1]), int(v[2])}
}

func alphaMod(op uint32, v [4]uint8) int {
	switch op {
	case 1:
		return 255 - int(v[3])
	case 2:
		return int(v[0])
	case 3:
		return 255 - int(v[0])
	case 4:
		return int(v[1])
	case 5:
		return 255 - int(v[1])
	case 6:
		return int(v[2])
	case 7:
		return 255 - int(v[2])
	}
	return int(v[3])
}

func clamp255(v int) int { return max(0, min(255, v)) }

func combine(op uint32, a, b, c int) int {
	switch op {
	case 0:
		return a
	case 1:
		return a * b / 255
	case 2:
		return min(255, a+b)
	case 3:
		return clamp255(a + b - 128)
	case 4:
		return (a*c + b*(255-c)) / 255
	case 5:
		return max(0, a-b)
	case 8:
		return min(255, a*b/255+c)
	case 9:
		return min(255, a+b) * c / 255
	}
	return a
}

func (g *gpu) fragment(s *rasterState, x, y int, f *fragment) {
	var tex [3][4]uint8
	for i := range s.tex {
		t := &s.tex[i]
		if !t.on {
			continue
		}
		tc := f.tc[i]
		if i == 2 && s.tex2UsesTC1 {
			tc = f.tc[1]
		}
		if i == 0 && t.typ != 0 {
			tex[0] = t.sampleSpecial(tc[0], tc[1], f.tc0w)
			continue
		}
		tex[i] = t.sample(tc[0], tc[1])
	}
	var tex3 [4]uint8
	if s.pt != nil {
		tc := f.tc[s.pt.coord]
		tex3 = s.pt.sample(tc[0], tc[1])
	}
	var lp, ls [4]uint8
	if s.light != nil {
		lp, ls = s.light.shade(f.quat, f.view, &tex)
	}
	var in [16][4]uint8 // the combiners' inputs by source number
	in[0], in[1], in[2] = f.color, lp, ls
	in[3], in[4], in[5], in[6] = tex[0], tex[1], tex[2], tex3
	var prev [4]uint8
	next := s.bufInit
	for i := range s.tev {
		st := &s.tev[i]
		out := prev
		if !s.tevPass[i] {
			srcReg, opReg, cmb, scale := st[0], st[1], st[2], st[4]
			in[0xe] = s.tevConst[i]
			in[0xf] = prev
			var cin [3][3]int
			var ain [3]int
			for k := uint32(0); k < 3; k++ {
				cin[k] = colorMod(opReg>>(4*k)&0xf, in[srcReg>>(4*k)&0xf])
				ain[k] = alphaMod(opReg>>(12+4*k)&7, in[srcReg>>(16+4*k)&0xf])
			}
			cop, aop := cmb&0xf, cmb>>16&0xf
			cs, as := int(1)<<(scale&3), int(1)<<(scale>>16&3)
			if cop == 6 || cop == 7 { // dot3
				d := ((cin[0][0]*2-255)*(cin[1][0]*2-255) + (cin[0][1]*2-255)*(cin[1][1]*2-255) + (cin[0][2]*2-255)*(cin[1][2]*2-255) + 128) >> 8
				d = clamp255(d * cs)
				out[0], out[1], out[2] = uint8(d), uint8(d), uint8(d)
				if cop == 7 {
					out[3] = uint8(d)
				}
			} else {
				for c := 0; c < 3; c++ {
					out[c] = uint8(clamp255(combine(cop, cin[0][c], cin[1][c], cin[2][c]) * cs))
				}
			}
			if cop != 7 {
				if aop == 6 || aop == 7 {
					d := ((ain[0]*2-255)*(ain[1]*2-255)*3 + 128) >> 8
					out[3] = uint8(clamp255(d * as))
				} else {
					out[3] = uint8(clamp255(combine(aop, ain[0], ain[1], ain[2]) * as))
				}
			}
		}
		prev = out
		in[0xd] = next // the buffer, one stage behind
		if i < 4 {
			if s.bufUpdate>>i&1 != 0 {
				next[0], next[1], next[2] = out[0], out[1], out[2]
			}
			if s.bufUpdate>>(4+i)&1 != 0 {
				next[3] = out[3]
			}
		}
	}
	col := prev
	if s.fog {
		z := f.depth
		if s.fogFlip {
			z = 1 - z
		}
		fi := z * 128
		i := max(0, min(127, int(fi)))
		e := s.fogLUT[i]
		k := float32(e>>13&0x7ff)/2047 + float32(int32(e<<19)>>19)/2047*(fi-float32(i))
		k = max(0, min(1, k))
		for c := 0; c < 3; c++ {
			col[c] = uint8(k*float32(col[c]) + (1-k)*s.fogColor[c] + 0.5)
		}
	}
	if s.alphaTest && !testFunc(s.alphaFunc, int(col[3]), int(s.alphaRef)) {
		return
	}
	// stencil and depth
	ym := s.fbH - 1 - y
	pix := int(tiledOffset(uint32(x), uint32(ym), uint32(s.fbW)))
	var dOld uint32
	var stOld uint8
	var dp []byte
	if s.depth != nil {
		dp = s.depth[pix*s.dBpp:]
		switch s.dFmt {
		case 0, 1:
			dOld = uint32(dp[0]) | uint32(dp[1])<<8
		case 2:
			dOld = uint32(dp[0]) | uint32(dp[1])<<8 | uint32(dp[2])<<16
		case 3:
			dOld = uint32(dp[0]) | uint32(dp[1])<<8 | uint32(dp[2])<<16
			stOld = dp[3]
		}
	}
	dMax := float32(0xffffff)
	if s.dBpp == 2 {
		dMax = 0xffff
	}
	dNew := uint32(f.depth * dMax)
	stencilOn := s.stencil && s.dFmt == 3
	if stencilOn {
		if !testFunc(s.stFunc, int(s.stRef&s.stMask), int(stOld&s.stMask)) {
			g.stencilOp(s, dp, stOld, s.stFail)
			return
		}
	}
	if s.depthTest && !testFunc(s.depthFunc, int(dNew), int(dOld)) {
		if stencilOn {
			g.stencilOp(s, dp, stOld, s.stZF)
		}
		return
	}
	if stencilOn {
		g.stencilOp(s, dp, stOld, s.stZP)
	}
	if s.depthWrite && dp != nil {
		switch s.dFmt {
		case 0, 1:
			dp[0], dp[1] = byte(dNew), byte(dNew>>8)
		default:
			dp[0], dp[1], dp[2] = byte(dNew), byte(dNew>>8), byte(dNew>>16)
		}
	}
	if !s.colorWrite || s.colorMask == 0 {
		return
	}
	cp := s.color[pix*s.cBpp:]
	var dst [4]uint8
	if s.colorRead {
		dst = readColor(cp, s.cFmt)
	}
	var res [4]uint8
	if s.blend {
		res = blend(s, col, dst)
	} else {
		for c := 0; c < 4; c++ {
			res[c] = logicOp(s.logicOp, col[c], dst[c])
		}
	}
	for c := 0; c < 4; c++ {
		if s.colorMask>>c&1 == 0 {
			res[c] = dst[c]
		}
	}
	writeColor(cp, s.cFmt, res)
}

func testFunc(fn uint32, a, b int) bool {
	switch fn {
	case 0:
		return false
	case 1:
		return true
	case 2:
		return a == b
	case 3:
		return a != b
	case 4:
		return a < b
	case 5:
		return a <= b
	case 6:
		return a > b
	}
	return a >= b
}

func (g *gpu) stencilOp(s *rasterState, dp []byte, old uint8, op uint32) {
	if !s.stencilWrite || dp == nil {
		return
	}
	var v uint8
	switch op {
	case 0:
		return
	case 1:
		v = 0
	case 2:
		v = s.stRef
	case 3:
		v = old
		if v < 255 {
			v++
		}
	case 4:
		v = old
		if v > 0 {
			v--
		}
	case 5:
		v = ^old
	case 6:
		v = old + 1
	case 7:
		v = old - 1
	}
	dp[3] = old&^s.stWMask | v&s.stWMask
}

func readColor(p []byte, f uint32) [4]uint8 {
	switch f {
	case 0:
		return [4]uint8{p[3], p[2], p[1], p[0]}
	case 1:
		return [4]uint8{p[2], p[1], p[0], 255}
	}
	v := uint32(p[0]) | uint32(p[1])<<8
	switch f {
	case 2:
		return [4]uint8{ex5(v >> 11), ex5(v >> 6 & 31), ex5(v >> 1 & 31), uint8(v&1) * 255}
	case 3:
		return [4]uint8{ex5(v >> 11), ex6(v >> 5 & 63), ex5(v & 31), 255}
	}
	return [4]uint8{uint8(v>>12) * 17, uint8(v>>8&15) * 17, uint8(v>>4&15) * 17, uint8(v&15) * 17}
}

func writeColor(p []byte, f uint32, c [4]uint8) {
	var v uint32
	switch f {
	case 0:
		p[0], p[1], p[2], p[3] = c[3], c[2], c[1], c[0]
		return
	case 1:
		p[0], p[1], p[2] = c[2], c[1], c[0]
		return
	case 2:
		v = uint32(c[0]>>3)<<11 | uint32(c[1]>>3)<<6 | uint32(c[2]>>3)<<1 | uint32(c[3]>>7)
	case 3:
		v = uint32(c[0]>>3)<<11 | uint32(c[1]>>2)<<5 | uint32(c[2]>>3)
	default:
		v = uint32(c[0]>>4)<<12 | uint32(c[1]>>4)<<8 | uint32(c[2]>>4)<<4 | uint32(c[3]>>4)
	}
	p[0], p[1] = byte(v), byte(v>>8)
}

func blendFactor(f uint32, src, dst, k [4]uint8, c int) int {
	switch f {
	case 0:
		return 0
	case 1:
		return 255
	case 2:
		return int(src[c])
	case 3:
		return 255 - int(src[c])
	case 4:
		return int(dst[c])
	case 5:
		return 255 - int(dst[c])
	case 6:
		return int(src[3])
	case 7:
		return 255 - int(src[3])
	case 8:
		return int(dst[3])
	case 9:
		return 255 - int(dst[3])
	case 10:
		return int(k[c])
	case 11:
		return 255 - int(k[c])
	case 12:
		return int(k[3])
	case 13:
		return 255 - int(k[3])
	case 14:
		if c == 3 {
			return 255
		}
		return min(int(src[3]), 255-int(dst[3]))
	}
	return 255
}

func blend(s *rasterState, src, dst [4]uint8) [4]uint8 {
	cfg := s.blendCfg
	var out [4]uint8
	for c := 0; c < 4; c++ {
		eq, sf, df := cfg&7, cfg>>16&0xf, cfg>>20&0xf
		if c == 3 {
			eq, sf, df = cfg>>8&7, cfg>>24&0xf, cfg>>28&0xf
		}
		a := int(src[c]) * blendFactor(sf, src, dst, s.blendColor, c)
		b := int(dst[c]) * blendFactor(df, src, dst, s.blendColor, c)
		var r int
		switch eq {
		case 0:
			r = (a + b) / 255
		case 1:
			r = (a - b) / 255
		case 2:
			r = (b - a) / 255
		case 3:
			r = min(int(src[c]), int(dst[c]))
		case 4:
			r = max(int(src[c]), int(dst[c]))
		default:
			r = (a + b) / 255
		}
		out[c] = uint8(clamp255(r))
	}
	return out
}

func logicOp(op uint32, s, d uint8) uint8 {
	switch op {
	case 0:
		return 0
	case 1:
		return s & d
	case 2:
		return s &^ d
	case 3:
		return s
	case 4:
		return 255
	case 5:
		return ^s
	case 6:
		return d
	case 7:
		return ^d
	case 8:
		return ^(s & d)
	case 9:
		return s | d
	case 10:
		return ^(s | d)
	case 11:
		return s ^ d
	case 12:
		return ^(s ^ d)
	case 13:
		return ^s & d
	case 14:
		return s | ^d
	}
	return ^s | d
}
