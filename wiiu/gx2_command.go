// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// Where GX2 sends its command packets, display lists, and context states.
//
// Each core has its own destination: the display list it is recording, if
// any, otherwise the GPU itself. Real GX2 batches packets in a command
// buffer and submits on flush; RetroPixels runs them as they're emitted,
// which a program can't tell apart.

// gx2CmdBuf is the display list a core is recording.
type gx2CmdBuf struct {
	isDL     bool
	buf      uint32 // where the list is
	size     uint32 // its capacity in words
	pos      uint32 // words written so far
	overflow bool   // a packet didn't fit
}

// pm4Filler is the type-2 packet used to pad display lists.
const pm4Filler = 0x80000000

// gx2Core is the core the running GX2 call belongs to.
func (m *Machine) gx2Core() int {
	t := m.running
	if t == nil || t.Core < 0 || t.Core >= len(m.gx2.cb) {
		return m.gx2.mainCore
	}
	return t.Core
}

func (m *Machine) gx2Target() *gx2CmdBuf { return &m.gx2.cb[m.gx2Core()] }

// emit delivers packets to the GPU or, packet by packet, into the display
// list the current core is recording.
func (m *Machine) emit(b *pm4) {
	cb := m.gx2Target()
	if !cb.isDL {
		m.gpu.exec(b.w)
		return
	}
	for w := b.w; len(w) > 0; {
		n := min(int(pm4Len(w[0])), len(w))
		m.dlWrite(cb, w[:n])
		w = w[n:]
	}
}

// dlWrite appends one whole packet to a display list. A packet that won't
// fit is dropped entirely, which keeps the list well-formed.
func (m *Machine) dlWrite(cb *gx2CmdBuf, w []uint32) {
	if uint64(cb.pos)+uint64(len(w)) > uint64(cb.size) {
		if !cb.overflow {
			m.warnOnce("gx2: a display list ran out of room")
			cb.overflow = true
		}
		return
	}
	for _, v := range w {
		m.Mem.Write32(cb.buf+4*cb.pos, v)
		cb.pos++
	}
}

// padCmdBuf pads a display list with filler to a 32-byte boundary.
func (m *Machine) padCmdBuf(cb *gx2CmdBuf) {
	for cb.pos%8 != 0 && cb.pos < cb.size {
		m.Mem.Write32(cb.buf+4*cb.pos, pm4Filler)
		cb.pos++
	}
}

// beginDisplayList starts recording into buf (size in bytes) on this core.
func (m *Machine) beginDisplayList(buf, size uint32) {
	*m.gx2Target() = gx2CmdBuf{isDL: true, buf: buf, size: size / 4}
}

// endDisplayList stops this core's recording and returns the list's size
// in bytes, padded.
func (m *Machine) endDisplayList() uint32 {
	cb := m.gx2Target()
	if !cb.isDL {
		return 0
	}
	m.padCmdBuf(cb)
	bytes := 4 * cb.pos
	*cb = gx2CmdBuf{}
	return bytes
}

// GX2PatchDisplayList patch kinds: each names the shader whose program
// address goes into the SET_CONTEXT_REG packet being patched.
const (
	patchFetchShader    = 1
	patchVertexShader   = 2
	patchGeometryCopy   = 3
	patchGeometryShader = 4
	patchPixelShader    = 5
)

// ---- context states ----
//
// A GX2ContextState is a memory image of the GPU registers, one area per
// register space, plus a small display list that loads them all. While a
// state is current, shadowing is on: each LOAD_* packet records where its
// registers came from, and later SET_* writes update the image as well.

// GX2ContextState layout
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
	ctxLoadDLBytes   = 0x300
	ctxStateSize     = 0xa100
)

// ctxLoad is one LOAD_* packet of the state's load list: the opcode, the
// area, the wait flag and the (register offset, count) ranges GX2 loads.
type ctxLoad struct {
	op, area, wait uint32
	ranges         []uint32
}

