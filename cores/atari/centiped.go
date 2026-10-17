// Package atari holds RetroPixels' Atari arcade boards: Centipede (raster,
// POKEY sound, trackball) and Asteroids Deluxe (the Digital Vector
// Generator, POKEY and discrete sound). They register themselves with the
// arcade package.
package atari

import (
	"retropixels/cores/arcade"
	"retropixels/cores/cpu6502"
	"retropixels/cores/pokey"
)

// Centipede (Atari, 1980).
//
// One 6502 at 1.512 MHz (12.096 MHz / 8) with 8 KB of program ROM at
// $2000-$3FFF (the processor's top two address lines aren't connected, so
// the vectors are read from $3FFA-$3FFF), 1 KB of work RAM, a 32x30 tile
// playfield with 16 sprites of 8x16 pixels, 16 colour registers, a POKEY for
// the sound and the random numbers, a trackball, and an ER2055 EAROM for the
// high scores. The IRQ comes four times a frame (every 64 lines) and stays
// up until the game acknowledges it; the one at line 240 is the vertical
// blank, when the game moves the sprites. The monitor is on its side: the
// board draws 256x240 and the framework turns it upright.

const (
	cnCPUHz    = 1512000
	cnFPS      = 60
	cnLines    = 256
	cnFrameCyc = cnCPUHz / cnFPS // 25200
	cnW        = 256
	cnH        = 240
	cnVblank   = 240
	cnSampleN  = 32 // POKEY clocks per sample (47.25 kHz)
)

type centiped struct {
	dips  map[string]int
	rom   []byte // $2000-$3FFF
	tiles []uint8
	spr   []uint8

	ram    [0x400]uint8 // $0000-$03FF
	vram   [0x400]uint8 // $0400-$07FF: playfield, then the sprites at $07C0
	palram [16]uint8
	pens   []uint8  // the picture's pens (0-15), before colouring
	latch  [8]uint8 // $1C00-$1C07: coin counters, LEDs, flip
	er     earom
	snd    *pokey.Chip
	out    pokeyOut

	cpu      *cpu6502.CPU
	frameCyc int64 // the CPU's cycle count at the start of this frame
	irqAt    int64 // the cycle the next interrupt is raised on
	dx, dy   int   // this frame's trackball rolls (-1, 0, 1)
	sndCyc   int   // CPU cycles not yet given to the POKEY
	watchdog int   // frames since the game last fed the watchdog

	ball   [2]trackball
	pads   []arcade.Pad
	screen []uint32
	res    *arcade.Resampler
}

// trackball is one axis: a 4-bit counter the game reads, and the
// direction it last moved (bit 7 of the port). The pad's buttons roll it
// faster the longer they're held.
type trackball struct {
	pos  int
	dir  uint8
	held int
	frac int
}

// roll moves the counter for one interrupt (four times a frame); d is -1, 0 or 1.
func (t *trackball) roll(d int) {
	if d == 0 {
		t.held, t.frac = 0, 0
		return
	}
	t.held++
	// speed in 1/4 counts per interrupt: 2 at first, 6 after half a second
	speed := min(2+t.held/30, 6)
	t.frac += speed
	n := t.frac >> 2
	t.frac &= 3
	t.pos = (t.pos + n*d) & 15
	if d < 0 {
		t.dir = 0x80
	} else {
		t.dir = 0
	}
}

func (t *trackball) port() uint8 { return t.dir | uint8(t.pos&15) }

type cnBus struct{ b *centiped }

func (x cnBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x cnBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }

func newCentiped(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &centiped{dips: opts.Dips}
	b.rom = arcade.Region(g, files, "cpu", 0x2000)
	b.decodeGraphics(arcade.Region(g, files, "gfx", 0x1000))
	if len(opts.SRAM) == len(b.er.mem) {
		copy(b.er.mem[:], opts.SRAM)
	}
	b.snd = newPOKEY()
	b.cpu = cpu6502.New(cnBus{b}, cpu6502.Options{Decimal: true, PollAtBoundary: true})
	b.screen = make([]uint32, cnW*cnH)
	b.pens = make([]uint8, cnW*cnH)
	b.res = arcade.NewResampler(cnCPUHz/cnSampleN, float64(opts.SampleRate))
	b.pads = []arcade.Pad{{}, {}}
	b.Reset()
	return b, nil
}

