package snes

import (
	"encoding/binary"
	"fmt"
	"sort"
)

// Debug tracer for the RetroPixels SNES Debugger, the Go port of the Tracer
// in web/emu/snes.js. It's off unless EnableTrace is called. It notes who
// wrote each byte of VRAM, CGRAM and WRAM (a DMA transfer or the CPU at an
// address) and where a DMA read it from, so the debugger can find the ROM
// addresses a background came from.
//
// Writer codes: >= 0 the DMA record's id, -1 never written, -3 HDMA,
// -4 a DMA not kept (too many records), <= -16 the CPU: -16 - its PC.
const traceMax = 65536

// DMARecord is one kind of DMA transfer the game made (the same transfer
// made again only counts up Count and moves Last).
type DMARecord struct {
	ID     int    `json:"id"`
	Ch     int    `json:"ch"`
	Ctrl   int    `json:"ctrl"`
	BBAD   int    `json:"bbad"`
	Src    int    `json:"src"`    // A-bus address (bank << 16 | address) the transfer starts at
	Size   int    `json:"size"`   // bytes
	VAddr  int    `json:"vaddr"`  // VRAM word address it started at (-1: not a VRAM transfer)
	VMain  int    `json:"vmain"`  // $2115 then
	CGAddr int    `json:"cgaddr"` // CGRAM address it started at (-1: not a CGRAM transfer)
	WMAdd  int    `json:"wmadd"`  // WRAM address it started at (-1: not a $2180 transfer)
	PC     int    `json:"pc"`     // CPU address just after the instruction that wrote $420B
	RegPC  [7]int `json:"regPc"`  // CPU address just after the instructions that last wrote $43x0-$43x6
	First  int    `json:"first"`  // frames since the tracer started
	Last   int    `json:"last"`
	Count  int    `json:"count"`
	Line   int    `json:"line"` // scan line of the first one
}

type dmaKey struct{ ch, ctrl, bbad, src, size, vaddr, vmain, cg, wm, pc int }

type tracer struct {
	s                 *SNES
	frame             int
	vramSrc, vramFrom [0x10000]int32
	cgSrc, cgFrom     [512]int32
	wramSrc, wramFrom [0x20000]int32
	regPC             [128]int32
	recs              []*DMARecord
	keys              map[dmaKey]int
	inDma, inHdma     bool
	cur, from         int
	cgLoSrc, cgLoFrom int32
}

func newTracer(s *SNES) *tracer {
	t := &tracer{s: s, keys: map[dmaKey]int{}, cur: -1, from: -1, cgLoSrc: -1, cgLoFrom: -1}
	t.forget()
	return t
}

func fillNeg(a []int32) {
	for i := range a {
		a[i] = -1
	}
}

// forget: the console was reset, its memory starts again.
func (t *tracer) forget() {
	fillNeg(t.vramSrc[:])
	fillNeg(t.vramFrom[:])
	fillNeg(t.cgSrc[:])
	fillNeg(t.cgFrom[:])
	fillNeg(t.wramSrc[:])
	fillNeg(t.wramFrom[:])
	fillNeg(t.regPC[:])
}

func (t *tracer) writer() int32 {
	if t.inDma {
		return int32(t.cur)
	}
	if t.inHdma {
		return -3
	}
	return int32(-16 - t.s.cpu.PC())
}

func (t *tracer) src() int32 {
	if t.inDma {
		return int32(t.from)
	}
	return -1
}

func (t *tracer) vram(b int) { t.vramSrc[b], t.vramFrom[b] = t.writer(), t.src() }
func (t *tracer) cgLo()      { t.cgLoSrc, t.cgLoFrom = t.writer(), t.src() }
func (t *tracer) wram(a int) { t.wramSrc[a], t.wramFrom[a] = t.writer(), t.src() }
func (t *tracer) reg(r int)  { t.regPC[r] = int32(t.s.cpu.PC()) }
func (t *tracer) dmaEnd()    { t.inDma, t.cur, t.from = false, -1, -1 }
func (t *tracer) cgHi(w int) {
	b := w * 2
	t.cgSrc[b], t.cgFrom[b] = t.cgLoSrc, t.cgLoFrom
	t.cgSrc[b+1], t.cgFrom[b+1] = t.writer(), t.src()
}

var dmaSpan = [8]int{1, 2, 2, 4, 4, 4, 2, 4}

func (t *tracer) dmaBegin(ch, ctrl, bbad, src, size int) {
	s, p, pc := t.s, t.s.ppu, t.s.cpu.PC()
	span := dmaSpan[ctrl&7]
	hits := func(lo, hi int) bool { return bbad <= hi && bbad+span-1 >= lo }
	vaddr, vmain, cg, wm := -1, -1, -1, -1
	if hits(0x18, 0x19) {
		vaddr, vmain = p.vramAddrMapped(), p.vmain
	}
	if hits(0x22, 0x22) {
		cg = p.cgaddr
	}
	if hits(0x80, 0x80) {
		wm = s.wmadd
	}
	k := dmaKey{ch, ctrl, bbad, src, size, vaddr, vmain, cg, wm, pc}
	id, ok := t.keys[k]
	if !ok {
		if len(t.recs) >= traceMax {
			id = -4
		} else {
			id = len(t.recs)
			r := &DMARecord{ID: id, Ch: ch, Ctrl: ctrl, BBAD: bbad, Src: src, Size: size, VAddr: vaddr, VMain: vmain,
				CGAddr: cg, WMAdd: wm, PC: pc, First: t.frame, Last: t.frame, Line: s.line}
			for i := range r.RegPC {
				r.RegPC[i] = int(t.regPC[ch*16+i])
			}
			t.recs = append(t.recs, r)
			t.keys[k] = id
		}
	}
	if id >= 0 {
		r := t.recs[id]
		r.Last = t.frame
		r.Count++
	}
	t.inDma, t.cur, t.from = true, id, -1
}

