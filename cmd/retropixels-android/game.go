package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"time"

	"retropixels/core"
	"retropixels/cores/arcade"
	"retropixels/internal/games"
)

// game is the running game: its core, the controls, the battery save and
// the save states. Everything happens on the engine's one thread, between
// requests, so nothing here needs a lock.
type game struct {
	g      *games.Game
	st     games.Store
	core   core.Core
	info   core.Info
	aspect float64
	fps    float64
	rate   int

	cmap        games.ControlMap
	input       *games.InputState
	analogStick bool
	rightStick  bool
	note        string
	seat        int

	lastSRAM []byte
	sramAt   time.Time
	undo     []byte
	failed   error
	message  string // shown once with the next frame

	pcm   []int16
	tmp   []int16
	reply frameReply
}

// openOptions: the "open" request's settings for the game.
type openOptions struct {
	SampleRate int               `json:"sampleRate"`
	Dips       map[string]int    `json:"dips"`
	Controls   games.ControlMap  `json:"controls"`           // the player's changes to the system's controls
	Firmware   map[string]string `json:"firmware,omitempty"` // chip programs just chosen, by chip, as files
	Seat       int               `json:"seat"`
	firmware   map[string][]byte // (read from Firmware)
}

func newGame(g *games.Game, st games.Store, o openOptions) (*game, error) {
	mk := games.Makers[g.System]
	if mk == nil {
		return nil, games.NotYetError{What: games.ByID(g.System).Label + " games are"}
	}
	rate := o.SampleRate
	if rate < 8000 || rate > 192000 {
		rate = 48000
	}
	var sram []byte
	if b, err := os.ReadFile(st.SRAMPath(g.System, g.Name)); err == nil && len(b) > 0 {
		sram = b
	}
	fw := st.LoadFirmware()
	for chip, b := range o.firmware {
		fw[chip] = b
	}
	c, err := safeNew(mk, g.ROM, core.Options{SampleRate: rate, SRAM: sram, Name: g.Name, Dips: o.Dips, Firmware: fw})
	if err != nil {
		return nil, err
	}
	s := &game{g: g, st: st, core: c, info: c.Info(), rate: rate, input: games.NewInputState(),
		cmap: games.LoadMap(g.System, o.Controls)}
	s.fps = c.FPS()
	if s.fps <= 0 {
		s.fps = 60
	}
	if r, ok := c.(interface{ UsesRightStick() bool }); ok {
		s.rightStick = r.UsesRightStick()
	}
	if a, ok := c.(interface{ AnalogStick() bool }); ok {
		s.analogStick = a.AnalogStick()
	}
	if n, ok := c.(interface{ ControlsNote() string }); ok {
		s.note = n.ControlsNote()
	}
	s.aspect = games.PictureAspect(s.info.Aspect, g.System, c.Width(), c.Height())
	if sr := c.SRAM(); sr != nil {
		s.lastSRAM = append([]byte{}, sr...)
	}
	s.sramAt = time.Now()
	s.setSeat(o.Seat)
	return s, nil
}

// safeNew: a core that panics on a bad file reports an error instead.
func safeNew(mk games.Maker, rom []byte, o core.Options) (c core.Core, err error) {
	defer func() {
		if r := recover(); r != nil {
			c, err = nil, fmt.Errorf("the emulator couldn't start this game (%v)", r)
		}
	}()
	return mk(rom, o)
}

// ---- running ----

func (s *game) run(q runRequest) *frameReply {
	f := &s.reply
	f.failed, f.pix, f.message, f.rumble = false, nil, s.message, 0
	s.message = ""
	s.pcm = s.pcm[:0]
	if s.failed == nil {
		s.readInput(q)
		for i := 0; i < q.frames && s.failed == nil; i++ {
			s.runFrame(!q.fast || i == 0, q.volume)
		}
	}
	f.pcm = s.pcm
	if s.failed != nil {
		f.failed = true
		f.message = "The emulator stopped: " + s.failed.Error()
		return f
	}
	if q.frames > 0 {
		w, h := s.core.Width(), s.core.Height()
		if pix := s.core.Screen(); w > 0 && h > 0 && len(pix) >= w*h {
			f.w, f.h, f.pix = w, h, pix[:w*h]
		}
		if r, ok := s.core.(core.Rumbler); ok && !q.fast {
			f.rumble = r.Rumble()
		}
		if time.Since(s.sramAt) > 2*time.Second {
			if err := s.checkSRAM(false); err != nil {
				f.message = "Couldn't write the battery save: " + err.Error()
			}
		}
	}
	return f
}

