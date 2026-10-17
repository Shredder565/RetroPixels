package capcom

// Capcom 1942 (1984).
//
// Main Z80 at 3 MHz: 0000-7fff program ROM, 8000-bfff one of three 16 KB
// banks (c806), c000-c004 inputs and switches, c800 sound command, c802-c803
// background scroll (9 bits), c804 flip (bit 7) and sound-CPU reset (bit 4),
// c805 background palette bank, cc00-cc7f sprites, d000-d7ff characters
// (codes, then colours), d800-dbff background, e000-efff work RAM. It runs
// in interrupt mode 0: an RST 10 at line 240 (just before the frame is drawn
// at 246, where the vertical blank starts) and an RST 08 at line 109.
//
// Sound Z80 at 3 MHz: 0000-3fff ROM, 4000-47ff RAM, 6000 the command,
// 8000-8001 and c000-c001 two AY-3-8910s at 1.5 MHz; it is interrupted four
// times a frame.
//
// Picture: 512 characters (8x8, 4 colours) with 64 colour sets, a 512x256
// background of 16x16 tiles in 8 colours (32 sets in each of 4 palette
// banks) scrolling sideways (up the upright screen), and 32 sprites of
// 16x16 in 16 colours, which can be stacked 2 or 4 high.

import (
	"retropixels/cores/arcade"
	"retropixels/cores/ay8910"
	"retropixels/cores/z80"
)

const (
	c42MainLine  = 192 // main CPU clocks a line (3 MHz)
	c42SoundLine = 192 // sound CPU clocks a line (3 MHz)
	c42AYLine    = 96  // AY clocks a line (1.5 MHz)
)

type b1942 struct {
	game *arcade.Game
	dips map[string]int

	rom, snd                 []byte
	chars, tiles, sprites    []uint8
	pal                      [256]uint32
	charPen, tilePen, sprPen [256]uint8 // lookup PROMs (low nibbles)
	main, sound              irqCPU
	mainOver, soundOver      int
	ram                      [0x1000]uint8
	vram                     [0x800]uint8 // characters: codes, then attributes
	bgram                    [0x400]uint8
	spr                      [0x80]uint8
	sndRAM                   [0x800]uint8
	latch                    uint8
	scroll                   uint16
	c804, palBank, bank      uint8
	soundReset               bool
	ay                       [2]*ay8910.PSG
	out                      *arcade.Resampler
	screen                   []uint32
	raw                      []uint8 // 256x256 palette indexes
	pads                     []arcade.Pad
	frame                    int64
	line                     int    // the line the next RunFrame starts at
	onRender                 func() // tests: called when a frame is drawn
}

type main42Bus struct{ b *b1942 }

func (x main42Bus) Read(a uint16) uint8     { return x.b.read(a) }
func (x main42Bus) Write(a uint16, v uint8) { x.b.write(a, v) }
func (x main42Bus) In(uint16) uint8         { return 0xff }
func (x main42Bus) Out(uint16, uint8)       {}
func (x main42Bus) Ack()                    { x.b.main.Ack() }

type snd42Bus struct{ b *b1942 }

func (x snd42Bus) Read(a uint16) uint8     { return x.b.sndRead(a) }
func (x snd42Bus) Write(a uint16, v uint8) { x.b.sndWrite(a, v) }
func (x snd42Bus) In(uint16) uint8         { return 0xff }
func (x snd42Bus) Out(uint16, uint8)       {}
func (x snd42Bus) Ack()                    { x.b.sound.Ack() }

