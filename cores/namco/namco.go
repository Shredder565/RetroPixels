// Package namco is Namco's Galaga (1981) and Dig Dug (1982) board for
// RetroPixels' arcade framework.
//
// Three Z80s at 3.072 MHz, each with its own program ROM, share the video
// RAM and three 1 KB work RAMs: the main processor runs the game, the second
// one moves the objects and the third one plays the music on Namco's 3-voice
// wavetable chip. The main processor talks to Namco's custom chips through
// the 06xx bus controller: the 51xx counts coins and credits and reads the
// joysticks and buttons, Dig Dug's 53xx reads the DIP switches and Galaga's
// 54xx makes the explosion noises. Those chips are 4-bit microcontrollers
// whose programs aren't in the user's sets, so this package does what they
// do, as seen from the game's side of the 06xx (see custom.go). The picture
// is a 36x28 character screen and 64 sprites (16x16, or 32 wide/high), with
// Galaga's 05xx starfield behind them, or Dig Dug's playfield ROM layer; the
// monitor is on its side, so the board draws 288x224 and the framework turns
// it upright. Dig Dug keeps its high scores in a 64-byte EAROM.
//
// The hardware details were worked out from the games' own code and by
// watching MAME run them as a black box (memory, interrupt times, the 06xx's
// transfer rate and pictures, probed from Lua), with RetroPixels' own
// stand-in for the 51xx answering in MAME too.
package namco

import (
	"retropixels/cores/arcade"
	"retropixels/cores/z80"
)

const (
	cpuHz    = 3072000
	lineCyc  = 192 // 384 pixels at 6.144 MHz
	lines    = 264
	frameCyc = lineCyc * lines
	scrW     = 288
	scrH     = 224
	// The frame starts at the start of vertical blank (MAME's frame edge):
	// line 0 here is the picture's line 224, and the picture's line 0 is
	// line 40 here.
	vblankLines = lines - scrH
	slice       = 24 // the processors take turns this many cycles at a time
)

// FPS is the frame rate: 60.606 Hz.
const FPS = float64(cpuHz) / frameCyc

type kind int

const (
	galaga kind = iota
	digdug
)

type board struct {
	kind kind
	game *arcade.Game
	dips map[string]int

	rom   [3][]uint8 // each processor's program
	vram  [0x800]uint8
	ram   [3][0x400]uint8
	cpu   [3]*z80.CPU
	cyc   [3]int // cycles run this frame
	irq   [2]bool
	latch [8]uint8 // misc latch at $6820: IRQ enables, NMI enable, sub reset
	vlat  [8]uint8 // video latch at $A000
	wdog  int

	io  io06xx
	n51 n51xx
	n54 n54xx
	snd wsg

	earom   [64]uint8
	eaAddr  int
	eaData  uint8
	eaCtl   uint8
	eaDirty bool

	vid    *video
	p      [2]arcade.Pad
	screen []uint32
	out    *arcade.Resampler
	rate   float64 // output sample rate
	t      int     // cycle of the frame the sound has reached
}

type cpuBus struct {
	b *board
	n int
}

func (x cpuBus) Read(a uint16) uint8     { return x.b.read(x.n, a) }
func (x cpuBus) Write(a uint16, v uint8) { x.b.write(x.n, a, v) }
func (x cpuBus) In(uint16) uint8         { return 0xff }
func (x cpuBus) Out(uint16, uint8)       {}

func newBoard(k kind) func(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	return func(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
		b := &board{kind: k, game: g, dips: opts.Dips, screen: make([]uint32, scrW*scrH)}
		if b.dips == nil {
			b.dips = map[string]int{}
		}
		for _, d := range dipsFor(k) {
			if _, ok := b.dips[d.ID]; !ok {
				b.dips[d.ID] = d.Default
			}
		}
		b.rom[0] = arcade.Region(g, files, "cpu1", 0x4000)
		b.rom[1] = arcade.Region(g, files, "cpu2", 0x4000)
		b.rom[2] = arcade.Region(g, files, "cpu3", 0x4000)
		var err error
		if b.vid, err = newVideo(k, g, files); err != nil {
			return nil, err
		}
		b.snd.setWaves(arcade.Region(g, files, "wave", 0x100), arcade.FindFile(fileFor(g, "wave"), files) != nil)
		if k == digdug && len(opts.SRAM) == len(b.earom) {
			copy(b.earom[:], opts.SRAM)
		}
		rate := opts.SampleRate
		if rate <= 0 {
			rate = 48000
		}
		b.rate = float64(rate)
		b.n54.init(wsgHz)
		b.p = [2]arcade.Pad{{}, {}}
		for i := range b.cpu {
			b.cpu[i] = z80.New(cpuBus{b, i})
		}
		b.Reset()
		return b, nil
	}
}

