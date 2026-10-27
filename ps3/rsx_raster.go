package ps3

// Rasterising: primitive assembly, near-plane clipping, viewport,
// triangle setup with perspective-correct attributes, the fragment
// program, alpha/depth tests, blending and the colour mask.

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"sync"
)

type rasterVert struct {
	x, y, z float32 // window
	invW    float32
	attr    [16]vec4 // VP outputs divided by w
}

type rasterCtx struct {
	m        *Machine
	r        *rsxState
	d        *rsxDraw
	s        *rsxSurface
	fp       *fpProg
	tex      [16]rsxTexture
	used     [16]int // VP outputs to interpolate
	nused    int
	x0, y0   int
	x1, y1   int
	bpp      uint32
	depth    bool
	depthW   bool
	depthFn  uint32
	z16      bool
	alphaT   bool
	alphaFn  uint32
	alphaRef float32
	blend    bool
	sfC, sfA uint32
	dfC, dfA uint32
	eqC, eqA uint32
	bcol     vec4
	cmask    uint32
	mrtBlend [3]bool
	mrtMask  [3]uint32
	out32    bool
	fixedOut bool
	cull     bool
	cullFace uint32
	frontCCW bool
	wpos     bool
	pix      int
}

func (m *Machine) loadFP(r *rsxState, d *rsxDraw) *fpProg {
	v := r.reg(0x8E4)
	loc := (v & 3) - 1
	off := v &^ 3
	a := m.gcm.offsetToAddr(off, loc)
	if a == 0 || !m.Mem.Mapped(a) {
		return nil
	}
	// cache by address and the first words (programs get rewritten)
	key := a
	if p, ok := d.fpCache[key]; ok && d.fpCheck(m, a, p) {
		return p
	}
	p := m.decodeFP(a)
	d.fpCache[key] = p
	if fpCaptureDir != "" {
		buf := make([]byte, p.size)
		m.Mem.ReadBytes(a, buf)
		os.WriteFile(fmt.Sprintf("%s/fp_%016x.bin", fpCaptureDir, p.shape), buf, 0o644)
	}
	return p
}

// fpCheck tells whether a cached program still matches memory.
func (d *rsxDraw) fpCheck(m *Machine, a uint32, p *fpProg) bool {
	if len(p.inst) == 0 {
		return false
	}
	q := m.decodeFP(a)
	if len(q.inst) != len(p.inst) {
		return false
	}
	for i := range q.inst {
		if q.inst[i] != p.inst[i] {
			return false
		}
	}
	return true
}

// fpCaptureDir (PS3_FPCAPTURE) collects every fragment program the game
// uses, for the shader translation tests.
var fpCaptureDir = os.Getenv("PS3_FPCAPTURE")

// FP input register -> VP output register
var fpInputToVP = [16]int{-1, 1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 6, -1, -1}

