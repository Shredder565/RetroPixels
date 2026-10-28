package main

// The RetroPixels SNES Debugger (tools/snes-debugger) opens alongside SNES
// games when Options > Open the SNES Debugger with SNES games is ticked (on
// unless it's unticked). The SNES core notes who writes VRAM, CGRAM and WRAM
// from the first frame; when the debugger asks (a file in the session's
// requests folder), the game thread takes a capture between two frames and
// internal/snesdbg writes it where the debugger reads it.

import (
	"encoding/json"
	"time"

	"retropixels/cores/snes"
	"retropixels/internal/snesdbg"
)

// snesDebugger: Options > Open the SNES Debugger with SNES games (unset: on).
func (s Settings) snesDebugger() bool { return boolOr(s.SNESDebugger, true) }

type snesTracer interface {
	EnableTrace()
	DebugCapture() (*snes.Capture, error)
}

type snesDbgState struct {
	sess   *snesdbg.Session
	tr     snesTracer
	pollAt time.Time
}

// attachSNESDebugger starts the tracer and the debugger's session; call it
// before start. launch: start the debugger program if it isn't running.
func (s *session) attachSNESDebugger(launch bool) error {
	tr, ok := s.core.(snesTracer)
	if !ok || s.game.System != "snes" {
		return nil
	}
	tr.EnableTrace()
	ds, err := snesdbg.Start(s.game.ROM, s.game.Name, "", "RetroPixels Native", launch)
	if err != nil {
		return err
	}
	s.dbg = &snesDbgState{sess: ds, tr: tr}
	return nil
}

// pollSNESDebugger (game thread): answers the debugger's capture requests.
func (s *session) pollSNESDebugger(now time.Time) {
	d := s.dbg
	if d == nil || now.Sub(d.pollAt) < 250*time.Millisecond {
		return
	}
	d.pollAt = now
	for _, id := range d.sess.Pending() {
		cp, err := d.tr.DebugCapture()
		if err != nil {
			continue
		}
		meta, _ := json.MarshalIndent(cp.Meta, "", " ")
		w, h := s.core.Width(), s.core.Height()
		px := append([]uint32(nil), s.core.Screen()...)
		sess := d.sess
		go func(id string) {
			if err := sess.Save(id, meta, cp.Files, snesdbg.ScreenPNG(px, w, h)); err == nil {
				s.host.event("message", "SNES Debugger: capture saved")
			}
		}(id)
	}
}

func (s *session) endSNESDebugger() {
	if s.dbg != nil {
		s.dbg.sess.End()
	}
}
