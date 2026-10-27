//go:build windows

package ps3

// The Direct3D 11 gpuDevice.

import (
	"fmt"
	"math"
	"slices"
	"unsafe"

	"retropixels/wiiu/d3d"
)

type d3dTex struct {
	w, h    int
	format  uint32
	kind    int
	bpp     int // bytes a texel (0 for block formats)
	res     d3d.Resource
	srv     d3d.View
	rtv     d3d.View
	dsv     d3d.View
	staging d3d.Resource
}

type d3dGPU struct {
	d       *d3d.Device
	c       d3d.Context
	tex     map[gpuHandle]*d3dTex
	ps      map[gpuHandle]d3d.Object
	next    gpuHandle
	vs      d3d.Object
	depthPS d3d.Object
	states  map[string]d3d.Object
	vb      d3d.Resource
	vbSRV   d3d.View
	vbSize  int
	ib      d3d.Resource
	ibSize  int
	cb      d3d.Resource
	cbSize  int
	vcb     d3d.Resource // the vertex shaders' constants
	vcbSize int
	// the constants last put in cb and vcb (unchanged ones aren't sent again)
	lastCB, lastVCB          d3d.Resource
	lastPSConst, lastVSConst []float32
	vss                      map[gpuHandle]d3d.Object
	// the picture read a frame late (ReadTextureLate): two staging
	// textures, the one copied to last, and whether it holds a picture
	late     [2]d3d.Resource
	lateW    int
	lateH    int
	lateFmt  uint32
	lateCur  int
	lateFull bool
}

// UseD3D11 moves the machine's drawing onto the graphics card (or, when
// there's none that works, Windows' own fast software Direct3D, WARP).
func (m *Machine) UseD3D11() error {
	dev, err := newD3DGPU(false)
	if err != nil {
		var err2 error
		if dev, err2 = newD3DGPU(true); err2 != nil {
			return fmt.Errorf("%v; WARP: %v", err, err2)
		}
	}
	if err := m.UseHostGPU(dev); err != nil {
		dev.Close()
		return err
	}
	return nil
}

func newD3DGPU(warp bool) (*d3dGPU, error) {
	d, err := d3d.NewDevice(warp)
	if err != nil {
		return nil, err
	}
	g := &d3dGPU{d: d, c: d.Ctx, tex: map[gpuHandle]*d3dTex{}, ps: map[gpuHandle]d3d.Object{}, vss: map[gpuHandle]d3d.Object{}, states: map[string]d3d.Object{}}
	if g.vs, err = d.CreateVertexShader(gpuVSCode()); err != nil {
		d.Close()
		return nil, err
	}
	if g.depthPS, err = d.CreatePixelShader(depthToRGBAPS()); err != nil {
		d.Close()
		return nil, err
	}
	return g, nil
}

// depthToRGBAPS reads texel (x, y) of a 24-bit depth texture and writes
// its bytes as DepthToRGBA8 describes.
func depthToRGBAPS() []byte {
	s := &d3d.Shader{Pixel: true}
	s.In = []d3d.Sig{{Name: "SV_Position", SV: d3d.SVPosition, Mask: 15}}
	s.Out = []d3d.Sig{{Name: "SV_Target", SV: d3d.SVTarget, Mask: 15}}
	s.DclResource(0, d3d.DimTex2D, d3d.RetFloat)
	s.DclInputPSSV(0, 15, d3d.InterpNoPerspective, d3d.SVPosition)
	s.DclOutput(0, 15)
	s.Ins(d3d.OpFtou, d3d.R(0).Mask(3), d3d.In(0))
	s.Ins(d3d.OpMov, d3d.R(0).Mask(12), d3d.ImmU(0, 0, 0, 0))
	s.Ins(d3d.OpLd, d3d.R(1).Mask(15), d3d.R(0), d3d.Tex(0))
	s.Ins(d3d.OpMul, d3d.R(1).Mask(1), d3d.R(1).C(0), d3d.ImmF(16777215))
	s.Ins(d3d.OpRoundNe, d3d.R(1).Mask(1), d3d.R(1).C(0))
	s.Ins(d3d.OpFtou, d3d.R(1).Mask(1), d3d.R(1).C(0))
	s.Ins(d3d.OpUbfe, d3d.R(2).Mask(15), d3d.ImmU(8, 8, 0, 8), d3d.ImmU(8, 0, 0, 16), d3d.R(1).C(0))
	s.Ins(d3d.OpUtof, d3d.R(2).Mask(15), d3d.R(2))
	s.Ins(d3d.OpMul, d3d.Out(0).Mask(15), d3d.R(2), d3d.ImmF(1.0/255, 1.0/255, 1.0/255, 1.0/255))
	s.Ins(d3d.OpRet)
	s.Temps(3)
	return s.Bytes()
}