var ctxLoads = []ctxLoad{
	{itLOAD_CONFIG_REG, ctxAreaConfig, 0x80000000, []uint32{
		0x300, 0x6, 0x900, 0x48, 0x980, 0x48, 0xa00, 0x48, 0x310, 0xc, 0x542, 0x1, 0x235, 0x1, 0x232, 0x2,
		0x23a, 0x1, 0x256, 0x1, 0x60c, 0x1, 0x5c5, 0x1, 0x2c8, 0x1, 0x363, 0x1, 0x404, 0x2,
	}},
	{itLOAD_CONTEXT_REG, ctxAreaContext, 0, []uint32{
		0x0, 0x2, 0x3, 0x3, 0xa, 0x4, 0x10, 0x38, 0x50, 0x34, 0x8e, 0x4, 0x94, 0x40, 0x100, 0x9,
		0x10c, 0x3, 0x10f, 0x60, 0x185, 0xa, 0x191, 0x27, 0x1e0, 0x9, 0x200, 0x1, 0x202, 0x7, 0xe0, 0x20,
		0x210, 0x29, 0x250, 0x34, 0x290, 0x1, 0x292, 0x2, 0x2a1, 0x1, 0x2a5, 0x1, 0x2a8, 0x2, 0x2ac, 0x3,
		0x2ca, 0x1, 0x2cc, 0x1, 0x2ce, 0x1, 0x300, 0x9, 0x30c, 0x1, 0x312, 0x1, 0x316, 0x2, 0x343, 0x2,
		0x349, 0x3, 0x34c, 0x2, 0x351, 0x1, 0x37e, 0x6, 0x2b4, 0x3, 0x2b8, 0x3, 0x2bc, 0x3, 0x2c0, 0x3,
		0x2c8, 0x1, 0x29b, 0x1, 0x8c, 0x1, 0xd5, 0x1, 0x284, 0xc,
	}},
	{itLOAD_ALU_CONST, ctxAreaALUConst, 0, []uint32{0, 0x800}},
	{itLOAD_LOOP_CONST, ctxAreaLoopConst, 0, []uint32{0, 0x60}},
	{itLOAD_RESOURCE, ctxAreaResource, 0, []uint32{
		0, 0x70, 0x380, 0x70, 0x460, 0x70, 0x7e0, 0x70, 0x8b9, 0x7, 0x8c0, 0x70, 0x930, 0x70, 0xcb0, 0x70, 0xd89, 0x7,
	}},
	{itLOAD_SAMPLER, ctxAreaSampler, 0, []uint32{0, 0x36, 0x36, 0x36, 0x6c, 0x36}},
}

// shadowing turns register shadowing on or off.
func shadowing(b *pm4, on bool) {
	if on {
		b.packet(itCONTEXT_CONTROL, ctxShadowOn, ctxShadowOn)
	} else {
		b.packet(itCONTEXT_CONTROL, 0x80000000, 0x80000000)
	}
}

// loadContext appends the packets that make the state at ctx current and
// shadow into it.
func loadContext(b *pm4, ctx uint32) {
	shadowing(b, true)
	for _, l := range ctxLoads {
		body := make([]uint32, 0, 2+len(l.ranges))
		body = append(body, ctx+l.area, l.wait)
		b.packet(l.op, append(body, l.ranges...)...)
	}
}

// setupContextState is GX2SetupContextStateEx: it clears the state, makes
// it current, writes the default register values through it, and records
// its load list.
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
	list := gx2CmdBuf{isDL: true, buf: ctx + ctxLoadDL, size: ctxLoadDLBytes / 4}
	m.dlWrite(&list, b.w)
	m.padCmdBuf(&list)
	m.Mem.Write32(ctx+ctxLoadDLSize, 4*list.pos)
}

// setContextState is GX2SetContextState; 0 turns shadowing off.
func (m *Machine) setContextState(ctx uint32) {
	b := &pm4{}
	if ctx == 0 {
		shadowing(b, false)
		m.emit(b)
		return
	}
	// run the state's load list; build the packets if it has none yet
	if n := m.Mem.Read32(ctx + ctxLoadDLSize); n != 0 {
		shadowing(b, true)
		b.packet(itINDIRECT_BUFFER_PR, ctx+ctxLoadDL, 0, n/4)
	} else {
		loadContext(b, ctx)
	}
	m.emit(b)
}

