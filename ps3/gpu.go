package ps3

// Drawing on the host GPU. The RSX's draws, clears and render targets are
// handed to a gpuDevice (Direct3D 11 on Windows; tests use a stand-in that
// runs the same shaders in Go). Render targets live in host textures while
// the GPU uses them and are copied back to video memory (in the RSX's own
// formats) only when something else reads that memory: a texture in a
// format the host copy can't serve, a 2D blit, or a flip. Textures are
// copied from memory when a draw uses them and kept until their bytes
// change.
//
// Vertex programs still run on the CPU (rsxVertices); the host vertex
// shader just passes their outputs on, with the position already in
// Direct3D's clip space.

import (
	"fmt"
	"hash/maphash"
	"math"
	"os"

	"retropixels/wiiu/d3d"
)

type gpuHandle uint32

// texture kinds for CreateTexture
const (
	gpuTexPlain  = 0 // a texture the shaders read
	gpuTexTarget = 1 // also a render target
	gpuTexDepth  = 2 // a depth buffer (the shaders read its depth)
)

// formats beyond the d3d package's list
const (
	dxgiBC1 = 71
	dxgiBC2 = 74
	dxgiBC3 = 77
)

// gpuDevice is what the renderer needs from a host GPU.
type gpuDevice interface {
	// CreateTexture makes a w x h texture; data (pitch bytes a row, or nil)
	// is its first contents. Depth textures take FormatR24G8Typeless (24-bit
	// depth in the low bits, stencil above) or FormatR16Typeless.
	CreateTexture(w, h int, format uint32, kind int, data []byte, pitch int) (gpuHandle, error)
	UpdateTexture(t gpuHandle, data []byte, pitch int)
	// ReadTexture copies a texture back (for depth: the same layout as
	// CreateTexture takes).
	ReadTexture(t gpuHandle) (data []byte, pitch int, err error)
	// ReadTextureLate is ReadTexture a frame late: it starts reading t and
	// returns what the call before read (the first time, t), so showing
	// the picture doesn't wait for the GPU to finish drawing it.
	ReadTextureLate(t gpuHandle) (data []byte, pitch int, err error)
	CopyTexture(dst, src gpuHandle)
	// DepthToRGBA8 fills an RGBA8 target from a 24-bit depth texture: the
	// texel bytes (r, g, b, a) = depth bits 15-8, 7-0, 0, 23-16 (the
	// RSX's A8R8G8B8 view of a D24S8 buffer, stencil read as 0).
	DepthToRGBA8(dst, src gpuHandle) error
	Release(t gpuHandle)
	CreatePS(code []byte) (gpuHandle, error)
	CreateVS(code []byte) (gpuHandle, error)
	ClearTarget(t gpuHandle, rgba [4]float32)
	ClearDepth(t gpuHandle, flags uint32, depth float32, stencil uint8)
	Draw(d *gpuDraw) error
	Close()
}

// gpuDraw is one draw call.
type gpuDraw struct {
	Target, Depth gpuHandle    // 0: none
	MRT           [3]gpuHandle // more colour targets (0: none)
	MRTBlend      [3]d3d.BlendTarget
	W, H          int // the targets' size (the viewport covers them)
	Scissor       d3d.Rect
	Blend         d3d.BlendTarget
	BlendFactor   [4]float32
	DepthState    d3d.DepthStencilDesc
	StencilRef    uint32
	Raster        d3d.RasterizerDesc
	PS            gpuHandle
	PSConst       []float32
	// VS (0: none) runs the vertex program on Verts, packed attributes;
	// without it Verts holds each vertex's outputs (gpuVertRegs vec4s)
	VS      gpuHandle
	VSConst []float32
	Tex     [16]gpuHandle
	Samp    [16]d3d.SamplerDesc
	Verts   []float32 // gpuVertRegs vec4s a vertex
	Index   []uint32  // a triangle list
}

const gpuVertRegs = dxPosReg + 1

// hostSurf is a render target or depth buffer held by the GPU.
type hostSurf struct {
	addr, pitch uint32
	w, h        int
	fmt         uint32 // RSX colour format, or depth format (1 Z16, 2 Z24S8)
	depth       bool
	tex         gpuHandle
	host        uint32    // DXGI format
	bpp         int       // host bytes a pixel
	dirty       bool      // the GPU's copy is newer than memory
	stale       bool      // memory is newer than the GPU's copy
	asRGBA      gpuHandle // depth only: the A8R8G8B8 view (DepthToRGBA8)
	asRGBAOK    bool
}

func (s *hostSurf) end() uint32 { return s.addr + s.pitch*uint32(s.h) }

type hostTexKey struct {
	ea          uint32
	fmt         uint32
	w, h, pitch int
	linear      bool
}

type hostTex struct {
	tex     gpuHandle
	host    uint32
	pos     [4]int
	fill    [4]float32
	hash    uint64
	checked uint64 // frame the bytes were last compared
}

// gpuRenderer is the RSX's drawing on a host GPU.
type gpuRenderer struct {
	m       *Machine
	dev     gpuDevice
	surfs   []*hostSurf
	texs    map[hostTexKey]*hostTex
	ps      map[fpKey]gpuHandle
	psErr   map[fpKey]error
	clearPS gpuHandle
	seed    maphash.Seed
	frame   uint64
	buf     []byte
	verts   []float32
	index   []uint32
	scratch map[[3]int]gpuHandle // temporary copies by (w, h, format)
	vs      map[vpShaderKey]gpuHandle
	vsConst []float32
	vmap    map[uint32]int

	Draws, Fallbacks int
	// per-frame costs (debugging): targets read back to memory, memory
	// put into targets, textures made or changed, texture bytes checked
	Readbacks, Uploads, TexChanges, TexChecked int
	lastErr                                    string
}

// UseHostGPU makes the machine draw with dev.
func (m *Machine) UseHostGPU(dev gpuDevice) error {
	g := &gpuRenderer{m: m, dev: dev, texs: map[hostTexKey]*hostTex{}, ps: map[fpKey]gpuHandle{}, vs: map[vpShaderKey]gpuHandle{},
		psErr: map[fpKey]error{}, seed: maphash.MakeSeed(), scratch: map[[3]int]gpuHandle{}}
	h, err := dev.CreatePS(gpuClearPS())
	if err != nil {
		return err
	}
	g.clearPS = h
	m.gpu = g
	return nil
}

