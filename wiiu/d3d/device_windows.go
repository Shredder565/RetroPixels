//go:build windows

package d3d

import (
	"errors"
	"unsafe"
)

// DXGI formats
const (
	FormatUnknown          = 0
	FormatRGBA32Float      = 2
	FormatRGBA32Uint       = 3
	FormatRGBA32Sint       = 4
	FormatRGBA16Float      = 10
	FormatRGBA16Unorm      = 11
	FormatRG32Float        = 16
	FormatR32G8X24Typeless = 19
	FormatD32FloatS8X24    = 20
	FormatRGB10A2Unorm     = 24
	FormatRGBA8Unorm       = 28
	FormatRGBA8UnormSRGB   = 29
	FormatRGBA8Uint        = 30
	FormatRG16Float        = 34
	FormatR32Typeless      = 39
	FormatD32Float         = 40
	FormatR32Float         = 41
	FormatR32Uint          = 42
	FormatR24G8Typeless    = 44
	FormatD24UnormS8Uint   = 45
	FormatR24UnormX8       = 46
	FormatR16Float         = 54
	FormatR16Typeless      = 53
	FormatD16Unorm         = 55
	FormatR16Unorm         = 56
	FormatR8Unorm          = 61
	FormatR32FloatX8X24    = 21
)

// usage, bind and CPU access flags
const (
	UsageDefault = 0
	UsageDynamic = 2
	UsageStaging = 3

	BindVertexBuffer   = 0x1
	BindIndexBuffer    = 0x2
	BindConstantBuffer = 0x4
	BindShaderResource = 0x8
	BindRenderTarget   = 0x20
	BindDepthStencil   = 0x40

	CPURead  = 0x20000
	CPUWrite = 0x10000

	MiscBufferAllowRaw = 0x20

	MapRead         = 1
	MapWriteDiscard = 4

	ClearDepth   = 1
	ClearStencil = 2
)

// primitive topologies
const (
	TopologyPointList     = 1
	TopologyLineList      = 2
	TopologyLineStrip     = 3
	TopologyTriangleList  = 4
	TopologyTriangleStrip = 5
)

type Texture2DDesc struct {
	Width, Height, MipLevels, ArraySize uint32
	Format                              uint32
	SampleCount, SampleQuality          uint32
	Usage, BindFlags, CPUAccess, Misc   uint32
}

type BufferDesc struct {
	ByteWidth, Usage, BindFlags, CPUAccess, Misc, StructureStride uint32
}

type subresourceData struct {
	sys        uintptr
	pitch      uint32
	slicePitch uint32
}

type Mapped struct {
	Data       uintptr
	RowPitch   uint32
	DepthPitch uint32
}

type Viewport struct{ X, Y, W, H, MinZ, MaxZ float32 }

type Rect struct{ Left, Top, Right, Bottom int32 }

// Device is an ID3D11Device with its immediate context.
type Device struct {
	Unknown
	Ctx   Context
	Level uint32
}

// Context is an ID3D11DeviceContext.
type Context struct{ Unknown }

// Resource is any ID3D11Resource (a texture or a buffer).
type Resource struct{ Unknown }

// View is a render target, depth stencil or shader resource view.
type View struct{ Unknown }

// Object is a shader or a state object.
type Object struct{ Unknown }

// NewDevice makes a Direct3D 11 device on the graphics card, or on WARP
// (Windows' software renderer) when warp is set.
func NewDevice(warp bool) (*Device, error) {
	if err := procCreateDevice.Find(); err != nil {
		return nil, errors.New("Direct3D 11 isn't available")
	}
	driver := uintptr(1) // hardware
	if warp {
		driver = 5
	}
	levels := []uint32{0xb000} // 11_0
	d := &Device{}
	r, _, _ := procCreateDevice.Call(0, driver, 0, 0, uintptr(unsafe.Pointer(&levels[0])), 1, 7,
		uintptr(unsafe.Pointer(&d.p)), uintptr(unsafe.Pointer(&d.Level)), uintptr(unsafe.Pointer(&d.Ctx.p)))
	if err := hr(r, "D3D11CreateDevice"); err != nil {
		return nil, err
	}
	return d, nil
}

