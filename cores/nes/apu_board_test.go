package nes

import "testing"

// TestStandaloneAPU: the APU on its own makes a square wave at the asked
// pitch and level, and a saved state carries on exactly.
func TestStandaloneAPU(t *testing.T) {
	for _, raw := range []bool{false, true} {
		x := NewAPU(48000)
		x.SetRaw(raw)
		x.Write(0x4015, 0x0f)
		x.Write(0x4000, 0xbf) // 50% duty, constant volume 15
		x.Write(0x4002, 0xfd) // period 253: 440 Hz
		x.Write(0x4003, 0x00)
		run := func(cycles int) []float32 { // taken a frame at a time, as a board does
			var out []float32
			for i := 0; i < cycles; i++ {
				x.Clock()
				if i%29830 == 0 {
					out = append(out, x.TakeAudio()...)
				}
			}
			return append(out, x.TakeAudio()...)
		}
		a := run(178977) // 0.1 s
		if len(a) < 4790 || len(a) > 4810 {
			t.Fatalf("raw %v: %d samples in 0.1 s", raw, len(a))
		}
		// count rising crossings of the middle
		lo, hi := a[2000], a[2000]
		for _, v := range a[2000:] {
			lo, hi = min(lo, v), max(hi, v)
		}
		mid, n := (lo+hi)/2, 0
		for i := 2001; i < len(a); i++ {
			if a[i-1] < mid && a[i] >= mid {
				n++
			}
		}
		if f := float64(n) / (float64(len(a)-2001) / 48000); f < 420 || f > 460 {
			t.Errorf("raw %v: %.0f Hz", raw, f)
		}
		if raw && (hi-lo < 0.14 || hi-lo > 0.16) {
			t.Errorf("raw output swing %g, want the mixer's 0.149", hi-lo)
		}
		st := x.SaveState()
		b1 := run(20000)
		if err := x.LoadState(st); err != nil {
			t.Fatal(err)
		}
		b2 := run(20000)
		if len(b1) != len(b2) {
			t.Fatal("state: different lengths")
		}
		for i := range b1 {
			if b1[i] != b2[i] {
				t.Fatalf("raw %v: state plays differently at %d", raw, i)
			}
		}
		if x.LoadState(st[:len(st)-1]) == nil {
			t.Error("a cut-off state loaded")
		}
	}
}