func new1942(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &b1942{game: g, dips: opts.Dips}
	b.rom = arcade.Region(g, files, "cpu", 0x1c000)
	b.snd = arcade.Region(g, files, "sound", 0x4000)
	b.chars = charLayout().decode(arcade.Region(g, files, "chars", 0x2000), 512)
	third := 0x4000 * 8
	tl := &gfxLayout{w: 16, h: 16, planes: []int{0, third, 2 * third},
		xs: append(seq(0, 1, 8), seq(128, 1, 8)...), ys: seq(0, 8, 16), step: 256}
	b.tiles = tl.decode(arcade.Region(g, files, "tiles", 0xc000), 512)
	b.sprites = spriteLayout(0x10000).decode(arcade.Region(g, files, "sprites", 0x10000), 512)
	b.pal = palette(arcade.Region(g, files, "red", 256), arcade.Region(g, files, "green", 256), arcade.Region(g, files, "blue", 256))
	for i, v := range arcade.Region(g, files, "charlut", 256)[:256] {
		b.charPen[i] = 0x80 | v&15
	}
	copy(b.tilePen[:], arcade.Region(g, files, "tilelut", 256))
	for i, v := range arcade.Region(g, files, "spritelut", 256)[:256] {
		b.sprPen[i] = v & 15
	}
	b.screen = make([]uint32, scrW*scrH)
	b.raw = make([]uint8, 256*256)
	b.ay[0], b.ay[1] = ay8910.New(false), ay8910.New(false)
	b.out = arcade.NewResampler(fps*lines*soundSteps, float64(opts.SampleRate))
	b.main.cpu = z80.New(main42Bus{b})
	b.sound.cpu = z80.New(snd42Bus{b})
	b.Reset()
	return b, nil
}

func (b *b1942) Reset() {
	b.main.cpu.Reset()
	b.sound.cpu.Reset()
	b.main.cpu.IRQ, b.sound.cpu.IRQ = false, false
	b.main.taken, b.sound.taken = false, false
	b.mainOver, b.soundOver = 0, 0
	b.ram, b.vram, b.bgram, b.spr, b.sndRAM = [0x1000]uint8{}, [0x800]uint8{}, [0x400]uint8{}, [0x80]uint8{}, [0x800]uint8{}
	b.latch, b.scroll, b.c804, b.palBank, b.bank, b.soundReset = 0, 0, 0, 0, 0, false
	b.ay[0].Reset()
	b.ay[1].Reset()
	b.frame, b.line = 0, 0
}

func (b *b1942) read(a uint16) uint8 {
	switch {
	case a < 0x8000:
		return b.rom[a]
	case a < 0xc000:
		return b.rom[0x10000+int(b.bank)*0x4000+int(a-0x8000)]
	case a < 0xc800:
		return b.input(a & 7)
	case a >= 0xcc00 && a < 0xcc80:
		return b.spr[a-0xcc00]
	case a >= 0xd000 && a < 0xd800:
		return b.vram[a-0xd000]
	case a >= 0xd800 && a < 0xdc00:
		return b.bgram[a-0xd800]
	case a >= 0xe000 && a < 0xf000:
		return b.ram[a-0xe000]
	}
	return 0xff
}

func (b *b1942) write(a uint16, v uint8) {
	switch {
	case a >= 0xc800 && a < 0xc810:
		switch a & 0xf {
		case 0:
			b.latch = v
		case 2:
			b.scroll = b.scroll&0x100 | uint16(v)
		case 3:
			b.scroll = b.scroll&0xff | uint16(v&1)<<8
		case 4:
			b.c804 = v
			reset := v&0x10 != 0
			if reset && !b.soundReset {
				b.sound.cpu.Reset()
				b.sound.cpu.IRQ = false
			}
			b.soundReset = reset
		case 5:
			b.palBank = v & 3
		case 6:
			b.bank = v & 3
			if b.bank == 3 {
				b.bank = 2
			}
		}
	case a >= 0xcc00 && a < 0xcc80:
		b.spr[a-0xcc00] = v
	case a >= 0xd000 && a < 0xd800:
		b.vram[a-0xd000] = v
	case a >= 0xd800 && a < 0xdc00:
		b.bgram[a-0xd800] = v
	case a >= 0xe000 && a < 0xf000:
		b.ram[a-0xe000] = v
	}
}