// gpuClearPS writes cb0[0] as the colour and cb0[1].x as the depth.
func gpuClearPS() []byte {
	s := &d3d.Shader{Pixel: true}
	s.Out = []d3d.Sig{{Name: "SV_Target", SV: d3d.SVTarget, Mask: 15},
		{Name: "SV_Depth", SV: d3d.SVDepth, Reg: 0xffffffff, Mask: 1}}
	s.DclCB(0, 2, false)
	s.DclOutput(0, 15)
	s.DclOutputDepth()
	s.Ins(d3d.OpMov, d3d.Out(0).Mask(15), d3d.CB(0, 0))
	s.Ins(d3d.OpMov, d3d.OutDepth(), d3d.CB(0, 1).C(0))
	s.Ins(d3d.OpRet)
	return s.Bytes()
}

func (g *gpuRenderer) fail(format string, a ...any) {
	msg := fmt.Sprintf(format, a...)
	if msg != g.lastErr {
		g.m.Logf("GPU: %s", msg)
		g.lastErr = msg
	}
}

// ---- surfaces

func surfHost(f uint32, depth bool) (uint32, int) {
	if depth {
		if f == 1 {
			return d3d.FormatR16Typeless, 2
		}
		return d3d.FormatR24G8Typeless, 4
	}
	switch f {
	case 11:
		return d3d.FormatRGBA16Float, 8
	case 12:
		return d3d.FormatRGBA32Float, 16
	case 13:
		return d3d.FormatR32Float, 4
	}
	return d3d.FormatRGBA8Unorm, 4
}

// surface finds (or makes) the host copy of a target.
func (g *gpuRenderer) surface(addr, pitch uint32, w, h int, f uint32, depth bool) *hostSurf {
	for _, s := range g.surfs {
		if s.addr == addr && s.depth == depth && s.pitch == pitch && s.fmt == f && s.w == w && s.h == h {
			if s.stale {
				g.upload(s)
			}
			return s
		}
	}
	n := &hostSurf{addr: addr, pitch: pitch, w: w, h: h, fmt: f, depth: depth}
	// whatever overlaps it goes back to memory and away
	g.syncRange(addr, n.end(), nil)
	keep := g.surfs[:0]
	for _, s := range g.surfs {
		if s.addr < n.end() && addr < s.end() {
			g.drop(s)
			continue
		}
		keep = append(keep, s)
	}
	g.surfs = keep
	n.host, n.bpp = surfHost(f, depth)
	kind := gpuTexTarget
	if depth {
		kind = gpuTexDepth
	}
	data := g.memToHost(n)
	t, err := g.dev.CreateTexture(w, h, n.host, kind, data, w*n.bpp)
	if err != nil {
		g.fail("target %dx%d: %v", w, h, err)
		return nil
	}
	n.tex = t
	g.surfs = append(g.surfs, n)
	return n
}

func (g *gpuRenderer) drop(s *hostSurf) {
	g.dev.Release(s.tex)
	if s.asRGBA != 0 {
		g.dev.Release(s.asRGBA)
	}
}

// upload copies memory into the host copy.
func (g *gpuRenderer) upload(s *hostSurf) {
	g.Uploads++
	g.dev.UpdateTexture(s.tex, g.memToHost(s), s.w*s.bpp)
	s.stale, s.dirty, s.asRGBAOK = false, false, false
}

// writeBack copies the host copy to memory.
func (g *gpuRenderer) writeBack(s *hostSurf) {
	if !s.dirty {
		return
	}
	g.Readbacks++
	if gpuStat {
		g.m.Logf("GPU readback %08X %dx%d fmt %d depth %v", s.addr, s.w, s.h, s.fmt, s.depth)
	}
	data, pitch, err := g.dev.ReadTexture(s.tex)
	if err != nil {
		g.fail("reading a target back: %v", err)
		return
	}
	g.hostToMem(s, data, pitch)
	s.dirty = false
}

// syncRange writes back dirty targets overlapping [a, b) (except skip).
func (g *gpuRenderer) syncRange(a, b uint32, skip *hostSurf) {
	for _, s := range g.surfs {
		if s != skip && s.dirty && s.addr < b && a < s.end() {
			g.writeBack(s)
		}
	}
}

// wroteRange marks targets overlapping [a, b) as older than memory.
func (g *gpuRenderer) wroteRange(a, b uint32) {
	for _, s := range g.surfs {
		if s.addr < b && a < s.end() {
			s.stale = true
			s.dirty = false
			s.asRGBAOK = false
		}
	}
}

// memToHost converts a target's memory to the host layout (w*bpp a row).
func (g *gpuRenderer) memToHost(s *hostSurf) []byte {
	out := make([]byte, s.w*s.h*s.bpp)
	row := make([]byte, int(s.pitch))
	rowBytes := s.w * memBpp(s)
	if rowBytes > len(row) {
		rowBytes = len(row)
	}
	for y := 0; y < s.h; y++ {
		g.m.Mem.ReadBytes(s.addr+uint32(y)*s.pitch, row[:rowBytes])
		o := out[y*s.w*s.bpp:]
		convertRow(s, row[:rowBytes], o, true)
	}
	return out
}

func (g *gpuRenderer) hostToMem(s *hostSurf, data []byte, pitch int) {
	rowBytes := s.w * memBpp(s)
	if rowBytes > int(s.pitch) {
		rowBytes = int(s.pitch)
	}
	row := make([]byte, rowBytes)
	for y := 0; y < s.h; y++ {
		convertRow(s, row, data[y*pitch:], false)
		g.m.Mem.WriteBytes(s.addr+uint32(y)*s.pitch, row)
	}
}

func memBpp(s *hostSurf) int {
	if s.depth {
		if s.fmt == 1 {
			return 2
		}
		return 4
	}
	return int(surfBpp(s.fmt))
}