func (m *Machine) rasterPrims(r *rsxState, d *rsxDraw, s *rsxSurface, verts []rsxVertex, vlist []int) {
	fp := m.loadFP(r, d)
	if fp == nil {
		return
	}
	c := &rasterCtx{m: m, r: r, d: d, s: s, fp: fp}
	c.bpp = surfBpp(s.fmt)
	for i := 0; i < 16; i++ {
		if fp.usesTex>>i&1 != 0 {
			c.tex[i] = m.loadTexture(r, i)
		}
	}
	for in := 1; in < 14; in++ {
		if fp.inputs>>in&1 != 0 && fpInputToVP[in] >= 0 {
			c.used[c.nused] = fpInputToVP[in]
			c.nused++
		}
	}
	c.wpos = fp.inputs&1 != 0
	c.x0, c.y0, c.x1, c.y1 = s.x, s.y, s.x+s.w, s.y+s.h
	sh, sv := r.reg(0x8C0), r.reg(0x8C4)
	if sw, shh := int(sh>>16), int(sv>>16); sw > 0 && shh > 0 && sw < 4096 && shh < 4096 {
		c.x0, c.y0 = max(c.x0, int(sh&0xffff)), max(c.y0, int(sv&0xffff))
		c.x1, c.y1 = min(c.x1, int(sh&0xffff)+sw), min(c.y1, int(sv&0xffff)+shh)
	}
	c.depth = r.reg(0xA74) != 0 && s.zAddr != 0 && s.zPitch >= 16 && s.zFmt != 0
	c.depthW = r.reg(0xA70) != 0 && r.reg(0xA74) != 0 && s.zAddr != 0 && s.zPitch >= 16 && s.zFmt != 0
	c.depthFn = r.reg(0xA6C)
	c.z16 = s.zFmt == 1
	c.alphaT = r.reg(0x304) != 0
	c.alphaFn = r.reg(0x308)
	c.alphaRef = float32(r.reg(0x30C)&0xff) / 255
	c.blend = r.reg(0x310) != 0
	sf, df := r.reg(0x314), r.reg(0x318)
	c.sfC, c.sfA, c.dfC, c.dfA = sf&0xffff, sf>>16, df&0xffff, df>>16
	eq := r.reg(0x320)
	c.eqC, c.eqA = eq&0xffff, eq>>16
	bc := r.reg(0x31C)
	c.bcol = vec4{float32(bc>>16&0xff) / 255, float32(bc>>8&0xff) / 255, float32(bc&0xff) / 255, float32(bc>>24) / 255}
	c.cmask = colorMask(r.reg(0x324))
	for k := 0; k < s.nmrt; k++ {
		c.mrtBlend[k] = r.reg(0x36C)>>(k+1)&1 != 0
		c.mrtMask[k] = r.reg(0x370) >> (4 * (k + 1)) & 0xf
	}
	c.out32 = r.reg(0x1D60)&0x40 != 0
	c.fixedOut = s.fmt != 11 && s.fmt != 12 && s.fmt != 13
	c.cull = r.reg(0x183C) != 0
	c.cullFace = r.reg(0x1830)
	c.frontCCW = r.reg(0x1834) == 0x901
	anyMask := c.cmask
	for k := 0; k < s.nmrt; k++ {
		anyMask |= c.mrtMask[k]
	}
	if anyMask == 0 && !c.depthW {
		return
	}
	// viewport
	sx, sy, sz := r.regF(0xA30), r.regF(0xA34), r.regF(0xA38)
	ox, oy, oz := r.regF(0xA20), r.regF(0xA24), r.regF(0xA28)
	toWin := func(v *vec4) rasterVert {
		w := v[3]
		if w == 0 {
			w = 1e-6
		}
		iw := 1 / w
		return rasterVert{x: v[0]*iw*sx + ox, y: v[1]*iw*sy + oy, z: v[2]*iw*sz + oz, invW: iw}
	}
	emit := func(a, b, cc int) {
		if a < 0 || b < 0 || cc < 0 {
			return
		}
		c.clipTriangle(&verts[a], &verts[b], &verts[cc], toWin)
	}
	n := len(vlist)
	switch d.prim {
	case 5: // triangles
		for i := 0; i+2 < n; i += 3 {
			emit(vlist[i], vlist[i+1], vlist[i+2])
		}
	case 6: // strip
		k := 0
		for i := 0; i < n; i++ {
			if vlist[i] < 0 {
				k = 0
				continue
			}
			if k >= 2 {
				if k%2 == 0 {
					emit(vlist[i-2], vlist[i-1], vlist[i])
				} else {
					emit(vlist[i-1], vlist[i-2], vlist[i])
				}
			}
			k++
		}
	case 7, 10: // fan, polygon
		start := 0
		for i := 0; i < n; i++ {
			if vlist[i] < 0 {
				start = i + 1
				continue
			}
			if i-start >= 2 {
				emit(vlist[start], vlist[i-1], vlist[i])
			}
		}
	case 8: // quads
		for i := 0; i+3 < n; i += 4 {
			emit(vlist[i], vlist[i+1], vlist[i+2])
			emit(vlist[i], vlist[i+2], vlist[i+3])
		}
	case 9: // quad strip
		for i := 0; i+3 < n; i += 2 {
			emit(vlist[i], vlist[i+1], vlist[i+3])
			emit(vlist[i], vlist[i+3], vlist[i+2])
		}
	}
	if rsxTrace {
		m.Logf("  %d pixels", c.pix)
	}
}

