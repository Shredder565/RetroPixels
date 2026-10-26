// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// The Latte command processor. It consumes PM4 command streams, whether
// built by RetroPixels' GX2 or written by the program itself, and carries
// out each packet: register writes and loads, draws, indirect buffers and
// the memory and synchronisation packets.
//
// A PM4 header's top two bits give its type. Type 0 writes a run of
// registers; type 2 is a one-word filler; type 3 carries an opcode in bits
// 8..15 and a body of (bits 16..29) + 1 words.

// type-3 opcodes (the R600 family's IT_* values)
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

// ctxShadowOn is the CONTEXT_CONTROL word that enables register shadowing:
// from then on a SET_* write is also stored to the memory word the register
// was last loaded from by a LOAD_* packet.
const ctxShadowOn = 0x80000077

const (
	regSQ_CONFIG                  = 0x8c00
	regSQ_VTX_SEMANTIC_CLEAR      = 0x288e0
	regVGT_STRMOUT_BUFFER_OFFSET0 = 0x28adc // buffer n's is 16n bytes on
)

// CP_COHER_CNTL bits in SURFACE_SYNC
const (
	coherCBDestAll = 0xff << 6
	coherTCAction  = 1 << 23
	coherCBAction  = 1 << 25
)

// the whole-memory size a SURFACE_SYNC uses
const syncEverything = 0xffffff00

// register bases for the SET_* and LOAD_* packets
var (
	setPacketBase = map[uint32]uint32{
		itSET_CONFIG_REG:   baseConfig,
		itSET_CONTEXT_REG:  baseContext,
		itSET_ALL_CONTEXTS: baseContext,
		itSET_ALU_CONST:    baseALUConst,
		itSET_BOOL_CONST:   baseBool,
		itSET_LOOP_CONST:   baseLoop,
		itSET_RESOURCE:     baseResource,
		itSET_SAMPLER:      baseSampler,
		itSET_CTL_CONST:    baseCtlConst,
	}
	loadPacketBase = map[uint32]uint32{
		itLOAD_CONFIG_REG:  baseConfig,
		itLOAD_CONTEXT_REG: baseContext,
		itLOAD_ALU_CONST:   baseALUConst,
		itLOAD_BOOL_CONST:  baseBool,
		itLOAD_LOOP_CONST:  baseLoop,
		itLOAD_RESOURCE:    baseResource,
		itLOAD_SAMPLER:     baseSampler,
	}
)

// gpuAddr maps an address found in a packet into RetroPixels' memory. GX2
// here puts the program's virtual addresses in packets, but a program that
// builds its own packets uses physical ones like the real GPU expects:
// MEM2 at 0x50000000 and MEM1 near 0.
func gpuAddr(a uint32) uint32 {
	const physMEM2, physMEM2End = 0x50000000, 0x90000000
	if a >= physMEM2 && a < physMEM2End {
		return DataBase + (a - physMEM2)
	}
	if a != 0 && a < SystemBase {
		return MEM1Base + a
	}
	return a
}

// pm4Words is the body length of a type-0 or type-3 packet.
func pm4Words(h uint32) int { return int(h>>16&0x3fff) + 1 }

// pm4Len is the full length of the packet whose header is h.
func pm4Len(h uint32) uint32 {
	if t := h >> 30; t == 0 || t == 3 {
		return 1 + uint32(pm4Words(h))
	}
	return 1
}

// exec runs a stream of packets.
func (g *Latte) exec(w []uint32) {
	for len(w) > 0 {
		h, body := w[0], w[1:]
		switch h >> 30 {
		case 0:
			n := min(pm4Words(h), len(body))
			g.type0(h, body[:n])
			w = body[n:]
		case 3:
			n := pm4Words(h)
			if n > len(body) {
				g.warn("truncated packet %08X", h)
				return
			}
			g.packet(h>>8&0xff, body[:n])
			w = body[n:]
		default: // type 2 filler (type 1 is never used)
			w = body
		}
	}
}

