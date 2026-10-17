// Package berzerk is Stern's Berzerk board (1980) for RetroPixels' arcade
// framework.
//
// A Z80 at 2.5 MHz runs 14 KB of program ROM. The picture is a 256x256
// one-bit bitmap (the top 32 lines are never shown, and the game keeps its
// stack there) with a colour RAM giving each 4x4 pixel block one of 16
// colours. Writes to the "magic" copy of video RAM go through a shifter
// (the byte joined to the one written before, shifted right 0-7 bits),
// an optional bit flip and a 74181 ALU that combines them with what's
// already on the screen, and report a collision when they overlap lit
// pixels: that's how the game draws and moves its sprites. Interrupts:
// eight NMIs a frame, every 32 lines, and an IM 2 interrupt at lines 128
// and 256. 1 KB of battery-backed RAM keeps the high scores. Sound is a
// 6840 timer (see sound.go) plus an S14001A speech chip.
//
// The hardware details were worked out from the game's own code and by
// watching MAME run it as a black box (memory, ports, interrupt times and
// the magic circuit's results, probed from Lua).
package berzerk

import (
	"errors"

	"retropixels/cores/arcade"
	"retropixels/cores/z80"
)

const (
	cpuHz     = 2500000 // 10 MHz / 4
	lineCyc   = 160     // 320 pixels at 5 MHz
	lines     = 262
	frameCyc  = lineCyc * lines
	firstLine = 32 // first line shown
	scrW      = 256
	scrH      = 224
)

// FPS is the frame rate: 59.637 Hz.
const FPS = float64(cpuHz) / frameCyc

type board struct {
	rom    [0x3800]uint8
	nvram  [0x400]uint8  // $0800-$0BFF (mirrored at $0C00)
	vram   [0x2000]uint8 // $4000-$5FFF
	colour [0x800]uint8  // $8000-$87FF (mirrored to $BFFF)

	cpu    *z80.CPU
	snd    *sound
	speech speech
	dips   map[string]int
	p1, p2 arcade.Pad
	screen []uint32

	magicCtrl  uint8 // port $4B: function (bits 4-7), flip (bit 3), shift (bits 0-2)
	magicLatch uint8 // the byte written before
	collision  bool
	nmiOn      bool
	irqOn      bool
	line       int // the line being drawn (0-261)
	cyc        int // CPU cycles into the line
}

// palette: bit 0 red, 1 green, 2 blue, 3 bright (levels as MAME shows them).
var palette = func() [16]uint32 {
	var p [16]uint32
	for i := range p {
		lv := 108
		if i&8 != 0 {
			lv = 255
		}
		c := [3]int{}
		for k := 0; k < 3; k++ {
			if i&(1<<k) != 0 {
				c[k] = lv
			}
		}
		p[i] = arcade.RGB(c[0], c[1], c[2])
	}
	return p
}()

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips, screen: make([]uint32, scrW*scrH)}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range dips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	copy(b.rom[:], arcade.Region(g, files, "cpu", 0x3800))
	if len(opts.SRAM) > 0 {
		copy(b.nvram[:], opts.SRAM)
	}
	b.p1, b.p2 = arcade.Pad{}, arcade.Pad{}
	b.snd = newSound(opts.SampleRate)
	b.cpu = z80.New(bus{b})
	b.Reset()
	return b, nil
}

func (b *board) Reset() {
	b.cpu.Reset()
	b.cpu.IRQ = false
	b.snd.reset()
	b.speech.reset()
	b.magicCtrl, b.magicLatch = 0, 0
	b.collision, b.nmiOn, b.irqOn = false, false, false
	b.line, b.cyc = 0, 0
}

// ---------------------------------------------------------------- the bus

type bus struct{ b *board }

func (x bus) Read(a uint16) uint8 {
	b := x.b
	switch {
	case a < 0x0800:
		return b.rom[a]
	case a < 0x1000:
		return b.nvram[a&0x3ff]
	case a < 0x3800:
		return b.rom[a]
	case a < 0x4000:
		return 0xff // no ROM fitted
	case a < 0x6000:
		return b.vram[a-0x4000]
	case a < 0x8000:
		return b.vram[a-0x6000] // the magic copy reads the plain one
	case a < 0xc000:
		return b.colour[a&0x7ff]
	}
	return 0
}

