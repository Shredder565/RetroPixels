package arcade

// Namco Galaxian hardware: a port of web/emu/arcade_galaxian.js.
//
// One Z80 at 3.072 MHz with up to 16 KB of program ROM, 1 KB of work RAM, a
// 32x32 tile screen where each tile column has its own scroll and colour, 8
// hardware sprites, 8 bullets drawn by the hardware itself, a twinkling
// starfield, and colours from one 32-byte PROM. The interrupt is the NMI at
// the start of vertical blank. The monitor is on its side: the board draws
// 256x224 and the framework turns the picture upright. The sound is
// RetroPixels' own simpler version of the board's analog circuits.

import (
	"math"

	"retropixels/cores/z80"
)

const (
	gxCPUHz   = 3072000
	gxLineCyc = 192
	gxLines   = 264
	gxFirst   = 16
	gxLast    = 240
	gxW       = 256
	gxH       = gxLast - gxFirst
)

var gxFPS = float64(gxCPUHz) / (gxLineCyc * gxLines)

type gxStar struct{ x, colour, group int }

// gxStars: RetroPixels' own starfield, by line.
var gxStars = func() [256][]gxStar {
	var out [256][]gxStar
	r := 0x1d872
	for y := 0; y < 256; y++ {
		for x := 0; x < 512; x++ {
			bit := ((r >> 16) ^ (r >> 4)) & 1
			r = ((r << 1) | bit) & 0x1ffff
			v := r
			if v&0x1fe00 == 0x1fe00 && v&0x3f != 0 {
				out[y] = append(out[y], gxStar{x, v & 0x3f, (v >> 6) & 3})
			}
		}
	}
	return out
}()

var gxStarLevels = [4]int{0x00, 0x97, 0xd7, 0xff}

type gxSound struct {
	tone            float64
	bg              [3]float64
	noise           int
	hitEnv, fireEnv float64
	fireF, firePh   float64
	lp, due         float64
}

type galaxian struct {
	dips       map[string]int
	sampleRate float64
	rom        []byte
	tiles      []uint8
	sprites    []uint8
	pens       [32]uint32
	starPens   [64]uint32
	ram, vram  [0x400]uint8
	obj        [0x100]uint8
	l60, l68   [8]uint8
	l70        [8]uint8
	screen     []uint32
	line       [gxW]bool
	p1, p2     Pad
	audio      []float32
	cpu        *z80.CPU
	pitch      int
	cycles     int
	frame      int
	snd        gxSound
}

type gxBus struct{ b *galaxian }

func (x gxBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x gxBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }
func (x gxBus) In(uint16) uint8         { return 0xff }
func (x gxBus) Out(uint16, uint8)       {}

func newGalaxian(g *Game, files Files, opts Opts) (Board, error) {
	b := &galaxian{dips: opts.Dips, sampleRate: float64(opts.SampleRate)}
	b.rom = Region(g, files, "cpu", 0x4000)
	gfx := Region(g, files, "gfx", 0x1000)
	prom := Region(g, files, "colour", 32)
	b.decodeGraphics(gfx)
	for i := range b.pens {
		b.pens[i] = promColour(prom[i])
	}
	for i := range b.starPens {
		b.starPens[i] = RGB(gxStarLevels[i&3], gxStarLevels[(i>>2)&3], gxStarLevels[i>>4])
	}
	b.screen = make([]uint32, gxW*gxH)
	b.p1, b.p2 = Pad{}, Pad{}
	b.audio = make([]float32, 0, 2048)
	b.cpu = z80.New(gxBus{b})
	b.Reset()
	return b, nil
}

func (b *galaxian) Reset() {
	b.cpu.Reset()
	b.ram, b.vram, b.obj = [0x400]uint8{}, [0x400]uint8{}, [0x100]uint8{}
	b.l60, b.l68, b.l70 = [8]uint8{}, [8]uint8{}, [8]uint8{}
	b.pitch, b.cycles, b.frame = 0xff, 0, 0
	b.snd = gxSound{noise: 1}
}

// two ROMs: the first gives each pixel's high bit, the second its low bit
func (b *galaxian) decodeGraphics(gfx []byte) {
	half := len(gfx) >> 1
	px := func(o, bit int) uint8 { return ((gfx[o]>>bit)&1)<<1 | (gfx[o+half]>>bit)&1 }
	b.tiles = make([]uint8, 256*64)
	for t := 0; t < 256; t++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				b.tiles[t*64+y*8+x] = px(t*8+y, 7-x)
			}
		}
	}
	b.sprites = make([]uint8, 64*256)
	for s := 0; s < 64; s++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				b.sprites[s*256+y*16+x] = px(s*32+(y&7)+(x&8)+((y&8)<<1), 7-(x&7))
			}
		}
	}
}

