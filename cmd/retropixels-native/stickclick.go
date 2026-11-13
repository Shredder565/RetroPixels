package main

import "time"

// stickClick: a stick click (L3 / R3) counts as the save-state shortcut only
// when it is held for a moment with the stick left in the middle. Moving a
// character with the stick, a hard flick (left then right quickly, say)
// often clicks the stick by accident; that used to load a state, which looked
// like the game jumping back to wherever the state was saved (Turtles in
// Time back at its intro). The same rule is in the web page (index.html).
type stickClick struct {
	at    time.Time // when the click went down (zero: up, or spoiled)
	fired bool
}

const (
	stickClickHold   = 350 * time.Millisecond
	stickClickCenter = 0.5 // the stick further out than this: moving, not clicking
)

// step: the click (down), whether its stick is pushed (moved), and the time.
// It says true once per hold, when the click has been held long enough with
// the stick in the middle all along.
func (c *stickClick) step(down, moved bool, now time.Time) bool {
	if !down {
		*c = stickClick{}
		return false
	}
	if c.fired {
		return false
	}
	if moved {
		c.fired = true // spoiled: wait for the click to come up again
		return false
	}
	if c.at.IsZero() {
		c.at = now
		return false
	}
	if now.Sub(c.at) >= stickClickHold {
		c.fired = true
		return true
	}
	return false
}

// stickMoved: the stick (x, y) is away from the middle.
func stickMoved(x, y float32) bool {
	return x < -stickClickCenter || x > stickClickCenter || y < -stickClickCenter || y > stickClickCenter
}
