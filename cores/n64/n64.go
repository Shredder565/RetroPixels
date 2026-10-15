// Package n64 is the Nintendo 64 for the native app: RetroPixels' own N64
// emulator (package retropixels/n64, which RetroPixels.exe already runs for
// the web player) wrapped as a core.Core.
//
// It drives the machine exactly the way the web player does: the controls are
// turned into N64 controller bits and stick positions as web/emu/n64.js does,
// and the machine is run, its sound resampled and its picture taken as
// n64host.go does, so a game plays the same in both, frame for frame and
// sample for sample.
package n64

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"

	"retropixels/core"
	machine "retropixels/n64"
)

// Buttons: bit i of SetInput (BUTTONS in n64.js).
var Buttons = []string{"A", "B", "Z", "Start", "Up", "Down", "Left", "Right", "L", "R",
	"C-Up", "C-Down", "C-Left", "C-Right", "Stick Up", "Stick Down", "Stick Left", "Stick Right"}

// the N64 controller bit for each of the first 14 buttons (BITS in n64.js)
var bits = [14]uint16{machine.BtnA, machine.BtnB, machine.BtnZ, machine.BtnStart,
	machine.BtnDUp, machine.BtnDDown, machine.BtnDLeft, machine.BtnDRight, machine.BtnL, machine.BtnR,
	machine.BtnCUp, machine.BtnCDown, machine.BtnCLeft, machine.BtnCRight}

const (
	stickReach = 80   // how far a real N64 stick reaches (STICK in n64.js)
	deadZone   = 0.15 // stick dead zone (DEAD in n64.js)

	bStickUp, bStickDown, bStickLeft, bStickRight = 1 << 14, 1 << 15, 1 << 16, 1 << 17
)

// N64 is one running game.
type N64 struct {
	m    *machine.Machine
	rate int // sound output rate

	buttons   [4]uint32
	sticks    [4][4]float32 // left X, left Y (down = +), right X, right Y
	connected int           // controllers plugged in (the first port always is)

	// sound, as n64host.go makes it
	rs         resampler
	soundAhead float64
	audioOut   []int16
	audio      []float32

	screen []uint32
	w, h   int
	save   []byte // the battery save as the web player keeps it (Saves.Export)
}

// New starts a game. opts.SRAM is a battery save: the web player's (and this
// app's) "RP64" file, or a plain .eep / .sra / .fla / .mpk.
func New(rom []byte, opts core.Options) (core.Core, error) {
	m, err := machine.New(rom)
	if err != nil {
		return nil, fmt.Errorf("that doesn't look like a Nintendo 64 game (%v)", err)
	}
	c := &N64{m: m, rate: opts.SampleRate, connected: 1}
	if c.rate <= 0 {
		c.rate = 48000
	}
	if len(opts.SRAM) > 0 {
		_ = m.Save.Import(opts.SRAM) // (n64host.go starts the game anyway when it isn't one)
	}
	m.SaveChanged()
	c.save = m.Save.Export()
	c.w, c.h = 320, 240
	c.screen = make([]uint32, 320*240)
	return c, nil
}

func (c *N64) Info() core.Info {
	return core.Info{System: "n64", Buttons: Buttons, Players: 4, Stereo: true, Aspect: 4.0 / 3}
}

func (c *N64) FPS() float64 { return c.m.FPS() }

func (c *N64) Width() int       { return c.w }
func (c *N64) Height() int      { return c.h }
func (c *N64) Screen() []uint32 { return c.screen[:c.w*c.h] }

func (c *N64) SetInput(player int, b uint32) {
	if player >= 0 && player < 4 {
		c.buttons[player] = b
	}
}

// SetAnalog: the left stick is the N64's stick, the right stick works the C buttons.
func (c *N64) SetAnalog(player, axis int, v float32) {
	if player >= 0 && player < 4 && axis >= 0 && axis < 4 {
		c.sticks[player][axis] = v
	}
}

// SetConnected tells the core how many controllers are plugged in; the
// N64 sees ports beyond them (other than the first) as empty.
func (c *N64) SetConnected(n int) { c.connected = n }

// AnalogStick: the core reads the left stick itself (it doesn't work the D-pad).
func (c *N64) AnalogStick() bool { return true }

