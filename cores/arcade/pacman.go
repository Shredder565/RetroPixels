package arcade

// Namco Pac-Man hardware (Puck Man / Pac-Man): a port of
// web/emu/arcade_pacman.js.
//
// One Z80 at 3.072 MHz with 16 KB of program ROM, 1 KB of tile RAM and 1 KB of
// tile colour RAM, 1 KB of work RAM (its last 16 bytes hold the 8 sprites'
// number / flip / colour), a 36x28 tile screen with 8 hardware sprites,
// colours from two PROMs, and Namco's 3-voice wavetable sound. The game sets
// the interrupt vector with an OUT to port 0 and runs in interrupt mode 2; the
// interrupt comes at the start of each vertical blank. The monitor is on its
// side, so the board draws 288x224 and the framework turns it upright.

import "retropixels/cores/z80"

const (
	pmCPUHz          = 3072000
	pmLineCyc        = 192
	pmLines          = 264
	pmShown          = 224
	pmW, pmH         = 288, 224
	pmSoundHz        = pmCPUHz / 32
	pmSamplesPerLine = pmLineCyc / 32
)

type pacman struct {
	game                             *Game
	dips                             map[string]int
	rom, tilesRom, spritesRom        []byte
	colourProm, lookupProm, waveProm []byte
	tiles, sprites                   []uint8
	pens                             [256]uint32
	clear                            [256]bool
	vram, cram, ram                  [0x400]uint8
	spritePos                        [16]uint8
	latch                            [8]uint8
	snd                              [32]uint8
	acc                              [3]uint32
	screen                           []uint32
	out                              *Resampler
	p1, p2                           Pad
	cpu                              *z80.CPU
	vector                           int
	irqLine                          bool
	watchdog, cycles, frame          int
}

type pacmanBus struct{ b *pacman }

func (x pacmanBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x pacmanBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }
func (x pacmanBus) In(uint16) uint8         { return 0xff }
func (x pacmanBus) Out(port uint16, v uint8) {
	if port&0xff == 0 {
		x.b.vector = int(v)
	}
}

// the vertical-blank interrupt stays on until the processor takes it
func (x pacmanBus) Ack()          { x.b.irqLine = false; x.b.cpu.IRQ = false }
func (x pacmanBus) Vector() uint8 { return uint8(x.b.vector) }

func newPacman(g *Game, files Files, opts Opts) (Board, error) {
	b := &pacman{game: g, dips: opts.Dips}
	b.rom = Region(g, files, "cpu", 0x4000)
	b.tilesRom = Region(g, files, "tiles", 0x1000)
	b.spritesRom = Region(g, files, "sprites", 0x1000)
	b.colourProm = Region(g, files, "colour", 32)
	b.lookupProm = Region(g, files, "lookup", 256)
	b.waveProm = Region(g, files, "wave", 256)
	b.decodeGraphics()
	b.decodeColours()
	b.screen = make([]uint32, pmW*pmH)
	b.out = NewResampler(pmSoundHz, float64(opts.SampleRate))
	b.p1, b.p2 = Pad{}, Pad{}
	b.cpu = z80.New(pacmanBus{b})
	b.Reset()
	return b, nil
}

func (b *pacman) Reset() {
	b.cpu.Reset()
	b.vram, b.cram, b.ram = [0x400]uint8{}, [0x400]uint8{}, [0x400]uint8{}
	b.spritePos, b.latch, b.snd, b.acc = [16]uint8{}, [8]uint8{}, [32]uint8{}, [3]uint32{}
	b.vector, b.irqLine = 0xff, false
	b.cpu.IRQ = false
	b.watchdog, b.cycles, b.frame = 0, 0, 0
}

