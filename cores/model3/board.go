package model3

import (
	"errors"
	"sort"

	"retropixels/cores/arcade"
)

// The Model 3 games in RetroPixels' Arcade library: the board registers with
// package arcade like the other Go boards, so both apps play it from a ROM
// set's zip with nothing else installed.

const nvramSize = 0x20000 + 64*2

func init() {
	var games []arcade.Game
	for _, g := range Games {
		games = append(games, arcadeGame(g))
	}
	arcade.Register(&arcade.BoardDef{
		ID: "model3", Title: "Sega Model 3",
		Width: screenW, Height: screenH, Aspect: 4.0 / 3, FPS: 57.52416,
		Buttons: []string{"Up", "Down", "Left", "Right", "Fire", "Button 1", "Coin", "Start (1P)", "Service"},
		Players: 1,
		Games:   games,
		New:     newArcadeBoard,
	})
}

// arcadeGame lists a game's chips for the arcade framework (by name and
// size, the way it matches a zip to a game).
func arcadeGame(g *GameDef) arcade.Game {
	var regions []string
	all := g.AllFiles()
	for r := range all {
		regions = append(regions, r)
	}
	sort.Strings(regions)
	var files []arcade.File
	for _, r := range regions {
		for _, f := range all[r] {
			files = append(files, arcade.File{Name: f.Name, Size: f.Size, Role: r, Offset: f.Offset,
				Optional: r == "driveboard_program"})
		}
	}
	return arcade.Game{ID: g.ID, Title: g.Title, Year: g.Year, Maker: g.Maker, Parent: g.Parent, Files: files}
}

type arcadeBoard struct {
	m      *Machine
	out    *arcade.Resampler
	pads   []arcade.Pad
	analog [2]float32 // the controller's stick, -1..1 (up negative)
	ramp   [2]float32 // the stick as the d-pad / keys move it
}

func newArcadeBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	def := Find(g.ID)
	if def == nil {
		return nil, Error("Unknown Model 3 game " + g.ID)
	}
	src := NewFileSource(files)
	if miss := def.Missing(src); len(miss) > 0 {
		return nil, Error("The ROM set for " + def.Title + " is missing files: " + joinNames(miss))
	}
	roms, err := def.LoadROMs(src)
	if err != nil {
		return nil, err
	}
	m, err := New(def, roms)
	if err != nil {
		return nil, err
	}
	if len(opts.SRAM) == nvramSize {
		m.LoadNVRAM(opts.SRAM)
	}
	rate := opts.SampleRate
	if rate == 0 {
		rate = 48000
	}
	return &arcadeBoard{m: m, out: arcade.NewResampler(44100, float64(rate))}, nil
}

func joinNames(n []string) string {
	sort.Strings(n)
	s := ""
	for i, x := range n {
		if i > 0 {
			s += ", "
		}
		s += x
	}
	return s
}

func (b *arcadeBoard) SetInput(pads []arcade.Pad) { b.pads = pads }

// SetAnalog takes the controller's left stick (axis 0 = x, 1 = y, up
// negative).
func (b *arcadeBoard) SetAnalog(player, axis int, v float32) {
	if player == 0 && axis < 2 {
		b.analog[axis] = v
	}
}

// AnalogStick: the board reads the stick itself.
func (b *arcadeBoard) AnalogStick() bool { return true }

// rampTo moves the key-driven stick toward its target, a little each frame,
// so tapping a key nudges the aim and holding it swings it all the way.
func rampTo(cur, target float32) float32 {
	const step = 0.12
	if target == 0 {
		step := float32(0.25)
		if cur > step {
			return cur - step
		}
		if cur < -step {
			return cur + step
		}
		return 0
	}
	if cur < target {
		cur = min(cur+step, target)
	} else if cur > target {
		cur = max(cur-step, target)
	}
	return cur
}

func (b *arcadeBoard) RunFrame() {
	var p arcade.Pad
	if len(b.pads) > 0 {
		p = b.pads[0]
	}
	var tx, ty float32
	if p["Left"] {
		tx--
	}
	if p["Right"] {
		tx++
	}
	if p["Up"] {
		ty++
	}
	if p["Down"] {
		ty--
	}
	b.ramp[0] = rampTo(b.ramp[0], tx)
	b.ramp[1] = rampTo(b.ramp[1], ty)
	x, y := b.ramp[0], b.ramp[1]
	const dead = 0.12
	if ax := b.analog[0]; ax > dead || ax < -dead {
		x = ax
	}
	if ay := -b.analog[1]; ay > dead || ay < -dead {
		y = ay
	}
	in := &b.m.Inputs
	in.StickX, in.StickY = max(-1, min(1, x)), max(-1, min(1, y))
	in.Trigger, in.Event = p["Fire"], p["Button 1"]
	in.Coin1, in.Start1, in.Service = p["Coin"], p["Start (1P)"], p["Service"]
	in.Up, in.Down, in.Left, in.Right = p["Up"], p["Down"], p["Left"], p["Right"]
	in.Buttons[0], in.Buttons[1] = p["Fire"], p["Button 1"]

	b.m.RunFrame()
	a := b.m.Audio()
	for i := 0; i+1 < len(a); i += 2 {
		b.out.Add(float64(a[i]+a[i+1]) * 0.5)
	}
}

func (b *arcadeBoard) Screen() []uint32     { return b.m.Picture[:] }
func (b *arcadeBoard) TakeAudio() []float32 { return b.out.Take() }
func (b *arcadeBoard) Reset()               { b.m.Reset() }

var errNoStates = errors.New("Save states aren't available for Sega Model 3 games yet")

func (b *arcadeBoard) SaveState() ([]byte, error) { return nil, errNoStates }
func (b *arcadeBoard) LoadState([]byte) error     { return errNoStates }

// SRAM is the backup RAM and the EEPROM (settings and high scores).
func (b *arcadeBoard) SRAM() []byte { return b.m.NVRAM() }