// clipTriangle clips against w > 0 (near) and rasterises the pieces.
func (c *rasterCtx) clipTriangle(a, b, d *rsxVertex, toWin func(*vec4) rasterVert) {
	const eps = 1e-5
	in := [3]*[16]vec4{&a.out, &b.out, &d.out}
	if a.out[0][3] > eps && b.out[0][3] > eps && d.out[0][3] > eps {
		c.setupTri(in[0], in[1], in[2], toWin)
		return
	}
	// Sutherland-Hodgman on w = eps
	var poly [][16]vec4
	for i := 0; i < 3; i++ {
		p, q := in[i], in[(i+1)%3]
		pw, qw := p[0][3], q[0][3]
		if pw > eps {
			poly = append(poly, *p)
		}
		if (pw > eps) != (qw > eps) {
			t := (eps - pw) / (qw - pw)
			var o [16]vec4
			for k := range o {
				for j := 0; j < 4; j++ {
					o[k][j] = p[k][j] + (q[k][j]-p[k][j])*t
				}
			}
			poly = append(poly, o)
		}
	}
	for i := 2; i < len(poly); i++ {
		c.setupTri(&poly[0], &poly[i-1], &poly[i], toWin)
	}
}

func (c *rasterCtx) setupTri(a, b, d *[16]vec4, toWin func(*vec4) rasterVert) {
	va, vb, vd := toWin(&a[0]), toWin(&b[0]), toWin(&d[0])
	for i := 0; i < c.nused; i++ {
		k := c.used[i]
		for j := 0; j < 4; j++ {
			va.attr[k][j] = a[k][j] * va.invW
			vb.attr[k][j] = b[k][j] * vb.invW
			vd.attr[k][j] = d[k][j] * vd.invW
		}
	}
	// signed area (window y grows down)
	area := (vb.x-va.x)*(vd.y-va.y) - (vd.x-va.x)*(vb.y-va.y)
	if area == 0 || area != area {
		return
	}
	if c.cull {
		// with y down, a positive area is clockwise on screen
		cw := area > 0
		front := cw != c.frontCCW
		// RSX/GL: front face CCW in a y-up window; our y is flipped
		switch c.cullFace {
		case 0x404:
			if front {
				return
			}
		case 0x405:
			if !front {
				return
			}
		case 0x408:
			return
		}
	}
	c.drawTri(&va, &vb, &vd, area)
}

func (c *rasterCtx) drawTri(a, b, d *rasterVert, area float32) {
	minX := int(math.Floor(float64(min(a.x, b.x, d.x))))
	maxX := int(math.Ceil(float64(max(a.x, b.x, d.x))))
	minY := int(math.Floor(float64(min(a.y, b.y, d.y))))
	maxY := int(math.Ceil(float64(max(a.y, b.y, d.y))))
	minX, minY = max(minX, c.x0), max(minY, c.y0)
	maxX, maxY = min(maxX, c.x1-1), min(maxY, c.y1-1)
	if minX > maxX || minY > maxY {
		return
	}
	inv := 1 / area
	rows := maxY - minY + 1
	if rows*(maxX-minX+1) < 16384 || rows < 8 {
		c.pix += c.drawRows(a, b, d, inv, minY, maxY, minX, maxX)
		return
	}
	// big triangles: bands of rows in parallel (pixels don't overlap)
	s := c.s
	for py := minY; py <= maxY; py++ {
		row := s.addr + uint32(py)*s.pitch
		c.m.Mem.Ptr(row + uint32(minX)*c.bpp)
		c.m.Mem.Ptr(row + uint32(maxX)*c.bpp)
		if c.depth || c.depthW {
			zr := s.zAddr + uint32(py)*s.zPitch
			c.m.Mem.Ptr(zr + uint32(minX)*4)
			c.m.Mem.Ptr(zr + uint32(maxX)*4)
		}
	}
	n := rasterWorkers
	var wg sync.WaitGroup
	counts := make([]int, n)
	for i := 0; i < n; i++ {
		y0 := minY + rows*i/n
		y1 := minY + rows*(i+1)/n - 1
		wg.Add(1)
		go func(i, y0, y1 int) {
			defer wg.Done()
			counts[i] = c.drawRows(a, b, d, inv, y0, y1, minX, maxX)
		}(i, y0, y1)
	}
	wg.Wait()
	for _, k := range counts {
		c.pix += k
	}
}