func (x bus) Write(a uint16, v uint8) {
	b := x.b
	switch {
	case a >= 0x0800 && a < 0x1000:
		b.nvram[a&0x3ff] = v
	case a >= 0x4000 && a < 0x6000:
		b.vram[a-0x4000] = v
	case a >= 0x6000 && a < 0x8000:
		b.magicWrite(int(a-0x6000), v)
	case a >= 0x8000 && a < 0xc000:
		b.colour[a&0x7ff] = v
	}
}

// Vector is the byte on the data bus when the interrupt is taken (IM 2).
func (x bus) Vector() uint8 { return 0xfc }

// Ack: the interrupt is held until the processor takes it.
func (x bus) Ack() { x.b.cpu.IRQ = false }

func (x bus) In(port uint16) uint8 {
	b := x.b
	switch port & 0xff {
	case 0x44:
		return b.speech.status()
	case 0x48:
		return padBits(b.p1, 0xe0)
	case 0x49:
		v := uint8(0xff) // bit 5 is coin slot 3, not used here
		for _, k := range []struct {
			p   arcade.Pad
			n   string
			bit uint8
		}{{b.p1, "Start (1P)", 1}, {b.p1, "Start (2P)", 2}, {b.p2, "Coin", 0x40}, {b.p1, "Coin", 0x80}} {
			if k.p[k.n] {
				v &^= k.bit
			}
		}
		return v
	case 0x4a:
		v := padBits(b.p2, 0x60)
		if b.dips["cabinet"] == 0 {
			v |= 0x80
		}
		return v
	case 0x4c:
		b.nmiOn = true
	case 0x4d:
		b.nmiOn = false
	case 0x4e:
		v := uint8(0)
		if b.line < firstLine || b.line >= firstLine+scrH {
			v |= 1
		}
		if b.collision {
			v |= 0x80
		}
		return v
	case 0x60: // switch bank F3
		return 0x3c | uint8(b.dips["inputTest"]) | uint8(b.dips["crosshair"])<<1 | uint8(b.dips["language"])<<6
	case 0x61: // F2
		return 0x3c | uint8(b.dips["colourTest"])*3 | uint8(b.dips["bonus"])<<6
	case 0x62: // F6
		return 0xf0 | uint8(b.dips["coin3"])
	case 0x63: // F5
		return 0xf0 | uint8(b.dips["coin2"])
	case 0x64: // F4
		return 0xf0 | uint8(b.dips["coin1"])
	case 0x65:
		return 0x7e
	}
	return 0
}

// padBits: a player's stick and button (active low) over the fixed bits.
func padBits(p arcade.Pad, fixed uint8) uint8 {
	v := uint8(0x1f)
	for i, n := range []string{"Left", "Right", "Up", "Down", "Fire"} {
		if p[n] {
			v &^= 1 << i
		}
	}
	return v | fixed | 0x80
}

func (x bus) Out(port uint16, v uint8) {
	b := x.b
	switch p := int(port & 0xff); {
	case p == 0x44:
		b.speech.write(v)
	case p >= 0x40 && p <= 0x47:
		b.snd.write(p-0x40, v)
	case p == 0x4b:
		b.magicCtrl = v
		b.collision = false
	case p == 0x4c:
		b.nmiOn = true
	case p == 0x4d:
		b.nmiOn = false
	case p == 0x4f:
		b.irqOn = v&1 != 0
	}
}

// magicWrite is a write through the shifter, flipper and ALU.
func (b *board) magicWrite(off int, v uint8) {
	c := b.magicCtrl
	d := uint8((uint16(b.magicLatch)<<8 | uint16(v)) >> (c & 7))
	b.magicLatch = v
	if c&8 != 0 {
		d = flip8[d]
	}
	old := b.vram[off]
	if d&old != 0 {
		b.collision = true
	}
	b.vram[off] = alu(c>>4, d, old)
}

