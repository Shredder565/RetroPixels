// Package boards holds the Konami arcade boards (Go ports of
// web/emu/arcade_tmnt.js, arcade_tmnt2.js and arcade_simpsons.js), which
// register themselves with the arcade package. Each behaves exactly like its
// JS original: the same picture and sound for the same ROM set and inputs.
package boards

import (
	"fmt"
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/konami"
	"retropixels/cores/m68k"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// ---- TMNT (1989): see arcade_tmnt.js for the board's description ----

const (
	tmntLines   = 264
	tmntLineCyc = 512
	tmntW       = 304
	tmntH       = 224
	tmntMapX0   = 104
	tmntMapY0   = 16
	tmntVblank  = 240
)

// The board's clocks and the sums made from them, worked out at run time
// with float64 steps in the JS's order (Go's constant arithmetic is exact and
// would round differently).
var (
	tmntCPUHz   = float64(8000000)
	tmntZ80Hz   = float64(3579545)
	tmntUPDHz   = float64(640000)
	tmntTitleHz = float64(20000)
	tmntFPS     = tmntCPUHz / (float64(tmntLineCyc) * float64(tmntLines))
	tmntZ80Line = tmntZ80Hz / (tmntFPS * float64(tmntLines))
	tmntDCKeep  = 1 - 2*math.Pi*float64(20)/(tmntZ80Hz/64)
	tmntUPDStep = float64(64) * tmntUPDHz / tmntZ80Hz
	tmntTitStep = tmntTitleHz * float64(64) / tmntZ80Hz
)

var tmntTileColorBase = [3]int{0, 32, 40}

const tmntSpriteColorBase = 16

// interleave2: program ROMs in pairs, even (high) and odd (low) bytes.
func interleave2(even, odd []byte) []byte {
	out := make([]byte, len(even)*2)
	for i := range even {
		out[i*2] = even[i]
		if i < len(odd) {
			out[i*2+1] = odd[i]
		}
	}
	return out
}

// wordPair: two 16-bit-wide ROMs side by side make one 32-bit word.
func wordPair(a, b []byte) []byte {
	out := make([]byte, len(a)*2)
	for i := 0; i+1 < len(a); i += 2 {
		out[i*2], out[i*2+1], out[i*2+2], out[i*2+3] = a[i], a[i+1], b[i], b[i+1]
	}
	return out
}

var sprW = [8]int{1, 2, 1, 2, 4, 2, 4, 8}
var sprH = [8]int{1, 1, 2, 2, 2, 4, 4, 8}
var sprLog = map[int]int{1: 0, 2: 1, 4: 2, 8: 3}

// cellPlace: where cell (code & 63) of a block stored as `size` pictures starts.
func cellPlace(size, c int) (start, cx, cy int) {
	nx, ny := sprLog[sprW[size]], sprLog[sprH[size]]
	n, k := 0, 0
	for b := 0; b < 6; b++ {
		bit, i := (c>>b)&1, b>>1
		if b&1 != 0 {
			if i < ny {
				cy |= bit << i
				continue
			}
		} else if i < nx {
			cx |= bit << i
			continue
		}
		n |= bit << k
		k++
	}
	return n * sprW[size] * sprH[size] * 32, cx, cy
}

var promSize = [8]int{0, 2, 1, 3, 5, 4, 6, 7}

func spriteCells(packed, prom []uint8) []uint8 {
	cells := len(packed) >> 8
	out := make([]uint8, cells*256)
	for c := 0; c < cells; c++ {
		size := promSize[prom[(c>>6)&255]&7]
		start, cx, cy := cellPlace(size, c&63)
		row := 2 * sprW[size]
		base := (c>>6)*2048 + start
		for y := 0; y < 16; y++ {
			w := base + (cy*16+y)*row + cx*2
			copy(out[c*256+y*16:c*256+y*16+16], packed[w*8:w*8+16])
		}
	}
	return out
}

func tmntSpriteStart(code, size int) int {
	start, _, _ := cellPlace(size, code&63)
	return (code&^63)*32 + start
}

// decodePacked: each 32-bit ROM word is a row of 8 pixels of 4 bits.
func decodePacked(rom []byte) []uint8 {
	n := len(rom) >> 2
	pens := make([]uint8, n*8)
	for w := 0; w < n; w++ {
		for k := 0; k < 4; k++ {
			v := rom[w*4+3-k]
			pens[w*8+k*2], pens[w*8+k*2+1] = v>>4, v&15
		}
	}
	return pens
}

// planarByte: one byte of a packed row as the video chips show it to the CPU.
func planarByte(pens []uint8, row, n int) int {
	at := (row * 8) % len(pens)
	v := 0
	for x := 0; x < 8; x++ {
		v |= int((pens[at+x]>>uint(n))&1) << (7 - x)
	}
	return v
}

// decodeTitle: the title song, 16-bit words of a 3-bit exponent over a 10-bit value.
func decodeTitle(rom []byte) []float32 {
	n := len(rom) >> 1
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		w := int(rom[i*2]) | int(rom[i*2+1])<<8
		e, m := w>>13, ((w>>3)&0x3ff)-0x200
		out[i] = float32(float64(m*(1<<e)) / float64(0x200*128))
	}
	return out
}

