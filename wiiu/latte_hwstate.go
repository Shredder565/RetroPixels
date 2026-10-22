package wiiu

// GPU state for a host graphics API (Direct3D 11): the Latte registers a
// draw uses, turned into the API's state descriptions, and the host pixel
// formats render targets live in. Portable, so it is tested everywhere;
// the backend itself is in latte_d3d_windows.go.

import (
	"math"

	"retropixels/wiiu/d3d"
)

// ---- host pixel formats ----

// hostColorFormat picks the four-component host format a Latte colour
// surface (format f, number kind k) is drawn into: the same kind, with at
// least the guest's bits a component.
func hostColorFormat(f uint32, k numKind) uint32 {
	bits := uint8(0)
	d := &fmtDescs[f&0x3f]
	for c := 0; c < d.nc; c++ {
		bits = max(bits, d.comp[c].n)
	}
	if d.float || d.small || k == kFloat || k == kUscaled || k == kSscaled {
		if bits <= 16 && (d.float || d.small) {
			return d3d.FormatRGBA16Float
		}
		return d3d.FormatRGBA32Float
	}
	switch k {
	case kSrgb:
		if bits <= 8 {
			return d3d.FormatRGBA8UnormSRGB
		}
		return d3d.FormatRGBA32Float
	case kSnorm:
		switch {
		case bits <= 8:
			return d3d.FormatRGBA8Snorm
		case bits <= 16:
			return d3d.FormatRGBA16Snorm
		}
		return d3d.FormatRGBA32Float
	case kUint:
		switch {
		case bits <= 8:
			return d3d.FormatRGBA8Uint
		case bits <= 16:
			return d3d.FormatRGBA16Uint
		}
		return d3d.FormatRGBA32Uint
	case kSint:
		switch {
		case bits <= 8:
			return d3d.FormatRGBA8Sint
		case bits <= 16:
			return d3d.FormatRGBA16Sint
		}
		return d3d.FormatRGBA32Sint
	}
	switch {
	case bits <= 8:
		return d3d.FormatRGBA8Unorm
	case bits <= 16:
		return d3d.FormatRGBA16Unorm
	}
	return d3d.FormatRGBA32Float
}

// hostPixelBytes is the size of one pixel of a host colour format.
func hostPixelBytes(h uint32) int {
	switch h {
	case d3d.FormatRGBA8Unorm, d3d.FormatRGBA8UnormSRGB, d3d.FormatRGBA8Snorm, d3d.FormatRGBA8Uint, d3d.FormatRGBA8Sint:
		return 4
	case d3d.FormatRGBA16Float, d3d.FormatRGBA16Unorm, d3d.FormatRGBA16Snorm, d3d.FormatRGBA16Uint, d3d.FormatRGBA16Sint:
		return 8
	}
	return 16
}

// hostKind gives the number kind and component size of a host format.
func hostKind(h uint32) (numKind, uint8, bool) {
	switch h {
	case d3d.FormatRGBA8Unorm:
		return kUnorm, 8, false
	case d3d.FormatRGBA8UnormSRGB:
		return kSrgb, 8, false
	case d3d.FormatRGBA8Snorm:
		return kSnorm, 8, false
	case d3d.FormatRGBA8Uint:
		return kUint, 8, false
	case d3d.FormatRGBA8Sint:
		return kSint, 8, false
	case d3d.FormatRGBA16Float:
		return kFloat, 16, true
	case d3d.FormatRGBA16Unorm:
		return kUnorm, 16, false
	case d3d.FormatRGBA16Snorm:
		return kSnorm, 16, false
	case d3d.FormatRGBA16Uint:
		return kUint, 16, false
	case d3d.FormatRGBA16Sint:
		return kSint, 16, false
	case d3d.FormatRGBA32Uint:
		return kUint, 32, false
	case d3d.FormatRGBA32Sint:
		return kSint, 32, false
	}
	return kFloat, 32, true
}

