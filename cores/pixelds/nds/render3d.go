package nds

// The 3D rasterizer: draws the polygon list made by the geometry engine
// (gpu3d.go) into a 256×192 picture with depth, translucency, textures,
// toon and highlight shading, shadows, edge marking and fog. The 2D engine
// shows it as BG0 (line).

import (
	"math"
	"runtime"
	"sort"
	"sync"
)

// attr buffer bits
const (
	pxOpaqueID  = 0x3f      // bits 0-5: opaque polygon ID
	pxTransID   = 0x3f << 8 // bits 8-13: translucent polygon ID
	pxFog       = 1 << 16   // fog applies
	pxEdge      = 1 << 17   // on a polygon's edge
	pxTrans     = 1 << 18   // a translucent polygon drew here
	pxPolyDrawn = 1 << 19   // a polygon (not the clear plane) drew here
)

// line fills BG0 with line y of the 3D picture.
func (g *gpu3D) line(y int, out *[256]uint32, alpha *[256]uint8) {
	row := g.out[y*256 : y*256+256]
	for x, v := range row {
		a := v >> 16 & 31
		if a == 0 {
			out[x] = 0
			alpha[x] = 0
			continue
		}
		out[x] = opaque | v&0x7fff
		alpha[x] = uint8(a)
	}
}

type scrVtx struct {
	x, y, z, iw float64
	r, g, b     float64 // over w
	s, t        float64 // over w
}

func (g *gpu3D) renderFrame() {
	s := &g.s
	dc := s.Disp3DCnt
	g.clearPlanes()
	wbuf := s.RenderFlag&2 != 0
	type item struct {
		p          *Polygon
		v          []scrVtx
		ymin, ymax float64
		trans      bool
	}
	items := make([]item, 0, len(s.Render))
	vp := s.Viewport
	vw := float64(vp[2] - vp[0] + 1)
	vh := float64(vp[3] - vp[1] + 1)
	top := float64(191 - vp[3])
	sc := float64(g.sc)
	for i := range s.Render {
		p := &s.Render[i]
		it := item{p: p, ymin: math.Inf(1), ymax: math.Inf(-1)}
		for _, v := range p.V {
			w := v.W
			if w <= 0 {
				w = 1.0 / 4096
			}
			iw := 1 / w
			sv := scrVtx{
				x:  ((v.X*iw+1)*vw/2 + float64(vp[0])) * sc,
				y:  ((1-v.Y*iw)*vh/2 + top) * sc,
				iw: iw,
				r:  v.R * iw, g: v.G * iw, b: v.B * iw,
				s: v.S * iw, t: v.T * iw,
			}
			if wbuf {
				sv.z = w
			} else {
				sv.z = (v.Z*iw*0x4000 + 0x3fff) * 0x200
			}
			it.ymin = math.Min(it.ymin, sv.y)
			it.ymax = math.Max(it.ymax, sv.y)
			it.v = append(it.v, sv)
		}
		alpha := p.Attr >> 16 & 31
		texAlpha := dc&1 != 0 && (p.Tex>>26&7 == 1 || p.Tex>>26&7 == 6)
		it.trans = alpha != 0 && alpha != 31 || texAlpha
		items = append(items, it)
	}
	manual := s.RenderFlag&1 != 0
	sort.SliceStable(items, func(a, b int) bool {
		A, B := &items[a], &items[b]
		if A.trans != B.trans {
			return !A.trans
		}
		if A.trans && manual {
			return false
		}
		if A.ymax != B.ymax {
			return A.ymax < B.ymax
		}
		return A.ymin < B.ymin
	})
	// Bands of lines are drawn at the same time, each going through all
	// the polygons in order.
	bands := runtime.NumCPU()
	if bands > 8 {
		bands = 8
	}
	if len(items) < 32 && g.sc == 1 {
		bands = 1
	}
	var wg sync.WaitGroup
	for b := 0; b < bands; b++ {
		lo, hi := g.H*b/bands, g.H*(b+1)/bands
		wg.Add(1)
		go func() {
			defer wg.Done()
			lastShadowMask := false
			for i := range items {
				it := &items[i]
				if it.ymax < float64(lo)-1 || it.ymin > float64(hi)+1 {
					continue
				}
				isMask := it.p.Attr>>4&3 == 3 && it.p.Attr>>24&63 == 0
				if isMask && !lastShadowMask {
					for k := lo * g.W; k < hi*g.W; k++ {
						g.stencil[k] = false
					}
				}
				lastShadowMask = isMask
				g.drawPoly(it.p, it.v, it.trans, wbuf, lo, hi)
			}
		}()
	}
	wg.Wait()
	if dc&(1<<5) != 0 {
		g.edgeMark()
	}
	if dc&(1<<7) != 0 {
		g.fog()
	}
	if g.sc == 1 {
		copy(g.out[:], g.color)
		return
	}
	copy(g.hi, g.color)
	// the DS-resolution picture: the middle of each block
	for y := 0; y < 192; y++ {
		row := (y*g.sc + g.sc/2) * g.W
		for x := 0; x < 256; x++ {
			g.out[y*256+x] = g.color[row+x*g.sc+g.sc/2]
		}
	}
}