// priorityTable: the layer-mixing PROM, or the same choices worked out by rule.
func priorityTable(prom []byte) [256]uint8 {
	var t [256]uint8
	ok := false
	if len(prom) >= 256 {
		for _, v := range prom {
			if v&15 != 0 {
				ok = true
				break
			}
		}
	}
	for i := 0; i < 256; i++ {
		if ok {
			t[i] = prom[i] & 7
			continue
		}
		aOn, bOn, sOn, fOn, sh, aFront := i&1 != 0, i&2 != 0, i&4 != 0, i&8 != 0, i&16 != 0, i&64 != 0
		sel := 0
		switch {
		case fOn:
			sel = 3
		case aOn && (aFront || !sOn):
			sel = 0
		case sOn:
			sel = 2
		case bOn:
			sel = 1
		}
		dark := sh && sel != 3 && !(sel == 0 && aOn && aFront) && (aOn || bOn || sOn)
		t[i] = uint8(sel)
		if !dark {
			t[i] |= 4
		}
	}
	return t
}

// roleFile: the first file of a role, or zeros of its size when it's missing.
func roleFile(g *arcade.Game, files arcade.Files, role string) []byte {
	for i := range g.Files {
		if f := &g.Files[i]; f.Role == role {
			if d := arcade.FindFile(f, files); d != nil {
				return d
			}
			return make([]byte, f.Size)
		}
	}
	return nil
}

type tmnt struct {
	game       *arcade.Game
	sampleRate int
	dips       map[string]int

	rom      [0x60000]uint8
	zrom     [0x8000]uint8
	title    []float32
	prioProm [256]uint8

	ram     [0x4000]uint8
	palRam  [0x800]uint8
	palette [1024]uint32
	shade   [1024]uint32
	zram    [0x800]uint8
	screen  []uint32
	bufB    []uint16
	bufA    []uint16
	bufF    []uint16
	bufS    []uint16
	bufSh   []uint8
	inputs  []arcade.Pad

	k052109 *konami.K052109
	k051960 *konami.K051960
	k007232 *konami.K007232
	upd     *konami.UPD7759
	ym      *ym2151.YM2151
	out     *arcade.Resampler
	cpu     *m68k.CPU
	z80     *z80.CPU

	ctrl, priority, soundLatch, sres int
	titlePos                         int
	titleAcc, updAcc                 float64
	mixClk                           int
	zcyc                             float64
	updDC, updPrev, updV, kL         float64
	line, frame                      int
}

type tmntMain struct{ b *tmnt }
type tmntSound struct{ b *tmnt }

