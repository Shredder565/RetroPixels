// Package ps3 is the PlayStation 3 for the native app: RetroPixels' own PS3
// emulator (package retropixels/ps3) wrapped as a core.Core. It plays one
// game, Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled.
//
// A game opens from its PSN package (.pkg), which is installed (unpacked
// and decrypted) into RetroPixels' folder the first time, or from the
// PARAM.SFO of a game installed already (as another emulator keeps it:
// dev_hdd0/game/<title id>/PARAM.SFO).
package ps3

import (
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"runtime/debug"
	"strings"
	"sync"

	"retropixels/core"
	machine "retropixels/ps3"
	"retropixels/wiiu"
)

// Buttons: bit i of SetInput (as a DUALSHOCK 3).
var Buttons = []string{"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
	"Start", "Select", "Up", "Down", "Left", "Right",
	"Stick Up", "Stick Down", "Stick Left", "Stick Right"}

var bits = [16]uint32{machine.PadCross, machine.PadCircle, machine.PadSquare, machine.PadTriangle,
	machine.PadL1, machine.PadR1, machine.PadL2, machine.PadR2, machine.PadL3, machine.PadR3,
	machine.PadStart, machine.PadSelect, machine.PadUp, machine.PadDown, machine.PadLeft, machine.PadRight}

const (
	bStickUp, bStickDown, bStickLeft, bStickRight = 1 << 16, 1 << 17, 1 << 18, 1 << 19

	deadZone = 0.12
	width    = 1280
	height   = 720
)

// ControlsNote is shown in the Controls window.
const ControlsNote = "PlayStation 3: RetroPixels plays Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled. " +
	"Open the game's .pkg (the first time it's installed into RetroPixels' folder, which takes a minute), " +
	"or the PARAM.SFO of the game installed already. The left stick is the controller's left stick."

// PS3 is one running game.
type PS3 struct {
	m    *machine.Machine
	dir  string // the installed game
	rate int

	// installing a package (in the background)
	installing bool
	instMu     sync.Mutex
	instDone   uint64
	instTotal  uint64
	instErr    error
	instOK     bool
	pkgPath    string

	buttons  uint32
	sticks   [4]float32
	gpuTried bool

	screen []uint32
	w, h   int
	audio  []float32
	frac   float64
	lastL  float32
	lastR  float32

	logf  *os.File
	fatal string // why the game stopped, shown on the screen
}

// New opens a game: a .pkg (installed first) or an installed game's
// PARAM.SFO or EBOOT.BIN.
func New(rom []byte, opts core.Options) (core.Core, error) {
	if opts.Path == "" {
		return nil, errors.New("open the PS3 game's file from disk (its .pkg, or its PARAM.SFO)")
	}
	c := &PS3{rate: opts.SampleRate, w: width, h: height}
	if c.rate <= 0 {
		c.rate = 48000
	}
	c.screen = make([]uint32, c.w*c.h)
	switch ext := strings.ToLower(filepath.Ext(opts.Path)); ext {
	case ".pkg":
		f, err := os.Open(opts.Path)
		if err != nil {
			return nil, err
		}
		p, err := machine.OpenPKG(f)
		f.Close()
		if err != nil {
			return nil, fmt.Errorf("%s isn't a PS3 package RetroPixels can read: %v", filepath.Base(opts.Path), err)
		}
		c.dir = filepath.Join(dataDir(opts.DataDir), "ps3", "games", titleID(p.ContentID))
		if _, err := os.Stat(filepath.Join(c.dir, "installed.txt")); err != nil {
			c.installing = true
			c.pkgPath = opts.Path
			go c.install()
			return c, nil
		}
	default: // .sfo, .bin: the game's folder
		dir := filepath.Dir(opts.Path)
		if strings.EqualFold(filepath.Base(dir), "USRDIR") {
			dir = filepath.Dir(dir)
		}
		c.dir = dir
	}
	if err := c.start(); err != nil {
		return nil, err
	}
	return c, nil
}

// titleID: NPEB00149 from a content ID (EP4062-NPEB00149_00-...).
func titleID(content string) string {
	if i := strings.IndexByte(content, '-'); i >= 0 && len(content) >= i+10 {
		return content[i+1 : i+10]
	}
	if content == "" {
		return "game"
	}
	return strings.Map(func(r rune) rune {
		if r < 32 || strings.ContainsRune(`<>:"/\|?*`, r) {
			return '_'
		}
		return r
	}, content)
}

func dataDir(d string) string {
	if d != "" {
		return d
	}
	if base, err := os.UserConfigDir(); err == nil {
		return filepath.Join(base, "RetroPixels Native")
	}
	return "."
}

func (c *PS3) install() {
	err := machine.Install(c.pkgPath, c.dir, func(done, total uint64) {
		c.instMu.Lock()
		c.instDone, c.instTotal = done, total
		c.instMu.Unlock()
	})
	if err == nil {
		err = os.WriteFile(filepath.Join(c.dir, "installed.txt"), []byte("installed from "+c.pkgPath+"\r\n"), 0o644)
	}
	c.instMu.Lock()
	c.instErr = err
	c.instOK = err == nil
	c.instMu.Unlock()
}

// start makes the machine for the installed game.
func (c *PS3) start() error {
	if c.logf == nil {
		os.MkdirAll(c.dir, 0o755)
		if f, err := os.Create(filepath.Join(c.dir, "RetroPixels-ps3.log")); err == nil {
			c.logf = f
		}
	}
	var log io.Writer
	if c.logf != nil {
		log = c.logf
	}
	m, err := machine.New(machine.Options{GameDir: c.dir, SaveDir: filepath.Join(c.dir, "savedata"), Log: log})
	if err != nil {
		return fmt.Errorf("couldn't start the game: %v", err)
	}
	c.m = m
	return nil
}

