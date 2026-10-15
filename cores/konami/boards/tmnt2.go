package boards

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/konami"
	"retropixels/cores/m68k"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// ---- TMNT2: Turtles in Time (1991): see arcade_tmnt2.js ----

const (
	t2LineCyc  = 1024
	t2Lines    = 264
	t2W        = 304
	t2H        = 224
	t2Top      = 16
	t2Left     = 104
	t2NMIDelay = 400
)

var (
	t2CPUHz   = float64(16000000)
	t2Z80Hz   = float64(8000000)
	t2YMHz    = float64(3579545)
	t2PCMGain = 1.5
	t2FPS     = t2CPUHz / (float64(t2LineCyc) * float64(t2Lines))
	t2SndCyc  = float64(t2LineCyc) * t2Z80Hz / t2CPUHz // Z80 cycles a line (512)
)

// t2LUT[(shadow ? 16 : 0) + dim][v]: brightness tables.
var t2LUT [32][256]uint8

func init() {
	for k := 0; k < 32; k++ {
		num := 5 * (20 - (k & 15))
		if k&16 != 0 {
			num = 3 * (20 - (k & 15))
		}
		for v := 0; v < 256; v++ {
			t2LUT[k][v] = uint8(v * num / 100)
		}
	}
}

// interleaveRole gathers files into one region: each file's bytes go `width`
// at a time, every `stride` bytes (File.Offset is the JS's base, Lane its lane).
func interleaveRole(g *arcade.Game, files arcade.Files, role string, width, stride int) []byte {
	size := 0
	base := func(f *arcade.File) int {
		if f.Offset == arcade.Next {
			return 0
		}
		return f.Offset
	}
	for i := range g.Files {
		if f := &g.Files[i]; f.Role == role {
			size = max(size, base(f)+f.Size*stride/width)
		}
	}
	out := make([]byte, size)
	for i := range out {
		out[i] = 0xff
	}
	for i := range g.Files {
		f := &g.Files[i]
		if f.Role != role {
			continue
		}
		d := arcade.FindFile(f, files)
		if d == nil {
			continue
		}
		for i, o := 0, base(f)+f.Lane; i < len(d); i, o = i+width, o+stride {
			for k := 0; k < width; k++ {
				if o+k < len(out) && i+k < len(d) {
					out[o+k] = d[i+k]
				}
			}
		}
	}
	return out
}

// byteEeprom: the board's byte-wide serial EEPROM (see arcade_tmnt2.js).
type byteEeprom struct {
	bytes                         [128]uint8
	cs, clk, dout                 int
	state, shift, bits, addr, out int
	enabled, all, erasing         bool
}

func newByteEeprom(initial []byte) *byteEeprom {
	e := &byteEeprom{dout: 1}
	for i := range e.bytes {
		e.bytes[i] = 0xff
	}
	copy(e.bytes[:], initial)
	return e
}

func (e *byteEeprom) read() int { return e.dout }

func (e *byteEeprom) write(csB, clkB, diB bool) {
	cs, clk, di := bit(csB), bit(clkB), bit(diB)
	if cs == 0 {
		if e.cs != 0 && e.state == 3 && e.bits == 0 && e.erasing {
			e.store(0xff)
		}
		e.cs, e.clk, e.state, e.dout = 0, clk, 0, 1
		return
	}
	if e.cs == 0 {
		e.state, e.dout = 0, 1
	}
	rising := clk != 0 && e.clk == 0
	e.cs, e.clk = 1, clk
	if !rising {
		return
	}
	switch e.state {
	case 0:
		if di != 0 {
			e.state, e.shift, e.bits = 1, 0, 0
		}
	case 1:
		e.shift = e.shift<<1 | di
		if e.bits++; e.bits == 11 {
			e.command(e.shift>>9, e.shift&0x1ff)
		}
	case 2:
		e.dout = (e.out >> 7) & 1
		e.out = (e.out << 1) & 0xff
		if e.bits++; e.bits == 8 {
			e.bits = 0
			e.addr = (e.addr + 1) & 0x7f
			e.out = int(e.bytes[e.addr])
		}
	case 3:
		e.shift = (e.shift<<1 | di) & 0xff
		if e.bits++; e.bits == 8 {
			e.store(e.shift)
			e.state, e.dout = 4, 1
		}
	}
}