// alu is the 74181 in logic mode: the 16 functions of the shifted data A
// and the screen byte B, numbered as the board's control register has them.
func alu(f, a, b uint8) uint8 {
	switch f {
	case 0:
		return a
	case 1:
		return a | b
	case 2:
		return a | ^b
	case 3:
		return 0xff
	case 4:
		return a & b
	case 5:
		return b
	case 6:
		return ^(a ^ b)
	case 7:
		return ^a | b
	case 8:
		return a &^ b
	case 9:
		return a ^ b
	case 10:
		return ^b
	case 11:
		return ^(a & b)
	case 12:
		return 0
	case 13:
		return ^a & b
	case 14:
		return ^(a | b)
	}
	return ^a
}

var flip8 = func() (t [256]uint8) {
	for i := range t {
		v := uint8(i)
		r := uint8(0)
		for k := 0; k < 8; k++ {
			r = r<<1 | v&1
			v >>= 1
		}
		t[i] = r
	}
	return
}()

// ---------------------------------------------------------------- frames

func (b *board) SetInput(p []arcade.Pad) {
	b.p1, b.p2 = arcade.Pad{}, arcade.Pad{}
	if len(p) > 0 && p[0] != nil {
		b.p1 = p[0]
	}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

// RunFrame runs up to the end of the picture (line 255): from line 256 of
// the last frame through vertical blank (lines 256-261 and 0-31) and the
// picture's lines 32-255, each drawn when the beam has passed it. (After a
// reset the first frame starts at line 0.)
func (b *board) RunFrame() {
	for {
		l := b.line
		if l&31 == 16 && b.nmiOn {
			b.cpu.NMI()
		}
		if (l == 128 || l == 256) && b.irqOn {
			b.cpu.IRQ = true
		}
		for b.cyc < lineCyc {
			n := b.cpu.Step()
			b.cyc += n
			b.snd.run(n)
		}
		b.cyc -= lineCyc
		if l >= firstLine && l < firstLine+scrH {
			b.drawLine(l)
		}
		b.line = (l + 1) % lines
		if l == firstLine+scrH-1 {
			return
		}
	}
}

func (b *board) drawLine(l int) {
	row := b.screen[(l-firstLine)*scrW : (l-firstLine+1)*scrW]
	px := b.vram[l*32 : l*32+32]
	col := b.colour[(l>>2)*32 : (l>>2)*32+32]
	for bx := 0; bx < 32; bx++ {
		v := px[bx]
		c := col[bx]
		left, right := palette[c>>4], palette[c&15]
		o := bx * 8
		for k := 0; k < 8; k++ {
			pc := right
			if k < 4 {
				pc = left
			}
			if v&(0x80>>k) != 0 {
				row[o+k] = pc
			} else {
				row[o+k] = 0
			}
		}
	}
}

func (b *board) Screen() []uint32 { return b.screen }

func (b *board) TakeAudio() []float32 { return b.snd.rs.Take() }

// SRAM is the battery-backed RAM (high scores and bookkeeping).
func (b *board) SRAM() []byte { return append([]byte(nil), b.nvram[:]...) }

// ---------------------------------------------------------------- state

const stateVer = 1

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.U8(stateVer)
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.nvram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.colour[:])
	w.U8(b.magicCtrl)
	w.U8(b.magicLatch)
	w.Bool(b.collision)
	w.Bool(b.nmiOn)
	w.Bool(b.irqOn)
	w.Int(b.line)
	w.Int(b.cyc)
	b.snd.save(w)
	b.speech.save(w)
	return w.B, nil
}

func (b *board) LoadState(s []byte) error {
	if len(s) < 1 || s[0] != stateVer {
		return arcade.ErrBadState
	}
	// restore into a copy first, so a damaged state changes nothing
	old, _ := b.SaveState()
	if err := b.loadState(s[1:]); err != nil {
		b.loadState(old[1:])
		return err
	}
	for l := firstLine; l < firstLine+scrH; l++ {
		b.drawLine(l)
	}
	return nil
}

func (b *board) loadState(s []byte) error {
	rest, err := b.cpu.LoadState(s)
	if err != nil {
		return errors.Join(arcade.ErrBadState, err)
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.nvram[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.colour[:])
	b.magicCtrl = r.U8()
	b.magicLatch = r.U8()
	b.collision = r.Bool()
	b.nmiOn = r.Bool()
	b.irqOn = r.Bool()
	b.line = r.Int() % lines
	b.cyc = r.Int()
	b.snd.load(r)
	b.speech.load(r)
	return r.Done()
}