func (g *d3dGPU) handle() gpuHandle { g.next++; return g.next }

func texBytes(format uint32) int {
	switch format {
	case d3d.FormatRGBA8Unorm, d3d.FormatR32Float, d3d.FormatR24G8Typeless:
		return 4
	case d3d.FormatRGBA16Float:
		return 8
	case d3d.FormatRGBA32Float:
		return 16
	case d3d.FormatR16Typeless:
		return 2
	}
	return 0
}

func (g *d3dGPU) CreateTexture(w, h int, format uint32, kind int, data []byte, pitch int) (gpuHandle, error) {
	t := &d3dTex{w: w, h: h, format: format, kind: kind, bpp: texBytes(format)}
	desc := d3d.Texture2DDesc{Width: uint32(w), Height: uint32(h), MipLevels: 1, ArraySize: 1, Format: format,
		SampleCount: 1, BindFlags: d3d.BindShaderResource}
	srvFmt := format
	var err error
	switch kind {
	case gpuTexTarget:
		desc.BindFlags |= d3d.BindRenderTarget
	case gpuTexDepth:
		desc.BindFlags |= d3d.BindDepthStencil
		srvFmt = d3d.FormatR24UnormX8
		if format == d3d.FormatR16Typeless {
			srvFmt = d3d.FormatR16Unorm
		}
	}
	init := data
	if kind != gpuTexPlain {
		init = nil
	}
	if t.res, err = g.d.CreateTexture2D(&desc, init, uint32(pitch)); err != nil {
		return 0, err
	}
	if t.srv, err = g.d.CreateSRV(t.res, srvFmt, 1); err != nil {
		t.release()
		return 0, err
	}
	switch kind {
	case gpuTexTarget:
		t.rtv, err = g.d.CreateRTV(t.res, format)
	case gpuTexDepth:
		dsvFmt := uint32(d3d.FormatD24UnormS8Uint)
		if format == d3d.FormatR16Typeless {
			dsvFmt = d3d.FormatD16Unorm
		}
		t.dsv, err = g.d.CreateDSV(t.res, dsvFmt)
	}
	if err == nil && kind != gpuTexPlain {
		t.staging, err = g.d.CreateTexture2D(&d3d.Texture2DDesc{Width: uint32(w), Height: uint32(h), MipLevels: 1,
			ArraySize: 1, Format: format, SampleCount: 1, Usage: d3d.UsageStaging, CPUAccess: d3d.CPURead | d3d.CPUWrite}, nil, 0)
	}
	if err != nil {
		t.release()
		return 0, err
	}
	h2 := g.handle()
	g.tex[h2] = t
	if data != nil && kind != gpuTexPlain {
		g.UpdateTexture(h2, data, pitch)
	}
	return h2, nil
}

func (t *d3dTex) release() {
	t.srv.Release()
	t.rtv.Release()
	t.dsv.Release()
	t.staging.Release()
	t.res.Release()
}

func (g *d3dGPU) UpdateTexture(h gpuHandle, data []byte, pitch int) {
	t := g.tex[h]
	if t == nil || len(data) == 0 {
		return
	}
	if t.kind == gpuTexPlain {
		g.c.UpdateSubresource(t.res, 0, data, uint32(pitch), 0)
		return
	}
	m, err := g.mapRes(t.staging, 0, d3d.MapWrite)
	if err != nil {
		return
	}
	n := t.w * t.bpp
	dst := m.Bytes(int(m.RowPitch) * t.h)
	for y := 0; y < t.h; y++ {
		copy(dst[y*int(m.RowPitch):y*int(m.RowPitch)+n], data[y*pitch:])
	}
	g.c.Unmap(t.staging, 0)
	g.c.CopyResource(t.res, t.staging)
}

