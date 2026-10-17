package arcade

// Tests with the user's own Donkey Kong, Donkey Kong Jr. and Galaxian sets;
// they skip when the sets aren't there.

import (
	"math"
	"os"
	"testing"

	"retropixels/core"
)

const batch2Dir = "/mnt/project-files/roms-private/arcade-batch2/"

// TestRealBatch2 boots each set, puts a coin in, starts a game and plays it a
// little: the picture and sound must be alive, a save state must play on
// exactly as the game does, and Reset must start the game over the same way.
func TestRealBatch2(t *testing.T) {
	for _, set := range []string{"dkong", "dkongjr", "galaxian"} {
		t.Run(set, func(t *testing.T) {
			data, err := os.ReadFile(batch2Dir + set + ".zip")
			if err != nil {
				t.Skip("no ROM set " + set)
			}
			cc, err := New(data, core.Options{Name: set + ".zip", SampleRate: 48000})
			if err != nil {
				t.Fatal(err)
			}
			c := cc.(*Core)
			idx := map[string]int{}
			for i, b := range c.Info().Buttons {
				idx[b] = i
			}
			s := uint32(7)
			input := func(f int) uint32 {
				var bits uint32
				if m := f % 900; m >= 120 && m < 126 {
					bits |= 1 << idx["Coin"]
				}
				if m := f % 900; m >= 200 && m < 206 {
					bits |= 1 << idx["Start (1P)"]
				}
				if f > 260 {
					s = s*1103515245 + 12345
					for i, b := range []string{"Up", "Down", "Left", "Right", "Jump", "Fire"} {
						if j, ok := idx[b]; ok && (s>>(16+i))&3 == 0 {
							bits |= 1 << j
						}
					}
				}
				return bits
			}
			run := func(from, n int) ([]uint32, []float32) {
				var audio []float32
				for f := from; f < from+n; f++ {
					c.SetInput(0, input(f))
					c.RunFrame()
					audio = append(audio, c.TakeAudio()...)
				}
				return append([]uint32(nil), c.Screen()...), audio
			}
			pic, audio := run(1, 1200)
			rms := 0.0
			for _, v := range audio {
				rms += float64(v) * float64(v)
			}
			if rms = math.Sqrt(rms / float64(len(audio))); rms < 0.005 {
				t.Errorf("sound rms %g", rms)
			}
			colours := map[uint32]bool{}
			for _, px := range pic {
				colours[px] = true
			}
			if len(colours) < 5 {
				t.Errorf("%d colours on screen", len(colours))
			}
			st, err := c.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			saved := s
			a, aa := run(1201, 120)
			s = saved
			if err := c.LoadState(st); err != nil {
				t.Fatal(err)
			}
			if b, ba := run(1201, 120); !slicesEq(a, b) || len(aa) != len(ba) {
				t.Error("a loaded state plays differently")
			}
			c.Reset()
			r1, _ := run(1, 400)
			c.Reset()
			r2, _ := run(1, 400)
			if !slicesEq(r1, r2) {
				t.Error("Reset doesn't start the same way twice")
			}
		})
	}
}

func slicesEq(a, b []uint32) bool {
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