// decodeGraphics: the two ROMs are the two bit planes (the first one the low
// bit). Tiles are 8x8 (8 bytes), sprites 8x16 (16 bytes); a byte's top bit
// is the leftmost pixel.
func (b *centiped) decodeGraphics(gfx []byte) {
	half := len(gfx) / 2
	px := func(o, x int) uint8 { return (gfx[o]>>(7-x))&1 | (gfx[o+half]>>(7-x))&1<<1 }
	b.tiles = make([]uint8, 256*64)
	for t := 0; t < 256; t++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				b.tiles[t*64+y*8+x] = px(t*8+y, x)
			}
		}
	}
	b.spr = make([]uint8, 128*128)
	for s := 0; s < 128; s++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 8; x++ {
				b.spr[s*128+y*8+x] = px(s*16+y, x)
			}
		}
	}
}

func (b *centiped) Reset() {
	b.ram, b.vram, b.palram, b.latch = [0x400]uint8{}, [0x400]uint8{}, [16]uint8{}, [8]uint8{}
	mem := b.er.mem
	b.er = earom{mem: mem}
	start := b.cpu.Cycles
	*b.snd = *newPOKEY()
	// RANDOM as MAME reads it: up to date at each access, IRQ acknowledge,
	// 16 lines and 1/50 second (Centipede draws its mushroom fields from it).
	b.snd.SyncEvery, b.snd.SyncBase = [2]int64{cnFrameCyc / 16, cnCPUHz / 50}, start
	b.out = pokeyOut{}
	b.cpu.IRQLine = false
	b.cpu.Reset()
	// The frame (and the first interrupt) starts at power-on. A bus access
	// that brings the cycle count to n here is MAME's cycle n from power-on
	// (its reset sequence takes a cycle more, its count starts a cycle
	// later), so the timing matches MAME's cycle for cycle.
	b.frameCyc, b.irqAt = start, start
	b.sndCyc, b.watchdog = 0, 0
	// at power-on the across counter reads $F, direction bit set, and the
	// other 0 (as in MAME)
	b.ball = [2]trackball{{pos: 15, dir: 0x80}, {}}
	b.res.Clear()
}

// clock raises the interrupt on its cycle (every 64 lines from the start
// of the vertical blank) and rolls the trackball; every bus access (so every
// cycle) calls it, so the CPU sees the line as it would on the board.
func (b *centiped) clock() {
	// The interrupt is taken at the first instruction boundary at or after
	// its cycle (the CPU samples at boundaries, as in MAME); the next
	// instruction's fetch is a cycle on.
	if b.cpu.Cycles >= b.irqAt-1 {
		b.irqAt += cnFrameCyc / 4
		b.cpu.IRQLine = true
		b.ball[0].roll(b.dx)
		b.ball[1].roll(b.dy)
	}
}

func (b *centiped) read(a int) uint8 {
	b.clock()
	a &= 0x3fff
	switch {
	case a < 0x400:
		return b.ram[a]
	case a < 0x800:
		return b.vram[a-0x400]
	case a >= 0x2000:
		return b.rom[a-0x2000]
	case a == 0x800:
		return b.dsw(1)
	case a == 0x801:
		return b.dsw(2)
	case a >= 0xc00 && a < 0xc04:
		return b.readInput(a & 3)
	case a >= 0x1000 && a < 0x1010:
		return b.snd.Read(a, b.cpu.Cycles)
	case a >= 0x1700 && a < 0x1740:
		return b.er.read()
	}
	return 0
}