// convertRow converts one row between memory (mem) and host (host)
// layouts, toHost choosing the direction.
func convertRow(s *hostSurf, mem, host []byte, toHost bool) {
	n := len(mem) / memBpp(s)
	if s.depth {
		if s.fmt == 1 {
			for x := 0; x < n; x++ {
				if toHost {
					host[2*x], host[2*x+1] = mem[2*x+1], mem[2*x]
				} else {
					mem[2*x], mem[2*x+1] = host[2*x+1], host[2*x]
				}
			}
			return
		}
		// memory: depth (24 bits) then stencil, big-endian; host: little
		// endian depth in bits 0-23, stencil in 24-31
		for x := 0; x < n; x++ {
			m, h := mem[4*x:4*x+4], host[4*x:4*x+4]
			if toHost {
				h[0], h[1], h[2], h[3] = m[2], m[1], m[0], m[3]
			} else {
				m[0], m[1], m[2], m[3] = h[2], h[1], h[0], h[3]
			}
		}
		return
	}
	switch s.fmt {
	case 11, 12, 13: // big-endian halves / floats -> little-endian
		k := 2
		if s.fmt != 11 {
			k = 4
		}
		for i := 0; i+k <= len(mem); i += k {
			for j := 0; j < k; j++ {
				if toHost {
					host[i+j] = mem[i+k-1-j]
				} else {
					mem[i+j] = host[i+k-1-j]
				}
			}
		}
	case 14, 15, 16: // bytes A, B, G, R
		for x := 0; x < n; x++ {
			m, h := mem[4*x:4*x+4], host[4*x:4*x+4]
			if toHost {
				a := m[0]
				if s.fmt != 16 {
					a = 255
				}
				h[0], h[1], h[2], h[3] = m[3], m[2], m[1], a
			} else {
				m[0], m[1], m[2], m[3] = h[3], h[2], h[1], h[0]
			}
		}
	case 3: // R5G6B5
		for x := 0; x < n; x++ {
			h := host[4*x : 4*x+4]
			if toHost {
				c := uint32(mem[2*x])<<8 | uint32(mem[2*x+1])
				h[0], h[1], h[2], h[3] = byte((c>>11)*255/31), byte((c>>5&63)*255/63), byte((c&31)*255/31), 255
			} else {
				c := uint16(h[0]>>3)<<11 | uint16(h[1]>>2)<<5 | uint16(h[2]>>3)
				mem[2*x], mem[2*x+1] = byte(c>>8), byte(c)
			}
		}
	case 1, 2: // X1R5G5B5
		for x := 0; x < n; x++ {
			h := host[4*x : 4*x+4]
			if toHost {
				c := uint32(mem[2*x])<<8 | uint32(mem[2*x+1])
				h[0], h[1], h[2], h[3] = byte((c>>10&31)*255/31), byte((c>>5&31)*255/31), byte((c&31)*255/31), 255
			} else {
				c := uint16(h[0]>>3)<<10 | uint16(h[1]>>3)<<5 | uint16(h[2]>>3)
				mem[2*x], mem[2*x+1] = byte(c>>8), byte(c)
			}
		}
	case 9: // B8
		for x := 0; x < n; x++ {
			h := host[4*x : 4*x+4]
			if toHost {
				v := mem[x]
				h[0], h[1], h[2], h[3] = v, v, v, v
			} else {
				mem[x] = h[0]
			}
		}
	case 10: // G8B8
		for x := 0; x < n; x++ {
			h := host[4*x : 4*x+4]
			if toHost {
				h[0], h[1], h[2], h[3] = mem[2*x], mem[2*x+1], 0, 255
			} else {
				mem[2*x], mem[2*x+1] = h[0], h[1]
			}
		}
	default: // bytes A, R, G, B
		for x := 0; x < n; x++ {
			m, h := mem[4*x:4*x+4], host[4*x:4*x+4]
			if toHost {
				a := m[0]
				if s.fmt != 8 {
					a = 255
				}
				h[0], h[1], h[2], h[3] = m[1], m[2], m[3], a
			} else {
				m[0], m[1], m[2], m[3] = h[3], h[0], h[1], h[2]
			}
		}
	}
}

// ---- textures

// bindTexture gives the host texture for unit i and how the shader reads
// it.
func (g *gpuRenderer) bindTexture(r *rsxState, i int, target, depth *hostSurf) (gpuHandle, gpuTexParams, d3d.SamplerDesc) {
	t := g.m.textureRegs(r, i)
	var tp gpuTexParams
	samp := d3d.SamplerDesc{Filter: d3d.FilterMinMagMipPoint, AddressU: d3d.AddressClamp, AddressV: d3d.AddressClamp,
		W: d3d.AddressClamp, MaxAnisotropy: 1, Compare: d3d.CmpNever, MaxLOD: 1000}
	if t.size == 0 {
		return 0, tp, samp // reads as 0 (rows all zero)
	}
	if t.magLinear || t.minLinear {
		samp.Filter = 0x14 // min/mag linear, mip point
	}
	samp.AddressU, samp.AddressV = texAddress(t.wrapS), texAddress(t.wrapT)
	samp.Border = [4]float32{t.border[0], t.border[1], t.border[2], t.border[3]}
	w, h := float32(t.w), float32(t.h)
	scale := func(sw, sh int) {
		if t.unnorm {
			tp.scale = [4]float32{1 / float32(sw), 1 / float32(sh), 1, 1}
		} else {
			tp.scale = [4]float32{w / float32(sw), h / float32(sh), 1, 1}
		}
	}
	// a render target?
	end := t.ea + uint32(t.size)
	for _, s := range g.surfs {
		if !(s.addr < end && t.ea < s.end()) {
			continue
		}
		if s.stale {
			break // memory has it
		}
		if hd, pos, fill, ok := g.alias(s, &t); ok {
			if hd == target.texOrZero() || hd == depth.texOrZero() {
				hd = g.copyOf(s, hd)
			}
			scale(s.w, s.h)
			tp.rows, tp.add = remapParams(pos, fill, t.remap)
			return hd, tp, samp
		}
		g.writeBack(s)
	}
	ht := g.memTexture(&t)
	if ht == nil {
		return 0, tp, samp
	}
	scale(t.w, t.h)
	tp.rows, tp.add = remapParams(ht.pos, ht.fill, t.remap)
	return ht.tex, tp, samp
}

