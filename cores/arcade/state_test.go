package arcade

import (
	"math"
	"testing"
)

// A save state made mid-game carries on exactly as the game would have.
func TestStateRoundTrip(t *testing.T) {
	for _, ts := range allTestSets() {
		c, err := ts.open(nil)
		if err != nil {
			t.Fatal(ts.name, err)
		}
		ins := script(7, 700, len(c.buttons), 0)
		step := func(i int) (uint64, []float32) {
			c.SetInput(0, ins[i].p0)
			c.SetInput(1, ins[i].p1)
			c.RunFrame()
			return hashPixels(c.Screen()), c.TakeAudio()
		}
		for i := 0; i < 400; i++ {
			step(i)
		}
		st, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		var hs []uint64
		var auds [][]float32
		for i := 400; i < 700; i++ {
			h, a := step(i)
			hs, auds = append(hs, h), append(auds, a)
		}
		if err := c.LoadState(st); err != nil {
			t.Fatal(ts.name, err)
		}
		for i := 400; i < 700; i++ {
			h, a := step(i)
			if h != hs[i-400] || len(a) != len(auds[i-400]) {
				t.Fatalf("%s: frame %d differs after loading the state", ts.name, i)
			}
			for k := range a {
				if math.Float32bits(a[k]) != math.Float32bits(auds[i-400][k]) {
					t.Fatalf("%s: frame %d's sound differs after loading the state", ts.name, i)
				}
			}
		}
		if err := c.LoadState(st[:len(st)-3]); err == nil {
			t.Fatalf("%s: a cut state was taken", ts.name)
		}
	}
	a, _ := pacmanSet(0, 1, false, "puckman.zip").open(nil)
	b, _ := dkongSet(1, false, "dkong.zip").open(nil)
	st, _ := a.SaveState()
	if err := b.LoadState(st); err == nil || err.Error() != "that save state is for a different arcade game" {
		t.Fatal("a state for another game was taken:", err)
	}
}

func BenchmarkFrame(b *testing.B) {
	for _, ts := range allTestSets()[:12] {
		if len(ts.name) > 6 && ts.name[len(ts.name)-6:] == "-chaos" {
			continue
		}
		c, err := ts.open(nil)
		if err != nil {
			b.Fatal(err)
		}
		b.Run(ts.name, func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				c.SetInput(0, uint32(i/20))
				c.RunFrame()
				c.TakeAudio()
			}
		})
	}
}
