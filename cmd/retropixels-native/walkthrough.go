package main

// Walkthrough mode (Game > Walkthrough arrow, F8): for games RetroPixels
// has a walkthrough for (so far Metroid on the NES), an arrow over the
// picture points the way to the next objective. internal/walkthrough reads
// the game's memory and draws it; this file switches it on and off and
// puts it on the picture. Recordings and screenshots stay as the game
// drew them.

import (
	"retropixels/core"
	"retropixels/internal/walkthrough"
)

// setupWalkthrough: a guide for the game, when there is one.
func (s *session) setupWalkthrough(on bool) {
	if m, ok := s.core.(core.Memory); ok {
		s.guide = walkthrough.New(s.info.System, m)
	}
	s.walkOn.Store(on && s.guide != nil)
}

// hasWalkthrough: the game has a walkthrough.
func (s *session) hasWalkthrough() bool { return s != nil && s.guide != nil }

// walkthroughFrame: the picture with the walkthrough's arrow on it (on the
// game's thread, or before it starts).
func (s *session) walkthroughFrame(pix []uint32, w, h int) []uint32 {
	if s.guide == nil || !s.walkOn.Load() {
		return pix
	}
	s.walkBuf = append(s.walkBuf[:0], pix...)
	walkthrough.Draw(s.walkBuf, w, h, s.guide.Update())
	return s.walkBuf
}