func (e *byteEeprom) store(v int) {
	if !e.enabled {
		return
	}
	if e.all {
		for i := range e.bytes {
			e.bytes[i] = uint8(v)
		}
	} else {
		e.bytes[e.addr] = uint8(v)
	}
}

func (e *byteEeprom) command(op, a int) {
	e.addr, e.all, e.erasing = a&0x7f, false, false
	if op == 2 {
		e.state, e.dout, e.out, e.bits = 2, 0, int(e.bytes[e.addr]), 0
		return
	}
	if op == 1 || op == 3 {
		e.state, e.shift, e.bits, e.erasing = 3, 0, 0, op == 3
		return
	}
	switch a >> 7 {
	case 0:
		e.enabled = false
	case 1:
		e.state, e.shift, e.bits, e.all = 3, 0, 0, true
		return
	case 2:
		if e.enabled {
			for i := range e.bytes {
				e.bytes[i] = 0xff
			}
		}
	case 3:
		e.enabled = true
	}
	e.state, e.dout = 4, 1
}

func (e *byteEeprom) appendState(w *arcade.StateW) {
	w.Bytes(e.bytes[:])
	w.Ints([]int{e.cs, e.clk, e.dout, e.state, e.shift, e.bits, e.addr, e.out, bit(e.enabled), bit(e.all), bit(e.erasing)})
}

func (e *byteEeprom) loadState(r *arcade.StateR) {
	r.Bytes(e.bytes[:])
	v := make([]int, 11)
	r.Ints(v)
	e.cs, e.clk, e.dout, e.state, e.shift, e.bits, e.addr, e.out = v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]
	e.enabled, e.all, e.erasing = v[8] != 0, v[9] != 0, v[10] != 0
}

func bit(b bool) int {
	if b {
		return 1
	}
	return 0
}

func allFF(b []byte) bool {
	for _, x := range b {
		if x != 0xff {
			return false
		}
	}
	return true
}

type tmnt2 struct {
	game       *arcade.Game
	sampleRate int
	rom        []byte
	sndRom     []byte
	ram        [0x4000]uint8
	palRam     [0x1000]uint8
	sprRam     [0x4000]uint8
	ioRam      [0x80]uint8
	prot       [16]uint16
	sndRam     [0x800]uint8
	palette    [2048]uint32
	screen     []uint32

	layerBase  [3]int
	spriteBase int
	k052109    *konami.K052109
	k053245    *konami.K053245
	k053251    *konami.K053251
	k053260    *konami.K053260
	ym         *ym2151.YM2151
	eeprom     *byteEeprom
	cpu        *m68k.CPU
	z80        *z80.CPU

	layerPens            [3][]uint16
	sprPens              []uint16
	sprPri, sprShadow    []uint8
	mixPens              []uint16
	mixShade             []uint8
	inputs               []arcade.Pad
	resampler            *arcade.Resampler
	pcmBuf               []float32
	eepromOut, ctrl300   int
	vblank               bool
	dmaBusy, setupFrames int
	nmiAt, zCyc, ymFrac  float64
	cyc                  int
}

type t2Main struct{ b *tmnt2 }
type t2Sound struct{ b *tmnt2 }