// Each 8x8 tile is 16 bytes: bytes 8-15 give pixels 0-3 of each line, bytes
// 0-7 pixels 4-7. A 16x16 sprite is four such strips side by side, top half
// first.
func (b *pacman) decodeGraphics() {
	px := func(rom []byte, base, x, y int, strips []int) uint8 {
		v := rom[base+strips[x>>2]+y]
		n := x & 3
		return ((v>>(7-n))&1)<<1 | (v>>(3-n))&1
	}
	b.tiles = make([]uint8, 256*64)
	for t := 0; t < 256; t++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				b.tiles[t*64+y*8+x] = px(b.tilesRom, t*16, x, y, []int{8, 0})
			}
		}
	}
	b.sprites = make([]uint8, 64*256)
	for s := 0; s < 64; s++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				half := 0
				if y >= 8 {
					half = 32
				}
				b.sprites[s*256+y*16+x] = px(b.spritesRom, s*64+half, x, y&7, []int{8, 16, 24, 0})
			}
		}
	}
}

// promColour: 3 bits red, 3 bits green, 2 bits blue through weighted resistors.
func promColour(c uint8) uint32 {
	bit := func(n uint) int { return int(c>>n) & 1 }
	return RGB(0x21*bit(0)+0x47*bit(1)+0x97*bit(2), 0x21*bit(3)+0x47*bit(4)+0x97*bit(5), 0x51*bit(6)+0xae*bit(7))
}

func (b *pacman) decodeColours() {
	var colours [16]uint32
	for i := range colours {
		colours[i] = promColour(b.colourProm[i])
	}
	for i := 0; i < 256; i++ {
		c := b.lookupProm[i] & 15
		b.pens[i] = colours[c]
		b.clear[i] = c == 0 // colour 0 is see-through on sprites
	}
}

func pmTileOffset(tc, tr int) int {
	if tc < 2 {
		return 0x3c0 + tc*32 + 2 + tr
	}
	if tc >= 34 {
		return (tc-34)*32 + 2 + tr
	}
	return 0x40 + tr*32 + (tc - 2)
}

func (b *pacman) render() {
	scr := b.screen
	for tr := 0; tr < 28; tr++ {
		for tc := 0; tc < 36; tc++ {
			o := pmTileOffset(tc, tr)
			t := int(b.vram[o]) * 64
			pal := int(b.cram[o]&0x1f) << 2
			for y := 0; y < 8; y++ {
				d := (tr*8+y)*pmW + tc*8
				row := b.tiles[t+y*8 : t+y*8+8]
				for x := 0; x < 8; x++ {
					scr[d+x] = b.pens[pal|int(row[x])]
				}
			}
		}
	}
	for n := 7; n >= 0; n-- {
		attr := int(b.ram[0x3f0+n*2])
		pal := int(b.ram[0x3f1+n*2]&0x3f) << 2
		sx := 272 - int(b.spritePos[n*2+1])
		sy := int(b.spritePos[n*2]) - 31
		base := (attr >> 2) * 256
		fx, fy := 0, 0
		if attr&1 != 0 {
			fx = 15
		}
		if attr&2 != 0 {
			fy = 15
		}
		for y := 0; y < 16; y++ {
			yy := (sy + y) & 0xff
			if yy >= pmH {
				continue
			}
			for x := 0; x < 16; x++ {
				xx := sx + x
				if xx < 16 || xx >= 272 {
					continue
				}
				p := pal | int(b.sprites[base+((y^fy)<<4)+(x^fx)])
				if !b.clear[p] {
					scr[yy*pmW+xx] = b.pens[p]
				}
			}
		}
	}
	if b.latch[3]&1 != 0 {
		for i, j := 0, len(scr)-1; i < j; i, j = i+1, j-1 {
			scr[i], scr[j] = scr[j], scr[i]
		}
	}
}

func (b *pacman) soundSample() {
	if b.latch[1]&1 == 0 {
		b.out.Add(0)
		return
	}
	r, wave := &b.snd, b.waveProm
	mix := 0
	for v := 0; v < 3; v++ {
		var freq int
		var vol, sel int
		switch v {
		case 0:
			freq = int(r[0x10]) | int(r[0x11])<<4 | int(r[0x12])<<8 | int(r[0x13])<<12 | int(r[0x14])<<16
			vol, sel = int(r[0x15]), int(r[0x05]&7)
		case 1:
			freq = int(r[0x16])<<4 | int(r[0x17])<<8 | int(r[0x18])<<12 | int(r[0x19])<<16
			vol, sel = int(r[0x1a]), int(r[0x0a]&7)
		default:
			freq = int(r[0x1b])<<4 | int(r[0x1c])<<8 | int(r[0x1d])<<12 | int(r[0x1e])<<16
			vol, sel = int(r[0x1f]), int(r[0x0f]&7)
		}
		a := (int(b.acc[v]) + freq) & 0xfffff
		b.acc[v] = uint32(a)
		mix += (int(wave[sel<<5|a>>15]&15) - 8) * vol
	}
	b.out.Add(float64(mix) / 360 * 0.7)
}

