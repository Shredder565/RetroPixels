package wiiu

// Latte draws: running the vertex shader, putting primitives together,
// clipping, the viewport, rasterizing triangles in 2x2 quads, the pixel
// shader, then depth, stencil, alpha test and blending into the colour
// buffers.

import (
	"math"
)

// vtxOut is a shaded vertex.
type vtxOut struct {
	pos   [4]float32 // clip space
	param [32][4]uint32
	psize float32
	// after the viewport
	sx, sy, sz, rw float32
}

// stageEnv connects a shader run to the GPU's state.
type stageEnv struct {
	g      *Latte
	stage  int
	deriv  bool // texture LOD from neighbouring lanes (pixel shaders)
	fsAddr uint32
	// what the current run exports into
	vtx    [nl]*vtxOut
	colors [8][4]lanes
	wrote  [8]bool
	depth  lanes
	zOut   bool
	nParam uint32
}

func (e *stageEnv) read32(addr uint32) uint32 { return e.g.read32(addr) }

func (e *stageEnv) cfile(i, ch uint32) uint32 {
	return e.g.reg(regSQ_ALU_CONSTANT0_0 + ((stageConstBase[e.stage]+i&0xff)*4+ch)*4)
}

var kcacheRegs = [3]uint32{regSQ_ALU_CONST_CACHE_PS_0, regSQ_ALU_CONST_CACHE_VS_0, regSQ_ALU_CONST_CACHE_GS_0}

func (e *stageEnv) kcache(bank, i, ch uint32) uint32 {
	base := e.g.reg(kcacheRegs[e.stage]+bank*4) << 8
	return e.g.read32(base + i*16 + ch*4)
}

func (e *stageEnv) loopConst(i uint32) uint32 {
	return e.g.reg(regSQ_LOOP_CONST_0 + (uint32(e.stage)*32+i&31)*4)
}

func (e *stageEnv) boolConst(i uint32) bool {
	return e.g.reg(regSQ_BOOL_CONST_0+uint32(e.stage)*4)>>(i&31)&1 != 0
}

func (e *stageEnv) fetchShader() uint32 { return e.fsAddr }

func (e *stageEnv) export(s *shaderRun, typ, base uint32, v *[4]lanes, mask laneMask) {
	switch e.stage {
	case stageVS:
		for l := 0; l < nl; l++ {
			o := e.vtx[l]
			if !mask[l] || o == nil {
				continue
			}
			switch {
			case typ == 1 && base == 60:
				o.pos = [4]float32{u2f(v[0][l]), u2f(v[1][l]), u2f(v[2][l]), u2f(v[3][l])}
			case typ == 1 && base == 61:
				o.psize = u2f(v[0][l])
			case typ == 2 && base < 32:
				o.param[base] = [4]uint32{v[0][l], v[1][l], v[2][l], v[3][l]}
			}
		}
	case stagePS:
		switch {
		case typ == 0 && base < 8:
			for l := 0; l < nl; l++ {
				if mask[l] {
					for c := 0; c < 4; c++ {
						e.colors[base][c][l] = v[c][l]
					}
				}
			}
			e.wrote[base] = true
		case typ == 0 && base == 61:
			for l := 0; l < nl; l++ {
				if mask[l] {
					e.depth[l] = v[0][l]
				}
			}
			e.zOut = true
		}
	}
}

// ---- vertex fetch ----

func (e *stageEnv) fetch(s *shaderRun, w0, w1, w2 uint32, mask laneMask) {
	g := e.g
	inst := w0 & 0x1f
	fetchType := w0 >> 5 & 3
	buf := w0 >> 8 & 0xff
	srcGPR := w0 >> 16 & 0x7f
	srcRel := w0>>23&1 != 0
	srcSel := w0 >> 24 & 3
	dstGPR := w1 & 0x7f
	dstRel := w1>>7&1 != 0
	if inst == 1 { // VTX_SEMANTIC: the destination comes from SQ_VTX_SEMANTIC
		sem := w1 & 0xff
		dstGPR, dstRel = 0xffff, false
		for i := uint32(0); i < 32; i++ {
			if g.reg(regSQ_VTX_SEMANTIC_0+i*4)&0xff == sem {
				dstGPR = i + 1
				break
			}
		}
		if dstGPR == 0xffff {
			return
		}
	}
	dsel := [4]uint32{w1 >> 9 & 7, w1 >> 12 & 7, w1 >> 15 & 7, w1 >> 18 & 7}
	useConst := w1>>21&1 != 0
	format := w1 >> 22 & 0x3f
	numFormat := w1 >> 28 & 3
	signed := w1>>30&1 != 0
	offset := w2 & 0xffff
	endian := w2 >> 16 & 3
	res := g.resource(stageResBase[e.stage] + buf)
	base := res[0]
	size := res[1] + 1
	stride := res[2] >> 8 & 0x7ff
	if useConst {
		format = res[2] >> 20 & 0x3f
		numFormat = res[2] >> 26 & 3
		signed = res[2]>>28&1 != 0
		endian = res[2] >> 30
	}
	kind := numKindFromFetch(numFormat, signed, false)
	n := fmtBytes(format)
	var out [4]lanes
	var b [16]byte
	for l := 0; l < nl; l++ {
		if !mask[l] {
			continue
		}
		r := srcGPR
		if srcRel {
			r += s.ar[0][l]
		}
		idx := s.gpr[r&127][srcSel][l]
		switch fetchType {
		case 0:
			idx += g.reg(regSQ_VTX_BASE_VTX_LOC)
		case 1:
			idx += g.reg(regSQ_VTX_START_INST_LOC)
		}
		off := idx*stride + offset
		var v [4]uint32
		if off+n <= size || size <= 1 {
			g.mem.ReadBytes(base+off, b[:n])
			swapBytes(b[:n], endian)
			v = decodeElem(format, kind, b[:n])
		} else {
			v = [4]uint32{0, 0, 0, f2u(1)}
			if kind == kUint || kind == kSint {
				v[3] = 1
			}
		}
		for c := 0; c < 4; c++ {
			out[c][l] = v[c]
		}
	}
	s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
}

// ---- texture fetch ----