func (g *gpu3D) clearPlanes() {
	s := &g.s
	cc := uint32(g.reg16(0x04000350)) | uint32(g.reg16(0x04000352))<<16
	cd := uint32(g.reg16(0x04000354)) & 0x7fff
	depth := cd * 0x200
	if cd == 0x7fff {
		depth += 0x1ff
	}
	id := cc >> 24 & 63
	if s.Disp3DCnt&(1<<14) != 0 { // rear-plane bitmap from texture slots 2 and 3
		off := g.reg16(0x04000356)
		ox, oy := int(off&0xff), int(off>>8)
		tex := g.m.vram.tex
		for y := 0; y < g.H; y++ {
			for x := 0; x < g.W; x++ {
				a := uint32(((y/g.sc+oy)&0xff)*256+(x/g.sc+ox)&0xff) * 2
				c := uint32(tex.read16(0x40000 + a))
				d := uint32(tex.read16(0x60000 + a))
				i := y*g.W + x
				al := uint32(0)
				if c&0x8000 != 0 {
					al = 31
				}
				g.color[i] = c&0x7fff | al<<16
				dd := (d & 0x7fff) * 0x200
				if d&0x7fff == 0x7fff {
					dd += 0x1ff
				}
				g.depth[i] = dd
				g.attr[i] = id
				if d&0x8000 != 0 {
					g.attr[i] |= pxFog
				}
				g.stencil[i] = false
			}
		}
		return
	}
	col := cc&0x7fff | (cc>>16&31)<<16
	at := id
	if cc&0x8000 != 0 {
		at |= pxFog
	}
	for i := range g.color {
		g.color[i] = col
		g.depth[i] = depth
		g.attr[i] = at
		g.stencil[i] = false
	}
}

func c5to6(c float64) int {
	v := int(c*(63.0/31) + 0.5)
	if v < 0 {
		return 0
	}
	if v > 63 {
		return 63
	}
	return v
}

func a5to6(a int) int {
	if a == 0 {
		return 0
	}
	return a*2 + 1
}

