package wiiu

// Where GX2's packets go, display lists, and context states.
//
// Each core sends its packets to the GPU, or into a display list while it
// records one. (The real GX2 fills command buffers and submits them on a
// flush; here packets run as they are written, which the program can't tell
// apart.)
//
// Written for RetroPixels from the SDK's documented GX2 calls and
// GX2ContextState layout.

// gx2CmdBuf is a core's display list while it records one.
type gx2CmdBuf struct {
	isDL     bool
	buf      uint32
	size     uint32 // room, in words
	pos      uint32 // words written
	overflow bool
}

// put adds whole packets to the list. A packet that doesn't fit is dropped
// (with a warning), so what the list holds stays readable.
func (cb *gx2CmdBuf) put(m *Machine, w []uint32) {
	if cb.pos+uint32(len(w)) > cb.size {
		if !cb.overflow {
			m.warnOnce("gx2: a display list ran out of room")
		}
		cb.overflow = true
		return
	}
	for _, v := range w {
		m.Mem.Write32(cb.buf+cb.pos*4, v)
		cb.pos++
	}
}

// pad fills the list with filler packets to a 32-byte boundary.
func (cb *gx2CmdBuf) pad(m *Machine) {
	for cb.pos%8 != 0 && cb.pos < cb.size {
		m.Mem.Write32(cb.buf+cb.pos*4, pm4Filler)
		cb.pos++
	}
}

// gx2Core is the core of the running GX2 call.
func (m *Machine) gx2Core() int {
	if t := m.running; t != nil && t.Core >= 0 && t.Core < len(m.gx2.cb) {
		return t.Core
	}
	return m.gx2.mainCore
}

func (m *Machine) gx2Target() *gx2CmdBuf { return &m.gx2.cb[m.gx2Core()] }

// emit sends packets to the GPU, or packet by packet into the display list.
func (m *Machine) emit(b *pm4) {
	cb := m.gx2Target()
	if !cb.isDL {
		m.gpu.exec(b.w)
		return
	}
	for w := b.w; len(w) > 0; {
		n := 1
		if h := w[0]; h>>30 == 0 || h>>30 == 3 {
			n += pm4Words(h)
		}
		n = min(n, len(w))
		cb.put(m, w[:n])
		w = w[n:]
	}
}

func (m *Machine) beginDisplayList(buf, size uint32) {
	*m.gx2Target() = gx2CmdBuf{isDL: true, buf: buf, size: size / 4}
}

// endDisplayList pads the list out and returns its size in bytes.
func (m *Machine) endDisplayList() uint32 {
	cb := m.gx2Target()
	if !cb.isDL {
		return 0
	}
	cb.pad(m)
	n := cb.pos * 4
	*cb = gx2CmdBuf{}
	return n
}

// ---- context states ----
//
// A GX2ContextState keeps a copy of the GPU's registers, one area per
// register space, and a display list that loads them. While it is current,
// shadowing keeps the copy up to date: the LOAD_* packets tell the GPU
// where each register lives, and SET_* writes go there too.

const (
	ctxAreaConfig    = 0x0000
	ctxAreaContext   = 0x2c00
	ctxAreaALUConst  = 0x3c00
	ctxAreaLoopConst = 0x5c00
	ctxAreaResource  = 0x5e00
	ctxAreaSampler   = 0x9500
	ctxProfiling     = 0x9800
	ctxLoadDLSize    = 0x9804
	ctxLoadDL        = 0x9e00
	ctxLoadDLRoom    = 0x300
	ctxStateSize     = 0xa100
)

