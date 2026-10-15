package main

import (
	"bytes"
	"errors"
	"fmt"
	"math"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"retropixels/core"
)

// The game running: its core, driven on its own thread (so menus, dialogs
// and moving the window never stop it), paced by the sound card like the
// web player, with input, battery saves, save states and recording.

const sampleRate = 48000

// soundOut is the sound card (waveOut on Windows).
type soundOut interface {
	Write(pcm []int16) // 16-bit stereo
	Queued() float64   // seconds waiting to be played; < 0 when there is no sound device
	Clear()
}

// host is what a session needs from the window.
type host interface {
	present(pix []uint32, w, h int) // show a picture (called on the game's thread)
	pads() [4]PadState              // the controllers
	rumble(pad int, strong, weak float32)
	event(name string, detail string) // tell the window something happened (any thread)
}

type session struct {
	game   *loadedGame
	core   core.Core
	info   core.Info
	aspect float64
	fps    float64
	snd    soundOut
	host   host

	// games with extra controls (Game Boy tilt cartridges): the core reads
	// the right stick itself, and has a note for the Controls window
	rightStick bool
	note       string
	// cores that read the left stick themselves (the N64's analog stick):
	// it doesn't work their D-pad
	analogStick bool

	cmds chan func()
	done chan struct{}

	// set from the window's thread
	paused, fast, holdFast, modal atomic.Bool
	volume                        atomic.Uint32 // math.Float32bits of 0..1
	mouseX                        atomic.Uint32 // float32 bits, -1..1, or NaN when not over the picture
	mousePaddle                   atomic.Bool
	mouseFire                     atomic.Bool
	seat                          atomic.Int32 // the player the keyboard and first controller play as (seat.go)

	keysMu sync.Mutex
	keys   map[string]bool
	taps   map[string]bool // pressed since the last frame (a quick tap still counts once)
	cmap   ControlMap

	// game thread only
	pacer     *pacer
	input     *inputState
	pcm       []int16
	lastSRAM  []byte
	sramAt    time.Time
	undo      []byte
	rec       *recorder
	prevPad   PadState
	rsDownAt  time.Time
	rsFired   bool
	rsxDir    int // the right stick pushed left (-1) or right (1): changes the save state slot
	rumbleLvl float32
	rumbleAt  time.Time
	failed    error
	quit      bool
	guard     soundGuard
	// an Archipelago client's LOCK holds the game still until then (archipelago.go)
	apHoldUntil time.Time

	// RETRO_NATIVE_STATS=<file>: frames per second and queued sound, every 2 s (for testing)
	stats      *os.File
	statFrames int
	statAt     time.Time
	statQueued float64
}

func newSession(g *loadedGame, h host, snd soundOut, cmap ControlMap) (*session, error) {
	mk := coreMakers[g.System]
	if mk == nil {
		return nil, errNotYet{systemByID(g.System).Label + " games are"}
	}
	var sram []byte
	if b, err := os.ReadFile(sramPath(g.System, g.Name)); err == nil && len(b) > 0 {
		sram = b
	}
	fw := loadFirmware()
	for chip, b := range g.Firmware {
		fw[chip] = b
	}
	c, err := safeNew(mk, g.ROM, core.Options{SampleRate: sampleRate, SRAM: sram, Name: g.Name, Dips: g.Dips, Firmware: fw})
	if err != nil {
		return nil, err
	}
	s := &session{game: g, core: c, info: c.Info(), host: h, snd: snd, cmds: make(chan func(), 64), done: make(chan struct{}),
		keys: map[string]bool{}, taps: map[string]bool{}, cmap: cmap, input: newInputState()}
	s.fps = c.FPS()
	if r, ok := c.(interface{ UsesRightStick() bool }); ok {
		s.rightStick = r.UsesRightStick()
	}
	if a, ok := c.(interface{ AnalogStick() bool }); ok {
		s.analogStick = a.AnalogStick()
	}
	if n, ok := c.(interface{ ControlsNote() string }); ok {
		s.note = n.ControlsNote()
	}
	s.pacer = newPacer(s.fps)
	s.aspect = pictureAspect(s.info.Aspect, g.System, c.Width(), c.Height())
	s.volume.Store(math.Float32bits(0.8))
	s.mouseX.Store(math.Float32bits(float32(math.NaN())))
	if sr := c.SRAM(); sr != nil {
		s.lastSRAM = append([]byte{}, sr...)
	}
	s.sramAt = time.Now()
	if p := os.Getenv("RETRO_NATIVE_STATS"); p != "" {
		s.stats, _ = os.Create(p)
		s.statAt = time.Now()
	}
	return s, nil
}