func (e *stageEnv) tex(s *shaderRun, w0, w1, w2 uint32, mask laneMask) {
	g := e.g
	inst := w0 & 0x1f
	resID := w0 >> 8 & 0xff
	srcGPR := w0 >> 16 & 0x7f
	srcRel := w0>>23&1 != 0
	dstGPR := w1 & 0x7f
	dstRel := w1>>7&1 != 0
	dsel := [4]uint32{w1 >> 9 & 7, w1 >> 12 & 7, w1 >> 15 & 7, w1 >> 18 & 7}
	lodBias := float32(int32(w1<<4)>>25) / 16
	norm := [4]bool{w1>>28&1 != 0, w1>>29&1 != 0, w1>>30&1 != 0, w1>>31 != 0}
	offX := float32(int32(w2<<27)>>27) / 2
	offY := float32(int32(w2<<22)>>27) / 2
	samplerID := w2 >> 15 & 0x1f
	ssel := [4]uint32{w2 >> 20 & 7, w2 >> 23 & 7, w2 >> 26 & 7, w2 >> 29 & 7}

	t := texFromWords(g.resource(stageResBase[e.stage] + resID))
	smp := g.sampler(stageSamplerBase[e.stage] + samplerID)
	var src [4]lanes
	for l := 0; l < nl; l++ {
		r := srcGPR
		if srcRel {
			r += s.ar[0][l]
		}
		for c := 0; c < 4; c++ {
			src[c][l] = s.selVal(r, ssel[c], l)
		}
	}
	var out [4]lanes
	if !t.valid {
		s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
		return
	}
	intFmt := t.kind == kUint || t.kind == kSint
	switch inst {
	case 4: // GET_TEXTURE_RESINFO
		for l := 0; l < nl; l++ {
			lvl := src[0][l]
			out[0][l] = max(t.width>>lvl, 1)
			out[1][l] = max(t.height>>lvl, 1)
			out[2][l] = t.slices()
			out[3][l] = t.lastLevel - t.baseLevel + 1
		}
		s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
		return
	case 7, 8: // GET_GRADIENTS_H / V
		for c := 0; c < 4; c++ {
			for l := 0; l < nl; l++ {
				var d float32
				if inst == 7 {
					d = u2f(src[c][l|1]) - u2f(src[c][l&^1])
				} else {
					d = u2f(src[c][l|2]) - u2f(src[c][l&^2])
				}
				out[c][l] = f2u(d)
			}
		}
		s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
		return
	case 3: // LD: integer texel coordinates, level in w
		for l := 0; l < nl; l++ {
			if !mask[l] {
				continue
			}
			lvl := t.baseLevel + src[3][l]
			lv := g.texLevel(&t, min(lvl, t.lastLevel))
			x := min(int32(src[0][l]), int32(lv.w)-1)
			y := min(int32(src[1][l]), int32(lv.h)-1)
			z := min(int32(src[2][l]), int32(lv.d)-1)
			if t.dim == 0 || t.dim == 4 {
				z = y
				y = 0
				if t.dim == 0 {
					z = 0
				}
				z = min(z, int32(lv.d)-1)
			}
			v := applySel(lv.texel(max(x, 0), max(y, 0), max(z, 0)), t.dstSel, intFmt)
			for c := 0; c < 4; c++ {
				out[c][l] = v[c]
			}
		}
		s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
		return
	}
	if inst < 0x10 && inst != 6 {
		// other fetch kinds aren't used by what runs yet
		g.warn("texture instruction %d", inst)
		s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
		return
	}
	// sampling: work out the coordinates of every lane (helpers included,
	// so the level of detail can be measured across the quad)
	var reqs [nl]sampleReq
	for l := 0; l < nl; l++ {
		r := &reqs[l]
		u, v, w := u2f(src[0][l]), u2f(src[1][l]), u2f(src[2][l])
		r.normalize = [3]bool{norm[0], norm[1], norm[2]}
		r.off = [3]float32{offX, offY}
		switch t.dim {
		case 0: // 1D
			v = 0.5
			r.normalize[1] = true
		case 3: // cube: faces in z, coordinates 1..2 from the CUBE instruction
			u, v = u-1, v-1
			r.slice = int32(math.Floor(float64(w) + 0.5))
			r.normalize = [3]bool{true, true, false}
		case 4: // 1D array
			r.slice = int32(math.Floor(float64(v) + 0.5))
			v = 0.5
			r.normalize[1] = true
		case 5, 7: // 2D array
			r.slice = int32(math.Floor(float64(w) + 0.5))
		}
		r.u, r.v, r.w = u, v, w
		switch inst {
		case 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f: // SAMPLE_C*
			r.compare = true
			r.ref = u2f(src[3][l])
		}
	}
	// level of detail
	lv0 := g.texLevel(&t, t.baseLevel)
	fw, fh := float32(lv0.w), float32(lv0.h)
	scale := func(l int) (float32, float32) {
		u, v := reqs[l].u, reqs[l].v
		if reqs[l].normalize[0] {
			u *= fw
		}
		if reqs[l].normalize[1] {
			v *= fh
		}
		return u, v
	}
	var lod [nl]float32
	switch inst {
	case 0x11, 0x15, 0x19, 0x1d: // _L: explicit
		for l := range lod {
			lod[l] = u2f(src[3][l])
		}
	case 0x13, 0x17, 0x1b, 0x1f: // _LZ
	default:
		var dudx, dvdx, dudy, dvdy float32
		if inst == 0x14 || inst == 0x16 || inst == 0x1c || inst == 0x1e { // _G: gradients set by SET_GRADIENTS
			dudx, dvdx = u2f(s.gradH[0][0])*fw, u2f(s.gradH[1][0])*fh
			dudy, dvdy = u2f(s.gradV[0][0])*fw, u2f(s.gradV[1][0])*fh
		} else if e.deriv {
			u0, v0 := scale(0)
			u1, v1 := scale(1)
			u2, v2 := scale(2)
			dudx, dvdx = u1-u0, v1-v0
			dudy, dvdy = u2-u0, v2-v0
		}
		px := dudx*dudx + dvdx*dvdx
		py := dudy*dudy + dvdy*dvdy
		ll := float32(0)
		if m := max(px, py); m > 0 {
			ll = float32(0.5 * math.Log2(float64(m)))
		} else {
			ll = -16
		}
		for l := range lod {
			lod[l] = ll
			if inst == 0x12 || inst == 0x1a { // _LB: bias in w
				lod[l] += u2f(src[3][l])
			}
		}
	}
	for l := 0; l < nl; l++ {
		if !mask[l] {
			continue
		}
		reqs[l].lod = lod[l] + lodBias + smp.lodBias
		if inst == 6 { // GET_COMP_TEX_LOD
			out[0][l] = f2u(reqs[l].lod)
			out[1][l] = f2u(reqs[l].lod)
			continue
		}
		v := g.sample(&t, &smp, &reqs[l])
		if !reqs[l].compare {
			v = applySel(v, t.dstSel, intFmt)
		}
		for c := 0; c < 4; c++ {
			out[c][l] = v[c]
		}
	}
	s.writeFetch(dstGPR, dstRel, dsel, &out, mask)
}

