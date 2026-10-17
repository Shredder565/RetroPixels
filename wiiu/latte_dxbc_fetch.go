package wiiu

// The texture and vertex fetch part of the Direct3D shader translation.
//
// Vertex buffers are bound as buffers of 32-bit words; the shader reads the
// words an attribute covers, swaps bytes as the fetch asks and decodes the
// format itself (the same table the software renderer uses). Textures are
// decoded by the renderer into Direct3D textures, so their fetches become
// ordinary samples.

import (
	"math"

	"retropixels/wiiu/d3d"
)

// registers the fetch code uses (inside the scratch area)
const (
	dxFW   = dxS + 8  // fetched words, x of 5 registers
	dxFOUT = dxS + 13 // fetch result (xyzw)
	dxFC   = dxS + 14 // fetch coordinates (xyzw)
	dxFT   = dxS + 15 // more scratch (xyzw)
)

// srv gives the resource slot for a texture or vertex buffer.
func (t *dxTr) srv(r dxSRV) int {
	if s, ok := t.srvSlot[r]; ok {
		return s
	}
	s := len(t.prog.srv)
	if s >= 128 {
		t.fail("too many resources")
	}
	t.srvSlot[r] = s
	t.prog.srv = append(t.prog.srv, r)
	return s
}

// dxSampCompare marks a sampler slot used for depth compares.
const dxSampCompare = 1 << 31

func (t *dxTr) sampler(id uint32, compare bool) int {
	k := id
	if compare {
		k |= dxSampCompare
	}
	if s, ok := t.sampSlot[k]; ok {
		return s
	}
	s := len(t.prog.samplers)
	if s >= 16 {
		t.fail("too many samplers")
	}
	t.sampSlot[k] = s
	t.prog.samplers = append(t.prog.samplers, k)
	return s
}

func (t *dxTr) vtxClause(addr, count uint32) {
	t.ifNZ(actv)
	for i := uint32(0); i < count; i++ {
		a := t.base + (addr+i*2)*8
		t.vfetch(t.cfg.read32(a), t.cfg.read32(a+4), t.cfg.read32(a+8))
	}
	t.endif()
}

func (t *dxTr) texClause(addr, count uint32) {
	for i := uint32(0); i < count; i++ {
		a := t.base + (addr+i*2)*8
		w0, w1, w2 := t.cfg.read32(a), t.cfg.read32(a+4), t.cfg.read32(a+8)
		switch op := w0 & 0x1f; op {
		case 0, 1:
			t.ifNZ(actv)
			t.vfetch(w0, w1, w2)
			t.endif()
		case 0x0b, 0x0c:
			t.fetchCoords(w0, w2)
			g := dxGH
			if op == 0x0c {
				g = dxGV
			}
			t.ins(d3d.OpMov, dxR(g).Mask(15), dxR(dxFC))
		default:
			t.tex(w0, w1, w2)
		}
	}
}

// writeFetch writes the fetch result with destination selects.
func (t *dxTr) writeFetch(dst uint32, rel bool, dsel [4]uint32) {
	for c := uint32(0); c < 4; c++ {
		var v d3d.Opnd
		switch dsel[c] {
		case 0, 1, 2, 3:
			v = dxRC(dxFOUT, dsel[c])
		case 4:
			v = dxU(0)
		case 5:
			v = dxF(1)
		default:
			continue
		}
		t.ins(d3d.OpMov, t.gprDst(dst, 1<<c, dxRC(dxAR, 0), rel), v)
	}
}

// ---- vertex fetch ----