// safeNew: a core that panics on a bad file reports an error instead.
func safeNew(mk func([]byte, core.Options) (core.Core, error), rom []byte, o core.Options) (c core.Core, err error) {
	defer func() {
		if r := recover(); r != nil {
			c, err = nil, fmt.Errorf("the emulator couldn't start this game (%v)", r)
		}
	}()
	return mk(rom, o)
}

func (s *session) start() { go s.loop() }

// do runs f on the game's thread.
func (s *session) do(f func()) {
	select {
	case s.cmds <- f:
	case <-s.done:
	}
}

// call runs f on the game's thread and waits for it.
func (s *session) call(f func() error) error {
	ch := make(chan error, 1)
	select {
	case s.cmds <- func() { ch <- f() }:
	case <-s.done:
		return errors.New("the game has stopped")
	}
	select {
	case err := <-ch:
		return err
	case <-s.done:
		return errors.New("the game has stopped")
	}
}

// stop ends the game: the battery save is written and the recording (if any)
// is handed back to be finished.
func (s *session) stop() *recorder {
	var rec *recorder
	s.call(func() error {
		s.checkSRAM(true)
		rec, s.rec = s.rec, nil
		s.setRumble(0)
		s.snd.Clear()
		s.quit = true
		return nil
	})
	<-s.done
	return rec
}

func (s *session) setKey(k string, down bool) {
	s.keysMu.Lock()
	if down {
		s.keys[k] = true
		s.taps[k] = true
	} else {
		delete(s.keys, k)
	}
	s.keysMu.Unlock()
}

func (s *session) clearKeys() {
	s.keysMu.Lock()
	s.keys = map[string]bool{}
	s.taps = map[string]bool{}
	s.keysMu.Unlock()
	s.holdFast.Store(false)
}

func (s *session) setControlMap(m ControlMap) {
	s.keysMu.Lock()
	s.cmap = m
	s.keysMu.Unlock()
}

func (s *session) usesKey(k string) bool {
	s.keysMu.Lock()
	defer s.keysMu.Unlock()
	return s.cmap.usesKey(k)
}