// fileFor: the game's first file of a role.
func fileFor(g *arcade.Game, role string) *arcade.File {
	for i := range g.Files {
		if g.Files[i].Role == role {
			return &g.Files[i]
		}
	}
	return &arcade.File{}
}

func (b *board) Reset() {
	for i, c := range b.cpu {
		c.Reset()
		c.IRQ = false
		b.cyc[i] = 0
	}
	b.vram = [0x800]uint8{}
	b.ram = [3][0x400]uint8{}
	b.irq = [2]bool{}
	b.latch, b.vlat = [8]uint8{}, [8]uint8{}
	b.wdog = 0
	b.io.reset()
	b.n51.reset()
	b.n54.reset()
	b.snd.reset()
	b.eaAddr, b.eaData, b.eaCtl = 0, 0, 0
	b.vid.reset()
	b.t = 0
	b.out = arcade.NewResampler(wsgHz, b.rate)
}

// ---------------------------------------------------------------- the bus

func (b *board) read(n int, a uint16) uint8 {
	switch {
	case a < 0x4000:
		return b.rom[n][a]
	case a >= 0x8000 && a < 0x8800:
		return b.vram[a&0x7ff]
	case a >= 0x8800 && a < 0xa000:
		return b.ram[(a-0x8800)>>11][a&0x3ff]
	case a >= 0x6800 && a < 0x6808:
		return b.dipBits(int(a & 7))
	case a >= 0x7000 && a < 0x7100:
		if n == 0 {
			return b.io.readData(b)
		}
	case a >= 0x7100 && a < 0x7200:
		if n == 0 {
			return b.io.ctl
		}
	case a >= 0xb800 && a < 0xb840 && b.kind == digdug:
		return b.earom[a&0x3f]
	}
	return 0xff
}

func (b *board) write(n int, a uint16, v uint8) {
	switch {
	case a < 0x4000:
	case a >= 0x8000 && a < 0x8800:
		b.vram[a&0x7ff] = v
	case a >= 0x8800 && a < 0xa000:
		b.ram[(a-0x8800)>>11][a&0x3ff] = v
	case a >= 0x6800 && a < 0x6820:
		b.soundTo(b.cyc[n])
		b.snd.write(int(a&0x1f), v)
	case a >= 0x6820 && a < 0x6828:
		b.setLatch(int(a&7), v&1)
	case a >= 0x6830 && a < 0x6838:
		b.wdog = 0
	case a >= 0x7000 && a < 0x7100:
		if n == 0 {
			b.io.writeData(b, v)
		}
	case a >= 0x7100 && a < 0x7200:
		if n == 0 {
			b.io.writeCtl(v, b.cyc[0])
		}
	case a >= 0xa000 && a < 0xa008:
		b.vlat[a&7] = v & 1
	case a >= 0xb800 && a < 0xb840 && b.kind == digdug:
		b.eaAddr, b.eaData = int(a&0x3f), v
	case a == 0xb840 && b.kind == digdug:
		b.earomCtl(v)
	}
}

// setLatch: the 74LS259 at $6820. Bit 0 lets the main processor's
// vertical-blank interrupt through (0 also clears one that is waiting), bit 1
// the same for the second processor, bit 2 (0 = on) the sound processor's
// NMIs, and bit 3 (0 = held) the reset of the other two processors.
func (b *board) setLatch(i int, v uint8) {
	b.latch[i] = v
	switch i {
	case 0, 1:
		if v == 0 {
			b.irq[i] = false
			b.cpu[i].IRQ = false
		}
	case 3:
		if v == 0 {
			b.cpu[1].Reset()
			b.cpu[2].Reset()
		}
	}
}

// dipBits: Galaga's switches at $6800-$6807: bit 0 is switch n of bank B,
// bit 1 switch n of bank A.
func (b *board) dipBits(n int) uint8 {
	if b.kind != galaga {
		return 0xff
	}
	a, bb := b.dsw()
	return 0xfc | (bb>>n)&1 | ((a>>n)&1)<<1
}

// earomCtl: Dig Dug's ER2055. The game latches an address (and data) by
// writing to $B800+n, then drives the chip's control lines at $B840: $0E
// erases the byte, $0A writes the latched data; reads return the byte.
func (b *board) earomCtl(v uint8) {
	switch v {
	case 0x0e:
		b.earom[b.eaAddr] = 0
		b.eaDirty = true
	case 0x0a:
		b.earom[b.eaAddr] |= b.eaData
		b.eaDirty = true
	}
	b.eaCtl = v
}