func (g *d3dGPU) ReadTexture(h gpuHandle) ([]byte, int, error) {
	t := g.tex[h]
	if t == nil || t.kind == gpuTexPlain {
		return nil, 0, fmt.Errorf("not a target")
	}
	g.c.CopyResource(t.staging, t.res)
	m, err := g.mapRes(t.staging, 0, d3d.MapRead)
	if err != nil {
		return nil, 0, err
	}
	n := t.w * t.bpp
	out := make([]byte, n*t.h)
	src := m.Bytes(int(m.RowPitch) * t.h)
	for y := 0; y < t.h; y++ {
		copy(out[y*n:(y+1)*n], src[y*int(m.RowPitch):])
	}
	g.c.Unmap(t.staging, 0)
	return out, n, nil
}

// ReadTextureLate starts copying texture h and returns the copy started
// at the call before (so the CPU doesn't wait for this frame's drawing);
// the first time, h itself.
func (g *d3dGPU) ReadTextureLate(h gpuHandle) ([]byte, int, error) {
	t := g.tex[h]
	if t == nil || t.kind == gpuTexPlain || t.bpp == 0 {
		return nil, 0, fmt.Errorf("not a target")
	}
	if g.late[0].Nil() || g.lateW != t.w || g.lateH != t.h || g.lateFmt != t.format {
		for i := range g.late {
			g.late[i].Release()
			var err error
			g.late[i], err = g.d.CreateTexture2D(&d3d.Texture2DDesc{Width: uint32(t.w), Height: uint32(t.h), MipLevels: 1,
				ArraySize: 1, Format: t.format, SampleCount: 1, Usage: d3d.UsageStaging, CPUAccess: d3d.CPURead}, nil, 0)
			if err != nil {
				g.late[0].Release()
				g.late[1].Release()
				return g.ReadTexture(h)
			}
		}
		g.lateW, g.lateH, g.lateFmt, g.lateFull = t.w, t.h, t.format, false
	}
	prev := g.lateCur
	g.lateCur ^= 1
	g.c.CopyResource(g.late[g.lateCur], t.res)
	if !g.lateFull {
		g.lateFull = true
		prev = g.lateCur // nothing older: this frame's
	}
	m, err := g.mapRes(g.late[prev], 0, d3d.MapRead)
	if err != nil {
		return nil, 0, err
	}
	n := t.w * t.bpp
	out := make([]byte, n*t.h)
	src := m.Bytes(int(m.RowPitch) * t.h)
	for y := 0; y < t.h; y++ {
		copy(out[y*n:(y+1)*n], src[y*int(m.RowPitch):])
	}
	g.c.Unmap(g.late[prev], 0)
	return out, n, nil
}

func (g *d3dGPU) CopyTexture(dst, src gpuHandle) {
	d, s := g.tex[dst], g.tex[src]
	if d != nil && s != nil {
		g.unbindTextures()
		g.c.CopyResource(d.res, s.res)
	}
}

func (g *d3dGPU) Release(h gpuHandle) {
	if t := g.tex[h]; t != nil {
		t.release()
		delete(g.tex, h)
	}
	if p, ok := g.ps[h]; ok {
		p.Release()
		delete(g.ps, h)
	}
}

func (g *d3dGPU) CreatePS(code []byte) (gpuHandle, error) {
	o, err := g.d.CreatePixelShader(code)
	if err != nil {
		return 0, err
	}
	h := g.handle()
	g.ps[h] = o
	return h, nil
}

func (g *d3dGPU) CreateVS(code []byte) (gpuHandle, error) {
	o, err := g.d.CreateVertexShader(code)
	if err != nil {
		return 0, err
	}
	h := g.handle()
	g.vss[h] = o
	return h, nil
}

func (g *d3dGPU) ClearTarget(h gpuHandle, rgba [4]float32) {
	if t := g.tex[h]; t != nil && !t.rtv.Nil() {
		g.c.ClearRenderTargetView(t.rtv, rgba)
	}
}

func (g *d3dGPU) ClearDepth(h gpuHandle, flags uint32, depth float32, stencil uint8) {
	if t := g.tex[h]; t != nil && !t.dsv.Nil() {
		g.c.ClearDepthStencilView(t.dsv, flags, depth, stencil)
	}
}

func (g *d3dGPU) Close() {
	for h := range g.tex {
		g.Release(h)
	}
	for h := range g.ps {
		g.Release(h)
	}
	for _, o := range g.states {
		o.Release()
	}
	g.vs.Release()
	g.depthPS.Release()
	g.vbSRV.Release()
	g.vb.Release()
	g.ib.Release()
	g.cb.Release()
	g.d.Close()
}