func newTMNT(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &tmnt{game: g, sampleRate: opts.SampleRate, dips: opts.Dips}
	if b.sampleRate == 0 {
		b.sampleRate = 48000
	}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range tmntDips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	get := func(role string) []byte { return roleFile(g, files, role) }
	copy(b.rom[:], interleave2(get("cpu_even"), get("cpu_odd")))
	p2 := interleave2(get("cpu2_even"), get("cpu2_odd"))
	copy(b.rom[0x40000:], p2[:min(len(p2), 0x20000)])
	copy(b.zrom[:], get("audio"))
	tiles := wordPair(get("tiles_a"), get("tiles_b"))
	spr := make([]byte, 0x200000)
	copy(spr, wordPair(get("sprites_a"), get("sprites_b")))
	copy(spr[0x100000:], wordPair(get("sprites_c"), get("sprites_d")))
	b.title = decodeTitle(get("title"))
	b.prioProm = priorityTable(get("prom_mixer"))

	b.screen = make([]uint32, tmntW*tmntH)
	n := tmntW * tmntH
	b.bufB, b.bufA, b.bufF, b.bufS = make([]uint16, n), make([]uint16, n), make([]uint16, n), make([]uint16, n)
	b.bufSh = make([]uint8, n)

	b.k052109 = konami.NewK052109(konami.K052109Opts{
		ROM: tiles, Decode: decodePacked,
		TileCallback: func(t *konami.Tile) {
			c := t.Color
			t.Code = t.Code | (c&0x03)<<8 | (c&0x10)<<6 | (c&0x0c)<<9 | t.Bank<<13
			t.Color = tmntTileColorBase[t.Layer] + (c&0xe0)>>5
			t.FlipX = false
		},
		RomRead: func(off int, k *konami.K052109) int {
			row := (k.BankReg(0)>>2)<<16 | int(k.Ram[0x1e00])<<11 | ((off >> 2) & 0x7ff)
			return planarByte(k.Pens, row, off&3)
		},
	})
	b.k052109.DX[1], b.k052109.DX[2] = -6, -6
	g7 := get("prom_g7")
	sprPacked := decodePacked(spr)
	haveG7 := false
	if len(g7) >= 256 {
		for _, v := range g7 {
			if v != 0 {
				haveG7 = true
				break
			}
		}
	}
	so := konami.K051960Opts{
		ROM: spr, Decode: func([]byte) []uint8 { return sprPacked },
		ReadbackAddr: func(w int) int { return (w & 0x3ffff) | ((w>>22)&1)<<18 },
		RomByte:      func(w, n int) int { return planarByte(sprPacked, w, n) },
		SpriteCallback: func(t *konami.Sprite) {
			t.Code |= (t.Color & 0x10) << 9
			t.Shadow = t.Color&0x80 != 0
			t.Color = tmntSpriteColorBase + t.Color&0x0f
		},
	}
	if haveG7 {
		so.Pens = spriteCells(sprPacked, g7)
	} else {
		so.SpriteStart = tmntSpriteStart
	}
	b.k051960 = konami.NewK051960(so)
	b.k007232 = konami.NewK007232(tmntZ80Hz, get("pcm"), func(v uint8) {
		hi, lo := int(v>>4)*0x11, int(v&15)*0x11
		b.k007232.SetVolume(0, hi, hi)
		b.k007232.SetVolume(1, lo, lo)
	})
	b.upd = konami.NewUPD7759(tmntUPDHz, get("speech"))
	b.ym = ym2151.New(tmntZ80Hz)
	b.out = arcade.NewResampler(tmntZ80Hz/64, float64(b.sampleRate))
	b.cpu = m68k.New(tmntMain{b})
	b.z80 = z80.New(tmntSound{b})
	b.Reset()
	return b, nil
}

func (b *tmnt) Reset() {
	b.ram = [0x4000]uint8{}
	b.palRam = [0x800]uint8{}
	b.palette = [1024]uint32{}
	b.shade = [1024]uint32{}
	b.zram = [0x800]uint8{}
	b.k052109.Reset()
	b.k051960.Reset()
	b.k007232.Reset()
	b.upd.Reset()
	b.ym.Reset()
	b.ctrl, b.priority, b.soundLatch, b.sres = 0, 0, 0, 0
	b.titlePos, b.titleAcc, b.updAcc, b.mixClk, b.zcyc, b.updDC, b.updPrev, b.updV, b.kL = -1, 0, 0, 0, 0, 0, 0, 0, 0
	b.line, b.frame = 0, 0
	b.cpu.Cycles = 0
	b.cpu.SetIRQ(0)
	b.cpu.Reset()
	b.cpu.Cycles = 0
	b.z80.Reset()
	b.z80.IRQ = false
	b.upd.ResetLine(false)
	b.out.Clear()
}

func (b *tmnt) SetInput(pads []arcade.Pad) { b.inputs = pads }