// ---- the draw ----

type drawState struct {
	vs, ps, fs  uint32
	vsEnv       stageEnv
	psEnv       stageEnv
	vte, clip   uint32
	scale, offs [3]float32
	zmin, zmax  float32
	sc          [4]int32 // scissor x0, y0, x1, y1 (exclusive)
	mode        uint32   // PA_SU_SC_MODE_CNTL
	cbs         [8]cbDesc
	cbOn        [8]bool
	writeMask   [8]uint32
	blendOn     [8]bool
	blend       [8]uint32
	blendConst  [4]float32
	multiWrite  bool
	db          dbDesc
	dbCtl       uint32
	stRef       [2]uint32
	alphaFunc   uint32
	alphaOn     bool
	alphaRef    float32
	psInputs    []psInput
	posAddr     int32
	faceAddr    int32
	faceChan    uint32
	faceAllBits bool
	flatAll     bool
	killOn      bool
	zExport     bool
	psRun       *shaderRun
	vsRun       *shaderRun
	nVSParam    uint32
	pointSize   float32
	lineWidth   float32
	polyOffset  bool
	offScale    float32
	offUnits    float32
	maxX, maxY  int32
}

type psInput struct {
	param   int // VS parameter, -1 = default value
	def     [4]uint32
	flat    bool
	linear  bool
	sprite  bool
	gpr     uint32
	present bool
}

func (g *Latte) drawState() *drawState {
	st := &drawState{}
	st.vs = g.reg(regSQ_PGM_START_VS) << 8
	st.ps = g.reg(regSQ_PGM_START_PS) << 8
	st.fs = g.reg(regSQ_PGM_START_FS) << 8
	st.vsEnv = stageEnv{g: g, stage: stageVS, fsAddr: st.fs}
	st.psEnv = stageEnv{g: g, stage: stagePS, deriv: true}
	st.vte = g.reg(regPA_CL_VTE_CNTL)
	st.clip = g.reg(regPA_CL_CLIP_CNTL)
	for i := 0; i < 3; i++ {
		st.scale[i] = g.regF(regPA_CL_VPORT_XSCALE_0 + uint32(i)*8)
		st.offs[i] = g.regF(regPA_CL_VPORT_XSCALE_0 + uint32(i)*8 + 4)
	}
	st.zmin, st.zmax = g.regF(regPA_SC_VPORT_ZMIN_0), g.regF(regPA_SC_VPORT_ZMAX_0)
	if st.zmax <= st.zmin {
		st.zmin, st.zmax = 0, 1
	}
	st.mode = g.reg(regPA_SU_SC_MODE_CNTL)

	// render targets
	st.maxX, st.maxY = 1<<14, 1<<14
	cc := g.reg(regCB_COLOR_CONTROL)
	st.multiWrite = cc>>1&1 != 0
	tmask := g.reg(regCB_TARGET_MASK)
	smask := g.reg(regCB_SHADER_MASK)
	perMRT := cc>>7&1 != 0
	for i := 0; i < 8; i++ {
		cb := g.cb(i)
		st.cbs[i] = cb
		st.writeMask[i] = tmask >> (4 * i) & smask >> (4 * i) & 15
		st.cbOn[i] = cb.ok && st.writeMask[i] != 0
		st.blendOn[i] = cc>>(8+i)&1 != 0 && cb.kind != kUint && cb.kind != kSint
		if perMRT {
			st.blend[i] = g.reg(regCB_BLEND0_CONTROL + uint32(i)*4)
		} else {
			st.blend[i] = g.reg(regCB_BLEND_CONTROL)
		}
		if st.cbOn[i] {
			st.maxX = min(st.maxX, int32(cb.lay.Pitch))
			st.maxY = min(st.maxY, int32(cb.lay.Height))
		}
	}
	for i := 0; i < 4; i++ {
		st.blendConst[i] = g.regF(regCB_BLEND_RED + uint32(i)*4)
	}
	st.db = g.db()
	st.dbCtl = g.reg(regDB_DEPTH_CONTROL)
	if st.dbCtl&7 == 0 || !st.db.ok {
		st.db.ok = st.db.ok && st.dbCtl&7 != 0
	}
	if st.db.ok {
		st.maxX = min(st.maxX, int32(st.db.lay.Pitch))
		st.maxY = min(st.maxY, int32(st.db.lay.Height))
	}
	st.stRef = [2]uint32{g.reg(regDB_STENCILREFMASK), g.reg(regDB_STENCILREFMASK_BF)}
	at := g.reg(regSX_ALPHA_TEST_CONTROL)
	st.alphaFunc, st.alphaOn = at&7, at>>3&1 != 0
	st.alphaRef = g.regF(regSX_ALPHA_REF)
	dsc := g.reg(regDB_SHADER_CONTROL)
	st.zExport = dsc&1 != 0
	st.killOn = dsc>>6&1 != 0

	// scissors: screen, window (with the window offset), generic, viewport 0
	sc := [4]int32{0, 0, st.maxX, st.maxY}
	clipTo := func(tl, br uint32, offset bool) {
		x0, y0 := int32(tl&0x7fff), int32(tl>>16&0x7fff)
		x1, y1 := int32(br&0x7fff), int32(br>>16&0x7fff)
		if offset && tl>>31 == 0 {
			wo := g.reg(regPA_SC_WINDOW_OFFSET)
			ox, oy := int32(int16(wo)), int32(int16(wo>>16))
			x0, y0, x1, y1 = x0+ox, y0+oy, x1+ox, y1+oy
		}
		sc[0], sc[1] = max(sc[0], x0), max(sc[1], y0)
		sc[2], sc[3] = min(sc[2], x1), min(sc[3], y1)
	}
	if br := g.reg(regPA_SC_SCREEN_SCISSOR_BR); br != 0 {
		clipTo(g.reg(regPA_SC_SCREEN_SCISSOR_TL), br, false)
	}
	if br := g.reg(regPA_SC_WINDOW_SCISSOR_BR); br != 0 {
		clipTo(g.reg(regPA_SC_WINDOW_SCISSOR_TL), br, false)
	}
	if br := g.reg(regPA_SC_GENERIC_SCISSOR_BR); br != 0 {
		clipTo(g.reg(regPA_SC_GENERIC_SCISSOR_TL), br, true)
	}
	if br := g.reg(regPA_SC_VPORT_SCISSOR_0_BR); br != 0 {
		clipTo(g.reg(regPA_SC_VPORT_SCISSOR_0_TL), br, true)
	}
	st.sc = sc

	// pixel shader inputs
	cfg := g.reg(regSPI_VS_OUT_CONFIG)
	st.nVSParam = cfg>>1&0x1f + 1
	var semOf [32]uint32
	for i := uint32(0); i < 32; i++ {
		semOf[i] = g.reg(regSPI_VS_OUT_ID_0+i/4*4) >> (8 * (i % 4)) & 0xff
	}
	in0 := g.reg(regSPI_PS_IN_CONTROL_0)
	in1 := g.reg(regSPI_PS_IN_CONTROL_1)
	interp := g.reg(regSPI_INTERP_CONTROL_0)
	st.flatAll = false
	nInterp := in0 & 0x3f
	for i := uint32(0); i < nInterp && i < 32; i++ {
		c := g.reg(regSPI_PS_INPUT_CNTL_0 + i*4)
		in := psInput{param: -1, gpr: i, present: true}
		sem := c & 0xff
		for p := uint32(0); p < st.nVSParam && p < 32; p++ {
			if semOf[p] == sem {
				in.param = int(p)
				break
			}
		}
		switch c >> 8 & 3 {
		case 1:
			in.def = [4]uint32{0, 0, 0, f2u(1)}
		case 2:
			in.def = [4]uint32{f2u(1), f2u(1), f2u(1), 0}
		case 3:
			in.def = [4]uint32{f2u(1), f2u(1), f2u(1), f2u(1)}
		}
		in.flat = c>>10&1 != 0 && interp&1 != 0
		in.linear = c>>12&1 != 0
		in.sprite = c>>17&1 != 0 && interp>>1&1 != 0
		st.psInputs = append(st.psInputs, in)
	}
	st.posAddr = -1
	if in0>>8&1 != 0 {
		st.posAddr = int32(in0 >> 10 & 0x1f)
	}
	st.faceAddr = -1
	if in1>>8&1 != 0 {
		st.faceAddr = int32(in1 >> 12 & 0x1f)
		st.faceChan = in1 >> 9 & 3
		st.faceAllBits = in1>>11&1 != 0
	}
	ps := g.reg(regPA_SU_POINT_SIZE)
	st.pointSize = float32(ps>>16) / 8
	if st.pointSize == 0 {
		st.pointSize = 1
	}
	st.lineWidth = max(float32(g.reg(regPA_SU_LINE_CNTL)&0xffff)/8, 1)
	st.polyOffset = st.mode>>11&3 != 0
	st.offScale = g.regF(regPA_SU_POLY_OFFSET_FRONT_SCALE)
	st.offUnits = g.regF(regPA_SU_POLY_OFFSET_FRONT_OFFS)
	st.vsRun = &shaderRun{env: &st.vsEnv}
	st.psRun = &shaderRun{env: &st.psEnv}
	return st
}