func (g *d3dGPU) state(kind byte, desc unsafe.Pointer, size uintptr) (d3d.Object, error) {
	k := string(kind) + string(unsafe.Slice((*byte)(desc), size))
	if o, ok := g.states[k]; ok {
		return o, nil
	}
	var o d3d.Object
	var err error
	switch kind {
	case 'b':
		o, err = g.d.CreateBlendState((*d3d.BlendDesc)(desc))
	case 'd':
		o, err = g.d.CreateDepthStencilState((*d3d.DepthStencilDesc)(desc))
	case 'r':
		o, err = g.d.CreateRasterizerState((*d3d.RasterizerDesc)(desc))
	case 's':
		o, err = g.d.CreateSamplerState((*d3d.SamplerDesc)(desc))
	}
	if err != nil {
		return o, err
	}
	g.states[k] = o
	return o, nil
}

// dynamic makes sure buf holds at least n bytes.
func (g *d3dGPU) dynamic(buf *d3d.Resource, size *int, n int, bind uint32) error {
	if n <= *size && !buf.Nil() {
		return nil
	}
	buf.Release()
	// a new buffer may reuse the old one's address: forget what was in it
	if buf == &g.cb {
		g.lastCB = d3d.Resource{}
	}
	if buf == &g.vcb {
		g.lastVCB = d3d.Resource{}
	}
	sz := max(n, 1<<16)
	if bind == d3d.BindConstantBuffer {
		sz = (n + 15) &^ 15
		if sz > 65536 {
			return fmt.Errorf("constants too big (%d bytes)", n)
		}
	} else {
		sz = 1 << int(math.Ceil(math.Log2(float64(sz))))
	}
	var err error
	*buf, err = g.d.CreateBuffer(&d3d.BufferDesc{ByteWidth: uint32(sz), Usage: d3d.UsageDynamic, BindFlags: bind, CPUAccess: d3d.CPUWrite}, nil)
	if err != nil {
		*size = 0
		return err
	}
	*size = sz
	if bind == d3d.BindShaderResource {
		g.vbSRV.Release()
		g.vbSRV, err = g.d.CreateBufferSRV(*buf, d3d.FormatRGBA32Float, 0, uint32(sz/16))
		if err != nil {
			*size = 0
			buf.Release()
			return err
		}
	}
	return nil
}

// mapRes maps a resource, refusing a mapping with no memory (which a
// driver can give back and still say it worked).
func (g *d3dGPU) mapRes(r d3d.Resource, sub, typ uint32) (d3d.Mapped, error) {
	m, err := g.c.Map(r, sub, typ)
	if err != nil {
		return m, err
	}
	if m.Data == 0 {
		g.c.Unmap(r, sub)
		return m, fmt.Errorf("the graphics card gave no memory to map")
	}
	return m, nil
}

func (g *d3dGPU) put(buf d3d.Resource, p unsafe.Pointer, n int) error {
	m, err := g.mapRes(buf, 0, d3d.MapWriteDiscard)
	if err != nil {
		return err
	}
	copy(m.Bytes(n), unsafe.Slice((*byte)(p), n))
	g.c.Unmap(buf, 0)
	return nil
}

func (g *d3dGPU) unbindTextures() {
	var none [16]d3d.View
	g.c.PSSetShaderResources(0, none[:]...)
}