func (b *tmnt) pad(p int) arcade.Pad {
	if p < len(b.inputs) {
		return b.inputs[p]
	}
	return nil
}

func (b *tmnt) playerByte(p int) int {
	i, p1 := b.pad(p), b.pad(0)
	start := i["Start (1P)"] || (p == 1 && p1["Start (2P)"])
	v := 0
	if i["Left"] {
		v |= 1
	}
	if i["Right"] {
		v |= 2
	}
	if i["Up"] {
		v |= 4
	}
	if i["Down"] {
		v |= 8
	}
	if i["Button 1"] || start {
		v |= 0x10
	}
	if i["Button 2"] {
		v |= 0x20
	}
	if start {
		v |= 0x80
	}
	return ^v & 0xff
}

func (b *tmnt) coinByte() int {
	p1 := b.pad(0)
	v := 0
	if p1["Coin"] {
		v |= 1
	}
	if b.pad(1)["Coin"] || p1["Coin 2"] {
		v |= 2
	}
	if b.pad(2)["Coin"] {
		v |= 4
	}
	if b.pad(3)["Coin"] {
		v |= 8
	}
	for n := 0; n < 4; n++ {
		if b.pad(n)["Service"] {
			v |= 0x10 << n
		}
	}
	return ^v & 0xff
}

func (b *tmnt) dsw(n int) int {
	d := b.dips
	switch n {
	case 1:
		return d["coinage"] & 0xff
	case 2:
		v := (d["lives"] & 3) | 0x1c | (d["difficulty"]&3)<<5
		if d["demo"] == 0 {
			v |= 0x80
		}
		return v & 0xff
	}
	v := 0xfa
	if d["flip"] == 0 {
		v |= 1
	}
	if d["test"] == 0 {
		v |= 4
	}
	return v & 0xff
}

func (b *tmnt) ioRead(a uint32) int {
	switch a & 0x1e {
	case 0x00:
		return b.coinByte()
	case 0x02:
		return b.playerByte(0)
	case 0x04:
		return b.playerByte(1)
	case 0x06:
		return b.playerByte(2)
	case 0x10:
		return b.dsw(1)
	case 0x12:
		return b.dsw(2)
	case 0x14:
		return b.playerByte(3)
	case 0x18:
		return b.dsw(3)
	}
	return 0xff
}

// vOff: the K052109 through this board's wiring (the 68000's A12 isn't connected).
func vOff(a uint32) int {
	w := int(a>>1) & 0x3fff
	return (w&0x3000)>>1 | (w & 0x07ff)
}

func (m tmntMain) Read16(a uint32) uint16 {
	b := m.b
	switch {
	case a < 0x60000:
		return uint16(b.rom[a])<<8 | uint16(b.rom[a+1])
	case a >= 0x60000 && a < 0x64000:
		a -= 0x60000
		return uint16(b.ram[a])<<8 | uint16(b.ram[a+1])
	case a >= 0x80000 && a < 0x81000:
		return 0xff00 | uint16(b.palRam[(a-0x80000)>>1])
	case a >= 0xa0000 && a < 0xa0020:
		return 0xff00 | uint16(b.ioRead(a))
	case a >= 0x100000 && a < 0x108000:
		o := vOff(a)
		return uint16(b.k052109.Read(o))<<8 | uint16(b.k052109.Read(o+0x2000))
	case a >= 0x140000 && a < 0x140008:
		r := int(a & 7)
		return uint16(b.k051960.ReadReg(r))<<8 | uint16(b.k051960.ReadReg(r+1))
	case a >= 0x140400 && a < 0x140800:
		return uint16(b.k051960.Read(int(a)))<<8 | uint16(b.k051960.Read(int(a)+1))
	}
	return 0xffff
}

func (m tmntMain) Read8(a uint32) uint8 {
	b := m.b
	switch {
	case a < 0x60000:
		return b.rom[a]
	case a >= 0x60000 && a < 0x64000:
		return b.ram[a-0x60000]
	case a >= 0x80000 && a < 0x81000:
		if a&1 != 0 {
			return b.palRam[(a-0x80000)>>1]
		}
		return 0xff
	case a >= 0xa0000 && a < 0xa0020:
		if a&1 != 0 {
			return uint8(b.ioRead(a))
		}
		return 0xff
	case a >= 0x100000 && a < 0x108000:
		o := vOff(a)
		if a&1 != 0 {
			o += 0x2000
		}
		return b.k052109.Read(o)
	case a >= 0x140000 && a < 0x140008:
		return b.k051960.ReadReg(int(a & 7))
	case a >= 0x140400 && a < 0x140800:
		return b.k051960.Read(int(a))
	}
	return 0xff
}