// The registers each LOAD_* packet loads, as (first, count) pairs. These are
// the ranges the console's own GX2 uses: a display list the program got from
// GX2GetContextStateDisplayList must load the same registers.
var ctxLoads = []struct {
	op, area, wait uint32
	ranges         []uint32
}{
	{itLOAD_CONFIG_REG, ctxAreaConfig, 0x80000000, []uint32{
		0x300, 6, 0x900, 0x48, 0x980, 0x48, 0xa00, 0x48, 0x310, 0xc, 0x542, 1, 0x235, 1, 0x232, 2,
		0x23a, 1, 0x256, 1, 0x60c, 1, 0x5c5, 1, 0x2c8, 1, 0x363, 1, 0x404, 2}},
	{itLOAD_CONTEXT_REG, ctxAreaContext, 0, []uint32{
		0, 2, 3, 3, 0xa, 4, 0x10, 0x38, 0x50, 0x34, 0x8e, 4, 0x94, 0x40, 0x100, 9,
		0x10c, 3, 0x10f, 0x60, 0x185, 0xa, 0x191, 0x27, 0x1e0, 9, 0x200, 1, 0x202, 7, 0xe0, 0x20,
		0x210, 0x29, 0x250, 0x34, 0x290, 1, 0x292, 2, 0x2a1, 1, 0x2a5, 1, 0x2a8, 2, 0x2ac, 3,
		0x2ca, 1, 0x2cc, 1, 0x2ce, 1, 0x300, 9, 0x30c, 1, 0x312, 1, 0x316, 2, 0x343, 2,
		0x349, 3, 0x34c, 2, 0x351, 1, 0x37e, 6, 0x2b4, 3, 0x2b8, 3, 0x2bc, 3, 0x2c0, 3,
		0x2c8, 1, 0x29b, 1, 0x8c, 1, 0xd5, 1, 0x284, 0xc}},
	{itLOAD_ALU_CONST, ctxAreaALUConst, 0, []uint32{0, 0x800}},
	{itLOAD_LOOP_CONST, ctxAreaLoopConst, 0, []uint32{0, 0x60}},
	{itLOAD_RESOURCE, ctxAreaResource, 0, []uint32{
		0, 0x70, 0x380, 0x70, 0x460, 0x70, 0x7e0, 0x70, 0x8b9, 7, 0x8c0, 0x70, 0x930, 0x70, 0xcb0, 0x70, 0xd89, 7}},
	{itLOAD_SAMPLER, ctxAreaSampler, 0, []uint32{0, 0x36, 0x36, 0x36, 0x6c, 0x36}},
}

func shadowing(b *pm4, on bool) {
	w := uint32(0x80000000)
	if on {
		w = ctxShadowOn
	}
	b.packet(itCONTEXT_CONTROL, w, w)
}

// loadContext adds the packets that make ctx's registers current and shadow
// into it.
func loadContext(b *pm4, ctx uint32) {
	shadowing(b, true)
	for _, l := range ctxLoads {
		b.packet(l.op, append([]uint32{ctx + l.area, l.wait}, l.ranges...)...)
	}
}

// setupContextState is GX2SetupContextStateEx: the state starts as GX2's
// defaults, and gets the display list that loads it.
func (m *Machine) setupContextState(ctx, profiling uint32) {
	m.Mem.Fill(ctx, 0, ctxStateSize)
	m.Mem.Write32(ctx+ctxProfiling, profiling&1)
	b := &pm4{}
	loadContext(b, ctx)
	shadowing(b, true)
	m.emit(b)
	m.gx2DefaultState()

	b = &pm4{}
	loadContext(b, ctx)
	dl := gx2CmdBuf{isDL: true, buf: ctx + ctxLoadDL, size: ctxLoadDLRoom / 4}
	dl.put(m, b.w)
	dl.pad(m)
	m.Mem.Write32(ctx+ctxLoadDLSize, dl.pos*4)
}

// setContextState is GX2SetContextState; no state turns shadowing off.
func (m *Machine) setContextState(ctx uint32) {
	b := &pm4{}
	if ctx == 0 {
		shadowing(b, false)
	} else if n := m.Mem.Read32(ctx + ctxLoadDLSize); n == 0 {
		loadContext(b, ctx)
	} else {
		shadowing(b, true)
		b.packet(itINDIRECT_BUFFER_PR, ctx+ctxLoadDL, 0, n/4)
	}
	m.emit(b)
}

// GX2PatchDisplayList's kinds: which shader's program address replaces the
// one in the SET_CONTEXT_REG packet at the offset
const (
	patchFetchShader    = 1
	patchVertexShader   = 2
	patchGeometryCopy   = 3
	patchGeometryShader = 4
	patchPixelShader    = 5
)

