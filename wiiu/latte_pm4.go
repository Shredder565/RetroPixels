package wiiu

// The GPU's command processor: it reads PM4 packets, from GX2 or written by
// the program itself, and carries them out.
//
// A packet starts with a header word whose top two bits give its type.
// Type 0 writes a run of registers; type 2 is a one-word filler; type 3 is
// an operation (opcode in bits 8-15) with its own data. Types 0 and 3 give
// their length, less one, in bits 16-29.
//
// Written for RetroPixels from AMD's published R600/R700 PM4 packet formats.

// type-3 opcodes
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

// ctxShadowOn is CONTEXT_CONTROL's first word with register shadowing on:
// SET_* writes then also go to the memory each register was last loaded
// from.
const ctxShadowOn = 0x80000077

const (
	regSQ_CONFIG                  = 0x8c00
	regSQ_VTX_SEMANTIC_CLEAR      = 0x288e0
	regVGT_STRMOUT_BUFFER_OFFSET0 = 0x28adc // each buffer's is 16 bytes on

	// SURFACE_SYNC's CP_COHER_CNTL bits
	coherCBDestAll = 0xff << 6
	coherTCAction  = 1 << 23
	coherCBAction  = 1 << 25

	pm4Filler = 0x80000000 // a type-2 packet
)

// gpuAddr turns an address in a packet into one in RetroPixels' memory.
// GX2 here writes the program's own (effective) addresses, but packets a
// program builds itself hold physical ones, as the real GPU wants.
func gpuAddr(a uint32) uint32 {
	if a >= 0x50000000 && a < 0x90000000 { // MEM2
		return a - 0x50000000 + DataBase
	}
	if a != 0 && a < SystemBase { // MEM1, below the system area
		return a + MEM1Base
	}
	return a
}

// pm4Words is how many words follow a type-0 or type-3 header.
func pm4Words(h uint32) int { return int(h>>16&0x3fff) + 1 }

// pm4Op carries out one type-3 packet with data d.
type pm4Op func(g *Latte, d []uint32)

var pm4Ops [256]pm4Op

// at is d[i], or 0 past the end.
func at(d []uint32, i int) uint32 {
	if i < len(d) {
		return d[i]
	}
	return 0
}

func init() {
	ignore := func(*Latte, []uint32) {}
	for _, op := range []uint32{itNOP, itME_INITIALIZE, itEVENT_WRITE, itSTRMOUT_BASE_UPD} {
		pm4Ops[op] = ignore
	}
	for op, base := range map[uint32]uint32{
		itSET_CONFIG_REG: baseConfig, itSET_CONTEXT_REG: baseContext, itSET_ALL_CONTEXTS: baseContext,
		itSET_ALU_CONST: baseALUConst, itSET_BOOL_CONST: baseBool, itSET_LOOP_CONST: baseLoop,
		itSET_RESOURCE: baseResource, itSET_SAMPLER: baseSampler, itSET_CTL_CONST: baseCtlConst,
	} {
		base := base
		pm4Ops[op] = func(g *Latte, d []uint32) { g.setRegs(base, d) }
	}
	for op, base := range map[uint32]uint32{
		itLOAD_CONFIG_REG: baseConfig, itLOAD_CONTEXT_REG: baseContext, itLOAD_ALU_CONST: baseALUConst,
		itLOAD_BOOL_CONST: baseBool, itLOAD_LOOP_CONST: baseLoop, itLOAD_RESOURCE: baseResource,
		itLOAD_SAMPLER: baseSampler,
	} {
		base := base
		pm4Ops[op] = func(g *Latte, d []uint32) { g.loadRegs(base, d) }
	}

	pm4Ops[itCONTEXT_CONTROL] = func(g *Latte, d []uint32) { g.ctxControl = [2]uint32{at(d, 0), at(d, 1)} }
	// query address, flags: a predication type of 0 turns it off
	pm4Ops[itSET_PREDICATION] = func(g *Latte, d []uint32) { g.predicate = at(d, 1)>>13&7 != 0 }
	// flags, size >> 8, address >> 8, poll interval
	pm4Ops[itSURFACE_SYNC] = func(g *Latte, d []uint32) { g.surfaceSync(at(d, 0), at(d, 1)<<8, gpuAddr(at(d, 2)<<8)) }
	pm4Ops[itINDEX_TYPE] = func(g *Latte, d []uint32) { g.indexType = at(d, 0) }
	pm4Ops[itNUM_INSTANCES] = func(g *Latte, d []uint32) { g.numInstances = max(at(d, 0), 1) }

	// draws
	pm4Ops[itDRAW_INDEX_AUTO] = func(g *Latte, d []uint32) { // count, initiator
		if len(d) > 0 && !g.computeMode() {
			g.draw(d[0], func(i uint32) uint32 { return i })
		}
	}
	pm4Ops[itDRAW_INDEX_2] = func(g *Latte, d []uint32) { // max, address, high, count, initiator
		if len(d) > 3 {
			g.draw(d[3], g.indexFetcher(gpuAddr(d[1])))
		}
	}
	pm4Ops[itDRAW_INDEX] = func(g *Latte, d []uint32) { // address, high, count, initiator
		if len(d) > 2 {
			g.draw(d[2], g.indexFetcher(gpuAddr(d[0])))
		}
	}
	pm4Ops[itDRAW_INDEX_IMMD] = func(g *Latte, d []uint32) { // count, initiator, packed indices
		if len(d) > 1 {
			g.drawImmediate(d[0], d[2:])
		}
	}

	// address, high, size in words
	indirect := func(g *Latte, d []uint32) {
		if len(d) > 2 {
			g.execMem(d[0], d[2])
		}
	}
	pm4Ops[itINDIRECT_BUFFER] = indirect
	pm4Ops[itINDIRECT_BUFFER_PR] = indirect

	pm4Ops[itSTRMOUT_BUF_UPDATE] = (*Latte).streamoutUpdate
	pm4Ops[itMEM_SEMAPHORE] = func(g *Latte, d []uint32) { g.memSemaphore(at(d, 0), at(d, 1)) }
	pm4Ops[itWAIT_REG_MEM] = (*Latte).waitRegMem
	pm4Ops[itMEM_WRITE] = (*Latte).memWrite
	pm4Ops[itEVENT_WRITE_EOP] = (*Latte).eventWriteEOP
}