func (b *pacman) TakeAudio() []float32 { return b.out.Take() }

func (b *pacman) read(a int) uint8 {
	a &= 0x7fff
	if a < 0x4000 {
		return b.rom[a]
	}
	if a < 0x5000 {
		switch a & 0xc00 {
		case 0x000:
			return b.vram[a&0x3ff]
		case 0x400:
			return b.cram[a&0x3ff]
		case 0xc00:
			return b.ram[a&0x3ff]
		}
		return 0xff
	}
	if a < 0x6000 {
		switch a & 0xc0 {
		case 0x00:
			return b.readIn0()
		case 0x40:
			return b.readIn1()
		case 0x80:
			return b.readDsw()
		}
	}
	return 0xff
}

func (b *pacman) write(a int, v uint8) {
	a &= 0x7fff
	if a < 0x4000 {
		return
	}
	if a < 0x5000 {
		switch a & 0xc00 {
		case 0x000:
			b.vram[a&0x3ff] = v
		case 0x400:
			b.cram[a&0x3ff] = v
		case 0xc00:
			b.ram[a&0x3ff] = v
		}
		return
	}
	if a < 0x6000 {
		r := a & 0xff
		switch {
		case r < 0x40:
			b.latch[r&7] = v & 1
			if r&7 == 0 && v&1 == 0 {
				b.irqLine = false
				b.cpu.IRQ = false
			}
		case r < 0x60:
			n := r & 0x1f
			v &= 15
			b.snd[n] = v
			voice := -1
			switch {
			case n < 5:
				voice = 0
			case n >= 6 && n < 10:
				voice = 1
			case n >= 11 && n < 15:
				voice = 2
			}
			if voice >= 0 {
				shift := n * 4
				if voice == 1 {
					shift = (n - 5) * 4
				} else if voice == 2 {
					shift = (n - 10) * 4
				}
				b.acc[voice] = uint32((int(b.acc[voice]) &^ (15 << shift) & 0xfffff) | int(v)<<shift)
			}
		case r < 0x70:
			b.spritePos[r&15] = v
		case r >= 0xc0:
			b.watchdog = 0
		}
	}
}

func (b *pacman) readIn0() uint8 {
	p, d := b.p1, b.dips
	v := 0xff
	if p["Up"] {
		v &^= 1
	}
	if p["Left"] {
		v &^= 2
	}
	if p["Right"] {
		v &^= 4
	}
	if p["Down"] {
		v &^= 8
	}
	if d["rack"] == 1 {
		v &^= 0x10
	}
	if p["Coin"] {
		v &^= 0x20
	}
	if p["Coin 2"] {
		v &^= 0x40
	}
	if p["Service"] {
		v &^= 0x80
	}
	return uint8(v)
}

func (b *pacman) readIn1() uint8 {
	p, q, d := b.p1, b.p2, b.dips
	v := 0xff
	if q["Up"] {
		v &^= 1
	}
	if q["Left"] {
		v &^= 2
	}
	if q["Right"] {
		v &^= 4
	}
	if q["Down"] {
		v &^= 8
	}
	if d["test"] == 1 {
		v &^= 0x10
	}
	if p["Start (1P)"] || q["Start (1P)"] {
		v &^= 0x20
	}
	if p["Start (2P)"] || q["Start (2P)"] {
		v &^= 0x40
	}
	if d["cabinet"] == 1 {
		v &^= 0x80
	}
	return uint8(v)
}

