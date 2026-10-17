// Package dkong3 is Nintendo's Donkey Kong 3 board (1983) for RetroPixels'
// arcade framework.
//
// A Z80 at 4 MHz runs the game: 40 KB of program ROM, 4 KB of work RAM, a
// 32x32 tile map and a sprite list that a Z80 DMA chip copies into the
// sprite memory once a frame. The colours come from two 256-entry colour
// PROMs (4 bits each of red, green and blue, inverted) and the tiles' colour
// groups from a third PROM, by column and by group of four rows. The sound
// is two Ricoh RP2A03s, the NES's processor with its sound unit built in,
// each with its own program ROM and 2 KB of RAM; the game talks to them
// through three latches and starts them with a reset line. RetroPixels runs
// them on its own 6502 (cores/cpu6502) and NES sound unit (cores/nes). The
// monitor is on its side: the board draws 256x224, turned a quarter turn
// anticlockwise.
//
// The set's fifth PROM (dkc1-v.5e) is the main board's address decoder: it
// turns the processor's address into the chip selects. The memory map below
// is that decoding written out, so the board runs without the PROM.
package dkong3

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/cpu6502"
	"retropixels/cores/nes"
	"retropixels/cores/z80"
)

const (
	cpuHz   = 4000000
	sndHz   = 1789772
	lineHz  = 6144000.0 / 384
	lines   = 264
	scrW    = 256
	scrH    = 224
	top     = 16
	lineCyc = 250 // Z80 cycles a line (4 MHz / 16 kHz)
	vblank  = top + scrH
)

// FPS is the board's frame rate.
var FPS = lineHz / lines

// sound is one RP2A03 with its ROM, RAM and the latches it reads.
type sound struct {
	cpu    *cpu6502.CPU
	apu    *nes.APU
	rom    []byte
	ram    [0x800]byte
	latch  [2]uint8 // what reads of $4016 and $4017 see
	second bool     // the second processor: nothing answers at $4017
	cyc    int64    // cycles run
}

func (s *sound) clock() {
	for n := s.apu.Clock(); n > 0; n-- { // a DMC fetch halts the processor
		s.apu.Clock()
		s.cpu.Cycles++
	}
}

func (s *sound) Read(a uint16) uint8 {
	s.clock()
	switch {
	case a >= 0x8000:
		return s.rom[a&0x1fff]
	case a == 0x4015:
		return s.apu.ReadStatus()
	case a == 0x4016:
		return s.latch[0]
	case a == 0x4017:
		if s.second {
			return 0
		}
		return s.latch[1]
	}
	return s.ram[a&0x7ff]
}

func (s *sound) Write(a uint16, v uint8) {
	s.clock()
	switch {
	case a >= 0x8000:
	case a >= 0x4000 && a <= 0x4013 || a == 0x4015 || a == 0x4017:
		s.apu.Write(a, v)
	default:
		s.ram[a&0x7ff] = v
	}
}

// dma is the Z80 DMA chip as the game uses it: programmed through port 0 with
// a memory-to-memory block copy, which runs when the board's ready line
// (written at 7e85) goes high.
type dma struct {
	wr      [7]int // the base byte last written to each write register
	addrA   int
	addrB   int
	length  int
	follow  []int // the follow-up bytes still expected: register*16+what
	enabled bool
	srcA    bool // port A is the source
	stepA   int  // address steps: +1, -1 or 0
	stepB   int
	curA    int
	curB    int
}

// what a follow-up byte sets
const (
	fAddrALo = iota
	fAddrAHi
	fLenLo
	fLenHi
	fAddrBLo
	fAddrBHi
	fIgnore
)

