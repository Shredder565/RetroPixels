// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu),
// src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp and
// src/Cafe/HW/Latte/Core/LattePM4.h, rewritten in Go for RetroPixels.

package wiiu

// The command processor: it reads PM4 packets (from GX2, or written by the
// program itself) and carries them out.

// PM4 type-3 opcodes
const (
	itNOP                = 0x10
	itSET_PREDICATION    = 0x20
	itDRAW_INDEX_2       = 0x27
	itCONTEXT_CONTROL    = 0x28
	itINDEX_TYPE         = 0x2a
	itDRAW_INDEX         = 0x2b
	itDRAW_INDEX_AUTO    = 0x2d
	itDRAW_INDEX_IMMD    = 0x2e
	itNUM_INSTANCES      = 0x2f
	itINDIRECT_BUFFER_PR = 0x32
	itSTRMOUT_BUF_UPDATE = 0x34
	itMEM_SEMAPHORE      = 0x39
	itWAIT_REG_MEM       = 0x3c
	itMEM_WRITE          = 0x3d
	itINDIRECT_BUFFER    = 0x3f
	itSURFACE_SYNC       = 0x43
	itME_INITIALIZE      = 0x44
	itEVENT_WRITE        = 0x46
	itEVENT_WRITE_EOP    = 0x47
	itLOAD_CONFIG_REG    = 0x60
	itLOAD_CONTEXT_REG   = 0x61
	itLOAD_ALU_CONST     = 0x62
	itLOAD_BOOL_CONST    = 0x63
	itLOAD_LOOP_CONST    = 0x64
	itLOAD_RESOURCE      = 0x65
	itLOAD_SAMPLER       = 0x66
	itSET_CONFIG_REG     = 0x68
	itSET_CONTEXT_REG    = 0x69
	itSET_ALU_CONST      = 0x6a
	itSET_BOOL_CONST     = 0x6b
	itSET_LOOP_CONST     = 0x6c
	itSET_RESOURCE       = 0x6d
	itSET_SAMPLER        = 0x6e
	itSET_CTL_CONST      = 0x6f
	itSTRMOUT_BASE_UPD   = 0x72
	itSET_ALL_CONTEXTS   = 0x74
)

// CONTEXT_CONTROL's first word when register shadowing is on: SET_* writes
// then also go to the memory the LOAD_* packets loaded each register from.
const ctxShadowOn = 0x80000077

const (
	regSQ_CONFIG                  = 0x8c00
	regSQ_VTX_SEMANTIC_CLEAR      = 0x288e0
	regVGT_STRMOUT_BUFFER_OFFSET0 = 0x28adc // 4 registers apart for each buffer
)

// SURFACE_SYNC's CP_COHER_CNTL bits
const (
	coherCBDestAll = 0xff << 6
	coherTCAction  = 1 << 23
	coherCBAction  = 1 << 25
)

// gpuAddr turns an address in a packet into one RetroPixels' memory has.
// GX2 here writes the program's (effective) addresses, but a program
// writing its own packets gives physical ones, as the real GPU wants.
func gpuAddr(a uint32) uint32 {
	switch {
	case a >= 0x50000000 && a < 0x90000000: // MEM2
		return a - 0x50000000 + DataBase
	case a != 0 && a < SystemBase: // MEM1 (the part not shadowed by the system area)
		return a + MEM1Base
	}
	return a
}

func pm4Words(h uint32) int { return int(h>>16&0x3fff) + 1 }

// exec runs PM4 packets.
func (g *Latte) exec(w []uint32) {
	for len(w) > 0 {
		h := w[0]
		w = w[1:]
		switch h >> 30 {
		case 0:
			n := min(pm4Words(h), len(w))
			g.type0(h, w[:n])
			w = w[n:]
		case 3:
			n := pm4Words(h)
			if n > len(w) {
				g.warn("truncated packet %08X", h)
				return
			}
			g.packet(h>>8&0xff, w[:n])
			w = w[n:]
		}
		// type 2 is filler; type 1 isn't used
	}
}

