// Package kangaroo is Sun Electronics' Kangaroo board (1982): a Z80 at
// 2.5 MHz that draws into two 4-bit bitmap planes (A, the playfield, which
// scrolls; B, the moving things) with a blitter that copies pictures out of
// the graphics ROMs, and a sound Z80 at 1.25 MHz with an AY-3-8910.
//
// The protection chip (an MB8841 microcontroller) is not needed: like MAME
// 0.264, the board answers its port with a counter that steps at each read,
// and gives the game the NMI the chip gives it at power-on.
package kangaroo

import (
	"retropixels/cores/arcade"
	"retropixels/cores/ay8910"
	"retropixels/cores/z80"
)

const (
	lines     = 260
	lineCyc   = 160 // main CPU clocks per line (2.5 MHz, 64 us lines)
	sndCyc    = 80  // sound CPU clocks per line (1.25 MHz)
	firstLine = 8
	lastLine  = 248 // vertical blank starts here
	scrW      = 512
	scrH      = lastLine - firstLine
	FPS       = 10000000.0 / 640 / lines
)

type board struct {
	rom, sndROM []byte
	gfx         []byte // the blitter's ROMs: two halves of 0x2000
	vram        [0x4000]uint32
	ctrl        [16]uint8
	ram         [0x400]uint8
	sndRAM      [0x400]uint8
	latch       uint8
	mcuClock    uint8
	bank        int
	main, snd   *z80.CPU
	ay          *ay8910.PSG
	out         *arcade.Resampler
	screen      []uint32
	p1, p2      arcade.Pad
	dips        map[string]int
	frame       int
	mainC, sndC int
}

type mainBus struct{ b *board }

func (x mainBus) Read(a uint16) uint8     { return x.b.read(a) }
func (x mainBus) Write(a uint16, v uint8) { x.b.write(a, v) }
func (x mainBus) In(uint16) uint8         { return 0xff }
func (x mainBus) Out(uint16, uint8)       {}
func (x mainBus) Ack()                    { x.b.main.IRQ = false }

type sndBus struct{ b *board }

func (x sndBus) Read(a uint16) uint8     { return x.b.sndRead(a) }
func (x sndBus) Write(a uint16, v uint8) { x.b.sndWrite(a, v) }
func (x sndBus) In(p uint16) uint8       { return x.b.sndRead(p) }
func (x sndBus) Out(p uint16, v uint8)   { x.b.sndWrite(p, v) }
func (x sndBus) Ack()                    { x.b.snd.IRQ = false }

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips}
	b.rom = arcade.Region(g, files, "cpu", 0x6000)
	b.sndROM = arcade.Region(g, files, "sound", 0x1000)
	b.gfx = arcade.Region(g, files, "blitter", 0x4000)
	b.screen = make([]uint32, scrW*scrH)
	b.ay = ay8910.New(false)
	b.out = arcade.NewResampler(FPS*lines, float64(opts.SampleRate))
	b.main = z80.New(mainBus{b})
	b.snd = z80.New(sndBus{b})
	b.Reset()
	return b, nil
}

func (b *board) Reset() {
	b.main.Reset()
	b.snd.Reset()
	b.main.IRQ, b.snd.IRQ = false, false
	// the protection chip's power-on NMI
	b.main.NMI()
	b.ay.Reset()
	b.vram, b.ctrl, b.ram, b.sndRAM = [0x4000]uint32{}, [16]uint8{}, [0x400]uint8{}, [0x400]uint8{}
	b.latch, b.mcuClock, b.bank = 0, 0, 0
	b.frame, b.mainC, b.sndC = 0, 0, 0
}

func bit(on bool, v uint8) uint8 {
	if on {
		return v
	}
	return 0
}

