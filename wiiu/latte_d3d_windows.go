//go:build windows

package wiiu

// The Direct3D 11 backend: draws run on the host GPU with the shaders
// latte_dxbc.go translates. Render targets and depth buffers live in host
// textures while the GPU uses them, and go back to guest memory (tiled, in
// the guest's format) when anything else reads that memory. Textures,
// vertex buffers and constants are copied from guest memory as draws use
// them, and kept until the memory is invalidated.

import (
	"encoding/binary"
	"fmt"
	"hash/maphash"
	"unsafe"

	"retropixels/wiiu/d3d"
)

type d3dBackend struct {
	g *Latte
	d *d3d.Device
	c d3d.Context

	surfs   []*hostSurf
	texs    map[hostTexKey]*hostTex
	bufs    map[[2]uint32]*hostBuf // vertex buffers and constant banks by (base, size)
	shaders map[shaderKey][]*hostShader
	states  map[string]d3d.Object

	sysCB, fileCB [2]d3d.Resource // per stage (VS, PS)
	ib            d3d.Resource
	ibSize        int
	seed          maphash.Seed
	err           string // why the last draw fell back
}

// newD3DBackend makes the backend on a new device (WARP if warp is set).
func newD3DBackend(g *Latte, warp bool) (*d3dBackend, error) {
	d, err := d3d.NewDevice(warp)
	if err != nil {
		return nil, err
	}
	b := &d3dBackend{g: g, d: d, c: d.Ctx, texs: map[hostTexKey]*hostTex{}, bufs: map[[2]uint32]*hostBuf{},
		shaders: map[shaderKey][]*hostShader{}, states: map[string]d3d.Object{}, seed: maphash.MakeSeed()}
	for i := 0; i < 2; i++ {
		if b.sysCB[i], err = b.dynBuffer(dxSysSize*16, d3d.BindConstantBuffer); err != nil {
			return nil, err
		}
		if b.fileCB[i], err = b.dynBuffer(256*16, d3d.BindConstantBuffer); err != nil {
			return nil, err
		}
	}
	return b, nil
}

func (b *d3dBackend) dynBuffer(size int, bind uint32) (d3d.Resource, error) {
	return b.d.CreateBuffer(&d3d.BufferDesc{ByteWidth: uint32(size), Usage: d3d.UsageDynamic, BindFlags: bind,
		CPUAccess: d3d.CPUWrite}, nil)
}

// upload copies data into a dynamic buffer.
func (b *d3dBackend) upload(r d3d.Resource, data []byte) {
	m, err := b.c.Map(r, 0, d3d.MapWriteDiscard)
	if err != nil {
		return
	}
	copy(m.Bytes(len(data)), data)
	b.c.Unmap(r, 0)
}

func (b *d3dBackend) close() {
	b.flushAll()
	for _, s := range b.surfs {
		s.release()
	}
	for _, t := range b.texs {
		t.release()
	}
	for _, v := range b.bufs {
		v.release()
	}
	for _, l := range b.shaders {
		for _, s := range l {
			s.obj.Release()
		}
	}
	for _, o := range b.states {
		o.Release()
	}
	for i := 0; i < 2; i++ {
		b.sysCB[i].Release()
		b.fileCB[i].Release()
	}
	b.ib.Release()
	b.d.Close()
}

// ---- surfaces ----

// hostSurf is one slice of a colour or depth buffer, held in a host
// texture.
type hostSurf struct {
	depth   bool
	lay     surfLayout
	format  uint32 // colour format, or DB_DEPTH_INFO.FORMAT
	kind    numKind
	slice   uint32
	lo, hi  uint32
	w, h    uint32
	hostFmt uint32
	tex     d3d.Resource
	staging d3d.Resource
	view    d3d.View // render target or depth stencil view
	dirty   bool     // the host has drawn into it since it was written back
}

func (s *hostSurf) release() {
	s.view.Release()
	s.tex.Release()
	s.staging.Release()
}