func (b *pacman) readDsw() uint8 {
	d := b.dips
	return uint8(d["coins"]&3 | (d["lives"]&3)<<2 | (d["bonus"]&3)<<4 | (d["difficulty"]&1)<<6 | (d["names"]&1)<<7)
}

func (b *pacman) SetInput(p []Pad) {
	b.p1 = p[0]
	b.p2 = Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

func (b *pacman) Screen() []uint32 { return b.screen }

func (b *pacman) RunFrame() {
	cpu := b.cpu
	for line := 0; line < pmLines; line++ {
		if line == pmShown {
			b.render()
			if b.latch[0]&1 != 0 {
				b.irqLine = true
				cpu.IRQ = true
			}
			// the watchdog restarts the board if the game stops checking in
			b.watchdog++
			if b.watchdog >= 16 {
				b.Reset()
				return
			}
		}
		end := b.cycles + pmLineCyc
		for b.cycles < end {
			b.cycles += cpu.Step()
		}
		for s := 0; s < pmSamplesPerLine; s++ {
			b.soundSample()
		}
	}
	b.cycles -= pmLines * pmLineCyc
	b.frame++
}

func (b *pacman) SaveState() ([]byte, error) {
	w := &StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.vram[:])
	w.Bytes(b.cram[:])
	w.Bytes(b.ram[:])
	w.Bytes(b.spritePos[:])
	w.Bytes(b.latch[:])
	w.Bytes(b.snd[:])
	w.U32s(b.acc[:])
	w.Ints([]int{b.vector, b.watchdog, b.cycles, b.frame})
	w.Bool(b.irqLine)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *pacman) LoadState(s []byte) error {
	rest, err := b.cpu.LoadState(s)
	if err != nil {
		return err
	}
	r := &StateR{B: rest}
	r.Bytes(b.vram[:])
	r.Bytes(b.cram[:])
	r.Bytes(b.ram[:])
	r.Bytes(b.spritePos[:])
	r.Bytes(b.latch[:])
	r.Bytes(b.snd[:])
	r.U32s(b.acc[:])
	m := make([]int, 4)
	r.Ints(m)
	b.vector, b.watchdog, b.cycles, b.frame = m[0], m[1], m[2], m[3]
	b.irqLine = r.Bool()
	b.cpu.IRQ = b.irqLine
	b.out.LoadState(r)
	b.out.Clear()
	b.render()
	return r.Done()
}

var pacmanProms = []File{
	{Name: "82s123.7f", Alt: []string{"pm1-1.7f"}, Size: 32, Role: "colour", Offset: Next},
	{Name: "82s126.4a", Alt: []string{"pm1-4.4a"}, Size: 256, Role: "lookup", Offset: Next},
	{Name: "82s126.1m", Alt: []string{"pm1-3.1m"}, Size: 256, Role: "wave", Offset: Next},
	{Name: "82s126.3m", Alt: []string{"pm1-2.3m"}, Size: 256, Role: "timing", Offset: Next, Optional: true},
}

var pacmanPromsNamco = []File{
	{Name: "pm1-1.7f", Alt: []string{"82s123.7f"}, Size: 32, Role: "colour", Offset: Next},
	{Name: "pm1-4.4a", Alt: []string{"82s126.4a"}, Size: 256, Role: "lookup", Offset: Next},
	{Name: "pm1-3.1m", Alt: []string{"82s126.1m"}, Size: 256, Role: "wave", Offset: Next},
	{Name: "pm1-2.3m", Alt: []string{"82s126.3m"}, Size: 256, Role: "timing", Offset: Next, Optional: true},
}

func files(lists ...[]File) []File {
	var out []File
	for _, l := range lists {
		out = append(out, l...)
	}
	return out
}

var offOpts = []DipOption{{"Off", 0}, {"On", 1}}