func (s *shaderRun) reset() {
	s.stack = s.stack[:0]
	s.loops = s.loops[:0]
	s.calls = s.calls[:0]
	s.callBase = s.callBase[:0]
	s.steps = 0
	s.killed = laneMask{}
	s.helper = laneMask{}
	s.pred = laneMask{}
	s.gradSet = false
}

func (g *Latte) draw(count uint32, index func(uint32) uint32) {
	if count == 0 {
		return
	}
	g.Draws++
	st := g.drawState()
	if st.vs == 0 || (st.ps == 0 && st.clip>>22&1 == 0) {
		return
	}
	prim := g.reg(regVGT_PRIMITIVE_TYPE)
	restartOn := g.reg(regVGT_MULTI_PRIM_IB_RESET_EN)&1 != 0
	restart := g.reg(regVGT_MULTI_PRIM_IB_RESET_INDX)
	idxOff := g.reg(regVGT_INDX_OFFSET)
	insts := max(g.numInstances, 1)
	for inst := uint32(0); inst < insts; inst++ {
		// shade the vertices this instance uses
		cache := map[uint32]*vtxOut{}
		var pending []uint32
		verts := make([]*vtxOut, count)
		flush := func() {
			if len(pending) > 0 {
				g.runVS(st, pending, inst, cache)
				pending = pending[:0]
			}
		}
		ids := make([]uint32, count)
		for i := uint32(0); i < count; i++ {
			id := index(i)
			ids[i] = id
			if restartOn && id == restart {
				continue
			}
			id += idxOff
			ids[i] = id
			if _, ok := cache[id]; !ok {
				cache[id] = nil
				pending = append(pending, id)
				if len(pending) == nl {
					flush()
				}
			}
		}
		flush()
		for i := range verts {
			if restartOn && index(uint32(i)) == restart {
				continue
			}
			verts[i] = cache[ids[i]]
		}
		g.assemble(st, prim, verts)
		for t := 0; t < 8; t++ {
			if st.cbOn[t] {
				cb := &st.cbs[t]
				g.tex.invalidate(cb.lay.Base, cb.lay.Base+cb.lay.sliceBytes()*(cb.slice+1))
			}
		}
		if st.db.ok && st.dbCtl&5 != 0 {
			g.tex.invalidate(st.db.lay.Base, st.db.lay.Base+st.db.lay.sliceBytes()*(st.db.slice+1))
		}
		if st.vsRun.Err != "" || st.psRun.Err != "" {
			g.ShaderErr = st.vsRun.Err + st.psRun.Err
			g.warn("shader: %s", g.ShaderErr)
			return
		}
	}
}