func newTMNT2(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &tmnt2{game: g, sampleRate: opts.SampleRate}
	if b.sampleRate == 0 {
		b.sampleRate = 48000
	}
	b.rom = make([]byte, 0x100000)
	for i := range b.rom {
		b.rom[i] = 0xff
	}
	copy(b.rom, interleaveRole(g, files, "cpu", 1, 2))
	b.sndRom = arcade.Region(g, files, "sound", 0x10000)
	tileRom := interleaveRole(g, files, "tiles", 2, 4)
	spriteRom := unscrambleSprites(interleaveRole(g, files, "sprites", 2, 4))
	pcm := arcade.Region(g, files, "pcm", 0x200000)
	b.screen = make([]uint32, t2W*t2H)

	b.k052109 = konami.NewK052109(konami.K052109Opts{
		ROM: tileRom,
		TileCallback: func(t *konami.Tile) {
			c := t.Color
			t.Code |= (c&0x03)<<8 | (c&0x10)<<6 | (c&0x0c)<<9 | t.Bank<<13
			t.Color = b.layerBase[t.Layer] + (c&0xe0)>>5
		},
	})
	b.k053245 = konami.NewK053245(konami.K053245Opts{
		Type: "053245", ROM: spriteRom, Layout: "planar", DX: -11, DY: -23,
		SpriteCallback: func(t *konami.Sprite2) {
			t.Pri = 0x20 | (t.Attr&0x60)>>2
			t.Color = b.spriteBase + t.Attr&0x1f
		},
	})
	b.k052109.DX = [3]int{0, -6, -6}
	b.k053251 = &konami.K053251{}
	b.k053260 = konami.NewK053260(pcm, t2YMHz)
	b.ym = ym2151.New(t2YMHz)
	var nv []byte
	if len(opts.SRAM) >= 128 {
		nv = opts.SRAM
	}
	if nv == nil {
		if d := roleFileIfPresent(g, files, "eeprom"); len(d) >= 128 {
			nv = d
		}
	}
	b.eeprom = newByteEeprom(nv)
	b.cpu = m68k.New(t2Main{b})
	b.z80 = z80.New(t2Sound{b})
	n := t2W * t2H
	for i := range b.layerPens {
		b.layerPens[i] = make([]uint16, n)
	}
	b.sprPens, b.sprPri, b.sprShadow = make([]uint16, n), make([]uint8, n), make([]uint8, n)
	b.mixPens, b.mixShade = make([]uint16, n), make([]uint8, n)
	b.resampler = arcade.NewResampler(t2YMHz/64, float64(b.sampleRate))
	b.pcmBuf = make([]float32, 64)
	b.Reset()
	return b, nil
}

// roleFileIfPresent: the first file of a role when it's in the set.
func roleFileIfPresent(g *arcade.Game, files arcade.Files, role string) []byte {
	for i := range g.Files {
		if f := &g.Files[i]; f.Role == role {
			return arcade.FindFile(f, files)
		}
	}
	return nil
}

func (b *tmnt2) Reset() {
	b.ram, b.palRam, b.sprRam, b.ioRam = [0x4000]uint8{}, [0x1000]uint8{}, [0x4000]uint8{}, [0x80]uint8{}
	b.prot, b.sndRam = [16]uint16{}, [0x800]uint8{}
	b.palette = [2048]uint32{}
	b.k052109.Reset()
	b.k053245.Reset()
	b.k053251.Reset()
	b.ym.Reset()
	b.k053260.Reset()
	b.eepromOut, b.ctrl300, b.vblank, b.dmaBusy = 0x10, 0, false, 0
	b.setupFrames = 0
	if allFF(b.eeprom.bytes[:]) {
		b.setupFrames = 60
	}
	b.cpu.SetIRQ(0)
	b.cpu.Reset()
	b.z80.Reset()
	b.z80.IRQ = false
	b.nmiAt = -1
	b.zCyc, b.ymFrac, b.cyc = 0, 0, 0
	b.resampler.Clear()
}

func (b *tmnt2) SetInput(pads []arcade.Pad) { b.inputs = pads }

func (b *tmnt2) pad(p int) arcade.Pad {
	if p < len(b.inputs) {
		return b.inputs[p]
	}
	return nil
}

func (b *tmnt2) playerByte(p int) int {
	i := b.pad(p)
	v := 0
	for k, n := range []string{"Left", "Right", "Up", "Down", "Button 1", "Button 2", "Button 3"} {
		if i[n] {
			v |= 1 << k
		}
	}
	if i["Start (1P)"] || (p == 1 && b.pad(0)["Start (2P)"]) {
		v |= 0x80
	}
	return ^v & 0xff
}

func (b *tmnt2) coinByte() int {
	v := 0
	for p := 0; p < 4; p++ {
		i := b.pad(p)
		if i["Coin"] || (p == 1 && b.pad(0)["Coin 2"]) {
			v |= 1 << p
		}
		if i["Service"] {
			v |= 0x10 << p
		}
	}
	return ^v & 0xff
}

func (b *tmnt2) eepromByte() int {
	p0 := b.pad(0)
	test := (p0["Service"] && p0["Start (1P)"]) || b.setupFrames > 0
	v := 0x70 | b.eeprom.read() | 2
	if b.dmaBusy > 0 {
		v |= 4
	}
	if b.vblank {
		v |= 8
	}
	if !test {
		v |= 0x80
	}
	return v
}