func (m tmntMain) Write16(a uint32, v uint16) {
	b := m.b
	switch {
	case a >= 0x60000 && a < 0x64000:
		a -= 0x60000
		b.ram[a], b.ram[a+1] = uint8(v>>8), uint8(v)
		return
	case a >= 0x100000 && a < 0x108000:
		o := vOff(a)
		b.k052109.Write(o, uint8(v>>8))
		b.k052109.Write(o+0x2000, uint8(v))
		return
	case a >= 0x140400 && a < 0x140800:
		b.k051960.Write(int(a), uint8(v>>8))
		b.k051960.Write(int(a)+1, uint8(v))
		return
	case a >= 0x140000 && a < 0x140008:
		r := int(a & 7)
		b.k051960.WriteReg(r, uint8(v>>8))
		b.k051960.WriteReg(r+1, uint8(v))
		return
	}
	m.Write8(a|1, uint8(v))
}

func (m tmntMain) Write8(a uint32, v uint8) {
	b := m.b
	switch {
	case a >= 0x60000 && a < 0x64000:
		b.ram[a-0x60000] = v
		return
	case a >= 0x80000 && a < 0x81000:
		if a&1 != 0 {
			b.palWrite(int(a-0x80000)>>1, v)
		}
		return
	case a >= 0x100000 && a < 0x108000:
		o := vOff(a)
		if a&1 != 0 {
			o += 0x2000
		}
		b.k052109.Write(o, v)
		return
	case a >= 0x140000 && a < 0x140008:
		b.k051960.WriteReg(int(a&7), v)
		return
	case a >= 0x140400 && a < 0x140800:
		b.k051960.Write(int(a), v)
		return
	}
	if a&1 == 0 {
		return
	}
	switch a {
	case 0xa0001:
		b.ctrlWrite(int(v))
	case 0xa0009:
		b.soundLatch = int(v)
	case 0xc0001:
		b.priority = int(v)
	}
}

func (m tmntMain) Ack(level int) int {
	m.b.cpu.SetIRQ(0)
	return -1
}

func (b *tmnt) ctrlWrite(v int) {
	old := b.ctrl
	b.ctrl = v
	if old&8 != 0 && v&8 == 0 {
		b.syncZ80()
		b.z80.IRQ = true
	}
	if v&0x20 == 0 {
		b.cpu.SetIRQ(0)
	}
	b.k052109.Rmrd = v&0x80 != 0
}

func (b *tmnt) palWrite(i int, v uint8) {
	b.palRam[i] = v
	n := i >> 1
	c := int(b.palRam[n*2])<<8 | int(b.palRam[n*2+1])
	r, g, bl := c&31, (c>>5)&31, (c>>10)&31
	R, G, B := r<<3|r>>2, g<<3|g>>2, bl<<3|bl>>2
	b.palette[n] = arcade.RGB(R, G, B)
	b.shade[n] = arcade.RGBf(float64(R)*0.75, float64(G)*0.75, float64(B)*0.75)
}

func (s tmntSound) Read(addr uint16) uint8 {
	b, a := s.b, int(addr)
	if a < 0x8000 {
		return b.zrom[a]
	}
	if a < 0x8800 {
		return b.zram[a-0x8000]
	}
	if a >= 0x8800 && a < 0x9000 {
		return 0xff
	}
	switch a & 0xf000 {
	case 0x9000:
		return uint8(b.sres)
	case 0xa000:
		return uint8(b.soundLatch)
	case 0xb000:
		if a&0xf < 14 {
			return b.k007232.Read(a & 0xf)
		}
		return 0xff
	case 0xc000:
		if a&1 != 0 {
			return b.ym.Read()
		}
		return 0xff
	case 0xf000:
		if b.upd.Busy() {
			return 0
		}
		return 1
	}
	return 0xff
}

