package model3

import (
	"math"
	"runtime"
	"sync"
)

// The Real3D's drawing, done in software: each frame the scene is walked
// (scene.go), every visible triangle is transformed, clipped and set up, and
// then bands of screen rows are drawn in parallel. Like the hardware (and as
// Supermodel models it) there are three buffers: the opaque colour buffer
// and two translucent layers, merged over the bottom tile layers at the end.

type rgba [4]float32

const (
	layerColour = 1
	layerTrans1 = 2
	layerTrans2 = 4
)

type drawConst struct {
	a                *polyAttr
	vp               *viewport
	texX, texY, page int
}

type tri struct {
	dc       *drawConst
	col      rgba
	lodBase  float32
	specFlat float32
	layers   uint8
	x, y     [3]float32
	y0, y1   int
	p        [4][3]float32 // iw, u*iw, v*iw, s*iw: value at (0,0), d/dx, d/dy
}

type renderer struct {
	gpu *real3D

	vertexFactor, npFactor float32

	// scene walk state
	prev       [4]pvert
	prevRaw    [4][2]uint16
	colorTable uint32
	lodTable   uint32
	matBase    uint32
	matBaseOK  bool
	planes     planes
	curVP      *viewport
	vps        []*viewport
	visits     int
	romModels  map[uint32]*model

	tris       [4][2][]tri // by priority and high-priority (overlay) pass
	hasModels  [4]bool
	hasOverlay [4]bool

	col, t1, t2      []rgba
	depth, depthCopy []float32
	sten, stenCopy   []uint8
	white            bool
	losBack          [4]float32

	bands int
}

func newRenderer(g *real3D) *renderer {
	r := &renderer{gpu: g, romModels: map[uint32]*model{}}
	if g.step > 0x10 {
		r.vertexFactor, r.npFactor = 1.0/2048, 1.0/16384
	} else {
		r.vertexFactor, r.npFactor = 1.0/128, 1.0/4096
	}
	n := screenW * screenH
	r.col, r.t1, r.t2 = make([]rgba, n), make([]rgba, n), make([]rgba, n)
	r.depth, r.depthCopy = make([]float32, n), make([]float32, n)
	r.sten, r.stenCopy = make([]uint8, n), make([]uint8, n)
	r.bands = min(runtime.NumCPU(), 8)
	texLUTOnce.Do(buildTexLUT)
	return r
}

// draw renders the scene in the Real3D's memories.
func (r *renderer) draw() {
	r.gpu.los = r.losBack
	r.losBack = [4]float32{}
	r.white = r.gpu.blockCulling
	for p := range r.tris {
		for o := range r.tris[p] {
			r.tris[p][o] = r.tris[p][o][:0]
		}
	}
	r.hasModels = [4]bool{}
	r.hasOverlay = [4]bool{}
	if !r.white {
		r.buildScene()
		for _, vp := range r.vps {
			for i := range vp.draws {
				r.setupDraw(vp, &vp.draws[i])
			}
		}
	}
	var wg sync.WaitGroup
	for b := 0; b < r.bands; b++ {
		y0, y1 := screenH*b/r.bands, screenH*(b+1)/r.bands
		wg.Add(1)
		go func() {
			defer wg.Done()
			r.drawBand(y0, y1)
		}()
	}
	wg.Wait()
}

// ---------------------------------------------------------------- setup

type cvert struct {
	x, y, z float32 // view space
	u, v, s float32
}