func (g *d3dGPU) Draw(dr *gpuDraw) error {
	ps := g.ps[dr.PS]
	if ps.Nil() {
		return fmt.Errorf("no pixel shader")
	}
	if len(dr.Index) == 0 || len(dr.Verts) == 0 {
		return nil
	}
	if err := g.dynamic(&g.vb, &g.vbSize, len(dr.Verts)*4, d3d.BindShaderResource); err != nil {
		return err
	}
	if err := g.dynamic(&g.ib, &g.ibSize, len(dr.Index)*4, d3d.BindIndexBuffer); err != nil {
		return err
	}
	if err := g.put(g.vb, unsafe.Pointer(&dr.Verts[0]), len(dr.Verts)*4); err != nil {
		return err
	}
	if err := g.put(g.ib, unsafe.Pointer(&dr.Index[0]), len(dr.Index)*4); err != nil {
		return err
	}
	if len(dr.PSConst) > 0 {
		if err := g.dynamic(&g.cb, &g.cbSize, len(dr.PSConst)*4, d3d.BindConstantBuffer); err != nil {
			return err
		}
		// the buffer may be bigger than the constants: fill the rest with 0
		if g.cb != g.lastCB || !slices.Equal(g.lastPSConst, dr.PSConst) {
			buf := make([]float32, g.cbSize/4)
			copy(buf, dr.PSConst)
			if err := g.put(g.cb, unsafe.Pointer(&buf[0]), g.cbSize); err != nil {
				return err
			}
			g.lastCB, g.lastPSConst = g.cb, append(g.lastPSConst[:0], dr.PSConst...)
		}
	}
	vs := g.vs
	if dr.VS != 0 {
		vs = g.vss[dr.VS]
		if vs.Nil() {
			return fmt.Errorf("no vertex shader")
		}
		if err := g.dynamic(&g.vcb, &g.vcbSize, len(dr.VSConst)*4, d3d.BindConstantBuffer); err != nil {
			return err
		}
		if g.vcb != g.lastVCB || !slices.Equal(g.lastVSConst, dr.VSConst) {
			buf := make([]float32, g.vcbSize/4)
			copy(buf, dr.VSConst)
			if err := g.put(g.vcb, unsafe.Pointer(&buf[0]), g.vcbSize); err != nil {
				return err
			}
			g.lastVCB, g.lastVSConst = g.vcb, append(g.lastVSConst[:0], dr.VSConst...)
		}
	}
	// state objects
	bd := d3d.BlendDesc{}
	bd.Target[0] = dr.Blend
	for k := range dr.MRT {
		if dr.MRT[k] != 0 {
			bd.Independent = 1
			bd.Target[1+k] = dr.MRTBlend[k]
		}
	}
	bo, err := g.state('b', unsafe.Pointer(&bd), unsafe.Sizeof(bd))
	if err != nil {
		return err
	}
	dd := dr.DepthState
	if dr.Depth == 0 {
		dd.DepthEnable, dd.DepthWriteMask = 0, 0
	}
	do, err := g.state('d', unsafe.Pointer(&dd), unsafe.Sizeof(dd))
	if err != nil {
		return err
	}
	rd := dr.Raster
	rd.DepthClip = 0
	ro, err := g.state('r', unsafe.Pointer(&rd), unsafe.Sizeof(rd))
	if err != nil {
		return err
	}
	var views [16]d3d.View
	var samps [16]d3d.Object
	for u := 0; u < 16; u++ {
		if t := g.tex[dr.Tex[u]]; t != nil {
			views[u] = t.srv
		}
		sd := dr.Samp[u]
		if sd.MaxAnisotropy == 0 {
			sd.MaxAnisotropy = 1
		}
		if sd.Compare == 0 {
			sd.Compare = d3d.CmpNever
		}
		if sd.AddressU == 0 {
			sd.AddressU, sd.AddressV, sd.W = d3d.AddressClamp, d3d.AddressClamp, d3d.AddressClamp
		}
		if sd.W == 0 {
			sd.W = d3d.AddressClamp
		}
		if samps[u], err = g.state('s', unsafe.Pointer(&sd), unsafe.Sizeof(sd)); err != nil {
			return err
		}
	}
	// targets (textures off first, in case one is about to be a target)
	g.unbindTextures()
	var rtvs []d3d.View
	if t := g.tex[dr.Target]; t != nil {
		rtvs = []d3d.View{t.rtv}
		for k := range dr.MRT {
			if mt := g.tex[dr.MRT[k]]; mt != nil && dr.MRT[k] != 0 {
				rtvs = append(rtvs, mt.rtv)
			} else {
				break
			}
		}
	}
	var dsv d3d.View
	if t := g.tex[dr.Depth]; t != nil {
		dsv = t.dsv
	}
	g.c.OMSetRenderTargets(rtvs, dsv)
	g.c.OMSetBlendState(bo, dr.BlendFactor, 0xffffffff)
	g.c.OMSetDepthStencilState(do, dr.StencilRef)
	g.c.RSSetState(ro)
	g.c.RSSetViewports(d3d.Viewport{W: float32(dr.W), H: float32(dr.H), MinZ: 0, MaxZ: 1})
	g.c.RSSetScissorRects(dr.Scissor)
	g.c.IASetInputLayout(d3d.Object{})
	g.c.IASetPrimitiveTopology(d3d.TopologyTriangleList)
	g.c.IASetIndexBuffer(g.ib, d3d.FormatR32Uint, 0)
	g.c.VSSetShader(vs)
	if dr.VS != 0 {
		g.c.VSSetConstantBuffers(0, g.vcb)
	}
	g.c.VSSetShaderResources(0, g.vbSRV)
	g.c.GSSetShader(d3d.Object{})
	g.c.PSSetShader(ps)
	if len(dr.PSConst) > 0 {
		g.c.PSSetConstantBuffers(0, g.cb)
	}
	g.c.PSSetShaderResources(0, views[:]...)
	g.c.PSSetSamplers(0, samps[:]...)
	g.c.DrawIndexed(uint32(len(dr.Index)), 0, 0)
	return nil
}