func (b *centiped) write(a int, v uint8) {
	b.clock()
	a &= 0x3fff
	switch {
	case a < 0x400:
		b.ram[a] = v
	case a < 0x800:
		b.vram[a-0x400] = v
	case a >= 0x1000 && a < 0x1010:
		b.snd.WriteAt(a, v, b.cpu.Cycles)
	case a >= 0x1400 && a < 0x1410:
		b.palram[a&15] = v
	case a >= 0x1600 && a < 0x1640:
		b.er.latch(a, v)
	case a == 0x1680:
		b.er.control(v)
	case a == 0x1800:
		b.cpu.IRQLine = false
		b.snd.Sync(b.cpu.Cycles)
	case a >= 0x1c00 && a < 0x1c08:
		b.latch[a&7] = v >> 7
	case a == 0x2000:
		b.watchdog = 0
	}
}

// line is the beam's line now (0-255; 240 on is the vertical blank).
func (b *centiped) line() int {
	return int(((b.cpu.Cycles-b.frameCyc)*cnLines/cnFrameCyc + cnVblank) % cnLines)
}

func (b *centiped) readInput(n int) uint8 {
	p1 := b.pads[0]
	low := func(on bool, bit uint8) uint8 {
		if on {
			return 0
		}
		return bit
	}
	switch n {
	case 0: // trackball across, the vertical blank, the test switch
		v := b.ball[0].port() | 0x10
		if b.line() >= cnVblank {
			v |= 0x40
		}
		if b.dips["test"] == 0 {
			v |= 0x20
		}
		return v
	case 1: // coins, starts, fire buttons
		v := low(p1["Start (1P)"], 0x01) | low(p1["Start (2P)"], 0x02) | low(p1["Fire"], 0x04) |
			low(b.pads[1]["Fire"], 0x08) | 0x10 | low(p1["Coin"], 0x20) | low(p1["Coin 2"], 0x40) | low(p1["Service"], 0x80)
		return v
	case 2: // trackball up and down
		return b.ball[1].port()
	}
	return 0xff // the joysticks (not used: the pad drives the trackball)
}

func (b *centiped) SetInput(p []arcade.Pad) { b.pads = p }

func (b *centiped) Screen() []uint32 { return b.screen }

func (b *centiped) RunFrame() {
	p := b.pads[0]
	dir := func(neg, pos string) int {
		switch {
		case p[neg] && !p[pos]:
			return -1
		case p[pos] && !p[neg]:
			return 1
		}
		return 0
	}
	// The trackball's counters, as the upright screen sees them: one counts
	// up going left, the other up going down.
	b.dx, b.dy = dir("Right", "Left"), dir("Up", "Down")
	// A frame here runs from the start of the vertical blank (line 240, where
	// power-on falls, as in MAME) to the next, and shows the picture drawn.
	end := b.frameCyc + cnFrameCyc
	for b.cpu.Cycles < end {
		n := b.cpu.Step()
		b.sound(n)
	}
	b.draw()
	b.frameCyc = end
	b.watchdog++
	if b.watchdog > 16 { // the game stopped: the watchdog resets it
		b.watchdog = 0
		b.cpu.Reset()
	}
}

func (b *centiped) sound(n int) {
	b.sndCyc += n
	for b.sndCyc >= cnSampleN {
		b.sndCyc -= cnSampleN
		b.res.Add(b.out.next(b.snd.Run(cnSampleN)))
	}
}

func (b *centiped) TakeAudio() []float32 { return b.res.Take() }

// cnColour: a colour register. The bits are low for on: 0 red, 1 green,
// 2 blue; bit 3 low dims the blue, or the green.
func cnColour(v uint8) uint32 {
	var r, g, bl int
	if v&1 == 0 {
		r = 0xff
	}
	if v&2 == 0 {
		g = 0xff
	}
	if v&4 == 0 {
		bl = 0xff
	}
	// bit 3 low dims the blue, or the green when there is no blue (as MAME
	// shows them); red alone isn't dimmed
	if v&8 == 0 {
		switch {
		case bl != 0:
			bl = 0xc0
		case g != 0:
			g = 0xc0
		}
	}
	return arcade.RGB(r, g, bl)
}