func (m t2Main) Read16(a uint32) uint16 {
	b := m.b
	a &= 0xffffff
	switch {
	case a < 0x100000:
		return uint16(b.rom[a])<<8 | uint16(b.rom[a+1])
	case a >= 0x104000 && a < 0x108000:
		o := a - 0x104000
		return uint16(b.ram[o])<<8 | uint16(b.ram[o+1])
	case a >= 0x140000 && a < 0x141000:
		o := a - 0x140000
		return uint16(b.palRam[o])<<8 | uint16(b.palRam[o+1])
	case a >= 0x180000 && a < 0x184000:
		o := a - 0x180000
		return uint16(b.sprRam[o])<<8 | uint16(b.sprRam[o+1])
	case a >= 0x600000 && a < 0x604000:
		o := int(a-0x600000) >> 1
		return uint16(b.k052109.Read(o))<<8 | uint16(b.k052109.Read(o+0x2000))
	}
	return uint16(m.Read8(a))<<8 | uint16(m.Read8(a+1))
}

func (m t2Main) Read8(a uint32) uint8 {
	b := m.b
	a &= 0xffffff
	switch {
	case a < 0x100000:
		return b.rom[a]
	case a >= 0x104000 && a < 0x108000:
		return b.ram[a-0x104000]
	case a >= 0x140000 && a < 0x141000:
		return b.palRam[a-0x140000]
	case a >= 0x180000 && a < 0x184000:
		return b.sprRam[a-0x180000]
	case a >= 0x600000 && a < 0x604000:
		o := int(a-0x600000) >> 1
		if a&1 != 0 {
			o += 0x2000
		}
		return b.k052109.Read(o)
	case a >= 0x1c0000 && a < 0x1c0008:
		if a&1 != 0 {
			return uint8(b.playerByte(int(a-0x1c0000) >> 1))
		}
		return 0xff
	case a == 0x1c0101:
		return uint8(b.coinByte())
	case a == 0x1c0103:
		return uint8(b.eepromByte())
	case a >= 0x1c0500 && a < 0x1c0580:
		return b.ioRam[a-0x1c0500]
	case a >= 0x1c0800 && a < 0x1c0820:
		w := b.prot[(a-0x1c0800)>>1]
		if a&1 != 0 {
			return uint8(w)
		}
		return uint8(w >> 8)
	case a >= 0x5a0000 && a < 0x5a0020:
		return b.k053245.RegRead(int((a&0x1c)>>1 | a&1))
	case a >= 0x5c0600 && a < 0x5c0604:
		if a&1 != 0 {
			return b.k053260.MainRead(int(a>>1) & 1)
		}
		return 0
	case a >= 0x5c0700 && a < 0x5c0720:
		if a&1 != 0 {
			return b.k053251.Read(int(a>>1) & 15)
		}
		return 0
	}
	return 0xff
}

func (m t2Main) Write16(a uint32, v uint16) {
	b := m.b
	a &= 0xffffff
	switch {
	case a >= 0x104000 && a < 0x108000:
		o := a - 0x104000
		b.ram[o], b.ram[o+1] = uint8(v>>8), uint8(v)
		return
	case a >= 0x140000 && a < 0x141000:
		o := a - 0x140000
		b.palRam[o], b.palRam[o+1] = uint8(v>>8), uint8(v)
		b.updateColour(int(o >> 1))
		return
	case a >= 0x180000 && a < 0x184000:
		o := a - 0x180000
		b.sprRam[o], b.sprRam[o+1] = uint8(v>>8), uint8(v)
		b.spriteWord(int(o >> 1))
		return
	case a >= 0x600000 && a < 0x604000:
		o := int(a-0x600000) >> 1
		b.k052109.Write(o, uint8(v>>8))
		b.k052109.Write(o+0x2000, uint8(v))
		return
	case a >= 0x1c0800 && a < 0x1c0820:
		b.protWrite(int(a-0x1c0800)>>1, v, true)
		return
	}
	m.Write8(a, uint8(v>>8))
	m.Write8(a+1, uint8(v))
}