func (r *renderer) setupDraw(vp *viewport, d *modelDraw) {
	if len(d.m.tris) == 0 {
		return
	}
	r.hasModels[vp.pri] = true
	if d.m.highPri {
		r.hasOverlay[vp.pri] = true
	}
	mt := &d.mat
	inv := 1 / d.scale
	nA := d.alpha < 1
	var dcs []*drawConst
	for i := range d.m.tris {
		t := &d.m.tris[i]
		a := t.a
		var layers uint8
		if !a.polyAlpha && !nA {
			layers |= layerColour
		}
		if a.texAlpha || a.polyAlpha || nA {
			if a.transSel {
				layers |= layerTrans2
			} else {
				layers |= layerTrans1
			}
		}
		if layers == 0 {
			continue
		}
		var cv [3]cvert
		for k := 0; k < 3; k++ {
			p := &t.v[k].pos
			cv[k].x = mt[0]*p[0] + mt[4]*p[1] + mt[8]*p[2] + mt[12]
			cv[k].y = mt[1]*p[0] + mt[5]*p[1] + mt[9]*p[2] + mt[13]
			cv[k].z = mt[2]*p[0] + mt[6]*p[1] + mt[10]*p[2] + mt[14]
			cv[k].u, cv[k].v = t.v[k].u, t.v[k].v
		}
		// back face test: the face normal against the direction to the eye
		fn := &t.fn
		nx := mt[0]*fn[0] + mt[4]*fn[1] + mt[8]*fn[2]
		ny := mt[1]*fn[0] + mt[5]*fn[1] + mt[9]*fn[2]
		nz := mt[2]*fn[0] + mt[6]*fn[1] + mt[10]*fn[2]
		cx := (cv[0].x + cv[1].x + cv[2].x) / 3
		cy := (cv[0].y + cv[1].y + cv[2].y) / 3
		cz := (cv[0].z + cv[1].z + cv[2].z) / 3
		dd := cx*nx + cy*ny + cz*nz
		if dd > 0 {
			continue
		}
		var specFlat float32
		if a.lighting {
			for k := 0; k < 3; k++ {
				if a.fixedShading {
					cv[k].s = t.v[k].shade
				} else {
					n := &t.v[k].n
					vx := (mt[0]*n[0] + mt[4]*n[1] + mt[8]*n[2]) * inv
					vy := (mt[1]*n[0] + mt[5]*n[1] + mt[9]*n[2]) * inv
					vz := (mt[2]*n[0] + mt[6]*n[1] + mt[10]*n[2]) * inv
					cv[k].s = vp.sun[0]*vx + vp.sun[1]*vy + vp.sun[2]*vz
				}
			}
			if a.specular && !a.smooth {
				n := &t.v[0].n
				vx := (mt[0]*n[0] + mt[4]*n[1] + mt[8]*n[2]) * inv
				vy := (mt[1]*n[0] + mt[5]*n[1] + mt[9]*n[2]) * inv
				vz := (mt[2]*n[0] + mt[6]*n[1] + mt[10]*n[2]) * inv
				ndl := vp.sun[0]*vx + vp.sun[1]*vy + vp.sun[2]*vz
				specFlat = max(0, -vp.sun[2]+2*ndl*vz)
			}
		}
		var dc *drawConst
		for _, c := range dcs {
			if c.a == a {
				dc = c
				break
			}
		}
		if dc == nil {
			dc = &drawConst{a: a, vp: vp, texX: a.x + d.texX, texY: a.y + d.texY, page: a.page ^ d.page}
			dcs = append(dcs, dc)
		}
		col := rgba{float32(t.col[0]) / 255, float32(t.col[1]) / 255, float32(t.col[2]) / 255, float32(t.col[3]) / 255 * d.alpha}
		if a.translatorMap {
			col[0] *= 16
			col[1] *= 16
			col[2] *= 16
		}
		base := tri{dc: dc, col: col, lodBase: -dd * vp.cota * t.np, specFlat: specFlat, layers: layers}
		ov := 0
		if a.highPri {
			ov = 1
		}
		r.clipAndAdd(vp, &base, cv, &r.tris[vp.pri][ov])
	}
}

func lerpV(a, b *cvert, t float32) cvert {
	return cvert{a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t, a.u + (b.u-a.u)*t, a.v + (b.v-a.v)*t, a.s + (b.s-a.s)*t}
}

