package wiiu

// gx2: the graphics library, driving the Latte GPU.
//
// For now this covers what's needed to show a picture: setting up the TV and
// GamePad scan buffers, clearing colour buffers, copying them to the scan
// buffers, swapping and waiting for vsync. Drawing (shaders, textures,
// vertex data) is the next milestone; draw calls are accepted and skipped.

import "math"

// GX2 is the graphics state.
type GX2 struct {
	Init bool
	// scan buffers of the TV (0) and GamePad (1)
	Scan        [2]scanBuf
	Swaps       uint32
	Flips       uint32
	Vsyncs      uint64
	Interval    uint32
	pendingSwap bool
	timestamp   uint64
}

type scanBuf struct {
	Addr      uint32
	Size      uint32
	Mode      uint32
	Format    uint32
	Buffering uint32
	Enabled   bool
	W, H      uint32
	Back      uint32 // the buffer copies go to
	Front     uint32 // the buffer shown
	Valid     bool   // something was copied and swapped
}

func newGX2() *GX2 { return &GX2{Interval: 1} }

func tvModeDims(mode uint32) (uint32, uint32) {
	switch mode {
	case 1: // 4:3 480p
		return 640, 480
	case 2: // 16:9 480p
		return 854, 480
	case 4: // 1080p
		return 1920, 1080
	}
	return 1280, 720
}

// GX2Surface fields
const (
	surfDim     = 0x00
	surfWidth   = 0x04
	surfHeight  = 0x08
	surfDepth   = 0x0c
	surfMips    = 0x10
	surfFormat  = 0x14
	surfAA      = 0x18
	surfUse     = 0x1c
	surfSize    = 0x20
	surfImage   = 0x24
	surfMipSize = 0x28
	surfMipmaps = 0x2c
	surfTile    = 0x30
	surfSwizzle = 0x34
	surfAlign   = 0x38
	surfPitch   = 0x3c
	surfMipOffs = 0x40
)

// calcSurface fills in a surface's size, alignment and pitch.
func (m *Machine) calcSurface(s uint32) {
	mem := m.Mem
	w := max(mem.Read32(s+surfWidth), 1)
	h := max(mem.Read32(s+surfHeight), 1)
	d := max(mem.Read32(s+surfDepth), 1)
	mips := max(mem.Read32(s+surfMips), 1)
	f := mem.Read32(s + surfFormat)
	tile := mem.Read32(s + surfTile)
	if tile == 0 {
		tile = 4 // 2D tiled thin 1
		if mem.Read32(s+surfDim) == 0 {
			tile = 1 // 1D textures are linear
		}
		mem.Write32(s+surfTile, tile)
	}
	bits := formatBits(f)
	if formatCompressed(f) {
		w, h = (w+3)/4, (h+3)/4
	}
	align := uint32(0x800)
	pitchAlign := uint32(32)
	if tile <= 1 {
		pitchAlign = max(64, 256*8/bits)
		align = 0x100
	}
	pitch := (w + pitchAlign - 1) &^ (pitchAlign - 1)
	hh := (h + 7) &^ 7
	size := pitch * hh * bits / 8 * d
	size = (size + align - 1) &^ (align - 1)
	mem.Write32(s+surfSize, size)
	mem.Write32(s+surfAlign, align)
	if formatCompressed(f) {
		mem.Write32(s+surfPitch, pitch)
	} else {
		mem.Write32(s+surfPitch, pitch)
	}
	// the rest of the mip chain
	var mipSize uint32
	mw, mh := w, h
	for i := uint32(1); i < mips && i < 14; i++ {
		mw, mh = max(mw/2, 1), max(mh/2, 1)
		p := (mw + pitchAlign - 1) &^ (pitchAlign - 1)
		n := p * ((mh + 7) &^ 7) * bits / 8 * d
		n = (n + align - 1) &^ (align - 1)
		if i < 13 {
			mem.Write32(s+surfMipOffs+(i-1)*4, mipSize)
		}
		mipSize += n
	}
	mem.Write32(s+surfMipSize, mipSize)
}