func (m t2Main) Write8(a uint32, v uint8) {
	b := m.b
	a &= 0xffffff
	switch {
	case a >= 0x104000 && a < 0x108000:
		b.ram[a-0x104000] = v
	case a >= 0x140000 && a < 0x141000:
		b.palRam[a-0x140000] = v
		b.updateColour(int(a-0x140000) >> 1)
	case a >= 0x180000 && a < 0x184000:
		b.sprRam[a-0x180000] = v
		b.spriteWord(int(a-0x180000) >> 1)
	case a >= 0x600000 && a < 0x604000:
		o := int(a-0x600000) >> 1
		if a&1 != 0 {
			o += 0x2000
		}
		b.k052109.Write(o, v)
	case a >= 0x1c0500 && a < 0x1c0580:
		b.ioRam[a-0x1c0500] = v
	case a >= 0x1c0800 && a < 0x1c0820:
		i := int(a-0x1c0800) >> 1
		if a&1 != 0 {
			b.protWrite(i, b.prot[i]&0xff00|uint16(v), false)
		} else {
			b.protWrite(i, b.prot[i]&0xff|uint16(v)<<8, true)
		}
	case a == 0x1c0201:
		b.eepromOut = int(v)
		b.eeprom.write(v&2 != 0, v&4 != 0, v&1 != 0)
	case a == 0x1c0301:
		b.ctrl300 = int(v)
		b.k052109.Rmrd = v&0x08 != 0
	case a >= 0x5a0000 && a < 0x5a0020:
		r := int((a&0x1c)>>1 | a&1)
		if r == 6 {
			b.dmaBusy = 2
		}
		b.k053245.RegWrite(r, v)
	case a >= 0x5c0600 && a < 0x5c0604:
		if a&1 != 0 {
			b.k053260.MainWrite(int(a>>1)&1, v)
		}
	case a == 0x5c0604 || a == 0x5c0605:
		b.z80.IRQ = true
	case a >= 0x5c0700 && a < 0x5c0720:
		if a&1 != 0 {
			b.k053251.Write(int(a>>1)&15, v)
		}
	}
}

func (m t2Main) Ack(level int) int {
	m.b.cpu.SetIRQ(0)
	return -1
}

func (b *tmnt2) updateColour(i int) {
	v := int(b.palRam[i*2])<<8 | int(b.palRam[i*2+1])
	c5 := func(x int) int { return x<<3 | x>>2 }
	b.palette[i] = arcade.RGB(c5(v&31), c5((v>>5)&31), c5((v>>10)&31))
}

func (b *tmnt2) spriteWord(w int) {
	if w&0x31 != 0 {
		return
	}
	k := (w&0x0e)>>1 | (w&0x1fc0)>>3
	b.k053245.WriteWord(k, int(b.sprRam[w*2])<<8|int(b.sprRam[w*2+1]))
}

func s16(x int) int { return int(int16(uint16(x))) }

// scalePart: how far a part sits from its object's centre once the object is zoomed.
func scalePart(off, zoom int) int {
	d := zoom - 0x4f00
	if d > 0 {
		return off + int(math.Trunc(math.Float64frombits(pow189Bits[(d>>8)&255])*float64(off)/600))
	}
	if d < 0 {
		k := zoom + int(math.Floor(float64(d*15)/64))
		if k > 0 {
			return int(math.Trunc(float64(off*k) / 0x4f00))
		}
		return 0
	}
	return off
}

// protWrite: the sprite helper chip (see arcade_tmnt2.js).
func (b *tmnt2) protWrite(i int, v uint16, hi bool) {
	b.prot[i] = v
	if i != 12 || !hi {
		return
	}
	P := &b.prot
	if P[8]&0xff00 != 0x8200 {
		return
	}
	main := t2Main{b}
	src := (int(P[1]&0xff)<<16 | int(P[0])) & 0xfffffe
	dst := (int(P[3]&0xff)<<16 | int(P[2])) & 0xfffffe
	mod := (int(P[5]&0xff)<<16 | int(P[4])) & 0xfffffe
	zLock := P[8]&0xff == 1
	rw := func(base, k int) int { return int(main.Read16(uint32(base + k*2))) }
	code, attr := rw(src, 0), rw(src, 1)
	localX, localY := s16(rw(src, 2)), s16(rw(src, 3))
	flags := rw(mod, 0)
	w0 := (attr >> 2) & 0x3f00
	w6 := (attr & 0x380) | (flags & 0x60)
	keepAspect := flags&0x14 == 0x14
	if flags&0x8000 != 0 {
		w0 |= 0x8000
	}
	if keepAspect {
		w0 |= 0x4000
	}
	if flags&0x4000 != 0 {
		w0 ^= 0x1000
		localX = -localX
	}
	partColour, objColour := attr&0x1f, rw(mod, 0x15)>>8
	if partColour != 0x0f && objColour <= 0x1f && !zLock {
		w6 |= objColour
	} else {
		w6 |= partColour
	}
	zoomX := rw(mod, 0x0e)
	zoomY := zoomX
	if !keepAspect {
		zoomY = rw(mod, 0x0f)
	}
	locked := flags&0x20 != 0 && (zoomX == 0 || zoomX == 0x100)
	if !locked {
		localX, localY = scalePart(localX, zoomX), scalePart(localY, zoomY)
	}
	if !zLock {
		localY += s16(rw(mod, 8))
	}
	x, y := localX+s16(rw(mod, 6)), localY+s16(rw(mod, 7))
	main.Write16(uint32(dst), uint16(w0))
	main.Write16(uint32(dst+4), uint16(code))
	main.Write16(uint32(dst+8), uint16(y&0xffff))
	main.Write16(uint32(dst+12), uint16(x&0xffff))
	main.Write16(uint32(dst+24), uint16(w6))
}