var (
	gxWhite  = RGB(0xff, 0xff, 0xff)
	gxYellow = RGB(0xff, 0xff, 0x00)
)

func (b *galaxian) drawLine(y int) {
	scr, row, obj, used := b.screen, (y-gxFirst)*gxW, &b.obj, &b.line
	*used = [gxW]bool{}
	for col := 0; col < 32; col++ {
		ty := (y + int(obj[col*2])) & 0xff
		pal := int(obj[col*2+1]&7) << 2
		t := int(b.vram[((ty>>3)<<5)|col])*64 + (ty&7)*8
		for x := 0; x < 8; x++ {
			p := b.tiles[t+x]
			o := col*8 + x
			scr[row+o] = b.pens[pal|int(p)]
			used[o] = p != 0
		}
	}
	for n := 7; n >= 0; n-- {
		base0 := 0x40 + n*4
		adj := 0
		if n < 3 {
			adj = 1
		}
		sy := (240 - int(obj[base0]) + adj) & 0xff
		r := (y - sy) & 0xff
		if r >= 16 {
			continue
		}
		attr := int(obj[base0+1])
		pal := int(obj[base0+2]&7) << 2
		sx := int(obj[base0+3]) + 1
		yy, fx := r, 0
		if attr&0x80 != 0 {
			yy = 15 - r
		}
		if attr&0x40 != 0 {
			fx = 15
		}
		base := (attr&0x3f)*256 + yy*16
		for x := 0; x < 16; x++ {
			o := sx + x
			if o >= gxW {
				break
			}
			if p := b.sprites[base+(x^fx)]; p != 0 {
				scr[row+o] = b.pens[pal|int(p)]
				used[o] = true
			}
		}
	}
	shell, missile := -1, -1
	for n := 0; n < 8; n++ {
		match := y
		if n < 3 {
			match = y - 1
		}
		match &= 0xff
		if (int(obj[0x60+n*4+1])+match)&0xff != 0xff {
			continue
		}
		if n == 7 {
			missile = n
		} else {
			shell = n
		}
	}
	for _, n := range [2]int{shell, missile} {
		if n < 0 {
			continue
		}
		x0 := 255 - int(obj[0x60+n*4+3]) - 4
		c := gxWhite
		if n == 7 {
			c = gxYellow
		}
		for x := x0; x < x0+4; x++ {
			if x >= 0 && x < gxW {
				scr[row+x] = c
				used[x] = true
			}
		}
	}
	if b.l70[4] != 0 {
		scroll, f := b.frame&511, b.frame
		for _, s := range gxStars[y] {
			if ((f>>5)+s.group)&2 != 0 {
				continue
			}
			x := (s.x + scroll) & 511
			if x < gxW && !used[x] {
				scr[row+x] = b.starPens[s.colour]
			}
		}
	}
}

func (b *galaxian) flipPicture() {
	s := b.screen
	if b.l70[6] != 0 {
		for y := 0; y < gxH; y++ {
			r := s[y*gxW : y*gxW+gxW]
			for i, j := 0, gxW-1; i < j; i, j = i+1, j-1 {
				r[i], r[j] = r[j], r[i]
			}
		}
	}
	if b.l70[7] != 0 {
		tmp := make([]uint32, gxW)
		for y := 0; y < gxH>>1; y++ {
			copy(tmp, s[y*gxW:y*gxW+gxW])
			copy(s[y*gxW:], s[(gxH-1-y)*gxW:(gxH-y)*gxW])
			copy(s[(gxH-1-y)*gxW:], tmp)
		}
	}
}