func (r *renderer) clipAndAdd(vp *viewport, base *tri, in [3]cvert, list *[]tri) {
	var poly [4]cvert
	n := 0
	const zc = -nearPlane
	for i := 0; i < 3; i++ {
		a, b := &in[i], &in[(i+1)%3]
		ain, bin := a.z <= zc, b.z <= zc
		if ain {
			poly[n] = *a
			n++
		}
		if ain != bin {
			poly[n] = lerpV(a, b, (zc-a.z)/(b.z-a.z))
			n++
		}
	}
	if n < 3 {
		return
	}
	type sv struct{ x, y, iw, u, v, s float32 }
	var p [4]sv
	for i := 0; i < n; i++ {
		c := &poly[i]
		iw := 1 / -c.z
		// the projection's w is -z
		ndx := (vp.px*c.x + vp.pa*c.z) * iw
		ndy := (vp.py*c.y + vp.pb*c.z) * iw
		p[i] = sv{
			x:  float32(vp.glX) + (ndx+1)*float32(vp.glW)/2,
			y:  384 - (float32(vp.glY) + (ndy+1)*float32(vp.glH)/2),
			iw: iw, u: c.u * iw, v: c.v * iw, s: c.s * iw,
		}
	}
	for k := 1; k+1 < n; k++ {
		v0, v1, v2 := &p[0], &p[k], &p[k+1]
		x0, y0 := float64(v0.x), float64(v0.y)
		dx1, dy1 := float64(v1.x)-x0, float64(v1.y)-y0
		dx2, dy2 := float64(v2.x)-x0, float64(v2.y)-y0
		area := dx1*dy2 - dx2*dy1
		if math.Abs(area) < 1e-9 || math.IsNaN(area) {
			continue
		}
		t := *base
		t.x = [3]float32{v0.x, v1.x, v2.x}
		t.y = [3]float32{v0.y, v1.y, v2.y}
		a0 := [4]float32{v0.iw, v0.u, v0.v, v0.s}
		a1 := [4]float32{v1.iw, v1.u, v1.v, v1.s}
		a2 := [4]float32{v2.iw, v2.u, v2.v, v2.s}
		for j := 0; j < 4; j++ {
			d1, d2 := float64(a1[j]-a0[j]), float64(a2[j]-a0[j])
			gx := (d1*dy2 - d2*dy1) / area
			gy := (d2*dx1 - d1*dx2) / area
			t.p[j] = [3]float32{float32(float64(a0[j]) - x0*gx - y0*gy), float32(gx), float32(gy)}
		}
		ymin := min(t.y[0], t.y[1], t.y[2])
		ymax := max(t.y[0], t.y[1], t.y[2])
		if ymax < -1e6 || ymin > 1e6 {
			continue
		}
		t.y0 = max(vp.ry0, int(math.Ceil(float64(ymin)-0.5)))
		t.y1 = min(vp.ry1, int(math.Ceil(float64(ymax)-0.5)))
		if t.y0 >= t.y1 {
			continue
		}
		*list = append(*list, t)
	}
}

// ---------------------------------------------------------------- bands