func (s t2Sound) Read(addr uint16) uint8 {
	b, a := s.b, int(addr)
	switch {
	case a < 0xf000:
		return b.sndRom[a]
	case a < 0xf800:
		return b.sndRam[a-0xf000]
	case a == 0xf801:
		return b.ym.Read()
	case a >= 0xfa00 && a < 0xfa30:
		return b.k053260.Read(a - 0xfa00)
	}
	return 0xff
}

func (s t2Sound) Write(addr uint16, v uint8) {
	b, a := s.b, int(addr)
	switch {
	case a >= 0xf000 && a < 0xf800:
		b.sndRam[a-0xf000] = v
	case a == 0xf800 || a == 0xf801:
		b.ym.Write(a&1, v)
	case a >= 0xfa00 && a < 0xfa30:
		b.k053260.Write(a-0xfa00, v)
	case a == 0xfc00:
		b.nmiAt = b.zCyc + t2NMIDelay
	}
}

func (s t2Sound) In(uint16) uint8   { return 0xff }
func (s t2Sound) Out(uint16, uint8) {}
func (s t2Sound) Ack()              { s.b.z80.IRQ = false }

func (b *tmnt2) runSound(cycles float64) {
	end := b.zCyc + cycles
	for b.zCyc < end {
		if b.nmiAt >= 0 && b.zCyc >= b.nmiAt {
			b.nmiAt = -1
			b.z80.NMI()
		}
		b.zCyc += float64(b.z80.Step())
	}
	b.ymFrac += cycles * (t2YMHz / 64) / t2Z80Hz
	n := int(math.Floor(b.ymFrac))
	b.ymFrac -= float64(n)
	if n > len(b.pcmBuf) {
		b.pcmBuf = make([]float32, n*2)
	}
	pcm := b.pcmBuf[:n]
	clear(pcm)
	b.k053260.Mix(pcm, 0, n, t2YMHz/64)
	for i := 0; i < n; i++ {
		b.ym.Tick()
		b.resampler.Add(float64(b.ym.Out[0]+b.ym.Out[1])/65536 + float64(pcm[i])*t2PCMGain)
	}
}

func (b *tmnt2) drawFrame() {
	k51, n := b.k053251, t2W*t2H
	b.spriteBase = k51.PaletteBase(1)
	b.layerBase[0], b.layerBase[1], b.layerBase[2] = k51.PaletteBase(2), k51.PaletteBase(4), k51.PaletteBase(3)
	for l := 0; l < 3; l++ {
		clear(b.layerPens[l])
		b.k052109.Render(l, b.layerPens[l], t2W, t2H, konami.RenderOpts{X0: t2Left, Y0: t2Top})
	}
	clear(b.sprPens)
	clear(b.sprPri)
	clear(b.sprShadow)
	b.k053245.Render(b.sprPens, b.sprPri, b.sprShadow, t2W, t2H)
	k51.Mix(b.mixPens, b.mixShade, n, []konami.MixLayer{
		{CI: 1, Pens: b.sprPens, PriArr: b.sprPri, Shadow: b.sprShadow, ShadowPri: true},
		{CI: 3, Pens: b.layerPens[2], Pri: k51.Priority(3)},
		{CI: 4, Pens: b.layerPens[1], Pri: k51.Priority(4)},
		{CI: 2, Pens: b.layerPens[0], Pri: k51.Priority(2)},
	}, k51.PaletteBase(0)*16)
	r5 := k51.Read(5)
	on := r5 != 0 && r5 != 0x3e
	night := on && b.eepromOut&0x10 == 0
	d := 0
	if on {
		d = (b.ctrl300 >> 4) & 7
		if night {
			d += 8
		}
	}
	ds := 16 | d
	if night {
		ds = d - 8
	}
	for i := 0; i < n; i++ {
		pen := int(b.mixPens[i] & 2047)
		c := b.palette[pen]
		text := pen >= 512 && pen < 640
		var k int
		switch {
		case b.mixShade[i] != 0 && text:
			if !night {
				k = 16
			}
		case b.mixShade[i] != 0:
			k = ds
		case text:
			k = 0
		default:
			k = d
		}
		if k != 0 {
			T := &t2LUT[k]
			c = uint32(T[(c>>16)&255])<<16 | uint32(T[(c>>8)&255])<<8 | uint32(T[c&255])
		}
		b.screen[i] = c
	}
}