func (b *b1942) input(r uint16) uint8 {
	p1, p2 := pad(b.pads, 0), pad(b.pads, 1)
	d := b.dips
	switch r {
	case 0:
		v := padBits(p1, "Start (1P)", "Start (2P)", "", "", "Service", "", "Coin 2", "Coin")
		if p2["Start (1P)"] {
			v &^= 1
		}
		if p2["Start (2P)"] {
			v &^= 2
		}
		if p2["Coin"] {
			v &^= 0x40
		}
		return v
	case 1:
		return padBits(p1, "Right", "Left", "Down", "Up", "Button 1", "Button 2")
	case 2:
		return padBits(p2, "Right", "Left", "Down", "Up", "Button 1", "Button 2")
	case 3:
		return uint8(d["coina"]&7 | (d["cabinet"]&1)<<3 | (d["bonus"]&3)<<4 | (d["lives"]&3)<<6)
	case 4:
		return uint8(d["coinb"]&7 | (d["service"]&1)<<3 | (d["flip"]&1)<<4 | (d["difficulty"]&3)<<5 | (d["stop"]&1)<<7)
	}
	return 0xff
}

func (b *b1942) sndRead(a uint16) uint8 {
	switch {
	case a < 0x4000:
		return b.snd[a]
	case a < 0x4800:
		return b.sndRAM[a-0x4000]
	case a >= 0x6000 && a < 0x6800:
		return b.latch
	case a >= 0x8000 && a < 0xa000:
		return b.ay[0].ReadData()
	case a >= 0xc000 && a < 0xe000:
		return b.ay[1].ReadData()
	}
	return 0xff
}

func (b *b1942) sndWrite(a uint16, v uint8) {
	switch {
	case a >= 0x4000 && a < 0x4800:
		b.sndRAM[a-0x4000] = v
	case a >= 0x8000 && a < 0xa000:
		if a&1 == 0 {
			b.ay[0].WriteAddr(v)
		} else {
			b.ay[0].WriteData(v)
		}
	case a >= 0xc000 && a < 0xe000:
		if a&1 == 0 {
			b.ay[1].WriteAddr(v)
		} else {
			b.ay[1].WriteData(v)
		}
	}
}

func (b *b1942) SetInput(p []arcade.Pad) { b.pads = p }
func (b *b1942) Screen() []uint32        { return b.screen }
func (b *b1942) TakeAudio() []float32    { return b.out.Take() }

// RunFrame runs from the start of the vertical blank (where the last frame
// was drawn) to the next one, and draws the frame. (After a reset the first
// frame starts at line 0.)
func (b *b1942) RunFrame() {
	for {
		line := b.line
		switch line {
		case 109:
			b.main.assert(0xcf) // RST 08
		case 240:
			b.main.assert(0xd7) // RST 10
		}
		if line == 44 || line == 109 || line == 175 || line == 240 {
			b.sound.assert(0xff)
		}
		b.main.run(c42MainLine, &b.mainOver)
		if b.soundReset {
			b.soundOver = 0
		} else {
			b.sound.run(c42SoundLine, &b.soundOver)
		}
		for s := 0; s < soundSteps; s++ {
			b.ay[0].Run(c42AYLine / soundSteps)
			b.ay[1].Run(c42AYLine / soundSteps)
			b.out.Add((b.ay[0].Take() + b.ay[1].Take()) / 6 * 0.9)
		}
		if b.line = (line + 1) % lines; b.line == vblankLine {
			break
		}
	}
	b.render()
	b.frame++
}