// hostDecode turns a host pixel into shader values.
func hostDecode(h uint32, b []byte) [4]uint32 {
	k, n, float := hostKind(h)
	var v [4]uint32
	for c := 0; c < 4; c++ {
		x := field(b, uint8(c)*n, n)
		switch {
		case float && n == 16:
			v[c] = f2u(halfToF32(uint16(x)))
		case float || n == 32:
			v[c] = x
		case k == kSrgb:
			// the host format decodes sRGB when sampling, but its
			// bytes are the encoded values: keep them so
			v[c] = convertIn(x, n, kUnorm, c)
			if c < 3 {
				v[c] = f2u(srgbToLinear(u2f(v[c])))
			}
		default:
			v[c] = convertIn(x, n, k, c)
		}
	}
	return v
}

// hostEncode writes shader values as a host pixel.
func hostEncode(h uint32, v [4]uint32, b []byte) {
	k, n, float := hostKind(h)
	for c := 0; c < 4; c++ {
		var x uint32
		switch {
		case float && n == 16:
			x = uint32(f32ToHalf(u2f(v[c])))
		case float || n == 32:
			x = v[c]
		default:
			x = convertOut(v[c], n, k, c)
		}
		setField(b, uint8(c)*n, n, x)
	}
}

// guestChannels is the write mask of the components a guest format has
// (the host format has four; the others must keep their defaults).
func guestChannels(f uint32) uint8 {
	nc := fmtDescs[f&0x3f].nc
	if f&0x3f == fmt5_9_9_9 {
		nc = 3
	}
	return uint8(1)<<nc - 1
}

// hostDepthFormats: the texture (typeless), depth view and shader view
// formats for a DB_DEPTH_INFO format, and the bytes a texel.
func hostDepthFormats(f uint32) (tex, dsv, srv uint32, bytes int) {
	switch f {
	case 1:
		return d3d.FormatR16Typeless, d3d.FormatD16Unorm, d3d.FormatR16Unorm, 2
	case 2, 3:
		return d3d.FormatR24G8Typeless, d3d.FormatD24UnormS8Uint, d3d.FormatR24UnormX8, 4
	case 6:
		return d3d.FormatR32Typeless, d3d.FormatD32Float, d3d.FormatR32Float, 4
	}
	// 24-bit float depth and 32-bit float with stencil
	return d3d.FormatR32G8X24Typeless, d3d.FormatD32FloatS8X24, d3d.FormatR32FloatX8X24, 8
}

// hostDepthDecode reads a host depth texel as depth (0..1) and stencil.
func hostDepthDecode(f uint32, b []byte) (float32, uint8) {
	switch f {
	case 1:
		return float32(uint16(b[0])|uint16(b[1])<<8) / 65535, 0
	case 2, 3:
		v := uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
		return float32(v&0xffffff) / 0xffffff, uint8(v >> 24)
	case 6:
		return u2f(uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24), 0
	}
	return u2f(uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24), b[4]
}

// hostDepthEncode writes depth and stencil as a host depth texel.
func hostDepthEncode(f uint32, z float32, s uint8, b []byte) {
	put := func(v uint32) { b[0], b[1], b[2], b[3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24) }
	switch f {
	case 1:
		v := uint16(math.Floor(float64(clamp01(z))*65535 + 0.5))
		b[0], b[1] = byte(v), byte(v>>8)
	case 2, 3:
		put(uint32(math.Floor(float64(clamp01(z))*0xffffff+0.5)) | uint32(s)<<24)
	case 6:
		put(f2u(z))
	default:
		put(f2u(z))
		b[4], b[5], b[6], b[7] = s, 0, 0, 0
	}
}

// ---- state ----

