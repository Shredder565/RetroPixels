package capcom

// Capcom 1943: The Battle of Midway (1987) and 1943 Kai (1987).
//
// Main Z80 at 6 MHz: 0000-7fff program ROM, 8000-bfff one of eight 16 KB
// banks (c804 bits 2-4), c000-c004 inputs and switches (c000 bit 3 is the
// vertical blank), c007 the protection MCU's answer, c800 sound command,
// c804 bank / flip (bit 6) / characters on (bit 7), c806 watchdog, c807 the
// question for the MCU, d000-d7ff characters (codes, then colours),
// d800-d802 front background scroll (x, 16 bits, and y), d803-d804 back
// background scroll, d806 layers on (bit 4 back, bit 5 front, bit 6 sprites),
// e000-efff work RAM, f000-ffff sprites. One interrupt (mode 1) a frame, at
// the start of the vertical blank.
//
// Sound Z80 at 3 MHz: 0000-7fff ROM, c000-c7ff RAM, c800 the command,
// e000-e003 two YM2203s at 1.5 MHz; interrupted 240 times a second (a
// timer of its own, not tied to the picture).
//
// Picture: 2048 characters (8x8, 4 colours, 32 colour sets); two
// backgrounds of 32x32 tiles in 16 colours, 2048 tiles long and 8 high, laid
// out in ROM (tile maps), scrolling sideways (up the upright screen): the
// back one (128 tiles) and the front one (512 tiles, colour 15 see-through);
// and 128 sprites of 16x16 in 16 colours (sprites in colour sets 10 and 11
// go behind the front background).
//
// Protection: an 8751 microcontroller answers the game's questions. The game
// writes a question to c807, later reads the answer from c007 and checks it
// against its own table at 021a (32 question / answer pairs); the board
// answers from that table.

import (
	"retropixels/cores/arcade"
	"retropixels/cores/ym2203"
	"retropixels/cores/z80"
)

const (
	c43MainLine  = 384 // main CPU clocks a line (6 MHz)
	c43SoundLine = 192 // sound CPU clocks a line (3 MHz)
	c43YMLine    = 96  // YM2203 clocks a line (1.5 MHz)
	c43MCUTable  = 0x021a
	c43SoundIRQ  = 3000000 / 240 // sound CPU clocks between its interrupts
)

type b1943 struct {
	game *arcade.Game
	dips map[string]int

	rom, snd                []byte
	chars, front, back, spr []uint8
	frontMap, backMap       []byte
	pal                     [256]uint32
	charPen                 [256]uint8
	frontPen, backPen       [256]uint8
	sprPen                  [256]uint8
	main, sound             irqCPU
	mainOver, soundOver     int
	ram                     [0x1000]uint8
	vram                    [0x800]uint8
	sprRAM                  [0x1000]uint8
	sndRAM                  [0x800]uint8
	latch                   uint8
	c804, layers            uint8
	frontX, backX           uint16
	frontY                  uint8
	mcuAnswer               uint8
	soundTimer              int // sound CPU clocks since its last interrupt
	vblank                  bool
	ym                      [2]*ym2203.Chip
	out                     *arcade.Resampler
	screen                  []uint32
	raw                     []uint8
	pads                    []arcade.Pad
	frame                   int64
	line                    int    // the line the next RunFrame starts at
	onRender                func() // tests: called when a frame is drawn
	fmGain, ssgGain         float64
}

type main43Bus struct{ b *b1943 }

func (x main43Bus) Read(a uint16) uint8     { return x.b.read(a) }
func (x main43Bus) Write(a uint16, v uint8) { x.b.write(a, v) }
func (x main43Bus) In(uint16) uint8         { return 0xff }
func (x main43Bus) Out(uint16, uint8)       {}
func (x main43Bus) Ack()                    { x.b.main.Ack() }

type snd43Bus struct{ b *b1943 }

func (x snd43Bus) Read(a uint16) uint8     { return x.b.sndRead(a) }
func (x snd43Bus) Write(a uint16, v uint8) { x.b.sndWrite(a, v) }
func (x snd43Bus) In(uint16) uint8         { return 0xff }
func (x snd43Bus) Out(uint16, uint8)       {}
func (x snd43Bus) Ack()                    { x.b.sound.Ack() }

// tile32Layout: 32x32 tiles, the 16x16 sprite layout grown to 32 pixels a
// line (four 8-pixel strips, 64 bytes apart) and 32 lines.
func tile32Layout(romBytes int) *gfxLayout {
	half := romBytes * 4
	var xs []int
	for s := 0; s < 4; s++ {
		xs = append(xs, seq(s*512, 1, 4)...)
		xs = append(xs, seq(s*512+8, 1, 4)...)
	}
	return &gfxLayout{w: 32, h: 32, planes: []int{half + 4, half, 4, 0}, xs: xs, ys: seq(0, 16, 32), step: 2048}
}

