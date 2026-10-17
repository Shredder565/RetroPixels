// Package vsnes is Nintendo's Vs. UniSystem for RetroPixels' arcade
// framework: an NES (RetroPixels' own NES emulator, cores/nes) in an arcade
// cabinet.
//
// The differences from the console that matter here: the PPU is an RGB
// one with its own colour order (see palette.go); the controller ports
// carry the coin slots, the service button and the eight DIP switches as
// well as the two controllers' serial data (a write to $4020 drives the
// coin counter, which needs nothing here); the board has 4 KB of nametable
// RAM (four-screen mirroring); the RGB PPU never skips a dot on odd
// frames; and the cabinet's 1-player and 2-player buttons take the
// controllers' Select bits (Start isn't used).
//
// Vs. Castlevania is the console's Castlevania on a UxROM-style board: 128
// KB of program ROM in 16 KB banks with the last one fixed, and 8 KB of
// character RAM.
package vsnes

import (
	"errors"

	"retropixels/cores/arcade"
	"retropixels/cores/nes"
)

const (
	scrW, scrH = 256, 240
	// FPS is the NTSC NES's frame rate.
	FPS = 60.0988
)

type board struct {
	n      *nes.NES
	dips   map[string]int
	p1, p2 arcade.Pad
}

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips, p1: arcade.Pad{}, p2: arcade.Pad{}}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range dips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	prg := arcade.Region(g, files, "prg", 0x20000)
	colours := palette2C04_0002()
	for i := range g.Files {
		f := &g.Files[i]
		if f.Role == "palette" {
			if d := arcade.FindFile(f, files); len(d) == f.Size && promOK(d) {
				colours = palettePROM(d)
			}
		}
	}
	n, err := nes.NewArcade(inesImage(prg), opts.SampleRate, nes.ArcadeHooks{
		Palette: buildPalette(colours),
		RGBPPU:  true,
		Port:    b.port,
	})
	if err != nil {
		return nil, err
	}
	b.n = n
	return b, nil
}

// promOK: a colour PROM dump in the layout palettePROM reads (every value
// 0-7); another layout is ignored and the built-in colours are used.
func promOK(d []byte) bool {
	for _, v := range d {
		if v > 7 {
			return false
		}
	}
	return true
}

// inesImage wraps the program ROM as an iNES file: mapper 2 (UxROM),
// character RAM, four-screen nametables.
func inesImage(prg []byte) []byte {
	h := []byte{'N', 'E', 'S', 0x1a, byte(len(prg) / 0x4000), 0, 0x20 | 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0}
	return append(h, prg...)
}

// port is a read of $4016 or $4017: the controller's serial bit, then
// $4016: service (bit 2), DIP switches 1-2 (bits 3-4), coins (bits 5-6);
// $4017: DIP switches 3-8 (bits 2-7).
func (b *board) port(addr, serial int) int {
	sw := b.switches()
	if addr == 0x4016 {
		v := serial | (sw&3)<<3
		if b.p1["Service"] {
			v |= 4
		}
		if b.p1["Coin"] {
			v |= 0x20
		}
		if b.p2["Coin"] {
			v |= 0x40
		}
		return v
	}
	return serial | sw&0xfc
}

// switches: the DIP switches as a byte (switch 1 = bit 0).
func (b *board) switches() int {
	d := b.dips
	return d["coinage"]&7 | (d["lives"]&1)<<3 | (d["bonus"]&3)<<4 | (d["difficulty"]&1)<<6 | (d["unused"]&1)<<7
}

// The cabinet's controls in the NES controller's bit order (A, B, Select,
// Start, Up, Down, Left, Right): A is Jump, B is Fire (the whip). As MAME has it for this game, player 1's
// stick and buttons are on the $4017 controller and player 2's on $4016;
// the 1-player button is the $4016 controller's Select, the 2-player
// button the $4017 one's.
func padBits(p arcade.Pad) uint32 {
	var v uint32
	for i, n := range []string{"Jump", "Fire", "", "", "Up", "Down", "Left", "Right"} {
		if n != "" && p[n] {
			v |= 1 << i
		}
	}
	return v
}

func (b *board) SetInput(p []arcade.Pad) {
	b.p1, b.p2 = arcade.Pad{}, arcade.Pad{}
	if len(p) > 0 && p[0] != nil {
		b.p1 = p[0]
	}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
	in4016 := padBits(b.p2)
	in4017 := padBits(b.p1)
	if b.p1["Start (1P)"] {
		in4016 |= 4
	}
	if b.p1["Start (2P)"] {
		in4017 |= 4
	}
	b.n.SetInput(0, in4016)
	b.n.SetInput(1, in4017)
}

func (b *board) RunFrame()        { b.n.RunFrame() }
func (b *board) Screen() []uint32 { return b.n.Screen() }

// TakeAudio: the NES core's sound at the Vs. cabinet's level (measured
// against MAME, the console level is about 2.1 times louder).
func (b *board) TakeAudio() []float32 {
	a := b.n.TakeAudio()
	for i := range a {
		a[i] *= vsLevel
	}
	return a
}

const vsLevel = 0.48

func (b *board) Reset() { b.n.Reset() }

const stateVer = 1

func (b *board) SaveState() ([]byte, error) {
	s, err := b.n.SaveState()
	if err != nil {
		return nil, err
	}
	w := &arcade.StateW{}
	w.U8(stateVer)
	w.Blob(s)
	return w.B, nil
}

func (b *board) LoadState(d []byte) error {
	r := &arcade.StateR{B: d}
	if r.U8() != stateVer {
		return arcade.ErrBadState
	}
	s := r.Blob()
	if err := r.Done(); err != nil {
		return err
	}
	if err := b.n.LoadState(s); err != nil {
		return errors.Join(arcade.ErrBadState, err)
	}
	return nil
}