func (s *session) loop() {
	runtime.LockOSThread()
	defer close(s.done)
	if s.stats != nil {
		defer s.stats.Close()
	}
	timer := time.NewTimer(time.Hour)
	last := time.Now()
	var lastPoll time.Time
	for {
		// commands first
		for drained := false; !drained; {
			select {
			case f := <-s.cmds:
				if s.runCmd(f) {
					return
				}
			default:
				drained = true
			}
		}
		now := time.Now()
		dt := now.Sub(last).Seconds()
		last = now
		wait := 16 * time.Millisecond
		if s.paused.Load() || s.modal.Load() || s.failed != nil || s.apHeld(now) {
			s.pacer.reset()
			if now.Sub(lastPoll) > 15*time.Millisecond && !s.modal.Load() && s.failed == nil {
				lastPoll = now
				pads := s.host.pads()
				s.padHotkeys(firstPad(pads))
			}
		} else {
			fast := s.fast.Load() || s.holdFast.Load()
			q := s.snd.Queued()
			if s.guard.untrusted {
				q = -1
			}
			frames := s.pacer.step(dt, q, fast)
			if s.guard.check(now, frames, fast, s.fps) {
				s.host.event("message", "The sound device isn't keeping time; the game follows the clock instead")
			}
			if frames > 0 {
				s.readInput()
				lastPoll = now
				for i := 0; i < frames && s.failed == nil; i++ {
					s.runFrame(!fast || i == 0)
				}
				if s.failed == nil {
					s.present()
				}
				s.statFrames += frames
				s.statQueued = q
			}
			if s.stats != nil && now.Sub(s.statAt) >= 2*time.Second {
				fmt.Fprintf(s.stats, "fps=%.2f queued=%.3f\n", float64(s.statFrames)/now.Sub(s.statAt).Seconds(), s.statQueued)
				s.statFrames, s.statAt = 0, now
			}
			if r, ok := s.core.(core.Rumbler); ok && s.failed == nil {
				lvl := r.Rumble()
				if fast {
					lvl = 0
				}
				s.setRumble(lvl)
			}
			if now.Sub(s.sramAt) > 2*time.Second {
				s.checkSRAM(false)
			}
			if fast {
				wait = time.Duration(float64(time.Second) / s.fps)
			} else {
				wait = time.Duration(s.pacer.untilNext() * float64(time.Second))
				if wait > 4*time.Millisecond {
					wait = 4 * time.Millisecond
				}
				if wait < time.Millisecond {
					wait = time.Millisecond
				}
			}
		}
		timer.Reset(wait)
		select {
		case f := <-s.cmds:
			timer.Stop() // (Go 1.23+ timers: no stale tick to drain)
			if s.runCmd(f) {
				return
			}
		case <-timer.C:
		}
	}
}

// runCmd runs a command; true means the session is over.
func (s *session) runCmd(f func()) bool {
	func() {
		defer func() {
			if r := recover(); r != nil {
				s.fail(fmt.Errorf("%v", r))
			}
		}()
		f()
	}()
	return s.quit
}

func (s *session) fail(err error) {
	s.failed = err
	s.snd.Clear()
	s.setRumble(0)
	s.host.event("failed", err.Error())
}

func (s *session) runFrame(sound bool) {
	defer func() {
		if r := recover(); r != nil {
			s.fail(fmt.Errorf("%v", r))
		}
	}()
	s.core.RunFrame()
	a := s.core.TakeAudio()
	if s.rec != nil {
		s.rec.frame(s.core.Screen(), s.core.Width(), s.core.Height())
		s.rec.audio(a, s.info.Stereo)
	}
	if sound && len(a) > 0 {
		s.pcm = toPCM(s.pcm, a, s.info.Stereo, s.currentVolume())
		s.snd.Write(s.pcm)
	}
}

func (s *session) currentVolume() float32 { return math.Float32frombits(s.volume.Load()) }

func (s *session) present() {
	w, h := s.core.Width(), s.core.Height()
	pix := s.core.Screen()
	if w > 0 && h > 0 && len(pix) >= w*h {
		s.host.present(pix[:w*h], w, h)
	}
}

// firstPad: the first connected controller (the one hotkeys listen to).
func firstPad(p [4]PadState) PadState {
	for _, x := range p {
		if x.Connected {
			return x
		}
	}
	return PadState{}
}