func (t *dxTr) vfetch(w0, w1, w2 uint32) {
	inst := w0 & 0x1f
	fetchType := w0 >> 5 & 3
	buf := w0 >> 8 & 0xff
	srcGPR := w0 >> 16 & 0x7f
	srcRel := w0>>23&1 != 0
	srcSel := w0 >> 24 & 3
	dstGPR := w1 & 0x7f
	dstRel := w1>>7&1 != 0
	if inst == 1 { // VTX_SEMANTIC
		sem := w1 & 0xff
		found := false
		for i := uint32(0); i < 32; i++ {
			if t.cfg.vtxSem[i]&0xff == sem {
				dstGPR, dstRel, found = i+1, false, true
				break
			}
		}
		if !found {
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
	key := t.cfg.vbufInfo(buf)
	t.prog.vbufDeps[buf] = key
	if useConst {
		r := key.word2
		format = r >> 20 & 0x3f
		numFormat = r >> 26 & 3
		signed = r>>28&1 != 0
		endian = r >> 30
	}
	kind := numKindFromFetch(numFormat, signed, false)
	n := fmtBytes(format)
	stride := key.stride
	slot := t.srv(dxSRV{vbuf: true, id: buf})
	vb := 0
	for _, r := range t.prog.srv[:slot] {
		if r.vbuf {
			vb++
		}
	}
	// the element's byte offset
	t.ins(d3d.OpMov, sxw(0), t.gprSrc(srcGPR, srcSel, dxRC(dxAR, 0), srcRel))
	switch fetchType {
	case 0:
		t.ins(d3d.OpIadd, sxw(0), sx(0), sysCB(dxSysDraw, 1))
	case 1:
		t.ins(d3d.OpIadd, sxw(0), sx(0), sysCB(dxSysDraw, 2))
	}
	t.ins(d3d.OpImad, sxw(1), sx(0), dxU(stride), dxU(offset))
	aligned := stride%4 == 0 && offset%4 == 0
	nw := int((n + 3) / 4)
	if !aligned {
		nw++
	}
	t.ins(d3d.OpUshr, sxw(2), sx(1), dxU(2))
	for j := 0; j < nw; j++ {
		t.ins(d3d.OpIadd, sxw(3), sx(2), dxU(uint32(j)))
		t.ins(d3d.OpLd, dxW(dxFW+j, 0), sx(3), d3d.Tex(slot).C(0))
	}
	if !aligned {
		// shift the words down by the byte offset within the first word
		t.ins(d3d.OpAnd, sxw(4), sx(1), dxU(3))
		t.ins(d3d.OpIshl, sxw(4), sx(4), dxU(3)) // sh
		t.ins(d3d.OpIneg, sxw(5), sx(4))
		t.ins(d3d.OpIadd, sxw(5), sx(5), dxU(32)) // 32 - sh
		for j := 0; j < nw-1; j++ {
			t.ins(d3d.OpUshr, sxw(6), dxRC(dxFW+j, 0), sx(4))
			t.ins(d3d.OpIshl, sxw(7), dxRC(dxFW+j+1, 0), sx(5))
			t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
			t.ins(d3d.OpMovc, dxW(dxFW+j, 0), sx(4), sx(6), dxRC(dxFW+j, 0))
		}
	}
	t.swapWords(endian, n)
	t.decodeElem(format, kind)
	// out of range reads give (0, 0, 0, 1)
	size := sysCB(dxSysBufSize+vb/4, uint32(vb%4))
	t.ins(d3d.OpIadd, sxw(6), sx(1), dxU(n))
	t.ins(d3d.OpUge, sxw(6), size, sx(6))
	t.ins(d3d.OpUge, sxw(7), dxU(1), size)
	t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
	def := d3d.ImmU(0, 0, 0, f2u(1))
	if kind == kUint || kind == kSint {
		def = d3d.ImmU(0, 0, 0, 1)
	}
	t.ins(d3d.OpMovc, dxR(dxFOUT).Mask(15), sx(6), dxR(dxFOUT), def)
	t.writeFetch(dstGPR, dstRel, dsel)
}

// swapWords swaps the bytes of the fetched words like swapBytes does for
// an n-byte element.
func (t *dxTr) swapWords(endian, n uint32) {
	w := func(j int) d3d.Opnd { return dxRC(dxFW+j, 0) }
	ww := func(j int) d3d.Opnd { return dxW(dxFW+j, 0) }
	bswap := func(j int) {
		t.ins(d3d.OpIshl, sxw(6), w(j), dxU(24))
		t.ins(d3d.OpUshr, sxw(7), w(j), dxU(24))
		t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
		t.ins(d3d.OpAnd, sxw(7), w(j), dxU(0xff00))
		t.ins(d3d.OpIshl, sxw(7), sx(7), dxU(8))
		t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
		t.ins(d3d.OpUshr, sxw(7), w(j), dxU(8))
		t.ins(d3d.OpAnd, sxw(7), sx(7), dxU(0xff00))
		t.ins(d3d.OpOr, ww(j), sx(6), sx(7))
	}
	switch endian {
	case 1: // 8 in 16
		for i := uint32(0); i+1 < n; i += 2 {
			j := int(i / 4)
			if i%4 == 0 && i+3 < n {
				t.ins(d3d.OpAnd, sxw(6), w(j), dxU(0x00ff00ff))
				t.ins(d3d.OpIshl, sxw(6), sx(6), dxU(8))
				t.ins(d3d.OpUshr, sxw(7), w(j), dxU(8))
				t.ins(d3d.OpAnd, sxw(7), sx(7), dxU(0x00ff00ff))
				t.ins(d3d.OpOr, ww(j), sx(6), sx(7))
				i += 2
				continue
			}
			// one pair at byte i%4 of word j
			sh := (i % 4) * 8
			mask := uint32(0xffff) << sh
			t.ins(d3d.OpUshr, sxw(6), w(j), dxU(sh))
			t.ins(d3d.OpAnd, sxw(7), sx(6), dxU(0xff))
			t.ins(d3d.OpIshl, sxw(7), sx(7), dxU(8))
			t.ins(d3d.OpUshr, sxw(6), sx(6), dxU(8))
			t.ins(d3d.OpAnd, sxw(6), sx(6), dxU(0xff))
			t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
			t.ins(d3d.OpIshl, sxw(6), sx(6), dxU(sh))
			t.ins(d3d.OpAnd, sxw(7), w(j), dxU(^mask))
			t.ins(d3d.OpOr, ww(j), sx(6), sx(7))
		}
	case 2: // 8 in 32
		for i := uint32(0); i+3 < n; i += 4 {
			bswap(int(i / 4))
		}
	case 3: // 8 in 64
		for i := uint32(0); i+7 < n; i += 8 {
			j := int(i / 4)
			bswap(j)
			bswap(j + 1)
			t.ins(d3d.OpMov, sxw(6), w(j))
			t.ins(d3d.OpMov, ww(j), w(j+1))
			t.ins(d3d.OpMov, ww(j+1), sx(6))
		}
	}
}

// decodeElem decodes the fetched words into dxFOUT.
func (t *dxTr) decodeElem(f uint32, k numKind) {
	d := &fmtDescs[f&0x3f]
	out := func(c int) d3d.Opnd { return dxW(dxFOUT, uint32(c)) }
	oc := func(c int) d3d.Opnd { return dxRC(dxFOUT, uint32(c)) }
	def := d3d.ImmU(0, 0, 0, f2u(1))
	if k == kUint || k == kSint {
		def = d3d.ImmU(0, 0, 0, 1)
	}
	t.ins(d3d.OpMov, dxR(dxFOUT).Mask(15), def)
	if f&0x3f == fmt5_9_9_9 {
		w := dxRC(dxFW, 0)
		// scale = 2^(e - 24): float bits (e - 24 + 127) << 23
		t.ins(d3d.OpUshr, sxw(6), w, dxU(27))
		t.ins(d3d.OpIadd, sxw(6), sx(6), dxU(103))
		t.ins(d3d.OpIshl, sxw(6), sx(6), dxU(23))
		for c := 0; c < 3; c++ {
			t.ins(d3d.OpUshr, sxw(7), w, dxU(uint32(9*c)))
			t.ins(d3d.OpAnd, sxw(7), sx(7), dxU(0x1ff))
			t.ins(d3d.OpUtof, sxw(7), sx(7))
			t.ins(d3d.OpMul, out(c), sx(7), sx(6))
		}
		return
	}
	for c := 0; c < d.nc; c++ {
		l := d.comp[c]
		wi, bit := int(l.shift/32), uint32(l.shift%32)
		n := uint32(l.n)
		w := dxRC(dxFW+wi, 0)
		switch {
		case n == 32:
			t.ins(d3d.OpMov, out(c), w)
		case bit+n <= 32:
			t.ins(d3d.OpUshr, out(c), w, dxU(bit))
			t.ins(d3d.OpAnd, out(c), oc(c), dxU(1<<n-1))
		default:
			t.ins(d3d.OpUshr, sxw(6), w, dxU(bit))
			t.ins(d3d.OpIshl, sxw(7), dxRC(dxFW+wi+1, 0), dxU(32-bit))
			t.ins(d3d.OpOr, out(c), sx(6), sx(7))
			t.ins(d3d.OpAnd, out(c), oc(c), dxU(1<<n-1))
		}
		switch {
		case d.small:
			t.smallFloat(out(c), oc(c), n-5)
		case d.float && n == 16:
			t.ins(d3d.OpF16tof32, out(c), oc(c))
		case d.float && n == 32:
		default:
			t.convertIn(out(c), oc(c), n, k, c)
		}
	}
}

// smallFloat turns an unsigned 5-bit-exponent float into a float.
func (t *dxTr) smallFloat(dst, v d3d.Opnd, mbits uint32) {
	t.ins(d3d.OpUshr, sxw(4), v, dxU(mbits))
	t.ins(d3d.OpAnd, sxw(4), sx(4), dxU(31)) // e
	t.ins(d3d.OpAnd, sxw(5), v, dxU(1<<mbits-1))
	// normal: (e+112)<<23 | m<<(23-mbits)
	t.ins(d3d.OpIadd, sxw(6), sx(4), dxU(112))
	t.ins(d3d.OpIshl, sxw(6), sx(6), dxU(23))
	t.ins(d3d.OpIshl, sxw(7), sx(5), dxU(23-mbits))
	t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
	// denormal: m * 2^-(mbits+14)
	t.ins(d3d.OpUtof, sxw(7), sx(5))
	t.ins(d3d.OpMul, sxw(7), sx(7), dxF(float32(math.Ldexp(1, -int(mbits+14)))))
	t.ins(d3d.OpMovc, sxw(6), sx(4), sx(6), sx(7))
	// e == 31: infinity or NaN
	t.ins(d3d.OpMovc, sxw(7), sx(5), dxU(0x7fc00000), dxU(0x7f800000))
	t.ins(d3d.OpIeq, sxw(4), sx(4), dxU(31))
	t.ins(d3d.OpMovc, dst, sx(4), sx(7), sx(6))
}

// convertIn converts an n-bit field (see convertIn in latte_fmt.go).
func (t *dxTr) convertIn(dst, v d3d.Opnd, n uint32, k numKind, c int) {
	maxU := float32(uint64(1)<<n - 1)
	sext := func() {
		if n < 32 {
			t.ins(d3d.OpIshl, dst, v, dxU(32-n))
			t.ins(d3d.OpIshr, dst, v, dxU(32-n))
		}
	}
	switch k {
	case kUnorm, kSrgb, kFloat:
		if k == kFloat && n == 16 {
			t.ins(d3d.OpF16tof32, dst, v)
			return
		}
		t.ins(d3d.OpUtof, dst, v)
		t.ins(d3d.OpDiv, dst, v, dxF(maxU))
		if k == kSrgb && c < 3 {
			t.srgbToLinear(dst, v)
		}
	case kSnorm:
		sext()
		t.ins(d3d.OpItof, dst, v)
		t.ins(d3d.OpDiv, dst, v, dxF(float32(max(uint64(1)<<(n-1)-1, 1)))) // 1-bit: 0 or -1
		t.ins(d3d.OpMax, dst, v, dxF(-1))
	case kUscaled:
		t.ins(d3d.OpUtof, dst, v)
	case kSscaled:
		sext()
		t.ins(d3d.OpItof, dst, v)
	case kSint:
		sext()
	}
}

func (t *dxTr) srgbToLinear(dst, v d3d.Opnd) {
	t.ins(d3d.OpDiv, sxw(6), v, dxF(12.92))
	t.ins(d3d.OpAdd, sxw(7), v, dxF(0.055))
	t.ins(d3d.OpDiv, sxw(7), sx(7), dxF(1.055))
	t.ins(d3d.OpLog, sxw(7), sx(7))
	t.ins(d3d.OpMul, sxw(7), sx(7), dxF(2.4))
	t.ins(d3d.OpExp, sxw(7), sx(7))
	t.ins(d3d.OpGe, sxw(4), dxF(0.04045), v)
	t.ins(d3d.OpMovc, dst, sx(4), sx(6), sx(7))
}

// ---- texture fetch ----

// fetchCoords builds a fetch's source vector (with its selects) in dxFC.
func (t *dxTr) fetchCoords(w0, w2 uint32) {
	srcGPR := w0 >> 16 & 0x7f
	srcRel := w0>>23&1 != 0
	ssel := [4]uint32{w2 >> 20 & 7, w2 >> 23 & 7, w2 >> 26 & 7, w2 >> 29 & 7}
	for c := uint32(0); c < 4; c++ {
		var v d3d.Opnd
		switch ssel[c] {
		case 0, 1, 2, 3:
			v = t.gprSrc(srcGPR, ssel[c], dxRC(dxAR, 0), srcRel)
		case 5:
			v = dxF(1)
		default:
			v = dxU(0)
		}
		t.ins(d3d.OpMov, dxW(dxFC, c), v)
	}
}

func (t *dxTr) tex(w0, w1, w2 uint32) {
	inst := w0 & 0x1f
	resID := w0 >> 8 & 0xff
	dstGPR := w1 & 0x7f
	dstRel := w1>>7&1 != 0
	dsel := [4]uint32{w1 >> 9 & 7, w1 >> 12 & 7, w1 >> 15 & 7, w1 >> 18 & 7}
	lodBias := float32(int32(w1<<4)>>25) / 16
	norm := [4]bool{w1>>28&1 != 0, w1>>29&1 != 0, w1>>30&1 != 0, w1>>31 != 0}
	offX := float32(int32(w2<<27)>>27) / 2
	offY := float32(int32(w2<<22)>>27) / 2
	samplerID := w2 >> 15 & 0x1f

	key := t.cfg.texInfo(resID)
	t.prog.texDeps[resID] = key
	t.fetchCoords(w0, w2)
	out := dxR(dxFOUT).Mask(15)
	t.ins(d3d.OpMov, out, dxU(0))
	if !key.valid {
		t.writeTex(dstGPR, dstRel, dsel)
		return
	}
	slot := t.srv(dxSRV{id: resID})
	res := d3d.Tex(slot)
	fc := dxR(dxFC)
	compare := inst >= 0x18 && inst <= 0x1f
	switch inst {
	case 4: // GET_TEXTURE_RESINFO
		t.ins(d3d.OpResinfo|2<<11, out, dxRC(dxFC, 0), res)
		t.writeTex(dstGPR, dstRel, dsel)
		return
	case 7, 8: // GET_GRADIENTS_H / V
		if t.cfg.ps {
			op := uint32(d3d.OpRtxFine)
			if inst == 8 {
				op = d3d.OpRtyFine
			}
			t.ins(op, out, fc)
		}
		t.writeTex(dstGPR, dstRel, dsel)
		return
	case 3: // LD
		if key.dim == 3 || key.dim == 6 {
			t.writeTex(dstGPR, dstRel, dsel)
			return
		}
		t.texelAddress(key.dim)
		t.ins(d3d.OpLd, out, dxR(dxFT), res)
		t.applySel(key)
		t.writeTex(dstGPR, dstRel, dsel)
		return
	}
	if inst < 0x10 && inst != 6 || key.dim == 6 {
		t.writeTex(dstGPR, dstRel, dsel)
		return
	}
	if key.intFmt {
		// integer textures can't be sampled: read the texel under the
		// coordinate from level 0
		t.normCoords(key.dim, [4]bool{true, true, true, true}, res, true)
		t.ins(d3d.OpMov, dxW(dxFC, 3), dxF(0))
		t.ins(d3d.OpFtoi, dxR(dxFC).Mask(15), fc)
		t.texelAddress(key.dim)
		t.ins(d3d.OpLd, out, dxR(dxFT), res)
		t.applySel(key)
		t.writeTex(dstGPR, dstRel, dsel)
		return
	}
	// the coordinate and its extra values (ref / lod / bias in w)
	t.ins(d3d.OpMov, dxW(dxFT, 3), dxRC(dxFC, 3))
	t.normCoords(key.dim, norm, res, false)
	if offX != 0 || offY != 0 {
		// offsets are in half texels; add them as a fraction of the size
		t.ins(d3d.OpResinfo, dxR(dxS+12).Mask(3), dxU(0), res)
		t.ins(d3d.OpDiv, dxR(dxS+12).Mask(3), d3d.ImmF(offX, offY, 0, 0), dxR(dxS+12))
		t.ins(d3d.OpAdd, dxR(dxFC).Mask(3), fc, dxR(dxS+12))
	}
	t.texCoordsForDim(key.dim)
	smp := d3d.Samp(t.sampler(samplerID, compare))
	extra := dxR(dxFT).Sel1(3)
	switch {
	case compare:
		if inst == 0x18 && t.cfg.ps || inst == 0x1c || inst == 0x1e {
			t.ins(d3d.OpSampleC, dxW(dxFOUT, 0), fc, res.C(0), smp, extra)
		} else {
			t.ins(d3d.OpSampleCLz, dxW(dxFOUT, 0), fc, res.C(0), smp, extra)
		}
		t.ins(d3d.OpMov, out, dxRC(dxFOUT, 0))
		t.writeTex(dstGPR, dstRel, dsel)
		return
	case inst == 6: // GET_COMP_TEX_LOD
		if t.cfg.ps {
			t.ins(d3d.OpLod, out, fc, res, smp)
			t.ins(d3d.OpMov, dxW(dxFOUT, 1), dxRC(dxFOUT, 0))
		}
		t.writeTex(dstGPR, dstRel, dsel)
		return
	case inst == 0x11 || inst == 0x15: // _L
		t.ins(d3d.OpAdd, sxw(0), extra, dxF(lodBias))
		t.ins(d3d.OpSampleL, out, fc, res, smp, sx(0))
	case inst == 0x13 || inst == 0x17: // _LZ
		t.ins(d3d.OpSampleL, out, fc, res, smp, dxF(lodBias))
	case inst == 0x14 || inst == 0x16: // _G
		t.ins(d3d.OpSampleD, out, fc, res, smp, dxR(dxGH), dxR(dxGV))
	case inst == 0x12: // _LB
		t.ins(d3d.OpAdd, sxw(0), extra, dxF(lodBias))
		if t.cfg.ps {
			t.ins(d3d.OpSampleB, out, fc, res, smp, sx(0))
		} else {
			t.ins(d3d.OpSampleL, out, fc, res, smp, sx(0))
		}
	default:
		switch {
		case !t.cfg.ps:
			t.ins(d3d.OpSampleL, out, fc, res, smp, dxF(0))
		case lodBias != 0:
			t.ins(d3d.OpSampleB, out, fc, res, smp, dxF(lodBias))
		default:
			t.ins(d3d.OpSample, out, fc, res, smp)
		}
	}
	t.applySel(key)
	t.writeTex(dstGPR, dstRel, dsel)
}

// normCoords divides the unnormalized coordinate components by the
// texture's size (toTexels: multiply normalized ones instead).
func (t *dxTr) normCoords(dim uint8, norm [4]bool, res d3d.Opnd, toTexels bool) {
	n := 2
	switch dim {
	case 0, 4:
		n = 1
	case 2:
		n = 3
	case 3:
		n = 0
	}
	var mask uint32
	for c := 0; c < n; c++ {
		if norm[c] == toTexels {
			mask |= 1 << c
		}
	}
	if mask == 0 {
		return
	}
	t.ins(d3d.OpResinfo, dxR(dxS+12).Mask(7), dxU(0), res)
	if toTexels {
		t.ins(d3d.OpMul, dxR(dxFC).Mask(mask), dxR(dxFC), dxR(dxS+12))
		t.ins(d3d.OpRoundNi, dxR(dxFC).Mask(mask), dxR(dxFC))
	} else {
		t.ins(d3d.OpDiv, dxR(dxFC).Mask(mask), dxR(dxFC), dxR(dxS+12))
	}
}

// texCoordsForDim rearranges dxFC for Direct3D: array slices rounded,
// cube coordinates turned into a direction.
func (t *dxTr) texCoordsForDim(dim uint8) {
	fc := func(c uint32) d3d.Opnd { return dxRC(dxFC, c) }
	fw := func(c uint32) d3d.Opnd { return dxW(dxFC, c) }
	switch dim {
	case 4: // 1D array: (u, slice)
		t.ins(d3d.OpAdd, fw(1), fc(1), dxF(0.5))
		t.ins(d3d.OpRoundNi, fw(1), fc(1))
	case 5, 7: // 2D array: (u, v, slice)
		t.ins(d3d.OpAdd, fw(2), fc(2), dxF(0.5))
		t.ins(d3d.OpRoundNi, fw(2), fc(2))
	case 3: // cube: (s, t) in 1..2 on face w
		// sc = 2(u-1)-1, tc = 2(v-1)-1
		t.ins(d3d.OpMad, sxw(0), fc(0), dxF(2), dxF(-3))
		t.ins(d3d.OpMad, sxw(1), fc(1), dxF(2), dxF(-3))
		t.ins(d3d.OpAdd, sxw(2), fc(2), dxF(0.5))
		t.ins(d3d.OpRoundNi, sxw(2), sx(2))
		t.ins(d3d.OpFtoi, sxw(2), sx(2)) // face
		t.ins(d3d.OpAnd, sxw(3), sx(2), dxU(1))
		t.ins(d3d.OpMovc, sxw(3), sx(3), dxF(-1), dxF(1)) // sign
		t.ins(d3d.OpMul, sxw(4), sx(3), sx(0))            // sign*sc
		t.ins(d3d.OpMul, sxw(5), sx(3), sx(1))            // sign*tc
		// face < 2: (sign, -tc, -sign*sc)
		t.ins(d3d.OpMov, fw(0), sx(3))
		t.ins(d3d.OpMov, fw(1), sx(1).Neg())
		t.ins(d3d.OpMov, fw(2), sx(4).Neg())
		t.ins(d3d.OpUlt, sxw(6), sx(2), dxU(2))
		t.ifZ(sx(6))
		t.ins(d3d.OpUlt, sxw(6), sx(2), dxU(4))
		t.ifNZ(sx(6))
		// face 2, 3: (sc, sign, sign*tc)
		t.ins(d3d.OpMov, fw(0), sx(0))
		t.ins(d3d.OpMov, fw(1), sx(3))
		t.ins(d3d.OpMov, fw(2), sx(5))
		t.els()
		// face 4, 5: (sign*sc, -tc, sign)
		t.ins(d3d.OpMov, fw(0), sx(4))
		t.ins(d3d.OpMov, fw(1), sx(1).Neg())
		t.ins(d3d.OpMov, fw(2), sx(3))
		t.endif()
		t.endif()
	}
}

// texelAddress builds an ld address (x, y, z, level) in dxFT from the
// integer coordinates in dxFC.
func (t *dxTr) texelAddress(dim uint8) {
	ft := dxR(dxFT)
	switch dim {
	case 0: // 1D: (x, level)
		t.ins(d3d.OpMov, ft.Mask(15), dxR(dxFC).Swz(0, 3, 3, 3))
		t.ins(d3d.OpMov, ft.Mask(6), dxU(0))
	case 4: // 1D array: (x, slice, level)
		t.ins(d3d.OpMov, ft.Mask(15), dxR(dxFC).Swz(0, 1, 3, 3))
		t.ins(d3d.OpMov, ft.Mask(4), dxU(0))
	case 1: // 2D
		t.ins(d3d.OpMov, ft.Mask(15), dxR(dxFC))
		t.ins(d3d.OpMov, ft.Mask(4), dxU(0))
	default:
		t.ins(d3d.OpMov, ft.Mask(15), dxR(dxFC))
	}
}

// applySel applies the texture's own component selects to dxFOUT.
func (t *dxTr) applySel(key dxTexKey) {
	s := key.sel
	if s == [4]uint8{0, 1, 2, 3} {
		return
	}
	one := dxF(1)
	if key.intFmt {
		one = dxU(1)
	}
	t.ins(d3d.OpMov, dxR(dxFT).Mask(15), dxR(dxFOUT))
	for c := uint32(0); c < 4; c++ {
		var v d3d.Opnd
		switch s[c] {
		case 0, 1, 2, 3:
			v = dxRC(dxFT, uint32(s[c]))
		case 4:
			v = dxU(0)
		default:
			v = one
		}
		t.ins(d3d.OpMov, dxW(dxFOUT, c), v)
	}
}

// writeTex writes a texture fetch's result for active lanes.
func (t *dxTr) writeTex(dst uint32, rel bool, dsel [4]uint32) {
	t.ifNZ(actv)
	t.writeFetch(dst, rel, dsel)
	t.endif()
}