func (d *dma) write(v int) {
	if len(d.follow) > 0 {
		f := d.follow[0]
		d.follow = d.follow[1:]
		switch f {
		case fAddrALo:
			d.addrA = d.addrA&0xff00 | v
		case fAddrAHi:
			d.addrA = d.addrA&0xff | v<<8
		case fLenLo:
			d.length = d.length&0xff00 | v
		case fLenHi:
			d.length = d.length&0xff | v<<8
		case fAddrBLo:
			d.addrB = d.addrB&0xff00 | v
		case fAddrBHi:
			d.addrB = d.addrB&0xff | v<<8
		}
		return
	}
	step := func(v int) int {
		switch (v >> 4) & 3 {
		case 0:
			return -1
		case 1:
			return 1
		}
		return 0
	}
	more := func(bits ...int) {
		for i := 0; i < len(bits); i += 2 {
			if v&bits[i] != 0 {
				d.follow = append(d.follow, bits[i+1])
			}
		}
	}
	switch {
	case v&0x80 == 0 && v&3 != 0: // WR0: direction, port A address, length
		d.wr[0] = v
		d.srcA = v&4 != 0
		more(0x08, fAddrALo, 0x10, fAddrAHi, 0x20, fLenLo, 0x40, fLenHi)
	case v&0x87 == 0x04: // WR1: port A
		d.wr[1] = v
		d.stepA = step(v)
		more(0x40, fIgnore)
	case v&0x87 == 0x00: // WR2: port B
		d.wr[2] = v
		d.stepB = step(v)
		more(0x40, fIgnore)
	case v&0x83 == 0x80: // WR3: mask and match
		d.wr[3] = v
		more(0x08, fIgnore, 0x10, fIgnore)
		if v&0x40 != 0 {
			d.enabled = true
		}
	case v&0x83 == 0x81: // WR4: mode, port B address
		d.wr[4] = v
		more(0x04, fAddrBLo, 0x08, fAddrBHi, 0x10, fIgnore)
	case v&0xc7 == 0x82: // WR5: ready and control lines
		d.wr[5] = v
	case v&0x83 == 0x83: // WR6: commands
		d.wr[6] = v
		switch v {
		case 0xcf: // load
			d.curA, d.curB = d.addrA, d.addrB
		case 0x87: // enable
			d.enabled = true
		case 0x83: // disable
			d.enabled = false
		case 0xc3: // reset
			d.enabled = false
			d.follow = nil
		case 0xbb: // read mask follows
			d.follow = append(d.follow, fIgnore)
		}
	}
}

// board is one running Donkey Kong 3.
type board struct {
	dips       map[string]int
	sampleRate float64
	rom        []byte // 0000-5fff and 8000-9fff
	tiles      []uint8
	sprites    []uint8
	palette    [256]uint32
	tileColour []byte

	ram       [0x1000]uint8
	spriteRam [0x400]uint8
	vram      [0x400]uint8
	screen    []uint32
	raster    []uint8
	p1, p2    arcade.Pad
	cpu       *z80.CPU
	snd       [2]*sound
	dma       dma

	ctrl    [8]uint8 // 7e80-7e87: coin counter, tile bank, flip, sprite bank, NMI on, DMA ready, palette bank
	sndRun  bool     // the sound processors' reset line is released
	line    int      // the line being drawn
	cyc     int      // Z80 cycles into the line
	stall   int      // Z80 cycles the DMA still holds the bus
	sndFrac float64  // sound cycles owed (fraction)
	audio   []float32
	pend    [2][]float32 // each sound unit's samples not yet mixed
	mixHP   [2]float64   // output stage high-pass (in, out)
}

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips, sampleRate: float64(opts.SampleRate)}
	if b.sampleRate <= 0 {
		b.sampleRate = 48000
	}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range dips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	b.rom = arcade.Region(g, files, "cpu", 0xa000)
	tileRom := arcade.Region(g, files, "tiles", 0x2000)
	spriteRom := arcade.Region(g, files, "sprites", 0x4000)
	pal := arcade.Region(g, files, "palette", 0x400)
	b.tileColour = append([]byte(nil), arcade.Region(g, files, "tilecolour", 0x100)...)
	b.decodeGraphics(tileRom, spriteRom)
	b.decodePalette(pal)
	for i := range b.snd {
		s := &sound{rom: arcade.Region(g, files, []string{"sound1", "sound2"}[i], 0x2000), second: i == 1}
		s.apu = nes.NewAPU(int(b.sampleRate))
		s.apu.SetRaw(true)
		s.cpu = cpu6502.New(s, cpu6502.Options{})
		b.snd[i] = s
	}
	b.screen = make([]uint32, scrW*scrH)
	b.raster = make([]uint8, scrW*256)
	b.p1, b.p2 = arcade.Pad{}, arcade.Pad{}
	b.audio = make([]float32, 0, 4096)
	b.cpu = z80.New(mainBus{b})
	b.Reset()
	return b, nil
}