func (g *gpu3D) drawPoly(p *Polygon, vs []scrVtx, trans, wbuf bool, lo, hi int) {
	s := &g.s
	dc := s.Disp3DCnt
	n := len(vs)
	ymin, ymax := math.Inf(1), math.Inf(-1)
	for _, v := range vs {
		ymin = math.Min(ymin, v.y)
		ymax = math.Max(ymax, v.y)
	}
	y0 := int(math.Ceil(ymin - 0.5))
	y1 := int(math.Ceil(ymax - 0.5))
	if y1 <= y0 {
		y1 = y0 + 1
	}
	attr := p.Attr
	polyAlpha := int(attr >> 16 & 31)
	wire := polyAlpha == 0
	if wire {
		polyAlpha = 31
	}
	mode := attr >> 4 & 3
	id := attr >> 24 & 63
	equal := attr&(1<<14) != 0
	texOn := dc&1 != 0 && p.Tex>>26&7 != 0
	alphaTest := dc&4 != 0
	alphaRef := int(g.reg16(0x04000340) & 31)
	blendOn := dc&8 != 0
	highlight := dc&2 != 0
	isMask := mode == 3 && id == 0
	isShadow := mode == 3 && id != 0
	var tx texSampler
	if texOn {
		tx = g.sampler(p.Tex, p.Pal)
		if g.tex.active() {
			tx.rep = g.lookup(p.Tex, p.Pal)
		}
	}
	var left, right scrVtx
	for y := max(y0, lo); y < min(y1, hi); y++ {
		yc := float64(y) + 0.5
		if yc < ymin {
			yc = ymin
		}
		if yc > ymax {
			yc = ymax
		}
		found := 0
		for i := 0; i < n; i++ {
			a, b := &vs[i], &vs[(i+1)%n]
			if a.y > b.y {
				a, b = b, a
			}
			if yc < a.y || yc > b.y {
				continue
			}
			var pts [2]scrVtx
			np := 0
			if b.y == a.y {
				pts[0], pts[1] = *a, *b
				np = 2
			} else {
				pts[0] = lerpS(a, b, (yc-a.y)/(b.y-a.y))
				np = 1
			}
			for k := 0; k < np; k++ {
				pt := pts[k]
				if found == 0 {
					left, right = pt, pt
				} else if pt.x < left.x {
					left = pt
				} else if pt.x > right.x {
					right = pt
				}
				found++
			}
		}
		if found == 0 {
			continue
		}
		xs := int(math.Ceil(left.x - 0.5))
		xe := int(math.Ceil(right.x - 0.5))
		if xe <= xs {
			xe = xs + 1
		}
		span := right.x - left.x
		for x := xs; x < xe; x++ {
			if x < 0 || x >= g.W {
				continue
			}
			edge := x == xs || x == xe-1 || y == y0 || y == y1-1
			if wire && !edge {
				continue
			}
			t := 0.0
			if span > 0 {
				t = (float64(x) + 0.5 - left.x) / span
				if t < 0 {
					t = 0
				} else if t > 1 {
					t = 1
				}
			}
			w := 1 / (left.iw + (right.iw-left.iw)*t)
			var z uint32
			if wbuf {
				z = clampDepth(w)
			} else {
				z = clampDepth(left.z + (right.z-left.z)*t)
			}
			i := y*g.W + x
			dz := g.depth[i]
			pass := z < dz
			if equal {
				d := int64(z) - int64(dz)
				pass = d >= -0x200 && d <= 0x200
			}
			if isMask {
				if !pass {
					g.stencil[i] = true
				}
				continue
			}
			if !pass {
				continue
			}
			if isShadow {
				if !g.stencil[i] || g.attr[i]&pxOpaqueID == id {
					continue
				}
				g.stencil[i] = false
			}
			pt := lerpS(&left, &right, t)
			// colour
			vr, vg, vb := c5to6(pt.r*w), c5to6(pt.g*w), c5to6(pt.b*w)
			va := a5to6(polyAlpha)
			r, gg, b, a := vr, vg, vb, va
			if mode == 2 {
				tc := g.reg16(0x04000380 + uint32(vr>>1)*2)
				tr, tg, tb := a5to6(int(tc&31)), a5to6(int(tc>>5&31)), a5to6(int(tc>>10&31))
				if !highlight {
					vr, vg, vb = tr, tg, tb
					r, gg, b = tr, tg, tb
				}
				if texOn {
					c, ta := tx.at(pt.s*w, pt.t*w)
					cr, cg, cb := a5to6(int(c&31)), a5to6(int(c>>5&31)), a5to6(int(c>>10&31))
					ta6 := a5to6(ta)
					r = ((cr+1)*(vr+1) - 1) >> 6
					gg = ((cg+1)*(vg+1) - 1) >> 6
					b = ((cb+1)*(vb+1) - 1) >> 6
					a = ((ta6+1)*(va+1) - 1) >> 6
				}
				if highlight {
					r, gg, b = min(r+tr, 63), min(gg+tg, 63), min(b+tb, 63)
				}
			} else if texOn {
				c, ta := tx.at(pt.s*w, pt.t*w)
				cr, cg, cb := a5to6(int(c&31)), a5to6(int(c>>5&31)), a5to6(int(c>>10&31))
				ta6 := a5to6(ta)
				if mode == 1 { // decal
					switch ta {
					case 0:
					case 31:
						r, gg, b = cr, cg, cb
					default:
						r = (cr*ta6 + vr*(63-ta6)) >> 6
						gg = (cg*ta6 + vg*(63-ta6)) >> 6
						b = (cb*ta6 + vb*(63-ta6)) >> 6
					}
				} else { // modulation (and shadow)
					r = ((cr+1)*(vr+1) - 1) >> 6
					gg = ((cg+1)*(vg+1) - 1) >> 6
					b = ((cb+1)*(vb+1) - 1) >> 6
					a = ((ta6+1)*(va+1) - 1) >> 6
				}
			}
			a5 := a >> 1
			if a5 == 0 {
				continue
			}
			if alphaTest && a5 <= alphaRef {
				continue
			}
			col := uint32(r>>1) | uint32(gg>>1)<<5 | uint32(b>>1)<<10
			at := g.attr[i]
			if a5 < 31 {
				if at&pxTrans != 0 && at>>8&63 == id {
					continue
				}
				dst := g.color[i]
				da := dst >> 16 & 31
				out := col
				oa := uint32(a5)
				if blendOn && da > 0 {
					out = blend3D(col, dst&0x7fff, uint32(a5))
					if da > oa {
						oa = da
					}
				}
				g.color[i] = out | oa<<16
				if attr&(1<<11) != 0 {
					g.depth[i] = z
				}
				fog := at & pxFog
				if attr&(1<<15) == 0 {
					fog = 0
				}
				g.attr[i] = at&^(pxTransID|pxFog) | id<<8 | pxTrans | fog | pxPolyDrawn
				continue
			}
			g.color[i] = col | 31<<16
			g.depth[i] = z
			na := id | pxPolyDrawn
			if attr&(1<<15) != 0 {
				na |= pxFog
			}
			if edge {
				na |= pxEdge
			}
			g.attr[i] = na
		}
	}
}