func (s tmntSound) Write(addr uint16, v uint8) {
	b, a := s.b, int(addr)
	if a >= 0x8000 && a < 0x8800 {
		b.zram[a-0x8000] = v
		return
	}
	switch a & 0xf000 {
	case 0x9000:
		old := b.sres
		b.sres = int(v)
		b.upd.ResetLine(v&2 != 0)
		if v&4 != 0 && old&4 == 0 {
			b.titlePos, b.titleAcc = 0, 0
		} else if v&4 == 0 {
			b.titlePos = -1
		}
	case 0xb000:
		if a&0xf < 14 {
			b.k007232.Write(a&0xf, v)
		}
	case 0xc000:
		b.ym.Write(a&1, v)
	case 0xd000:
		b.upd.Port(v)
	case 0xe000:
		b.upd.Start(v&1 != 0)
	}
}

func (s tmntSound) In(uint16) uint8   { return 0xff }
func (s tmntSound) Out(uint16, uint8) {}
func (s tmntSound) Ack()              { s.b.z80.IRQ = false }

func (b *tmnt) syncZ80() {
	b.runZ80To(float64(min(b.cpu.Cycles, int64(tmntLines*tmntLineCyc))) / float64(tmntLineCyc) * tmntZ80Line)
}

func (b *tmnt) runZ80To(target float64) {
	for b.zcyc < target {
		c := b.z80.Step()
		b.zcyc += float64(c)
		b.soundClocks(c)
	}
}

func (b *tmnt) soundClocks(c int) {
	b.mixClk += c
	ym, k, upd := b.ym, b.k007232, b.upd
	for b.mixClk >= 64 {
		b.mixClk -= 64
		ym.Tick()
		k.Clocks(64)
		if k.AccN != 0 {
			b.kL = k.Acc[0] / float64(k.AccN)
			k.Acc[0], k.Acc[1], k.AccN = 0, 0, 0
		}
		b.updAcc += tmntUPDStep
		n := math.Floor(b.updAcc)
		b.updAcc -= n
		if n != 0 {
			upd.Clocks(int(n))
			if upd.AccN != 0 {
				b.updV = upd.Acc / float64(upd.AccN)
				upd.Acc, upd.AccN = 0, 0
			}
		}
		title := 0.0
		if b.titlePos >= 0 {
			if b.titlePos < len(b.title) {
				title = float64(b.title[b.titlePos])
			}
			b.titleAcc += tmntTitStep
			for b.titleAcc >= 1 {
				b.titleAcc--
				b.titlePos++
			}
			if b.titlePos >= len(b.title) {
				b.titlePos = -1
			}
		}
		fm := float64(ym.Out[0]+ym.Out[1]) / 2 / 16384
		u := b.updV
		b.updDC = u - b.updPrev + tmntDCKeep*b.updDC
		b.updPrev = u
		v := fm*0.62 + b.kL*0.235 + b.updDC*0.255 + title*0.32
		if v > 0.75 {
			v = 0.75 + 0.2*math.Tanh((v-0.75)*5)
		} else if v < -0.75 {
			v = -0.75 - 0.2*math.Tanh((-v-0.75)*5)
		}
		b.out.Add(v)
	}
}

func (b *tmnt) RunFrame() {
	cpu := b.cpu
	for line := 0; line < tmntLines; line++ {
		b.line = line
		if line == tmntVblank {
			b.draw()
			if b.ctrl&0x20 != 0 {
				cpu.SetIRQ(5)
			}
		}
		cpu.Run(int64((line + 1) * tmntLineCyc))
		b.runZ80To(float64(line+1) * tmntZ80Line)
	}
	cpu.Cycles -= tmntLines * tmntLineCyc
	b.zcyc -= float64(tmntLines) * tmntZ80Line
	b.frame++
}

func (b *tmnt) TakeAudio() []float32 { return b.out.Take() }
func (b *tmnt) Screen() []uint32     { return b.screen }

