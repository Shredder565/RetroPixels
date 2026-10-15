package gb

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"retropixels/core"
)

// frameOut is what one frame produced.
type frameOut struct {
	hash   uint32
	audio  []float32
	rumble float32
}

func runScript(g *GB, ins []frameIn, wall *float64) []frameOut {
	out := make([]frameOut, len(ins))
	for f, in := range ins {
		*wall = in.wall
		if in.reset {
			g.Reset()
		}
		g.SetInput(0, in.bits)
		g.SetAnalog(0, 2, float32(in.sx))
		g.SetAnalog(0, 3, float32(in.sy))
		g.RunFrame()
		out[f] = frameOut{screenHash(g.Screen()), g.TakeAudio(), g.Rumble()}
	}
	return out
}

func sameRun(t *testing.T, what string, a, b []frameOut) {
	t.Helper()
	for f := range a {
		if a[f].hash != b[f].hash {
			t.Fatalf("%s: frame %d picture differs", what, f)
		}
		if a[f].rumble != b[f].rumble {
			t.Fatalf("%s: frame %d rumble differs", what, f)
		}
		if len(a[f].audio) != len(b[f].audio) {
			t.Fatalf("%s: frame %d: %d audio samples, %d before", what, f, len(b[f].audio), len(a[f].audio))
		}
		for i := range a[f].audio {
			if a[f].audio[i] != b[f].audio[i] {
				t.Fatalf("%s: frame %d sample %d differs", what, f, i)
			}
		}
	}
}

type testROM struct {
	name string
	data []byte
}

// stateROMs: the cartridge test ROMs, plus any in GB_COMPARE_LIST.
func stateROMs(t *testing.T) []testROM {
	var out []testROM
	for i, s := range fuzzSpecs() {
		if s.typ == 0x2a {
			continue // (no such chip)
		}
		s.seed = 0x4321 + i*0x0101
		out = append(out, testROM{s.name, buildFuzzROM(s)})
	}
	if list := os.Getenv("GB_COMPARE_LIST"); list != "" {
		for _, e := range readList(t, list) {
			b, err := os.ReadFile(e.path)
			if err != nil {
				t.Fatal(err)
			}
			out = append(out, testROM{filepath.Base(e.path), b})
		}
	}
	return out
}

// TestStateRoundTrip: a state saved mid-game and loaded (into a new copy of
// the game, and into the same one later on) continues exactly as the game did.
func TestStateRoundTrip(t *testing.T) {
	before, after := 700, 300
	if testing.Short() {
		before, after = 200, 100
	}
	for _, r := range stateROMs(t) {
		r := r
		t.Run(r.name, func(t *testing.T) {
			t.Parallel()
			wall := wallStart
			clock := func() float64 { return wall }
			ins := makeScript(seedOf(r.name), before+after, wallStart)
			for i := range ins {
				ins[i].reset = false
			}
			g, err := newGB(r.data, core.Options{SampleRate: 48000}, clock)
			if err != nil {
				t.Fatal(err)
			}
			runScript(g, ins[:before], &wall)
			st, err := g.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			ref := runScript(g, ins[before:], &wall)
			refSram := g.SRAM()

			// a new copy of the game
			wall = ins[before-1].wall
			g2, _ := newGB(r.data, core.Options{SampleRate: 48000}, clock)
			if err := g2.LoadState(st); err != nil {
				t.Fatal(err)
			}
			sameRun(t, "new copy", ref, runScript(g2, ins[before:], &wall))
			if !bytes.Equal(refSram, g2.SRAM()) {
				t.Fatal("new copy: battery save differs")
			}
			// the same game, going back
			wall = ins[before-1].wall
			if err := g.LoadState(st); err != nil {
				t.Fatal(err)
			}
			sameRun(t, "same game", ref, runScript(g, ins[before:], &wall))

			// a damaged state is refused and changes nothing
			now, _ := g.SaveState()
			if g.LoadState(st[:len(st)-7]) == nil {
				t.Fatal("a cut-short state loaded")
			}
			if again, _ := g.SaveState(); !bytes.Equal(now, again) {
				t.Fatal("a refused state changed the game")
			}
		})
	}
}

func TestStateFromAnotherGame(t *testing.T) {
	roms := stateROMs(t)
	a, _ := newGB(roms[2].data, core.Options{}, wallClock)
	b, _ := newGB(roms[3].data, core.Options{}, wallClock)
	st, _ := a.SaveState()
	if b.LoadState(st) == nil {
		t.Fatal("loaded another game's state")
	}
	if b.LoadState([]byte("RPNES\x01xxxxxxxx")) == nil {
		t.Fatal("loaded an NES state")
	}
}