func (d *Device) Close() {
	d.Ctx.Release()
	d.Release()
}

// CreateTexture2D makes a texture, with data for its first level (or nil).
func (d *Device) CreateTexture2D(desc *Texture2DDesc, data []byte, pitch uint32) (Resource, error) {
	var t Resource
	var init uintptr
	var sd subresourceData
	if data != nil {
		sd = subresourceData{uintptr(unsafe.Pointer(&data[0])), pitch, pitch * desc.Height}
		init = uintptr(unsafe.Pointer(&sd))
	}
	r := d.call(5, uintptr(unsafe.Pointer(desc)), init, uintptr(unsafe.Pointer(&t.p)))
	return t, hr(r, "CreateTexture2D")
}

func (d *Device) CreateBuffer(desc *BufferDesc, data []byte) (Resource, error) {
	var b Resource
	var init uintptr
	var sd subresourceData
	if data != nil {
		sd = subresourceData{uintptr(unsafe.Pointer(&data[0])), 0, 0}
		init = uintptr(unsafe.Pointer(&sd))
	}
	r := d.call(3, uintptr(unsafe.Pointer(desc)), init, uintptr(unsafe.Pointer(&b.p)))
	return b, hr(r, "CreateBuffer")
}

// view descriptions
type srvTex2D struct {
	Format, Dim       uint32
	MostDetailed, Mip uint32
	_                 [2]uint32
}
type srvRaw struct {
	Format, Dim               uint32
	FirstElement, NumElements uint32
	Flags                     uint32
	_                         uint32
}
type rtvTex2D struct {
	Format, Dim, Mip uint32
	_                [2]uint32
}
type dsvTex2D struct {
	Format, Dim, Flags, Mip uint32
	_                       [2]uint32
}

// CreateSRV makes a shader resource view of a texture (format 0: the
// texture's).
func (d *Device) CreateSRV(t Resource, format, mips uint32) (View, error) {
	var v View
	desc := srvTex2D{Format: format, Dim: 4, Mip: mips} // TEXTURE2D
	r := d.call(7, t.p, uintptr(unsafe.Pointer(&desc)), uintptr(unsafe.Pointer(&v.p)))
	return v, hr(r, "CreateShaderResourceView")
}

// CreateBufferSRV makes a typed view of n elements of a buffer.
func (d *Device) CreateBufferSRV(b Resource, format, first, n uint32) (View, error) {
	var v View
	desc := srvRaw{Format: format, Dim: 1, FirstElement: first, NumElements: n} // BUFFER
	r := d.call(7, b.p, uintptr(unsafe.Pointer(&desc)), uintptr(unsafe.Pointer(&v.p)))
	return v, hr(r, "CreateShaderResourceView (buffer)")
}

func (d *Device) CreateRTV(t Resource, format uint32) (View, error) {
	var v View
	desc := rtvTex2D{Format: format, Dim: 4}
	r := d.call(9, t.p, uintptr(unsafe.Pointer(&desc)), uintptr(unsafe.Pointer(&v.p)))
	return v, hr(r, "CreateRenderTargetView")
}

func (d *Device) CreateDSV(t Resource, format uint32) (View, error) {
	var v View
	desc := dsvTex2D{Format: format, Dim: 3}
	r := d.call(10, t.p, uintptr(unsafe.Pointer(&desc)), uintptr(unsafe.Pointer(&v.p)))
	return v, hr(r, "CreateDepthStencilView")
}

func (d *Device) CreateVertexShader(code []byte) (Object, error) {
	var o Object
	r := d.call(12, uintptr(unsafe.Pointer(&code[0])), uintptr(len(code)), 0, uintptr(unsafe.Pointer(&o.p)))
	return o, hr(r, "CreateVertexShader")
}