// The tiles: 512 of 8x8, the first ROM holding every tile's low bit plane
// and the second its high one. The sprites: 256 of 16x16, the first two ROMs
// the low plane (left halves, then right halves), the last two the high one.
func (b *board) decodeGraphics(tileRom, spriteRom []byte) {
	b.tiles = make([]uint8, 512*64)
	for n := 0; n < 512; n++ {
		for y := 0; y < 8; y++ {
			lo, hi := tileRom[n*8+y], tileRom[0x1000+n*8+y]
			for x := 0; x < 8; x++ {
				s := 7 - x
				b.tiles[n*64+y*8+x] = (lo>>s)&1 | ((hi>>s)&1)<<1
			}
		}
	}
	b.sprites = make([]uint8, 256*256)
	for n := 0; n < 256; n++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				half := 0
				if x >= 8 {
					half = 0x1000
				}
				s, o := 7-(x&7), half+n*16+y
				b.sprites[n*256+y*16+x] = (spriteRom[o]>>s)&1 | ((spriteRom[o+0x2000]>>s)&1)<<1
			}
		}
	}
}

// The levels each 4-bit value gives on the monitor, measured, and how much of
// the other two guns' values leaks into each.
var (
	levR = [16]float64{0.3, 13.7, 36.3, 51.0, 73.2, 84.0, 99.9, 115.1, 138.3, 152.2, 166.1, 178.0, 193.3, 208.6, 221.9, 235.2}
	levG = [16]float64{-0.2, 12.4, 36.9, 52.0, 73.5, 87.7, 103.2, 118.7, 141.4, 156.1, 171.7, 183.5, 202.7, 215.8, 229.5, 243.2}
	levB = [16]float64{0.1, 18.4, 36.7, 49.7, 68.6, 83.0, 97.3, 111.3, 134.3, 146.0, 162.0, 175.4, 188.7, 202.1, 214.9, 227.8}
)

func level(v float64) int { return int(math.Max(0, math.Min(255, math.Floor(v+0.5)))) }

// decodePalette: colour i is red (high nibble) and green (low nibble) of the
// first PROM's entry i and blue (low nibble) of the second's, all inverted.
func (b *board) decodePalette(p []byte) {
	for i := 0; i < 256; i++ {
		r, g, bl := int(^p[i]>>4)&15, int(^p[i])&15, int(^p[0x200+i])&15
		fr, fg, fb := float64(r), float64(g), float64(bl)
		b.palette[i] = arcade.RGB(level(levR[r]+1.126*fg+0.215*fb), level(levG[g]+0.556*fr+0.225*fb), level(levB[bl]+0.619*fr+1.222*fg))
	}
}

func (b *board) Reset() {
	b.cpu.Reset()
	b.ram, b.spriteRam, b.vram = [0x1000]uint8{}, [0x400]uint8{}, [0x400]uint8{}
	b.ctrl = [8]uint8{}
	b.dma = dma{}
	b.line, b.cyc, b.stall, b.sndFrac = 0, 0, 0, 0
	b.mixHP = [2]float64{}
	b.audio = b.audio[:0]
	b.pend = [2][]float32{}
	b.sndRun = true
	for _, s := range b.snd {
		s.ram = [0x800]byte{}
		s.latch = [2]uint8{}
		s.apu.Power()
		s.cpu.Reset()
		s.cyc = 0
	}
	for i := range b.screen {
		b.screen[i] = 0
	}
}