func (b *galaxian) makeSound(n int) {
	st, rate, l68 := &b.snd, b.sampleRate, &b.l68
	toneHz := 0.0
	if b.pitch != 0xff {
		toneHz = 96000 / float64(2*(256-b.pitch))
	}
	toneVol := 0.06 * float64(1+int(l68[6])+2*int(l68[7]))
	lfo := int(b.l60[4]) | int(b.l60[5])<<1 | int(b.l60[6])<<2 | int(b.l60[7])<<3
	bgBase := float64(140 + lfo*22)
	for i := 0; i < n; i++ {
		v := 0.0
		if toneHz != 0 {
			st.tone = math.Mod(st.tone+toneHz/rate, 1)
			if st.tone < 0.5 {
				v += toneVol
			} else {
				v += -toneVol
			}
		}
		for k := 0; k < 3; k++ {
			if l68[k] == 0 {
				continue
			}
			st.bg[k] = math.Mod(st.bg[k]+bgBase*(1+float64(k)*0.26)/rate, 1)
			if st.bg[k] < 0.5 {
				v += 0.035
			} else {
				v += -0.035
			}
		}
		if st.hitEnv > 0.001 {
			bit := (st.noise ^ (st.noise >> 2)) & 1
			st.noise = (st.noise >> 1) | (bit << 16)
			x := -1.0
			if st.noise&1 != 0 {
				x = 1
			}
			st.lp += (x - st.lp) * 0.08
			v += st.lp * st.hitEnv * 0.5
			if l68[3] != 0 {
				st.hitEnv *= 0.99996
			} else {
				st.hitEnv *= 0.9997
			}
		}
		if st.fireEnv > 0.001 {
			st.firePh = math.Mod(st.firePh+st.fireF/rate, 1)
			x := -1.0
			if st.firePh < 0.5 {
				x = 1
			}
			v += x * st.fireEnv * 0.18
			st.fireF *= 0.99993
			st.fireEnv *= 0.99985
		}
		b.audio = append(b.audio, float32(v))
	}
}

func (b *galaxian) TakeAudio() []float32 {
	out := append([]float32(nil), b.audio...)
	b.audio = b.audio[:0]
	return out
}

func (b *galaxian) read(a int) uint8 {
	a &= 0x7fff
	switch {
	case a < 0x4000:
		return b.rom[a]
	case a < 0x4800:
		return b.ram[a&0x3ff]
	case a < 0x5000:
		return 0xff
	case a < 0x5800:
		return b.vram[a&0x3ff]
	case a < 0x6000:
		return b.obj[a&0xff]
	case a < 0x6800:
		return b.readIn0()
	case a < 0x7000:
		return b.readIn1()
	case a < 0x7800:
		d := b.dips
		return uint8(d["bonus"]&3 | (d["lives"]&1)<<2)
	}
	return 0xff
}

func (b *galaxian) write(a int, v uint8) {
	a &= 0x7fff
	switch {
	case a < 0x4000:
	case a < 0x4800:
		b.ram[a&0x3ff] = v
	case a < 0x5000:
	case a < 0x5800:
		b.vram[a&0x3ff] = v
	case a < 0x6000:
		b.obj[a&0xff] = v
	case a < 0x6800:
		b.l60[a&7] = v & 1
	case a < 0x7000:
		n := a & 7
		was := b.l68[n]
		b.l68[n] = v & 1
		if n == 3 && v&1 != 0 && was == 0 {
			b.snd.hitEnv = 1
		}
		if n == 5 && v&1 != 0 && was == 0 {
			b.snd.fireEnv = 1
			b.snd.fireF = 1800
		}
	case a < 0x7800:
		b.l70[a&7] = v & 1
	default:
		b.pitch = int(v)
	}
}

func bit(on bool, v int) int {
	if on {
		return v
	}
	return 0
}

func (b *galaxian) readIn0() uint8 {
	p, d := b.p1, b.dips
	return uint8(bit(p["Coin"], 1) | bit(p["Coin 2"], 2) | bit(p["Left"], 4) | bit(p["Right"], 8) | bit(p["Fire"], 0x10) |
		bit(d["cabinet"] != 0, 0x20) | bit(d["test"] != 0, 0x40) | bit(p["Service"], 0x80))
}

func (b *galaxian) readIn1() uint8 {
	p, q, d := b.p1, b.p2, b.dips
	return uint8(bit(p["Start (1P)"] || q["Start (1P)"], 1) | bit(p["Start (2P)"] || q["Start (2P)"], 2) |
		bit(q["Left"], 4) | bit(q["Right"], 8) | bit(q["Fire"], 0x10) | (d["coins"]&3)<<6)
}

