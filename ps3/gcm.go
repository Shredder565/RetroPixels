package ps3

// cellGcmSys and cellResc: the game's side of the RSX. The command buffer
// lives in main memory mapped into the RSX's I/O space; the game writes
// commands and moves the control register's put pointer, and RetroPixels'
// RSX (rsx.go) reads them.

const (
	gcmLocalSize = 0x0F900000
	gcmIOMax     = 0x10000000
)

type displayBuffer struct {
	offset, pitch, width, height uint32
}

type tileInfo struct {
	location, offset, size, pitch, comp, base, bank uint32
	bound                                           bool
}

type rescSrc struct {
	format, pitch, width, height, offset uint32
}

type gcmState struct {
	inited      bool
	ioAddress   uint32
	ioSize      uint32
	ioToEA      [gcmIOMax >> 20]uint32 // 1 MB pages: I/O offset -> EA (0 = unmapped)
	eaToIO      map[uint32]uint32      // EA 1 MB page -> I/O offset
	context     uint32                 // CellGcmContextData
	control     uint32                 // CellGcmControl (put, get, ref)
	labels      uint32                 // 256 labels of 16 bytes
	reports     uint32                 // 2048 reports of 16 bytes
	cmdBegin    uint32
	cmdSize     uint32
	callbackOPD uint32
	display     [8]displayBuffer
	flipMode    uint32
	flipHandler uint32
	vblHandler  uint32
	tiles       [15]tileInfo
	zcull       [8][8]uint32

	// cellResc
	rescSrcs     [8]rescSrc
	rescMode     uint32
	rescFlip     uint32
	rescVBlank   uint32
	rescBufs     uint32
	rescDstPitch uint32

	intr *intrThread
}

var tiledPitches = []uint32{0x0000, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0A00, 0x0C00, 0x0D00,
	0x0E00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800, 0x4000, 0x5000, 0x6000,
	0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000}

// ioToAddr turns an I/O offset into an EA (0 when unmapped).
func (g *gcmState) ioToAddr(off uint32) uint32 {
	if off >= gcmIOMax {
		return 0
	}
	p := g.ioToEA[off>>20]
	if p == 0 {
		return 0
	}
	return p + off&0xfffff
}

// offsetToAddr: an RSX offset in a location (0 local, 1 main/IO) to an EA.
func (g *gcmState) offsetToAddr(off uint32, location uint32) uint32 {
	if location == 0 {
		return RSXBase + off
	}
	return g.ioToAddr(off)
}

func (g *gcmState) mapIO(ea, io, size uint32) {
	if g.eaToIO == nil {
		g.eaToIO = map[uint32]uint32{}
	}
	for i := uint32(0); i < size; i += 0x100000 {
		g.ioToEA[(io+i)>>20] = ea + i
		g.eaToIO[(ea+i)>>20] = io + i
	}
}

func (m *Machine) gcm0() *gcmState {
	if m.gcm == nil {
		m.gcm = &gcmState{}
	}
	return m.gcm
}

