package gba

import (
	"bytes"
	"fmt"
	"os"
	"testing"
	"time"

	"retropixels/core"
)

// frameOut is what one frame produced.
type frameOut struct {
	hash   uint32
	audio  []float32
	rumble float32
}

func runScript(g *GBA, ins []frameIn, wall *float64) []frameOut {
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

type namedROM struct {
	name string
	data []byte
}

// stateROMs: the always-available mini ROM plus the built test ROMs.
func stateROMs(t *testing.T) []namedROM {
	out := []namedROM{{"mini", miniROM()}}
	if haveTestROMs() {
		for _, s := range romSpecs {
			out = append(out, namedROM{s.name, testROM(t, s.name)})
		}
	}
	return out
}

func clockAt(wall *float64) func() time.Time {
	return func() time.Time { return time.UnixMilli(int64(*wall)).UTC() }
}

// TestStateRoundTrip: a state saved part way through a run, loaded into a new
// machine and back into the same one, carries on exactly as the run did; a
// damaged state or one from another game is refused and changes nothing.
func TestStateRoundTrip(t *testing.T) {
	other := miniROM()
	other[0xa0] = 'X'
	for _, r := range stateROMs(t) {
		t.Run(r.name, func(t *testing.T) {
			t.Parallel()
			wall := wallStart
			opts := core.Options{SampleRate: 48000}
			g, err := newGBA(r.data, opts, clockAt(&wall))
			if err != nil {
				t.Fatal(err)
			}
			ins := makeScript(seedOf(r.name), 240, wallStart)
			for i := range ins {
				ins[i].reset = false
			}
			runScript(g, ins[:120], &wall)
			st, err := g.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			wallAt := wall
			want := runScript(g, ins[120:], &wall)

			// into a new machine
			wall = wallAt
			g2, _ := newGBA(r.data, opts, clockAt(&wall))
			if err := g2.LoadState(st); err != nil {
				t.Fatal(err)
			}
			sameRun(t, "new machine", want, runScript(g2, ins[120:], &wall))

			// back into the same one
			wall = wallAt
			if err := g.LoadState(st); err != nil {
				t.Fatal(err)
			}
			st2, _ := g.SaveState()
			if !bytes.Equal(st, st2) {
				t.Fatal("saving straight after loading gives a different state")
			}
			sameRun(t, "same machine", want, runScript(g, ins[120:], &wall))

			// a damaged state is refused and changes nothing
			wall = wallAt
			g.LoadState(st)
			for _, bad := range [][]byte{st[:len(st)-1], st[:len(st)/2], append(append([]byte{}, st...), 0), []byte("RPGBA"), nil} {
				if err := g.LoadState(bad); err == nil {
					t.Fatalf("a damaged state (%d bytes) was accepted", len(bad))
				}
			}
			if st3, _ := g.SaveState(); !bytes.Equal(st, st3) {
				t.Fatal("a refused state changed the machine")
			}
			// one from another game too
			o, _ := newGBA(other, opts, clockAt(&wall))
			ost, _ := o.SaveState()
			if err := g.LoadState(ost); err == nil {
				t.Fatal("a state from another game was accepted")
			}
			sameRun(t, "after refused states", want, runScript(g, ins[120:], &wall))
		})
	}
}

// TestDamagedStateNoCrash: states with bytes changed load or are refused,
// and the machine runs either way.
func TestDamagedStateNoCrash(t *testing.T) {
	g, _ := newGBA(miniROM(), core.Options{SampleRate: 48000}, time.Now)
	for i := 0; i < 60; i++ {
		g.RunFrame()
	}
	st, _ := g.SaveState()
	for i := 0; i < 300; i++ {
		b := append([]byte(nil), st...)
		for k := 0; k < 8; k++ {
			b[10+(i*7919+k*104729)%(len(b)-10)] ^= byte(1 + i + k)
		}
		g.LoadState(b)
		g.RunFrame()
	}
}

func TestRefusesBadFiles(t *testing.T) {
	if _, err := New(nil, core.Options{SampleRate: 48000}); err == nil {
		t.Fatal("an empty file was accepted")
	}
	if _, err := New(make([]byte, 64<<20+1), core.Options{SampleRate: 48000}); err == nil {
		t.Fatal("a file over 64 MB was accepted")
	}
}

// TestBatterySaves: each save type gives a battery save of its size that
// loads back.
func TestBatterySaves(t *testing.T) {
	sizes := map[string]int{"save_sram": 0x8000, "save_sramf": 0x8000, "save_flash64": 0x10000, "save_flash512": 0x10000,
		"save_flash128": 0x20000, "save_ee512": 512, "save_ee8k": 8192}
	for name, size := range sizes {
		rom := testROM(t, name)
		g, _ := newGBA(rom, core.Options{SampleRate: 48000}, time.Now)
		ins := makeScript(seedOf(name), 900, wallStart)
		wall := wallStart
		runScript(g, ins, &wall)
		sram := g.SRAM()
		if len(sram) != size {
			t.Fatalf("%s: battery save is %d bytes, want %d", name, len(sram), size)
		}
		if bytes.Count(sram, []byte{0xff}) == len(sram) {
			t.Fatalf("%s: nothing was saved", name)
		}
		g2, _ := newGBA(rom, core.Options{SampleRate: 48000, SRAM: sram}, time.Now)
		if !bytes.Equal(g2.SRAM(), sram) {
			t.Fatalf("%s: the battery save doesn't load back", name)
		}
	}
	// a game with no save memory has no battery save until it writes one
	g, _ := newGBA(testROM(t, "gpio_plain"), core.Options{SampleRate: 48000}, time.Now)
	g.RunFrame()
	if g.SRAM() != nil {
		t.Fatal("a game with no save memory has a battery save")
	}
}

// TestCartInfo: the buttons, stick, note and rumble of each kind of cart.
func TestCartInfo(t *testing.T) {
	for _, c := range []struct {
		code, extra string
		stick, rum  bool
	}{
		{"RPMN", "", false, false},
		{"U3IE", "[More Sun Less Sun]", true, false},
		{"RZWE", "[Turn Left Turn Right]", true, true},
		{"V49E", "", false, true},
		{"KYGE", "[Tilt Up Tilt Down Tilt Left Tilt Right]", true, false},
		{"KHPJ", "[Tilt Up Tilt Down Tilt Left Tilt Right]", true, false},
	} {
		rom := miniROM()
		copy(rom[0xac:], c.code)
		g, err := newGBA(rom, core.Options{SampleRate: 48000}, time.Now)
		if err != nil {
			t.Fatal(err)
		}
		info := g.Info()
		extra := ""
		if len(info.Buttons) > len(Buttons) {
			extra = fmt.Sprint(info.Buttons[len(Buttons):])
		}
		if fmt.Sprint(info.Buttons[:len(Buttons)]) != fmt.Sprint(Buttons) || extra != c.extra {
			t.Fatalf("%s: buttons %v", c.code, info.Buttons)
		}
		if g.UsesRightStick() != c.stick || (g.ControlsNote() != "") != c.stick || g.HasRumble() != c.rum {
			t.Fatalf("%s: stick %v note %q rumble %v", c.code, g.UsesRightStick(), g.ControlsNote(), g.HasRumble())
		}
		if info.System != "gba" || info.Players != 1 || !info.Stereo || g.Width() != 240 || g.Height() != 160 {
			t.Fatalf("%s: info %+v", c.code, info)
		}
	}
}

// TestMiniROMRuns: the mini ROM draws, plays and saves (a check on the
// program the always-run tests use).
func TestMiniROMRuns(t *testing.T) {
	g, _ := newGBA(miniROM(), core.Options{SampleRate: 48000}, time.Now)
	var samples, loud int
	for i := 0; i < 120; i++ {
		g.SetInput(0, uint32(i/10)&0x3ff)
		g.RunFrame()
		for _, s := range g.TakeAudio() {
			samples++
			if s > 0.01 || s < -0.01 {
				loud++
			}
		}
	}
	lit := 0
	for _, c := range g.Screen() {
		if c != 0 {
			lit++
		}
	}
	if lit < 240*100 || loud == 0 || samples < 120*1500 || g.SRAM()[3] != 96 {
		t.Fatalf("mini ROM: %d pixels lit, %d of %d samples loud, save %v", lit, loud, samples, g.SRAM()[:4])
	}
}

// BenchmarkFrame: one frame of emulation (a 60 fps game needs well under 16 ms).
// GBA_BENCH_ROM picks a ROM file; otherwise the demo test ROM is used if it
// can be built, else the mini ROM.
func BenchmarkFrame(b *testing.B) {
	rom := miniROM()
	if p := os.Getenv("GBA_BENCH_ROM"); p != "" {
		var err error
		if rom, err = os.ReadFile(p); err != nil {
			b.Fatal(err)
		}
	} else if haveTestROMs() {
		rom = testROM(b, "demo")
	}
	g, err := newGBA(rom, core.Options{SampleRate: 48000}, time.Now)
	if err != nil {
		b.Fatal(err)
	}
	ins := makeScript(1, 600, wallStart)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		in := ins[i%len(ins)]
		g.SetInput(0, in.bits)
		g.RunFrame()
		g.TakeAudio()
	}
	b.ReportMetric(float64(b.Elapsed().Microseconds())/1000/float64(b.N), "ms/frame")
}