// clearSurface fills a colour surface with one colour.
func (m *Machine) clearSurface(s uint32, r, g, b, a float64) {
	mem := m.Mem
	img := mem.Read32(s + surfImage)
	size := mem.Read32(s + surfSize)
	if img == 0 || size == 0 {
		return
	}
	f := mem.Read32(s+surfFormat) & 0x3f
	u8 := func(v float64) byte { return byte(math.Round(math.Max(0, math.Min(1, v)) * 255)) }
	var px []byte
	switch formatBits(f) {
	case 32:
		if f == 0x19 { // R10G10B10A2
			q := func(v float64, n float64) uint32 { return uint32(math.Round(math.Max(0, math.Min(1, v)) * n)) }
			v := q(a, 3)<<30 | q(b, 1023)<<20 | q(g, 1023)<<10 | q(r, 1023)
			px = []byte{byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)}
		} else {
			px = []byte{u8(r), u8(g), u8(b), u8(a)}
		}
	case 64:
		h := func(v float64) []byte { x := f32toHalf(float32(v)); return []byte{byte(x), byte(x >> 8)} }
		px = append(append(append(h(r), h(g)...), h(b)...), h(a)...)
	case 16:
		v := uint16(u8(r))>>3<<11 | uint16(u8(g))>>2<<5 | uint16(u8(b))>>3
		px = []byte{byte(v), byte(v >> 8)}
	default:
		px = []byte{u8(r)}
	}
	buf := make([]byte, 4096)
	for i := range buf {
		buf[i] = px[i%len(px)]
	}
	for o := uint32(0); o < size; o += uint32(len(buf)) {
		n := min(uint32(len(buf)), size-o)
		mem.WriteBytes(img+o, buf[:n])
	}
}

func f32toHalf(f float32) uint16 {
	b := math.Float32bits(f)
	sign := uint16(b>>16) & 0x8000
	e := int(b>>23&0xff) - 127 + 15
	mant := b & 0x7fffff
	switch {
	case e <= 0:
		return sign
	case e >= 31:
		return sign | 0x7c00
	}
	return sign | uint16(e)<<10 | uint16(mant>>13)
}

// copyToScan copies a colour buffer into a scan buffer's back buffer.
func (m *Machine) copyToScan(cb uint32, target uint32) {
	i := 0
	if target&4 != 0 || target == 4 {
		i = 1
	}
	sb := &m.gx2.Scan[i]
	if sb.Addr == 0 {
		return
	}
	mem := m.Mem
	img := mem.Read32(cb + surfImage)
	w, h := mem.Read32(cb+surfWidth), mem.Read32(cb+surfHeight)
	pitch := mem.Read32(cb + surfPitch)
	tile := mem.Read32(cb + surfTile)
	f := mem.Read32(cb+surfFormat) & 0x3f
	if img == 0 || formatBits(f) != 32 {
		return
	}
	frame := sb.W * sb.H * 4
	dst := sb.Addr + sb.Back*frame
	row := make([]byte, sb.W*4)
	for y := uint32(0); y < sb.H; y++ {
		sy := y * h / sb.H
		for x := uint32(0); x < sb.W; x++ {
			sx := x * w / sb.W
			o := m.surfaceOffset(sx, sy, pitch, h, 4, tile)
			v := mem.Read32(img + o)
			if f == 0x19 {
				// R10G10B10A2 (little-endian) to RGBA8
				le := v>>24 | v>>8&0xff00 | v<<8&0xff0000 | v<<24
				r, g, b := le&0x3ff, le>>10&0x3ff, le>>20&0x3ff
				v = (r>>2)<<24 | (g>>2)<<16 | (b>>2)<<8 | 0xff
			}
			row[x*4], row[x*4+1], row[x*4+2], row[x*4+3] = byte(v>>24), byte(v>>16), byte(v>>8), byte(v)
		}
		mem.WriteBytes(dst+y*sb.W*4, row)
	}
}

// surfaceOffset: where pixel x, y is in a surface (tiled layouts come with
// the GPU; for now surfaces are laid out by row)
func (m *Machine) surfaceOffset(x, y, pitch, height, bpp, tile uint32) uint32 {
	return (y*pitch + x) * bpp
}