func clampDepth(z float64) uint32 {
	if z < 0 {
		return 0
	}
	if z > 0xffffff {
		return 0xffffff
	}
	return uint32(z)
}

func lerpS(a, b *scrVtx, t float64) scrVtx {
	return scrVtx{
		a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t, a.iw + (b.iw-a.iw)*t,
		a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t, a.b + (b.b-a.b)*t, a.s + (b.s-a.s)*t, a.t + (b.t-a.t)*t,
	}
}

func (g *gpu3D) edgeMark() {
	clearID := uint32(g.reg16(0x04000352)) >> 8 & 63
	cd := uint32(g.reg16(0x04000354)&0x7fff) * 0x200
	for y := 0; y < g.H; y++ {
		for x := 0; x < g.W; x++ {
			i := y*g.W + x
			at := g.attr[i]
			if at&pxEdge == 0 || g.color[i]>>16&31 == 0 {
				continue
			}
			id := at & pxOpaqueID
			d := g.depth[i]
			mark := false
			check := func(nx, ny int) {
				nid, nd := clearID, cd
				if nx >= 0 && nx < g.W && ny >= 0 && ny < g.H {
					j := ny*g.W + nx
					nid, nd = g.attr[j]&pxOpaqueID, g.depth[j]
				}
				if nid != id && d < nd {
					mark = true
				}
			}
			check(x-1, y)
			check(x+1, y)
			check(x, y-1)
			check(x, y+1)
			if mark {
				ec := uint32(g.reg16(0x04000330+(id>>3)*2)) & 0x7fff
				g.color[i] = g.color[i]&^0x7fff | ec
			}
		}
	}
}

func (g *gpu3D) fog() {
	dc := g.s.Disp3DCnt
	shift := dc >> 8 & 15
	step := int(0x400 >> shift)
	if step == 0 {
		step = 1
	}
	off := int(g.reg16(0x0400035c) & 0x7fff)
	fc := uint32(g.reg16(0x04000358))
	fa := uint32(g.reg16(0x0400035a) & 31)
	var table [32]int
	for i := range table {
		b := g.reg16(0x04000360 + uint32(i&^1))
		v := int(b>>(uint(i&1)*8)) & 0x7f
		if v == 127 {
			v = 128
		}
		table[i] = v
	}
	alphaOnly := dc&(1<<6) != 0
	for i := range g.color {
		if g.attr[i]&pxFog == 0 {
			continue
		}
		d := int(g.depth[i] >> 9)
		var dens int
		k := d - off
		switch {
		case k <= step:
			dens = table[0]
		case k >= step*32:
			dens = table[31]
		default:
			j := k/step - 1
			f := k % step
			dens = (table[j]*(step-f) + table[j+1]*f) / step
		}
		c := g.color[i]
		ca := c >> 16 & 31
		na := (fa*uint32(dens) + ca*uint32(128-dens)) >> 7
		col := c & 0x7fff
		if !alphaOnly {
			mix := func(sh uint) uint32 {
				return ((fc>>sh&31)*uint32(dens) + (col>>sh&31)*uint32(128-dens)) >> 7
			}
			col = mix(0) | mix(5)<<5 | mix(10)<<10
		}
		g.color[i] = col | na<<16
	}
}

// ---------------- textures ----------------