func init() {
	const x = "gx2"
	// writes both through the two pointer arguments starting at i
	outPair := func(m *Machine, cpu *CPU, i int, a, b uint32) {
		for k, v := range [2]uint32{a, b} {
			if p := arg(cpu, i+k); p != 0 {
				m.Mem.Write32(p, v)
			}
		}
	}

	// packets run as they are emitted, so there is never anything to flush
	reg(x, "GX2Flush", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2GetMainCoreId", func(m *Machine, cpu *CPU, t *Thread) {
		if m.gx2.Init {
			ret(cpu, uint32(m.gx2.mainCore))
		} else {
			ret(cpu, 0xffffffff)
		}
	})

	// ---- display lists ----
	// GX2BeginDisplayList(Ex)(buffer, bytes[, profiling])
	begin := func(m *Machine, cpu *CPU, t *Thread) { m.beginDisplayList(arg(cpu, 0), arg(cpu, 1)) }
	reg(x, "GX2BeginDisplayList", begin)
	reg(x, "GX2BeginDisplayListEx", begin)
	reg(x, "GX2EndDisplayList", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.endDisplayList()) })
	// GX2GetCurrentDisplayList(u32 *buffer, u32 *bytes): false when this
	// core isn't recording
	reg(x, "GX2GetCurrentDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		cb := m.gx2Target()
		if !cb.isDL {
			ret(cpu, 0)
			return
		}
		outPair(m, cpu, 0, cb.buf, 4*cb.size)
		ret(cpu, 1)
	})
	reg(x, "GX2GetDisplayListWriteStatus", func(m *Machine, cpu *CPU, t *Thread) {
		retBool(cpu, m.gx2Target().isDL)
	})
	// GX2CallDisplayList(list, bytes): an indirect buffer in the stream
	reg(x, "GX2CallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		b := &pm4{}
		b.packet(itINDIRECT_BUFFER_PR, arg(cpu, 0), 0, arg(cpu, 1)/4)
		m.emit(b)
	})
	// GX2DirectCallDisplayList runs the list on the GPU at once, even
	// while this core records a list
	reg(x, "GX2DirectCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		m.gpu.execMem(arg(cpu, 0), arg(cpu, 1)/4)
	})
	// GX2CopyDisplayList(list, bytes) inlines the list's words
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
		words := make([]uint32, n)
		for i := range words {
			words[i] = m.Mem.Read32(src + 4*uint32(i))
		}
		m.dlWrite(cb, words)
	})
	// GX2PatchDisplayList(list, kind, byte offset, shader): store the
	// shader program's address >> 8 as the packet's value
	reg(x, "GX2PatchDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		list, kind, offset, shader := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		mem := m.Mem
		// a shader's program is either a plain pointer, or (when that is
		// 0) the buffer of its GX2RBuffer
		program := func(ptr, rbuf uint32) uint32 {
			if p := mem.Read32(shader + ptr); p != 0 {
				return p
			}
			return mem.Read32(shader + rbuf + gx2rBufData)
		}
		var p uint32
		switch kind {
		case patchFetchShader:
			p = mem.Read32(shader + 0xc)
		case patchVertexShader:
			p = program(0xd4, 0x124)
		case patchGeometryCopy:
			p = program(0x58, 0xb0)
		case patchGeometryShader:
			p = program(0x50, 0xa0)
		case patchPixelShader:
			p = program(0xa8, 0xd8)
		default:
			m.warnOnce("gx2: GX2PatchDisplayList of an unknown kind")
			return
		}
		// the packet is header, register offset, value
		mem.Write32(list+offset&^3+8, p>>8)
	})

	// ---- context states ----
	reg(x, "GX2SetupContextStateEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.setupContextState(arg(cpu, 0), arg(cpu, 1))
	})
	reg(x, "GX2SetupContextState", func(m *Machine, cpu *CPU, t *Thread) { m.setupContextState(arg(cpu, 0), 0) })
	reg(x, "GX2SetContextState", func(m *Machine, cpu *CPU, t *Thread) { m.setContextState(arg(cpu, 0)) })
	// GX2GetContextStateDisplayList(state, u32 *list, u32 *bytes)
	reg(x, "GX2GetContextStateDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		ctx := arg(cpu, 0)
		outPair(m, cpu, 1, ctx+ctxLoadDL, m.Mem.Read32(ctx+ctxLoadDLSize))
	})
}