func (s *hostSurf) texOrZero() gpuHandle {
	if s == nil {
		return 0
	}
	return s.tex
}

// copyOf copies a texture a draw also renders to.
func (g *gpuRenderer) copyOf(s *hostSurf, h gpuHandle) gpuHandle {
	if s.depth && h == s.tex {
		return 0 // can't copy a depth buffer that's bound; read as 0
	}
	k := [3]int{s.w, s.h, int(s.host)}
	c, ok := g.scratch[k]
	if !ok {
		var err error
		c, err = g.dev.CreateTexture(s.w, s.h, s.host, gpuTexTarget, nil, 0)
		if err != nil {
			return 0
		}
		g.scratch[k] = c
	}
	g.dev.CopyTexture(c, h)
	return c
}

// alias tells whether texture t can read target s directly: the host
// texture, and where the RSX's A, R, G, B channels are in its texels.
func (g *gpuRenderer) alias(s *hostSurf, t *rsxTexture) (gpuHandle, [4]int, [4]float32, bool) {
	var fill [4]float32
	if t.ea != s.addr || !t.linear || t.w > s.w || t.h > s.h {
		return 0, [4]int{}, fill, false
	}
	if t.pitch != 0 && uint32(t.pitch) != s.pitch && t.h > 1 {
		return 0, [4]int{}, fill, false
	}
	argb := [4]int{3, 0, 1, 2}
	if s.depth {
		switch {
		case s.fmt == 2 && (t.fmt == texD24S8 || t.fmt == texD24S8F):
			return s.tex, [4]int{0, 0, 0, 0}, fill, true
		case s.fmt == 1 && (t.fmt == texD16 || t.fmt == texD16F || t.fmt == texX16):
			return s.tex, [4]int{0, 0, 0, 0}, fill, true
		case s.fmt == 2 && (t.fmt == texA8R8G8B8 || t.fmt == texD8R8G8B8):
			if s.asRGBA == 0 {
				h, err := g.dev.CreateTexture(s.w, s.h, d3d.FormatRGBA8Unorm, gpuTexTarget, nil, 0)
				if err != nil {
					return 0, [4]int{}, fill, false
				}
				s.asRGBA = h
			}
			if !s.asRGBAOK {
				if err := g.dev.DepthToRGBA8(s.asRGBA, s.tex); err != nil {
					return 0, [4]int{}, fill, false
				}
				s.asRGBAOK = true
			}
			if t.fmt == texD8R8G8B8 {
				return s.asRGBA, [4]int{-1, 0, 1, 2}, [4]float32{1, 0, 0, 0}, true
			}
			return s.asRGBA, argb, fill, true
		}
		return 0, [4]int{}, fill, false
	}
	switch s.host {
	case d3d.FormatRGBA8Unorm:
		if surfBpp(s.fmt) != 4 {
			return 0, [4]int{}, fill, false
		}
		pos := argb
		if s.fmt == 14 || s.fmt == 15 || s.fmt == 16 {
			pos = [4]int{3, 2, 1, 0} // memory A, B, G, R read as A, R, G, B
		}
		switch t.fmt {
		case texA8R8G8B8:
			return s.tex, pos, fill, true
		case texD8R8G8B8:
			pos[0] = -1
			fill[0] = 1
			return s.tex, pos, fill, true
		}
	case d3d.FormatRGBA16Float:
		if t.fmt == texW16F {
			return s.tex, argb, fill, true
		}
	case d3d.FormatRGBA32Float:
		if t.fmt == texW32F {
			return s.tex, argb, fill, true
		}
	case d3d.FormatR32Float:
		if t.fmt == texX32F {
			return s.tex, [4]int{0, 0, 0, 0}, fill, true
		}
	}
	return 0, [4]int{}, fill, false
}

func texAddress(mode uint32) uint32 {
	switch mode {
	case 1:
		return d3d.AddressWrap
	case 2:
		return d3d.AddressMirror
	case 4, 7:
		return d3d.AddressBorder
	case 6, 8:
		return d3d.AddressMirrorOnce
	}
	return d3d.AddressClamp
}

// memTexture finds or uploads the texture at t's address.
func (g *gpuRenderer) memTexture(t *rsxTexture) *hostTex {
	key := hostTexKey{ea: t.ea, fmt: t.fmt, w: t.w, h: t.h, pitch: t.pitch, linear: t.linear}
	ht := g.texs[key]
	if ht != nil && ht.checked == g.frame {
		return ht
	}
	if cap(g.buf) < t.size {
		g.buf = make([]byte, t.size)
	}
	raw := g.buf[:t.size]
	g.m.Mem.ReadBytes(t.ea, raw)
	g.TexChecked += t.size
	hash := maphash.Bytes(g.seed, raw)
	if ht != nil && ht.hash == hash {
		ht.checked = g.frame
		return ht
	}
	g.TexChanges++
	tt := *t
	tt.data = raw
	format, data, pitch, pos, fill := hostTexData(&tt)
	if ht != nil && ht.host == format {
		g.dev.UpdateTexture(ht.tex, data, pitch)
	} else {
		if ht != nil {
			g.dev.Release(ht.tex)
		}
		h, err := g.dev.CreateTexture(t.w, t.h, format, gpuTexPlain, data, pitch)
		if err != nil {
			g.fail("texture %dx%d format %x: %v", t.w, t.h, t.fmt, err)
			delete(g.texs, key)
			return nil
		}
		ht = &hostTex{tex: h, host: format}
		g.texs[key] = ht
	}
	ht.pos, ht.fill, ht.hash, ht.checked = pos, fill, hash, g.frame
	return ht
}