func (s *hostSurf) same(depth bool, lay *surfLayout, format uint32, kind numKind, slice uint32) bool {
	return s.depth == depth && s.lay == *lay && s.format == format && s.kind == kind && s.slice == slice
}

// surface gives the host copy of a surface, making it from guest memory
// (and dropping copies it overlaps) if needed.
func (b *d3dBackend) surface(depth bool, lay *surfLayout, format uint32, kind numKind, slice uint32) (*hostSurf, error) {
	lo, hi := surfRange(lay, slice)
	for _, s := range b.surfs {
		if s.same(depth, lay, format, kind, slice) {
			return s, nil
		}
	}
	b.flush(lo, hi)
	b.dropSurfs(lo, hi)
	b.dropTextures(lo, hi)
	s := &hostSurf{depth: depth, lay: *lay, format: format, kind: kind, slice: slice, lo: lo, hi: hi,
		w: lay.Pitch, h: lay.Height}
	var err error
	if depth {
		texFmt, dsvFmt, _, _ := hostDepthFormats(format)
		s.hostFmt = texFmt
		s.tex, err = b.d.CreateTexture2D(&d3d.Texture2DDesc{Width: s.w, Height: s.h, MipLevels: 1, ArraySize: 1,
			Format: texFmt, SampleCount: 1, BindFlags: d3d.BindDepthStencil}, nil, 0)
		if err == nil {
			s.view, err = b.d.CreateDSV(s.tex, dsvFmt)
		}
	} else {
		s.hostFmt = hostColorFormat(format, kind)
		s.tex, err = b.d.CreateTexture2D(&d3d.Texture2DDesc{Width: s.w, Height: s.h, MipLevels: 1, ArraySize: 1,
			Format: s.hostFmt, SampleCount: 1, BindFlags: d3d.BindRenderTarget | d3d.BindShaderResource}, nil, 0)
		if err == nil {
			s.view, err = b.d.CreateRTV(s.tex, s.hostFmt)
		}
	}
	if err == nil {
		s.staging, err = b.d.CreateTexture2D(&d3d.Texture2DDesc{Width: s.w, Height: s.h, MipLevels: 1, ArraySize: 1,
			Format: s.hostFmt, SampleCount: 1, Usage: d3d.UsageStaging, CPUAccess: d3d.CPURead | d3d.CPUWrite}, nil, 0)
	}
	if err != nil {
		s.release()
		return nil, err
	}
	if err := b.load(s); err != nil {
		s.release()
		return nil, err
	}
	b.surfs = append(b.surfs, s)
	return s, nil
}

// load copies a surface from guest memory to the host.
func (b *d3dBackend) load(s *hostSurf) error {
	g := b.g
	m, err := b.c.Map(s.staging, 0, d3d.MapReadWrite)
	if err != nil {
		return err
	}
	data := m.Bytes(int(m.RowPitch) * int(s.h))
	addrs := make([]uint32, s.w)
	buf := make([]byte, 16)
	if s.depth {
		_, _, _, pb := hostDepthFormats(s.format)
		db := dbDesc{lay: s.lay, format: s.format, slice: s.slice, ok: true}
		for y := uint32(0); y < s.h; y++ {
			s.lay.rowAddrs(0, y, s.slice, addrs)
			row := data[y*m.RowPitch:]
			for x, a := range addrs {
				z, st := g.readDepth(&db, a)
				hostDepthEncode(s.format, z, st, row[x*pb:])
			}
		}
	} else {
		pb := hostPixelBytes(s.hostFmt)
		n := fmtBytes(s.format)
		for y := uint32(0); y < s.h; y++ {
			s.lay.rowAddrs(0, y, s.slice, addrs)
			row := data[y*m.RowPitch:]
			for x, a := range addrs {
				v := g.readElem(&s.lay, s.format, s.kind, a, buf[:n])
				hostEncode(s.hostFmt, v, row[x*pb:])
			}
		}
	}
	b.c.Unmap(s.staging, 0)
	b.c.CopyResource(s.tex, s.staging)
	return nil
}

