// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu),
// src/Cafe/OS/libs/gx2/GX2_Command.cpp, GX2_Command.h and
// GX2_ContextState.cpp, rewritten in Go for RetroPixels.

package wiiu

// Where GX2's packets go. Each core has its own target: a display list
// while one is being recorded there, or else the GPU (the real GX2 fills
// command buffers and submits them on a flush; here the packets run as
// they're written, which looks the same to the program).

// gx2CmdBuf is one core's display list, when it's recording one.
type gx2CmdBuf struct {
	isDL     bool
	buf      uint32 // address
	size     uint32 // in words
	pos      uint32 // words written
	overflow bool
}

// gx2Core is the core whose GX2 call is running.
func (m *Machine) gx2Core() int {
	if t := m.running; t != nil && t.Core >= 0 && t.Core < len(m.gx2.cb) {
		return t.Core
	}
	return m.gx2.mainCore
}

// emit sends PM4 packets to the GPU, or into the display list.
func (m *Machine) emit(b *pm4) {
	cb := &m.gx2.cb[m.gx2Core()]
	if !cb.isDL {
		m.gpu.exec(b.w)
		return
	}
	for w := b.w; len(w) > 0; {
		n := 1
		if t := w[0] >> 30; t == 0 || t == 3 {
			n += pm4Words(w[0])
		}
		n = min(n, len(w))
		m.dlWrite(cb, w[:n])
		w = w[n:]
	}
}