func (d *Device) CreatePixelShader(code []byte) (Object, error) {
	var o Object
	r := d.call(15, uintptr(unsafe.Pointer(&code[0])), uintptr(len(code)), 0, uintptr(unsafe.Pointer(&o.p)))
	return o, hr(r, "CreatePixelShader")
}

// BlendTarget is D3D11_RENDER_TARGET_BLEND_DESC.
type BlendTarget struct {
	Enable                      int32
	Src, Dst, Op                uint32
	SrcAlpha, DstAlpha, OpAlpha uint32
	WriteMask                   uint8
	_                           [3]byte
}

type BlendDesc struct {
	AlphaToCoverage, Independent int32
	Target                       [8]BlendTarget
}

func (d *Device) CreateBlendState(desc *BlendDesc) (Object, error) {
	var o Object
	r := d.call(20, uintptr(unsafe.Pointer(desc)), uintptr(unsafe.Pointer(&o.p)))
	return o, hr(r, "CreateBlendState")
}

type StencilOps struct{ Fail, DepthFail, Pass, Func uint32 }

type DepthStencilDesc struct {
	DepthEnable    int32
	DepthWriteMask uint32
	DepthFunc      uint32
	StencilEnable  int32
	ReadMask       uint8
	WriteMask      uint8
	_              [2]byte
	Front, Back    StencilOps
}

func (d *Device) CreateDepthStencilState(desc *DepthStencilDesc) (Object, error) {
	var o Object
	r := d.call(21, uintptr(unsafe.Pointer(desc)), uintptr(unsafe.Pointer(&o.p)))
	return o, hr(r, "CreateDepthStencilState")
}

type RasterizerDesc struct {
	Fill, Cull            uint32
	FrontCCW              int32
	DepthBias             int32
	DepthBiasClamp        float32
	SlopeScaledDepthBias  float32
	DepthClip             int32
	Scissor, MSAA, AALine int32
}

func (d *Device) CreateRasterizerState(desc *RasterizerDesc) (Object, error) {
	var o Object
	r := d.call(22, uintptr(unsafe.Pointer(desc)), uintptr(unsafe.Pointer(&o.p)))
	return o, hr(r, "CreateRasterizerState")
}

type SamplerDesc struct {
	Filter                uint32
	AddressU, AddressV, W uint32
	MipLODBias            float32
	MaxAnisotropy         uint32
	Compare               uint32
	Border                [4]float32
	MinLOD, MaxLOD        float32
}

func (d *Device) CreateSamplerState(desc *SamplerDesc) (Object, error) {
	var o Object
	r := d.call(23, uintptr(unsafe.Pointer(desc)), uintptr(unsafe.Pointer(&o.p)))
	return o, hr(r, "CreateSamplerState")
}

// ---- the context ----