func (b *tmnt) draw() {
	k, s := b.k052109, b.k051960
	opt := konami.RenderOpts{X0: tmntMapX0, Y0: tmntMapY0}
	clear(b.bufA)
	clear(b.bufF)
	clear(b.bufS)
	clear(b.bufSh)
	k.Render(2, b.bufB, tmntW, tmntH, konami.RenderOpts{X0: tmntMapX0, Y0: tmntMapY0, Opaque: true})
	k.Render(1, b.bufA, tmntW, tmntH, opt)
	k.Render(0, b.bufF, tmntW, tmntH, opt)
	s.Render(b.bufS, tmntW, tmntH, konami.K051960RenderOpts{X0: tmntMapX0, Y0: tmntMapY0, Shadow: b.bufSh})
	prom, latch := &b.prioProm, (b.priority&4)<<4
	B, A, F, S, Sh := b.bufB, b.bufA, b.bufF, b.bufS, b.bufSh
	last := -1
	if s.FlipScreen() {
		last = tmntW*tmntH - 1
	}
	for i := 0; i < tmntW*tmntH; i++ {
		j := i
		if last >= 0 {
			j = last - i
		}
		sp, a, f := S[j], A[i], F[i]
		idx := 2 | int(Sh[j])<<4 | latch
		if a&15 != 0 {
			idx |= 1
		}
		if sp&15 != 0 {
			idx |= 4
		}
		if f&15 != 0 {
			idx |= 8
		}
		o := prom[idx&255]
		var c uint16
		switch o & 3 {
		case 0:
			c = a
		case 1:
			c = B[i]
		case 2:
			c = sp
		default:
			c = f
		}
		if o&4 != 0 {
			b.screen[i] = b.palette[c&1023]
		} else {
			b.screen[i] = b.shade[c&1023]
		}
	}
}

const tmntStateMagic = "TMNT1"

func (b *tmnt) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(tmntStateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.B = b.z80.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.palRam[:])
	w.Bytes(b.zram[:])
	b.k052109.AppendState(w)
	b.k051960.AppendState(w)
	b.k007232.AppendState(w)
	b.upd.AppendState(w)
	w.B = b.ym.AppendState(w.B)
	w.Ints([]int{b.ctrl, b.priority, b.soundLatch, b.sres, b.titlePos, b.mixClk, b.frame})
	w.F64s([]float64{b.titleAcc, b.updAcc, b.zcyc, b.updDC, b.updPrev, b.updV, b.kL})
	b.out.AppendState(w)
	return w.B, nil
}

func (b *tmnt) LoadState(data []byte) (err error) {
	if len(data) < len(tmntStateMagic) || string(data[:len(tmntStateMagic)]) != tmntStateMagic {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: data[len(tmntStateMagic):]}
	if r.B, err = b.cpu.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.z80.LoadState(r.B); err != nil {
		return err
	}
	r.Bytes(b.ram[:])
	r.Bytes(b.palRam[:])
	r.Bytes(b.zram[:])
	b.k052109.LoadState(r)
	b.k051960.LoadState(r)
	b.k007232.LoadState(r)
	b.upd.LoadState(r)
	if r.Err == nil {
		if r.B, err = b.ym.LoadState(r.B); err != nil {
			return err
		}
	}
	m := make([]int, 7)
	r.Ints(m)
	f := make([]float64, 7)
	r.F64s(f)
	b.out.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	b.ctrl, b.priority, b.soundLatch, b.sres, b.titlePos, b.mixClk, b.frame = m[0], m[1], m[2], m[3], m[4], m[5], m[6]
	b.titleAcc, b.updAcc, b.zcyc, b.updDC, b.updPrev, b.updV, b.kL = f[0], f[1], f[2], f[3], f[4], f[5], f[6]
	b.k052109.Rmrd = b.ctrl&0x80 != 0
	for i := 0; i < 0x800; i += 2 {
		b.palWrite(i, b.palRam[i])
	}
	b.out.Clear()
	b.draw()
	return nil
}