// scanShow puts a scan buffer's front buffer in the picture.
func (m *Machine) scanShow(i int) {
	sb := &m.gx2.Scan[i]
	if sb.Addr == 0 || !sb.Valid {
		return
	}
	o := m.screen
	dst, dw, dh := o.TV, uint32(TVWidth), uint32(TVHeight)
	if i == 1 {
		dst, dw, dh = o.DRC, DRCWidth, DRCHeight
	}
	frame := sb.W * sb.H * 4
	src := sb.Addr + sb.Front*frame
	row := make([]byte, sb.W*4)
	srgb := sb.Format&0x400 != 0
	_ = srgb
	for y := uint32(0); y < dh; y++ {
		sy := y * sb.H / dh
		m.Mem.ReadBytes(src+sy*sb.W*4, row)
		d := dst[y*dw : y*dw+dw]
		for x := uint32(0); x < dw; x++ {
			sx := x * sb.W / dw
			d[x] = uint32(row[sx*4])<<16 | uint32(row[sx*4+1])<<8 | uint32(row[sx*4+2])
		}
	}
	if i == 0 {
		o.TVUsed, o.TVFrame = true, m.Frame
	} else {
		o.DRCUsed, o.DRCFrame = true, m.Frame
	}
}

func (g *GX2) vsync(m *Machine) {
	g.Vsyncs++
	if g.pendingSwap {
		g.pendingSwap = false
		g.Flips++
		for i := range g.Scan {
			m.scanShow(i)
		}
		m.wake(keyFlip)
	}
}