// execMem runs a command buffer of n words in memory. As on the GPU, the
// last packet may reach past the end; its words are read anyway.
func (g *Latte) execMem(addr, n uint32) {
	if n == 0 || n > 1<<22 || g.ibDepth >= 8 {
		return
	}
	addr = gpuAddr(addr)
	total := uint32(0)
	for total < n {
		h := g.mem.Read32(addr + total*4)
		total++
		if t := h >> 30; t == 0 || t == 3 {
			total += uint32(pm4Words(h))
		}
	}
	if total > n {
		g.warn("packet runs past the end of the command buffer at %08X", addr)
	}
	w := make([]uint32, total)
	for i := range w {
		w[i] = g.mem.Read32(addr + uint32(i)*4)
	}
	g.ibDepth++
	g.exec(w)
	g.ibDepth--
}

// type0 writes registers from a base, except for the two the GPU treats
// as events.
func (g *Latte) type0(h uint32, d []uint32) {
	switch base := h & 0xffff; base {
	case 0x304a: // top-of-pipe timestamp interrupt
		if g.m != nil && g.m.gx2 != nil {
			g.m.gx2Event(0)
		}
	case 0x304b:
	default:
		for i, v := range d {
			g.setReg(base*4+uint32(i)*4, v)
		}
	}
}

// setRegs is a SET_* packet: a register offset from base, then values.
func (g *Latte) setRegs(base uint32, d []uint32) {
	if len(d) == 0 {
		return
	}
	a := base + d[0]*4
	shadow := g.ctxControl[0] == ctxShadowOn
	for i, v := range d[1:] {
		r := a + uint32(i)*4
		if r < baseConfig || r >= regTop {
			break
		}
		k := (r - baseConfig) / 4
		g.regs[k] = v
		if shadow && g.shadowAddr[k] != 0 {
			g.mem.Write32(g.shadowAddr[k], v)
		}
	}
	if base == baseContext && a <= regSQ_VTX_SEMANTIC_CLEAR && a+uint32(len(d)-1)*4 > regSQ_VTX_SEMANTIC_CLEAR {
		for i := uint32(0); i < 32; i++ {
			g.setReg(regSQ_VTX_SEMANTIC_0+i*4, 0xff)
		}
	}
}

// loadRegs is a LOAD_* packet: memory address, wait flag, then (offset,
// count) pairs. Each register is read from the area at its own offset (the
// areas in a GX2ContextState are as big as their register spaces), and
// remembers that word as its shadow.
func (g *Latte) loadRegs(base uint32, d []uint32) {
	if len(d) < 2 || len(d)&1 != 0 {
		return
	}
	area := gpuAddr(d[0])
	for e := d[2:]; len(e) >= 2; e = e[2:] {
		for i := uint32(0); i < e[1]; i++ {
			r := base + (e[0]+i)*4
			if r < baseConfig || r >= regTop {
				break
			}
			k := (r - baseConfig) / 4
			src := area + (e[0]+i)*4
			g.shadowAddr[k] = src
			g.regs[k] = g.mem.Read32(src)
		}
	}
}