func (r *renderer) drawBand(y0, y1 int) {
	i0, i1 := y0*screenW, y1*screenW
	if r.white {
		for i := i0; i < i1; i++ {
			r.col[i] = rgba{1, 1, 1, 1}
			r.t1[i], r.t2[i] = rgba{}, rgba{}
		}
		return
	}
	clear(r.col[i0:i1])
	clear(r.t1[i0:i1])
	clear(r.t2[i0:i1])
	r.drawAmbientFog(y0, y1)
	r.drawScrollFog(y0, y1)
	for pri := 0; pri < 4; pri++ {
		if !r.hasModels[pri] {
			continue
		}
		for ov := 0; ov < 2; ov++ {
			clear(r.depth[i0:i1])
			clear(r.sten[i0:i1])
			list := r.tris[pri][ov]
			for i := range list {
				if list[i].layers&layerColour != 0 {
					r.raster(&list[i], 0, y0, y1)
				}
			}
			if ov == 0 {
				r.lineOfSight(pri, y0, y1)
			}
			copy(r.depthCopy[i0:i1], r.depth[i0:i1])
			copy(r.stenCopy[i0:i1], r.sten[i0:i1])
			for i := range list {
				if list[i].layers&layerTrans1 != 0 {
					r.raster(&list[i], 1, y0, y1)
				}
			}
			copy(r.depth[i0:i1], r.depthCopy[i0:i1])
			copy(r.sten[i0:i1], r.stenCopy[i0:i1])
			for i := range list {
				if list[i].layers&layerTrans2 != 0 {
					r.raster(&list[i], 2, y0, y1)
				}
			}
			if !r.hasOverlay[pri] {
				break
			}
		}
	}
}

func (r *renderer) lineOfSight(pri, y0, y1 int) {
	for _, vp := range r.vps {
		if vp.pri != pri || (vp.losX == 0 && vp.losY == 0) {
			continue
		}
		row := vp.losY - 1
		if row >= y0 && row < y1 && vp.losX >= 0 && vp.losX < screenW {
			i := row*screenW + vp.losX
			z := math.Float32bits(r.depth[i])
			if r.sten[i]&0x80 == 0 {
				z |= 1
			} else {
				z &^= 1
			}
			r.losBack[pri] = math.Float32frombits(z)
		}
		return
	}
}

func (r *renderer) fillRect(vp *viewport, y0, y1 int, f func(px, py int) rgba) {
	for py := max(y0, vp.ry0); py < min(y1, vp.ry1); py++ {
		for px := vp.rx0; px < vp.rx1; px++ {
			r.col[py*screenW+px] = f(px, py)
		}
	}
}

func (r *renderer) drawAmbientFog(y0, y1 int) {
	amb := float32(1)
	var node *viewport
	for i := 0; i < 4; i++ {
		has := false
		for _, vp := range r.vps {
			if vp.pri != i {
				continue
			}
			has = true
			if vp.scrollFog > 0 || (vp.fogDensity <= 0 && vp.fogStart <= 0) {
				continue
			}
			if vp.fogAmbient < amb {
				node, amb = vp, vp.fogAmbient
			}
		}
		if node != nil || has {
			break
		}
	}
	if node != nil {
		c := rgba{0, 0, 0, 1 - amb}
		r.fillRect(node, y0, y1, func(int, int) rgba { return c })
	}
}

func (r *renderer) drawScrollFog(y0, y1 int) {
	var node *viewport
	for i := 0; i < 4 && node == nil; i++ {
		for _, vp := range r.vps {
			if vp.pri == i && vp.scrollFog > 0 {
				if node == nil || node.scrollFog < vp.scrollFog {
					node = vp
				}
			}
		}
	}
	if node == nil {
		return
	}
	for _, n := range r.vps {
		if n.fogColour != node.fogColour {
			continue
		}
		spotOn := n.spotFogColor != [3]float32{}
		fog := n.fogDensity > 0 || n.fogStart > 0 || n.scrollAtt > 0
		spotFog := n.fogAtt > 0 && spotOn
		if !fog && !spotFog {
			continue
		}
		att := n.fogAtt
		if fog && spotFog {
			att = 0
		}
		fc := node.fogColour
		amb := n.fogAmbient
		sc := n.spotFogColor
		se := n.spotEllipse
		a := node.scrollFog
		r.fillRect(node, y0, y1, func(px, py int) rgba {
			ex := (float32(px) + 0.5 - se[0]) / se[2]
			ey := (384 - (float32(py) + 0.5) - se[1]) / se[3]
			e := max(0, 1-(ex*ex+ey*ey))
			var c rgba
			for k := 0; k < 3; k++ {
				c[k] = fc[k]*amb + sc[k]*e*fc[k]*att
			}
			c[3] = a
			return c
		})
		return
	}
}

