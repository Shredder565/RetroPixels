// Package pixelds is PixelDS, RetroPixels' own Nintendo DS and Nintendo 3DS
// emulator: one emulator for both handhelds, written for RetroPixels.
//
// Both machines sit behind the same front: one picture with the two screens
// (top above bottom), one set of buttons, the stylus on the bottom screen,
// stereo sound, the game's save and save states. The machines themselves:
//
//	nds   the Nintendo DS (ARM946E-S + ARM7TDMI, cores/pixelds/arm)
//	ctr   the Nintendo 3DS (ARM11 MPCore, with the system software done at
//	      a high level; added stage by stage, see Open)
//
// cmd/pixelds is the program (PixelDS.exe) that the 3DS / DS tab of both
// RetroPixels apps starts.
package pixelds

import (
	"bytes"
	"encoding/binary"
	"errors"
	"path/filepath"
	"strings"
	"time"

	"retropixels/cores/pixelds/nds"
)

// Version of PixelDS (follows RetroPixels' version).
const Version = "2.33.0"

// Buttons in input bit order (both machines; ZL and ZR are the 3DS's).
var Buttons = append(append([]string(nil), nds.Buttons...), "ZL", "ZR")

// Button bits.
const (
	BtnA = 1 << iota
	BtnB
	BtnSelect
	BtnStart
	BtnRight
	BtnLeft
	BtnUp
	BtnDown
	BtnR
	BtnL
	BtnX
	BtnY
	BtnLid
	BtnZL
	BtnZR
)

// System is which handheld a game is for.
type System int

const (
	DS System = iota
	ThreeDS
)

func (s System) String() string {
	if s == ThreeDS {
		return "Nintendo 3DS"
	}
	return "Nintendo DS"
}

// machine is what each handheld provides to the front.
type machine interface {
	RunFrame()
	Frame() []uint32
	SetKeys(bits uint32)
	SetTouch(x, y int, down bool)
	TakeAudio() []float32
	Save() []byte
	SaveChanged() bool
	SaveState() ([]byte, error)
	LoadState([]byte) error
	Title() string
	GameCode() string
	Err() error
}

// circlePad is the 3DS's analog stick.
type circlePad interface{ setCircle(x, y float64) }

// Emulator is one running game.
type Emulator struct {
	System System
	m      machine
	fps    float64
	w, h   int

	bottomX, bottomW int // where the bottom screen is in the picture
}

// Options for Open.
type Options struct {
	SampleRate int
	Save       []byte           // the game's save, or nil
	Now        func() time.Time // the clock (time.Now when nil)
	Firmware   []byte           // a DS firmware image to use (optional)
	UserName   string           // the name games see
	SDCard     string           // the 3DS's SD card folder
	SaveDir    string           // the 3DS game's save data folder
	Log        func(string)     // what the 3DS side reports (optional)
}

// Detect tells which handheld a game file is for, from its contents (and
// its name when the contents don't say).
func Detect(name string, data []byte) (System, error) {
	if len(data) >= 0x200 && string(data[0x100:0x104]) == "NCSD" || len(data) >= 0x200 && string(data[0x100:0x104]) == "NCCH" {
		return ThreeDS, nil
	}
	if len(data) >= 4 && string(data[:4]) == "3DSX" {
		return ThreeDS, nil
	}
	switch strings.ToLower(filepath.Ext(name)) {
	case ".3ds", ".cci", ".cxi", ".3dsx", ".cia":
		return ThreeDS, nil
	}
	if len(data) >= 0x200 && looksDS(data) {
		return DS, nil
	}
	switch strings.ToLower(filepath.Ext(name)) {
	case ".nds", ".srl", ".dsi":
		return DS, nil
	}
	return DS, errors.New("this isn't a DS or 3DS game")
}