// store writes a surface back to guest memory.
func (b *d3dBackend) store(s *hostSurf) {
	g := b.g
	b.c.CopyResource(s.staging, s.tex)
	m, err := b.c.Map(s.staging, 0, d3d.MapRead)
	if err != nil {
		return
	}
	data := m.Bytes(int(m.RowPitch) * int(s.h))
	addrs := make([]uint32, s.w)
	buf := make([]byte, 16)
	if s.depth {
		_, _, _, pb := hostDepthFormats(s.format)
		db := dbDesc{lay: s.lay, format: s.format, slice: s.slice, ok: true}
		for y := uint32(0); y < s.h; y++ {
			s.lay.rowAddrs(0, y, s.slice, addrs)
			row := data[y*m.RowPitch:]
			for x, a := range addrs {
				z, st := hostDepthDecode(s.format, row[x*pb:])
				g.writeDepth(&db, a, z, st, true, true)
			}
		}
	} else {
		pb := hostPixelBytes(s.hostFmt)
		n := fmtBytes(s.format)
		for y := uint32(0); y < s.h; y++ {
			s.lay.rowAddrs(0, y, s.slice, addrs)
			row := data[y*m.RowPitch:]
			for x, a := range addrs {
				v := hostDecode(s.hostFmt, row[x*pb:])
				encodeElem(s.format, s.kind, v, buf[:n])
				g.mem.WriteBytes(a, buf[:n])
			}
		}
	}
	b.c.Unmap(s.staging, 0)
	s.dirty = false
}

func (b *d3dBackend) flush(lo, hi uint32) {
	for _, s := range b.surfs {
		if s.dirty && s.lo < hi && lo < s.hi {
			b.store(s)
		}
	}
}

func (b *d3dBackend) flushAll() { b.flush(0, ^uint32(0)) }

// dropSurfs forgets the host copies of surfaces in [lo, hi) (written back
// first by the caller if they matter).
func (b *d3dBackend) dropSurfs(lo, hi uint32) {
	k := 0
	for _, s := range b.surfs {
		if s.lo < hi && lo < s.hi {
			s.release()
			continue
		}
		b.surfs[k] = s
		k++
	}
	b.surfs = b.surfs[:k]
}

func (b *d3dBackend) invalidate(lo, hi uint32) {
	b.dropSurfs(lo, hi)
	b.dropTextures(lo, hi)
	for k, v := range b.bufs {
		if v.lo < hi && lo < v.hi {
			v.release()
			delete(b.bufs, k)
		}
	}
}

func (b *d3dBackend) clearColor(cb *cbDesc, c [4]float32) bool {
	for _, s := range b.surfs {
		if s.same(false, &cb.lay, cb.format, cb.kind, cb.slice) {
			b.dropTextures(s.lo, s.hi)
			b.c.ClearRenderTargetView(s.view, c)
			s.dirty = true
			return true
		}
	}
	return false
}

func (b *d3dBackend) clearDepth(db *dbDesc, z float32, stencil, flags uint32) bool {
	for _, s := range b.surfs {
		if s.same(true, &db.lay, db.format, 0, db.slice) {
			b.dropTextures(s.lo, s.hi)
			b.c.ClearDepthStencilView(s.view, flags&3, z, uint8(stencil))
			s.dirty = true
			return true
		}
	}
	return false
}

// ---- textures ----

// hostTexKey: the resource words a texture is made from
type hostTexKey struct{ w [6]uint32 }

type hostTex struct {
	lo, hi uint32
	tex    d3d.Resource
	srv    d3d.View
}

func (t *hostTex) release() {
	t.srv.Release()
	t.tex.Release()
}

func (b *d3dBackend) dropTextures(lo, hi uint32) {
	for k, t := range b.texs {
		if t.lo < hi && lo < t.hi {
			t.release()
			delete(b.texs, k)
		}
	}
}