func (b *tmnt2) RunFrame() {
	cpu := b.cpu
	for line := 0; line < t2Lines; line++ {
		if line == t2Top+t2H {
			b.vblank = true
			b.drawFrame()
			if b.k052109.IRQEnabled() {
				cpu.SetIRQ(4)
			}
		} else if line == t2Top {
			b.vblank = false
		}
		end := (line + 1) * t2LineCyc
		for b.cyc < end {
			b.cyc += cpu.Step()
		}
		if b.dmaBusy > 0 {
			b.dmaBusy--
		}
		b.runSound(t2SndCyc)
	}
	b.cyc -= t2Lines * t2LineCyc
	if b.setupFrames > 0 {
		b.setupFrames--
	}
}

func (b *tmnt2) TakeAudio() []float32 { return b.resampler.Take() }
func (b *tmnt2) Screen() []uint32     { return b.screen }
func (b *tmnt2) SRAM() []byte         { return append([]byte(nil), b.eeprom.bytes[:]...) }

const t2StateMagic = "TMNT2\x01"

func (b *tmnt2) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(t2StateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.B = b.z80.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.palRam[:])
	w.Bytes(b.sprRam[:])
	w.Bytes(b.ioRam[:])
	w.U16s(b.prot[:])
	w.Bytes(b.sndRam[:])
	b.k052109.AppendState(w)
	b.k053245.AppendState(w)
	b.k053251.AppendState(w)
	b.k053260.AppendState(w)
	w.B = b.ym.AppendState(w.B)
	b.eeprom.appendState(w)
	w.Ints([]int{b.eepromOut, b.ctrl300, bit(b.vblank), b.cyc, b.dmaBusy, b.setupFrames})
	w.F64s([]float64{b.nmiAt, b.zCyc, b.ymFrac})
	b.resampler.AppendState(w)
	return w.B, nil
}

func (b *tmnt2) LoadState(data []byte) (err error) {
	if len(data) < len(t2StateMagic) || string(data[:len(t2StateMagic)]) != t2StateMagic {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: data[len(t2StateMagic):]}
	if r.B, err = b.cpu.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.z80.LoadState(r.B); err != nil {
		return err
	}
	r.Bytes(b.ram[:])
	r.Bytes(b.palRam[:])
	r.Bytes(b.sprRam[:])
	r.Bytes(b.ioRam[:])
	r.U16s(b.prot[:])
	r.Bytes(b.sndRam[:])
	b.k052109.LoadState(r)
	b.k052109.DX = [3]int{0, -6, -6}
	b.k053245.LoadState(r)
	b.k053251.LoadState(r)
	b.k053260.LoadState(r)
	if r.Err == nil {
		if r.B, err = b.ym.LoadState(r.B); err != nil {
			return err
		}
	}
	b.eeprom.loadState(r)
	m := make([]int, 6)
	r.Ints(m)
	f := make([]float64, 3)
	r.F64s(f)
	b.resampler.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	b.eepromOut, b.ctrl300, b.vblank, b.cyc, b.dmaBusy, b.setupFrames = m[0], m[1], m[2] != 0, m[3], m[4], m[5]
	b.nmiAt, b.zCyc, b.ymFrac = f[0], f[1], f[2]
	b.k052109.Rmrd = b.ctrl300&0x08 != 0
	for i := 0; i < 2048; i++ {
		b.updateColour(i)
	}
	b.resampler.Clear()
	b.drawFrame()
	return nil
}