// looksDS: the header's program offsets and sizes fit in the file.
func looksDS(d []byte) bool {
	a9o, a9s := binary.LittleEndian.Uint32(d[0x20:]), binary.LittleEndian.Uint32(d[0x2c:])
	a7o, a7s := binary.LittleEndian.Uint32(d[0x30:]), binary.LittleEndian.Uint32(d[0x3c:])
	return a9s > 0 && a7s > 0 && int64(a9o)+int64(a9s) <= int64(len(d)) && int64(a7o)+int64(a7s) <= int64(len(d))
}

// Open starts a game.
func Open(name string, data []byte, o Options) (*Emulator, error) {
	sys, err := Detect(name, data)
	if err != nil {
		return nil, err
	}
	if sys == ThreeDS {
		return open3DS(name, data, o)
	}
	m, err := nds.New(data, nds.Options{SampleRate: o.SampleRate, Save: o.Save, Now: o.Now, Firmware: o.Firmware, UserName: o.UserName})
	if err != nil {
		return nil, err
	}
	return &Emulator{System: DS, m: dsMachine{m}, fps: nds.FPS, w: nds.ScreenW, h: nds.ScreenH * 2, bottomW: nds.ScreenW}, nil
}

func (e *Emulator) FPS() float64           { return e.fps }
func (e *Emulator) Width() int             { return e.w }
func (e *Emulator) Height() int            { return e.h }
func (e *Emulator) Screen() []uint32       { return e.m.Frame() }
func (e *Emulator) RunFrame()              { e.m.RunFrame() }
func (e *Emulator) SetButtons(bits uint32) { e.m.SetKeys(bits) }
func (e *Emulator) TakeAudio() []float32   { return e.m.TakeAudio() }
func (e *Emulator) Save() []byte           { return e.m.Save() }
func (e *Emulator) SaveChanged() bool      { return e.m.SaveChanged() }
func (e *Emulator) Title() string          { return e.m.Title() }
func (e *Emulator) GameCode() string       { return e.m.GameCode() }

// TopHeight is the height of the top screen in the picture (the bottom
// screen, the touch screen, is below it).
func (e *Emulator) TopHeight() int { return e.h / 2 }

// dsMachine: the DS runs whatever it's given, so it never stops.
type dsMachine struct{ *nds.Machine }

func (dsMachine) Err() error { return nil }

// BottomX and BottomWidth: where the bottom screen sits across the
// picture (the 3DS's is narrower than its top screen, and centred).
func (e *Emulator) BottomX() int     { return e.bottomX }
func (e *Emulator) BottomWidth() int { return e.bottomW }

// HasCirclePad: the game's handheld has an analog stick (the 3DS).
func (e *Emulator) HasCirclePad() bool { _, ok := e.m.(circlePad); return ok }

// SetCirclePad moves the 3DS's circle pad (x right, y up, -1 to 1).
func (e *Emulator) SetCirclePad(x, y float64) {
	if c, ok := e.m.(circlePad); ok {
		c.setCircle(max(-1, min(1, x)), max(-1, min(1, y)))
	}
}

// Err is why the game stopped, if it did (the 3DS side stops on what it
// can't run yet, and says what).
func (e *Emulator) Err() error { return e.m.Err() }

// Touch puts the stylus at (x, y) of the bottom screen, in its pixels, or
// lifts it.
func (e *Emulator) Touch(x, y int, down bool) { e.m.SetTouch(x, y, down) }

// stateMagic starts every save state.
var stateMagic = []byte("PixelDS state 1\n")

// SaveState captures the whole machine.
func (e *Emulator) SaveState() ([]byte, error) {
	b, err := e.m.SaveState()
	if err != nil {
		return nil, err
	}
	return append(append([]byte(nil), stateMagic...), b...), nil
}

// LoadState goes back to a captured machine.
func (e *Emulator) LoadState(b []byte) error {
	if !bytes.HasPrefix(b, stateMagic) {
		return errors.New("that isn't a PixelDS save state")
	}
	return e.m.LoadState(b[len(stateMagic):])
}