func (g *Latte) packet(op uint32, d []uint32) {
	arg := func(i int) uint32 {
		if i < len(d) {
			return d[i]
		}
		return 0
	}
	switch op {
	case itNOP, itME_INITIALIZE, itEVENT_WRITE, itSTRMOUT_BASE_UPD:
	case itSET_CONFIG_REG:
		g.setRegs(baseConfig, d)
	case itSET_CONTEXT_REG, itSET_ALL_CONTEXTS:
		g.setRegs(baseContext, d)
	case itSET_ALU_CONST:
		g.setRegs(baseALUConst, d)
	case itSET_BOOL_CONST:
		g.setRegs(baseBool, d)
	case itSET_LOOP_CONST:
		g.setRegs(baseLoop, d)
	case itSET_RESOURCE:
		g.setRegs(baseResource, d)
	case itSET_SAMPLER:
		g.setRegs(baseSampler, d)
	case itSET_CTL_CONST:
		g.setRegs(baseCtlConst, d)
	case itLOAD_CONFIG_REG:
		g.loadRegs(baseConfig, d)
	case itLOAD_CONTEXT_REG:
		g.loadRegs(baseContext, d)
	case itLOAD_ALU_CONST:
		g.loadRegs(baseALUConst, d)
	case itLOAD_BOOL_CONST:
		g.loadRegs(baseBool, d)
	case itLOAD_LOOP_CONST:
		g.loadRegs(baseLoop, d)
	case itLOAD_RESOURCE:
		g.loadRegs(baseResource, d)
	case itLOAD_SAMPLER:
		g.loadRegs(baseSampler, d)
	case itCONTEXT_CONTROL:
		g.ctxControl = [2]uint32{arg(0), arg(1)}
	case itSET_PREDICATION: // query address, flags (type 0 = off)
		g.predicate = arg(1)>>13&7 != 0
	case itSURFACE_SYNC: // flags, size >> 8, address >> 8, poll interval
		g.surfaceSync(arg(0), arg(1)<<8, gpuAddr(arg(2)<<8))
	case itINDEX_TYPE:
		g.indexType = arg(0)
	case itNUM_INSTANCES:
		g.numInstances = max(arg(0), 1)
	case itDRAW_INDEX_AUTO: // count, initiator
		if len(d) >= 1 && !g.computeMode() {
			g.draw(d[0], func(i uint32) uint32 { return i })
		}
	case itDRAW_INDEX_2: // max size, address, address high, count, initiator
		if len(d) >= 4 {
			g.draw(d[3], g.indexFetcher(gpuAddr(d[1])))
		}
	case itDRAW_INDEX: // address, address high, count, initiator
		if len(d) >= 3 {
			g.draw(d[2], g.indexFetcher(gpuAddr(d[0])))
		}
	case itDRAW_INDEX_IMMD: // count, initiator, indices (packed in words)
		if len(d) >= 2 {
			g.drawImmediate(d[0], d[2:])
		}
	case itINDIRECT_BUFFER, itINDIRECT_BUFFER_PR: // address, address high, size in words
		if len(d) >= 3 {
			g.execMem(d[0], d[2])
		}
	case itSTRMOUT_BUF_UPDATE:
		g.streamoutUpdate(d)
	case itMEM_SEMAPHORE:
		g.memSemaphore(arg(0), arg(1))
	case itWAIT_REG_MEM:
		g.waitRegMem(d)
	case itMEM_WRITE:
		g.memWrite(d)
	case itEVENT_WRITE_EOP:
		g.eventWriteEOP(d)
	default:
		if !g.hlePacket(op, d) {
			g.warn("unhandled PM4 packet %02X", op)
		}
	}
}

// computeMode: the shader priorities GX2 sets for compute shaders, whose
// "draws" aren't draws.
func (g *Latte) computeMode() bool {
	c := g.reg(regSQ_CONFIG)
	return c>>24&3 == 0 && c>>26&3 == 1 && c>>28&3 == 2 && c>>30&3 == 3
}

func (g *Latte) drawImmediate(count uint32, data []uint32) {
	is32 := g.indexType&3 == 1
	g.draw(count, func(i uint32) uint32 {
		if is32 {
			if int(i) < len(data) {
				return data[i]
			}
			return 0
		}
		if int(i/2) < len(data) {
			return data[i/2] >> (16 * (i & 1)) & 0xffff
		}
		return 0
	})
}