func init() {
	Register(&BoardDef{
		ID: "pacman", Title: "Namco Pac-Man", Width: pmW, Height: pmH, Rotate: 90,
		FPS:     float64(pmCPUHz) / (pmLineCyc * pmLines),
		Buttons: []string{"Up", "Down", "Left", "Right", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips: []Dip{
			{ID: "coins", Label: "Coins", Default: 1, Options: []DipOption{{"1 coin, 1 game", 1}, {"1 coin, 2 games", 2}, {"2 coins, 1 game", 3}, {"Free play", 0}}},
			{ID: "lives", Label: "Lives", Default: 2, Options: []DipOption{{"1", 0}, {"2", 1}, {"3", 2}, {"5", 3}}},
			{ID: "bonus", Label: "Extra life at", Default: 0, Options: []DipOption{{"10,000 points", 0}, {"15,000 points", 1}, {"20,000 points", 2}, {"No extra life", 3}}},
			{ID: "difficulty", Label: "Difficulty", Default: 1, Options: []DipOption{{"Normal", 1}, {"Hard", 0}}},
			{ID: "names", Label: "Ghost names", Default: 1, Options: []DipOption{{"Normal", 1}, {"Alternate", 0}}},
			{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []DipOption{{"Upright", 0}, {"Cocktail table", 1}}},
			{ID: "rack", Label: "Rack test (skip levels)", Default: 0, Options: offOpts},
			{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: offOpts},
		},
		Games: []Game{
			{ID: "puckman", Title: "Puck Man", Year: 1980, Maker: "Namco", Files: files([]File{
				{Name: "pm1_prg1.6e", Size: 0x800, Role: "cpu", Offset: 0x0000}, {Name: "pm1_prg2.6k", Size: 0x800, Role: "cpu", Offset: 0x0800},
				{Name: "pm1_prg3.6f", Size: 0x800, Role: "cpu", Offset: 0x1000}, {Name: "pm1_prg4.6m", Size: 0x800, Role: "cpu", Offset: 0x1800},
				{Name: "pm1_prg5.6h", Size: 0x800, Role: "cpu", Offset: 0x2000}, {Name: "pm1_prg6.6n", Size: 0x800, Role: "cpu", Offset: 0x2800},
				{Name: "pm1_prg7.6j", Size: 0x800, Role: "cpu", Offset: 0x3000}, {Name: "pm1_prg8.6p", Size: 0x800, Role: "cpu", Offset: 0x3800},
				{Name: "pm1_chg1.5e", Size: 0x800, Role: "tiles", Offset: 0}, {Name: "pm1_chg2.5h", Size: 0x800, Role: "tiles", Offset: 0x800},
				{Name: "pm1_chg3.5f", Size: 0x800, Role: "sprites", Offset: 0}, {Name: "pm1_chg4.5j", Size: 0x800, Role: "sprites", Offset: 0x800},
			}, pacmanPromsNamco)},
			// older dumps of the same game with 4 KB program chips
			{ID: "puckman", Title: "Puck Man", Year: 1980, Maker: "Namco", Files: files([]File{
				{Name: "namcopac.6e", Size: 0x1000, Role: "cpu", Offset: 0x0000}, {Name: "namcopac.6f", Size: 0x1000, Role: "cpu", Offset: 0x1000},
				{Name: "namcopac.6h", Size: 0x1000, Role: "cpu", Offset: 0x2000}, {Name: "namcopac.6j", Size: 0x1000, Role: "cpu", Offset: 0x3000},
				{Name: "pacman.5e", Size: 0x1000, Role: "tiles", Offset: Next}, {Name: "pacman.5f", Size: 0x1000, Role: "sprites", Offset: Next},
			}, pacmanProms)},
			{ID: "pacman", Title: "Pac-Man", Year: 1980, Maker: "Namco (Midway license)", Parent: "puckman", Files: files([]File{
				{Name: "pacman.6e", Size: 0x1000, Role: "cpu", Offset: 0x0000}, {Name: "pacman.6f", Size: 0x1000, Role: "cpu", Offset: 0x1000},
				{Name: "pacman.6h", Size: 0x1000, Role: "cpu", Offset: 0x2000}, {Name: "pacman.6j", Size: 0x1000, Role: "cpu", Offset: 0x3000},
				{Name: "pacman.5e", Size: 0x1000, Role: "tiles", Offset: Next}, {Name: "pacman.5f", Size: 0x1000, Role: "sprites", Offset: Next},
			}, pacmanProms)},
		},
		New: newPacman,
	})
}