// hostTexData converts a texture's texels (t.data) for the host: the
// format, the data, its pitch and where A, R, G, B are in a host texel.
func hostTexData(t *rsxTexture) (uint32, []byte, int, [4]int, [4]float32) {
	argb := [4]int{3, 0, 1, 2}
	var fill [4]float32
	if (t.fmt == texDXT1 || t.fmt == texDXT23 || t.fmt == texDXT45) && t.w%4 == 0 && t.h%4 == 0 {
		bs := 16
		f := uint32(dxgiBC3)
		switch t.fmt {
		case texDXT1:
			bs, f = 8, dxgiBC1
		case texDXT23:
			f = dxgiBC2
		}
		return f, t.data, t.w / 4 * bs, argb, fill
	}
	wide := false
	switch t.fmt {
	case texX16, texY16X16, texD16, texD16F, texD24S8, texD24S8F, texX32F, texY16X16F, texW16F, texW32F:
		wide = true
	}
	if !wide {
		out := make([]byte, t.w*t.h*4)
		for y := 0; y < t.h; y++ {
			for x := 0; x < t.w; x++ {
				c := t.texel(x, y)
				o := out[(y*t.w+x)*4:]
				o[0], o[1], o[2], o[3] = byte(u8c(c[1])), byte(u8c(c[2])), byte(u8c(c[3])), byte(u8c(c[0]))
			}
		}
		return d3d.FormatRGBA8Unorm, out, t.w * 4, argb, fill
	}
	out := make([]byte, t.w*t.h*16)
	for y := 0; y < t.h; y++ {
		for x := 0; x < t.w; x++ {
			c := t.texel(x, y)
			o := out[(y*t.w+x)*16:]
			for k, v := range [4]float32{c[1], c[2], c[3], c[0]} {
				u := math.Float32bits(v)
				o[4*k], o[4*k+1], o[4*k+2], o[4*k+3] = byte(u), byte(u>>8), byte(u>>16), byte(u>>24)
			}
		}
	}
	return d3d.FormatRGBA32Float, out, t.w * 16, argb, fill
}

// ---- draws

var glBlend = map[uint32]uint32{0: d3d.BlendZero, 1: d3d.BlendOne, 0x300: d3d.BlendSrcColor, 0x301: d3d.BlendInvSrcColor,
	0x302: d3d.BlendSrcAlpha, 0x303: d3d.BlendInvSrcAlpha, 0x304: d3d.BlendDestAlpha, 0x305: d3d.BlendInvDestAlpha,
	0x306: d3d.BlendDestColor, 0x307: d3d.BlendInvDestColor, 0x308: d3d.BlendSrcAlphaSat,
	0x8001: d3d.BlendFactor, 0x8002: d3d.BlendInvBlendFactor, 0x8003: d3d.BlendFactor, 0x8004: d3d.BlendInvBlendFactor}

func blendOp(eq uint32) uint32 {
	switch eq {
	case 0x8007:
		return d3d.BlendOpMin
	case 0x8008:
		return d3d.BlendOpMax
	case 0x800A:
		return d3d.BlendOpSub
	case 0x800B:
		return d3d.BlendOpRevSub
	}
	return d3d.BlendOpAdd
}

// alphaFactor is a blend factor for the alpha channel (no colour factors).
func alphaFactor(f uint32) uint32 {
	switch f {
	case d3d.BlendSrcColor:
		return d3d.BlendSrcAlpha
	case d3d.BlendInvSrcColor:
		return d3d.BlendInvSrcAlpha
	case d3d.BlendDestColor:
		return d3d.BlendDestAlpha
	case d3d.BlendInvDestColor:
		return d3d.BlendInvDestAlpha
	}
	return f
}

func glFactor(f uint32) uint32 {
	if v, ok := glBlend[f]; ok {
		return v
	}
	return d3d.BlendOne
}

// targets finds the draw's colour and depth targets.
func (g *gpuRenderer) targets(r *rsxState, s *rsxSurface, wantDepth bool) (col, dep *hostSurf, w, h int) {
	w, h = s.x+s.w, s.y+s.h
	col = g.surface(s.addr, s.pitch, w, h, s.fmt, false)
	if wantDepth && s.zAddr != 0 && s.zPitch >= 16 && s.zFmt != 0 {
		dep = g.surface(s.zAddr, s.zPitch, w, h, s.zFmt, true)
	}
	return
}