// exec runs a run of packets.
func (g *Latte) exec(w []uint32) {
	for len(w) > 0 {
		h := w[0]
		w = w[1:]
		if h>>30 == 2 || h>>30 == 1 { // filler, and a type nothing uses
			continue
		}
		n := pm4Words(h)
		if h>>30 == 0 {
			n = min(n, len(w))
			g.type0(h, w[:n])
		} else {
			if n > len(w) {
				g.warn("truncated packet %08X", h)
				return
			}
			g.packet(h>>8&0xff, w[:n])
		}
		w = w[n:]
	}
}

func (g *Latte) packet(op uint32, d []uint32) {
	if f := pm4Ops[op]; f != nil {
		f(g, d)
		return
	}
	if !g.hlePacket(op, d) {
		g.warn("unhandled PM4 packet %02X", op)
	}
}

// execMem runs a command buffer of n words in memory. As on the GPU, a last
// packet that runs past the end is read whole anyway.
func (g *Latte) execMem(addr, n uint32) {
	if n == 0 || n > 1<<22 || g.ibDepth >= 8 {
		return
	}
	addr = gpuAddr(addr)
	var words uint32
	for words < n {
		h := g.mem.Read32(addr + words*4)
		words++
		if h>>30 == 0 || h>>30 == 3 {
			words += uint32(pm4Words(h))
		}
	}
	if words > n {
		g.warn("packet runs past the end of the command buffer at %08X", addr)
	}
	buf := make([]uint32, words)
	for i := range buf {
		buf[i] = g.mem.Read32(addr + uint32(i)*4)
	}
	g.ibDepth++
	g.exec(buf)
	g.ibDepth--
}

// type0 writes registers from the header's base, except at the two bases
// the GPU treats as events.
func (g *Latte) type0(h uint32, d []uint32) {
	base := h & 0xffff
	switch {
	case base == 0x304a: // a top-of-pipe timestamp interrupt
		if g.m != nil && g.m.gx2 != nil {
			g.m.gx2Event(0)
		}
	case base != 0x304b:
		for i, v := range d {
			g.setReg((base+uint32(i))*4, v)
		}
	}
}

// regIndex is where a register's byte address sits in g.regs.
func regIndex(r uint32) (int, bool) {
	if r < baseConfig || r >= regTop {
		return 0, false
	}
	return int((r - baseConfig) / 4), true
}

// setRegs is a SET_* packet: a register offset from base, then the values
// for consecutive registers.
func (g *Latte) setRegs(base uint32, d []uint32) {
	if len(d) == 0 {
		return
	}
	first := base + d[0]*4
	shadowing := g.ctxControl[0] == ctxShadowOn
	for i, v := range d[1:] {
		k, ok := regIndex(first + uint32(i)*4)
		if !ok {
			break
		}
		g.regs[k] = v
		if shadowing && g.shadowAddr[k] != 0 {
			g.mem.Write32(g.shadowAddr[k], v)
		}
	}
	// writing SQ_VTX_SEMANTIC_CLEAR resets the 32 vertex semantics
	last := first + uint32(len(d)-1)*4
	if base == baseContext && first <= regSQ_VTX_SEMANTIC_CLEAR && last > regSQ_VTX_SEMANTIC_CLEAR {
		for i := uint32(0); i < 32; i++ {
			g.setReg(regSQ_VTX_SEMANTIC_0+i*4, 0xff)
		}
	}
}