func init() {
	x := "gx2"
	reg(x, "GX2Init", func(m *Machine, cpu *CPU, t *Thread) { m.gx2.Init = true })
	reg(x, "GX2Shutdown", func(m *Machine, cpu *CPU, t *Thread) { m.gx2.Init = false })
	calcSize := func(dims func(uint32) (uint32, uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			mode, buffering := arg(cpu, 0), arg(cpu, 2)
			w, h := dims(mode)
			if buffering < 1 {
				buffering = 2
			}
			m.Mem.Write32(arg(cpu, 3), w*h*4*buffering)
			if p := arg(cpu, 4); p != 0 {
				m.Mem.Write32(p, 0)
			}
		}
	}
	reg(x, "GX2CalcTVSize", calcSize(tvModeDims))
	reg(x, "GX2CalcDRCSize", calcSize(func(uint32) (uint32, uint32) { return DRCWidth, DRCHeight }))
	setBuf := func(i int, dims func(uint32) (uint32, uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			sb := &m.gx2.Scan[i]
			sb.Addr, sb.Size, sb.Mode, sb.Format, sb.Buffering = arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4)
			sb.W, sb.H = dims(sb.Mode)
			sb.Back, sb.Front = 0, 0
			if sb.Buffering < 1 {
				sb.Buffering = 2
			}
		}
	}
	reg(x, "GX2SetTVBuffer", setBuf(0, tvModeDims))
	reg(x, "GX2SetDRCBuffer", setBuf(1, func(uint32) (uint32, uint32) { return DRCWidth, DRCHeight }))
	reg(x, "GX2SetTVEnable", func(m *Machine, cpu *CPU, t *Thread) { m.gx2.Scan[0].Enabled = arg(cpu, 0) != 0 })
	reg(x, "GX2SetDRCEnable", func(m *Machine, cpu *CPU, t *Thread) { m.gx2.Scan[1].Enabled = arg(cpu, 0) != 0 })
	reg(x, "GX2SetTVScale", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SetDRCScale", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SetTVGamma", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SetDRCGamma", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2GetSystemTVScanMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 3) }) // 720p
	reg(x, "GX2GetSystemTVAspectRatio", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2GetSystemDRCScanMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2GetSystemDRCMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2IsVideoOutReady", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2SetSwapInterval", func(m *Machine, cpu *CPU, t *Thread) { m.gx2.Interval = arg(cpu, 0) })
	reg(x, "GX2GetSwapInterval", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.gx2.Interval) })
	reg(x, "GX2CopyColorBufferToScanBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		m.copyToScan(arg(cpu, 0), arg(cpu, 1))
	})
	reg(x, "GX2SwapScanBuffers", func(m *Machine, cpu *CPU, t *Thread) {
		for i := range m.gx2.Scan {
			sb := &m.gx2.Scan[i]
			if sb.Addr == 0 {
				continue
			}
			sb.Front = sb.Back
			sb.Valid = true
			if sb.Buffering > 1 {
				sb.Back = (sb.Back + 1) % sb.Buffering
			}
		}
		m.gx2.Swaps++
		m.gx2.pendingSwap = true
	})
	reg(x, "GX2WaitForVsync", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 {
			t.Phase = 0
			return
		}
		t.Phase = 1
		t.block(keyVsync, 0)
	})
	reg(x, "GX2WaitForFlip", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 || !m.gx2.pendingSwap {
			t.Phase = 0
			return
		}
		t.Phase = 1
		t.block(keyFlip, 0)
	})
	reg(x, "GX2GetSwapStatus", func(m *Machine, cpu *CPU, t *Thread) {
		g := m.gx2
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, g.Swaps)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, g.Flips)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write64(p, m.bootTime+m.Time())
		}
		if p := arg(cpu, 3); p != 0 {
			m.Mem.Write64(p, m.bootTime+m.Time())
		}
	})
	reg(x, "GX2ClearColor", func(m *Machine, cpu *CPU, t *Thread) {
		m.clearSurface(arg(cpu, 0), argF(cpu, 0), argF(cpu, 1), argF(cpu, 2), argF(cpu, 3))
	})
	reg(x, "GX2ClearBuffersEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.clearSurface(arg(cpu, 0), argF(cpu, 0), argF(cpu, 1), argF(cpu, 2), argF(cpu, 3))
	})
	reg(x, "GX2ClearBuffers", func(m *Machine, cpu *CPU, t *Thread) {
		m.clearSurface(arg(cpu, 0), argF(cpu, 0), argF(cpu, 1), argF(cpu, 2), argF(cpu, 3))
	})
	reg(x, "GX2CalcSurfaceSizeAndAlignment", func(m *Machine, cpu *CPU, t *Thread) { m.calcSurface(arg(cpu, 0)) })
	reg(x, "GX2CalcDepthBufferHiZInfo", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0x1000)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, 0x800)
		}
	})
	reg(x, "GX2CalcColorBufferAuxInfo", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0x1000)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, 0x800)
		}
	})
	reg(x, "GX2DrawDone", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2WaitTimeStamp", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	ts := func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, m.bootTime+m.Time()) }
	reg(x, "GX2GetRetiredTimeStamp", ts)
	reg(x, "GX2GetLastSubmittedTimeStamp", ts)
	reg(x, "GX2GetLastFrame", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2GetLastFrameGamma", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2GetMainCoreId", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2GetGPUTimeout", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 10000) })
	reg(x, "GX2GetMiscParam", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2SetMiscParam", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2TempGetGPUVersion", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 2) })
	reg(x, "GX2GetEventCallback", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, 0)
		}
	})
	// the rest of the drawing API: accepted, drawn with the GPU milestone
	for _, n := range []string{"GX2Flush", "GX2Invalidate", "GX2SetContextState", "GX2SetupContextStateEx",
		"GX2SetupContextState", "GX2SetDefaultState", "GX2InitColorBufferRegs", "GX2InitDepthBufferRegs",
		"GX2SetColorBuffer", "GX2SetDepthBuffer", "GX2SetViewport", "GX2SetScissor", "GX2SetClearDepthStencil",
		"GX2ClearDepthStencilEx", "GX2ClearDepthStencil", "GX2SetFetchShader", "GX2SetVertexShader", "GX2SetPixelShader",
		"GX2SetGeometryShader", "GX2SetShaderModeEx", "GX2SetShaderMode", "GX2SetAttribBuffer", "GX2SetVertexUniformReg",
		"GX2SetPixelUniformReg", "GX2SetVertexUniformBlock", "GX2SetPixelUniformBlock", "GX2SetGeometryUniformBlock",
		"GX2SetPixelTexture", "GX2SetVertexTexture", "GX2SetPixelSampler", "GX2SetVertexSampler", "GX2DrawEx",
		"GX2DrawIndexedEx", "GX2DrawEx2", "GX2DrawIndexedEx2", "GX2DrawIndexedImmediateEx", "GX2SetColorControl",
		"GX2SetBlendControl", "GX2SetDepthStencilControl", "GX2SetPolygonControl", "GX2SetCullOnlyControl",
		"GX2SetAlphaTest", "GX2SetTargetChannelMasks", "GX2SetStencilMask", "GX2SetPrimitiveRestartIndex",
		"GX2SetLineWidth", "GX2SetPointSize", "GX2SetPointLimits", "GX2SetBlendConstantColor", "GX2SetAlphaToMask",
		"GX2SetPolygonOffset", "GX2SetRasterizerClipControl", "GX2SetRasterizerClipControlEx", "GX2SetSpecialState",
		"GX2InitTextureRegs", "GX2InitSampler", "GX2InitSamplerClamping", "GX2InitSamplerLOD", "GX2InitSamplerXYFilter",
		"GX2InitSamplerZMFilter", "GX2InitSamplerBorderType", "GX2InitSamplerDepthCompare", "GX2InitSamplerFilterAdjust",
		"GX2InitSamplerLODAdjust", "GX2InitSamplerRoundingMode", "GX2BeginDisplayList", "GX2BeginDisplayListEx",
		"GX2CallDisplayList", "GX2DirectCallDisplayList", "GX2CopyDisplayList", "GX2SetDRCConnectCallback",
		"GX2SetEventCallback", "GX2SetDepthOnlyControl", "GX2SetStreamOutBuffer", "GX2SetStreamOutEnable",
		"GX2SetStreamOutContext", "GX2SaveStreamOutContext", "GX2SetTessellation", "GX2SetMaxTessellationLevel",
		"GX2SetMinTessellationLevel", "GX2CopySurface", "GX2CopySurfaceEx", "GX2ResolveAAColorBuffer",
		"GX2ExpandDepthBuffer", "GX2ExpandAAColorBuffer", "GX2ConvertDepthBufferToTextureSurface", "GX2SetAAMode",
		"GX2SetAAModeEx", "GX2SetAAMask", "GX2SetSurfaceSwizzle", "GX2SetupCompatibilityContextState",
		"GX2SetGPUTimeout", "GX2PrintGPUStatus", "GX2DebugTagUserString", "GX2NotifyMemAlloc", "GX2NotifyMemFree",
		"GX2InitFetchShaderEx", "GX2SetVertexUniformBlockTimeStamp", "GX2SetGeometryShaderInputRingBuffer",
		"GX2SetGeometryShaderOutputRingBuffer", "GX2SetShaderModeEx2", "GX2SetDepthBufferHiZEnable",
		"GX2SetColorBufferTarget", "GX2Begin", "GX2End", "GX2SetDRCConnectCallbackEx", "GX2SetTVBufferEx"} {
		reg(x, n, func(m *Machine, cpu *CPU, t *Thread) {})
	}
	reg(x, "GX2EndDisplayList", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2GetCurrentDisplayList", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2GetDisplayListWriteStatus", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2CalcGeometryShaderInputRingBufferSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x10000) })
	reg(x, "GX2CalcGeometryShaderOutputRingBufferSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x10000) })
	reg(x, "GX2CalcFetchShaderSizeEx", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x100+arg(cpu, 0)*0x20) })
	reg(x, "GX2GetVertexShaderGPRs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 64) })
	reg(x, "GX2GetPixelShaderGPRs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 64) })
	reg(x, "GX2GetGeometryShaderGPRs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 64) })
	reg(x, "GX2GetVertexShaderStackEntries", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 32) })
	reg(x, "GX2GetPixelShaderStackEntries", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 32) })
	reg(x, "GX2GetGeometryShaderStackEntries", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 32) })
	reg(x, "GX2GetSurfaceSwizzle", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)+surfSwizzle)>>8&7)
	})
}