// draw renders the whole picture: the playfield, then the sprites on top.
func (b *centiped) draw() {
	var pens [16]uint32
	for i, v := range b.palram {
		pens[i] = cnColour(v)
	}
	b.render(b.pens)
	for i, p := range b.pens {
		b.screen[i] = pens[p]
	}
}

// render draws the playfield and sprites into s as pens (0-15).
func (b *centiped) render(s []uint8) {
	for row := 0; row < 30; row++ {
		for col := 0; col < 32; col++ {
			code := int(b.vram[row*32+col])
			t := b.tiles[((code&0x3f)|0x40)*64:]
			fx, fy := 0, 0
			if code&0x40 != 0 {
				fx = 7
			}
			if code&0x80 != 0 {
				fy = 7
			}
			for y := 0; y < 8; y++ {
				o := (row*8+y)*cnW + col*8
				src := t[(y^fy)*8:]
				for x := 0; x < 8; x++ {
					s[o+x] = 4 + src[x^fx]
				}
			}
		}
	}
	for n := 0; n < 16; n++ {
		code := int(b.vram[0x3c0+n])
		sy := 240 - int(b.vram[0x3d0+n])
		sx := int(b.vram[0x3e0+n])
		colr := int(b.vram[0x3f0+n])
		img := b.spr[(code>>1&0x1f|(code&1)<<6)*128:]
		fx, fy := 0, 0
		if code&0x40 != 0 {
			fx = 7
		}
		if code&0x80 != 0 {
			fy = 15
		}
		for y := 0; y < 16; y++ {
			py := sy + y
			if py < 0 || py >= cnH {
				continue
			}
			src := img[(y^fy)*8:]
			for x := 0; x < 8; x++ {
				px := sx + x
				if px >= cnW-8 { // the score line (the last 8 columns) has no sprites
					break
				}
				if c := src[x^fx]; c != 0 {
					s[py*cnW+px] = uint8(12 + (colr>>(2*(c-1)))&3)
				}
			}
		}
	}
}

func (b *centiped) SRAM() []byte {
	return append([]byte(nil), b.er.mem[:]...)
}

func (b *centiped) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes(b.cpu.SaveState())
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.palram[:])
	w.Bytes(b.latch[:])
	b.er.save(w)
	w.B = b.snd.AppendState(w.B)
	w.F64(b.out.lp)
	w.Int(int(b.cpu.Cycles - b.frameCyc))
	w.Int(int(b.irqAt - b.frameCyc))
	w.Int(b.sndCyc)
	w.Int(b.watchdog)
	for _, t := range b.ball {
		w.Ints([]int{t.pos, int(t.dir), t.held, t.frac})
	}
	b.res.AppendState(w)
	return w.B, nil
}

func (b *centiped) LoadState(st []byte) error {
	if len(st) < cpu6502.StateSize {
		return arcade.ErrBadState
	}
	n := *b
	cpu := *b.cpu
	snd := *b.snd
	n.cpu, n.snd = &cpu, &snd
	cpu.SetBus(cnBus{&n})
	if err := cpu.LoadState(st[:cpu6502.StateSize]); err != nil {
		return err
	}
	r := &arcade.StateR{B: st[cpu6502.StateSize:]}
	r.Bytes(n.ram[:])
	r.Bytes(n.vram[:])
	r.Bytes(n.palram[:])
	r.Bytes(n.latch[:])
	n.er.load(r)
	if r.Err == nil {
		rest, err := snd.LoadState(r.B)
		if err != nil {
			return arcade.ErrBadState
		}
		r.B = rest
	}
	n.out.lp = r.F64()
	into := int64(r.Int())
	irq := int64(r.Int())
	n.sndCyc = r.Int()
	n.watchdog = r.Int()
	for i := range n.ball {
		v := make([]int, 4)
		r.Ints(v)
		n.ball[i] = trackball{v[0], uint8(v[1]), v[2], v[3]}
	}
	res := *b.res
	n.res = &res
	n.res.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	// keep the board's own objects (the framework holds b)
	*b.cpu = cpu
	b.cpu.SetBus(cnBus{b})
	*b.snd = snd
	*b.res = res
	n.cpu, n.snd, n.res = b.cpu, b.snd, b.res
	*b = n
	b.frameCyc = b.cpu.Cycles - into
	b.irqAt = b.frameCyc + irq
	b.draw()
	return nil
}

