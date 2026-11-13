package main

import (
	"testing"
	"time"
)

func TestStickClick(t *testing.T) {
	t0 := time.Unix(1000, 0)
	at := func(ms int) time.Time { return t0.Add(time.Duration(ms) * time.Millisecond) }
	var c stickClick
	// a quick click during a left-then-right flick: never counts
	if c.step(true, true, at(0)) || c.step(true, false, at(400)) || c.step(true, false, at(800)) {
		t.Fatal("a click while the stick was pushed counted")
	}
	c.step(false, false, at(900))
	// a short click in the middle: doesn't count
	if c.step(true, false, at(1000)) || c.step(true, false, at(1100)) || c.step(false, false, at(1150)) {
		t.Fatal("a short click counted")
	}
	// held in the middle: once
	n := 0
	for ms := 2000; ms < 3000; ms += 16 {
		if c.step(true, false, at(ms)) {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("held: %d times", n)
	}
	// the stick pushed part way through the hold: spoiled
	c.step(false, false, at(3100))
	c.step(true, false, at(3200))
	if c.step(true, true, at(3300)) || c.step(true, false, at(3700)) {
		t.Fatal("spoiled hold counted")
	}
}

// The left stick clicked while moving doesn't load a state (Turtles in Time
// jumped back to the intro); held in the middle it does.
func TestPadLoadNeedsHold(t *testing.T) {
	s, h, _ := testSession(t)
	p := PadState{Connected: true}
	p.Axes[0] = -1
	p.Buttons[padL3] = true
	s.padHotkeys(p)
	p.Axes[0] = 1
	s.padHotkeys(p)
	p.Axes[0] = 0
	s.padHotkeys(p)
	time.Sleep(400 * time.Millisecond)
	s.padHotkeys(p)
	h.mu.Lock()
	n := len(h.events)
	h.mu.Unlock()
	if n != 0 {
		t.Fatalf("events while moving: %v", h.events)
	}
	p.Buttons[padL3] = false
	s.padHotkeys(p)
	p.Buttons[padL3] = true
	s.padHotkeys(p)
	time.Sleep(400 * time.Millisecond)
	s.padHotkeys(p)
	s.padHotkeys(p)
	h.mu.Lock()
	defer h.mu.Unlock()
	if len(h.events) != 1 || h.events[0] != "pad-load" {
		t.Fatalf("held: %v", h.events)
	}
}