// texKeyFor is what the shader translation needs to know of a texture.
func texKeyFor(t *texDesc) dxTexKey {
	if !t.valid {
		return dxTexKey{}
	}
	k := dxTexKey{valid: true, dim: uint8(t.dim), intFmt: t.kind == kUint || t.kind == kSint}
	for c := 0; c < 4; c++ {
		k.sel[c] = uint8(t.dstSel[c])
	}
	return k
}

// texture gives the host copy of a texture.
func (b *d3dBackend) texture(t *texDesc) (*hostTex, error) {
	key := hostTexKey{}
	copy(key.w[:], t.w[:6])
	key.w[4] &^= 0x7ff << 16 // the swizzle is the shader's business
	if h := b.texs[key]; h != nil {
		return h, nil
	}
	switch t.dim {
	case 1, 3, 5, 7:
	default:
		return nil, fmt.Errorf("texture dimension %d", t.dim)
	}
	g := b.g
	levels := t.lastLevel - t.baseLevel + 1
	if t.lastLevel < t.baseLevel {
		levels = 1
	}
	levels = min(levels, 14)
	l0 := g.texLevel(t, t.baseLevel)
	// the memory the levels came from: render targets there go back first
	var decoded []*texLevel
	for i := uint32(0); i < levels; i++ {
		l := g.texLevel(t, t.baseLevel+i)
		b.flush(l.lo, l.hi)
		decoded = append(decoded, l)
	}
	// (decoding again if a flush changed the memory)
	for i := range decoded {
		decoded[i] = g.texLevel(t, t.baseLevel+uint32(i))
	}
	l0 = decoded[0]
	intFmt := t.kind == kUint || t.kind == kSint
	one := fmtDescs[t.format&0x3f].nc == 1 && !intFmt && l0.rgba8 == nil
	format := uint32(d3d.FormatRGBA32Float)
	pb := 16
	switch {
	case intFmt:
		format = d3d.FormatRGBA32Uint
	case l0.rgba8 != nil:
		format, pb = d3d.FormatRGBA8Unorm, 4
		if l0.srgb {
			format = d3d.FormatRGBA8UnormSRGB
		}
	case one:
		// one component (depth textures among them): R32F can be
		// sampled with a comparison
		format, pb = d3d.FormatR32Float, 4
	}
	slices := l0.d
	if t.dim == 3 {
		slices = 6 * max(l0.d/6, 1)
	}
	desc := &d3d.Texture2DDesc{Width: l0.w, Height: l0.h, MipLevels: levels, ArraySize: slices, Format: format,
		SampleCount: 1, BindFlags: d3d.BindShaderResource}
	if t.dim == 3 {
		desc.Misc = 4 // TEXTURECUBE
	}
	tex, err := b.d.CreateTexture2D(desc, nil, 0)
	if err != nil {
		return nil, err
	}
	h := &hostTex{lo: ^uint32(0), tex: tex}
	for i, l := range decoded {
		h.lo, h.hi = min(h.lo, l.lo), max(h.hi, l.hi)
		w, hh := max(l0.w>>i, 1), max(l0.h>>i, 1)
		if l.w != w || l.h != hh {
			continue
		}
		data := make([]byte, int(w*hh)*pb)
		for z := uint32(0); z < min(l.d, slices); z++ {
			n := int(w * hh)
			off := int(z) * n
			for p := 0; p < n; p++ {
				o := data[p*pb:]
				switch {
				case l.rgba8 != nil:
					binary.LittleEndian.PutUint32(o, l.rgba8[off+p])
				case pb == 4:
					binary.LittleEndian.PutUint32(o, l.vals[(off+p)*4])
				default:
					for c := 0; c < 4; c++ {
						binary.LittleEndian.PutUint32(o[4*c:], l.vals[(off+p)*4+c])
					}
				}
			}
			b.c.UpdateSubresource(tex, uint32(i)+z*levels, data, w*uint32(pb), 0)
		}
	}
	dim := uint32(4) // TEXTURE2D
	switch t.dim {
	case 3:
		dim = 9
	case 5, 7:
		dim = 5
	}
	h.srv, err = b.d.CreateTexSRV(tex, format, dim, levels, slices)
	if err != nil {
		tex.Release()
		return nil, err
	}
	b.texs[key] = h
	return h, nil
}