func (b *board) read(a uint16) uint8 {
	switch {
	case a < 0x6000:
		return b.rom[a]
	case a >= 0xc000 && a < 0xe000:
		return b.gfx[b.bank*0x2000+int(a-0xc000)]
	case a >= 0xe000 && a < 0xe400:
		return b.ram[a-0xe000]
	case a >= 0xe400 && a < 0xe800:
		d := b.dips
		return uint8(d["lives"]&1 | (d["difficulty"]&1)<<1 | (d["bonus"]&3)<<2 | (d["coins"]&15)<<4)
	case a >= 0xec00 && a < 0xed00:
		p, q, d := b.p1, b.p2, b.dips
		return bit(p["Service"], 1) | bit(p["Start (1P)"] || q["Start (1P)"], 2) | bit(p["Start (2P)"] || q["Start (2P)"], 4) |
			bit(p["Coin"], 8) | bit(p["Coin 2"], 0x10) | uint8(d["music"]&1)<<5 | uint8(d["cabinet"]&1)<<6
	case a >= 0xed00 && a < 0xee00:
		return pad(b.p1) | uint8(b.dips["test"]&1)<<7
	case a >= 0xee00 && a < 0xef00:
		return pad(b.p2)
	case a >= 0xef00 && a < 0xf000:
		b.mcuClock++
		return b.mcuClock & 15
	}
	return 0xff
}

func pad(p arcade.Pad) uint8 {
	return bit(p["Right"], 1) | bit(p["Left"], 2) | bit(p["Up"], 4) | bit(p["Down"], 8) | bit(p["Punch"], 0x10)
}

func (b *board) write(a uint16, v uint8) {
	switch {
	case a >= 0x8000 && a < 0xc000:
		b.vramWrite(int(a-0x8000), v, b.ctrl[8])
	case a >= 0xe000 && a < 0xe400:
		b.ram[a-0xe000] = v
	case a >= 0xe800 && a < 0xec00:
		r := int(a & 0x0f)
		if r > 10 {
			return
		}
		b.ctrl[r] = v
		switch r {
		case 5:
			b.blit()
		case 8:
			b.bank = 1
			if v&5 != 0 {
				b.bank = 0
			}
		}
	case a >= 0xec00 && a < 0xed00:
		b.latch = v
	}
}

// vramWrite: v holds four 2-bit pixels (bit n and bit n+4 for pixel n); mask
// picks which of the two bit pairs of planes A (bits 0-3) and B (4-7) take
// them.
func (b *board) vramWrite(o int, v, mask uint8) {
	var exp uint32
	for n := 0; n < 4; n++ {
		if v>>n&1 != 0 {
			exp |= 0x55 << (8 * n)
		}
		if v>>(n+4)&1 != 0 {
			exp |= 0xaa << (8 * n)
		}
	}
	var lm uint32
	if mask&8 != 0 {
		lm |= 0x30303030
	}
	if mask&4 != 0 {
		lm |= 0xc0c0c0c0
	}
	if mask&2 != 0 {
		lm |= 0x03030303
	}
	if mask&1 != 0 {
		lm |= 0x0c0c0c0c
	}
	b.vram[o] = b.vram[o]&^lm | exp&lm
}

func (b *board) blit() {
	c := &b.ctrl
	src := int(c[0]) | int(c[1])<<8
	dst := uint16(int(c[2]) | int(c[3])<<8)
	h, w := int(c[5]), int(c[4])
	mask := c[8]
	if mask&0x0c != 0 {
		mask |= 0x0c
	}
	if mask&0x03 != 0 {
		mask |= 0x03
	}
	half := len(b.gfx) / 2
	for y := 0; y <= h; y, dst = y+1, dst+256 {
		for x := 0; x <= w; x++ {
			d := int(dst+uint16(x)) & 0x3fff
			s := src & (half - 1)
			src++
			b.vramWrite(d, b.gfx[s], mask&0x05)
			b.vramWrite(d, b.gfx[half+s], mask&0x0a)
		}
	}
}

func (b *board) sndRead(a uint16) uint8 {
	switch {
	case a < 0x1000:
		return b.sndROM[a]
	case a >= 0x4000 && a < 0x5000:
		return b.sndRAM[a&0x3ff]
	case a >= 0x6000 && a < 0x7000:
		return b.latch
	}
	return 0xff
}