// EnableTrace starts the debug tracer (RetroPixels SNES Debugger): from now
// on the core notes who writes VRAM, CGRAM and WRAM.
func (s *SNES) EnableTrace() {
	if s.tr == nil {
		s.tr = newTracer(s)
	}
}

// Capture is what the debugger saves for one moment of the game: Meta is
// capture.json, Files the binary files by name, as web/emu/snes.js makes them.
type Capture struct {
	Meta  map[string]any
	Files map[string][]byte
}

func le32(a []int32) []byte {
	b := make([]byte, len(a)*4)
	for i, v := range a {
		binary.LittleEndian.PutUint32(b[i*4:], uint32(v))
	}
	return b
}

func le16(a []uint16) []byte {
	b := make([]byte, len(a)*2)
	for i, v := range a {
		binary.LittleEndian.PutUint16(b[i*2:], v)
	}
	return b
}

// DebugCapture returns the capture, or an error when EnableTrace wasn't called.
func (s *SNES) DebugCapture() (*Capture, error) {
	t := s.tr
	if t == nil {
		return nil, fmt.Errorf("snes: the debug tracer isn't on")
	}
	p, c := s.ppu, s.cart
	used := map[int]bool{}
	for _, arr := range [][]int32{t.vramSrc[:], t.cgSrc[:], t.wramSrc[:]} {
		for _, v := range arr {
			if v >= 0 {
				used[int(v)] = true
			}
		}
	}
	recent := append([]*DMARecord(nil), t.recs...)
	sort.SliceStable(recent, func(i, j int) bool {
		if recent[i].Last != recent[j].Last {
			return recent[i].Last > recent[j].Last
		}
		return recent[i].ID > recent[j].ID
	})
	if len(recent) > 512 {
		recent = recent[:512]
	}
	for _, r := range recent {
		used[r.ID] = true
	}
	ids := make([]int, 0, len(used))
	for id := range used {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	records := make([]DMARecord, len(ids))
	for i, id := range ids {
		records[i] = *t.recs[id]
	}
	st := s.cpu.GetState()
	ints := func(a []int) []int { return append([]int(nil), a...) }
	dmaRegs := make([]int, len(s.dma))
	for i, v := range s.dma {
		dmaRegs[i] = int(v)
	}
	mapName := [...]string{"lo", "hi", "exhi"}
	meta := map[string]any{
		"format": "retropixels-snes-capture", "version": 1,
		"frame": t.frame, "line": s.line, "title": c.title, "map": mapName[c.mapping], "romSize": c.size, "headerAt": c.headerAt, "chip": c.chip,
		"cpu": map[string]any{"a": st[0], "x": st[1], "y": st[2], "s": st[3], "d": st[4], "dbr": st[5], "pc": st[6]<<16 | st[7], "p": st[8], "e": st[9]},
		"ppu": map[string]any{
			"inidisp": p.inidisp, "obsel": p.obsel, "bgmode": p.bgmode, "mosaic": p.mosaic, "bgsc": ints(p.bgsc[:]), "bgnba": ints(p.bgnba[:]),
			"hofs": ints(p.hofs[:]), "vofs": ints(p.vofs[:]), "vmain": p.vmain, "vaddr": p.vaddr, "m7sel": p.m7sel, "m7": ints(p.m7[:]),
			"m7hofs": p.m7hofs, "m7vofs": p.m7vofs, "cgaddr": p.cgaddr, "wsel": ints(p.wsel[:]), "wh": ints(p.wh[:]), "wbglog": p.wbglog,
			"wobjlog": p.wobjlog, "tm": p.tm, "ts": p.ts, "tmw": p.tmw, "tsw": p.tsw, "cgwsel": p.cgwsel, "cgadsub": p.cgadsub,
			"fixed": p.fixed, "setini": p.setini,
		},
		"dmaRegs": dmaRegs, "hdmaen": s.hdmaen, "nmitimen": s.nmitimen, "records": records,
	}
	files := map[string][]byte{
		"vram.bin": le16(p.vram[:]), "cgram.bin": le16(p.cgram[:]), "oam.bin": append([]byte(nil), p.oam[:]...), "wram.bin": append([]byte(nil), s.wram[:]...),
		"vram_src.bin": le32(t.vramSrc[:]), "vram_from.bin": le32(t.vramFrom[:]), "cgram_src.bin": le32(t.cgSrc[:]), "cgram_from.bin": le32(t.cgFrom[:]),
		"wram_src.bin": le32(t.wramSrc[:]), "wram_from.bin": le32(t.wramFrom[:]),
	}
	return &Capture{Meta: meta, Files: files}, nil
}