// ---- buffers ----

type hostBuf struct {
	lo, hi uint32
	hash   uint64
	buf    d3d.Resource
	srv    d3d.View
}

func (v *hostBuf) release() {
	v.srv.Release()
	v.buf.Release()
}

// smallBuf: buffers this size or smaller are checked for changes every
// draw (bigger ones only change through invalidation).
const smallBuf = 64 << 10

// buffer gives a host copy of guest memory [base, base+size), as a vertex
// buffer (a view of 32-bit words) or a constant buffer.
func (b *d3dBackend) buffer(base, size uint32, cb bool) (*hostBuf, error) {
	size = (size + 15) &^ 15
	key := [2]uint32{base, size | b2u(cb)}
	data := make([]byte, size)
	b.g.mem.ReadBytes(base, data)
	if cb {
		// constants are big-endian words
		for i := 0; i+3 < len(data); i += 4 {
			data[i], data[i+1], data[i+2], data[i+3] = data[i+3], data[i+2], data[i+1], data[i]
		}
	}
	v := b.bufs[key]
	if v != nil {
		if size > smallBuf || v.hash == maphash.Bytes(b.seed, data) {
			return v, nil
		}
		v.release()
		delete(b.bufs, key)
	}
	b.flush(base, base+size)
	bind := uint32(d3d.BindShaderResource)
	if cb {
		bind = d3d.BindConstantBuffer
	}
	buf, err := b.d.CreateBuffer(&d3d.BufferDesc{ByteWidth: size, BindFlags: bind}, data)
	if err != nil {
		return nil, err
	}
	v = &hostBuf{lo: base, hi: base + size, hash: maphash.Bytes(b.seed, data), buf: buf}
	if !cb {
		if v.srv, err = b.d.CreateBufferSRV(buf, d3d.FormatR32Uint, 0, size/4); err != nil {
			buf.Release()
			return nil, err
		}
	}
	b.bufs[key] = v
	return v, nil
}

// ---- shaders ----

type shaderKey struct {
	ps     bool
	addr   uint32
	fs     uint32
	config string
}

type hostShader struct {
	prog  *dxProgram
	obj   d3d.Object
	words map[uint32]uint32 // the program words it was made from
}

// shader gives the host shader for a program under cfg, translating it if
// its code, configuration or the textures and buffers it depends on
// changed.
func (b *d3dBackend) shader(cfg *dxConfig, addr uint32) (*hostShader, error) {
	g := b.g
	key := shaderKey{ps: cfg.ps, addr: addr, fs: cfg.fsAddr, config: fmt.Sprintf("%v %v %d %#x %v %d %d %d %v %v %d %v",
		cfg.ps, cfg.vtxSem, cfg.nParams, cfg.vte, cfg.psIn, cfg.posGPR, cfg.faceGPR, cfg.faceChan, cfg.faceBits,
		cfg.alphaOn, cfg.alphaFunc, cfg.multiWrite)}
outer:
	for _, s := range b.shaders[key] {
		for a, w := range s.words {
			if g.read32(a) != w {
				continue outer
			}
		}
		for id, k := range s.prog.texDeps {
			if cfg.texInfo(id) != k {
				continue outer
			}
		}
		for id, k := range s.prog.vbufDeps {
			if cfg.vbufInfo(id) != k {
				continue outer
			}
		}
		return s, nil
	}
	words := map[uint32]uint32{}
	c := *cfg
	c.read32 = func(a uint32) uint32 {
		w := g.read32(a)
		words[a] = w
		return w
	}
	p, err := translateDX(&c, addr)
	if err != nil {
		return nil, err
	}
	var obj d3d.Object
	if cfg.ps {
		obj, err = b.d.CreatePixelShader(p.code)
	} else {
		obj, err = b.d.CreateVertexShader(p.code)
	}
	if err != nil {
		return nil, err
	}
	if dxDumpShaders {
		fmt.Printf("---- %s shader at %#x (%s)\n%s", map[bool]string{false: "vertex", true: "pixel"}[cfg.ps], addr, key.config, d3d.Disasm(p.code))
	}
	s := &hostShader{prog: p, obj: obj, words: words}
	b.shaders[key] = append(b.shaders[key], s)
	return s, nil
}