func (s *session) readInput() {
	pads := s.host.pads()
	// Controller n plays as player n, counting only connected ones (like the
	// browser's list of gamepads).
	var conn []*PadState
	for i := range pads {
		if pads[i].Connected {
			conn = append(conn, &pads[i])
		}
	}
	players := s.info.Players
	if players < 1 {
		players = 1
	}
	if players > 4 {
		players = 4
	}
	s.keysMu.Lock()
	keys := s.keys
	if len(s.taps) > 0 {
		keys = make(map[string]bool, len(s.keys)+len(s.taps))
		for k := range s.keys {
			keys[k] = true
		}
		for k := range s.taps {
			keys[k] = true
		}
		s.taps = map[string]bool{}
	}
	cmap := s.cmap
	// (the N64 sees controllers plugged in and out)
	if pc, ok := s.core.(interface{ SetConnected(n int) }); ok {
		pc.SetConnected(len(conn))
	}
	for p := 0; p < players; p++ {
		var pad *PadState
		if p < len(conn) {
			pad = conn[p]
		}
		bits := s.input.bits(p, s.info.Buttons, cmap, keys, pad, s.analogStick)
		if p == 0 && s.mousePaddle.Load() && s.mouseFire.Load() {
			bits |= s.buttonBit("Fire")
		}
		who := s.seatOf(p, players)
		s.core.SetInput(who, bits)
		if an, ok := s.core.(core.Analog); ok {
			var ax [4]float32
			if pad != nil {
				ax = pad.Axes
			}
			if p == 0 && s.mousePaddle.Load() {
				if mx := math.Float32frombits(s.mouseX.Load()); !math.IsNaN(float64(mx)) {
					ax[0] = mx
				}
			}
			for i, v := range ax {
				an.SetAnalog(who, i, v)
			}
		}
	}
	s.keysMu.Unlock()
	s.padHotkeys(firstPad(pads))
}

func (s *session) buttonBit(name string) uint32 {
	for i, n := range s.info.Buttons {
		if n == name && i < 32 {
			return 1 << i
		}
	}
	return 0
}

// padHotkeys: click the right stick to save, the left stick to load, flick
// it left or right to change the save state slot, hold it down for a second
// to start or stop recording (as in the web version). The N64 uses its right
// stick for the C buttons, so there View + D-pad left / right changes the
// slot and View + D-pad down starts or stops recording instead (as in the
// web version).
func (s *session) padHotkeys(p PadState) {
	if s.game.System == "n64" {
		vd := p.Buttons[padView] && p.Buttons[padDown]
		if vd && !(s.prevPad.Buttons[padView] && s.prevPad.Buttons[padDown]) {
			s.host.event("pad-record", "")
		}
		// View + D-pad left / right: the save state slot
		for _, d := range [2]struct {
			b   int
			dir string
		}{{padLeft, "-1"}, {padRight, "+1"}} {
			if p.Buttons[padView] && p.Buttons[d.b] && !(s.prevPad.Buttons[padView] && s.prevPad.Buttons[d.b]) {
				s.host.event("pad-slot", d.dir)
			}
		}
	} else if !s.rightStick {
		// flick the right stick left / right: the save state slot
		dir := 0
		if p.Axes[2] > 0.7 {
			dir = 1
		} else if p.Axes[2] < -0.7 {
			dir = -1
		}
		if dir != 0 && dir != s.rsxDir {
			s.host.event("pad-slot", fmt.Sprintf("%+d", dir))
		}
		s.rsxDir = dir
	}
	if p.Buttons[padR3] && !s.prevPad.Buttons[padR3] {
		s.host.event("pad-save", "")
	}
	if p.Buttons[padL3] && !s.prevPad.Buttons[padL3] {
		s.host.event("pad-load", "")
	}
	if p.Axes[3] > 0.7 && !s.rightStick { // (tilt games use the right stick themselves)
		if s.rsDownAt.IsZero() {
			s.rsDownAt = time.Now()
		} else if !s.rsFired && time.Since(s.rsDownAt) > time.Second {
			s.rsFired = true
			s.host.event("pad-record", "")
		}
	} else {
		s.rsDownAt, s.rsFired = time.Time{}, false
	}
	s.prevPad = p
}

func (s *session) setRumble(level float32) {
	if level < 0.02 {
		level = 0
	}
	if level > 1 {
		level = 1
	}
	now := time.Now()
	if level == s.rumbleLvl && (level == 0 || now.Sub(s.rumbleAt) < 500*time.Millisecond) {
		return
	}
	if level != 0 && math.Abs(float64(level-s.rumbleLvl)) < 0.05 && now.Sub(s.rumbleAt) < 80*time.Millisecond {
		return
	}
	s.rumbleLvl, s.rumbleAt = level, now
	s.host.rumble(0, level, level*0.6)
}

