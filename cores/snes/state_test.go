package snes

import (
	"bytes"
	"hash/fnv"
	"math"
	"testing"

	"retropixels/core"
)

// run plays frames of a script and fingerprints what comes out: every
// picture, every sample and the battery save at the end.
func runFingerprint(g *SNES, ins []frameIn, wall *float64) []uint64 {
	var out []uint64
	for _, in := range ins {
		*wall = in.wall
		if in.reset {
			g.Reset()
		}
		g.SetInput(0, in.bits[0])
		g.SetInput(1, in.bits[1])
		g.RunFrame()
		h := fnv.New64a()
		for _, p := range g.Screen() {
			h.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16)})
		}
		for _, a := range g.TakeAudio() {
			b := math.Float32bits(a)
			h.Write([]byte{byte(b), byte(b >> 8), byte(b >> 16), byte(b >> 24)})
		}
		out = append(out, h.Sum64())
	}
	h := fnv.New64a()
	h.Write(g.SRAM())
	return append(out, h.Sum64())
}

// TestStateRoundTrip: for the generated test ROMs (the plain console and
// every chip), a game restored from a save state, in a new emulator or the
// same one, plays on exactly like the original: the same pictures, sound
// and battery save.
func TestStateRoundTrip(t *testing.T) {
	frames := 150
	if testing.Short() {
		frames = 60
	}
	for i, v := range fuzzVariants {
		v.seed = int64(77000 + i)
		rom, f := buildFuzz(v)
		if f.maxFrames != 0 {
			continue // (ROMs both cores refuse)
		}
		t.Run(v.name, func(t *testing.T) {
			t.Parallel()
			opts := core.Options{SampleRate: 48000, Firmware: f.firmware}
			wall := 1.7e12
			now := func() float64 { return wall }
			g, err := newSNES(rom, opts, now)
			if err != nil {
				t.Fatal(err)
			}
			ins := makeScript(uint64(v.seed), 2*frames, wall)
			for k := range ins {
				ins[k].reset = false
			}
			runFingerprint(g, ins[:frames], &wall)
			st, err := g.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			wallAt := wall
			want := runFingerprint(g, ins[frames:], &wall)
			// a new emulator
			wall = wallAt
			g2, err := newSNES(rom, opts, now)
			if err != nil {
				t.Fatal(err)
			}
			if err := g2.LoadState(st); err != nil {
				t.Fatal(err)
			}
			if got := runFingerprint(g2, ins[frames:], &wall); !equalU64(got, want) {
				t.Fatalf("new emulator: differs from frame %d", firstDiff(got, want))
			}
			// the same one, back in time
			wall = wallAt
			if err := g.LoadState(st); err != nil {
				t.Fatal(err)
			}
			if got := runFingerprint(g, ins[frames:], &wall); !equalU64(got, want) {
				t.Fatalf("same emulator: differs from frame %d", firstDiff(got, want))
			}
			// a state saved again straight after loading is the same
			if err := g2.LoadState(st); err != nil {
				t.Fatal(err)
			}
			if st2, _ := g2.SaveState(); !bytes.Equal(st, st2) {
				t.Fatal("state saved after loading differs")
			}
			// a broken state is refused and leaves the game alone
			if err := g2.LoadState(st[:len(st)/2]); err == nil {
				t.Fatal("cut state accepted")
			}
			if st3, _ := g2.SaveState(); !bytes.Equal(st, st3) {
				t.Fatal("refused state changed the game")
			}
		})
	}
}

func equalU64(a, b []uint64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func firstDiff(a, b []uint64) int {
	for i := range min(len(a), len(b)) {
		if a[i] != b[i] {
			return i
		}
	}
	return min(len(a), len(b))
}