// loadRegs is a LOAD_* packet: a memory area, a wait flag, then (offset,
// count) pairs of registers to load. Each register comes from the word at
// its own offset in the area, which then becomes its shadow.
func (g *Latte) loadRegs(base uint32, d []uint32) {
	if len(d) < 2 || len(d)%2 != 0 {
		return
	}
	area := gpuAddr(d[0])
	for pairs := d[2:]; len(pairs) >= 2; pairs = pairs[2:] {
		off, count := pairs[0], pairs[1]
		for i := uint32(0); i < count; i++ {
			k, ok := regIndex(base + (off+i)*4)
			if !ok {
				break
			}
			src := area + (off+i)*4
			g.shadowAddr[k] = src
			g.regs[k] = g.mem.Read32(src)
		}
	}
}

// computeMode reports the shader priorities GX2 sets for compute shaders,
// whose "draws" aren't draws.
func (g *Latte) computeMode() bool {
	return g.reg(regSQ_CONFIG)>>24 == 0b11_10_01_00
}

// drawImmediate draws with indices packed into the packet, two 16-bit ones
// per word (low half first) or one 32-bit one.
func (g *Latte) drawImmediate(count uint32, packed []uint32) {
	wide := g.indexType&3 == 1
	g.draw(count, func(i uint32) uint32 {
		if wide {
			return at(packed, int(i))
		}
		return at(packed, int(i/2)) >> (i & 1 * 16) & 0xffff
	})
}

// surfaceSync tells the GPU some memory changed (TC: textures and buffers
// must be read again) or must be written back (CB: colour buffers).
func (g *Latte) surfaceSync(flags, size, addr uint32) {
	everything := addr == 0 || size == 0xffffff00
	if flags&(coherCBAction|coherCBDestAll) != 0 {
		switch {
		case !everything:
			g.hwFlush(addr, addr+size)
		case g.hw != nil:
			g.hw.flushAll()
		}
	}
	// invalidating all of memory is too slow to be worth it
	if !everything && flags&coherTCAction != 0 {
		g.tex.invalidate(addr, addr+size)
		g.hwInvalidate(addr, addr+size)
	}
}

// streamoutUpdate: control (mode in bits 1-2, buffer in 8-9), write
// address, -, read address, -.
func (g *Latte) streamoutUpdate(d []uint32) {
	if len(d) < 5 {
		return
	}
	offset := regVGT_STRMOUT_BUFFER_OFFSET0 + (d[0]>>8&3)*16
	switch d[0] >> 1 & 3 {
	case 0: // from the start
		g.setReg(offset, 0)
	case 3: // store the offset
		g.mem.Write32(gpuAddr(d[1]), g.reg(offset))
	}
}

// memSemaphore: an address, and the action in the top three bits of ctl (6
// signal, 7 wait), on a little-endian 64-bit count.
func (g *Latte) memSemaphore(addr, ctl uint32) {
	addr = gpuAddr(addr)
	count := bswap64(g.mem.Read64(addr))
	switch ctl >> 29 {
	case 6:
		count++
	case 7:
		// commands run as they're sent, so the GPU can't wait for the CPU
		if count == 0 {
			g.warn("GPU waits on a semaphore that is never signalled")
			return
		}
		count--
	default:
		return
	}
	g.mem.Write64(addr, bswap64(count))
}

// waitRegMem: function (| 0x10 for memory), address, -, reference, mask,
// poll interval. The GPU can't wait for the CPU here, so a wait that isn't
// met yet is only reported.
func (g *Latte) waitRegMem(d []uint32) {
	if len(d) < 5 || d[0]&0x10 == 0 {
		return
	}
	v, ref := g.mem.Read32(gpuAddr(d[1]&^3))&d[4], d[3]
	met := [8]bool{true, v < ref, v <= ref, v == ref, v != ref, v >= ref, v > ref, true}[d[0]&7]
	if !met {
		g.warn("GPU waits on memory that doesn't match yet (%08X)", d[1])
	}
}

// memWrite: address, size (0x40000: 32 bits; 0: 64; 0x20000: 64 bits
// little-endian), low word, high word.
func (g *Latte) memWrite(d []uint32) {
	if len(d) < 4 {
		return
	}
	a := gpuAddr(d[0] &^ 3)
	if a == 0 {
		return
	}
	lo, hi := d[2], d[3]
	switch d[1] {
	case 0x40000:
		g.mem.Write32(a, lo)
	case 0x20000:
		g.mem.Write32(a, bswap32(lo))
		g.mem.Write32(a+4, bswap32(hi))
	case 0:
		g.mem.Write32(a, lo)
		g.mem.Write32(a+4, hi)
	}
}

// eventWriteEOP: event, address, high | data select (bits 29-31), low,
// high. Data select 1 writes 32 bits, 2 writes 64, 3 the GPU clock.
func (g *Latte) eventWriteEOP(d []uint32) {
	if len(d) < 5 {
		return
	}
	a := gpuAddr(d[1] &^ 3)
	switch d[2] >> 29 {
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