// SRAM is Dig Dug's EAROM (high scores); Galaga keeps nothing.
func (b *board) SRAM() []byte {
	if b.kind != digdug {
		return nil
	}
	return append([]byte(nil), b.earom[:]...)
}

// ---------------------------------------------------------------- input

func (b *board) SetInput(pads []arcade.Pad) {
	b.p[0], b.p[1] = arcade.Pad{}, arcade.Pad{}
	if len(pads) > 0 && pads[0] != nil {
		b.p[0] = pads[0]
	}
	if len(pads) > 1 && pads[1] != nil {
		b.p[1] = pads[1]
	}
}

// in0 is the joysticks as the 51xx sees them (active low): player 1 in bits
// 0-3, player 2 in bits 4-7, each up, right, down, left.
func (b *board) in0() uint8 {
	v := uint8(0xff)
	for n, p := range b.p {
		for i, k := range []string{"Up", "Right", "Down", "Left"} {
			if p[k] {
				v &^= 1 << (n*4 + i)
			}
		}
	}
	return v
}

// in1 is the buttons (active low): fire 1, fire 2, start 1, start 2, coin 1,
// coin 2, service coin, service mode switch.
func (b *board) in1() uint8 {
	v := uint8(0xff)
	p, q := b.p[0], b.p[1]
	for _, k := range []struct {
		on  bool
		bit uint8
	}{
		{p["Fire"], 0x01}, {q["Fire"], 0x02},
		{p["Start (1P)"] || q["Start (1P)"], 0x04}, {p["Start (2P)"] || q["Start (2P)"], 0x08},
		{p["Coin"], 0x10}, {p["Coin 2"] || q["Coin"], 0x20}, {p["Service"], 0x40},
		{b.dips["service"] == 1, 0x80},
	} {
		if k.on {
			v &^= k.bit
		}
	}
	return v
}

// dsw: the two switch banks as MAME lays them out.
func (b *board) dsw() (a, bb uint8) {
	a, bb = 0xff, 0xff
	for _, d := range dipsFor(b.kind) {
		if d.mask == 0 {
			continue
		}
		v := uint8(b.dips[d.ID]) & d.mask
		if d.bank == 0 {
			a = a&^d.mask | v
		} else {
			bb = bb&^d.mask | v
		}
	}
	return
}

// ---------------------------------------------------------------- running

func (b *board) Screen() []uint32 { return b.screen }

func (b *board) TakeAudio() []float32 { return b.out.Take() }

func (b *board) RunFrame() {
	// vertical blank: the vertical-blank interrupts
	for i := 0; i < 2; i++ {
		if b.latch[i] != 0 {
			b.irq[i] = true
			b.cpu[i].IRQ = true
		}
	}
	for line := 0; line < lines; line++ {
		if (line == 104 || line == 232) && b.latch[2] == 0 && b.latch[3] != 0 {
			b.cpu[2].NMI() // picture lines 64 and 192
		}
		end := (line + 1) * lineCyc
		step := slice
		if line == 0 {
			// the main processor answers the interrupt first (Galaga
			// reads the sub processor's frame counter there before the
			// sub processor moves it on)
			step = lineCyc
		}
		for t := line*lineCyc + step; t <= end; t += step {
			b.run(t)
		}
	}
	b.soundTo(frameCyc)
	for i := range b.cyc {
		b.cyc[i] -= frameCyc
	}
	b.io.nextNMI -= frameCyc
	b.t = 0
	b.vid.endFrame(b)
	b.vid.render(b)
	// the watchdog restarts the board when the game stops checking in
	b.wdog++
	if b.wdog > 8 {
		b.Reset()
	}
}

// run lets each processor run up to cycle t of the frame.
func (b *board) run(t int) {
	c := b.cpu[0]
	for b.cyc[0] < t {
		if b.io.active() && b.cyc[0] >= b.io.nextNMI {
			c.NMI()
			b.io.nextNMI += b.io.period()
		}
		b.cyc[0] += c.Step()
	}
	for n := 1; n < 3; n++ {
		if b.latch[3] == 0 { // held in reset
			b.cyc[n] = t
			continue
		}
		for b.cyc[n] < t {
			b.cyc[n] += b.cpu[n].Step()
		}
	}
}

// soundTo makes the sound up to cycle t of the frame (one sample every 32
// cycles, 96 kHz).
func (b *board) soundTo(t int) {
	for ; b.t+32 <= t; b.t += 32 {
		v := b.snd.sample() + b.n54.sample()
		b.out.Add(v)
	}
}