// surfaceSync: the GPU's caches are told some memory changed (TC: textures
// and buffers) or must be written back (CB: colour buffers).
func (g *Latte) surfaceSync(flags, size, addr uint32) {
	if flags&(coherCBAction|coherCBDestAll) != 0 {
		if addr == 0 || size == 0xffffff00 {
			if g.hw != nil {
				g.hw.flushAll()
			}
		} else {
			g.hwFlush(addr, addr+size)
		}
	}
	if addr == 0 || size == 0xffffff00 || flags&coherTCAction == 0 {
		return // whole-memory invalidations are too slow to be worth it
	}
	g.tex.invalidate(addr, addr+size)
	g.hwInvalidate(addr, addr+size)
}

// streamoutUpdate: control (mode, buffer), write address, -, read address, -
func (g *Latte) streamoutUpdate(d []uint32) {
	if len(d) < 5 {
		return
	}
	mode, buf := d[0]>>1&3, d[0]>>8&3
	r := regVGT_STRMOUT_BUFFER_OFFSET0 + buf*16
	switch mode {
	case 0: // start from the beginning
		g.setReg(r, 0)
	case 3: // save the offset to memory
		g.mem.Write32(gpuAddr(d[1]), g.reg(r))
	}
}

// memSemaphore: address, then the action in the top bits (6 signal, 7
// wait). The count is a little-endian 64-bit value.
func (g *Latte) memSemaphore(addr, ctl uint32) {
	addr = gpuAddr(addr)
	v := bswap64(g.mem.Read64(addr))
	switch ctl >> 29 & 7 {
	case 6:
		v++
	case 7:
		// the GPU can't wait for the CPU here: commands run as they're sent
		if v == 0 {
			g.warn("GPU waits on a semaphore that is never signalled")
			return
		}
		v--
	default:
		return
	}
	g.mem.Write64(addr, bswap64(v))
}

// waitRegMem: compare function | 0x10 for memory, address, -, reference,
// mask, poll interval. The GPU can't wait for the CPU here, so a wait that
// isn't met yet is only noted.
func (g *Latte) waitRegMem(d []uint32) {
	if len(d) < 5 || d[0]&0x10 == 0 {
		return
	}
	v := g.mem.Read32(gpuAddr(d[1]&^3)) & d[4]
	ref := d[3]
	ok := true
	switch d[0] & 7 {
	case 1:
		ok = v < ref
	case 2:
		ok = v <= ref
	case 3:
		ok = v == ref
	case 4:
		ok = v != ref
	case 5:
		ok = v >= ref
	case 6:
		ok = v > ref
	}
	if !ok {
		g.warn("GPU waits on memory that doesn't match yet (%08X)", d[1])
	}
}

// memWrite: address, size (0x40000 32 bits, 0 64 bits, 0x20000 64 bits
// little-endian), data low, data high
func (g *Latte) memWrite(d []uint32) {
	if len(d) < 4 {
		return
	}
	a := gpuAddr(d[0] &^ 3)
	if a == 0 {
		return
	}
	switch d[1] {
	case 0x40000:
		g.mem.Write32(a, d[2])
	case 0:
		g.mem.Write32(a, d[2])
		g.mem.Write32(a+4, d[3])
	case 0x20000:
		g.mem.Write32(a, bswap32(d[2]))
		g.mem.Write32(a+4, bswap32(d[3]))
	}
}

// eventWriteEOP: event, address, address high | data select, data low,
// data high. Data select 1 writes 32 bits, 2 writes 64, 3 the GPU clock.
func (g *Latte) eventWriteEOP(d []uint32) {
	if len(d) < 5 {
		return
	}
	a := gpuAddr(d[1] &^ 3)
	switch d[2] >> 29 & 7 {
	case 1:
		g.mem.Write32(a, d[3])
	case 2:
		g.mem.Write32(a, d[4])
		g.mem.Write32(a+4, d[3])
	case 3:
		if g.m != nil {
			g.mem.Write64(a, g.m.bootTime+g.m.Time())
		}
	}
}

func bswap64(v uint64) uint64 {
	return uint64(bswap32(uint32(v)))<<32 | uint64(bswap32(uint32(v>>32)))
}