func (s *game) runFrame(sound bool, volume float32) {
	defer func() {
		if r := recover(); r != nil {
			s.failed = fmt.Errorf("%v", r)
		}
	}()
	s.core.RunFrame()
	a := s.core.TakeAudio()
	if sound && len(a) > 0 {
		s.tmp = games.ToPCM(s.tmp, a, s.info.Stereo, volume)
		s.pcm = append(s.pcm, s.tmp...)
	}
}

// readInput: controller n plays as player n; the keyboard and the touch
// controls play as player 1 (with the seat picked in arcade games for 3 or 4).
func (s *game) readInput(q runRequest) {
	players := min(max(s.info.Players, 1), 4)
	if pc, ok := s.core.(interface{ SetConnected(n int) }); ok {
		pc.SetConnected(max(len(q.pads), 1))
	}
	for p := 0; p < players; p++ {
		var pad *games.PadState
		if p < len(q.pads) {
			pad = &games.PadState{Connected: true, Axes: q.pads[p].axes}
			for b := 0; b < games.PadButtons; b++ {
				pad.Buttons[b] = q.pads[p].buttons&(1<<b) != 0
			}
		}
		var held uint32
		if p == 0 {
			held = q.touch
		}
		bits := s.input.Bits(p, s.info.Buttons, s.cmap, q.keys, pad, s.analogStick, held)
		who := s.seatOf(p, players)
		s.core.SetInput(who, bits)
		if an, ok := s.core.(core.Analog); ok {
			var ax [4]float32
			if pad != nil {
				ax = pad.Axes
			}
			if p == 0 {
				for i := 0; i < 2; i++ {
					if q.touchAxes&(1<<i) != 0 {
						ax[i] = q.axis[i]
					}
				}
			}
			for i, v := range ax {
				an.SetAnalog(who, i, v)
			}
		}
	}
}

// checkSRAM writes the battery save when it has changed.
func (s *game) checkSRAM(force bool) error {
	s.sramAt = time.Now()
	if s.failed != nil {
		return nil
	}
	sr := s.core.SRAM()
	if sr == nil {
		return nil
	}
	if bytes.Equal(sr, s.lastSRAM) && !(force && s.lastSRAM == nil) {
		return nil
	}
	snap := append([]byte{}, sr...)
	if err := games.WriteFileAtomic(s.st.SRAMPath(s.g.System, s.g.Name), snap); err != nil {
		return err
	}
	s.lastSRAM = snap
	return nil
}

// ---- arcade players and machine settings ----

type characterCore interface{ Characters() []string }

// seats: how many players the Player choice offers (0: none), as the native app.
func (s *game) seats() int {
	if _, ok := s.core.(characterCore); !ok || s.info.Players < 3 {
		return 0
	}
	return min(s.info.Players, 4)
}

func (s *game) seatNames() []string {
	var out []string
	for n := 0; n < s.seats(); n++ {
		t := fmt.Sprintf("Player %d", n+1)
		if c, ok := s.core.(characterCore); ok {
			if names := c.Characters(); n < len(names) && names[n] != "" {
				t += " (" + names[n] + ")"
			}
		}
		out = append(out, t)
	}
	return out
}

func (s *game) setSeat(n int) int {
	if n < 0 || n >= s.seats() {
		n = 0
	}
	s.seat = n
	return n
}

func (s *game) seatOf(p, players int) int {
	if s.seat > 0 && players > 0 {
		return (p + s.seat) % players
	}
	return p
}

type dipCore interface {
	Dips() []arcade.DipSetting
	SetDip(id string, v int) bool
}

type dipJSON struct {
	ID      string       `json:"id"`
	Label   string       `json:"label"`
	Value   int          `json:"value"`
	Options []dipOptJSON `json:"options"`
}

type dipOptJSON struct {
	Label string `json:"label"`
	Value int    `json:"value"`
}

