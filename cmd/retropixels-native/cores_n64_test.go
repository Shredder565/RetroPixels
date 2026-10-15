package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"retropixels/core"
)

// a 1 MB .z64 whose program loops forever
func z64Test() []byte {
	rom := make([]byte, 0x101000)
	binary.BigEndian.PutUint32(rom[0:], 0x80371240)
	binary.BigEndian.PutUint32(rom[8:], 0x80000400)
	copy(rom[0x20:], "LOOP")
	binary.BigEndian.PutUint32(rom[0x1000:], 0x08000100) // j 0x80000400
	return rom
}

func swapped(b []byte, n int) []byte {
	out := append([]byte(nil), b...)
	for i := 0; i+n <= len(out); i += n {
		slices.Reverse(out[i : i+n])
	}
	return out
}

func TestN64Wiring(t *testing.T) {
	c, err := coreMakers["n64"](z64Test(), core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(c.Info().Buttons, systemButtons["n64"]) || c.Info().Players != 4 || !c.Info().Stereo {
		t.Fatalf("info %+v, shell has %v", c.Info(), systemButtons["n64"])
	}
	for name := range defaultMaps["n64"] {
		if !slices.Contains(systemButtons["n64"], name) {
			t.Errorf("default control for %q, which the core hasn't got", name)
		}
	}
	for i := 0; i < 5; i++ {
		c.RunFrame()
	}
	if c.Width()*c.Height() == 0 || len(c.Screen()) != c.Width()*c.Height() || len(c.TakeAudio()) == 0 || c.FPS() != 60 {
		t.Fatal("frame")
	}
	if a := pictureAspect(c.Info().Aspect, "n64", 640, 480); a != 4.0/3 {
		t.Errorf("aspect %v", a)
	}

	// host.js defaults: X / Z / A = A / B / Z, Enter = Start, Q / W = L / R,
	// T F G H = D-pad, I J K L = C buttons, arrows = the stick;
	// Xbox A / X = A / B, LT = Z, Menu = Start, LB / RB = L / R, D-pad = D-pad
	m := loadMap("n64", nil)
	buttons := systemButtons["n64"]
	in := newInputState()
	bit := func(names ...string) uint32 {
		var b uint32
		for _, n := range names {
			b |= 1 << slices.Index(buttons, n)
		}
		return b
	}
	for key, name := range map[string]string{"x": "A", "z": "B", "a": "Z", "enter": "Start", "q": "L", "w": "R",
		"t": "Up", "g": "Down", "f": "Left", "h": "Right", "i": "C-Up", "k": "C-Down", "j": "C-Left", "l": "C-Right",
		"arrowup": "Stick Up", "arrowdown": "Stick Down", "arrowleft": "Stick Left", "arrowright": "Stick Right"} {
		if b := in.bits(0, buttons, m, map[string]bool{key: true}, nil, true); b != bit(name) {
			t.Errorf("key %q: bits %b, want %s", key, b, name)
		}
	}
	pad := &PadState{Connected: true}
	for _, p := range []int{padA, padX, padLT, padMenu, padLB, padRB, padUp} {
		pad.Buttons[p] = true
	}
	pad.Axes[0], pad.Axes[1] = 1, 1 // the stick doesn't work the D-pad
	if b := in.bits(2, buttons, m, nil, pad, true); b != bit("A", "B", "Z", "Start", "L", "R", "Up") {
		t.Fatalf("pad: bits %b", b)
	}

	dir := t.TempDir()
	for name, data := range map[string][]byte{
		"Game.z64":   z64Test(),
		"Game.v64":   swapped(z64Test(), 2),
		"Game.n64":   swapped(z64Test(), 4),
		"Zipped.zip": zipOf(t, map[string][]byte{"Game (USA).z64": z64Test(), "readme.txt": {1}}),
	} {
		p := filepath.Join(dir, name)
		os.WriteFile(p, data, 0o644)
		g, err := openGame(p)
		if err != nil || g.System != "n64" {
			t.Errorf("%s: %+v %v", name, g, err)
			continue
		}
		if _, err := coreMakers["n64"](g.ROM, core.Options{SampleRate: 48000}); err != nil {
			t.Errorf("%s: %v", name, err)
		}
	}
	if !slices.Contains(availableExts(), ".v64") {
		t.Error(".v64 isn't listed")
	}
}

// The N64's session: the left stick is its stick, the right stick its C
// buttons, so recording is View + D-pad down (as in the web version).
func TestN64Session(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	h, card := &fakeHost{}, &fakeCard{}
	s, err := newSession(&loadedGame{System: "n64", ROM: z64Test(), Name: "Loop"}, h, card, loadMap("n64", nil))
	if err != nil {
		t.Fatal(err)
	}
	if !s.analogStick || !s.rightStick || !strings.Contains(s.note, "C buttons") || s.aspect != 4.0/3 {
		t.Fatalf("session %+v", s)
	}
	var p PadState
	p.Connected = true
	p.Axes[3] = 1
	s.padHotkeys(p)
	p.Buttons[padView] = true
	s.padHotkeys(p)
	p.Buttons[padDown] = true
	s.padHotkeys(p)
	s.padHotkeys(p) // held: once
	if ev := strings.Join(h.events, ","); ev != "pad-record" {
		t.Fatalf("events %q", ev)
	}
	if !strings.Contains(systemNotes["n64"], "View + D-pad down") {
		t.Error("the Controls window doesn't say how to record")
	}
}
