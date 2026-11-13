package main

import (
	"testing"

	"retropixels/core"
)

// A game without a walkthrough: no guide, the switch stays off, and the
// picture goes out untouched.
func TestWalkthroughOnlyForKnownGames(t *testing.T) {
	c, err := newTestCore([]byte{1}, core.Options{})
	if err != nil {
		t.Fatal(err)
	}
	s := &session{core: c, info: c.Info()}
	s.setupWalkthrough(true)
	if s.hasWalkthrough() || s.walkOn.Load() {
		t.Fatal("a walkthrough for the test core")
	}
	pix := c.Screen()
	if got := s.walkthroughFrame(pix, c.Width(), c.Height()); &got[0] != &pix[0] {
		t.Fatal("the picture was copied")
	}
	var none *session
	if none.hasWalkthrough() {
		t.Fatal("no game, but a walkthrough")
	}
}