// draw runs one RSX draw on the GPU (false: it couldn't).
func (g *gpuRenderer) draw(r *rsxState, d *rsxDraw) bool {
	m := g.m
	s := m.surface(r)
	if !s.ok {
		return true
	}
	fp := m.loadFP(r, d)
	if fp == nil {
		return true
	}
	cmask := colorMask(r.reg(0x324))
	depthT := r.reg(0xA74) != 0
	depthW := r.reg(0xA70) != 0
	var mrtMask [3]uint32
	anyMask := cmask
	for k := 0; k < s.nmrt; k++ {
		mrtMask[k] = r.reg(0x370) >> (4 * (k + 1)) & 0xf
		anyMask |= mrtMask[k]
	}
	if anyMask == 0 && !(depthT && depthW) {
		return true
	}
	alphaT := r.reg(0x304) != 0
	key := fpKey{shape: fp.shape, out32: r.reg(0x1D60)&0x40 != 0, fixedOut: s.fmt != 11 && s.fmt != 12 && s.fmt != 13, mrt: uint8(s.nmrt)}
	if alphaT {
		key.alphaFn = r.reg(0x308)
	}
	ps, ok := g.ps[key]
	if !ok {
		if err := g.psErr[key]; err != nil {
			return false
		}
		code, err := translateFP(fp, key)
		if err == nil {
			ps, err = g.dev.CreatePS(code)
		}
		if err != nil {
			g.psErr[key] = err
			g.fail("pixel shader: %v", err)
			return false
		}
		g.ps[key] = ps
	}
	col, dep, w, h := g.targets(r, &s, depthT)
	if col == nil {
		return false
	}
	dr := &gpuDraw{Target: col.tex, W: w, H: h, PS: ps}
	if !g.gpuVertices(r, d, dr, w, h) {
		g.cpuVertices(r, d, dr, w, h)
	}
	if len(dr.Index) == 0 {
		return true
	}

	var mrts [3]*hostSurf
	for k := 0; k < s.nmrt; k++ {
		mrts[k] = g.surface(s.mrt[k].addr, s.mrt[k].pitch, w, h, s.fmt, false)
		if mrts[k] == nil {
			return false
		}
		dr.MRT[k] = mrts[k].tex
	}
	if dep != nil {
		dr.Depth = dep.tex
	}
	// scissor: the surface clip and the scissor box
	x0, y0, x1, y1 := s.x, s.y, s.x+s.w, s.y+s.h
	sh, sv := r.reg(0x8C0), r.reg(0x8C4)
	if sw, shh := int(sh>>16), int(sv>>16); sw > 0 && shh > 0 && sw < 4096 && shh < 4096 {
		x0, y0 = max(x0, int(sh&0xffff)), max(y0, int(sv&0xffff))
		x1, y1 = min(x1, int(sh&0xffff)+sw), min(y1, int(sv&0xffff)+shh)
	}
	if x0 >= x1 || y0 >= y1 {
		return true
	}
	dr.Scissor = d3d.Rect{Left: int32(x0), Top: int32(y0), Right: int32(x1), Bottom: int32(y1)}
	// blending and the colour mask
	bt := &dr.Blend
	bt.WriteMask = uint8(cmask>>2&1 | cmask>>1&1<<1 | cmask&1<<2 | cmask>>3&1<<3)
	if r.reg(0x310) != 0 {
		sf, df := r.reg(0x314), r.reg(0x318)
		eq := r.reg(0x320)
		bt.Enable = 1
		bt.Src, bt.Dst, bt.Op = glFactor(sf&0xffff), glFactor(df&0xffff), blendOp(eq&0xffff)
		bt.SrcAlpha, bt.DstAlpha, bt.OpAlpha = alphaFactor(glFactor(sf>>16)), alphaFactor(glFactor(df>>16)), blendOp(eq>>16)
		bc := r.reg(0x31C)
		dr.BlendFactor = [4]float32{float32(bc>>16&0xff) / 255, float32(bc>>8&0xff) / 255, float32(bc&0xff) / 255, float32(bc>>24) / 255}
		if sf&0xffff == 0x8003 || sf&0xffff == 0x8004 || df&0xffff == 0x8003 || df&0xffff == 0x8004 {
			a := dr.BlendFactor[3]
			dr.BlendFactor = [4]float32{a, a, a, a}
		}
	} else {
		bt.Src, bt.Dst, bt.Op = d3d.BlendOne, d3d.BlendZero, d3d.BlendOpAdd
		bt.SrcAlpha, bt.DstAlpha, bt.OpAlpha = d3d.BlendOne, d3d.BlendZero, d3d.BlendOpAdd
	}
	for k := 0; k < s.nmrt; k++ {
		mb := *bt
		if r.reg(0x36C)>>(k+1)&1 == 0 {
			mb.Enable = 0
			mb.Src, mb.Dst, mb.Op = d3d.BlendOne, d3d.BlendZero, d3d.BlendOpAdd
			mb.SrcAlpha, mb.DstAlpha, mb.OpAlpha = d3d.BlendOne, d3d.BlendZero, d3d.BlendOpAdd
		} else if mb.Enable == 0 {
			sf, df := r.reg(0x314), r.reg(0x318)
			eq := r.reg(0x320)
			mb.Enable = 1
			mb.Src, mb.Dst, mb.Op = glFactor(sf&0xffff), glFactor(df&0xffff), blendOp(eq&0xffff)
			mb.SrcAlpha, mb.DstAlpha, mb.OpAlpha = alphaFactor(glFactor(sf>>16)), alphaFactor(glFactor(df>>16)), blendOp(eq>>16)
		}
		mm := mrtMask[k]
		mb.WriteMask = uint8(mm>>2&1 | mm>>1&1<<1 | mm&1<<2 | mm>>3&1<<3)
		dr.MRTBlend[k] = mb
	}
	// depth
	ds := &dr.DepthState
	ds.Front = d3d.StencilOps{Fail: d3d.StencilKeep, DepthFail: d3d.StencilKeep, Pass: d3d.StencilKeep, Func: d3d.CmpAlways}
	ds.Back = ds.Front
	if dep != nil && depthT {
		ds.DepthEnable = 1
		fn := r.reg(0xA6C)
		if fn >= 0x200 && fn <= 0x207 {
			ds.DepthFunc = fn - 0x200 + 1
		} else {
			ds.DepthFunc = d3d.CmpAlways
		}
		if depthW {
			ds.DepthWriteMask = 1
		}
	} else {
		ds.DepthFunc = d3d.CmpAlways
	}
	// culling
	rs := &dr.Raster
	rs.Fill, rs.Cull, rs.Scissor = d3d.FillSolid, d3d.CullNone, 1
	if r.reg(0x183C) != 0 {
		switch r.reg(0x1830) {
		case 0x404:
			rs.Cull = d3d.CullFront
		case 0x405:
			rs.Cull = d3d.CullBack
		case 0x408:
			return true
		}
	}
	if r.reg(0x1834) == 0x901 {
		rs.FrontCCW = 1
	}
	// textures and constants
	var tps [16]gpuTexParams
	for u := 0; u < 16; u++ {
		if fp.usesTex>>u&1 != 0 {
			dr.Tex[u], tps[u], dr.Samp[u] = g.bindTexture(r, u, col, dep)
		}
	}
	dr.PSConst = fpConstBlock(fp, float32(r.reg(0x30C)&0xff)/255, &tps)
	if err := g.dev.Draw(dr); err != nil {
		g.fail("draw: %v", err)
		return false
	}
	g.Draws++
	if cmask != 0 {
		col.dirty = true
		col.stale = false
	}
	for k := 0; k < s.nmrt; k++ {
		if mrtMask[k] != 0 {
			mrts[k].dirty = true
			mrts[k].stale = false
		}
	}
	if dep != nil && depthT && depthW {
		dep.dirty = true
		dep.asRGBAOK = false
	}
	return true
}

