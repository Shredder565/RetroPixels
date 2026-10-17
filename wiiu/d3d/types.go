package d3d

// Direct3D 11 constants and descriptions (portable, so the state
// translation builds and tests everywhere).

// DXGI formats
const (
	FormatUnknown          = 0
	FormatRGBA32Float      = 2
	FormatRGBA32Uint       = 3
	FormatRGBA32Sint       = 4
	FormatRGBA16Float      = 10
	FormatRGBA16Unorm      = 11
	FormatRGBA16Uint       = 12
	FormatRGBA16Snorm      = 13
	FormatRGBA16Sint       = 14
	FormatRG32Float        = 16
	FormatR32G8X24Typeless = 19
	FormatD32FloatS8X24    = 20
	FormatRGB10A2Unorm     = 24
	FormatRGBA8Unorm       = 28
	FormatRGBA8UnormSRGB   = 29
	FormatRGBA8Uint        = 30
	FormatRGBA8Snorm       = 31
	FormatRGBA8Sint        = 32
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
	MapWrite        = 2
	MapReadWrite    = 3
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

type Texture1DDesc struct {
	Width, MipLevels, ArraySize       uint32
	Format                            uint32
	Usage, BindFlags, CPUAccess, Misc uint32
}

type Texture3DDesc struct {
	Width, Height, Depth, MipLevels   uint32
	Format                            uint32
	Usage, BindFlags, CPUAccess, Misc uint32
}

type BufferDesc struct {
	ByteWidth, Usage, BindFlags, CPUAccess, Misc, StructureStride uint32
}

type Viewport struct{ X, Y, W, H, MinZ, MaxZ float32 }

type Rect struct{ Left, Top, Right, Bottom int32 }

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

type RasterizerDesc struct {
	Fill, Cull            uint32
	FrontCCW              int32
	DepthBias             int32
	DepthBiasClamp        float32
	SlopeScaledDepthBias  float32
	DepthClip             int32
	Scissor, MSAA, AALine int32
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

type Box struct{ Left, Top, Front, Right, Bottom, Back uint32 }

// comparison functions, stencil operations, blend factors and operations,
// filters and address modes (D3D11_*)
const (
	CmpNever  = 1 // then LESS, EQUAL, LESS_EQUAL, GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS
	CmpAlways = 8

	StencilKeep = 1 // then ZERO, REPLACE, INCR_SAT, DECR_SAT, INVERT, INCR, DECR

	BlendZero           = 1
	BlendOne            = 2
	BlendSrcColor       = 3
	BlendInvSrcColor    = 4
	BlendSrcAlpha       = 5
	BlendInvSrcAlpha    = 6
	BlendDestAlpha      = 7
	BlendInvDestAlpha   = 8
	BlendDestColor      = 9
	BlendInvDestColor   = 10
	BlendSrcAlphaSat    = 11
	BlendFactor         = 14
	BlendInvBlendFactor = 15
	BlendSrc1Color      = 16
	BlendInvSrc1Color   = 17
	BlendSrc1Alpha      = 18
	BlendInvSrc1Alpha   = 19

	BlendOpAdd    = 1
	BlendOpSub    = 2
	BlendOpRevSub = 3
	BlendOpMin    = 4
	BlendOpMax    = 5

	FillSolid = 3
	CullNone  = 1
	CullFront = 2
	CullBack  = 3

	FilterMinMagMipPoint  = 0x00
	FilterMinMagMipLinear = 0x15
	FilterAnisotropic     = 0x55
	FilterComparison      = 0x80 // added to a filter

	AddressWrap       = 1
	AddressMirror     = 2
	AddressClamp      = 3
	AddressBorder     = 4
	AddressMirrorOnce = 5
)