// TestBatterySaveLayouts: the battery save holds exactly what the web
// version's does (RAM, then the chip's own block), and loads back.
func TestBatterySaveLayouts(t *testing.T) {
	want := map[string]int{"FUZZ ROM": -1, "FUZZ ROMRAMBAT": 0x2000, "FUZZ MBC1": 0x8000, "FUZZ MBC2": 512,
		"FUZZ MBC3RTC": 0x8000 + 48, "FUZZ MBC3TIMER": 48, "FUZZ MBC3": 0x8000, "FUZZ MBC30": 0x10000 + 48,
		"FUZZ MBC5": 0x20000, "FUZZ MBC5RUMBLE": 0x8000, "FUZZ MBC5 8MB": -1, "FUZZ MBC6": 0x8000 + 0x100000,
		"FUZZ MBC7": 256, "FUZZ HUC1": 0x8000, "FUZZ HUC3": 0x8000 + 272, "FUZZ MMM01": 0x8000, "FUZZ TAMA5": 48,
		"FUZZ CAMERA": 0x20000}
	for _, r := range stateROMs(t) {
		n, ok := want[r.name]
		if !ok {
			continue
		}
		wall := wallStart
		g, err := newGB(r.data, core.Options{SampleRate: 48000}, func() float64 { return wall })
		if err != nil {
			t.Fatal(err)
		}
		runScript(g, makeScript(1, 300, wall), &wall)
		s := g.SRAM()
		if n < 0 {
			if s != nil {
				t.Errorf("%s: has a battery save", r.name)
			}
			continue
		}
		if len(s) != n {
			t.Errorf("%s: battery save is %d bytes, want %d", r.name, len(s), n)
			continue
		}
		g2, _ := newGB(r.data, core.Options{SampleRate: 48000, SRAM: s}, func() float64 { return wall })
		if !bytes.Equal(g2.SRAM(), s) {
			t.Errorf("%s: battery save doesn't load back", r.name)
		}
	}
}

func TestRefusesNonGames(t *testing.T) {
	if _, err := New(make([]byte, 0x14f), core.Options{}); err == nil || err.Error() != "This isn't a Game Boy game file" {
		t.Fatalf("got %v", err)
	}
}

func TestInfo(t *testing.T) {
	roms := stateROMs(t)
	for _, r := range roms {
		g, _ := newGB(r.data, core.Options{}, wallClock)
		i := g.Info()
		tilt := r.name == "FUZZ MBC7"
		if (len(i.Buttons) == 12) != tilt || g.UsesRightStick() != tilt || (g.ControlsNote() != "") != tilt {
			t.Errorf("%s: buttons %v", r.name, i.Buttons)
		}
		if i.System != "gb" || !i.Stereo || i.Players != 1 || i.Aspect != 10.0/9 || g.Width() != 160 || g.Height() != 144 {
			t.Errorf("%s: info %+v", r.name, i)
		}
	}
}

// TestColorTableMatchesJS: all 32768 Game Boy Color colours, as the web core makes them.
func TestColorTableMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	out, err := exec.Command(node, "testdata/tables.js", "../../web/emu").Output()
	if err != nil {
		t.Fatal(err)
	}
	var js []uint32
	if err := json.Unmarshal(out, &js); err != nil {
		t.Fatal(err)
	}
	if len(js) != len(cgbRGB) {
		t.Fatalf("%d colours, want %d", len(js), len(cgbRGB))
	}
	for i := range js {
		if js[i] != cgbRGB[i] {
			t.Fatalf("colour %04x: js %06x go %06x", i, js[i], cgbRGB[i])
		}
	}
}

// BenchmarkFrame: one frame of a busy game (the MBC5 test ROM in Color mode:
// the CPU never rests, the sound runs, the picture is drawn).
func BenchmarkFrame(b *testing.B) {
	var rom []byte
	for _, s := range fuzzSpecs() {
		if s.name == "FUZZ MBC5" {
			s.seed = 99
			rom = buildFuzzROM(s)
		}
	}
	g, err := newGB(rom, core.Options{SampleRate: 48000}, wallClock)
	if err != nil {
		b.Fatal(err)
	}
	for i := 0; i < 60; i++ {
		g.RunFrame()
		g.TakeAudio()
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		g.RunFrame()
		g.TakeAudio()
	}
}