// execMem runs a command buffer of n words at addr. Like the hardware, it
// finishes a last packet that extends past the end of the buffer.
func (g *Latte) execMem(addr, n uint32) {
	const maxWords, maxNesting = 1 << 22, 8
	if n == 0 || n > maxWords || g.ibDepth >= maxNesting {
		return
	}
	addr = gpuAddr(addr)
	var end uint32
	for end < n {
		end += pm4Len(g.mem.Read32(addr + end*4))
	}
	if end > n {
		g.warn("packet runs past the end of the command buffer at %08X", addr)
	}
	words := make([]uint32, end)
	for i := range words {
		words[i] = g.mem.Read32(addr + uint32(i)*4)
	}
	g.ibDepth++
	defer func() { g.ibDepth-- }()
	g.exec(words)
}

// type0 writes registers starting at the header's base. Two bases act as
// interrupts instead.
func (g *Latte) type0(h uint32, vals []uint32) {
	base := h & 0xffff
	switch base {
	case 0x304a: // top-of-pipe timestamp interrupt
		if g.m != nil && g.m.gx2 != nil {
			g.m.gx2Event(0)
		}
		return
	case 0x304b:
		return
	}
	for i, v := range vals {
		g.setReg((base+uint32(i))*4, v)
	}
}

// regIndex is the index into g.regs of the register at byte address r, or
// false outside the register file.
func regIndex(r uint32) (uint32, bool) {
	if r < baseConfig || r >= regTop {
		return 0, false
	}
	return (r - baseConfig) / 4, true
}

// setRegs is a SET_* packet: a register offset (in words from base), then
// the values for consecutive registers.
func (g *Latte) setRegs(base uint32, d []uint32) {
	if len(d) == 0 {
		return
	}
	first := base + d[0]*4
	vals := d[1:]
	shadowing := g.ctxControl[0] == ctxShadowOn
	for i, v := range vals {
		k, ok := regIndex(first + uint32(i)*4)
		if !ok {
			break
		}
		g.regs[k] = v
		if a := g.shadowAddr[k]; shadowing && a != 0 {
			g.mem.Write32(a, v)
		}
	}
	// writing SQ_VTX_SEMANTIC_CLEAR resets all 32 vertex semantics
	last := first + uint32(len(vals))*4
	if base == baseContext && first <= regSQ_VTX_SEMANTIC_CLEAR && regSQ_VTX_SEMANTIC_CLEAR < last {
		for i := uint32(0); i < 32; i++ {
			g.setReg(regSQ_VTX_SEMANTIC_0+4*i, 0xff)
		}
	}
}

// loadRegs is a LOAD_* packet: the memory area, a wait flag, then pairs of
// (register offset, count). A register at offset o is read from the area's
// word o, and that word becomes its shadow location.
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