// ---------------------------------------------------------------- pixels

var (
	specExp  = [4]float64{8, 16, 32, 64}
	specMult = [4]float32{1.6, 1.6, 2.4, 3.2}
)

func (r *renderer) raster(t *tri, layer int, b0, b1 int) {
	ys, ye := max(t.y0, b0), min(t.y1, b1)
	if ys >= ye {
		return
	}
	dc := t.dc
	a := dc.a
	vp := dc.vp
	var dst []rgba
	switch layer {
	case 0:
		dst = r.col
	case 1:
		dst = r.t1
	default:
		dst = r.t2
	}
	spot := vp.spotLight || vp.spotFog
	for py := ys; py < ye; py++ {
		yc := float32(py) + 0.5
		xl, xr := float32(math.Inf(1)), float32(math.Inf(-1))
		for e := 0; e < 3; e++ {
			xi, yi := t.x[e], t.y[e]
			xj, yj := t.x[(e+1)%3], t.y[(e+1)%3]
			if yi == yj {
				continue
			}
			lo, hi := min(yi, yj), max(yi, yj)
			if yc < lo || yc >= hi {
				continue
			}
			x := xi + (yc-yi)*(xj-xi)/(yj-yi)
			xl = min(xl, x)
			xr = max(xr, x)
		}
		if !(xl <= xr) {
			continue
		}
		px0 := max(vp.rx0, int(math.Ceil(float64(xl)-0.5)))
		px1 := min(vp.rx1, int(math.Ceil(float64(xr)-0.5)))
		if px0 >= px1 {
			continue
		}
		row := py * screenW
		fx0 := float32(px0) + 0.5
		iw := t.p[0][0] + fx0*t.p[0][1] + yc*t.p[0][2]
		uw := t.p[1][0] + fx0*t.p[1][1] + yc*t.p[1][2]
		vw := t.p[2][0] + fx0*t.p[2][1] + yc*t.p[2][2]
		sw := t.p[3][0] + fx0*t.p[3][1] + yc*t.p[3][2]
		for px := px0; px < px1; px, iw, uw, vw, sw = px+1, iw+t.p[0][1], uw+t.p[1][1], vw+t.p[2][1], sw+t.p[3][1] {
			if iw <= 0 {
				continue
			}
			i := row + px
			var pass bool
			if layer == 0 {
				pass = iw >= r.depth[i]
			} else {
				pass = iw > r.depth[i]
			}
			if !pass && !a.layered {
				continue
			}
			st := r.sten[i]
			if a.layered && st&0x7F != 0 {
				continue
			}
			z := 1 / iw

			// colour
			fr, fg, fb, fa := t.col[0], t.col[1], t.col[2], t.col[3]
			if a.textured {
				tx := r.sample(dc, uw*z, vw*z, z, t.lodBase)
				if a.alphaTest && tx[3] < 32.0/255 {
					continue
				}
				if a.texAlpha {
					if layer == 0 {
						if tx[3] < 1 {
							continue
						}
					} else if tx[3]*t.col[3] >= 1 {
						continue
					}
				}
				if !a.texAlpha && !a.alphaTest {
					tx[3] = 1
				}
				fr, fg, fb, fa = fr*tx[0], fg*tx[1], fb*tx[2], fa*tx[3]
			}
			if fa < 1.0/32 {
				continue
			}
			var lobe, lobeFog float32
			if spot {
				ex := (float32(px) + 0.5 - vp.spotEllipse[0]) / vp.spotEllipse[2]
				ey := (384 - yc - vp.spotEllipse[1]) / vp.spotEllipse[3]
				el := max(0, 1-(ex*ex+ey*ey))
				var en float32
				if z >= vp.spotRange[0] {
					en = 1
				}
				var rng float32
				if vp.spotRange[1] != 0 {
					ab := float32(math.Abs(float64(vp.spotRange[1])))
					d := min(vp.spotRange[0]+ab-z, 0)
					q := d/(1+ab) - 1
					rng = 1 / (q * q) * en
				}
				lobe = rng * el
				lobeFog = en * el
			}
			if a.lighting {
				sf := sw * z
				sf = min(max(sf, -1), 1)
				if vp.sunClamp || fa < 0.999 {
					sf = max(sf, 0)
				}
				li := sf*vp.sunI + vp.amb
				lr, lg, lb := li+vp.spotColor[0]*lobe, li+vp.spotColor[1]*lobe, li+vp.spotColor[2]*lobe
				if r.gpu.step == 0x10 {
					lr, lg, lb = min(lr, 1), min(lg, 1), min(lb, 1)
				}
				fr, fg, fb = fr*lr, fg*lg, fb*lb
				if a.specular {
					var sp float32
					if a.smooth {
						sp = float32(math.Pow(float64(max(0, sf)), specExp[a.shininess])) * specMult[a.shininess]
					} else {
						sp = t.specFlat
					}
					sp *= a.specVal * vp.sunI
					if t.col[3] < 1 {
						fa = max(fa, sp)
					}
					fr, fg, fb = fr+sp, fg+sp, fb+sp
				}
			}
			fr, fg, fb = min(fr, 1), min(fg, 1), min(fb, 1)
			if fogA := a.fogIntensity * clamp01(vp.fogStart+z*vp.fogDensity); fogA != 0 {
				k := vp.fogAtt * lobeFog
				fogR := vp.fogColour[0]*vp.fogAmbient + vp.spotFogColor[0]*k*vp.fogColour[0]
				fogG := vp.fogColour[1]*vp.fogAmbient + vp.spotFogColor[1]*k*vp.fogColour[1]
				fogB := vp.fogColour[2]*vp.fogAmbient + vp.spotFogColor[2]*k*vp.fogColour[2]
				fr += (fogR - fr) * fogA
				fg += (fogG - fg) * fogA
				fb += (fogB - fb) * fogA
			}

			if a.layered {
				r.sten[i] = st&0x80 | min(st&0x7F+1, 0x7F)
			}
			if !pass {
				continue
			}
			r.depth[i] = iw
			if !a.layered {
				if a.noLos {
					r.sten[i] |= 0x80
				} else {
					r.sten[i] &^= 0x80
				}
			}
			if layer == 0 {
				fa = 1
			}
			dst[i] = rgba{fr, fg, fb, fa}
		}
	}
}