// cpuVertices runs the vertex program on the CPU: the buffer holds each
// vertex's outputs (the position in Direct3D clip space, outputs 1-15, and
// (window z, 0, 0, 1/w)) for the pass-through vertex shader.
func (g *gpuRenderer) cpuVertices(r *rsxState, d *rsxDraw, dr *gpuDraw, w, h int) {
	verts, vlist := g.m.rsxVertices(r, d)
	dr.Index = g.triangles(d.prim, vlist)
	if len(dr.Index) == 0 {
		return
	}
	sx, sy, sz := r.regF(0xA30), r.regF(0xA34), r.regF(0xA38)
	ox, oy, oz := r.regF(0xA20), r.regF(0xA24), r.regF(0xA28)
	vb := g.verts[:0]
	for i := range verts {
		o := &verts[i].out
		x, y, z, wc := o[0][0], o[0][1], o[0][2], o[0][3]
		vb = append(vb,
			2*(x*sx+ox*wc)/float32(w)-wc,
			wc-2*(y*sy+oy*wc)/float32(h),
			z*sz+oz*wc,
			wc)
		for k := 1; k < 16; k++ {
			vb = append(vb, o[k][0], o[k][1], o[k][2], o[k][3])
		}
		iw := 1 / wc
		if wc == 0 {
			iw = 1e6
		}
		vb = append(vb, z*iw*sz+oz, 0, 0, iw)
	}
	g.verts = vb
	dr.Verts = vb
}

// gpuNoVS (PS3_NOVS): transform every vertex on the CPU (debugging)
var gpuNoVS = os.Getenv("PS3_NOVS") != ""

// gpuVertices has the GPU run the vertex program (false: it can't, see
// translateVP): the buffer holds each vertex's enabled attributes.
func (g *gpuRenderer) gpuVertices(r *rsxState, d *rsxDraw, dr *gpuDraw, w, h int) bool {
	if gpuNoVS || len(d.inline) > 0 {
		return false
	}
	m := g.m
	m.rsxVP(d, r)
	pcs, hash, ok := d.vpTraced()
	if !ok {
		return false
	}
	inMask := r.reg(0x1FF0) & 0xffff
	if inMask == 0 {
		return false
	}
	key := vpShaderKey{hash: hash, slots: inMask}
	vs, ok := g.vs[key]
	if !ok {
		code, err := translateVP(d.vpProg, pcs, inMask)
		if err == nil {
			vs, err = g.dev.CreateVS(code)
		}
		if err != nil {
			g.m.Logf("GPU: vertex shader: %v (the CPU transforms these)", err)
			vs = 0
		}
		g.vs[key] = vs
	}
	if vs == 0 {
		return false
	}
	uniq, vlist := dedupe(m.rsxIndices(r, d), g.vcache())
	dr.Index = g.triangles(d.prim, vlist)
	if len(dr.Index) == 0 {
		return true
	}
	vb := g.verts[:0]
	for _, i := range uniq {
		for a := 0; a < 16; a++ {
			if inMask>>a&1 != 0 {
				v := m.fetchAttr(r, d, a, i)
				vb = append(vb, v[0], v[1], v[2], v[3])
			}
		}
	}
	g.verts = vb
	dr.Verts = vb
	dr.VS = vs
	cb := g.vsConst[:0]
	for i := 0; i < 468; i++ {
		cb = append(cb, d.vc[i][0], d.vc[i][1], d.vc[i][2], d.vc[i][3])
	}
	cb = append(cb, make([]float32, 4*(vxConstVP-468))...)
	cb = append(cb, r.regF(0xA30), r.regF(0xA34), r.regF(0xA38), 0,
		r.regF(0xA20), r.regF(0xA24), r.regF(0xA28), 0,
		float32(w), float32(h), 0, 0)
	g.vsConst = cb
	dr.VSConst = cb
	return true
}

// vcache is an empty index map (kept between draws).
func (g *gpuRenderer) vcache() map[uint32]int {
	if g.vmap == nil {
		g.vmap = map[uint32]int{}
	}
	clear(g.vmap)
	return g.vmap
}

// triangles turns a primitive's vertex list into a triangle list, as
// rasterPrims does.
func (g *gpuRenderer) triangles(prim uint32, vlist []int) []uint32 {
	out := g.index[:0]
	emit := func(a, b, c int) {
		if a >= 0 && b >= 0 && c >= 0 {
			out = append(out, uint32(a), uint32(b), uint32(c))
		}
	}
	n := len(vlist)
	switch prim {
	case 5:
		for i := 0; i+2 < n; i += 3 {
			emit(vlist[i], vlist[i+1], vlist[i+2])
		}
	case 6:
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
	case 7, 10:
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
	case 8:
		for i := 0; i+3 < n; i += 4 {
			emit(vlist[i], vlist[i+1], vlist[i+2])
			emit(vlist[i], vlist[i+2], vlist[i+3])
		}
	case 9:
		for i := 0; i+3 < n; i += 2 {
			emit(vlist[i], vlist[i+1], vlist[i+3])
			emit(vlist[i], vlist[i+3], vlist[i+2])
		}
	}
	g.index = out
	return out
}