// UsesRightStick: the right stick is the C buttons, so the app leaves it alone.
func (c *N64) UsesRightStick() bool { return true }

// ControlsNote is shown in the Controls window.
func (c *N64) ControlsNote() string { return ControlsNote }

// ControlsNote for the Controls window.
const ControlsNote = "Left stick = the analog stick, right stick = the C buttons. " +
	"Clicking the sticks (L3 / R3) loads and saves states; View + D-pad down starts or stops recording."

// stickValue: a stick axis (-1..1) as the N64 reads it (stickValue in n64.js).
func stickValue(v float64) int {
	a := math.Abs(v)
	if a < deadZone {
		return 0
	}
	s := (a - deadZone) / (1 - deadZone)
	// Math.round: halves go up
	return int(math.Floor(sign(v)*math.Min(1, s)*stickReach + 0.5))
}

func sign(v float64) float64 {
	if v < 0 {
		return -1
	}
	return 1
}

// pads: the four controllers as n64.js sends them and n64host.go reads them.
func (c *N64) pads() [4]machine.PadState {
	var out [4]machine.PadState
	for p := range out {
		b, st := c.buttons[p], c.sticks[p]
		var n uint16
		for i, bit := range bits {
			if b&(1<<i) != 0 {
				n |= bit
			}
		}
		if st[2] < -0.5 {
			n |= machine.BtnCLeft
		}
		if st[2] > 0.5 {
			n |= machine.BtnCRight
		}
		if st[3] < -0.5 {
			n |= machine.BtnCUp
		}
		if st[3] > 0.5 {
			n |= machine.BtnCDown
		}
		x, y := stickValue(float64(st[0])), -stickValue(float64(st[1])) // N64 up is +Y
		// stick keys (arrows by default) push the stick all the way
		if b&bStickLeft != 0 {
			x = -stickReach
		} else if b&bStickRight != 0 {
			x = stickReach
		}
		if b&bStickUp != 0 {
			y = stickReach
		} else if b&bStickDown != 0 {
			y = -stickReach
		}
		out[p] = machine.PadState{Buttons: n, X: int8(x), Y: int8(y), Present: p == 0 || p < c.connected, Pak: true}
	}
	return out
}

// RunFrame runs one frame (one tick of n64host.go).
func (c *N64) RunFrame() {
	m := c.m
	m.Pad = c.pads()
	m.RunFrame()
	c.takeAudio()
	c.takePicture()
}

// takeAudio: takeAudio in n64host.go, then the page's int16 -> float.
func (c *N64) takeAudio() {
	m := c.m
	before := len(c.audioOut)
	if len(m.Audio) >= 2 && m.AudioRate > 0 {
		c.audioOut = c.rs.run(m.Audio, m.AudioRate, c.rate, c.audioOut)
	}
	m.Audio = m.Audio[:0]
	// Games hand over sound in pieces that don't line up with frames: keep
	// count of how far the sound is ahead of or behind the picture, and when
	// the game plays nothing for a while, add silence so the sound card
	// (which sets the game's speed) keeps the right pace.
	per := float64(c.rate) / m.FPS()
	c.soundAhead += float64(len(c.audioOut)-before)/2 - per
	if c.soundAhead > 30*per {
		c.soundAhead = 30 * per
	}
	if c.soundAhead < -3*per {
		n := int(-c.soundAhead - per)
		for i := 0; i < n; i++ {
			c.audioOut = append(c.audioOut, 0, 0)
		}
		c.soundAhead += float64(n)
	}
	for _, v := range c.audioOut {
		c.audio = append(c.audio, float32(v)/32768)
	}
	c.audioOut = c.audioOut[:0]
}

// takePicture: the picture as n64host.go sends it (320x240 black until the
// game shows one), turned from the machine's ABGR to 0x00RRGGBB.
func (c *N64) takePicture() {
	m := c.m
	if m.FrameW <= 0 || m.FrameH <= 0 || len(m.Frame) < m.FrameW*m.FrameH {
		c.w, c.h = 320, 240
		clear(c.screen[:320*240])
		return
	}
	c.w, c.h = m.FrameW, m.FrameH
	n := c.w * c.h
	if len(c.screen) < n {
		c.screen = make([]uint32, n)
	}
	for i, p := range m.Frame[:n] {
		c.screen[i] = (p&0xff)<<16 | p&0xff00 | (p>>16)&0xff
	}
}

