// Package wiiu is the Wii U for the native app: RetroPixels' own Wii U
// emulator (package retropixels/wiiu) wrapped as a core.Core.
//
// The picture is the TV, or the GamePad's screen when the program only draws
// there; the "TV / GamePad" button switches between them. The left stick is
// the GamePad's left stick and the right stick its right stick.
package wiiu

import (
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"retropixels/core"
	machine "retropixels/wiiu"
)

// Buttons: bit i of SetInput.
var Buttons = []string{"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "Home",
	"Up", "Down", "Left", "Right", "L Stick Click", "R Stick Click",
	"Stick Up", "Stick Down", "Stick Left", "Stick Right", "TV / GamePad"}

// the GamePad bit for each of the first 17 buttons
var bits = [17]uint32{machine.BtnA, machine.BtnB, machine.BtnX, machine.BtnY, machine.BtnL, machine.BtnR,
	machine.BtnZL, machine.BtnZR, machine.BtnPlus, machine.BtnMinus, machine.BtnHome,
	machine.BtnUp, machine.BtnDown, machine.BtnLeft, machine.BtnRight, machine.BtnStickL, machine.BtnStickR}

const (
	bStickUp, bStickDown, bStickLeft, bStickRight = 1 << 17, 1 << 18, 1 << 19, 1 << 20
	bSwap                                         = 1 << 21

	deadZone = 0.12
)

// ControlsNote is shown in the Controls window.
const ControlsNote = "Left stick and right stick = the GamePad's sticks. " +
	"TV / GamePad switches which screen you see. Wii U homebrew (.rpx) runs now. For a game, open the .rpx in its code folder, " +
	"a .wua, or the title.tmd of an encrypted title (with your console's common key in a keys.txt next to it or next to RetroPixels)."

// WiiU is one running program.
type WiiU struct {
	m    *machine.Machine
	rpx  []byte
	name string
	path string
	rate int

	buttons uint32
	prev    uint32
	sticks  [4]float32
	show    int  // 0 = TV, 1 = GamePad
	chosen  bool // the player picked the screen

	screen []uint32
	w, h   int
	audio  []float32
	frac   float64
	lastL  float32
	lastR  float32
}

// New starts a program (.rpx).
func New(rpx []byte, opts core.Options) (core.Core, error) {
	name := opts.Name
	if name == "" {
		name = "game.rpx"
	}
	c := &WiiU{rpx: rpx, name: name, path: opts.Path, rate: opts.SampleRate}
	m, err := c.start(opts.SRAM)
	if err != nil {
		return nil, err
	}
	c.m = m
	if c.rate <= 0 {
		c.rate = 48000
	}
	c.w, c.h = machine.TVWidth, machine.TVHeight
	c.screen = make([]uint32, c.w*c.h)
	return c, nil
}

func (c *WiiU) Info() core.Info {
	return core.Info{System: "wiiu", Buttons: Buttons, Players: 1, Stereo: true, Aspect: 16.0 / 9}
}

func (c *WiiU) FPS() float64 { return machine.FPS }

func (c *WiiU) Width() int       { return c.w }
func (c *WiiU) Height() int      { return c.h }
func (c *WiiU) Screen() []uint32 { return c.screen[:c.w*c.h] }

func (c *WiiU) SetInput(player int, b uint32) {
	if player == 0 {
		c.buttons = b
	}
}

// SetAnalog: axes 0, 1 = left stick X, Y (down is +), 2, 3 = right stick.
func (c *WiiU) SetAnalog(player, axis int, v float32) {
	if player == 0 && axis >= 0 && axis < 4 {
		c.sticks[axis] = v
	}
}

// AnalogStick: the core reads the left stick itself.
func (c *WiiU) AnalogStick() bool { return true }

// UsesRightStick: the right stick is the GamePad's.
func (c *WiiU) UsesRightStick() bool { return true }

func (c *WiiU) ControlsNote() string { return ControlsNote }

func (c *WiiU) Rumble() float32 {
	if c.m.Rumble() {
		return 1
	}
	return 0
}

func axis(v float32) float32 {
	a := math.Abs(float64(v))
	if a < deadZone {
		return 0
	}
	s := math.Min(1, (a-deadZone)/(1-deadZone))
	if v < 0 {
		return float32(-s)
	}
	return float32(s)
}