func (b *board) SetInput(p []arcade.Pad) {
	b.p1, b.p2 = arcade.Pad{}, arcade.Pad{}
	if len(p) > 0 && p[0] != nil {
		b.p1 = p[0]
	}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

func bit(on bool, v int) int {
	if on {
		return v
	}
	return 0
}

func joy(p arcade.Pad) int {
	return bit(p["Right"], 1) | bit(p["Left"], 2) | bit(p["Up"], 4) | bit(p["Down"], 8) | bit(p["Fire"], 0x10)
}

type mainBus struct{ b *board }

func (x mainBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x mainBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }
func (x mainBus) In(uint16) uint8         { return 0xff }
func (x mainBus) Out(p uint16, v uint8) {
	if p&0xff == 0 {
		x.b.dma.write(int(v))
	}
}

func (b *board) read(a int) uint8 {
	switch {
	case a < 0x6000:
		return b.rom[a]
	case a < 0x7000:
		return b.ram[a-0x6000]
	case a < 0x7400:
		return b.spriteRam[a-0x7000]
	case a < 0x7800:
		return b.vram[a-0x7400]
	case a >= 0x8000 && a < 0xa000:
		return b.rom[a-0x2000]
	}
	d := b.dips
	switch a {
	case 0x7c00:
		p := b.p1
		return uint8(joy(p) | bit(p["Start (1P)"], 0x20) | bit(p["Start (2P)"], 0x40) | bit(p["Service"], 0x80))
	case 0x7c80:
		return uint8(joy(b.p2) | bit(b.p1["Coin"], 0x20) | bit(b.p1["Coin 2"], 0x40))
	case 0x7d00:
		return uint8(d["coins"]&7 | (d["sw4"]&1)<<3 | (d["sw5"]&1)<<4 | (d["sw6"]&1)<<5 | (d["service"]&1)<<6 | (d["cabinet"]&1)<<7)
	case 0x7d80:
		return uint8(d["lives"]&3 | (d["bonus"]&3)<<2 | (d["bonus2"]&3)<<4 | (d["difficulty"]&3)<<6)
	}
	return 0
}

func (b *board) write(a int, v uint8) {
	switch {
	case a < 0x6000:
	case a < 0x7000:
		b.ram[a-0x6000] = v
	case a < 0x7400:
		b.spriteRam[a-0x7000] = v
	case a < 0x7800:
		b.vram[a-0x7400] = v
	case a == 0x7c00:
		b.snd[0].latch[0] = v
	case a == 0x7c80:
		b.snd[0].latch[1] = v
	case a == 0x7d00:
		b.snd[1].latch[0] = v
	case a == 0x7d80:
		run := v&1 != 0
		if run && !b.sndRun {
			for _, s := range b.snd {
				s.apu.Reset()
				s.cpu.Reset()
			}
		}
		b.sndRun = run
	case a >= 0x7e80 && a < 0x7e88:
		r := a & 7
		old := b.ctrl[r]
		b.ctrl[r] = v & 1
		if r == 5 && v&1 != 0 && old == 0 {
			b.runDma()
		}
	}
}

// runDma: the block copy, done at once; the Z80 is held off the bus for the
// time it takes.
func (b *board) runDma() {
	d := &b.dma
	if !d.enabled || d.wr[0]&3 == 0 {
		return
	}
	n := d.length + 1
	src, dst, ss, ds := d.curA, d.curB, d.stepA, d.stepB
	if !d.srcA {
		src, dst, ss, ds = d.curB, d.curA, d.stepB, d.stepA
	}
	for i := 0; i < n; i++ {
		b.write(dst&0xffff, b.read(src&0xffff))
		src += ss
		dst += ds
	}
	if d.srcA {
		d.curA, d.curB = src, dst
	} else {
		d.curB, d.curA = src, dst
	}
	d.enabled = false
	b.stall += n * dmaByteCycles
}

func (b *board) drawFrame() {
	raster, vram := b.raster, &b.vram
	bank := int(b.ctrl[6] | b.ctrl[7]<<1)
	tbank := int(b.ctrl[1]^1) * 256
	for row := 2; row < 30; row++ {
		for col := 0; col < 32; col++ {
			code := int(vram[row*32+col]) + tbank
			colour := (int(b.tileColour[col+32*(row>>2)]&0x0f) + 16*bank) * 4
			g := code * 64
			for y := 0; y < 8; y++ {
				o, t := (row*8+y)*scrW+col*8, g+y*8
				for x := 0; x < 8; x++ {
					if p := b.tiles[t+x]; p != 0 {
						raster[o+x] = uint8(colour + int(p))
					} else {
						raster[o+x] = 0
					}
				}
			}
		}
	}
	sr := &b.spriteRam
	base := 0
	if b.ctrl[3] != 0 {
		base = 0x200
	}
	for o := base; o < base+0x1a0; o += 4 {
		yb := int(sr[o])
		if yb == 0 {
			continue
		}
		code := int(sr[o+1]&0x7f) | int(sr[o+2]&0x40)<<1
		flipY, flipX := sr[o+1]&0x80 != 0, sr[o+2]&0x80 != 0
		colour := (int(sr[o+2]&0x0f) + 16*bank) * 4
		sx, sy, g := int(sr[o+3])-8, 247-yb, code*256
		for y := 0; y < 16; y++ {
			ry := sy + y
			if ry < top || ry >= top+scrH {
				continue
			}
			srcY := y
			if flipY {
				srcY = 15 - y
			}
			for x := 0; x < 16; x++ {
				rx := (sx + x) & 0xff
				xx := x
				if flipX {
					xx = 15 - x
				}
				if p := b.sprites[g+srcY*16+xx]; p != 0 {
					raster[ry*scrW+rx] = uint8(colour + int(p))
				}
			}
		}
	}
	pal, scr, flip := &b.palette, b.screen, b.ctrl[2] != 0
	for y := 0; y < scrH; y++ {
		src := (y + top) * scrW
		for x := 0; x < scrW; x++ {
			c := raster[src+x]
			px := uint32(0)
			if c != 0 {
				px = pal[c]
			}
			if flip {
				scr[(scrH-1-y)*scrW+(scrW-1-x)] = px
			} else {
				scr[y*scrW+x] = px
			}
		}
	}
}

func (b *board) Screen() []uint32 { return b.screen }

// runSound runs both sound processors for n of their cycles.
func (b *board) runSound(n int) {
	for _, s := range b.snd {
		for s.cyc < int64(n) { // s.cyc: cycles run beyond the last target
			if b.sndRun {
				s.cpu.IRQLine = s.apu.IRQ()
				c := s.cpu.Step()
				s.cyc += int64(c)
			} else {
				s.apu.Clock()
				s.cyc++
			}
		}
		s.cyc -= int64(n)
	}
}

// RunFrame runs from one vertical blank to the next (the first frame after a
// reset from the top of the picture): the interrupts as the blank starts,
// then the lines, and the picture as the next blank starts.
func (b *board) RunFrame() {
	perLine := sndHz / lineHz
	if b.line == vblank {
		if b.ctrl[4] != 0 {
			b.cpu.NMI()
		}
		if b.sndRun {
			for _, s := range b.snd {
				s.cpu.NMIPending = true
			}
		}
	}
	for {
		for b.cyc < lineCyc {
			if b.stall > 0 {
				b.cyc += b.stall
				b.stall = 0
				continue
			}
			b.cyc += b.cpu.Step()
		}
		b.cyc -= lineCyc
		b.sndFrac += perLine
		n := int(b.sndFrac)
		b.sndFrac -= float64(n)
		b.runSound(n)
		if b.line++; b.line == lines {
			b.line = 0
		}
		if b.line == vblank {
			break
		}
	}
	b.drawFrame()
	b.mix()
}

// mix: the two sound units' outputs added, through the board's output stage.
func (b *board) mix() {
	for i, s := range b.snd {
		b.pend[i] = append(b.pend[i], s.apu.TakeAudio()...)
	}
	a, c := b.pend[0], b.pend[1]
	n := min(len(a), len(c))
	k := math.Exp(-2 * math.Pi * 5 / b.sampleRate) // a 5 Hz high-pass: only the steady offset goes
	for i := 0; i < n; i++ {
		x := float64(a[i]+c[i]) * mixGain
		b.mixHP[1] = k * (b.mixHP[1] + x - b.mixHP[0])
		b.mixHP[0] = x
		v := b.mixHP[1]
		b.audio = append(b.audio, float32(math.Max(-1, math.Min(1, v))))
	}
	b.pend[0], b.pend[1] = append(a[:0], a[n:]...), append(c[:0], c[n:]...)
}

// mixGain: the board's level, measured. The two units' outputs are added
// as they come (DC-coupled), only their steady offset taken off.
const mixGain = 0.5

func (b *board) TakeAudio() []float32 {
	out := append([]float32(nil), b.audio...)
	b.audio = b.audio[:0]
	return out
}

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.spriteRam[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.ctrl[:])
	w.Bool(b.sndRun)
	w.Ints([]int{b.line, b.cyc, b.stall})
	w.F64s([]float64{b.sndFrac, b.mixHP[0], b.mixHP[1]})
	d := &b.dma
	w.Ints(d.wr[:])
	w.Ints([]int{d.addrA, d.addrB, d.length, d.stepA, d.stepB, d.curA, d.curB, len(d.follow)})
	w.Ints(d.follow)
	w.Bool(d.enabled)
	w.Bool(d.srcA)
	for _, s := range b.snd {
		w.Blob(s.cpu.SaveState())
		w.Blob(s.apu.SaveState())
		w.Bytes(s.ram[:])
		w.Bytes(s.latch[:])
		w.Int(int(s.cyc))
	}
	return w.B, nil
}