func init() {
	x := "gx2"
	reg(x, "GX2Flush", func(m *Machine, cpu *CPU, t *Thread) {}) // nothing waits to be submitted
	reg(x, "GX2GetMainCoreId", func(m *Machine, cpu *CPU, t *Thread) {
		core := uint32(0xffffffff)
		if m.gx2.Init {
			core = uint32(m.gx2.mainCore)
		}
		ret(cpu, core)
	})

	// display lists: (buffer, size in bytes[, profiling])
	begin := func(m *Machine, cpu *CPU, t *Thread) { m.beginDisplayList(arg(cpu, 0), arg(cpu, 1)) }
	reg(x, "GX2BeginDisplayList", begin)
	reg(x, "GX2BeginDisplayListEx", begin)
	reg(x, "GX2EndDisplayList", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.endDisplayList()) })
	// (buffer out, size out): whether this core records one
	reg(x, "GX2GetCurrentDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		cb := m.gx2Target()
		if cb.isDL {
			for i, v := range [2]uint32{cb.buf, cb.size * 4} {
				if p := arg(cpu, i); p != 0 {
					m.Mem.Write32(p, v)
				}
			}
		}
		retBool(cpu, cb.isDL)
	})
	reg(x, "GX2GetDisplayListWriteStatus", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.gx2Target().isDL) })
	// (list, size in bytes): run the list from here in the command stream
	reg(x, "GX2CallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		b := &pm4{}
		b.packet(itINDIRECT_BUFFER_PR, arg(cpu, 0), 0, arg(cpu, 1)/4)
		m.emit(b)
	})
	// straight to the GPU, even while recording
	reg(x, "GX2DirectCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) { m.gpu.execMem(arg(cpu, 0), arg(cpu, 1)/4) })
	// copy the list's words into the command stream
	reg(x, "GX2CopyDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		src, n := arg(cpu, 0), arg(cpu, 1)/4
		if n == 0 || n > 1<<22 {
			return
		}
		cb := m.gx2Target()
		if !cb.isDL {
			m.gpu.execMem(src, n)
			return
		}
		w := make([]uint32, n)
		for i := range w {
			w[i] = m.Mem.Read32(src + uint32(i)*4)
		}
		cb.put(m, w)
	})
	// (list, kind, byte offset of the packet, shader): a shader's program
	// address is its pointer, or failing that its GX2RBuffer's
	reg(x, "GX2PatchDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		dl, kind, off, shader := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		mem := m.Mem
		fields := map[uint32][2]uint32{ // program pointer, GX2RBuffer
			patchVertexShader: {0xd4, 0x124}, patchPixelShader: {0xa8, 0xd8},
			patchGeometryShader: {0x50, 0xa0}, patchGeometryCopy: {0x58, 0xb0},
		}
		var prog uint32
		if kind == patchFetchShader {
			prog = mem.Read32(shader + 0xc)
		} else if f, ok := fields[kind]; ok {
			if prog = mem.Read32(shader + f[0]); prog == 0 {
				prog = mem.Read32(shader + f[1] + 12)
			}
		} else {
			m.warnOnce("gx2: GX2PatchDisplayList of an unknown kind")
			return
		}
		mem.Write32(dl+off&^3+8, prog>>8)
	})

	// context states: (state, profiling)
	reg(x, "GX2SetupContextStateEx", func(m *Machine, cpu *CPU, t *Thread) { m.setupContextState(arg(cpu, 0), arg(cpu, 1)) })
	reg(x, "GX2SetupContextState", func(m *Machine, cpu *CPU, t *Thread) { m.setupContextState(arg(cpu, 0), 0) })
	reg(x, "GX2SetContextState", func(m *Machine, cpu *CPU, t *Thread) { m.setContextState(arg(cpu, 0)) })
	// (state, list out, size out): the display list that loads it
	reg(x, "GX2GetContextStateDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		ctx := arg(cpu, 0)
		for i, v := range [2]uint32{ctx + ctxLoadDL, m.Mem.Read32(ctx + ctxLoadDLSize)} {
			if p := arg(cpu, 1+i); p != 0 {
				m.Mem.Write32(p, v)
			}
		}
	})
}