func (g *Latte) runVS(st *drawState, ids []uint32, inst uint32, cache map[uint32]*vtxOut) {
	s := st.vsRun
	s.reset()
	s.valid = laneMask{}
	e := &st.vsEnv
	for l := 0; l < nl; l++ {
		e.vtx[l] = nil
		if l < len(ids) {
			o := &vtxOut{psize: st.pointSize}
			e.vtx[l] = o
			cache[ids[l]] = o
			s.valid[l] = true
			s.gpr[0][0][l] = ids[l]
			s.gpr[0][1][l] = 0
			s.gpr[0][2][l] = 0
			s.gpr[0][3][l] = inst
		}
	}
	s.run(st.vs)
	for l := 0; l < len(ids); l++ {
		g.viewport(st, e.vtx[l])
	}
}

// viewport turns clip-space positions into window coordinates.
func (g *Latte) viewport(st *drawState, o *vtxOut) {
	x, y, z, w := o.pos[0], o.pos[1], o.pos[2], o.pos[3]
	rw := float32(1)
	if st.vte>>8&1 == 0 { // x, y not yet divided by w
		if w != 0 {
			rw = 1 / w
		}
		x, y = x*rw, y*rw
	}
	if st.vte>>9&1 == 0 {
		z *= rw
	}
	if st.vte&1 != 0 {
		x *= st.scale[0]
	}
	if st.vte>>1&1 != 0 {
		x += st.offs[0]
	}
	if st.vte>>2&1 != 0 {
		y *= st.scale[1]
	}
	if st.vte>>3&1 != 0 {
		y += st.offs[1]
	}
	if st.vte>>4&1 != 0 {
		z *= st.scale[2]
	}
	if st.vte>>5&1 != 0 {
		z += st.offs[2]
	}
	o.sx, o.sy, o.sz, o.rw = x, y, z, rw
}

// assemble makes primitives out of the vertices.
func (g *Latte) assemble(st *drawState, prim uint32, v []*vtxOut) {
	last := st.mode>>19&1 != 0 // PROVOKING_VTX_LAST
	tri := func(a, b, c *vtxOut) {
		if a != nil && b != nil && c != nil {
			pv := a
			if last {
				pv = c
			}
			g.clipTriangle(st, a, b, c, pv)
		}
	}
	n := len(v)
	switch prim {
	case 1: // points
		for _, p := range v {
			if p != nil {
				g.point(st, p)
			}
		}
	case 2: // lines
		for i := 0; i+1 < n; i += 2 {
			g.line(st, v[i], v[i+1])
		}
	case 3, 0x12: // line strip, line loop
		for i := 0; i+1 < n; i++ {
			g.line(st, v[i], v[i+1])
		}
		if prim == 0x12 && n > 2 {
			g.line(st, v[n-1], v[0])
		}
	case 4: // triangles
		for i := 0; i+2 < n; i += 3 {
			tri(v[i], v[i+1], v[i+2])
		}
	case 5, 0x15: // fan, polygon
		for i := 1; i+1 < n; i++ {
			tri(v[0], v[i], v[i+1])
		}
	case 6: // strip (restart indices break it)
		k := 0
		for i := 0; i < n; i++ {
			if v[i] == nil {
				k = 0
				continue
			}
			if k >= 2 {
				if k%2 == 0 {
					tri(v[i-2], v[i-1], v[i])
				} else {
					tri(v[i-1], v[i-2], v[i])
				}
			}
			k++
		}
	case 0x11: // rectangles: three corners, the fourth worked out
		for i := 0; i+2 < n; i += 3 {
			a, b, c := v[i], v[i+1], v[i+2]
			if a == nil || b == nil || c == nil {
				continue
			}
			d := &vtxOut{}
			for k := 0; k < 4; k++ {
				d.pos[k] = b.pos[k] + c.pos[k] - a.pos[k]
			}
			for p := 0; p < 32; p++ {
				for k := 0; k < 4; k++ {
					d.param[p][k] = f2u(u2f(b.param[p][k]) + u2f(c.param[p][k]) - u2f(a.param[p][k]))
				}
			}
			d.sx, d.sy, d.sz, d.rw = b.sx+c.sx-a.sx, b.sy+c.sy-a.sy, b.sz+c.sz-a.sz, b.rw+c.rw-a.rw
			// rectangles aren't clipped or culled
			g.raster(st, a, b, c, c, true)
			g.raster(st, b, d, c, c, true)
		}
	case 0x13: // quads
		for i := 0; i+3 < n; i += 4 {
			tri(v[i], v[i+1], v[i+2])
			tri(v[i], v[i+2], v[i+3])
		}
	case 0x14: // quad strip
		for i := 0; i+3 < n; i += 2 {
			tri(v[i], v[i+1], v[i+3])
			tri(v[i], v[i+3], v[i+2])
		}
	default:
		g.warn("primitive type %d", prim)
	}
}