func (c *WiiU) pad() machine.PadState {
	var p machine.PadState
	for i, bit := range bits {
		if c.buttons&(1<<i) != 0 {
			p.Buttons |= bit
		}
	}
	p.LX, p.LY = axis(c.sticks[0]), -axis(c.sticks[1])
	p.RX, p.RY = axis(c.sticks[2]), -axis(c.sticks[3])
	if c.buttons&bStickLeft != 0 {
		p.LX = -1
	} else if c.buttons&bStickRight != 0 {
		p.LX = 1
	}
	if c.buttons&bStickUp != 0 {
		p.LY = 1
	} else if c.buttons&bStickDown != 0 {
		p.LY = -1
	}
	return p
}

func (c *WiiU) RunFrame() {
	if c.buttons&bSwap != 0 && c.prev&bSwap == 0 {
		c.show ^= 1
		c.chosen = true
	}
	c.prev = c.buttons
	c.m.SetGamePad(c.pad())
	c.m.RunFrame()
	c.takePicture()
	c.takeSound()
}

// takeSound converts the machine's 48 kHz sound to the output rate.
func (c *WiiU) takeSound() {
	in := c.m.TakeAudio()
	if c.rate == 48000 {
		c.audio = append(c.audio, in...)
		return
	}
	step := 48000 / float64(c.rate)
	for i := 0; i+1 < len(in); i += 2 {
		l, r := in[i], in[i+1]
		for c.frac < 1 {
			t := float32(c.frac)
			c.audio = append(c.audio, c.lastL+(l-c.lastL)*t, c.lastR+(r-c.lastR)*t)
			c.frac += step
		}
		c.frac--
		c.lastL, c.lastR = l, r
	}
}

func (c *WiiU) takePicture() {
	which := c.show
	if !c.chosen {
		// the TV, unless the program only draws on the GamePad
		which = 0
		if !c.m.PictureUsed(0) && c.m.PictureUsed(1) {
			which = 1
		}
	}
	pix, w, h := c.m.Picture(which)
	if len(c.screen) < w*h {
		c.screen = make([]uint32, w*h)
	}
	c.w, c.h = w, h
	copy(c.screen, pix)
	var msg string
	switch {
	case c.m.Err != nil:
		msg = "The program stopped: " + c.m.Err.Error()
	case c.m.Exited:
		msg = fmt.Sprintf("The program ended (exit code %d).", int32(c.m.ExitCode))
	}
	if msg != "" {
		machine.DrawText(c.screen[:w*h], w, h, machine.WrapText(msg, w/12-4))
	}
}

func (c *WiiU) TakeAudio() []float32 {
	a := c.audio
	c.audio = nil
	return a
}

// start makes the machine: a program in a title's code folder gets the
// title's folders, and its saves come back from sram.
func (c *WiiU) start(sram []byte) (*machine.Machine, error) {
	files := machine.TitleFolders(c.path)
	if c.path != "" && strings.EqualFold(filepath.Ext(c.path), ".wua") {
		rpx, name, f, err := machine.TitleFromWUA(c.path)
		if err != nil {
			return nil, err
		}
		c.rpx, c.name, files = rpx, name, f
	} else if c.isNUS() {
		// an encrypted title: its title.tmd was opened
		var keyDirs []string
		if exe, err := os.Executable(); err == nil {
			keyDirs = append(keyDirs, filepath.Dir(exe))
		}
		rpx, name, f, err := machine.TitleFromNUS(filepath.Dir(c.path), keyDirs...)
		if err != nil {
			return nil, err
		}
		c.rpx, c.name, files = rpx, name, f
	}
	if files == nil {
		files = machine.NewFileSystem()
	}
	if len(sram) > 0 {
		if err := files.LoadSaveData(sram); err != nil {
			return nil, err
		}
	}
	m, err := machine.New(c.rpx, c.name, files)
	if err != nil {
		return nil, err
	}
	m.UseHostGPU()
	return m, nil
}

// isNUS: the file opened is an encrypted title's title.tmd.
func (c *WiiU) isNUS() bool {
	return c.path != "" && strings.EqualFold(filepath.Ext(c.path), ".tmd")
}

// Reset starts the program again (with the saves it made).
func (c *WiiU) Reset() {
	if m, err := c.start(c.SRAM()); err == nil {
		c.m.Close()
		c.m = m
	}
}

func (c *WiiU) SaveState() ([]byte, error) {
	return nil, errors.New("save states aren't available for the Wii U yet")
}

func (c *WiiU) LoadState([]byte) error {
	return errors.New("save states aren't available for the Wii U yet")
}

// Close lets go of the host GPU.
func (c *WiiU) Close() { c.m.Close() }

// SRAM: the program's save files.
func (c *WiiU) SRAM() []byte { return c.m.Files.SaveData() }