var tmntDips = []arcade.Dip{
	{ID: "coinage", Label: "Coins", Default: 0xff, Options: []arcade.DipOption{{Label: "1 coin, 1 credit", Value: 0xff}, {Label: "1 coin, 2 credits", Value: 0xee},
		{Label: "2 coins, 1 credit", Value: 0x22}, {Label: "Free play", Value: 0x00}}},
	{ID: "lives", Label: "Lives", Default: 2, Options: []arcade.DipOption{{Label: "1", Value: 3}, {Label: "2", Value: 2}, {Label: "3", Value: 1}, {Label: "5", Value: 0}}},
	{ID: "difficulty", Label: "Difficulty", Default: 2, Options: []arcade.DipOption{{Label: "Easy", Value: 3}, {Label: "Normal", Value: 2}, {Label: "Hard", Value: 1}, {Label: "Very hard", Value: 0}}},
	{ID: "demo", Label: "Sound in attract mode", Default: 1, Options: []arcade.DipOption{{Label: "On", Value: 1}, {Label: "Off", Value: 0}}},
	{ID: "flip", Label: "Flip screen", Default: 0, Options: []arcade.DipOption{{Label: "Off", Value: 0}, {Label: "On", Value: 1}}},
	{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: []arcade.DipOption{{Label: "Off", Value: 0}, {Label: "On", Value: 1}}},
}

func tmntGames() []arcade.Game {
	// (the JS list's g19 mixing PROM entry sits inside a comment, so it isn't
	// part of the set there and the board works the mixing out by rule; kept so)
	common := []arcade.File{
		{Name: "963e20.g13", Size: 0x8000, Role: "audio", Offset: arcade.Next},
		{Name: "963a28.h27", Size: 0x80000, Role: "tiles_a", Offset: arcade.Next}, {Name: "963a29.k27", Size: 0x80000, Role: "tiles_b", Offset: arcade.Next},
		{Name: "963a17.h4", Size: 0x80000, Role: "sprites_a", Offset: arcade.Next}, {Name: "963a15.k4", Size: 0x80000, Role: "sprites_b", Offset: arcade.Next},
		{Name: "963a18.h6", Size: 0x80000, Role: "sprites_c", Offset: arcade.Next}, {Name: "963a16.k6", Size: 0x80000, Role: "sprites_d", Offset: arcade.Next},
		{Name: "963a30.g7", Size: 0x100, Role: "prom_g7", Offset: arcade.Next},
		{Name: "963a26.c13", Size: 0x20000, Role: "pcm", Offset: arcade.Next}, {Name: "963a27.d18", Size: 0x20000, Role: "speech", Offset: arcade.Next},
		{Name: "963a25.d5", Size: 0x80000, Role: "title", Offset: arcade.Next},
	}
	version := func(id, title, v, parent string, players int) arcade.Game {
		files := []arcade.File{
			{Name: fmt.Sprintf("963-%s23.j17", v), Size: 0x20000, Role: "cpu_even", Offset: arcade.Next},
			{Name: fmt.Sprintf("963-%s24.k17", v), Size: 0x20000, Role: "cpu_odd", Offset: arcade.Next},
			{Name: fmt.Sprintf("963-%s21.j15", v), Size: 0x10000, Role: "cpu2_even", Offset: arcade.Next},
			{Name: fmt.Sprintf("963-%s22.k15", v), Size: 0x10000, Role: "cpu2_odd", Offset: arcade.Next},
		}
		return arcade.Game{ID: id, Title: title, Year: 1989, Maker: "Konami", Loose: true, Parent: parent, Players: players,
			Files: append(files, common...)}
	}
	return []arcade.Game{
		version("tmnt", "Teenage Mutant Ninja Turtles (World, 4 players)", "x", "", 0),
		version("tmntu", "Teenage Mutant Ninja Turtles (US, 4 players)", "r", "tmnt", 0),
		version("tmntua", "Teenage Mutant Ninja Turtles (US, 4 players, older)", "n", "tmnt", 0),
		version("tmht", "Teenage Mutant Hero Turtles (UK, 4 players)", "f", "tmnt", 0),
		version("tmntj", "Teenage Mutant Ninja Turtles (Japan, 4 players)", "2", "tmnt", 0),
		version("tmht2p", "Teenage Mutant Hero Turtles (UK, 2 players)", "u", "tmnt", 2),
		version("tmnt2pj", "Teenage Mutant Ninja Turtles (Japan, 2 players)", "1", "tmnt", 2),
	}
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "tmnt", Title: "Konami TMNT", Width: tmntW, Height: tmntH, Rotate: 0, FPS: tmntFPS, Players: 4,
		Characters: []string{"Leonardo", "Michelangelo", "Donatello", "Raphael"},
		Buttons:    []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:       tmntDips, Games: tmntGames(), New: newTMNT,
	})
}