func init() {
	reg("_cellGcmInitBody", func(m *Machine, t *Thread) { // (context**, cmdSize, ioSize, ioAddress)
		g := m.gcm0()
		cmdSize, ioSize, io := t.arg(1), t.arg(2), t.arg(3)
		if io&0xfffff != 0 || ioSize&0xfffff != 0 {
			t.ret(0x802100ff)
			return
		}
		g.inited = true
		g.ioAddress, g.ioSize = io, ioSize
		g.mapIO(io, 0, ioSize)
		g.cmdBegin, g.cmdSize = io, cmdSize
		g.context = m.sysAlloc(16, 16)
		g.control = m.sysAlloc(16, 16)
		g.labels = m.sysAlloc(0x1000, 0x1000)
		g.reports = m.sysAlloc(0x10000, 0x1000)
		m.Mem.Fill(g.labels, 0, 0x1000)
		m.Mem.Fill(g.reports, 0, 0x10000)
		g.callbackOPD = m.hleOPD("cellGcmCallback", gcmCallback)
		begin := io + 0x1000
		m.Mem.Write32(g.context, begin)
		m.Mem.Write32(g.context+4, io+0x8000-4)
		m.Mem.Write32(g.context+8, begin)
		m.Mem.Write32(g.context+12, g.callbackOPD)
		m.Mem.Write32(g.control, 0x1000) // put
		m.Mem.Write32(g.control+4, 0x1000)
		m.Mem.Write32(g.control+8, 0)
		m.Mem.Write32(t.arg(0), g.context)
		g.intr = m.newIntrThread("_gcm_intr_thread")
		m.rsxInit()
		m.Logf("GCM: command buffer %08X (%x), I/O %08X (%x)", io, cmdSize, io, ioSize)
		t.ret(0)
	})
	reg("_cellGcmFunc15", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellGcmGetConfiguration", func(m *Machine, t *Thread) {
		g := m.gcm0()
		c := t.arg(0)
		m.Mem.Write32(c, RSXBase)
		m.Mem.Write32(c+4, g.ioAddress)
		m.Mem.Write32(c+8, gcmLocalSize)
		m.Mem.Write32(c+12, g.ioSize)
		m.Mem.Write32(c+16, 650000000)
		m.Mem.Write32(c+20, 500000000)
		t.ret(0)
	})
	reg("cellGcmGetControlRegister", func(m *Machine, t *Thread) { t.ret(m.gcm0().control) })
	reg("cellGcmGetLabelAddress", func(m *Machine, t *Thread) { t.ret(m.gcm0().labels + 16*(t.arg(0)&0xff)) })
	reg("cellGcmGetReportDataAddressLocation", func(m *Machine, t *Thread) {
		t.ret(m.gcm0().reports + 16*(t.arg(0)&0x7ff))
	})
	reg("cellGcmAddressToOffset", func(m *Machine, t *Thread) { // (address, offset*)
		g := m.gcm0()
		a := t.arg(0)
		var off uint32
		switch {
		case a >= RSXBase && a < RSXBase+RSXSize:
			off = a - RSXBase
		default:
			io, ok := g.eaToIO[a>>20]
			if !ok {
				t.ret(0x802100ff)
				return
			}
			off = io + a&0xfffff
		}
		m.Mem.Write32(t.arg(1), off)
		t.ret(0)
	})
	reg("cellGcmMapMainMemory", func(m *Machine, t *Thread) { // (ea, size, offset*)
		g := m.gcm0()
		ea, size := t.arg(0), t.arg(1)
		if ea&0xfffff != 0 || size&0xfffff != 0 {
			t.ret(0x802100ff)
			return
		}
		// first free run of I/O pages
		n := size >> 20
		for start := uint32(0); start+n <= gcmIOMax>>20; start++ {
			free := true
			for k := uint32(0); k < n; k++ {
				if g.ioToEA[start+k] != 0 {
					free = false
					break
				}
			}
			if free {
				g.mapIO(ea, start<<20, size)
				m.Mem.Write32(t.arg(2), start<<20)
				t.ret(0)
				return
			}
		}
		t.ret(0x802100ff)
	})
	reg("cellGcmMapEaIoAddress", func(m *Machine, t *Thread) { // (ea, io, size)
		m.gcm0().mapIO(t.arg(0), t.arg(1), t.arg(2))
		t.ret(0)
	})
	reg("cellGcmUnmapEaIoAddress", func(m *Machine, t *Thread) {
		g := m.gcm0()
		ea := t.arg(0)
		if io, ok := g.eaToIO[ea>>20]; ok {
			for p := ea >> 20; ; p++ {
				o, ok := g.eaToIO[p]
				if !ok {
					break
				}
				g.ioToEA[o>>20] = 0
				delete(g.eaToIO, p)
			}
			_ = io
		}
		t.ret(0)
	})
	reg("cellGcmSetDisplayBuffer", func(m *Machine, t *Thread) { // (id, offset, pitch, width, height)
		id := t.arg(0)
		if id > 7 {
			t.ret(0x80210002)
			return
		}
		m.gcm0().display[id] = displayBuffer{t.arg(1), t.arg(2), t.arg(3), t.arg(4)}
		m.Logf("display buffer %d: offset %x pitch %d %dx%d", id, t.arg(1), t.arg(2), t.arg(3), t.arg(4))
		t.ret(0)
	})
	reg("cellGcmSetFlipMode", func(m *Machine, t *Thread) { m.gcm0().flipMode = t.arg(0) })
	reg("cellGcmSetFlipHandler", func(m *Machine, t *Thread) { m.gcm0().flipHandler = t.arg(0) })
	reg("cellGcmSetVBlankHandler", func(m *Machine, t *Thread) { m.gcm0().vblHandler = t.arg(0) })
	reg("_cellGcmSetFlipCommand", func(m *Machine, t *Thread) { // (context*, id)
		m.gcmPutFlip(t, t.arg(0), t.arg(1))
		t.ret(0)
	})
	reg("_cellGcmSetFlipCommandWithWaitLabel", func(m *Machine, t *Thread) { // (context*, id, label, value)
		ctx := t.arg(0)
		m.gcmPutCommand(t, ctx, 0x0064, []uint32{t.arg(2) * 16, t.arg(3)}) // NV406E semaphore offset + acquire
		m.gcmPutFlip(t, ctx, t.arg(1))
		t.ret(0)
	})
	reg("cellGcmGetTiledPitchSize", func(m *Machine, t *Thread) {
		s := t.arg(0)
		for i := 0; i < len(tiledPitches)-1; i++ {
			if tiledPitches[i] < s && s <= tiledPitches[i+1] {
				t.ret(tiledPitches[i+1])
				return
			}
		}
		t.ret(0)
	})
	reg("cellGcmSetTileInfo", func(m *Machine, t *Thread) { // (index, location, offset, size, pitch, comp, base, bank)
		i := t.arg(0)
		if i >= 15 {
			t.ret(0x80210002)
			return
		}
		m.gcm0().tiles[i] = tileInfo{location: t.arg(1), offset: t.arg(2), size: t.arg(3), pitch: t.arg(4), comp: t.arg(5), base: t.arg(6), bank: t.arg(7)}
		t.ret(0)
	})
	reg("cellGcmBindTile", func(m *Machine, t *Thread) {
		if i := t.arg(0); i < 15 {
			m.gcm0().tiles[i].bound = true
		}
		t.ret(0)
	})
	reg("cellGcmUnbindTile", func(m *Machine, t *Thread) {
		if i := t.arg(0); i < 15 {
			m.gcm0().tiles[i].bound = false
		}
		t.ret(0)
	})
	reg("cellGcmBindZcull", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellGcmUnbindZcull", func(m *Machine, t *Thread) { t.ret(0) })

	// --- cellResc: scaling the game's picture to the TV (here: shown as is)
	reg("cellRescInit", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellRescExit", func(m *Machine, t *Thread) {})
	reg("cellRescSetDsts", func(m *Machine, t *Thread) { // (bufferMode, dsts*)
		m.gcm0().rescDstPitch = m.Mem.Read32(t.arg(1) + 4)
		t.ret(0)
	})
	reg("cellRescSetDisplayMode", func(m *Machine, t *Thread) {
		m.gcm0().rescMode = t.arg(0)
		t.ret(0)
	})
	reg("cellRescGetNumColorBuffers", func(m *Machine, t *Thread) { t.ret(2) })
	reg("cellRescGetBufferSize", func(m *Machine, t *Thread) { // (colorBuffers*, vertexArray*, fragmentShader*)
		if t.arg(0) != 0 {
			m.Mem.Write32(t.arg(0), 2*0x400000)
		}
		if t.arg(1) != 0 {
			m.Mem.Write32(t.arg(1), 0x1000)
		}
		if t.arg(2) != 0 {
			m.Mem.Write32(t.arg(2), 0x1000)
		}
		t.ret(0)
	})
	reg("cellRescSetBufferAddress", func(m *Machine, t *Thread) {
		m.gcm0().rescBufs = t.arg(0)
		t.ret(0)
	})
	reg("cellRescSetSrc", func(m *Machine, t *Thread) { // (idx, src*)
		i, s := t.arg(0), t.arg(1)
		if i > 7 {
			t.ret(0x80210302)
			return
		}
		m.gcm0().rescSrcs[i] = rescSrc{format: m.Mem.Read32(s), pitch: m.Mem.Read32(s + 4), width: uint32(m.Mem.Read16(s + 8)),
			height: uint32(m.Mem.Read16(s + 10)), offset: m.Mem.Read32(s + 12)}
		t.ret(0)
	})
	reg("cellRescGcmSurface2RescSrc", func(m *Machine, t *Thread) { // (gcmSurface*, rescSrc*)
		g, r := t.arg(0), t.arg(1)
		m.Mem.Write32(r, uint32(m.Mem.Read8(g+2)))
		m.Mem.Write32(r+4, m.Mem.Read32(g+24))
		m.Mem.Write16(r+8, m.Mem.Read16(g+52))
		m.Mem.Write16(r+10, m.Mem.Read16(g+54))
		m.Mem.Write32(r+12, m.Mem.Read32(g+8))
		t.ret(0)
	})
	reg("cellRescSetConvertAndFlip", func(m *Machine, t *Thread) { // (context*, idx)
		m.gcmPutFlip(t, t.arg(0), 0x100|t.arg(1))
		t.ret(0)
	})
	reg("cellRescSetWaitFlip", func(m *Machine, t *Thread) {})
	reg("cellRescSetFlipHandler", func(m *Machine, t *Thread) { m.gcm0().rescFlip = t.arg(0) })
	reg("cellRescSetVBlankHandler", func(m *Machine, t *Thread) { m.gcm0().rescVBlank = t.arg(0) })
	reg("cellRescAdjustAspectRatio", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellRescCreateInterlaceTable", func(m *Machine, t *Thread) { t.ret(0) })
}

// gcmPutCommand writes a method with its data into a context's buffer.
func (m *Machine) gcmPutCommand(t *Thread, ctx uint32, method uint32, data []uint32) {
	cur := m.Mem.Read32(ctx + 8)
	end := m.Mem.Read32(ctx + 4)
	if cur+4*uint32(len(data)+1) > end {
		m.gcmWrap(ctx)
		cur = m.Mem.Read32(ctx + 8)
	}
	m.Mem.Write32(cur, uint32(len(data))<<18|method)
	for i, d := range data {
		m.Mem.Write32(cur+4+4*uint32(i), d)
	}
	m.Mem.Write32(ctx+8, cur+4+4*uint32(len(data)))
}

// gcmPutFlip queues a flip (id >= 0x100: a cellResc source).
func (m *Machine) gcmPutFlip(t *Thread, ctx uint32, id uint32) {
	m.gcmPutCommand(t, ctx, 0xFEAC, []uint32{id})
	g := m.gcm0()
	if ctx == g.context {
		// the default context: the flip goes out now (as libgcm does)
		m.Mem.Write32(g.control, m.gcmAddrToIO(m.Mem.Read32(ctx+8)))
	}
}

func (m *Machine) gcmAddrToIO(a uint32) uint32 {
	g := m.gcm0()
	if io, ok := g.eaToIO[a>>20]; ok {
		return io + a&0xfffff
	}
	return 0
}

// gcmWrap moves a context to the next 32 KB piece of the command buffer.
func (m *Machine) gcmWrap(ctx uint32) {
	g := m.gcm0()
	cur := m.Mem.Read32(ctx + 8)
	next := (cur&^0x7fff + 0x8000)
	if next >= g.cmdBegin+g.cmdSize {
		next = g.cmdBegin
	}
	begin := next
	if begin == g.cmdBegin {
		begin += 0x1000
	}
	m.Mem.Write32(cur, 0x20000000|m.gcmAddrToIO(begin)) // jump
	m.Mem.Write32(g.control, m.gcmAddrToIO(cur))
	m.Mem.Write32(ctx, begin)
	m.Mem.Write32(ctx+4, next+0x8000-4)
	m.Mem.Write32(ctx+8, begin)
	// let the RSX catch up (it must leave the piece we're about to reuse)
	m.rsxRunUntil(func() bool {
		get := m.gcmIOGet()
		return !(get >= m.gcmAddrToIO(begin) && get < m.gcmAddrToIO(next+0x8000))
	})
}

// gcmCallback is the context's callback: called by the game's command
// writers when the buffer piece is full.
func gcmCallback(m *Machine, t *Thread) {
	ctx := t.arg(0)
	g := m.gcm0()
	cur := m.Mem.Read32(ctx + 8)
	next := (cur&^0x7fff + 0x8000)
	if next >= g.cmdBegin+g.cmdSize {
		next = g.cmdBegin
	}
	begin := next
	if begin == g.cmdBegin {
		begin += 0x1000
	}
	m.Mem.Write32(g.control, m.gcmAddrToIO(cur))
	m.Mem.Write32(cur, 0x20000000|m.gcmAddrToIO(begin))
	m.Mem.Write32(ctx, begin)
	m.Mem.Write32(ctx+4, next+0x8000-4)
	m.Mem.Write32(ctx+8, begin)
	lo, hi := m.gcmAddrToIO(begin), m.gcmAddrToIO(next+0x8000)
	t.ret(0)
	m.block(t, "command buffer", func() bool {
		m.rsxRun()
		get := m.gcmIOGet()
		return get < lo || get >= hi
	})
}

func (m *Machine) gcmIOGet() uint32 { return m.Mem.Read32(m.gcm0().control + 4) }

// ---- interrupt threads: run the game's handlers (vblank, flip)

type intrThread struct {
	t     *Thread
	queue []intrCall
}

type intrCall struct {
	opd  uint32
	args []uint64
}

func (m *Machine) newIntrThread(name string) *intrThread {
	it := &intrThread{}
	loop := m.hleSpecial(name+" loop", func(m *Machine, t *Thread) {
		self := t.PC - 4
		t.PC = self
		if len(it.queue) == 0 {
			m.block(t, "interrupt", func() bool { return len(it.queue) > 0 })
			return
		}
		c := it.queue[0]
		it.queue = it.queue[1:]
		m.callGuest(t, c.opd, c.args, nil)
	})
	opd := m.sysAlloc(8, 8)
	m.Mem.Write32(opd, loop)
	t := m.newThread(name, opd, 0, 1, 0x4000)
	t.state = thReady
	it.t = t
	return it
}

func (it *intrThread) post(opd uint32, args ...uint64) {
	if opd != 0 && len(it.queue) < 8 {
		it.queue = append(it.queue, intrCall{opd, args})
	}
}