var rasterWorkers = max(1, min(runtime.NumCPU(), 8))

func (c *rasterCtx) drawRows(a, b, d *rasterVert, inv float32, minY, maxY, minX, maxX int) int {
	pix := 0
	var inputs [16]vec4
	var x fpRun
	var fin [16]vec4
	x.in = &fin
	samp := c.sample
	s := c.s
	for py := minY; py <= maxY; py++ {
		fy := float32(py) + 0.5
		rowAddr := s.addr + uint32(py)*s.pitch
		for px := minX; px <= maxX; px++ {
			fx := float32(px) + 0.5
			w0 := ((b.x-fx)*(d.y-fy) - (d.x-fx)*(b.y-fy)) * inv
			w1 := ((d.x-fx)*(a.y-fy) - (a.x-fx)*(d.y-fy)) * inv
			w2 := 1 - w0 - w1
			if w0 < 0 || w1 < 0 || w2 < 0 {
				// top-left-ish rule: accept exact zero edges
				if w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6 {
					continue
				}
			}
			z := w0*a.z + w1*b.z + w2*d.z
			// depth test
			var zaddr uint32
			var zold uint32
			if c.depth || c.depthW {
				if c.z16 {
					zaddr = s.zAddr + uint32(py)*s.zPitch + uint32(px)*2
					zold = uint32(c.m.Mem.Read16(zaddr))
				} else {
					zaddr = s.zAddr + uint32(py)*s.zPitch + uint32(px)*4
					zold = c.m.Mem.Read32(zaddr)
				}
			}
			zc := min(max(z, 0), 1)
			var znew uint32
			if c.z16 {
				znew = uint32(zc * 65535)
			} else {
				znew = uint32(zc*16777215) << 8
			}
			if c.depth {
				zo, zn := zold, znew
				if !c.z16 {
					zo, zn = zold>>8, znew>>8
				}
				if !depthPass(c.depthFn, zn, zo) {
					continue
				}
			}
			// inputs
			iw := w0*a.invW + w1*b.invW + w2*d.invW
			rw := 1 / iw
			for i := 0; i < c.nused; i++ {
				k := c.used[i]
				for j := 0; j < 4; j++ {
					inputs[k][j] = (w0*a.attr[k][j] + w1*b.attr[k][j] + w2*d.attr[k][j]) * rw
				}
			}
			fin = [16]vec4{}
			for in := 1; in < 14; in++ {
				if c.fp.inputs>>in&1 != 0 && fpInputToVP[in] >= 0 {
					fin[in] = inputs[fpInputToVP[in]]
				}
			}
			if c.wpos {
				fin[0] = vec4{fx, fy, z, iw}
			}
			fin[14] = vec4{1, 1, 1, 1}
			x.killed = false
			for i := range x.r[:5] {
				x.r[i] = vec4{}
			}
			for i := range x.h[:9] {
				x.h[i] = vec4{}
			}
			x.cc = [2]vec4{}
			c.fp.run(&x, samp)
			if x.killed {
				continue
			}
			col := x.h[0]
			if c.out32 {
				col = x.r[0]
			}
			if c.fixedOut {
				col = fpClamp(col, 4) // 8-bit targets take 0..1
			}
			if c.alphaT && !alphaPass(c.alphaFn, col[3], c.alphaRef) {
				continue
			}
			if c.depthW {
				if c.z16 {
					c.m.Mem.Write16(zaddr, uint16(znew))
				} else {
					c.m.Mem.Write32(zaddr, znew|zold&0xff)
				}
			}
			for k := 0; k < s.nmrt; k++ {
				if c.mrtMask[k] == 0 {
					continue
				}
				col := x.h[4+2*k]
				if c.out32 {
					col = x.r[2+k]
				}
				if c.fixedOut {
					col = fpClamp(col, 4)
				}
				pa := s.mrt[k].addr + uint32(py)*s.mrt[k].pitch + uint32(px)*c.bpp
				if c.mrtBlend[k] {
					col = c.blendPix(col, c.m.readPixel(s, pa))
				}
				c.m.writePixel(s, pa, col, c.mrtMask[k])
			}
			if c.cmask == 0 {
				continue
			}
			pa := rowAddr + uint32(px)*c.bpp
			if c.blend {
				dst := c.m.readPixel(s, pa)
				col = c.blendPix(col, dst)
			}
			c.m.writePixel(s, pa, col, c.cmask)
			pix++
		}
	}
	return pix
}