func (s *game) dips() []dipJSON {
	d, ok := s.core.(dipCore)
	if !ok {
		return []dipJSON{}
	}
	out := []dipJSON{}
	for _, x := range d.Dips() {
		j := dipJSON{ID: x.ID, Label: x.Label, Value: x.Value}
		for _, o := range x.Options {
			j.Options = append(j.Options, dipOptJSON{o.Label, o.Value})
		}
		out = append(out, j)
	}
	return out
}

func (s *game) setDip(id string, v int) error {
	d, ok := s.core.(dipCore)
	if !ok || !d.SetDip(id, v) {
		return errors.New("this game hasn't got that setting")
	}
	return nil
}

// ---- save states ----

const thumbHeight = 120

type slotJSON struct {
	Used  bool   `json:"used"`
	Time  int64  `json:"time"` // ms since 1970
	Thumb string `json:"thumb,omitempty"`
}

func (s *game) slots() []slotJSON {
	out := make([]slotJSON, games.SlotCount)
	for i := range out {
		p := s.st.SlotPath(s.g.System, s.g.Name, i)
		if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
			out[i] = slotJSON{Used: true, Time: fi.ModTime().UnixMilli()}
			if _, err := os.Stat(p + ".png"); err == nil {
				out[i].Thumb = p + ".png"
			}
		}
	}
	return out
}

func (s *game) saveSlot(slot int) error {
	if s.failed != nil {
		return errors.New("the game has stopped")
	}
	if slot < 0 || slot >= games.SlotCount {
		return errors.New("no such slot")
	}
	st, err := s.core.SaveState()
	if err != nil {
		return err
	}
	if len(st) == 0 {
		return errors.New("the emulator returned an empty state")
	}
	if err := games.WriteFileAtomic(s.st.SlotPath(s.g.System, s.g.Name, slot), games.EncodeState(s.g.System, st)); err != nil {
		return err
	}
	w, h := s.core.Width(), s.core.Height()
	if pix := s.core.Screen(); w > 0 && h > 0 && len(pix) >= w*h {
		if b, err := games.ThumbPNG(pix[:w*h], w, h, s.aspect, thumbHeight); err == nil {
			games.WriteFileAtomic(s.st.SlotThumbPath(s.g.System, s.g.Name, slot), b)
		}
	}
	return nil
}

func (s *game) loadSlot(slot int) error {
	if slot < 0 || slot >= games.SlotCount {
		return errors.New("no such slot")
	}
	file, err := os.ReadFile(s.st.SlotPath(s.g.System, s.g.Name, slot))
	if os.IsNotExist(err) {
		return fmt.Errorf("slot %d is empty", slot+1)
	}
	if err != nil {
		return err
	}
	st, err := games.DecodeState(s.g.System, file)
	if err != nil {
		return err
	}
	return s.applyWithUndo(st)
}

func (s *game) applyWithUndo(st []byte) error {
	var before []byte
	if s.failed == nil {
		before, _ = s.core.SaveState()
	}
	if err := s.applyState(st); err != nil {
		return err
	}
	s.undo = before
	return nil
}

func (s *game) applyState(st []byte) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("that save state couldn't be loaded (%v)", r)
		}
	}()
	if err := s.core.LoadState(st); err != nil {
		return err
	}
	s.failed = nil
	return nil
}

func (s *game) undoLoad() error {
	if s.undo == nil {
		return errors.New("there's no load to undo")
	}
	if err := s.applyState(s.undo); err != nil {
		return err
	}
	s.undo = nil
	return nil
}

// importState loads a state file from anywhere: a RetroPixels Native save
// state, or else the bytes go to the emulator as they are.
func (s *game) importState(file []byte) error {
	st := file
	if bytes.HasPrefix(file, games.StateMagic) {
		var err error
		if st, err = games.DecodeState(s.g.System, file); err != nil {
			return err
		}
	}
	if len(st) == 0 {
		return errors.New("that file is empty")
	}
	return s.applyWithUndo(st)
}

func (s *game) reset() {
	defer func() {
		if r := recover(); r != nil {
			s.failed = fmt.Errorf("%v", r)
		}
	}()
	s.core.Reset()
	s.failed = nil // (a game that stopped gets another go)
}