// clipTriangle clips against the near and far planes (and a guard band
// for x and y), then rasterizes. pv is the provoking vertex.
func (g *Latte) clipTriangle(st *drawState, a, b, c, pv *vtxOut) {
	g.Prims++
	if st.clip>>16&1 != 0 { // clipping off
		g.raster(st, a, b, c, pv, false)
		return
	}
	inside := func(o *vtxOut) bool {
		x, y, z, w := o.pos[0], o.pos[1], o.pos[2], o.pos[3]
		if w <= 0 {
			return false
		}
		const gb = 8
		if x < -gb*w || x > gb*w || y < -gb*w || y > gb*w {
			return false
		}
		if st.clip>>26&1 == 0 && (st.clip>>19&1 != 0 && z < 0 || st.clip>>19&1 == 0 && z < -w) {
			return false
		}
		if st.clip>>27&1 == 0 && z > w {
			return false
		}
		return true
	}
	if inside(a) && inside(b) && inside(c) {
		g.raster(st, a, b, c, pv, false)
		return
	}
	poly := []*vtxOut{a, b, c}
	planes := []func(o *vtxOut) float32{
		func(o *vtxOut) float32 { return o.pos[3] - 1e-6 },
		func(o *vtxOut) float32 { return o.pos[0] + 8*o.pos[3] },
		func(o *vtxOut) float32 { return 8*o.pos[3] - o.pos[0] },
		func(o *vtxOut) float32 { return o.pos[1] + 8*o.pos[3] },
		func(o *vtxOut) float32 { return 8*o.pos[3] - o.pos[1] },
	}
	if st.clip>>26&1 == 0 {
		if st.clip>>19&1 != 0 {
			planes = append(planes, func(o *vtxOut) float32 { return o.pos[2] })
		} else {
			planes = append(planes, func(o *vtxOut) float32 { return o.pos[2] + o.pos[3] })
		}
	}
	if st.clip>>27&1 == 0 {
		planes = append(planes, func(o *vtxOut) float32 { return o.pos[3] - o.pos[2] })
	}
	for _, d := range planes {
		var out []*vtxOut
		for i := range poly {
			p, q := poly[i], poly[(i+1)%len(poly)]
			dp, dq := d(p), d(q)
			if dp >= 0 {
				out = append(out, p)
			}
			if (dp >= 0) != (dq >= 0) {
				t := dp / (dp - dq)
				out = append(out, lerpVtx(p, q, t))
			}
		}
		poly = out
		if len(poly) < 3 {
			return
		}
	}
	for _, o := range poly {
		if o != a && o != b && o != c {
			g.viewport(st, o)
		}
	}
	for i := 1; i+1 < len(poly); i++ {
		g.raster(st, poly[0], poly[i], poly[i+1], pv, false)
	}
}

func lerpVtx(p, q *vtxOut, t float32) *vtxOut {
	o := &vtxOut{psize: p.psize}
	for k := 0; k < 4; k++ {
		o.pos[k] = p.pos[k] + (q.pos[k]-p.pos[k])*t
	}
	for i := 0; i < 32; i++ {
		for k := 0; k < 4; k++ {
			a, b := u2f(p.param[i][k]), u2f(q.param[i][k])
			o.param[i][k] = f2u(a + (b-a)*t)
		}
	}
	return o
}

// point draws a point sprite as a square.
func (g *Latte) point(st *drawState, p *vtxOut) {
	if p.pos[3] <= 0 {
		return
	}
	h := st.pointSize / 2
	if p.psize > 0 && g.reg(regPA_CL_VS_OUT_CNTL)>>16&1 != 0 {
		h = p.psize / 2
	}
	corner := func(dx, dy float32, u, v float32) *vtxOut {
		o := *p
		o.sx += dx
		o.sy += dy
		for i, in := range st.psInputs {
			if in.sprite && in.param >= 0 {
				o.param[in.param][0], o.param[in.param][1] = f2u(u), f2u(v)
			}
			_ = i
		}
		return &o
	}
	a := corner(-h, -h, 0, 0)
	b := corner(h, -h, 1, 0)
	c := corner(-h, h, 0, 1)
	d := corner(h, h, 1, 1)
	g.raster(st, a, b, c, p, true)
	g.raster(st, b, d, c, p, true)
}

// line draws a line as a thin rectangle.
func (g *Latte) line(st *drawState, a, b *vtxOut) {
	if a == nil || b == nil || a.pos[3] <= 0 || b.pos[3] <= 0 {
		return
	}
	dx, dy := b.sx-a.sx, b.sy-a.sy
	l := float32(math.Sqrt(float64(dx*dx + dy*dy)))
	if l == 0 {
		return
	}
	w := st.lineWidth / 2
	nx, ny := -dy/l*w, dx/l*w
	off := func(o *vtxOut, s float32) *vtxOut {
		c := *o
		c.sx += nx * s
		c.sy += ny * s
		return &c
	}
	a0, a1, b0, b1 := off(a, 1), off(a, -1), off(b, 1), off(b, -1)
	g.raster(st, a0, a1, b0, b, true)
	g.raster(st, a1, b1, b0, b, true)
}

// ---- rasterizing ----

const subBits = 8

func (g *Latte) raster(st *drawState, a, b, c, pv *vtxOut, noCull bool) {
	fx := func(v float32) int64 { return int64(math.Round(float64(v) * (1 << subBits))) }
	x0, y0 := fx(a.sx), fx(a.sy)
	x1, y1 := fx(b.sx), fx(b.sy)
	x2, y2 := fx(c.sx), fx(c.sy)
	area := (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0)
	if area == 0 {
		return
	}
	// facing: counter-clockwise on screen (y down) gives a negative area
	ccw := area < 0
	front := ccw
	if st.mode>>2&1 != 0 {
		front = !ccw
	}
	if !noCull {
		if front && st.mode&1 != 0 || !front && st.mode>>1&1 != 0 {
			return
		}
	}
	if area > 0 { // make the winding consistent for the edge tests
		b, c = c, b
		x1, y1, x2, y2 = x2, y2, x1, y1
		area = -area
	}
	// bounding box in pixels, inside the scissor
	minX := max(int32(min(x0, x1, x2)>>subBits), st.sc[0])
	maxX := min(int32((max(x0, x1, x2)+(1<<subBits)-1)>>subBits), st.sc[2]-1)
	minY := max(int32(min(y0, y1, y2)>>subBits), st.sc[1])
	maxY := min(int32((max(y0, y1, y2)+(1<<subBits)-1)>>subBits), st.sc[3]-1)
	if minX > maxX || minY > maxY {
		return
	}
	minX &^= 1
	minY &^= 1
	// edge functions (negative area: inside where all are <= 0)
	type edge struct{ a, b, c int64 }
	mk := func(xa, ya, xb, yb int64) edge {
		e := edge{a: ya - yb, b: xb - xa}
		e.c = -(e.a*xa + e.b*ya)
		// top-left rule: on the edge counts only for top and left edges
		if !(e.a < 0 || e.a == 0 && e.b < 0) {
			e.c++
		}
		return e
	}
	e0 := mk(x1, y1, x2, y2) // opposite a
	e1 := mk(x2, y2, x0, y0) // opposite b
	e2 := mk(x0, y0, x1, y1) // opposite c
	fa := float64(area)
	// polygon offset
	zoff := float32(0)
	if st.polyOffset {
		// a rough slope: the larger z change per pixel
		zoff = st.offUnits / (1 << 24)
		_ = st.offScale
	}
	q := &quad{st: st, a: a, b: b, c: c, pv: pv, front: front}
	half := int64(1) << (subBits - 1)
	for y := minY; y <= maxY; y += 2 {
		for x := minX; x <= maxX; x += 2 {
			var cover laneMask
			var bary [nl][3]float32
			for l := 0; l < nl; l++ {
				px := int64(x+int32(l&1))<<subBits + half
				py := int64(y+int32(l>>1))<<subBits + half
				w0 := e0.a*px + e0.b*py + e0.c
				w1 := e1.a*px + e1.b*py + e1.c
				w2 := e2.a*px + e2.b*py + e2.c
				in := w0 <= 0 && w1 <= 0 && w2 <= 0
				lx, ly := x+int32(l&1), y+int32(l>>1)
				if lx < st.sc[0] || ly < st.sc[1] || lx >= st.sc[2] || ly >= st.sc[3] {
					in = false
				}
				cover[l] = in
				bary[l] = [3]float32{float32(float64(w0) / fa), float32(float64(w1) / fa), float32(float64(w2) / fa)}
			}
			if !cover.any() {
				continue
			}
			q.x, q.y = x, y
			q.cover = cover
			q.bary = bary
			q.zoff = zoff
			g.shadeQuad(q)
		}
	}
}