func (c *rasterCtx) sample(unit uint8, co vec4, bias, lod float32, useLod bool) vec4 {
	t := &c.tex[unit&15]
	return t.sample(co[0], co[1])
}

func depthPass(fn, n, o uint32) bool {
	switch fn {
	case 0x200:
		return false
	case 0x201:
		return n < o
	case 0x202:
		return n == o
	case 0x203:
		return n <= o
	case 0x204:
		return n > o
	case 0x205:
		return n != o
	case 0x206:
		return n >= o
	}
	return true
}

func alphaPass(fn uint32, a, ref float32) bool {
	switch fn {
	case 0x200:
		return false
	case 0x201:
		return a < ref
	case 0x202:
		return a == ref
	case 0x203:
		return a <= ref
	case 0x204:
		return a > ref
	case 0x205:
		return a != ref
	case 0x206:
		return a >= ref
	}
	return true
}

func blendFactor(f uint32, s, d, k vec4, i int) float32 {
	switch f {
	case 0:
		return 0
	case 1:
		return 1
	case 0x300:
		return s[i]
	case 0x301:
		return 1 - s[i]
	case 0x302:
		return s[3]
	case 0x303:
		return 1 - s[3]
	case 0x304:
		return d[3]
	case 0x305:
		return 1 - d[3]
	case 0x306:
		return d[i]
	case 0x307:
		return 1 - d[i]
	case 0x308:
		if i == 3 {
			return 1
		}
		return min(s[3], 1-d[3])
	case 0x8001:
		return k[i]
	case 0x8002:
		return 1 - k[i]
	case 0x8003:
		return k[3]
	case 0x8004:
		return 1 - k[3]
	}
	return 1
}

func (c *rasterCtx) blendPix(s, d vec4) vec4 {
	var o vec4
	for i := 0; i < 4; i++ {
		sf, df, eq := c.sfC, c.dfC, c.eqC
		if i == 3 {
			sf, df, eq = c.sfA, c.dfA, c.eqA
		}
		a := s[i] * blendFactor(sf, s, d, c.bcol, i)
		b := d[i] * blendFactor(df, s, d, c.bcol, i)
		switch eq {
		case 0x8007:
			o[i] = min(s[i], d[i])
		case 0x8008:
			o[i] = max(s[i], d[i])
		case 0x800A:
			o[i] = a - b
		case 0x800B:
			o[i] = b - a
		default:
			o[i] = a + b
		}
	}
	return o
}