// ---- state objects ----

func (b *d3dBackend) state(kind byte, desc unsafe.Pointer, size uintptr) (d3d.Object, error) {
	k := string(kind) + string(unsafe.Slice((*byte)(desc), size))
	if o, ok := b.states[k]; ok {
		return o, nil
	}
	var o d3d.Object
	var err error
	switch kind {
	case 'b':
		o, err = b.d.CreateBlendState((*d3d.BlendDesc)(desc))
	case 'd':
		o, err = b.d.CreateDepthStencilState((*d3d.DepthStencilDesc)(desc))
	case 'r':
		o, err = b.d.CreateRasterizerState((*d3d.RasterizerDesc)(desc))
	case 's':
		o, err = b.d.CreateSamplerState((*d3d.SamplerDesc)(desc))
	}
	if err != nil {
		return o, err
	}
	b.states[k] = o
	return o, nil
}

// ---- the draw ----

func (b *d3dBackend) fallback(f string, a ...any) bool {
	b.err = fmt.Sprintf(f, a...)
	b.g.warn("d3d: %s", b.err)
	return false
}

func (b *d3dBackend) draw(st *drawState, prim uint32, ids []uint32, insts uint32) bool {
	g := b.g
	if st.rop != 0xcc && st.rop != 0 {
		return b.fallback("raster operation %#x", st.rop)
	}
	if st.clip>>22&1 != 0 {
		return b.fallback("rasterizer off")
	}
	topo, idx, ok := hostIndices(prim, ids, st.mode>>19&1 != 0)
	if !ok {
		return b.fallback("primitive type %#x", prim)
	}
	rs, cullAll := hostRaster(st, g)
	if cullAll && topo == d3d.TopologyTriangleList || len(idx) == 0 {
		return true
	}
	// the stages' configurations
	stageTex := func(stage int) func(uint32) dxTexKey {
		return func(id uint32) dxTexKey {
			t := texFromWords(g.resource(stageResBase[stage] + id))
			return texKeyFor(&t)
		}
	}
	vbufInfo := func(id uint32) dxFetchKey {
		r := g.resource(stageResBase[stageVS] + id)
		return dxFetchKey{stride: r[2] >> 8 & 0x7ff, word2: r[2]}
	}
	vcfg := &dxConfig{read32: g.read32, fsAddr: st.fs, texInfo: stageTex(stageVS), vbufInfo: vbufInfo,
		nParams: int(st.nVSParam), vte: st.vte}
	for i := uint32(0); i < 32; i++ {
		vcfg.vtxSem[i] = g.reg(regSQ_VTX_SEMANTIC_0 + i*4)
	}
	pcfg := &dxConfig{ps: true, read32: g.read32, texInfo: stageTex(stagePS), vbufInfo: vbufInfo,
		nParams: int(st.nVSParam), posGPR: st.posAddr, faceGPR: st.faceAddr, faceChan: st.faceChan,
		faceBits: st.faceAllBits, alphaOn: st.alphaOn, alphaFunc: st.alphaFunc, multiWrite: st.multiWrite}
	for _, in := range st.psInputs {
		reg := -1
		if in.param >= 0 {
			reg = in.param + 1 // after the position
		}
		pcfg.psIn = append(pcfg.psIn, dxPSIn{reg: reg, def: in.def, flat: in.flat || st.flatAll, linear: in.linear})
	}
	vs, err := b.shader(vcfg, st.vs)
	if err != nil {
		return b.fallback("vertex shader: %v", err)
	}
	var ps *hostShader
	if st.ps != 0 {
		if ps, err = b.shader(pcfg, st.ps); err != nil {
			return b.fallback("pixel shader: %v", err)
		}
	}
	// targets
	var rtvs []d3d.View
	var drawn []*hostSurf
	tw, th := uint32(1<<14), uint32(1<<14)
	for i := 0; i < 8; i++ {
		if !st.cbOn[i] {
			rtvs = append(rtvs, d3d.View{})
			continue
		}
		cb := &st.cbs[i]
		s, err := b.surface(false, &cb.lay, cb.format, cb.kind, cb.slice)
		if err != nil {
			return b.fallback("colour buffer: %v", err)
		}
		rtvs = append(rtvs, s.view)
		drawn = append(drawn, s)
		tw, th = min(tw, s.w), min(th, s.h)
	}
	for len(rtvs) > 0 && rtvs[len(rtvs)-1].Nil() {
		rtvs = rtvs[:len(rtvs)-1]
	}
	var dsv d3d.View
	if st.db.ok && st.dbCtl&7 != 0 {
		s, err := b.surface(true, &st.db.lay, st.db.format, 0, st.db.slice)
		if err != nil {
			return b.fallback("depth buffer: %v", err)
		}
		dsv = s.view
		drawn = append(drawn, s)
		tw, th = min(tw, s.w), min(th, s.h)
	}
	if len(drawn) == 0 {
		return true
	}
	c := b.c
	c.ClearState()
	// resources
	bind := func(stage int, p *dxProgram, ps bool) bool {
		var views []d3d.View
		vb := 0
		var sys [dxSysSize * 4]uint32
		for _, r := range p.srv {
			if r.vbuf {
				res := g.resource(stageResBase[stage] + r.id)
				size := res[1] + 1
				v, err := b.buffer(res[0], size, false)
				if err != nil {
					return b.fallback("vertex buffer: %v", err)
				}
				views = append(views, v.srv)
				sys[(dxSysBufSize+vb/4)*4+vb%4] = size
				vb++
				continue
			}
			t := texFromWords(g.resource(stageResBase[stage] + r.id))
			h, err := b.texture(&t)
			if err != nil {
				return b.fallback("texture: %v", err)
			}
			views = append(views, h.srv)
		}
		var samps []d3d.Object
		for _, id := range p.samplers {
			s := g.sampler(stageSamplerBase[stage] + id&^dxSampCompare)
			w0 := g.reg(regSQ_TEX_SAMPLER_WORD0_0 + (stageSamplerBase[stage]+id&^dxSampCompare)*12)
			sd := hostSampler(&s, w0>>19&7, id&dxSampCompare != 0)
			o, err := b.state('s', unsafe.Pointer(&sd), unsafe.Sizeof(sd))
			if err != nil {
				return b.fallback("sampler: %v", err)
			}
			samps = append(samps, o)
		}
		// constants: the system block, the constant file and the banks
		e := &stageEnv{g: g, stage: stage}
		sys[dxSysBool*4] = g.reg(regSQ_BOOL_CONST_0 + uint32(stage)*4)
		sys[dxSysBool*4+1] = f2u(st.alphaRef)
		for i := uint32(0); i < 32; i++ {
			sys[dxSysLoop*4+i] = e.loopConst(i)
		}
		vp, vpz := hostViewport(st, float32(tw), float32(th))
		for i := 0; i < 4; i++ {
			sys[dxSysVport*4+i] = f2u(vp[i])
		}
		sys[dxSysVportZ*4], sys[dxSysVportZ*4+1] = f2u(vpz[0]), f2u(vpz[1])
		sys[dxSysDraw*4+1] = g.reg(regSQ_VTX_BASE_VTX_LOC)
		sys[dxSysDraw*4+2] = g.reg(regSQ_VTX_START_INST_LOC)
		si := 0
		if ps {
			si = 1
		}
		raw := make([]byte, 0, 256*16)
		for _, w := range sys {
			raw = binary.LittleEndian.AppendUint32(raw, w)
		}
		b.upload(b.sysCB[si], raw)
		raw = raw[:0]
		for i := uint32(0); i < 256*4; i++ {
			raw = binary.LittleEndian.AppendUint32(raw, e.cfile(i/4, i%4))
		}
		b.upload(b.fileCB[si], raw)
		cbs := []d3d.Resource{b.sysCB[si], b.fileCB[si]}
		for _, bank := range p.banks {
			base := g.reg(kcacheRegs[stage]+bank*4) << 8
			n := g.reg(sizeRegs[stage]+bank*4) * 256
			if n == 0 || n > 64<<10 {
				n = 64 << 10
			}
			v, err := b.buffer(base, n, true)
			if err != nil {
				return b.fallback("constant bank: %v", err)
			}
			cbs = append(cbs, v.buf)
		}
		if ps {
			c.PSSetConstantBuffers(0, cbs...)
			if len(views) > 0 {
				c.PSSetShaderResources(0, views...)
			}
			if len(samps) > 0 {
				c.PSSetSamplers(0, samps...)
			}
		} else {
			c.VSSetConstantBuffers(0, cbs...)
			if len(views) > 0 {
				c.VSSetShaderResources(0, views...)
			}
			if len(samps) > 0 {
				c.VSSetSamplers(0, samps...)
			}
		}
		return true
	}
	if !bind(stageVS, vs.prog, false) {
		return false
	}
	c.VSSetShader(vs.obj)
	if ps != nil {
		if !bind(stagePS, ps.prog, true) {
			return false
		}
		c.PSSetShader(ps.obj)
	}
	// state
	bd, factor := hostBlend(st)
	bo, err := b.state('b', unsafe.Pointer(&bd), unsafe.Sizeof(bd))
	if err != nil {
		return b.fallback("blend: %v", err)
	}
	c.OMSetBlendState(bo, factor, 0xffffffff)
	dd, ref := hostDepthStencil(st)
	if dsv.Nil() {
		dd, ref = hostDepthStencil(&drawState{})
	}
	do, err := b.state('d', unsafe.Pointer(&dd), unsafe.Sizeof(dd))
	if err != nil {
		return b.fallback("depth: %v", err)
	}
	c.OMSetDepthStencilState(do, ref)
	ro, err := b.state('r', unsafe.Pointer(&rs), unsafe.Sizeof(rs))
	if err != nil {
		return b.fallback("rasterizer: %v", err)
	}
	c.RSSetState(ro)
	c.RSSetViewports(d3d.Viewport{W: float32(tw), H: float32(th), MinZ: 0, MaxZ: 1})
	sc := st.sc
	c.RSSetScissorRects(d3d.Rect{Left: sc[0], Top: sc[1], Right: min(sc[2], int32(tw)), Bottom: min(sc[3], int32(th))})
	c.OMSetRenderTargets(rtvs, dsv)
	// indices
	need := len(idx) * 4
	if need > b.ibSize {
		b.ib.Release()
		b.ibSize = max(need, 64<<10)
		if b.ib, err = b.dynBuffer(b.ibSize, d3d.BindIndexBuffer); err != nil {
			b.ibSize = 0
			return b.fallback("index buffer: %v", err)
		}
	}
	b.upload(b.ib, unsafe.Slice((*byte)(unsafe.Pointer(&idx[0])), need))
	c.IASetIndexBuffer(b.ib, d3d.FormatR32Uint, 0)
	c.IASetPrimitiveTopology(topo)
	c.DrawIndexedInstanced(uint32(len(idx)), insts, 0, 0, 0)
	// what was drawn is newer on the host; textures made from it are old
	for _, s := range drawn {
		s.dirty = true
		b.dropTextures(s.lo, s.hi)
	}
	return true
}

// dxDumpShaders prints every translated shader (for debugging).
var dxDumpShaders bool

var sizeRegs = [3]uint32{regSQ_ALU_CONST_BUFFER_SIZE_PS_0, regSQ_ALU_CONST_BUFFER_SIZE_VS_0, regSQ_ALU_CONST_BUFFER_SIZE_GS_0}