type quad struct {
	st         *drawState
	a, b, c    *vtxOut
	pv         *vtxOut
	front      bool
	x, y       int32
	cover      laneMask
	bary       [nl][3]float32
	zoff       float32
	z          [nl]float32
	depthAddrs [nl]uint32
}

func (g *Latte) shadeQuad(q *quad) {
	st := q.st
	a, b, c := q.a, q.b, q.c
	for l := 0; l < nl; l++ {
		w := q.bary[l]
		z := w[0]*a.sz + w[1]*b.sz + w[2]*c.sz + q.zoff
		q.z[l] = min(max(z, st.zmin), st.zmax)
	}
	// depth and stencil before the pixel shader when the shader can't
	// change the outcome
	early := !st.killOn && !st.zExport && !st.alphaOn
	pass := q.cover
	if st.db.ok {
		for l := 0; l < nl; l++ {
			if pass[l] {
				q.depthAddrs[l] = st.db.lay.addr(uint32(q.x+int32(l&1)), uint32(q.y+int32(l>>1)), st.db.slice)
			}
		}
		if early {
			pass = g.depthTest(q, pass, q.z)
			if !pass.any() {
				return
			}
		}
	}
	// pixel shader inputs
	s := st.psRun
	s.reset()
	s.valid = laneMask{true, true, true, true}
	for l := 0; l < nl; l++ {
		s.helper[l] = !pass[l]
	}
	for _, in := range st.psInputs {
		for l := 0; l < nl; l++ {
			var v [4]uint32
			switch {
			case in.param < 0:
				v = in.def
			case in.flat:
				v = q.pv.param[in.param]
			default:
				w := q.bary[l]
				var wa, wb, wc float32
				if in.linear {
					wa, wb, wc = w[0], w[1], w[2]
				} else {
					pa, pb, pc := w[0]*a.rw, w[1]*b.rw, w[2]*c.rw
					sum := pa + pb + pc
					if sum != 0 {
						wa, wb, wc = pa/sum, pb/sum, pc/sum
					}
				}
				for k := 0; k < 4; k++ {
					v[k] = f2u(wa*u2f(a.param[in.param][k]) + wb*u2f(b.param[in.param][k]) + wc*u2f(c.param[in.param][k]))
				}
			}
			for k := 0; k < 4; k++ {
				s.gpr[in.gpr][k][l] = v[k]
			}
		}
	}
	if st.posAddr >= 0 {
		for l := 0; l < nl; l++ {
			w := q.bary[l]
			rw := w[0]*a.rw + w[1]*b.rw + w[2]*c.rw
			p := st.posAddr & 127
			s.gpr[p][0][l] = f2u(float32(q.x+int32(l&1)) + 0.5)
			s.gpr[p][1][l] = f2u(float32(q.y+int32(l>>1)) + 0.5)
			s.gpr[p][2][l] = f2u(q.z[l])
			s.gpr[p][3][l] = f2u(rw)
		}
	}
	if st.faceAddr >= 0 {
		v := f2u(-1)
		if q.front {
			v = f2u(1)
		}
		if st.faceAllBits {
			v = 0
			if q.front {
				v = 0xffffffff
			}
		}
		for l := 0; l < nl; l++ {
			s.gpr[st.faceAddr&127][st.faceChan][l] = v
		}
	}
	e := &st.psEnv
	e.wrote = [8]bool{}
	e.zOut = false
	s.run(st.ps)
	g.Pixels += uint64(countLanes(pass))
	alive := pass
	for l := 0; l < nl; l++ {
		alive[l] = alive[l] && !s.killed[l]
	}
	if st.alphaOn {
		for l := 0; l < nl; l++ {
			if alive[l] && !compareOK(st.alphaFunc, u2f(e.colors[0][3][l]), st.alphaRef) {
				alive[l] = false
			}
		}
	}
	if !alive.any() {
		return
	}
	if st.db.ok && !early {
		z := q.z
		if e.zOut && st.zExport {
			for l := 0; l < nl; l++ {
				z[l] = clamp01(u2f(e.depth[l]))
			}
		}
		alive = g.depthTest(q, alive, z)
		if !alive.any() {
			return
		}
	}
	for t := 0; t < 8; t++ {
		if !st.cbOn[t] {
			continue
		}
		src := t
		if st.multiWrite {
			src = 0
		}
		if !e.wrote[src] {
			continue
		}
		g.writeColor(st, t, q, alive, &e.colors[src], &e.colors[1])
	}
}

func countLanes(m laneMask) int {
	n := 0
	for _, b := range m {
		if b {
			n++
		}
	}
	return n
}