// unscrambleSprites: reorder the sprite ROM's tiles into the chip's numbering.
func unscrambleSprites(rom []byte) []byte {
	out := make([]byte, len(rom))
	n := len(rom) >> 7
	for c := 0; c < n; c++ {
		x := (c & 1) | ((c >> 1) & 2) | ((c >> 2) & 4)
		y := ((c >> 1) & 1) | ((c >> 2) & 2) | ((c >> 3) & 4)
		r := (c &^ 0x3f) | x | y<<3
		copy(out[c<<7:(c+1)<<7], rom[r<<7:(r+1)<<7])
	}
	return out
}

func tmnt2Games() []arcade.Game {
	shared := []arcade.File{
		{Name: "063b01.2f", Size: 0x10000, Role: "sound", Offset: arcade.Next},
		{Name: "063b12.16k", Size: 0x80000, Role: "tiles", Lane: 0},
		{Name: "063b11.12k", Size: 0x80000, Role: "tiles", Lane: 2},
		{Name: "063b09.7l", Size: 0x100000, Role: "sprites", Lane: 0},
		{Name: "063b07.3l", Size: 0x100000, Role: "sprites", Lane: 2},
		{Name: "063b10.7k", Size: 0x80000, Role: "sprites", Offset: 0x200000, Lane: 0},
		{Name: "063b08.3k", Size: 0x80000, Role: "sprites", Offset: 0x200000, Lane: 2},
		{Name: "063b06.1d", Size: 0x200000, Role: "pcm", Offset: arcade.Next},
	}
	version := func(id, title, code string, players int, nv string) arcade.Game {
		parent := "tmnt2"
		if id == "tmnt2" {
			parent = ""
		}
		files := []arcade.File{
			{Name: "063" + code + "02.8e", Size: 0x20000, Role: "cpu", Offset: 0, Lane: 0},
			{Name: "063" + code + "03.8g", Size: 0x20000, Role: "cpu", Offset: 0, Lane: 1},
			{Name: "063" + code + "04.10e", Size: 0x20000, Role: "cpu", Offset: 0x40000, Lane: 0},
			{Name: "063" + code + "05.10g", Size: 0x20000, Role: "cpu", Offset: 0x40000, Lane: 1},
		}
		files = append(files, shared...)
		files = append(files, arcade.File{Name: nv, Size: 0x80, Role: "eeprom", Offset: arcade.Next, Optional: true})
		return arcade.Game{ID: id, Title: title, Year: 1991, Maker: "Konami", Players: players, Loose: true, Parent: parent, Files: files}
	}
	return []arcade.Game{
		version("tmnt2", "Teenage Mutant Ninja Turtles - Turtles in Time (4 players, US)", "uaa", 4, "tmnt2_uaa.nv"),
		version("tmnt2a", "Teenage Mutant Ninja Turtles - Turtles in Time (4 players, Asia)", "ada", 4, "tmnt2_ada.nv"),
		version("tmnt22pu", "Teenage Mutant Ninja Turtles - Turtles in Time (2 players, US)", "uda", 2, "tmnt22pu.nv"),
		version("tmht22pe", "Teenage Mutant Hero Turtles - Turtles in Time (2 players, Europe)", "eba", 2, "tmht22pe.nv"),
		version("tmht24pe", "Teenage Mutant Hero Turtles - Turtles in Time (4 players, Europe)", "eaa", 4, "tmht24pe.nv"),
		version("tmnt22pj", "Teenage Mutant Ninja Turtles 2 (2 players, Japan)", "jba", 2, "tmnt22pj.nv"),
		version("tmnt24pj", "Teenage Mutant Ninja Turtles 2 (4 players, Japan)", "jaa", 4, "tmnt24pj.nv"),
	}
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "tmnt2", Title: "Konami TMNT2 (Turtles in Time)", Width: t2W, Height: t2H, Rotate: 0, FPS: t2FPS, Players: 4,
		Characters: []string{"Leonardo", "Michelangelo", "Donatello", "Raphael"},
		Buttons:    []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Service"},
		Dips:       []arcade.Dip{}, Games: tmnt2Games(), New: newTMNT2,
	})
}
