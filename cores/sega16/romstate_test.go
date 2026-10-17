package sega16

import (
	"hash/fnv"
	"math"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// These tests need the user's own ROM set; they skip without it.

func loadSetDips(t *testing.T, dips map[string]int) *board {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(romDir, "altbeast.zip"))
	if err != nil {
		t.Skip("ROM set not present:", err)
	}
	c, err := arcade.New(data, core.Options{Name: "altbeast.zip", SampleRate: 48000, Dips: dips})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core).Board().(*board)
}

// play runs frames from..to with a coin at 120 and a start at 200, and
// returns a hash of the pictures and sound.
func play(b *board, pads []arcade.Pad, from, to int) uint64 {
	h := fnv.New64a()
	for f := from; f <= to; f++ {
		pads[0]["Coin"] = f >= 120 && f < 126
		pads[0]["Start (1P)"] = f >= 200 && f < 206
		pads[0]["Right"] = f > 300 && f%50 < 30
		pads[0]["Button 1"] = f > 300 && f%20 < 3
		b.RunFrame()
		for _, p := range b.Screen() {
			h.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16)})
		}
		for _, s := range b.TakeAudio() {
			u := math.Float32bits(s)
			h.Write([]byte{byte(u), byte(u >> 8), byte(u >> 16), byte(u >> 24)})
		}
	}
	return h.Sum64()
}

// TestAltbeastLivesDip: the lives switch reaches the game (lives at 0xfff019).
func TestAltbeastLivesDip(t *testing.T) {
	for _, c := range []struct{ dip, lives int }{{3, 3}, {2, 2}, {1, 4}} {
		b := loadSetDips(t, map[string]int{"lives": c.dip})
		pads := []arcade.Pad{{}, {}}
		b.SetInput(pads)
		play(b, pads, 1, 260)
		if got := b.ram[0x3019]; int(got) != c.lives {
			t.Errorf("lives switch %d: the game has %d lives, want %d", c.dip, got, c.lives)
		}
	}
}

// TestAltbeastStateAndReset: a state saved in play loads back to the same
// pictures and sound, and Reset starts the game over like a fresh board.
func TestAltbeastStateAndReset(t *testing.T) {
	b := loadSetDips(t, nil)
	pads := []arcade.Pad{{}, {}}
	b.SetInput(pads)
	first := play(b, pads, 1, 400)
	st, err := b.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	want := play(b, pads, 401, 520)
	if err := b.LoadState(st); err != nil {
		t.Fatal(err)
	}
	if got := play(b, pads, 401, 520); got != want {
		t.Error("frames after loading a state differ")
	}
	b.Reset()
	if got := play(b, pads, 1, 400); got != first {
		t.Error("frames after Reset differ from a cold start")
	}
}
