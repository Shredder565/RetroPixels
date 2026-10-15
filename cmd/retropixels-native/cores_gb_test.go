package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestGameBoyControls(t *testing.T) {
	m := loadMap("gb", nil)
	for name, key := range map[string]string{"A": "x", "B": "z", "Select": "shift", "Start": "enter", "Up": "arrowup",
		"Tilt Up": "i", "Tilt Down": "k", "Tilt Left": "j", "Tilt Right": "l"} {
		if bd := m[name]; len(bd.Keys) != 1 || bd.Keys[0] != key {
			t.Errorf("%s: keys %v", name, bd.Keys)
		}
	}
	if m["A"].Pad[0] != padB || m["B"].Pad[0] != padA {
		t.Error("Xbox B should be A and Xbox A should be B, as on the original pad")
	}
	if strings.Join(systemButtons["gb"], ",") != "A,B,Select,Start,Up,Down,Left,Right" {
		t.Errorf("buttons %v", systemButtons["gb"])
	}
}

func TestOpensGameBoyGames(t *testing.T) {
	dir := t.TempDir()
	rom := make([]byte, 0x8000)
	for _, n := range []string{"a.gb", "b.GBC"} {
		p := filepath.Join(dir, n)
		os.WriteFile(p, rom, 0o644)
		g, err := openGame(p)
		if err != nil || g.System != "gb" {
			t.Fatalf("%s: %v %+v", n, err, g)
		}
	}
}

// A tilt game reads the right stick itself: holding it down doesn't start a recording.
func TestRightStickGameKeepsItsStick(t *testing.T) {
	s, h, _ := testSession(t)
	s.rightStick = true
	s.start()
	defer s.stop()
	h.mu.Lock()
	h.padSt[0].Connected = true
	h.padSt[0].Axes[3] = 1
	h.mu.Unlock()
	time.Sleep(1300 * time.Millisecond)
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, e := range h.events {
		if e == "pad-record" {
			t.Fatal("started recording")
		}
	}
}