func (c *N64) TakeAudio() []float32 {
	a := c.audio
	c.audio = nil
	return a
}

// Reset presses the console's reset (n64host.go's reset; the page drops the sound it had).
func (c *N64) Reset() {
	c.m.Reset()
	c.audio = nil
}

// SRAM is the battery save in the web player's format ("RP64": EEPROM,
// SRAM / FlashRAM and the four Controller Paks), refreshed when the game
// has written to its save memory.
func (c *N64) SRAM() []byte {
	if c.m.SaveChanged() {
		c.save = c.m.Save.Export()
	}
	return c.save
}

// Save states are the web player's ("RPN1" + the machine's state, deflated,
// as /api/n64/state hands them out) with this core's sound position after
// them, so a state loads back to exactly the same sound.

var tailMagic = []byte("RPNw")

const tailLen = 4 * 8

func (c *N64) SaveState() ([]byte, error) {
	raw := c.m.SaveState()
	if len(raw) == 0 {
		return nil, errors.New("save states aren't available for Nintendo 64 yet")
	}
	var buf bytes.Buffer
	buf.WriteString("RPN1")
	zw, _ := flate.NewWriter(&buf, flate.BestSpeed)
	_, _ = zw.Write(raw)
	_ = zw.Close()
	var tail [tailLen]byte
	for i, v := range []float64{c.rs.pos, c.rs.prevL, c.rs.prevR, c.soundAhead} {
		binary.LittleEndian.PutUint64(tail[i*8:], math.Float64bits(v))
	}
	buf.Write(tail[:])
	buf.Write(tailMagic)
	return buf.Bytes(), nil
}

func (c *N64) LoadState(b []byte) error {
	var tail []byte
	if n := len(b); n >= 4+tailLen+4 && bytes.Equal(b[n-4:], tailMagic) {
		tail = b[n-4-tailLen : n-4]
		b = b[:n-4-tailLen]
	}
	raw := b
	if len(b) >= 4 && string(b[:4]) == "RPN1" {
		var err error
		raw, err = io.ReadAll(io.LimitReader(flate.NewReader(bytes.NewReader(b[4:])), 1<<30))
		if err != nil {
			return errors.New("that save state is damaged")
		}
	}
	if err := c.m.LoadState(raw); err != nil {
		return fmt.Errorf("couldn't load that save state: %w", err)
	}
	if tail != nil {
		v := func(i int) float64 { return math.Float64frombits(binary.LittleEndian.Uint64(tail[i*8:])) }
		c.rs = resampler{pos: v(0), prevL: v(1), prevR: v(2)}
		c.soundAhead = v(3)
	}
	c.m.Audio = c.m.Audio[:0]
	c.audioOut = c.audioOut[:0]
	c.audio = nil
	return nil
}

// resampler converts stereo int16 sound between sample rates by straight-
// line interpolation, carrying its position from one piece to the next
// (n64Resampler in n64host.go).
type resampler struct {
	pos          float64 // where the next output sample is; 0 = the last input sample
	prevL, prevR float64
}

func (r *resampler) run(in []int16, inRate, outRate int, out []int16) []int16 {
	n := len(in) / 2
	if n == 0 {
		return out
	}
	step := float64(inRate) / float64(outRate)
	at := func(i int) (float64, float64) { // i = 0 is the previous piece's last sample
		if i == 0 {
			return r.prevL, r.prevR
		}
		return float64(in[2*i-2]), float64(in[2*i-1])
	}
	pos := r.pos
	for pos < float64(n) {
		i := int(pos)
		f := pos - float64(i)
		l0, r0 := at(i)
		l1, r1 := at(i + 1)
		out = append(out, int16(l0+(l1-l0)*f), int16(r0+(r1-r0)*f))
		pos += step
	}
	r.pos = pos - float64(n)
	r.prevL, r.prevR = float64(in[2*n-2]), float64(in[2*n-1])
	return out
}
