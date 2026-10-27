package model3

import (
	"encoding/binary"
	"math"
)

// The Real3D scene: the renderer walks the viewport list in culling RAM, the
// tree of culling nodes under each viewport (matrices, level of detail,
// texture offsets) and the models (lists of polygons in polygon RAM or VROM)
// they point to. This follows the scene format worked out by the Supermodel
// project.

const nearPlane = 1e-3

type mat4 [16]float32 // column major, as OpenGL

func identity() mat4 { return mat4{0: 1, 5: 1, 10: 1, 15: 1} }

// mul: a = a*b
func (a *mat4) mul(b *mat4) {
	var r mat4
	for i := 0; i < 4; i++ {
		a0, a1, a2, a3 := a[i], a[4+i], a[8+i], a[12+i]
		for j := 0; j < 4; j++ {
			r[j*4+i] = a0*b[j*4] + a1*b[j*4+1] + a2*b[j*4+2] + a3*b[j*4+3]
		}
	}
	*a = r
}

func f32(v uint32) float32 { return math.Float32frombits(v) }

// proFloat16: the Real3D's 16-bit float
func proFloat16(v uint32) float32 {
	a := v << 15
	e := (a & 0x7E000000) >> 25
	if e <= 31 {
		e += 127
	} else {
		e += 127 - 64
	}
	return f32(a&0x80000000 | e<<23 | (a&0x1FFFFFF)>>2)
}

