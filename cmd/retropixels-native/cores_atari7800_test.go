package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"

	"retropixels/core"
)

// a 32K .a78 whose program loops forever with the display off
func a78Test() []byte {
	h := make([]byte, 128)
	h[0] = 3
	copy(h[1:], "ATARI7800")
	rom := make([]byte, 0x8000)
	rom[0] = 0x4c // $8000: jmp $8000
	rom[2] = 0x80
	rom[0x7ffc], rom[0x7ffd] = 0x00, 0x80
	return append(h, rom...)
}

func TestAtari7800Wiring(t *testing.T) {
	c, err := coreMakers["atari7800"](a78Test(), core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(c.Info().Buttons, systemButtons["atari7800"]) {
		t.Fatalf("buttons %v, shell has %v", c.Info().Buttons, systemButtons["atari7800"])
	}
	c.RunFrame()
	if c.Width() != 320 || c.Height() != 240 || len(c.TakeAudio()) == 0 {
		t.Fatal("frame")
	}

	// host.js defaults: Z / X = buttons 1 / 2, Enter = Pause, Shift = Select,
	// R = Reset, 1 / 2 flip the difficulty switches; Xbox A / B, Menu, View, Y, LB / RB
	m := loadMap("atari7800", nil)
	buttons := systemButtons["atari7800"]
	in := newInputState()
	keys := map[string]bool{"z": true, "x": true, "enter": true, "shift": true, "r": true, "arrowup": true}
	if b := in.bits(0, buttons, m, keys, nil, false); b != 1|16|32|64|128|256 {
		t.Fatalf("keys: bits %b", b)
	}
	pad := &PadState{Connected: true}
	for _, p := range []int{padA, padB, padMenu, padView, padY, padLB, padRB, padRight} {
		pad.Buttons[p] = true
	}
	if b := in.bits(1, buttons, m, nil, pad, false); b != 8|16|32|64|128|256|512|1024 {
		t.Fatalf("pad: bits %b", b)
	}
	// the difficulty switches stay where they were put
	if b := in.bits(1, buttons, m, nil, &PadState{Connected: true}, false); b != 512|1024 {
		t.Fatalf("switches: bits %b", b)
	}

	dir := t.TempDir()
	for name, data := range map[string][]byte{
		"Game.a78":     a78Test(),
		"Game.bin":     a78Test(),
		"Zipped.zip":   zipOf(t, map[string][]byte{"Game (USA).a78": a78Test(), "readme.txt": {1}}),
		"ZippedBi.zip": zipOf(t, map[string][]byte{"game.bin": a78Test()}),
	} {
		p := filepath.Join(dir, name)
		os.WriteFile(p, data, 0o644)
		g, err := openGame(p)
		if err != nil || g.System != "atari7800" {
			t.Errorf("%s: %+v %v", name, g, err)
		}
	}
}