// hostBlendFactor maps a Latte blend factor; alpha factors can't name
// colours.
func hostBlendFactor(f uint32, alpha bool) uint32 {
	m := [...]uint32{d3d.BlendZero, d3d.BlendOne, d3d.BlendSrcColor, d3d.BlendInvSrcColor, d3d.BlendSrcAlpha,
		d3d.BlendInvSrcAlpha, d3d.BlendDestAlpha, d3d.BlendInvDestAlpha, d3d.BlendDestColor, d3d.BlendInvDestColor,
		d3d.BlendSrcAlphaSat, d3d.BlendSrcAlpha, d3d.BlendInvSrcAlpha, d3d.BlendFactor, d3d.BlendInvBlendFactor,
		d3d.BlendSrc1Color, d3d.BlendInvSrc1Color, d3d.BlendSrc1Alpha, d3d.BlendInvSrc1Alpha, d3d.BlendFactor,
		d3d.BlendInvBlendFactor}
	r := uint32(d3d.BlendOne)
	if f < uint32(len(m)) {
		r = m[f]
	}
	if alpha {
		switch r {
		case d3d.BlendSrcColor:
			r = d3d.BlendSrcAlpha
		case d3d.BlendInvSrcColor:
			r = d3d.BlendInvSrcAlpha
		case d3d.BlendDestColor:
			r = d3d.BlendDestAlpha
		case d3d.BlendInvDestColor:
			r = d3d.BlendInvDestAlpha
		case d3d.BlendSrc1Color:
			r = d3d.BlendSrc1Alpha
		case d3d.BlendInvSrc1Color:
			r = d3d.BlendInvSrc1Alpha
		}
	}
	return r
}

func hostBlendOp(op uint32) uint32 {
	switch op {
	case 1:
		return d3d.BlendOpSub
	case 2:
		return d3d.BlendOpMin
	case 3:
		return d3d.BlendOpMax
	case 4:
		return d3d.BlendOpRevSub
	}
	return d3d.BlendOpAdd
}

// hostBlend gives the blend state of a draw, and the blend factor constant
// (Direct3D has one constant for colour and alpha factors: when a target
// uses the constant's alpha for colour, it gets the alpha in every
// component).
func hostBlend(st *drawState) (d3d.BlendDesc, [4]float32) {
	var bd d3d.BlendDesc
	bd.Independent = 1
	k := st.blendConst
	for i := 0; i < 8; i++ {
		t := &bd.Target[i]
		wm := uint8(st.writeMask[i])
		if st.cbOn[i] {
			wm &= guestChannels(st.cbs[i].format)
		}
		t.WriteMask = wm
		t.Src, t.Dst, t.Op = d3d.BlendOne, d3d.BlendZero, d3d.BlendOpAdd
		t.SrcAlpha, t.DstAlpha, t.OpAlpha = d3d.BlendOne, d3d.BlendZero, d3d.BlendOpAdd
		if !st.blendOn[i] || !st.cbOn[i] {
			continue
		}
		ctl := st.blend[i]
		cs, cf, cd := ctl&0x1f, ctl>>5&7, ctl>>8&0x1f
		as, af, ad := cs, cf, cd
		if ctl>>29&1 != 0 {
			as, af, ad = ctl>>16&0x1f, ctl>>21&7, ctl>>24&0x1f
		}
		if cs == 19 || cs == 20 || cd == 19 || cd == 20 {
			k = [4]float32{k[3], k[3], k[3], k[3]}
		}
		t.Enable = 1
		t.Src, t.Dst, t.Op = hostBlendFactor(cs, false), hostBlendFactor(cd, false), hostBlendOp(cf)
		t.SrcAlpha, t.DstAlpha, t.OpAlpha = hostBlendFactor(as, true), hostBlendFactor(ad, true), hostBlendOp(af)
	}
	return bd, k
}