func clamp01(v float32) float32 {
	if v < 0 {
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}

// ---------------------------------------------------------------- models

// polyAttr: the drawing state of a group of polygons (a "mesh")
type polyAttr struct {
	textured, alphaTest, texAlpha, polyAlpha bool
	lighting, fixedShading, smooth           bool
	highPri, transSel, layered               bool
	specular, translatorMap, noLos, inverted bool
	micro                                    bool
	shininess                                int
	specVal, fogIntensity                    float32
	format, x, y, w, h, page                 int
	wrapU, wrapV                             int
	microID                                  int
	microMinLOD                              float32
}

type pvert struct {
	pos   [3]float32
	n     [3]float32
	u, v  float32 // texels (raw coordinate times the UV scale)
	ru    uint16  // raw coordinates
	rv    uint16
	shade float32
}

// mtri: one triangle of a model
type mtri struct {
	a   *polyAttr
	v   [3]pvert
	fn  [3]float32
	col [4]uint8
	np  float32
}

type model struct {
	tris    []mtri
	highPri bool
}

func newAttr(h *[7]uint32) *polyAttr {
	a := &polyAttr{}
	a.textured = h[6]&0x400 != 0
	a.alphaTest = h[6]&0x80000000 != 0
	a.texAlpha = h[6]&7 != 0
	a.polyAlpha = h[6]&0x800000 == 0
	a.lighting = h[6]&0x10000 == 0
	a.smooth = h[1]&8 != 0
	a.fixedShading = h[1]&0x20 != 0 && !a.smooth
	a.highPri = h[6]&0x10 != 0
	a.transSel = h[6]&0x20000 != 0
	a.layered = h[6]&8 != 0
	a.specular = h[0]&0x80 != 0
	a.shininess = int(h[6] >> 5 & 3)
	a.specVal = float32(h[0]>>26) / 63
	a.fogIntensity = float32(h[6]>>11&0x1F) / 16
	a.translatorMap = h[4]&0x80 != 0
	a.noLos = h[1]&1 != 0
	if a.textured {
		f := int(h[6] >> 7 & 7)
		if a.alphaTest && f >= 1 && f <= 4 {
			f += 7
		}
		a.format = f
		if f == 7 {
			a.alphaTest = false
		}
		a.x = int(32*((h[4]&0x1F)<<1|(h[5]>>7&1))) & 2047
		a.y = int(32*(h[5]&0x1F)) & 2047
		tw, th := h[3]>>3&7, h[3]&7
		if tw >= 6 {
			tw = 0
		}
		if th >= 6 {
			th = 0
		}
		a.w, a.h = 32<<tw, 32<<th
		a.micro = h[2]&0x10 != 0
		a.inverted = h[6]>>24&0x7F == 2
		a.page = int(h[4] & 0x40 >> 6)
		smoothU, smoothV := h[3]&0x80 != 0, h[3]&0x40 != 0
		if h[6]&0x80000000 != 0 {
			smoothU, smoothV = false, false
		}
		wrap := func(mirror, smooth bool) int {
			switch {
			case mirror && smooth:
				return 2
			case mirror:
				return 3
			case smooth:
				return 0
			}
			return 1
		}
		a.wrapU = wrap(h[2]&2 != 0, smoothU)
		a.wrapV = wrap(h[2]&1 != 0, smoothV)
		if a.micro {
			a.microID = int(h[2] >> 5 & 7)
			a.microMinLOD = float32(h[2] >> 2 & 3)
		}
	}
	return a
}

func polyHash(h *[7]uint32) uint64 {
	b := func(c bool) uint64 {
		if c {
			return 1
		}
		return 0
	}
	x := uint64(h[3] & 0xFF)
	x |= uint64((h[4]&0x1F)<<1|(h[5]>>7&1)) << 8
	x |= uint64(h[5]&0x1F) << 14
	x |= uint64((h[4]&0xC0)>>6) << 19
	x |= b(h[1]&0x10 != 0) << 21
	x |= b(h[6]&0x80000000 != 0) << 22
	x |= b(h[6]&0x800000 == 0) << 23
	x |= uint64(h[2]&0xFF) << 24
	x |= b(h[0]&0x80 != 0) << 32
	x |= b(h[1]&8 != 0) << 33
	x |= b(h[1]&0x20 != 0) << 34
	x |= uint64(h[0]>>26) << 35
	x |= uint64(h[6]&0x3FFFF) << 41
	x |= b(h[1]&1 != 0) << 59
	return x
}

var sharedVerts = [16]int{0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4}

func byteToFloat(b uint32) float32 { return (2*float32(int8(b)) + 1) / 255 }

// decodeModel reads the polygons of a model. It reports whether the model
// takes its colours from the colour table (and so can't be cached).
func (r *renderer) decodeModel(word func(uint32) uint32, addr uint32) (*model, bool) {
	m := &model{}
	dynamic := false
	meshes := map[uint64]*polyAttr{}
	var cur *polyAttr
	lastHash := ^uint64(0)
	var raw [4][2]uint16
	var h [7]uint32
	for n := 0; n < 65536; n++ {
		for i := range h {
			h[i] = word(addr + uint32(i))
		}
		if h[6] == 0 {
			break
		}
		if h[1]&2 == 0 {
			dynamic = true
		}
		hash := polyHash(&h)
		if hash != lastHash {
			if a := meshes[hash]; a != nil {
				cur = a
			} else {
				cur = newAttr(&h)
				meshes[hash] = cur
			}
		}
		if cur.highPri {
			m.highPri = true
		}
		nv := 3
		if h[0]&0x40 != 0 {
			nv = 4
		}
		uvScale := float32(1.0 / 8)
		if h[1]&0x40 != 0 {
			uvScale = 1
		}
		var fn [3]float32
		for i := 0; i < 3; i++ {
			fn[i] = float32(int32(h[1+i])>>8) * (1.0 / 4194304.0)
		}
		var v [4]pvert
		v[3] = r.prev[3] // left over from the polygon before, as on the hardware's emulation
		j := 0
		for i := 0; i < 4; i++ {
			if h[0]&(1<<i) != 0 {
				v[j] = r.prev[i]
				raw[j] = r.prevRaw[i]
				if hash != lastHash {
					v[j].u = float32(raw[j][0]) * uvScale
					v[j].v = float32(raw[j][1]) * uvScale
				}
				j++
			}
		}
		lastHash = hash
		var col [4]uint8
		if h[1]&2 == 0 {
			c := r.polyWord(r.colorTable + (h[4] >> 8 & 0xFFF))
			col[0], col[1], col[2] = uint8(c>>16), uint8(c>>8), uint8(c)
		} else {
			col[0], col[1], col[2] = uint8(h[4]>>24), uint8(h[4]>>16), uint8(h[4]>>8)
		}
		if h[6]&0x800000 != 0 {
			col[3] = 255
		} else {
			col[3] = uint8((h[6] >> 18 & 0x3F) * 255 / 32)
		}
		disc1, disc2 := h[0]&0x200 != 0, h[0]&0x100 != 0
		if disc1 && !disc2 {
			col[3] /= 2
		}
		if !cur.smooth {
			for i := 0; i < nv; i++ {
				v[i].n = fn
			}
		}
		np := float32(h[5]>>8) * r.npFactor
		data := addr + 7
		for ; j < nv; j++ {
			ix, iy, iz, it := word(data), word(data+1), word(data+2), word(data+3)
			v[j].pos = [3]float32{float32(int32(ix)>>8) * r.vertexFactor, float32(int32(iy)>>8) * r.vertexFactor, float32(int32(iz)>>8) * r.vertexFactor}
			if cur.smooth {
				v[j].n = [3]float32{byteToFloat(ix & 0xFF), byteToFloat(iy & 0xFF), byteToFloat(iz & 0xFF)}
			}
			if cur.fixedShading {
				if cur.specular {
					v[j].shade = float32(ix&0xFF) / 255
				} else {
					v[j].shade = byteToFloat(ix & 0xFF)
				}
			}
			raw[j] = [2]uint16{uint16(it >> 16), uint16(it)}
			v[j].u = float32(raw[j][0]) * uvScale
			v[j].v = float32(raw[j][1]) * uvScale
			data += 4
		}
		discard := disc1 && disc2
		if h[1]&0x10 != 0 && !discard {
			fv := v
			for i := 0; i < nv; i++ {
				fv[i].n = [3]float32{-fv[i].n[0], -fv[i].n[1], -fv[i].n[2]}
			}
			m.add(cur, &fv, nv, [3]float32{-fn[0], -fn[1], -fn[2]}, col, np)
		}
		if !discard {
			m.add(cur, &v, nv, fn, col, np)
		}
		r.prev = v
		r.prevRaw = raw
		if h[1]&4 != 0 {
			break
		}
		addr += 7 + uint32((nv-sharedVerts[h[0]&0xF])*4)
	}
	return m, dynamic
}

func (m *model) add(a *polyAttr, v *[4]pvert, nv int, fn [3]float32, col [4]uint8, np float32) {
	if col[3] == 0 {
		return
	}
	m.tris = append(m.tris, mtri{a: a, v: [3]pvert{v[0], v[1], v[2]}, fn: fn, col: col, np: np})
	if nv == 4 {
		m.tris = append(m.tris, mtri{a: a, v: [3]pvert{v[0], v[2], v[3]}, fn: fn, col: col, np: np})
	}
}

// ---------------------------------------------------------------- scene

type viewport struct {
	pri                   int
	vpX, vpY, vpW, vpH    float32
	left, right, top, bot float32
	cota                  float32
	// window rectangle (top-down rows)
	rx0, ry0, rx1, ry1 int
	glX, glY, glW, glH int
	px, py, pa, pb     float32 // projection

	sun                  [3]float32
	sunI, amb            float32
	sunClamp             bool
	spotEllipse          [4]float32
	spotRange            [2]float32
	spotColor            [3]float32
	spotFogColor         [3]float32
	spotLight, spotFog   bool
	fogColour            [3]float32
	fogDensity, fogStart float32
	fogAtt, fogAmbient   float32
	scrollFog, scrollAtt float32
	losX, losY           int

	draws []modelDraw
}

type modelDraw struct {
	mat        mat4
	texX, texY int
	page       int
	scale      float32
	alpha      float32
	m          *model
}

type nodeAttr struct {
	texX, texY, page int
	scale, alpha     float32
	noCull           bool
}

type planes struct{ lu, lv, tu, tw, ru, rv, bu, bw float32 }

var spotColours = [8][3]float32{{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {1, 0, 1}, {1, 1, 0}, {1, 1, 1}}

// cullWord reads culling RAM at a word address (0 when it isn't there)
func (r *renderer) cullWord(a uint32) uint32 {
	a &= 0xFFFFFF
	if a >= 0x800000 && a < 0x840000 {
		return r.gpu.cullHi[a&0x3FFFF]
	}
	if a < 0x100000 {
		return r.gpu.cullLo[a]
	}
	return 0
}

func cullValid(a uint32) bool {
	a &= 0xFFFFFF
	return (a >= 0x800000 && a < 0x840000) || a < 0x100000
}

func (r *renderer) polyWord(a uint32) uint32 {
	if a < uint32(len(r.gpu.poly)) {
		return r.gpu.poly[a]
	}
	return 0
}

func (r *renderer) vromWord(a uint32) uint32 {
	o := uint64(a) * 4 % uint64(len(r.gpu.vrom))
	return binary.LittleEndian.Uint32(r.gpu.vrom[o:])
}

func (r *renderer) buildScene() {
	r.vps = r.vps[:0]
	r.visits = 0
	addr := uint32(0x800000)
	for n := 0; n < 256 && addr&0xFFFFFF != 0 && cullValid(addr); n++ {
		r.viewportNode(addr)
		next := r.cullWord(addr + 1)
		if next == 0x01000000 {
			break
		}
		addr = next
	}
}

func (r *renderer) viewportNode(addr uint32) {
	w := func(i uint32) uint32 { return r.cullWord(addr + i) }
	r.vps = append(r.vps, &viewport{})
	vp := r.vps[len(r.vps)-1]
	vp.pri = int(w(0) >> 3 & 3)
	vp.vpX = float32(w(0x1A)&0xFFFF) / 16
	vp.vpY = float32(w(0x1A)>>16) / 16
	vp.vpW = float32(w(0x14)&0xFFFF) / 4
	vp.vpH = float32(w(0x14)>>16) / 4
	matBase := w(0x16) & 0xFFFFFF
	r.lodTable = w(0x17) & 0xFFFFFF
	cv, cw := f32(w(8)), f32(w(9))
	io, jo := f32(w(0xA)), f32(w(0xB))
	r.planes = planes{f32(w(0xC)), f32(w(0xD)), f32(w(0xE)), f32(w(0xF)), f32(w(0x10)), f32(w(0x11)), f32(w(0x12)), f32(w(0x13))}
	vp.left = (0 - jo) / cv
	vp.right = (1 - jo) / cv
	vp.bot = -(1 - io) / cw
	vp.top = -(0 - io) / cw
	vp.cota = f32(w(3))

	// window and projection
	vp.glX = int(vp.vpX)
	vp.glY = int(384 - (vp.vpY + vp.vpH))
	vp.glW = int(vp.vpW)
	vp.glH = int(vp.vpH)
	vp.rx0, vp.rx1 = max(0, vp.glX), min(screenW, vp.glX+vp.glW)
	vp.ry0, vp.ry1 = max(0, 384-vp.glY-vp.glH), min(screenH, 384-vp.glY)
	vp.px = 2 / (vp.right - vp.left)
	vp.py = 2 / (vp.top - vp.bot)
	vp.pa = (vp.right + vp.left) / (vp.right - vp.left)
	vp.pb = (vp.top + vp.bot) / (vp.top - vp.bot)

	vp.sun = [3]float32{f32(w(5)), -f32(w(6)), -f32(w(4))}
	vp.sunI = min(max(f32(w(7)), 0), 1)
	if f32(w(7)) != f32(w(7)) {
		vp.sunI = 0
	}
	vp.amb = float32(w(0x24)>>8&0xFF) / 255
	vp.sunClamp = r.gpu.modeword[4]&0x40000 == 0

	spotIdx, spotFogIdx := w(0x20)>>11&7, w(0x20)>>8&7
	vp.spotEllipse[0] = float32(int16(w(0x1E))) / 8
	vp.spotEllipse[1] = 384 - float32(int16(w(0x1D)))/8
	vp.spotEllipse[2] = float32(math.Round(float64(2047 / max(1, float32(w(0x1E)>>16)))))
	vp.spotEllipse[3] = float32(math.Round(float64(2047 / max(1, float32(w(0x1D)>>16)))))
	vp.spotRange = [2]float32{1 / f32(w(0x21)), f32(w(0x1F))}
	vp.spotColor = spotColours[spotIdx]
	vp.spotFogColor = spotColours[spotFogIdx]
	vp.losX = int(float32(w(0x1C)&0xFFFF)/16 + 0.5)
	vp.losY = int(float32(w(0x1C)>>16)/16 + 0.5)
	vp.fogColour = [3]float32{float32(w(0x22)>>16&0xFF) / 255, float32(w(0x22)>>8&0xFF) / 255, float32(w(0x22)&0xFF) / 255}
	vp.fogDensity = float32(math.Abs(float64(f32(w(0x23)))))
	if math.IsInf(float64(vp.fogDensity), 0) || vp.fogDensity != vp.fogDensity {
		vp.fogDensity = 0
	}
	vp.fogStart = float32(int16(w(0x25))) / 255
	vp.fogAtt = float32(w(0x24)>>16&0xFF) / 255
	vp.fogAmbient = float32(w(0x25)>>16&0xFF) / 255
	vp.scrollFog = float32(w(0x20)&0xFF) / 255
	vp.scrollAtt = float32(w(0x24)&0xFF) / 255
	vp.spotLight = vp.spotColor != [3]float32{} && vp.spotRange[1] != 0
	vp.spotFog = vp.spotFogColor != [3]float32{} && vp.fogAtt > 0

	// coordinate system
	m := mat4{4: 1, 9: -1, 2: -1, 15: 1}
	r.matBase = matBase
	r.matBaseOK = cullValid(matBase)
	r.multMatrix(0, &m)
	r.curVP = vp
	if w(0)&0x20 == 0 {
		child := w(2)
		if child>>24&5 == 0 {
			r.nodePtr(child, &m, nodeAttr{scale: 1, alpha: 1}, 0)
		}
	}
}

func (r *renderer) multMatrix(off uint32, m *mat4) {
	if !r.matBaseOK {
		return
	}
	b := r.matBase + off*12
	var s [12]float32
	for i := range s {
		s[i] = f32(r.cullWord(b + uint32(i)))
	}
	t := mat4{
		0: s[3], 4: s[4], 8: s[5], 12: s[0],
		1: s[6], 5: s[7], 9: s[8], 13: s[1],
		2: s[9], 6: s[10], 10: s[11], 14: s[2],
		15: 1,
	}
	m.mul(&t)
}

func resetMatrix(mat *mat4) {
	m := *mat
	m[1], m[4] = m[4], m[1]
	m[2], m[8] = m[8], m[2]
	m[6], m[9] = m[9], m[6]
	m[12], m[13], m[14], m[15] = 0, 0, 0, 1
	s1 := float32(math.Sqrt(float64(m[0]*m[0] + m[1]*m[1] + m[2]*m[2])))
	s2 := float32(math.Sqrt(float64(m[4]*m[4] + m[5]*m[5] + m[6]*m[6])))
	s3 := float32(math.Sqrt(float64(m[8]*m[8] + m[9]*m[9] + m[10]*m[10])))
	m[0] /= s1
	m[1] /= s1
	m[2] /= s1
	m[4] /= s2
	m[5] /= s2
	m[6] /= s2
	m[8] /= s3
	m[9] /= s3
	m[10] /= s3
	mat.mul(&m)
}

func (r *renderer) nodePtr(a uint32, m *mat4, at nodeAttr, depth int) {
	if a&0xFFFFFF == 0 {
		return
	}
	switch a >> 24 & 5 {
	case 0:
		r.cullingNode(a&0xFFFFFF, m, at, depth)
	case 1:
		r.drawModel(a&0xFFFFFF, m, at)
	case 4:
		l := a & 0xFFFFFF
		if !cullValid(l) {
			return
		}
		for i := uint32(0); i < 4096; i++ {
			e := r.cullWord(l + i)
			if e&0x01000000 != 0 {
				break
			}
			r.cullingNode(e&0xFFFFFF, m, at, depth)
			if e&0x02000000 != 0 {
				break
			}
		}
	}
}

func (r *renderer) lod(ptr uint32, i int) (del, blend float32) {
	a := r.lodTable + ptr*8 + uint32(i)*2
	return f32(r.cullWord(a)), f32(r.cullWord(a + 1))
}

func nodeAlphaClamp(v float32) float32 {
	if v != v {
		return 1
	}
	return clamp01(v)
}

func (r *renderer) cullingNode(addr uint32, parent *mat4, at nodeAttr, depth int) {
	r.visits++
	if depth > 128 || r.visits > 200000 || !cullValid(addr) {
		return
	}
	w := func(i uint32) uint32 { return r.cullWord(addr + i) }
	n0 := w(0)
	if n0&3 == 0 || n0&0x300 == 0x300 {
		return
	}
	child1 := w(7) & 0x7FFFFFF
	sib := w(8) & 0x1FFFFFF
	matOff := w(3) & 0xFFF
	lodPtr := w(3) >> 12 & 0x7F

	if n0&7 != 6 && sib&0x1000000 == 0 && sib != 0 {
		r.cullingNode(sib, parent, at, depth+1)
	}
	if n0&4 != 0 {
		r.colorTable = (w(3)>>19 | (w(7)>>28)<<13 | (w(8)>>25)<<17) & 0xFFFFF
	}
	if w(1)&1 != 0 {
		at.scale = f32(w(1) &^ 3)
	}
	if w(1)&2 != 0 {
		at.noCull = true
	}
	if w(2)&0x8000 != 0 {
		at.texX = 32 * int(w(2)>>7&0x3F)
		at.texY = 32 * int(w(2)&0x1F)
		at.page = int(w(2) & 0x4000 >> 14)
	}
	m := *parent
	if n0&0x10 != 0 {
		t := identity()
		t[12], t[13], t[14] = f32(w(4)), f32(w(5)), f32(w(6))
		m.mul(&t)
	} else if matOff != 0 {
		r.multMatrix(matOff, &m)
	}
	if n0&0x80 != 0 {
		resetMatrix(&m)
	}
	x, y, z := m[12], m[13], m[14]
	cullR := proFloat16(w(9)&0xFFFF) * at.scale
	blendR := proFloat16(w(9)>>16) * at.scale
	p := &r.planes
	outside := z*p.lu-x*p.lv > cullR || z*p.tu+y*p.tw > cullR || z*p.ru-x*p.rv > cullR || z*p.bu+y*p.bw > cullR

	lodScale := float32(math.MaxFloat32)
	if !at.noCull {
		lodScale = blendR / float32(math.Sqrt(float64(x*x+y*y+z*z)))
	}
	if lodScale < 0 {
		lodScale = 0
	}
	if lodScale > math.MaxFloat32 {
		lodScale = math.MaxFloat32
	}
	del3, blend3 := r.lod(lodPtr, 3)
	if !(at.noCull || (!outside && lodScale >= del3)) {
		return
	}
	if n0&8 != 0 {
		lp := child1
		if !cullValid(lp) {
			return
		}
		ml := 0
		for ; ml < 3; ml++ {
			d, _ := r.lod(lodPtr, ml)
			if lodScale >= d && r.cullWord(lp+uint32(ml))&0x1000000 != 0 {
				break
			}
		}
		d, b := r.lod(lodPtr, ml)
		temp := at.alpha
		na := nodeAlphaClamp(b * (lodScale - d))
		if na > 31.0/32 {
			na = 1
		} else if na < 1.0/32 {
			na = 0
		}
		at.alpha *= na
		node := w(3)&0x20000000 != 0
		visit := func(e uint32, at nodeAttr) {
			if node {
				r.cullingNode(e&0xFFFFFF, &m, at, depth+1)
			} else {
				r.drawModel(e&0xFFFFFF, &m, at)
			}
		}
		visit(r.cullWord(lp+uint32(ml)), at)
		if na < 1 && ml != 3 {
			at.alpha = (1 - na) * temp
			visit(r.cullWord(lp+uint32(ml)+1), at)
		}
		return
	}
	at.alpha *= nodeAlphaClamp(blend3 * (lodScale - del3))
	r.nodePtr(child1, &m, at, depth+1)
}

func (r *renderer) drawModel(addr uint32, m *mat4, at nodeAttr) {
	var mod *model
	if addr >= 0x100000 {
		if c, ok := r.romModels[addr]; ok {
			mod = c
		} else {
			var dyn bool
			mod, dyn = r.decodeModel(r.vromWord, addr)
			if !dyn {
				r.romModels[addr] = mod
			}
		}
	} else {
		mod, _ = r.decodeModel(r.polyWord, addr)
	}
	vp := r.curVP
	vp.draws = append(vp.draws, modelDraw{mat: *m, texX: at.texX, texY: at.texY, page: at.page, scale: at.scale, alpha: at.alpha, m: mod})
}