// ---------------------------------------------------------------- textures

// texLUT: each texture format's texels as RGBA8 (R in the top byte)
var (
	texLUT     *[12][65536]uint32
	texLUTOnce sync.Once
	byteF      [256]float32
)

func buildTexLUT() {
	texLUT = new([12][65536]uint32)
	for i := range byteF {
		byteF[i] = float32(i) / 255
	}
	n4 := func(v uint32) uint32 { return v & 0xF * 17 }
	pack := func(r, g, b, a uint32) uint32 { return r<<24 | g<<16 | b<<8 | a }
	for f := 0; f < 12; f++ {
		for vi := 0; vi < 65536; vi++ {
			v := uint32(vi)
			var c uint32
			switch f {
			case 0:
				c5 := func(x uint32) uint32 { return (x&0x1F*255 + 15) / 31 }
				a := uint32(255)
				if v&0x8000 != 0 {
					a = 0
				}
				c = pack(c5(v>>10), c5(v>>5), c5(v), a)
			case 1:
				l := n4(v)
				c = pack(l, l, l, n4(v>>4))
			case 2:
				l := n4(v >> 4)
				c = pack(l, l, l, n4(v))
			case 3:
				l := n4(v >> 8)
				c = pack(l, l, l, n4(v>>12))
			case 4:
				l := n4(v >> 12)
				c = pack(l, l, l, n4(v>>8))
			case 5, 6, 8, 9, 10, 11:
				var l uint32
				switch f {
				case 5:
					l = v & 0xFF
				case 6:
					l = v >> 8 & 0xFF
				case 8:
					l = n4(v)
				case 9:
					l = n4(v >> 4)
				case 10:
					l = n4(v >> 8)
				case 11:
					l = n4(v >> 12)
				}
				a := uint32(255)
				if l == 255 {
					a = 0
				}
				c = pack(l, l, l, a)
			case 7:
				c = pack(n4(v>>12), n4(v>>8), n4(v>>4), n4(v))
			}
			texLUT[f][vi] = c
		}
	}
}