func (b *board) LoadState(st []byte) error {
	rest, err := b.cpu.LoadState(st)
	if err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.spriteRam[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.ctrl[:])
	b.sndRun = r.Bool()
	m := make([]int, 3)
	r.Ints(m)
	b.line, b.cyc, b.stall = m[0], m[1], m[2]
	f := make([]float64, 3)
	r.F64s(f)
	b.sndFrac, b.mixHP[0], b.mixHP[1] = f[0], f[1], f[2]
	d := &b.dma
	r.Ints(d.wr[:])
	m = make([]int, 8)
	r.Ints(m)
	d.addrA, d.addrB, d.length, d.stepA, d.stepB, d.curA, d.curB = m[0], m[1], m[2], m[3], m[4], m[5], m[6]
	if m[7] < 0 || m[7] > 16 {
		return arcade.ErrBadState
	}
	d.follow = make([]int, m[7])
	r.Ints(d.follow)
	d.enabled = r.Bool()
	d.srcA = r.Bool()
	for _, s := range b.snd {
		if err := s.cpu.LoadState(r.Blob()); err != nil {
			return err
		}
		if err := s.apu.LoadState(r.Blob()); err != nil {
			return err
		}
		r.Bytes(s.ram[:])
		r.Bytes(s.latch[:])
		s.cyc = int64(r.Int())
	}
	if err := r.Done(); err != nil {
		return err
	}
	b.audio = b.audio[:0]
	b.pend = [2][]float32{}
	b.drawFrame()
	return nil
}

// dmaByteCycles: Z80 cycles the DMA takes per byte copied.
const dmaByteCycles = 6