type texSampler struct {
	g            *gpu3D
	addr, pal    uint32
	w, h         int
	fmt          uint32
	repS, repT   bool
	flipS, flipT bool
	color0Trans  bool
	rep          *texRep // a texture pack's picture
}

func (g *gpu3D) sampler(tp, pltt uint32) texSampler {
	f := tp >> 26 & 7
	pal := pltt << 4
	if f == 2 {
		pal = pltt << 3
	}
	return texSampler{
		g: g, addr: (tp & 0xffff) << 3, pal: pal,
		w: 8 << (tp >> 20 & 7), h: 8 << (tp >> 23 & 7), fmt: f,
		repS: tp&(1<<16) != 0, repT: tp&(1<<17) != 0,
		flipS: tp&(1<<18) != 0, flipT: tp&(1<<19) != 0,
		color0Trans: tp&(1<<29) != 0,
	}
}

func wrap(v, n int, rep, flip bool) int {
	if !rep {
		if v < 0 {
			return 0
		}
		if v >= n {
			return n - 1
		}
		return v
	}
	if flip {
		v &= 2*n - 1
		if v >= n {
			v = 2*n - 1 - v
		}
		return v
	}
	return v & (n - 1)
}

// at returns the texel at texture coordinates s, t (texels × 16): its
// colour and alpha (0-31).
func (ts *texSampler) at(sf, tf float64) (uint16, int) {
	if ts.rep != nil {
		return ts.rep.at(ts, sf, tf)
	}
	s := wrap(int(math.Floor(sf/16)), ts.w, ts.repS, ts.flipS)
	t := wrap(int(math.Floor(tf/16)), ts.h, ts.repT, ts.flipT)
	tex, pal := ts.g.m.vram.tex, ts.g.m.vram.texpal
	i := uint32(t*ts.w + s)
	palc := func(idx uint32) uint16 { return pal.read16(ts.pal+idx*2) & 0x7fff }
	switch ts.fmt {
	case 1: // A3I5
		b := uint32(tex.read8(ts.addr + i))
		a := int(b >> 5)
		return palc(b & 31), a*4 + a/2
	case 2: // 4 colours
		idx := uint32(tex.read8(ts.addr+i/4)) >> (i & 3 * 2) & 3
		if idx == 0 && ts.color0Trans {
			return 0, 0
		}
		return palc(idx), 31
	case 3: // 16 colours
		idx := uint32(tex.read8(ts.addr+i/2)) >> (i & 1 * 4) & 15
		if idx == 0 && ts.color0Trans {
			return 0, 0
		}
		return palc(idx), 31
	case 4: // 256 colours
		idx := uint32(tex.read8(ts.addr + i))
		if idx == 0 && ts.color0Trans {
			return 0, 0
		}
		return palc(idx), 31
	case 5: // 4×4 compressed
		blk := ts.addr + uint32((t/4)*(ts.w/4)+s/4)*4
		word := tex.read32(blk)
		texel := word >> (uint(t&3)*8 + uint(s&3)*2) & 3
		slot1 := 0x20000 + (blk&0x1ffff)/2
		if blk >= 0x40000 {
			slot1 += 0x10000
		}
		info := uint32(tex.read16(slot1))
		pa := ts.pal + (info&0x3fff)*4
		c := func(k uint32) uint16 { return pal.read16(pa+k*2) & 0x7fff }
		mix := func(a, b uint16, wa, wb, d uint32) uint16 {
			ch := func(sh uint) uint16 {
				return uint16((uint32(a>>sh&31)*wa + uint32(b>>sh&31)*wb) / d)
			}
			return ch(0) | ch(5)<<5 | ch(10)<<10
		}
		switch info >> 14 {
		case 0:
			if texel == 3 {
				return 0, 0
			}
			return c(texel), 31
		case 1:
			switch texel {
			case 2:
				return mix(c(0), c(1), 1, 1, 2), 31
			case 3:
				return 0, 0
			}
			return c(texel), 31
		case 2:
			return c(texel), 31
		default:
			switch texel {
			case 2:
				return mix(c(0), c(1), 5, 3, 8), 31
			case 3:
				return mix(c(0), c(1), 3, 5, 8), 31
			}
			return c(texel), 31
		}
	case 6: // A5I3
		b := uint32(tex.read8(ts.addr + i))
		return palc(b & 7), int(b >> 3)
	case 7: // direct colour
		c := tex.read16(ts.addr + i*2)
		if c&0x8000 == 0 {
			return 0, 0
		}
		return c & 0x7fff, 31
	}
	return 0, 31
}
