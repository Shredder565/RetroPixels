package model3

import (
	"errors"
	"math"
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
		Buttons:     []string{"Up", "Down", "Left", "Right", "Fire", "Button 1", "Coin", "Start (1P)", "Service"},
		Players:     1,
		Dips:        stickDips,
		Games:       games,
		New:         newArcadeBoard,
		AnalogStick: true,
		MouseAim:    true,
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

	// The mouse aims too (as in Supermodel's default controls): its place
	// over the picture is where the stick points, left button fires, right
	// button is the thumb button. Whichever of mouse and controller moved
	// last does the aiming.
	mouse      [2]float32 // -1..1 over the picture (down positive), NaN = not over it
	mouseLast  [2]float32
	mouseBtns  uint32
	mouseAims  bool
	mouseKnown bool

	dips   map[string]int // the machine settings (stickDips), changed in place
	smooth [2]float32     // the controller's stick after the flight stick smoothing
}

// The controller's stick settings, in the machine settings (DIP switches) of
// both apps, as Supermodel's tab had them in 2.21.0.
var stickDips = []arcade.Dip{
	{ID: "flightstick", Label: "Flight stick feel (Xbox stick)", Default: 1, Options: []arcade.DipOption{
		{Label: "On", Value: 1}, {Label: "Off: plain thumbstick", Value: 0}}},
	{ID: "smoothness", Label: "Flight stick smoothness", Default: 60, Options: []arcade.DipOption{
		{Label: "Off", Value: 0}, {Label: "Light", Value: 35}, {Label: "Medium", Value: 60}, {Label: "Heavy", Value: 100}}},
	{ID: "inverty", Label: "Stick up / down", Default: 0, Options: []arcade.DipOption{
		{Label: "Up climbs", Value: 0}, {Label: "Up dives (flight-style)", Value: 1}}},
}

// The flight stick feel: a thumbstick has a round gate, a short throw and
// no resistance, so on its own it aims jumpily and can't reach a corner.
// The stick's two axes are read together: the dead zone is round, the round
// gate is stretched to a square one (full left and full up at once reach the
// corner), a cubic response curve gives fine aim near the centre with full
// deflection at the edge, and the aim is smoothed over time like a yoke's
// weight and spring (letting go glides back to the centre).
const (
	flightCurve = 0.60
	flightDead  = 0.05
)

// flightShape: the stick (x right, y up, -1..1) shaped as above, and
// whether it's out of the dead zone.
func flightShape(u, w float32) (float32, float32, bool) {
	r := float32(math.Hypot(float64(u), float64(w)))
	if r <= flightDead {
		return 0, 0, false
	}
	r2 := min(1, (r-flightDead)/(1-flightDead))
	m := max(abs32(u), abs32(w))
	curve := func(v float32) float32 {
		v = max(-1, min(1, v*r2/m))
		a := abs32(v)
		a = (1-flightCurve)*a + flightCurve*a*a*a
		if v < 0 {
			return -a
		}
		return a
	}
	return curve(u), curve(w), true
}

func abs32(v float32) float32 {
	if v < 0 {
		return -v
	}
	return v
}

// dip: a machine setting's value (its default when unset).
func (b *arcadeBoard) dip(id string) int {
	if v, ok := b.dips[id]; ok {
		return v
	}
	for _, d := range stickDips {
		if d.ID == id {
			return d.Default
		}
	}
	return 0
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
	return &arcadeBoard{m: m, out: arcade.NewResampler(44100, float64(rate)), dips: opts.Dips}, nil
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

// MouseAim: the mouse aims and fires, alongside the controller.
func (b *arcadeBoard) MouseAim() bool { return true }

// SetMouse takes the mouse: x, y -1..1 over the picture (right and down
// positive; NaN when it's not over the picture) and its buttons (bit 0
// left, bit 1 right).
func (b *arcadeBoard) SetMouse(x, y float32, buttons uint32) {
	b.mouse = [2]float32{x, y}
	b.mouseBtns = buttons
}

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
	padMoved := tx != 0 || ty != 0

	// the controller's stick, with the flight stick feel (or plain)
	var sx, sy float32
	var stick bool
	if b.dip("flightstick") != 0 {
		sx, sy, stick = flightShape(b.analog[0], -b.analog[1])
	} else {
		const dead = 0.12
		if ax := b.analog[0]; ax > dead || ax < -dead {
			sx, stick = ax, true
		}
		if ay := -b.analog[1]; ay > dead || ay < -dead {
			sy, stick = ay, true
		}
	}
	if ms := b.dip("smoothness"); ms > 0 && b.dip("flightstick") != 0 {
		k := float32(1 - math.Exp(-1/57.52416/(float64(ms)/1000)))
		b.smooth[0] += (sx - b.smooth[0]) * k
		b.smooth[1] += (sy - b.smooth[1]) * k
	} else {
		b.smooth = [2]float32{sx, sy}
	}
	gliding := abs32(b.smooth[0]) > 0.002 || abs32(b.smooth[1]) > 0.002
	if stick {
		padMoved = true
	}
	if tx == 0 && ty == 0 && (stick || gliding) {
		x, y = b.smooth[0], b.smooth[1]
	}
	if b.dip("inverty") != 0 {
		y = -y
	}
	if mx, my := b.mouse[0], b.mouse[1]; !math.IsNaN(float64(mx)) && !math.IsNaN(float64(my)) {
		if !b.mouseKnown || mx != b.mouseLast[0] || my != b.mouseLast[1] || b.mouseBtns != 0 {
			b.mouseAims = true
		}
		b.mouseLast, b.mouseKnown = b.mouse, true
	}
	if padMoved {
		b.mouseAims = false
	}
	if b.mouseAims && b.mouseKnown {
		x, y = b.mouseLast[0], -b.mouseLast[1]
	}
	in := &b.m.Inputs
	in.StickX, in.StickY = max(-1, min(1, x)), max(-1, min(1, y))
	in.Trigger = p["Fire"] || b.mouseBtns&1 != 0
	in.Event = p["Button 1"] || b.mouseBtns&2 != 0
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