func (g *d3dGPU) DepthToRGBA8(dst, src gpuHandle) error {
	d, s := g.tex[dst], g.tex[src]
	if d == nil || s == nil || d.rtv.Nil() {
		return fmt.Errorf("bad textures")
	}
	// one triangle over the whole target
	verts := make([]float32, 3*gpuVertRegs*4)
	for i, p := range [3][2]float32{{-1, 1}, {3, 1}, {-1, -3}} {
		copy(verts[i*gpuVertRegs*4:], []float32{p[0], p[1], 0, 1})
	}
	if err := g.dynamic(&g.vb, &g.vbSize, len(verts)*4, d3d.BindShaderResource); err != nil {
		return err
	}
	if err := g.dynamic(&g.ib, &g.ibSize, 12, d3d.BindIndexBuffer); err != nil {
		return err
	}
	idx := []uint32{0, 1, 2}
	if err := g.put(g.vb, unsafe.Pointer(&verts[0]), len(verts)*4); err != nil {
		return err
	}
	if err := g.put(g.ib, unsafe.Pointer(&idx[0]), 12); err != nil {
		return err
	}
	bd := d3d.BlendDesc{}
	bd.Target[0] = d3d.BlendTarget{Src: d3d.BlendOne, Dst: d3d.BlendZero, Op: d3d.BlendOpAdd,
		SrcAlpha: d3d.BlendOne, DstAlpha: d3d.BlendZero, OpAlpha: d3d.BlendOpAdd, WriteMask: 15}
	bo, err := g.state('b', unsafe.Pointer(&bd), unsafe.Sizeof(bd))
	if err != nil {
		return err
	}
	dd := d3d.DepthStencilDesc{DepthFunc: d3d.CmpAlways,
		Front: d3d.StencilOps{Fail: d3d.StencilKeep, DepthFail: d3d.StencilKeep, Pass: d3d.StencilKeep, Func: d3d.CmpAlways}}
	dd.Back = dd.Front
	do, err := g.state('d', unsafe.Pointer(&dd), unsafe.Sizeof(dd))
	if err != nil {
		return err
	}
	rd := d3d.RasterizerDesc{Fill: d3d.FillSolid, Cull: d3d.CullNone}
	ro, err := g.state('r', unsafe.Pointer(&rd), unsafe.Sizeof(rd))
	if err != nil {
		return err
	}
	g.unbindTextures()
	g.c.OMSetRenderTargets([]d3d.View{d.rtv}, d3d.View{})
	g.c.OMSetBlendState(bo, [4]float32{}, 0xffffffff)
	g.c.OMSetDepthStencilState(do, 0)
	g.c.RSSetState(ro)
	g.c.RSSetViewports(d3d.Viewport{W: float32(d.w), H: float32(d.h), MinZ: 0, MaxZ: 1})
	g.c.IASetInputLayout(d3d.Object{})
	g.c.IASetPrimitiveTopology(d3d.TopologyTriangleList)
	g.c.IASetIndexBuffer(g.ib, d3d.FormatR32Uint, 0)
	g.c.VSSetShader(g.vs)
	g.c.VSSetShaderResources(0, g.vbSRV)
	g.c.GSSetShader(d3d.Object{})
	g.c.PSSetShader(g.depthPS)
	g.c.PSSetShaderResources(0, s.srv)
	g.c.DrawIndexed(3, 0, 0)
	g.unbindTextures()
	return nil
}