// packet carries out one type-3 packet.
func (g *Latte) packet(op uint32, d []uint32) {
	if base, ok := setPacketBase[op]; ok {
		g.setRegs(base, d)
		return
	}
	if base, ok := loadPacketBase[op]; ok {
		g.loadRegs(base, d)
		return
	}
	word := func(i int) uint32 {
		if i < len(d) {
			return d[i]
		}
		return 0
	}
	switch op {
	case itNOP, itME_INITIALIZE, itEVENT_WRITE, itSTRMOUT_BASE_UPD:
		// nothing to do
	case itCONTEXT_CONTROL:
		g.ctxControl = [2]uint32{word(0), word(1)}
	case itSET_PREDICATION: // (query address, flags): predication type 0 is off
		g.predicate = word(1)>>13&7 != 0
	case itSURFACE_SYNC: // (coher flags, size >> 8, base >> 8, poll interval)
		g.surfaceSync(word(0), word(1)<<8, gpuAddr(word(2)<<8))
	case itINDEX_TYPE:
		g.indexType = word(0)
	case itNUM_INSTANCES:
		g.numInstances = max(word(0), 1)
	case itDRAW_INDEX_AUTO: // (count, initiator)
		if len(d) > 0 && !g.computeMode() {
			g.draw(d[0], func(i uint32) uint32 { return i })
		}
	case itDRAW_INDEX: // (address, address hi, count, initiator)
		if len(d) >= 3 {
			g.draw(d[2], g.indexFetcher(gpuAddr(d[0])))
		}
	case itDRAW_INDEX_2: // (max size, address, address hi, count, initiator)
		if len(d) >= 4 {
			g.draw(d[3], g.indexFetcher(gpuAddr(d[1])))
		}
	case itDRAW_INDEX_IMMD: // (count, initiator, indices...)
		if len(d) >= 2 {
			g.drawImmediate(d[0], d[2:])
		}
	case itINDIRECT_BUFFER, itINDIRECT_BUFFER_PR: // (address, address hi, words)
		if len(d) >= 3 {
			g.execMem(d[0], d[2])
		}
	case itSTRMOUT_BUF_UPDATE:
		g.streamoutUpdate(d)
	case itMEM_SEMAPHORE:
		g.memSemaphore(word(0), word(1))
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

// computeMode reports whether SQ_CONFIG holds the thread priorities GX2
// programs for compute dispatch, in which case DRAW_INDEX_AUTO is a
// dispatch rather than a draw.
func (g *Latte) computeMode() bool {
	return g.reg(regSQ_CONFIG)>>24 == 0b11_10_01_00
}

// drawImmediate draws with indices carried in the packet: 32-bit, or two
// 16-bit indices per word (low half first).
func (g *Latte) drawImmediate(count uint32, packed []uint32) {
	wide := g.indexType&3 == 1
	g.draw(count, func(i uint32) uint32 {
		if wide {
			if i < uint32(len(packed)) {
				return packed[i]
			}
			return 0
		}
		if w := i / 2; w < uint32(len(packed)) {
			return packed[w] >> (i & 1 * 16) & 0xffff
		}
		return 0
	})
}

// surfaceSync: colour buffer actions mean pending render target data must
// reach memory; a texture cache action means memory changed underneath the
// GPU's caches.
func (g *Latte) surfaceSync(flags, size, addr uint32) {
	whole := addr == 0 || size == syncEverything
	if flags&(coherCBAction|coherCBDestAll) != 0 {
		switch {
		case !whole:
			g.hwFlush(addr, addr+size)
		case g.hw != nil:
			g.hw.flushAll()
		}
	}
	// invalidating everything would cost more than it gains
	if whole || flags&coherTCAction == 0 {
		return
	}
	g.tex.invalidate(addr, addr+size)
	g.hwInvalidate(addr, addr+size)
}

// streamoutUpdate: (control, write address, -, read address, -). Control
// bits 1..2 pick the action, 8..9 the buffer.
func (g *Latte) streamoutUpdate(d []uint32) {
	if len(d) < 5 {
		return
	}
	action, buf := d[0]>>1&3, d[0]>>8&3
	offsetReg := regVGT_STRMOUT_BUFFER_OFFSET0 + 16*buf
	switch action {
	case 0: // restart at offset 0
		g.setReg(offsetReg, 0)
	case 3: // store the current offset
		g.mem.Write32(gpuAddr(d[1]), g.reg(offsetReg))
	}
}

// memSemaphore: (address, control). Control bits 29..31 are 6 to signal
// and 7 to wait. The semaphore is a little-endian 64-bit counter.
func (g *Latte) memSemaphore(addr, ctl uint32) {
	addr = gpuAddr(addr)
	count := bswap64(g.mem.Read64(addr))
	switch ctl >> 29 {
	case 6:
		count++
	case 7:
		// commands run as they are submitted, so a wait can't be deferred
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

// waitRegMem: (function | 0x10 for memory, address, -, reference, mask,
// poll interval). Only memory waits are checked; one that isn't satisfied
// is reported, as there is no later point at which to resume.
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

// memWrite: (address, control, low, high). Control 0x40000 writes 32 bits,
// 0 writes 64, 0x20000 writes 64 bits byte-swapped (little-endian).
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
	case 0:
		g.mem.Write32(a, lo)
		g.mem.Write32(a+4, hi)
	case 0x20000:
		g.mem.Write32(a, bswap32(lo))
		g.mem.Write32(a+4, bswap32(hi))
	}
}

// eventWriteEOP: (event, address, address hi | data select << 29, low,
// high). Select 1 writes the low word, 2 the 64-bit value (high word
// first), 3 the GPU's clock.
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

// bswap64 reverses the byte order of a 64-bit value.
func bswap64(v uint64) uint64 {
	hi, lo := uint32(v>>32), uint32(v)
	return uint64(bswap32(lo))<<32 | uint64(bswap32(hi))
}