// render draws the frame: background, sprites, characters.
func (b *b1942) render() {
	raw := b.raw
	// background: 32 columns x 16 rows of 16x16 tiles; each column is 16
	// codes then 16 attributes (colour 0-4, flip x 5, flip y 6, code bit 8 7)
	sc := int(b.scroll)
	bank := int(b.palBank) << 4
	for y := 0; y < 256; y++ {
		row := y >> 4
		for x := 0; x < 256; x++ {
			bx := (x + sc) & 511
			col := bx >> 4
			o := col<<5 | row
			attr := int(b.bgram[o+16])
			code := int(b.bgram[o]) | (attr&0x80)<<1
			tx, ty := bx&15, y&15
			if attr&0x20 != 0 {
				tx ^= 15
			}
			if attr&0x40 != 0 {
				ty ^= 15
			}
			p := b.tiles[code<<8|ty<<4|tx]
			raw[y<<8|x] = uint8(bank | int(b.tilePen[(attr&0x1f)<<3|int(p)]&15))
		}
	}
	// sprites: 4 bytes each, drawn last to first
	for o := 0x7c; o >= 0; o -= 4 {
		s := b.spr[o : o+4]
		code := int(s[0]&0x7f) | int(s[1]&0x20)<<2 | int(s[0]&0x80)<<1
		colr := int(s[1]&15) << 4
		sx := int(s[3]) - int(s[1]&0x10)<<4
		sy := int(s[2])
		n := int(s[1]&0xc0) >> 6
		if n == 2 {
			n = 3
		}
		for i := n; i >= 0; i-- {
			b.sprite(code+i, colr, sx, sy+16*i)
		}
	}
	// characters: 32x32; attribute bit 7 is code bit 8, bits 0-5 the colour
	for ty := 0; ty < 32; ty++ {
		for tx := 0; tx < 32; tx++ {
			o := ty<<5 | tx
			attr := int(b.vram[0x400+o])
			code := int(b.vram[o]) | (attr&0x80)<<1
			colr := (attr & 0x3f) << 2
			t := b.chars[code<<6:]
			for y := 0; y < 8; y++ {
				d := (ty*8+y)<<8 | tx*8
				for x := 0; x < 8; x++ {
					if p := t[y<<3|x]; p != 0 {
						raw[d+x] = b.charPen[colr|int(p)]
					}
				}
			}
		}
	}
	b.toScreen()
	if b.onRender != nil {
		b.onRender()
	}
}

func (b *b1942) sprite(code, colr, sx, sy int) {
	t := b.sprites[(code&511)<<8:]
	for y := 0; y < 16; y++ {
		yy := sy + y
		if yy < 0 || yy > 255 {
			continue
		}
		for x := 0; x < 16; x++ {
			xx := sx + x
			if xx < 0 || xx > 255 {
				continue
			}
			c := b.sprPen[colr|int(t[y<<4|x])]
			if c != 15 {
				b.raw[yy<<8|xx] = 0x40 | c
			}
		}
	}
}

// toScreen turns the shown lines into pixels.
func (b *b1942) toScreen() {
	flip := b.c804&0x80 != 0
	for y := 0; y < scrH; y++ {
		for x := 0; x < scrW; x++ {
			sx, sy := x, y+firstLine
			if flip {
				sx, sy = 255-x, 255-sy
			}
			b.screen[y*scrW+x] = b.pal[b.raw[sy<<8|sx]]
		}
	}
}

func (b *b1942) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.cpu.AppendState(w.B)
	w.B = b.sound.cpu.AppendState(w.B)
	w.B = b.ay[0].AppendState(w.B)
	w.B = b.ay[1].AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.bgram[:])
	w.Bytes(b.spr[:])
	w.Bytes(b.sndRAM[:])
	w.U8(b.latch)
	w.U16(b.scroll)
	w.U8(b.c804)
	w.U8(b.palBank)
	w.U8(b.bank)
	w.Bool(b.soundReset)
	w.U8(b.main.vector)
	w.U8(b.sound.vector)
	w.Int(b.mainOver)
	w.Int(b.soundOver)
	w.U32(uint32(b.frame))
	w.U32(uint32(b.frame >> 32))
	w.Int(b.line)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *b1942) LoadState(s []byte) error {
	rest, err := b.main.cpu.LoadState(s)
	if err == nil {
		rest, err = b.sound.cpu.LoadState(rest)
	}
	if err == nil {
		rest, err = b.ay[0].LoadState(rest)
	}
	if err == nil {
		rest, err = b.ay[1].LoadState(rest)
	}
	if err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.bgram[:])
	r.Bytes(b.spr[:])
	r.Bytes(b.sndRAM[:])
	b.latch = r.U8()
	b.scroll = r.U16() & 0x1ff
	b.c804 = r.U8()
	b.palBank = r.U8() & 3
	b.bank = r.U8() % 3
	b.soundReset = r.Bool()
	b.main.vector = r.U8()
	b.sound.vector = r.U8()
	b.mainOver = clampOver(r.Int())
	b.soundOver = clampOver(r.Int())
	b.frame = int64(r.U32())
	b.frame |= int64(r.U32()) << 32
	b.line = max(0, min(r.Int(), lines-1))
	b.out.LoadState(r)
	b.out.Clear()
	b.main.taken, b.sound.taken = false, false
	if r.Err == nil {
		b.render()
	}
	return r.Done()
}