func new1943(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &b1943{game: g, dips: opts.Dips, fmGain: 0.030, ssgGain: 0.053}
	b.rom = arcade.Region(g, files, "cpu", 0x30000)
	b.snd = arcade.Region(g, files, "sound", 0x8000)
	b.chars = charLayout().decode(arcade.Region(g, files, "chars", 0x8000), 2048)
	b.front = tile32Layout(0x40000).decode(arcade.Region(g, files, "front", 0x40000), 512)
	b.back = tile32Layout(0x10000).decode(arcade.Region(g, files, "back", 0x10000), 128)
	b.spr = spriteLayout(0x40000).decode(arcade.Region(g, files, "sprites", 0x40000), 2048)
	b.frontMap = arcade.Region(g, files, "frontmap", 0x8000)
	b.backMap = arcade.Region(g, files, "backmap", 0x8000)
	b.pal = palette(arcade.Region(g, files, "red", 256), arcade.Region(g, files, "green", 256), arcade.Region(g, files, "blue", 256))
	lut := func(role string) []byte { return arcade.Region(g, files, role, 256) }
	cl, fl, fh, bl, bh, sl, sh := lut("charlut"), lut("frontlut"), lut("frontluth"), lut("backlut"), lut("backluth"), lut("spritelut"), lut("spriteluth")
	for i := 0; i < 256; i++ {
		b.charPen[i] = 0x40 | cl[i]&15
		b.frontPen[i] = (fh[i]&3)<<4 | fl[i]&15
		b.backPen[i] = (bh[i]&3)<<4 | bl[i]&15
		b.sprPen[i] = 0x80 | (sh[i]&7)<<4 | sl[i]&15
	}
	b.screen = make([]uint32, scrW*scrH)
	b.raw = make([]uint8, 256*256)
	b.ym[0], b.ym[1] = ym2203.New(), ym2203.New()
	b.out = arcade.NewResampler(fps*lines*soundSteps, float64(opts.SampleRate))
	b.main.cpu = z80.New(main43Bus{b})
	b.sound.cpu = z80.New(snd43Bus{b})
	b.Reset()
	return b, nil
}

func (b *b1943) Reset() {
	b.main.cpu.Reset()
	b.sound.cpu.Reset()
	b.main.cpu.IRQ, b.sound.cpu.IRQ = false, false
	b.main.taken, b.sound.taken = false, false
	b.mainOver, b.soundOver = 0, 0
	b.ram, b.vram, b.sprRAM, b.sndRAM = [0x1000]uint8{}, [0x800]uint8{}, [0x1000]uint8{}, [0x800]uint8{}
	b.latch, b.c804, b.layers, b.frontX, b.backX, b.frontY, b.mcuAnswer = 0, 0, 0, 0, 0, 0, 0
	b.vblank = false
	b.soundTimer = 0
	b.ym[0].Reset()
	b.ym[1].Reset()
	b.frame, b.line = 0, 0
}

func (b *b1943) bank() int { return int(b.c804>>2) & 7 }

func (b *b1943) read(a uint16) uint8 {
	switch {
	case a < 0x8000:
		return b.rom[a]
	case a < 0xc000:
		return b.rom[0x10000+b.bank()*0x4000+int(a-0x8000)]
	case a < 0xc800:
		return b.input(a & 7)
	case a >= 0xd000 && a < 0xd800:
		return b.vram[a-0xd000]
	case a >= 0xe000 && a < 0xf000:
		return b.ram[a-0xe000]
	case a >= 0xf000:
		return b.sprRAM[a-0xf000]
	}
	return 0xff
}

func (b *b1943) write(a uint16, v uint8) {
	switch {
	case a >= 0xc800 && a < 0xd000:
		switch a & 0xf {
		case 0:
			b.latch = v
		case 4:
			b.c804 = v
		case 7:
			b.mcuAnswer = b.mcuReply(v)
		}
	case a >= 0xd000 && a < 0xd800:
		b.vram[a-0xd000] = v
	case a >= 0xd800 && a < 0xe000:
		switch a & 0xf {
		case 0:
			b.frontX = b.frontX&0xff00 | uint16(v)
		case 1:
			b.frontX = b.frontX&0xff | uint16(v)<<8
		case 2:
			b.frontY = v
		case 3:
			b.backX = b.backX&0xff00 | uint16(v)
		case 4:
			b.backX = b.backX&0xff | uint16(v)<<8
		case 6:
			b.layers = v
		}
	case a >= 0xe000 && a < 0xf000:
		b.ram[a-0xe000] = v
	case a >= 0xf000:
		b.sprRAM[a-0xf000] = v
	}
}