// dlWrite puts whole packets into a display list. A packet that doesn't
// fit is left out, so the list stays one the GPU can read.
func (m *Machine) dlWrite(cb *gx2CmdBuf, w []uint32) {
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

// padCmdBuf fills a display list with filler to a multiple of 32 bytes.
func (m *Machine) padCmdBuf(cb *gx2CmdBuf) {
	for cb.pos&7 != 0 && cb.pos < cb.size {
		m.Mem.Write32(cb.buf+cb.pos*4, pm4Filler)
		cb.pos++
	}
}

const pm4Filler = 0x80000000 // a type-2 packet

func (m *Machine) beginDisplayList(buf, size uint32) {
	m.gx2.cb[m.gx2Core()] = gx2CmdBuf{isDL: true, buf: buf, size: size / 4}
}

// endDisplayList pads the list and gives its size in bytes.
func (m *Machine) endDisplayList() uint32 {
	cb := &m.gx2.cb[m.gx2Core()]
	if !cb.isDL {
		return 0
	}
	m.padCmdBuf(cb)
	n := cb.pos * 4
	*cb = gx2CmdBuf{}
	return n
}

// GX2PatchDisplayList's kinds of patch: the program address in the
// SET_CONTEXT_REG packet at the offset is replaced by a shader's
const (
	patchFetchShader    = 1
	patchVertexShader   = 2
	patchGeometryCopy   = 3
	patchGeometryShader = 4
	patchPixelShader    = 5
)

// ---- context states ----
//
// A GX2ContextState holds a copy of the GPU's registers, area by area, and
// a display list that loads them. While it's current, register shadowing
// keeps the copy up to date: each LOAD_* packet tells the GPU where its
// registers live in memory, and SET_* writes go there too.

const (
	ctxAreaConfig    = 0x0000 // 0xb00 registers
	ctxAreaContext   = 0x2c00 // 0x400
	ctxAreaALUConst  = 0x3c00 // 0x800
	ctxAreaLoopConst = 0x5c00 // 0x60
	ctxAreaResource  = 0x5e00 // 0xd9e
	ctxAreaSampler   = 0x9500 // 0xa2
	ctxProfiling     = 0x9800
	ctxLoadDLSize    = 0x9804
	ctxLoadDL        = 0x9e00 // 0x300 bytes
	ctxStateSize     = 0xa100
)

// the (register offset, count) ranges each LOAD_* packet loads
var (
	ctxConfigLoads = []uint32{
		0x300, 0x6, 0x900, 0x48, 0x980, 0x48, 0xa00, 0x48, 0x310, 0xc, 0x542, 0x1, 0x235, 0x1, 0x232, 0x2,
		0x23a, 0x1, 0x256, 0x1, 0x60c, 0x1, 0x5c5, 0x1, 0x2c8, 0x1, 0x363, 0x1, 0x404, 0x2,
	}
	ctxContextLoads = []uint32{
		0x0, 0x2, 0x3, 0x3, 0xa, 0x4, 0x10, 0x38, 0x50, 0x34, 0x8e, 0x4, 0x94, 0x40, 0x100, 0x9,
		0x10c, 0x3, 0x10f, 0x60, 0x185, 0xa, 0x191, 0x27, 0x1e0, 0x9, 0x200, 0x1, 0x202, 0x7, 0xe0, 0x20,
		0x210, 0x29, 0x250, 0x34, 0x290, 0x1, 0x292, 0x2, 0x2a1, 0x1, 0x2a5, 0x1, 0x2a8, 0x2, 0x2ac, 0x3,
		0x2ca, 0x1, 0x2cc, 0x1, 0x2ce, 0x1, 0x300, 0x9, 0x30c, 0x1, 0x312, 0x1, 0x316, 0x2, 0x343, 0x2,
		0x349, 0x3, 0x34c, 0x2, 0x351, 0x1, 0x37e, 0x6, 0x2b4, 0x3, 0x2b8, 0x3, 0x2bc, 0x3, 0x2c0, 0x3,
		0x2c8, 0x1, 0x29b, 0x1, 0x8c, 0x1, 0xd5, 0x1, 0x284, 0xc,
	}
	ctxALUConstLoads  = []uint32{0, 0x800}
	ctxLoopConstLoads = []uint32{0, 0x60}
	ctxResourceLoads  = []uint32{
		0, 0x70, 0x380, 0x70, 0x460, 0x70, 0x7e0, 0x70, 0x8b9, 0x7, 0x8c0, 0x70, 0x930, 0x70, 0xcb0, 0x70, 0xd89, 0x7,
	}
	ctxSamplerLoads = []uint32{0, 0x36, 0x36, 0x36, 0x6c, 0x36}
)

func shadowingOn(b *pm4)  { b.packet(itCONTEXT_CONTROL, ctxShadowOn, ctxShadowOn) }
func shadowingOff(b *pm4) { b.packet(itCONTEXT_CONTROL, 0x80000000, 0x80000000) }

// ctxRestore: the packets that make ctx's registers current, and turn on
// shadowing into it.
func ctxRestore(b *pm4, ctx uint32) {
	shadowingOn(b)
	load := func(op, area, wait uint32, ranges []uint32) {
		b.packet(op, append([]uint32{ctx + area, wait}, ranges...)...)
	}
	load(itLOAD_CONFIG_REG, ctxAreaConfig, 0x80000000, ctxConfigLoads)
	load(itLOAD_CONTEXT_REG, ctxAreaContext, 0, ctxContextLoads)
	load(itLOAD_ALU_CONST, ctxAreaALUConst, 0, ctxALUConstLoads)
	load(itLOAD_LOOP_CONST, ctxAreaLoopConst, 0, ctxLoopConstLoads)
	load(itLOAD_RESOURCE, ctxAreaResource, 0, ctxResourceLoads)
	load(itLOAD_SAMPLER, ctxAreaSampler, 0, ctxSamplerLoads)
}

func (m *Machine) setupContextState(ctx, profiling uint32) {
	m.Mem.Fill(ctx, 0, ctxStateSize)
	m.Mem.Write32(ctx+ctxProfiling, profiling&1)
	// load the empty state, then fill it with the defaults
	b := &pm4{}
	ctxRestore(b, ctx)
	shadowingOn(b)
	m.emit(b)
	m.gx2DefaultState()
	// the display list GX2SetContextState calls
	b = &pm4{}
	ctxRestore(b, ctx)
	cb := gx2CmdBuf{isDL: true, buf: ctx + ctxLoadDL, size: 0x300 / 4}
	m.dlWrite(&cb, b.w)
	m.padCmdBuf(&cb)
	m.Mem.Write32(ctx+ctxLoadDLSize, cb.pos*4)
}

func (m *Machine) setContextState(ctx uint32) {
	b := &pm4{}
	if ctx == 0 {
		shadowingOff(b)
	} else if n := m.Mem.Read32(ctx + ctxLoadDLSize); n == 0 {
		ctxRestore(b, ctx)
	} else {
		shadowingOn(b)
		b.packet(itINDIRECT_BUFFER_PR, ctx+ctxLoadDL, 0, n/4)
	}
	m.emit(b)
}

func init() {
	x := "gx2"
	// the GPU runs packets as they come, so there's nothing to submit
	reg(x, "GX2Flush", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2GetMainCoreId", func(m *Machine, cpu *CPU, t *Thread) {
		if !m.gx2.Init {
			ret(cpu, 0xffffffff)
			return
		}
		ret(cpu, uint32(m.gx2.mainCore))
	})

	// ---- display lists ----
	// (buffer, size in bytes[, profiling])
	reg(x, "GX2BeginDisplayList", func(m *Machine, cpu *CPU, t *Thread) { m.beginDisplayList(arg(cpu, 0), arg(cpu, 1)) })
	reg(x, "GX2BeginDisplayListEx", func(m *Machine, cpu *CPU, t *Thread) { m.beginDisplayList(arg(cpu, 0), arg(cpu, 1)) })
	reg(x, "GX2EndDisplayList", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.endDisplayList()) })
	// (u32 *buffer, u32 *size) -> whether this core is recording one
	reg(x, "GX2GetCurrentDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		cb := &m.gx2.cb[m.gx2Core()]
		if !cb.isDL {
			ret(cpu, 0)
			return
		}
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, cb.buf)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, cb.size*4)
		}
		ret(cpu, 1)
	})
	reg(x, "GX2GetDisplayListWriteStatus", func(m *Machine, cpu *CPU, t *Thread) {
		retBool(cpu, m.gx2.cb[m.gx2Core()].isDL)
	})
	// (list, size in bytes): the list runs from here in the command stream
	reg(x, "GX2CallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		b := &pm4{}
		b.packet(itINDIRECT_BUFFER_PR, arg(cpu, 0), 0, arg(cpu, 1)/4)
		m.emit(b)
	})
	// goes to the GPU straight away, even while a display list records
	reg(x, "GX2DirectCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		m.gpu.execMem(arg(cpu, 0), arg(cpu, 1)/4)
	})
	// the list's words are copied into the command stream
	reg(x, "GX2CopyDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		addr, n := arg(cpu, 0), arg(cpu, 1)/4
		if n == 0 || n > 1<<22 {
			return
		}
		cb := &m.gx2.cb[m.gx2Core()]
		if !cb.isDL {
			m.gpu.execMem(addr, n)
			return
		}
		w := make([]uint32, n)
		for i := range w {
			w[i] = m.Mem.Read32(addr + uint32(i)*4)
		}
		m.dlWrite(cb, w)
	})
	// (list, kind, byte offset of the packet, shader)
	reg(x, "GX2PatchDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		dl, kind, off, obj := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		mem := m.Mem
		prog := func(ptrOff, rbufOff uint32) uint32 {
			if p := mem.Read32(obj + ptrOff); p != 0 {
				return p
			}
			return mem.Read32(obj + rbufOff + 12)
		}
		var p uint32
		switch kind {
		case patchFetchShader:
			p = mem.Read32(obj + 0xc)
		case patchVertexShader:
			p = prog(0xd4, 0x124)
		case patchPixelShader:
			p = prog(0xa8, 0xd8)
		case patchGeometryShader:
			p = prog(0x50, 0xa0)
		case patchGeometryCopy:
			p = prog(0x58, 0xb0)
		default:
			m.warnOnce("gx2: GX2PatchDisplayList of an unknown kind")
			return
		}
		mem.Write32(dl+off&^3+8, p>>8)
	})

	// ---- context states ----
	// (state, profiling)
	reg(x, "GX2SetupContextStateEx", func(m *Machine, cpu *CPU, t *Thread) { m.setupContextState(arg(cpu, 0), arg(cpu, 1)) })
	reg(x, "GX2SetupContextState", func(m *Machine, cpu *CPU, t *Thread) { m.setupContextState(arg(cpu, 0), 0) })
	reg(x, "GX2SetContextState", func(m *Machine, cpu *CPU, t *Thread) { m.setContextState(arg(cpu, 0)) })
	// (state, u32 *list, u32 *size): the display list that loads it
	reg(x, "GX2GetContextStateDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		ctx := arg(cpu, 0)
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, ctx+ctxLoadDL)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, m.Mem.Read32(ctx+ctxLoadDLSize))
		}
	})
}