// The switches, each option's value being its bits in its bank (as the game
// reads them).
var cnDips = []arcade.Dip{
	{ID: "language", Label: "Language", Default: 0, Options: []arcade.DipOption{dopt("English", 0), dopt("German", 1), dopt("French", 2), dopt("Spanish", 3)}},
	{ID: "lives", Label: "Lives", Default: 4, Options: []arcade.DipOption{dopt("2", 0), dopt("3", 4), dopt("4", 8), dopt("5", 0xc)}},
	{ID: "bonus", Label: "Bonus life", Default: 0x10, Options: []arcade.DipOption{dopt("10,000", 0), dopt("12,000", 0x10), dopt("15,000", 0x20), dopt("20,000", 0x30)}},
	{ID: "difficulty", Label: "Difficulty", Default: 0x40, Options: []arcade.DipOption{dopt("Easy", 0x40), dopt("Hard", 0)}},
	{ID: "minimum", Label: "Credit minimum", Default: 0, Options: []arcade.DipOption{dopt("1", 0), dopt("2", 0x80)}},
	{ID: "coinage", Label: "Coinage", Default: 2, Options: []arcade.DipOption{dopt("2 coins, 1 credit", 3), dopt("1 coin, 1 credit", 2), dopt("1 coin, 2 credits", 1), dopt("Free play", 0)}},
	{ID: "gametime", Label: "Game time", Default: 0, Options: []arcade.DipOption{dopt("Untimed", 0), dopt("1 minute", 4), dopt("2 minutes", 8), dopt("3 minutes", 0xc), dopt("4 minutes", 0x10), dopt("5 minutes", 0x14), dopt("6 minutes", 0x18), dopt("7 minutes", 0x1c)}},
	{ID: "bonuscoins", Label: "Bonus coins", Default: 0, Options: []arcade.DipOption{dopt("None", 0), dopt("3 credits for 2 coins", 0x20), dopt("5 credits for 4 coins", 0x40), dopt("6 credits for 4 coins", 0x60), dopt("6 credits for 5 coins", 0x80), dopt("4 credits for 3 coins", 0xa0)}},
	{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: []arcade.DipOption{dopt("Off", 0), dopt("On", 1)}},
}

// dsw: a switch bank as the game reads it.
func (b *centiped) dsw(bank int) uint8 {
	d := b.dips
	if bank == 1 {
		return uint8(d["language"] | d["lives"] | d["bonus"] | d["difficulty"] | d["minimum"])
	}
	return uint8(d["coinage"] | d["gametime"] | d["bonuscoins"])
}

func init() {
	files := []arcade.File{
		{Name: "136001-407.d1", Size: 0x800, Role: "cpu", Offset: 0x0000},
		{Name: "136001-408.e1", Size: 0x800, Role: "cpu", Offset: 0x0800},
		{Name: "136001-409.fh1", Size: 0x800, Role: "cpu", Offset: 0x1000},
		{Name: "136001-410.j1", Size: 0x800, Role: "cpu", Offset: 0x1800},
		{Name: "136001-211.f7", Size: 0x800, Role: "gfx", Offset: 0x0000},
		{Name: "136001-212.hj7", Size: 0x800, Role: "gfx", Offset: 0x0800},
		// a timing PROM the emulation doesn't need
		{Name: "136001-213.p4", Size: 0x100, Role: "prom", Offset: 0, Optional: true},
	}
	arcade.Register(&arcade.BoardDef{
		ID: "centiped", Title: "Atari Centipede", Width: cnW, Height: cnH, Rotate: 270, FPS: cnFPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    cnDips,
		Games: []arcade.Game{
			{ID: "centiped", Title: "Centipede (revision 4)", Year: 1980, Maker: "Atari", Files: files},
		},
		New: newCentiped,
	})
}