// mcuReply is the protection MCU's answer to a question: the answer the
// game's own table (32 pairs at 021a) expects.
func (b *b1943) mcuReply(q uint8) uint8 {
	for i := 0; i < 32; i++ {
		if b.rom[c43MCUTable+i*2] == q {
			return b.rom[c43MCUTable+i*2+1]
		}
	}
	return 0
}

func (b *b1943) input(r uint16) uint8 {
	p1, p2 := pad(b.pads, 0), pad(b.pads, 1)
	d := b.dips
	switch r {
	case 0:
		v := padBits(p1, "Start (1P)", "Start (2P)", "", "", "", "", "Coin", "Coin 2")
		if p2["Start (1P)"] {
			v &^= 1
		}
		if p2["Start (2P)"] {
			v &^= 2
		}
		if p2["Coin"] {
			v &^= 0x80
		}
		if !b.vblank {
			v &^= 8
		}
		return v
	case 1:
		return padBits(p1, "Right", "Left", "Down", "Up", "Button 1", "Button 2")
	case 2:
		return padBits(p2, "Right", "Left", "Down", "Up", "Button 1", "Button 2")
	case 3:
		return uint8(d["difficulty"]&15 | (d["twoplayer"]&1)<<4 | (d["flip"]&1)<<5 | (d["stop"]&1)<<6 | (d["service"]&1)<<7)
	case 4:
		return uint8(d["coina"]&7 | (d["coinb"]&7)<<3 | (d["continue"]&1)<<6 | (d["demosound"]&1)<<7)
	case 7:
		return b.mcuAnswer
	}
	return 0xff
}

func (b *b1943) sndRead(a uint16) uint8 {
	switch {
	case a < 0x8000:
		return b.snd[a]
	case a >= 0xc000 && a < 0xc800:
		return b.sndRAM[a-0xc000]
	case a >= 0xc800 && a < 0xd000:
		return b.latch
	case a >= 0xe000 && a < 0xe004:
		return b.ym[a>>1&1].Read(int(a & 1))
	}
	return 0xff
}

func (b *b1943) sndWrite(a uint16, v uint8) {
	switch {
	case a >= 0xc000 && a < 0xc800:
		b.sndRAM[a-0xc000] = v
	case a >= 0xe000 && a < 0xe004:
		b.ym[a>>1&1].Write(int(a&1), v)
	}
}

func (b *b1943) SetInput(p []arcade.Pad) { b.pads = p }
func (b *b1943) Screen() []uint32        { return b.screen }
func (b *b1943) TakeAudio() []float32    { return b.out.Take() }

// RunFrame runs from the start of the vertical blank (where the last frame
// was drawn) to the next one, and draws the frame. (After a reset the first
// frame starts at line 0.)
func (b *b1943) RunFrame() {
	for {
		line := b.line
		switch line {
		case vblankLine:
			b.vblank = true
			b.main.assert(0xff)
		case vblankEnd:
			b.vblank = false
		}
		// the sound CPU's interrupt comes from its own 240 Hz timer
		b.soundTimer += c43SoundLine
		if b.soundTimer >= c43SoundIRQ {
			b.soundTimer -= c43SoundIRQ
			b.sound.assert(0xff)
		}
		b.main.run(c43MainLine, &b.mainOver)
		b.sound.run(c43SoundLine, &b.soundOver)
		for s := 0; s < soundSteps; s++ {
			var v float64
			for _, y := range b.ym {
				y.Run(c43YMLine / soundSteps)
				fm, ssg := y.Take()
				v += fm*b.fmGain + ssg*b.ssgGain
			}
			b.out.Add(v)
		}
		if b.line = (line + 1) % lines; b.line == vblankLine {
			break
		}
	}
	b.render()
	b.frame++
}