// hostDepthStencil gives the depth and stencil state, and the stencil
// reference.
func hostDepthStencil(st *drawState) (d3d.DepthStencilDesc, uint32) {
	ctl := st.dbCtl
	var ds d3d.DepthStencilDesc
	if !st.db.ok {
		ds.DepthFunc = d3d.CmpAlways
		ds.Front = d3d.StencilOps{Fail: d3d.StencilKeep, DepthFail: d3d.StencilKeep, Pass: d3d.StencilKeep, Func: d3d.CmpAlways}
		ds.Back = ds.Front
		return ds, 0
	}
	ds.DepthEnable = b2i32(ctl>>1&1 != 0)
	ds.DepthWriteMask = ctl >> 2 & 1
	ds.DepthFunc = ctl>>4&7 + d3d.CmpNever
	ds.StencilEnable = b2i32(ctl&1 != 0 && st.db.hasStencil())
	rm := st.stRef[0]
	ds.ReadMask, ds.WriteMask = uint8(rm>>8), uint8(rm>>16)
	ops := func(sh uint32) d3d.StencilOps {
		return d3d.StencilOps{
			Func:      ctl>>sh&7 + d3d.CmpNever,
			Fail:      ctl>>(sh+3)&7 + d3d.StencilKeep,
			Pass:      ctl>>(sh+6)&7 + d3d.StencilKeep,
			DepthFail: ctl>>(sh+9)&7 + d3d.StencilKeep,
		}
	}
	ds.Front = ops(8)
	ds.Back = ds.Front
	if ctl>>7&1 != 0 {
		ds.Back = ops(20)
	}
	return ds, rm & 0xff
}

// hostRaster gives the rasterizer state; cullAll is set when both faces
// are culled (Direct3D can't say so).
func hostRaster(st *drawState, g *Latte) (rs d3d.RasterizerDesc, cullAll bool) {
	mode := st.mode
	rs.Fill = d3d.FillSolid
	rs.Cull = d3d.CullNone
	switch mode & 3 {
	case 1:
		rs.Cull = d3d.CullFront
	case 2:
		rs.Cull = d3d.CullBack
	case 3:
		cullAll = true
	}
	// counter-clockwise on the screen is the front unless FACE is set
	rs.FrontCCW = b2i32(mode>>2&1 == 0)
	rs.DepthClip = b2i32(st.clip>>26&3 != 3)
	rs.Scissor = 1
	if mode>>11&1 != 0 {
		rs.SlopeScaledDepthBias = g.regF(regPA_SU_POLY_OFFSET_FRONT_SCALE) / 16
		rs.DepthBias = int32(g.regF(regPA_SU_POLY_OFFSET_FRONT_OFFS))
		rs.DepthBiasClamp = g.regF(regPA_SU_POLY_OFFSET_CLAMP)
	}
	return rs, cullAll
}

// hostSampler gives a sampler's state; compare is for depth textures
// sampled with a comparison.
func hostSampler(s *samplerDesc, aniso uint32, compare bool) d3d.SamplerDesc {
	var sd d3d.SamplerDesc
	lin := func(f uint32) uint32 { return f & 1 }
	mip := uint32(0)
	if s.mipFilter == 2 {
		mip = 1
	}
	sd.Filter = lin(s.minFilter)<<4 | lin(s.magFilter)<<2 | mip
	if s.minFilter >= 2 || s.magFilter >= 2 {
		sd.Filter = d3d.FilterAnisotropic
		sd.MaxAnisotropy = min(uint32(1)<<aniso, 16)
	}
	addr := func(m uint32) uint32 {
		switch m {
		case 0:
			return d3d.AddressWrap
		case 1:
			return d3d.AddressMirror
		case 3, 5, 7:
			return d3d.AddressMirrorOnce
		case 6:
			return d3d.AddressBorder
		}
		return d3d.AddressClamp
	}
	sd.AddressU, sd.AddressV, sd.W = addr(s.clamp[0]), addr(s.clamp[1]), addr(s.clamp[2])
	sd.MipLODBias = s.lodBias
	sd.MinLOD, sd.MaxLOD = s.minLOD, s.maxLOD
	if s.mipFilter == 0 {
		sd.MaxLOD = sd.MinLOD
	}
	sd.Border = s.borderColor
	sd.Compare = d3d.CmpNever
	if compare {
		sd.Filter |= d3d.FilterComparison
		sd.Compare = s.compare + d3d.CmpNever
	}
	return sd
}