// depthTest runs the depth and stencil tests for the lanes in m and
// updates the buffer; it returns the lanes that pass.
func (g *Latte) depthTest(q *quad, m laneMask, z [nl]float32) laneMask {
	st := q.st
	ctl := st.dbCtl
	zOn := ctl>>1&1 != 0
	zWrite := ctl>>2&1 != 0
	zFunc := ctl >> 4 & 7
	stOn := ctl&1 != 0 && st.db.hasStencil()
	back := !q.front && ctl>>7&1 != 0
	sFunc, sFail, sPass, sZFail := ctl>>8&7, ctl>>11&7, ctl>>14&7, ctl>>17&7
	rm := st.stRef[0]
	if back {
		sFunc, sFail, sPass, sZFail = ctl>>20&7, ctl>>23&7, ctl>>26&7, ctl>>29&7
		rm = st.stRef[1]
	}
	ref, mask, wmask := uint8(rm), uint8(rm>>8), uint8(rm>>16)
	out := m
	for l := 0; l < nl; l++ {
		if !m[l] {
			continue
		}
		a := q.depthAddrs[l]
		old, sv := g.readDepth(&st.db, a)
		zPass := !zOn || depthCmp(zFunc, z[l], old)
		sOK := true
		ns := sv
		if stOn {
			sOK = compareU8(sFunc, ref&mask, sv&mask)
			op := sPass
			if !sOK {
				op = sFail
			} else if !zPass {
				op = sZFail
			}
			ns = stencilOp(op, sv, ref)
			ns = sv&^wmask | ns&wmask
		}
		ok := sOK && zPass
		writeZ := ok && zOn && zWrite
		if writeZ || ns != sv {
			g.writeDepth(&st.db, a, z[l], ns, writeZ, ns != sv)
		}
		out[l] = ok
	}
	return out
}

func depthCmp(f uint32, a, b float32) bool { return compareOK(f, a, b) }

func compareU8(f uint32, a, b uint8) bool {
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

func stencilOp(op uint32, v, ref uint8) uint8 {
	switch op {
	case 1:
		return 0
	case 2:
		return ref
	case 3:
		if v < 255 {
			return v + 1
		}
		return v
	case 4:
		if v > 0 {
			return v - 1
		}
		return v
	case 5:
		return ^v
	case 6:
		return v + 1
	case 7:
		return v - 1
	}
	return v
}

// writeColor blends and writes the quad's colours into target t.
func (g *Latte) writeColor(st *drawState, t int, q *quad, m laneMask, col *[4]lanes, src1 *[4]lanes) {
	cb := &st.cbs[t]
	wm := st.writeMask[t]
	n := fmtBytes(cb.format)
	var buf [16]byte
	blend := st.blendOn[t]
	ctl := st.blend[t]
	fast := cb.format == fmt8_8_8_8 && cb.kind == kUnorm && !blend && wm == 15
	for l := 0; l < nl; l++ {
		if !m[l] {
			continue
		}
		x, y := uint32(q.x+int32(l&1)), uint32(q.y+int32(l>>1))
		a := cb.lay.addr(x, y, cb.slice)
		v := [4]uint32{col[0][l], col[1][l], col[2][l], col[3][l]}
		if fast {
			g.mem.Write32(a, q8(v[0])<<24|q8(v[1])<<16|q8(v[2])<<8|q8(v[3]))
			continue
		}
		var dst [4]uint32
		if blend || wm != 15 {
			g.mem.ReadBytes(a, buf[:n])
			dst = decodeElem(cb.format, cb.kind, buf[:n])
		}
		if blend {
			s1 := [4]uint32{src1[0][l], src1[1][l], src1[2][l], src1[3][l]}
			v = blendPixel(ctl, v, dst, s1, st.blendConst, cb.kind == kUnorm || cb.kind == kSrgb)
		}
		for c := 0; c < 4; c++ {
			if wm>>c&1 == 0 {
				v[c] = dst[c]
			}
		}
		encodeElem(cb.format, cb.kind, v, buf[:n])
		g.mem.WriteBytes(a, buf[:n])
	}
}

func blendFactor(f uint32, s, d, s1 [4]float32, k [4]float32, c int) float32 {
	switch f {
	case 0:
		return 0
	case 1:
		return 1
	case 2:
		return s[c]
	case 3:
		return 1 - s[c]
	case 4:
		return s[3]
	case 5:
		return 1 - s[3]
	case 6:
		return d[3]
	case 7:
		return 1 - d[3]
	case 8:
		return d[c]
	case 9:
		return 1 - d[c]
	case 10:
		if c == 3 {
			return 1
		}
		return min(s[3], 1-d[3])
	case 11:
		return s[3]
	case 12:
		return 1 - s[3]
	case 13:
		return k[c]
	case 14:
		return 1 - k[c]
	case 15:
		return s1[c]
	case 16:
		return 1 - s1[c]
	case 17:
		return s1[3]
	case 18:
		return 1 - s1[3]
	case 19:
		return k[3]
	case 20:
		return 1 - k[3]
	}
	return 1
}

func blendPixel(ctl uint32, sv, dv, s1v [4]uint32, k [4]float32, clampIn bool) [4]uint32 {
	var s, d, s1 [4]float32
	for c := 0; c < 4; c++ {
		s[c], d[c], s1[c] = u2f(sv[c]), u2f(dv[c]), u2f(s1v[c])
		if clampIn {
			s[c] = clamp01(s[c])
			s1[c] = clamp01(s1[c])
		}
	}
	cs, cf, cd := ctl&0x1f, ctl>>5&7, ctl>>8&0x1f
	as, af, ad := cs, cf, cd
	if ctl>>29&1 != 0 {
		as, af, ad = ctl>>16&0x1f, ctl>>21&7, ctl>>24&0x1f
	}
	var o [4]uint32
	for c := 0; c < 4; c++ {
		fs, fn, fd := cs, cf, cd
		if c == 3 {
			fs, fn, fd = as, af, ad
		}
		a := s[c] * blendFactor(fs, s, d, s1, k, c)
		b := d[c] * blendFactor(fd, s, d, s1, k, c)
		var r float32
		switch fn {
		case 0:
			r = a + b
		case 1:
			r = a - b
		case 2:
			r = min(s[c], d[c])
		case 3:
			r = max(s[c], d[c])
		case 4:
			r = b - a
		}
		o[c] = f2u(r)
	}
	return o
}
