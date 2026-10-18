package wiiu

// gx2: the graphics library.
//
// GX2 drives the Latte GPU (latte.go) the way the real library does, by
// writing PM4 command packets. The packets run straight away, or go into a
// display list in memory when the program is recording one. This file has
// the video output side (scan buffers, swaps, vsync) and the surface
// helpers; gx2_state.go has the drawing state, shaders and draws.

import "math"

// GX2 is the graphics state.
type GX2 struct {
	Init bool
	// GX2R's allocator (the program's functions; 0 = the default heap)
	rAlloc, rFree uint32
	// scan buffers of the TV (0) and GamePad (1)
	Scan        [2]scanBuf
	Swaps       uint32
	Flips       uint32
	Vsyncs      uint64
	lastFlip    uint64 // system time of the last flip and vsync
	lastVsync   uint64
	Interval    uint32
	pendingSwap bool
	// GX2SetEventCallback: function and data for each event type
	// (0 start of pipe, 1 end of pipe, 2 vsync, 3 flip, 4 display list overrun)
	events [5][2]uint32

	// where each core's packets go (gx2_command.go)
	cb       [3]gx2CmdBuf
	mainCore int // the core GX2Init ran on
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

func (m *Machine) warnOnce(s string) {
	if !m.warned[s] {
		m.warned[s] = true
		m.logf("%s", s)
	}
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
	surfEnd     = 0x74
)

type gx2Surface struct {
	addr                                         uint32
	dim, w, h, d, mips, format, aa, use, tile    uint32
	image, mipmaps, swizzle, pitch, size, mipOff uint32
}

func (m *Machine) readSurface(s uint32) gx2Surface {
	r := func(o uint32) uint32 { return m.Mem.Read32(s + o) }
	return gx2Surface{addr: s, dim: r(surfDim), w: max(r(surfWidth), 1), h: max(r(surfHeight), 1), d: max(r(surfDepth), 1),
		mips: max(r(surfMips), 1), format: r(surfFormat), aa: r(surfAA), use: r(surfUse), tile: r(surfTile),
		image: r(surfImage), mipmaps: r(surfMipmaps), swizzle: r(surfSwizzle), pitch: r(surfPitch), size: r(surfSize)}
}

// defaultTile: what GX2_TILE_MODE_DEFAULT becomes
func defaultTile(s *gx2Surface) uint32 {
	switch s.tile {
	case 0:
		if s.dim == 0 || s.dim == 4 { // 1D textures are linear
			return tileLinearAligned
		}
		return tile2DThin1
	case tileLinearSpecial:
		return tileLinearAligned
	}
	return s.tile
}

func (s *gx2Surface) info() surfInfo {
	return calcSurfInfo(s.dim, s.w, s.h, s.d, s.mips, s.format, s.aa, s.use, defaultTile(s))
}

// level gives the layout of mip level lvl, as GPU registers would see it.
func (s *gx2Surface) level(lvl uint32) (surfLayout, uint32, uint32) {
	info := s.info()
	if int(lvl) >= len(info.Levels) {
		lvl = uint32(len(info.Levels) - 1)
	}
	li := info.Levels[lvl]
	base := s.image
	if lvl > 0 {
		base = s.mipmaps + li.Offset
		if s.mipmaps == 0 {
			base = s.image + alignUp(info.Size, info.Align) + li.Offset
		}
	}
	pitch := li.Pitch
	if lvl == 0 && s.pitch != 0 {
		pitch = s.pitch
	}
	slices := s.d
	if s.dim == 2 {
		slices = max(s.d>>lvl, 1)
	}
	sw := uint32(0)
	if isMacroTiled(li.Mode) {
		sw = s.swizzle & 0x700
	}
	w, h := max(s.w>>lvl, 1), max(s.h>>lvl, 1)
	if formatCompressed(s.format) {
		w, h = (w+3)/4, (h+3)/4
	}
	return surfLayout{Base: base, Pitch: pitch, Height: li.Height, Slices: slices, BPP: formatBits(s.format), Mode: li.Mode,
		Samples: 1 << (s.aa & 3), Depth: s.use&4 != 0, Swizzle: sw}, w, h
}

// calcSurface fills in a surface's size, alignment, pitch and mip offsets
// (GX2CalcSurfaceSizeAndAlignment).
func (m *Machine) calcSurface(addr uint32) {
	s := m.readSurface(addr)
	if s.tile == 0 {
		m.Mem.Write32(addr+surfTile, defaultTile(&s))
		s.tile = defaultTile(&s)
	}
	info := s.info()
	mem := m.Mem
	mem.Write32(addr+surfSize, info.Size)
	mem.Write32(addr+surfAlign, info.Align)
	mem.Write32(addr+surfPitch, info.Pitch)
	var mipSize uint32
	for i := 1; i < len(info.Levels); i++ {
		l := info.Levels[i]
		if i <= 13 {
			mem.Write32(addr+surfMipOffs+uint32(i-1)*4, l.Offset)
		}
		mipSize = l.Offset + l.Size
	}
	mem.Write32(addr+surfMipSize, mipSize)
}

func f32toHalf(f float32) uint16 { return f32ToHalf(f) }

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
	xs := make([]uint32, dw) // the scan buffer column of each screen column
	for x := range xs {
		xs[x] = uint32(x) * sb.W / dw * 4
	}
	for y := uint32(0); y < dh; y++ {
		sy := y * sb.H / dh
		r := m.Mem.Slice(src+sy*sb.W*4, len(row))
		if r == nil {
			m.Mem.ReadBytes(src+sy*sb.W*4, row)
			r = row
		}
		d := dst[y*dw : y*dw+dw]
		for x, o := range xs {
			d[x] = uint32(r[o])<<16 | uint32(r[o+1])<<8 | uint32(r[o+2])
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
	g.lastVsync = m.bootTime + m.Time()
	m.gx2Event(2)
	if g.pendingSwap {
		g.pendingSwap = false
		// every swap asked for so far is on screen now (two in one frame
		// show as one picture, but the counts must still meet)
		g.Flips = g.Swaps
		g.lastFlip = g.lastVsync
		for i := range g.Scan {
			m.scanShow(i)
		}
		m.wake(keyFlip)
		m.gx2Event(3)
	}
}

// gx2Event runs the program's callback for a GPU event, like the GPU's
// interrupt does: on core 1, from the interrupt thread.
func (m *Machine) gx2Event(kind int) {
	if e := m.gx2.events[kind]; e[0] != 0 {
		m.queueCallback(1, e[0], uint32(kind), e[1])
	}
}

func argF32(c *CPU, i int) float32 { return float32(argF(c, i)) }

// stackArg: integer argument i (from 0) of a call, past the eight that go
// in registers
func stackArg(m *Machine, c *CPU, i int) uint32 {
	if i < 8 {
		return arg(c, i)
	}
	return m.Mem.Read32(c.GPR[1] + 8 + uint32(i-8)*4)
}

func init() {
	x := "gx2"
	reg(x, "GX2Init", func(m *Machine, cpu *CPU, t *Thread) {
		m.gx2.Init = true
		m.gx2.mainCore = cpu.Core
		m.gx2DefaultState()
	})
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
	for _, n := range []string{"GX2SetTVScale", "GX2SetDRCScale", "GX2SetTVGamma", "GX2SetDRCGamma"} {
		reg(x, n, func(m *Machine, cpu *CPU, t *Thread) {})
	}
	reg(x, "GX2GetSystemTVScanMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 3) }) // 720p
	reg(x, "GX2GetSystemTVAspectRatio", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2GetSystemDRCScanMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2GetSystemDRCMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2IsVideoOutReady", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2SetSwapInterval", func(m *Machine, cpu *CPU, t *Thread) { m.gx2.Interval = arg(cpu, 0) })
	reg(x, "GX2GetSwapInterval", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.gx2.Interval) })
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
			m.Mem.Write64(p, g.lastFlip)
		}
		if p := arg(cpu, 3); p != 0 {
			m.Mem.Write64(p, g.lastVsync)
		}
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
	reg(x, "GX2SetSurfaceSwizzle", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0)
		v := m.Mem.Read32(s + surfSwizzle)
		m.Mem.Write32(s+surfSwizzle, v&^0x700|(arg(cpu, 1)&7)<<8)
	})
	reg(x, "GX2GetSurfaceSwizzle", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)+surfSwizzle)>>8&7)
	})
	reg(x, "GX2DrawDone", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2WaitTimeStamp", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	ts := func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, m.bootTime+m.Time()) }
	reg(x, "GX2GetRetiredTimeStamp", ts)
	reg(x, "GX2GetLastSubmittedTimeStamp", ts)
	// (u64 *out): the GPU's clock when it reaches this point
	sample := func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write64(p, m.bootTime+m.Time())
		}
	}
	reg(x, "GX2SampleTopGPUCycle", sample)
	reg(x, "GX2SampleBottomGPUCycle", sample)
	reg(x, "GX2GetLastFrame", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2GetLastFrameGamma", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2GetGPUTimeout", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 10000) })
	reg(x, "GX2GetMiscParam", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(x, "GX2SetMiscParam", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2TempGetGPUVersion", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 2) })
	// (type, function, data)
	reg(x, "GX2SetEventCallback", func(m *Machine, cpu *CPU, t *Thread) {
		if k := arg(cpu, 0); k < uint32(len(m.gx2.events)) {
			m.gx2.events[k] = [2]uint32{arg(cpu, 1), arg(cpu, 2)}
		}
	})
	reg(x, "GX2GetEventCallback", func(m *Machine, cpu *CPU, t *Thread) {
		var e [2]uint32
		if k := arg(cpu, 0); k < uint32(len(m.gx2.events)) {
			e = m.gx2.events[k]
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, e[0])
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, e[1])
		}
	})
	for _, n := range []string{"GX2SetDRCConnectCallback", "GX2SetGPUTimeout",
		"GX2PrintGPUStatus", "GX2DebugTagUserString", "GX2NotifyMemAlloc", "GX2NotifyMemFree", "GX2Begin", "GX2End",
		"GX2SetDRCConnectCallbackEx", "GX2SetTVBufferEx", "GX2SetAAMode", "GX2SetAAModeEx", "GX2SetAAMask",
		"GX2ExpandDepthBuffer", "GX2ExpandAAColorBuffer", "GX2SetDepthBufferHiZEnable", "GX2SetShaderModeEx",
		"GX2SetShaderMode", "GX2SetShaderModeEx2", "GX2SetStreamOutBuffer", "GX2SetStreamOutEnable",
		"GX2SetStreamOutContext", "GX2SaveStreamOutContext", "GX2SetTessellation", "GX2SetMaxTessellationLevel",
		"GX2SetMinTessellationLevel", "GX2SetGeometryShaderInputRingBuffer", "GX2SetGeometryShaderOutputRingBuffer",
		"GX2SetVertexUniformBlockTimeStamp", "GX2SetSpecialState", "GX2SetupCompatibilityContextState",
		"GX2InitDepthBufferHiZEnable"} {
		reg(x, n, func(m *Machine, cpu *CPU, t *Thread) {})
	}
	reg(x, "GX2CalcGeometryShaderInputRingBufferSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x10000) })
	reg(x, "GX2CalcGeometryShaderOutputRingBufferSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x10000) })
	reg(x, "GX2GetVertexShaderGPRs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 64) })
	reg(x, "GX2GetPixelShaderGPRs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 64) })
	reg(x, "GX2GetGeometryShaderGPRs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 64) })
	reg(x, "GX2GetVertexShaderStackEntries", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 32) })
	reg(x, "GX2GetPixelShaderStackEntries", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 32) })
	reg(x, "GX2GetGeometryShaderStackEntries", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 32) })
	_ = math.Pi
}