func (b *board) sndWrite(a uint16, v uint8) {
	switch {
	case a >= 0x4000 && a < 0x5000:
		b.sndRAM[a&0x3ff] = v
	case a >= 0x7000 && a < 0x8000:
		b.ay.WriteData(v)
	case a >= 0x8000 && a < 0x9000:
		b.ay.WriteAddr(v)
	}
}

func (b *board) SetInput(p []arcade.Pad) {
	b.p1 = p[0]
	b.p2 = arcade.Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

// renderLine draws screen line y (8..247): two output pixels per bitmap
// pixel; the second of each pair can be dimmed by the colour mask latch.
func (b *board) renderLine(y int) {
	c := &b.ctrl
	scrollY, scrollX := c[6], c[7]
	maskA, maskB := (c[10]&0x28)>>3, c[10]&0x07
	var xorA, xorB uint8
	if c[9]&0x20 != 0 {
		xorA = 0xff
	}
	if c[9]&0x10 != 0 {
		xorB = 0xff
	}
	enA, enB := c[9]&8 != 0, c[9]&4 != 0
	priA, priB := c[9]&2 == 0, c[9]&1 == 0
	row := b.screen[(y-firstLine)*scrW:]
	for x := 0; x < scrW; x += 2 {
		exa := scrollX + (uint8(x/2) ^ xorA)
		eya := scrollY + (uint8(y) ^ xorA)
		exb := uint8(x/2) ^ xorB
		eyb := uint8(y) ^ xorB
		pa := uint8(b.vram[int(eya)+256*int(exa/4)]>>(8*(exa%4))) & 15
		pb := uint8(b.vram[int(eyb)+256*int(exb/4)]>>(8*(exb%4)+4)) & 15
		var f uint8
		if enA && (priA || pb == 0) {
			f |= pa
		}
		if enB && (priB || pa == 0) {
			f |= pb
		}
		row[x] = pen(f)
		f = 0
		if enA && (priA || pb == 0) {
			q := pa
			if q&8 == 0 {
				q &= maskA
			}
			f |= q
		}
		if enB && (priB || pa == 0) {
			q := pb
			if q&8 == 0 {
				q &= maskB
			}
			f |= q
		}
		row[x+1] = pen(f)
	}
}

func pen(v uint8) uint32 {
	c := func(n uint) int { return int(v>>n&1) * 255 }
	return arcade.RGB(c(2), c(1), c(0))
}

func (b *board) Screen() []uint32 { return b.screen }

func (b *board) RunFrame() {
	for line := 0; line < lines; line++ {
		if line >= firstLine && line < lastLine {
			b.renderLine(line)
		}
		if line == lastLine {
			b.main.IRQ = true
			b.snd.IRQ = true
		}
		b.mainC += lineCyc
		for b.mainC > 0 {
			b.mainC -= b.main.Step()
		}
		b.sndC += sndCyc
		for b.sndC > 0 {
			b.sndC -= b.snd.Step()
		}
		b.ay.Run(sndCyc)
		b.out.Add(b.ay.Take() / 3 * 0.6)
	}
	b.frame++
}

func (b *board) TakeAudio() []float32 { return b.out.Take() }

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.AppendState(w.B)
	w.B = b.snd.AppendState(w.B)
	w.U32s(b.vram[:])
	w.Bytes(b.ctrl[:])
	w.Bytes(b.ram[:])
	w.Bytes(b.sndRAM[:])
	w.Bytes([]uint8{b.latch, b.mcuClock})
	w.Ints([]int{b.bank, b.frame, b.mainC, b.sndC})
	w.B = b.ay.AppendState(w.B)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *board) LoadState(s []byte) error {
	rest, err := b.main.LoadState(s)
	if err != nil {
		return err
	}
	if rest, err = b.snd.LoadState(rest); err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.U32s(b.vram[:])
	r.Bytes(b.ctrl[:])
	r.Bytes(b.ram[:])
	r.Bytes(b.sndRAM[:])
	v := make([]uint8, 2)
	r.Bytes(v)
	b.latch, b.mcuClock = v[0], v[1]
	m := make([]int, 4)
	r.Ints(m)
	b.bank, b.frame, b.mainC, b.sndC = m[0], m[1], m[2], m[3]
	if r.B, err = b.ay.LoadState(r.B); err != nil {
		return err
	}
	b.out.LoadState(r)
	b.out.Clear()
	for y := firstLine; y < lastLine; y++ {
		b.renderLine(y)
	}
	return r.Done()
}

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