func (c Context) VSSetConstantBuffers(slot uint32, bufs ...Resource) {
	c.call(7, uintptr(slot), uintptr(len(bufs)), uintptr(unsafe.Pointer(&bufs[0])))
}
func (c Context) PSSetShaderResources(slot uint32, views ...View) {
	c.call(8, uintptr(slot), uintptr(len(views)), uintptr(unsafe.Pointer(&views[0])))
}
func (c Context) PSSetShader(s Object) { c.call(9, s.p, 0, 0) }
func (c Context) PSSetSamplers(slot uint32, s ...Object) {
	c.call(10, uintptr(slot), uintptr(len(s)), uintptr(unsafe.Pointer(&s[0])))
}
func (c Context) VSSetShader(s Object) { c.call(11, s.p, 0, 0) }
func (c Context) DrawIndexed(n, first uint32, base int32) {
	c.call(12, uintptr(n), uintptr(first), uintptr(base))
}
func (c Context) Draw(n, first uint32) { c.call(13, uintptr(n), uintptr(first)) }
func (c Context) Map(r Resource, sub, typ uint32) (Mapped, error) {
	var m Mapped
	e := c.call(14, r.p, uintptr(sub), uintptr(typ), 0, uintptr(unsafe.Pointer(&m)))
	return m, hr(e, "Map")
}
func (c Context) Unmap(r Resource, sub uint32) { c.call(15, r.p, uintptr(sub)) }
func (c Context) PSSetConstantBuffers(slot uint32, bufs ...Resource) {
	c.call(16, uintptr(slot), uintptr(len(bufs)), uintptr(unsafe.Pointer(&bufs[0])))
}
func (c Context) IASetInputLayout(l Object) { c.call(17, l.p) }
func (c Context) IASetIndexBuffer(b Resource, format, offset uint32) {
	c.call(19, b.p, uintptr(format), uintptr(offset))
}
func (c Context) DrawIndexedInstanced(n, inst, first uint32, base int32, firstInst uint32) {
	c.call(20, uintptr(n), uintptr(inst), uintptr(first), uintptr(base), uintptr(firstInst))
}
func (c Context) DrawInstanced(n, inst, first, firstInst uint32) {
	c.call(21, uintptr(n), uintptr(inst), uintptr(first), uintptr(firstInst))
}
func (c Context) IASetPrimitiveTopology(t uint32) { c.call(24, uintptr(t)) }
func (c Context) VSSetShaderResources(slot uint32, views ...View) {
	c.call(25, uintptr(slot), uintptr(len(views)), uintptr(unsafe.Pointer(&views[0])))
}
func (c Context) VSSetSamplers(slot uint32, s ...Object) {
	c.call(26, uintptr(slot), uintptr(len(s)), uintptr(unsafe.Pointer(&s[0])))
}
func (c Context) OMSetRenderTargets(rtvs []View, dsv View) {
	var p uintptr
	if len(rtvs) > 0 {
		p = uintptr(unsafe.Pointer(&rtvs[0]))
	}
	c.call(33, uintptr(len(rtvs)), p, dsv.p)
}
func (c Context) OMSetBlendState(s Object, factor [4]float32, mask uint32) {
	c.call(35, s.p, uintptr(unsafe.Pointer(&factor[0])), uintptr(mask))
}
func (c Context) OMSetDepthStencilState(s Object, ref uint32) { c.call(36, s.p, uintptr(ref)) }
func (c Context) RSSetState(s Object)                         { c.call(43, s.p) }
func (c Context) RSSetViewports(v ...Viewport) {
	c.call(44, uintptr(len(v)), uintptr(unsafe.Pointer(&v[0])))
}
func (c Context) RSSetScissorRects(r ...Rect) {
	c.call(45, uintptr(len(r)), uintptr(unsafe.Pointer(&r[0])))
}

// Box is a D3D11_BOX.
type Box struct{ Left, Top, Front, Right, Bottom, Back uint32 }

func (c Context) CopySubresourceRegion(dst Resource, dsub, x, y, z uint32, src Resource, ssub uint32, box *Box) {
	c.call(46, dst.p, uintptr(dsub), uintptr(x), uintptr(y), uintptr(z), src.p, uintptr(ssub), uintptr(unsafe.Pointer(box)))
}
func (c Context) CopyResource(dst, src Resource) { c.call(47, dst.p, src.p) }
func (c Context) UpdateSubresource(r Resource, sub uint32, data []byte, pitch, slicePitch uint32) {
	c.call(48, r.p, uintptr(sub), 0, uintptr(unsafe.Pointer(&data[0])), uintptr(pitch), uintptr(slicePitch))
}
func (c Context) ClearRenderTargetView(v View, rgba [4]float32) {
	c.call(50, v.p, uintptr(unsafe.Pointer(&rgba[0])))
}
func (c Context) ClearDepthStencilView(v View, flags uint32, depth float32, stencil uint8) {
	c.call(53, v.p, uintptr(flags), f32(depth), uintptr(stencil))
}
func (c Context) ClearState() { c.call(110) }
func (c Context) Flush()      { c.call(111) }