func floorf(x float32) float32 { return float32(math.Floor(float64(x))) }
func fract(x float32) float32  { return x - floorf(x) }

// texLoc: the two texels to blend along one axis, and the weight of the second
func texLoc(mode, size int, u float32) (int, int, float32) {
	fs := float32(size)
	switch mode {
	case 0:
		u = u*fs - 0.5
		f := floorf(u)
		i0 := int(f) % size
		if i0 < 0 {
			i0 += size
		}
		return i0, (i0 + 1) % size, u - f
	case 1:
		u = fract(u)
	default:
		odd := floorf(u - 2*floorf(u/2))
		if odd > 0 {
			u = 1 - fract(u)
		} else {
			u = fract(u)
		}
	}
	u = u*fs - 0.5
	f := floorf(u)
	i0 := int(f)
	i1 := i0 + 1
	if i0 < 0 {
		i0 = 0
	}
	if i1 >= size {
		i1 = size - 1
	}
	return i0, i1, u - f
}

func unpack(c uint32) rgba {
	return rgba{byteF[c>>24], byteF[c>>16&0xFF], byteF[c>>8&0xFF], byteF[c&0xFF]}
}

func (r *renderer) bilinear(lut *[65536]uint32, page, level, posX, posY, w, h, wrapU, wrapV int, u, v float32, alphaTest bool) rgba {
	w, h = max(1, w>>level), max(1, h>>level)
	posX, posY = posX>>level, posY>>level
	x0, x1, fa := texLoc(wrapU, w, u)
	y0, y1, fb := texLoc(wrapV, h, v)
	mx, my := 2047>>level, 1023>>level
	bx, by := int(mipXBase[level]), page*1024+int(mipYBase[level])
	tex := r.gpu.tex
	at := func(x, y int) rgba {
		return unpack(lut[tex[(by+(y+posY)&my)*texW+bx+(x+posX)&mx]])
	}
	p00, p10, p01, p11 := at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)
	if alphaTest {
		if p00[3] > p10[3] {
			p10[0], p10[1], p10[2] = p00[0], p00[1], p00[2]
		}
		if p00[3] > p01[3] {
			p01[0], p01[1], p01[2] = p00[0], p00[1], p00[2]
		}
		if p10[3] > p00[3] {
			p00[0], p00[1], p00[2] = p10[0], p10[1], p10[2]
		}
		if p10[3] > p11[3] {
			p11[0], p11[1], p11[2] = p10[0], p10[1], p10[2]
		}
		if p01[3] > p00[3] {
			p00[0], p00[1], p00[2] = p01[0], p01[1], p01[2]
		}
		if p01[3] > p11[3] {
			p11[0], p11[1], p11[2] = p01[0], p01[1], p01[2]
		}
		if p11[3] > p01[3] {
			p01[0], p01[1], p01[2] = p11[0], p11[1], p11[2]
		}
		if p11[3] > p10[3] {
			p10[0], p10[1], p10[2] = p11[0], p11[1], p11[2]
		}
	}
	var c rgba
	for k := 0; k < 4; k++ {
		q0 := p00[k] + (p10[k]-p00[k])*fa
		q1 := p01[k] + (p11[k]-p01[k])*fa
		c[k] = q0 + (q1-q0)*fb
	}
	return c
}