// render draws the frame: back background, low sprites, front background,
// other sprites, characters.
func (b *b1943) render() {
	raw := b.raw
	if b.layers&0x10 != 0 {
		b.drawBG(b.back, b.backMap, &b.backPen, int(b.backX), 0, 127, false)
	} else {
		for i := range raw {
			raw[i] = 0
		}
	}
	if b.layers&0x40 != 0 {
		b.drawSprites(true)
	}
	if b.layers&0x20 != 0 {
		b.drawBG(b.front, b.frontMap, &b.frontPen, int(b.frontX), int(b.frontY), 511, true)
	}
	if b.layers&0x40 != 0 {
		b.drawSprites(false)
	}
	if b.c804&0x80 != 0 {
		for ty := 0; ty < 32; ty++ {
			for tx := 0; tx < 32; tx++ {
				o := ty<<5 | tx
				attr := int(b.vram[0x400+o])
				code := int(b.vram[o]) | (attr&0xe0)<<3
				colr := (attr & 0x1f) << 2
				t := b.chars[code<<6:]
				for y := 0; y < 8; y++ {
					d := (ty*8+y)<<8 | tx*8
					for x := 0; x < 8; x++ {
						if c := b.charPen[colr|int(t[y<<3|x])]; c&15 != 15 {
							raw[d+x] = c
						}
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

// drawBG draws a 32x32-tile background from its tile map ROM: tiles go down
// each column of 8, two bytes each (code, then attributes: code bit 8 in
// bit 0, colour in bits 2-5, flip x 6, flip y 7).
func (b *b1943) drawBG(gfx []uint8, tmap []byte, pens *[256]uint8, sx, sy, mask int, see bool) {
	for y := 0; y < 256; y++ {
		by := (y + sy) & 255
		row := by >> 5
		for x := 0; x < 256; x++ {
			bx := (x + sx) & 0xffff
			o := ((bx>>5)<<3 | row) * 2
			attr := int(tmap[o+1])
			code := (int(tmap[o]) | (attr&1)<<8) & mask
			tx, ty := bx&31, by&31
			if attr&0x40 != 0 {
				tx ^= 31
			}
			if attr&0x80 != 0 {
				ty ^= 31
			}
			c := pens[(attr>>2&15)<<4|int(gfx[code<<10|ty<<5|tx])]
			if see && c&15 == 15 {
				continue
			}
			b.raw[y<<8|x] = c
		}
	}
}

// drawSprites draws the sprites behind the front background (colour sets
// 10 and 11) or the others, last to first.
func (b *b1943) drawSprites(behind bool) {
	for o := 0xfe0; o >= 0; o -= 32 {
		s := b.sprRAM[o : o+4]
		col := int(s[1] & 15)
		if (col == 10 || col == 11) != behind {
			continue
		}
		code := int(s[0]) | int(s[1]&0xe0)<<3
		sx := int(s[3]) - int(s[1]&0x10)<<4
		sy := int(s[2])
		t := b.spr[code<<8:]
		for y := 0; y < 16; y++ {
			yy := sy + y
			if yy > 255 {
				break
			}
			for x := 0; x < 16; x++ {
				xx := sx + x
				if xx < 0 || xx > 255 {
					continue
				}
				c := b.sprPen[col<<4|int(t[y<<4|x])]
				if c&15 != 15 {
					b.raw[yy<<8|xx] = c
				}
			}
		}
	}
}

func (b *b1943) toScreen() {
	flip := b.c804&0x40 != 0
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

func (b *b1943) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.cpu.AppendState(w.B)
	w.B = b.sound.cpu.AppendState(w.B)
	w.B = b.ym[0].AppendState(w.B)
	w.B = b.ym[1].AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.sprRAM[:])
	w.Bytes(b.sndRAM[:])
	w.U8(b.latch)
	w.U8(b.c804)
	w.U8(b.layers)
	w.U16(b.frontX)
	w.U16(b.backX)
	w.U8(b.frontY)
	w.U8(b.mcuAnswer)
	w.Int(b.soundTimer)
	w.Bool(b.vblank)
	w.Int(b.mainOver)
	w.Int(b.soundOver)
	w.U32(uint32(b.frame))
	w.U32(uint32(b.frame >> 32))
	w.Int(b.line)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *b1943) LoadState(s []byte) error {
	rest, err := b.main.cpu.LoadState(s)
	if err == nil {
		rest, err = b.sound.cpu.LoadState(rest)
	}
	if err == nil {
		rest, err = b.ym[0].LoadState(rest)
	}
	if err == nil {
		rest, err = b.ym[1].LoadState(rest)
	}
	if err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.sprRAM[:])
	r.Bytes(b.sndRAM[:])
	b.latch = r.U8()
	b.c804 = r.U8()
	b.layers = r.U8()
	b.frontX = r.U16()
	b.backX = r.U16()
	b.frontY = r.U8()
	b.mcuAnswer = r.U8()
	b.soundTimer = max(0, min(r.Int(), c43SoundIRQ-1))
	b.vblank = r.Bool()
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