var offOn = []arcade.DipOption{opt("Off", 0), opt("On", 1)}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "kangaroo", Title: "Sun Electronics Kangaroo", Width: scrW, Height: scrH, Rotate: 90, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Punch", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips: []arcade.Dip{
			{ID: "coins", Label: "Coins", Default: 0, Options: []arcade.DipOption{
				opt("1 coin, 1 game", 0), opt("2 coins, 1 game", 1), opt("1 coin, 2 games", 8), opt("Free play", 15)}},
			{ID: "lives", Label: "Lives", Default: 0, Options: []arcade.DipOption{opt("3", 0), opt("5", 1)}},
			{ID: "difficulty", Label: "Difficulty", Default: 0, Options: []arcade.DipOption{opt("Easy", 0), opt("Hard", 1)}},
			{ID: "bonus", Label: "Extra life at", Default: 0, Options: []arcade.DipOption{
				opt("No extra life", 0), opt("10,000 points", 1), opt("10,000 and 30,000 points", 2), opt("20,000 and 40,000 points", 3)}},
			{ID: "music", Label: "Music", Default: 0, Options: []arcade.DipOption{opt("On", 0), opt("Off", 1)}},
			{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []arcade.DipOption{opt("Upright", 0), opt("Cocktail table", 1)}},
			{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: offOn},
		},
		Games: []arcade.Game{
			{ID: "kangaroo", Title: "Kangaroo", Year: 1982, Maker: "Sun Electronics", Files: []arcade.File{
				{Name: "tvg_75.0", Alt: []string{"tvg_75.ic7"}, Size: 0x1000, Role: "cpu", Offset: 0x0000},
				{Name: "tvg_76.1", Alt: []string{"tvg_76.ic8"}, Size: 0x1000, Role: "cpu", Offset: 0x1000},
				{Name: "tvg_77.2", Alt: []string{"tvg_77.ic9"}, Size: 0x1000, Role: "cpu", Offset: 0x2000},
				{Name: "tvg_78.3", Alt: []string{"tvg_78.ic10"}, Size: 0x1000, Role: "cpu", Offset: 0x3000},
				{Name: "tvg_79.4", Alt: []string{"tvg_79.ic16"}, Size: 0x1000, Role: "cpu", Offset: 0x4000},
				{Name: "tvg_80.5", Alt: []string{"tvg_80.ic17"}, Size: 0x1000, Role: "cpu", Offset: 0x5000},
				{Name: "tvg_81.8", Alt: []string{"tvg_81.ic24"}, Size: 0x1000, Role: "sound", Offset: 0},
				{Name: "tvg_83.v0", Alt: []string{"tvg_83.ic76"}, Size: 0x1000, Role: "blitter", Offset: 0x0000},
				{Name: "tvg_85.v2", Alt: []string{"tvg_85.ic77"}, Size: 0x1000, Role: "blitter", Offset: 0x1000},
				{Name: "tvg_84.v1", Alt: []string{"tvg_84.ic52"}, Size: 0x1000, Role: "blitter", Offset: 0x2000},
				{Name: "tvg_86.v3", Alt: []string{"tvg_86.ic53"}, Size: 0x1000, Role: "blitter", Offset: 0x3000},
				// the protection chip's data and program (not needed)
				{Name: "tvg_82.12", Alt: []string{"tvg_82.ic28"}, Size: 0x800, Role: "prot", Offset: 0, Optional: true},
				{Name: "mb8841_477m.ic29", Alt: []string{"mb8841.ic29"}, Size: 0x800, Role: "mcu", Offset: 0, Optional: true},
			}},
		},
		New: newBoard,
	})
}