// checkSRAM writes the battery save when it has changed.
func (s *session) checkSRAM(force bool) {
	s.sramAt = time.Now()
	sr := s.core.SRAM()
	if sr == nil {
		return
	}
	if bytes.Equal(sr, s.lastSRAM) && !(force && s.lastSRAM == nil) {
		return
	}
	snap := append([]byte{}, sr...)
	if err := writeFileAtomic(sramPath(s.game.System, s.game.Name), snap); err != nil {
		s.host.event("message", "Couldn't write the battery save: "+err.Error())
		return
	}
	s.lastSRAM = snap
}

// ---- commands (called from the window's thread) ----

func (s *session) reset() {
	s.do(func() {
		s.core.Reset()
		s.snd.Clear()
	})
}

// saveState / loadState: slot 1 (ui_saves.go has the slots).
func (s *session) saveState() error { return s.saveSlot(0) }

func (s *session) loadState() error { return s.loadSlot(0) }

func (s *session) undoLoad() error {
	return s.call(func() error {
		if s.undo == nil {
			return errors.New("there's no load to undo")
		}
		if err := s.applyState(s.undo); err != nil {
			return err
		}
		s.undo = nil
		return nil
	})
}

func (s *session) canUndo() bool {
	ok := false
	s.call(func() error { ok = s.undo != nil; return nil })
	return ok
}

func (s *session) applyState(st []byte) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("that save state couldn't be loaded (%v)", r)
		}
	}()
	if err := s.core.LoadState(st); err != nil {
		return err
	}
	s.snd.Clear()
	s.failed = nil
	s.present()
	return nil
}

func (s *session) startRecording(ff, enc string) error {
	return s.startRecordingTo(ff, enc, "balanced", recordingsDir())
}

// startRecordingTo: with the quality and folder picked in the Record menu.
func (s *session) startRecordingTo(ff, enc, quality, dir string) error {
	return s.call(func() error {
		if s.rec != nil {
			return nil
		}
		w, h := s.core.Width(), s.core.Height()
		r, err := startRecorder(ff, enc, quality, dir, s.game.Name, w, h, s.aspect, s.fps, sampleRate)
		if err != nil {
			return err
		}
		s.rec = r
		return nil
	})
}

// stopRecording hands back the recorder to be finished (nil if none).
func (s *session) stopRecording() *recorder {
	var r *recorder
	s.call(func() error { r, s.rec = s.rec, nil; return nil })
	return r
}

func (s *session) recording() (bool, time.Time) {
	var on bool
	var t time.Time
	s.call(func() error {
		if s.rec != nil {
			on, t = true, s.rec.started
		}
		return nil
	})
	return on, t
}

// recError: why the recording broke, if it did.
func (s *session) recError() error {
	var err error
	s.call(func() error {
		if s.rec != nil {
			err = s.rec.failed()
		}
		return nil
	})
	return err
}

// soundGuard notices a sound device that swallows sound faster than it can
// play it (a broken driver, or a "null" device): the game would run fast to
// keep it fed. After 6 s of that, the clock alone paces the game.
type soundGuard struct {
	untrusted bool
	start     time.Time
	frames    int
	strikes   int
}

// check counts frames; true when it has just stopped trusting the device.
func (g *soundGuard) check(now time.Time, frames int, fast bool, fps float64) bool {
	if g.untrusted {
		return false
	}
	if fast || g.start.IsZero() {
		g.start, g.frames = now, 0
		return false
	}
	g.frames += frames
	if d := now.Sub(g.start).Seconds(); d >= 2 {
		if float64(g.frames) > d*fps*1.25 {
			g.strikes++
		} else {
			g.strikes = 0
		}
		g.start, g.frames = now, 0
		if g.strikes >= 3 {
			g.untrusted = true
			return true
		}
	}
	return false
}