var (
	microX = [8]int{0, 0, 128, 128, 0, 0, 128, 128}
	microY = [8]int{0, 128, 0, 128, 256, 384, 256, 384}
	microS = [4]float32{2, 4, 16, 256}
)

// sample: the texture colour at texel coordinates (u, v), z units from the eye
func (r *renderer) sample(dc *drawConst, u, v, z, lodBase float32) rgba {
	a := dc.a
	lut := &texLUT[a.format]
	nu, nv := u/float32(a.w), v/float32(a.h)
	lod := float32(-math.Log2(float64(lodBase)) + 2*math.Log2(float64(z)))
	levels := float32(math.Floor(math.Log2(float64(min(a.w, a.h))))) - 1
	fl := lod
	if fl != fl || fl < 0 {
		fl = 0
	}
	if fl > levels {
		fl = levels
	}
	il := int(fl)
	c := r.bilinear(lut, dc.page, il, dc.texX, dc.texY, a.w, a.h, a.wrapU, a.wrapV, nu, nv, a.alphaTest)
	if ff := fl - float32(il); ff > 0 {
		c2 := r.bilinear(lut, dc.page, il+1, dc.texX, dc.texY, a.w, a.h, a.wrapU, a.wrapV, nu, nv, a.alphaTest)
		for k := 0; k < 4; k++ {
			c[k] += (c2[k] - c[k]) * ff
		}
	} else if a.micro && lod < 0 {
		s := microS[int(a.microMinLOD)]
		c2 := r.bilinear(lut, (dc.page+1)&1, 0, microX[a.microID], microY[a.microID], 128, 128, 0, 0, u*s/128, v*s/128, a.alphaTest)
		bl := min(-lod*float32(math.Exp2(-float64(a.microMinLOD)))*0.5, 0.5)
		for k := 0; k < 4; k++ {
			c[k] += (c2[k] - c[k]) * bl
		}
	}
	if a.inverted {
		c[0], c[1], c[2] = 1-c[0], 1-c[1], 1-c[2]
	}
	return c
}

// ---------------------------------------------------------------- output

// composite merges the bottom tile layers, the 3D picture and the top tile
// layers into dst (0xRRGGBB).
func (r *renderer) composite(bottom, top, dst []uint32) {
	for i := range dst {
		var br, bg, bb float32
		if b := bottom[i]; b != 0 {
			br, bg, bb = byteF[b>>16&0xFF], byteF[b>>8&0xFF], byteF[b&0xFF]
		}
		c := &r.col[i]
		if c[3] != 0 {
			br += (c[0] - br) * c[3]
			bg += (c[1] - bg) * c[3]
			bb += (c[2] - bb) * c[3]
		}
		t1, t2 := &r.t1[i], &r.t2[i]
		var t rgba
		switch {
		case t1[3]*t2[3] > 0:
			k := (t2[3] + (1 - t1[3])) / 2
			t = rgba{t1[0] + (t2[0]-t1[0])*k, t1[1] + (t2[1]-t1[1])*k, t1[2] + (t2[2]-t1[2])*k, 1}
		case t1[3] > 0:
			t = *t1
		default:
			t = *t2
		}
		if t[3] != 0 {
			ta := min(t[3], 1)
			br += (t[0] - br) * ta
			bg += (t[1] - bg) * ta
			bb += (t[2] - bb) * ta
		}
		out := to8(br)<<16 | to8(bg)<<8 | to8(bb)
		if tp := top[i]; tp != 0 {
			out = tp & 0xFFFFFF
		}
		dst[i] = out
	}
}

func to8(v float32) uint32 {
	if v <= 0 {
		return 0
	}
	if v >= 1 {
		return 255
	}
	return uint32(v*255 + 0.5)
}
