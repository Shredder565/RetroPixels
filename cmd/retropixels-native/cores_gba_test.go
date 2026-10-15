package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/core"
)

func TestGameBoyAdvanceControls(t *testing.T) {
	m := loadMap("gba", nil)
	for name, key := range map[string]string{"A": "x", "B": "z", "L": "q", "R": "w", "Select": "shift", "Start": "enter", "Up": "arrowup",
		"Tilt Up": "i", "Tilt Down": "k", "Tilt Left": "j", "Tilt Right": "l", "Turn Left": "j", "Turn Right": "l",
		"More Sun": "i", "Less Sun": "k"} {
		if bd := m[name]; len(bd.Keys) != 1 || bd.Keys[0] != key {
			t.Errorf("%s: keys %v", name, bd.Keys)
		}
	}
	if m["A"].Pad[0] != padB || m["B"].Pad[0] != padA || m["L"].Pad[0] != padLB || m["R"].Pad[0] != padRB {
		t.Error("Xbox B should be A, Xbox A B, LB L and RB R")
	}
	if strings.Join(systemButtons["gba"], ",") != "A,B,Select,Start,Right,Left,Up,Down,R,L" {
		t.Errorf("buttons %v", systemButtons["gba"])
	}
}

func TestOpensGameBoyAdvanceGames(t *testing.T) {
	dir := t.TempDir()
	rom := make([]byte, 0x400)
	copy(rom, []byte{0xfe, 0xff, 0xff, 0xea}) // b . (an endless loop)
	for _, n := range []string{"a.gba", "b.AGB"} {
		p := filepath.Join(dir, n)
		os.WriteFile(p, rom, 0o644)
		g, err := openGame(p)
		if err != nil || g.System != "gba" {
			t.Fatalf("%s: %v %+v", n, err, g)
		}
		c, err := coreMakers["gba"](g.ROM, core.Options{SampleRate: 48000})
		if err != nil {
			t.Fatal(err)
		}
		c.RunFrame()
		if c.Width() != 240 || c.Height() != 160 || c.Info().System != "gba" {
			t.Fatalf("%s: %+v", n, c.Info())
		}
	}
	p := filepath.Join(dir, "c.zip")
	os.WriteFile(p, zipOf(t, map[string][]byte{"readme.txt": {1}, "Game (USA).gba": rom}), 0o644)
	if g, err := openGame(p); err != nil || g.System != "gba" || len(g.ROM) != len(rom) {
		t.Fatalf("zip: %v %+v", err, g)
	}
}