func (c *PS3) Info() core.Info {
	return core.Info{System: "ps3", Buttons: Buttons, Players: 1, Stereo: true, Aspect: 16.0 / 9}
}

func (c *PS3) FPS() float64 { return 59.94 }

func (c *PS3) Width() int       { return c.w }
func (c *PS3) Height() int      { return c.h }
func (c *PS3) Screen() []uint32 { return c.screen[:c.w*c.h] }

func (c *PS3) SetInput(player int, b uint32) {
	if player == 0 {
		c.buttons = b
	}
}

// SetAnalog: axes 0, 1 = left stick X, Y (down is +), 2, 3 = right stick.
func (c *PS3) SetAnalog(player, axis int, v float32) {
	if player == 0 && axis >= 0 && axis < 4 {
		c.sticks[axis] = v
	}
}

// AnalogStick: the core reads the left stick itself.
func (c *PS3) AnalogStick() bool { return true }

// UsesRightStick: the right stick is the controller's.
func (c *PS3) UsesRightStick() bool { return true }

func (c *PS3) ControlsNote() string {
	if c.m == nil {
		return ControlsNote
	}
	return ControlsNote + " Drawing with " + c.m.Renderer() + "."
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

func (c *PS3) RunFrame() {
	if c.installing {
		c.instMu.Lock()
		done, total, err, ok := c.instDone, c.instTotal, c.instErr, c.instOK
		c.instMu.Unlock()
		switch {
		case err != nil:
			c.message("Couldn't install the game: " + err.Error())
			return
		case ok:
			c.installing = false
			if err := c.start(); err != nil {
				c.fatal = err.Error()
			}
		default:
			pct := 0
			if total > 0 {
				pct = int(done * 100 / total)
			}
			c.message(fmt.Sprintf("Installing the game into RetroPixels (only the first time)... %d%%", pct))
			return
		}
	}
	if c.m == nil {
		c.message(c.fatal)
		return
	}
	if !c.gpuTried {
		// the graphics card, made on the game's thread (which draws with it)
		c.gpuTried = true
		if os.Getenv("RETRO_PS3_SOFTWARE") == "" {
			if err := useGPU(c.m); err != nil && c.logf != nil {
				fmt.Fprintf(c.logf, "drawing in software: no graphics card (%v)\r\n", err)
			}
		}
	}
	var b uint32
	for i, bit := range bits {
		if c.buttons&(1<<i) != 0 {
			b |= bit
		}
	}
	lx, ly := axis(c.sticks[0]), axis(c.sticks[1])
	if c.buttons&bStickLeft != 0 {
		lx = -1
	} else if c.buttons&bStickRight != 0 {
		lx = 1
	}
	if c.buttons&bStickUp != 0 {
		ly = -1
	} else if c.buttons&bStickDown != 0 {
		ly = 1
	}
	c.m.SetPad(b, lx, ly, axis(c.sticks[2]), axis(c.sticks[3]))
	if c.fatal == "" {
		func() {
			defer func() {
				if r := recover(); r != nil {
					c.fatal = fmt.Sprintf("RetroPixels crashed: %v", r)
					if c.logf != nil {
						fmt.Fprintf(c.logf, "%s\r\n%s\r\n%s\r\n", c.fatal, c.m.Threads(), debug.Stack())
					}
				}
			}()
			if err := c.m.RunFrame(); err != nil && c.fatal == "" {
				c.fatal = "The game stopped: " + err.Error()
				if c.logf != nil {
					fmt.Fprintf(c.logf, "%s\r\n%s\r\n", c.fatal, c.m.Threads())
				}
			}
		}()
		if c.m.Exited && c.fatal == "" {
			c.fatal = "The game ended."
		}
	}
	c.takePicture()
	c.takeSound()
}

// message shows text on a dark screen.
func (c *PS3) message(s string) {
	for i := range c.screen {
		c.screen[i] = 0x101018
	}
	wiiu.DrawText(c.screen[:c.w*c.h], c.w, c.h, wiiu.WrapText(s, c.w/12-4))
}

func (c *PS3) takePicture() {
	if sc := c.m.Picture(); sc != nil && sc.W > 0 && sc.H > 0 {
		if len(c.screen) < sc.W*sc.H {
			c.screen = make([]uint32, sc.W*sc.H)
		}
		c.w, c.h = sc.W, sc.H
		copy(c.screen, sc.Pix)
	}
	if c.fatal != "" {
		wiiu.DrawText(c.screen[:c.w*c.h], c.w, c.h, wiiu.WrapText(c.fatal, c.w/12-4))
	}
}

// takeSound converts the machine's 48 kHz sound to the output rate.
func (c *PS3) takeSound() {
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

func (c *PS3) TakeAudio() []float32 {
	a := c.audio
	c.audio = nil
	return a
}

// Reset starts the game again.
func (c *PS3) Reset() {
	if c.m == nil {
		return
	}
	c.m.Close()
	c.m, c.fatal, c.gpuTried = nil, "", false
	if err := c.start(); err != nil {
		c.fatal = err.Error()
	}
}

func (c *PS3) SaveState() ([]byte, error) {
	return nil, errors.New("save states aren't available for the PS3 yet (the game saves its own progress)")
}

func (c *PS3) LoadState([]byte) error {
	return errors.New("save states aren't available for the PS3 yet")
}

// SRAM: the game's saves are files in its folder.
func (c *PS3) SRAM() []byte { return nil }

// Close lets go of the graphics card and the log.
func (c *PS3) Close() {
	if c.m != nil {
		c.m.Close()
	}
	if c.logf != nil {
		c.logf.Close()
	}
}