// hostViewport gives the system constants that map the program's position
// onto a w x h target (see dxSysVport): window coordinates become the
// target's pixels.
func hostViewport(st *drawState, w, h float32) (vp [4]float32, vpz [2]float32) {
	sx, ox, sy, oy, sz, oz := float32(1), float32(0), float32(1), float32(0), float32(1), float32(0)
	if st.vte&1 != 0 {
		sx = st.scale[0]
	}
	if st.vte>>1&1 != 0 {
		ox = st.offs[0]
	}
	if st.vte>>2&1 != 0 {
		sy = st.scale[1]
	}
	if st.vte>>3&1 != 0 {
		oy = st.offs[1]
	}
	if st.vte>>4&1 != 0 {
		sz = st.scale[2]
	}
	if st.vte>>5&1 != 0 {
		oz = st.offs[2]
	}
	// window x = sx*x/w + ox; host x/w = 2*window/w - 1, and y is flipped
	vp = [4]float32{2 * sx / w, 2*ox/w - 1, -2 * sy / h, 1 - 2*oy/h}
	vpz = [2]float32{sz, oz}
	return
}

func b2i32(b bool) int32 {
	if b {
		return 1
	}
	return 0
}

// ---- primitives ----

// hostIndices turns a draw's vertex list (ids, after the index offset;
// restart entries are ^0) into a list of the primitives Direct3D draws:
// point, line or triangle lists. Rectangles come out as their three
// corners, for dxRectGS. ok is false for primitives the host can't draw.
func hostIndices(prim uint32, ids []uint32, provokingLast bool) (topo uint32, out []uint32, ok bool) {
	// Direct3D's provoking vertex is the first: rotate the last one there
	// (keeping the winding)
	tri := func(a, b, c uint32) {
		if a != ^uint32(0) && b != ^uint32(0) && c != ^uint32(0) {
			if provokingLast {
				a, b, c = c, a, b
			}
			out = append(out, a, b, c)
		}
	}
	line := func(a, b uint32) {
		if a != ^uint32(0) && b != ^uint32(0) {
			if provokingLast {
				a, b = b, a
			}
			out = append(out, a, b)
		}
	}
	n := len(ids)
	switch prim {
	case 1:
		for _, v := range ids {
			if v != ^uint32(0) {
				out = append(out, v)
			}
		}
		return d3d.TopologyPointList, out, true
	case 2:
		for i := 0; i+1 < n; i += 2 {
			line(ids[i], ids[i+1])
		}
		return d3d.TopologyLineList, out, true
	case 3, 0x12:
		for i := 0; i+1 < n; i++ {
			line(ids[i], ids[i+1])
		}
		if prim == 0x12 && n > 2 {
			line(ids[n-1], ids[0])
		}
		return d3d.TopologyLineList, out, true
	case 4:
		for i := 0; i+2 < n; i += 3 {
			tri(ids[i], ids[i+1], ids[i+2])
		}
	case 5, 0x15:
		for i := 1; i+1 < n; i++ {
			tri(ids[0], ids[i], ids[i+1])
		}
	case 6:
		k := 0
		for i := 0; i < n; i++ {
			if ids[i] == ^uint32(0) {
				k = 0
				continue
			}
			if k >= 2 {
				if k%2 == 0 {
					tri(ids[i-2], ids[i-1], ids[i])
				} else {
					tri(ids[i-1], ids[i-2], ids[i])
				}
			}
			k++
		}
	case 0x11:
		// rectangles: three corners each, as they are (a geometry shader
		// adds the fourth)
		for i := 0; i+2 < n; i += 3 {
			a, b, c := ids[i], ids[i+1], ids[i+2]
			if a != ^uint32(0) && b != ^uint32(0) && c != ^uint32(0) {
				out = append(out, a, b, c)
			}
		}
	case 0x13:
		for i := 0; i+3 < n; i += 4 {
			tri(ids[i], ids[i+1], ids[i+2])
			tri(ids[i], ids[i+2], ids[i+3])
		}
	case 0x14:
		for i := 0; i+3 < n; i += 2 {
			tri(ids[i], ids[i+1], ids[i+3])
			tri(ids[i], ids[i+3], ids[i+2])
		}
	default:
		return 0, nil, false
	}
	return d3d.TopologyTriangleList, out, true
}