func (b *galaxian) SetInput(p []Pad) {
	b.p1 = p[0]
	b.p2 = Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

func (b *galaxian) Screen() []uint32 { return b.screen }

func (b *galaxian) RunFrame() {
	cpu := b.cpu
	perLine := b.sampleRate / (gxFPS * gxLines)
	for y := 0; y < gxLines; y++ {
		if y == gxLast {
			b.flipPicture()
			if b.l70[1] != 0 {
				cpu.NMI()
			}
		}
		end := b.cycles + gxLineCyc
		for b.cycles < end {
			b.cycles += cpu.Step()
		}
		if y >= gxFirst && y < gxLast {
			b.drawLine(y)
		}
		b.snd.due += perLine
		n := math.Floor(b.snd.due)
		b.snd.due -= n
		b.makeSound(int(n))
	}
	b.cycles -= gxLines * gxLineCyc
	b.frame++
}

func (b *galaxian) SaveState() ([]byte, error) {
	w := &StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.obj[:])
	w.Bytes(b.l60[:])
	w.Bytes(b.l68[:])
	w.Bytes(b.l70[:])
	w.Ints([]int{b.pitch, b.cycles, b.frame, b.snd.noise})
	s := &b.snd
	w.F64s([]float64{s.tone, s.bg[0], s.bg[1], s.bg[2], s.hitEnv, s.fireEnv, s.fireF, s.firePh, s.lp, s.due})
	return w.B, nil
}

func (b *galaxian) LoadState(st []byte) error {
	rest, err := b.cpu.LoadState(st)
	if err != nil {
		return err
	}
	r := &StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.obj[:])
	r.Bytes(b.l60[:])
	r.Bytes(b.l68[:])
	r.Bytes(b.l70[:])
	m := make([]int, 4)
	r.Ints(m)
	b.pitch, b.cycles, b.frame, b.snd.noise = m[0], m[1], m[2], m[3]
	f := make([]float64, 10)
	r.F64s(f)
	s := &b.snd
	s.tone, s.bg[0], s.bg[1], s.bg[2], s.hitEnv, s.fireEnv, s.fireF, s.firePh, s.lp, s.due = f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]
	if err := r.Done(); err != nil {
		return err
	}
	b.audio = b.audio[:0]
	for y := gxFirst; y < gxLast; y++ {
		b.drawLine(y)
	}
	b.flipPicture()
	return nil
}

func init() {
	fs := []File{
		{Name: "galmidw.u", Size: 0x800, Role: "cpu", Offset: 0x0000},
		{Name: "galmidw.v", Size: 0x800, Role: "cpu", Offset: 0x0800},
		{Name: "galmidw.w", Size: 0x800, Role: "cpu", Offset: 0x1000},
		{Name: "galmidw.y", Size: 0x800, Role: "cpu", Offset: 0x1800},
		{Name: "7l", Alt: []string{"galmidw.z", "7l.bin"}, Size: 0x800, Role: "cpu", Offset: 0x2000},
		{Name: "1h.bin", Alt: []string{"1h", "galmidw.1j", "galaxian.1j"}, Size: 0x800, Role: "gfx", Offset: 0},
		{Name: "1k.bin", Alt: []string{"1k", "galmidw.1k", "galaxian.1k"}, Size: 0x800, Role: "gfx", Offset: 0x800},
		{Name: "6l.bpr", Alt: []string{"6l", "galaxian.6l"}, Size: 0x20, Role: "colour", Offset: Next},
	}
	Register(&BoardDef{
		ID: "galaxian", Title: "Namco Galaxian", Width: gxW, Height: gxH, Rotate: 90, FPS: gxFPS,
		Buttons: []string{"Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips: []Dip{
			{ID: "coins", Label: "Coins", Default: 0, Options: []DipOption{{"1 coin, 1 game", 0}, {"2 coins, 1 game", 1}, {"1 coin, 2 games", 2}, {"Free play", 3}}},
			{ID: "bonus", Label: "Extra ship at", Default: 0, Options: []DipOption{{"7,000 points", 0}, {"10,000 points", 1}, {"12,000 points", 2}, {"20,000 points", 3}}},
			{ID: "lives", Label: "Ships", Default: 1, Options: []DipOption{{"2", 0}, {"3", 1}}},
			{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []DipOption{{"Upright", 0}, {"Cocktail table", 1}}},
			{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: offOpts},
		},
		Games: []Game{
			{ID: "galaxian", Title: "Galaxian", Year: 1979, Maker: "Namco", Files: fs},
			{ID: "galmidw", Title: "Galaxian (Midway)", Year: 1979, Maker: "Namco (Midway license)", Parent: "galaxian", Files: fs},
		},
		New: newGalaxian,
	})
}