// clear does CLEAR_SURFACE (v: 1 depth, 2 stencil, 0x10/20/40/80 R/G/B/A).
func (g *gpuRenderer) clear(r *rsxState, v uint32) {
	s := g.m.surface(r)
	if !s.ok {
		return
	}
	wantZ := v&3 != 0
	col, dep, w, h := g.targets(r, &s, wantZ)
	x0, y0, x1, y1 := s.x, s.y, s.x+s.w, s.y+s.h
	sh, sv := r.reg(0x8C0), r.reg(0x8C4)
	if sw, shh := int(sh>>16), int(sv>>16); sw > 0 && shh > 0 {
		x0, y0 = max(x0, int(sh&0xffff)), max(y0, int(sv&0xffff))
		x1, y1 = min(x1, int(sh&0xffff)+sw), min(y1, int(sv&0xffff)+shh)
	}
	if x0 >= x1 || y0 >= y1 {
		return
	}
	full := x0 == 0 && y0 == 0 && x1 == w && y1 == h
	c := r.reg(0x1D90)
	rgba := [4]float32{float32(c>>16&0xff) / 255, float32(c>>8&0xff) / 255, float32(c&0xff) / 255, float32(c>>24) / 255}
	var mask uint8
	if v&0x10 != 0 {
		mask |= 1
	}
	if v&0x20 != 0 {
		mask |= 2
	}
	if v&0x40 != 0 {
		mask |= 4
	}
	if v&0x80 != 0 {
		mask |= 8
	}
	zv := r.reg(0x1D8C)
	depth := float32(zv>>8) / 0xffffff
	stencil := uint8(zv)
	if s.zFmt == 1 {
		depth = float32(zv&0xffff) / 0xffff
	}
	var zflags uint32
	if dep != nil {
		if v&1 != 0 {
			zflags |= d3d.ClearDepth
		}
		if v&2 != 0 && dep.fmt == 2 {
			zflags |= d3d.ClearStencil
		}
	}
	if full {
		if col != nil && mask == 15 {
			g.dev.ClearTarget(col.tex, rgba)
			col.dirty, col.stale = true, false
			for k := 0; k < s.nmrt; k++ { // every target being drawn is cleared
				if mt := g.surface(s.mrt[k].addr, s.mrt[k].pitch, w, h, s.fmt, false); mt != nil {
					g.dev.ClearTarget(mt.tex, rgba)
					mt.dirty, mt.stale = true, false
				}
			}
			mask = 0
		}
		if zflags != 0 {
			g.dev.ClearDepth(dep.tex, zflags, depth, stencil)
			dep.dirty, dep.asRGBAOK = true, false
			zflags = 0
		}
	}
	if mask == 0 && zflags&d3d.ClearDepth == 0 {
		return
	}
	// a quad over the rectangle
	dr := &gpuDraw{W: w, H: h, PS: g.clearPS, Scissor: d3d.Rect{Left: int32(x0), Top: int32(y0), Right: int32(x1), Bottom: int32(y1)}}
	if col != nil {
		dr.Target = col.tex
	}
	dr.Blend = d3d.BlendTarget{Src: d3d.BlendOne, Dst: d3d.BlendZero, Op: d3d.BlendOpAdd,
		SrcAlpha: d3d.BlendOne, DstAlpha: d3d.BlendZero, OpAlpha: d3d.BlendOpAdd, WriteMask: mask}
	dr.DepthState.Front = d3d.StencilOps{Fail: d3d.StencilKeep, DepthFail: d3d.StencilKeep, Pass: d3d.StencilKeep, Func: d3d.CmpAlways}
	dr.DepthState.Back = dr.DepthState.Front
	dr.DepthState.DepthFunc = d3d.CmpAlways
	if zflags&d3d.ClearDepth != 0 {
		dr.Depth = dep.tex
		dr.DepthState.DepthEnable = 1
		dr.DepthState.DepthWriteMask = 1
	}
	dr.Raster = d3d.RasterizerDesc{Fill: d3d.FillSolid, Cull: d3d.CullNone, Scissor: 1}
	dr.PSConst = []float32{rgba[0], rgba[1], rgba[2], rgba[3], depth, 0, 0, 0}
	quad := [4][2]float32{{-1, 1}, {1, 1}, {-1, -1}, {1, -1}}
	vb := g.verts[:0]
	for _, q := range quad {
		vb = append(vb, q[0], q[1], 0, 1)
		vb = append(vb, make([]float32, 4*(gpuVertRegs-1))...)
	}
	g.verts = vb
	dr.Verts = vb
	dr.Index = []uint32{0, 1, 2, 2, 1, 3}
	if err := g.dev.Draw(dr); err != nil {
		g.fail("clear: %v", err)
		return
	}
	if mask != 0 && col != nil {
		col.dirty, col.stale = true, false
	}
	if zflags&d3d.ClearDepth != 0 {
		dep.dirty, dep.asRGBAOK = true, false
	}
}

// screen fills sc from the target at addr if the GPU has it.
func (g *gpuRenderer) screen(addr, pitch uint32, w, h int, sc *Screen) bool {
	for _, s := range g.surfs {
		if s.addr != addr || s.depth || s.stale || s.host != d3d.FormatRGBA8Unorm || s.w < w || s.h < h {
			continue
		}
		if !s.dirty {
			return false // memory is as new
		}
		data, p, err := g.dev.ReadTextureLate(s.tex)
		if err != nil {
			g.fail("reading the picture: %v", err)
			return false
		}
		swap := s.fmt == 14 || s.fmt == 15 || s.fmt == 16
		for y := 0; y < h; y++ {
			row := data[y*p:]
			for x := 0; x < w; x++ {
				r, gg, b := uint32(row[4*x]), uint32(row[4*x+1]), uint32(row[4*x+2])
				if swap {
					r, b = b, r
				}
				sc.Pix[y*w+x] = r<<16 | gg<<8 | b
			}
		}
		return true
	}
	return false
}

// endFrame is called at each flip.
func (g *gpuRenderer) endFrame() {
	if gpuStat {
		g.m.Logf("GPU frame %d: %d draws, %d software, %d readbacks, %d uploads, %d textures changed, %d KB of textures checked",
			g.frame, g.Draws, g.Fallbacks, g.Readbacks, g.Uploads, g.TexChanges, g.TexChecked>>10)
		if vpStat {
			g.m.Logf("VP: %d draws, %d vertices, %d instructions (at most)", vpStatDraws, vpStatVerts, vpStatInsts)
			vpStatDraws, vpStatVerts, vpStatInsts = 0, 0, 0
		}
		g.Draws, g.Fallbacks, g.Readbacks, g.Uploads, g.TexChanges, g.TexChecked = 0, 0, 0, 0, 0, 0
	}
	g.frame++
}

// gpuStat (PS3_GPUSTAT): log what each frame cost the GPU renderer
var gpuStat = os.Getenv("PS3_GPUSTAT") != ""

// readMem / wroteMem keep memory and the GPU in step around the CPU-side
// 2D engines.
func (m *Machine) gpuReadMem(a, n uint32) {
	if m.gpu != nil && n > 0 {
		m.gpu.syncRange(a, a+n, nil)
	}
}

func (m *Machine) gpuWroteMem(a, n uint32) {
	if m.gpu != nil && n > 0 {
		m.gpu.wroteRange(a, a+n)
	}
}

// Close lets go of the host GPU (if the machine drew with one).
func (m *Machine) Close() {
	if m.gpu != nil {
		m.gpu.dev.Close()
		m.gpu = nil
	}
}

// Renderer names what draws the pictures.
func (m *Machine) Renderer() string {
	if m.gpu != nil {
		return "the graphics card"
	}
	return "the CPU (software)"
}
