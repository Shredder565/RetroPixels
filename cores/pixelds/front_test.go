package pixelds

import (
	"os"
	"testing"
)

// The front's save states, on both sides: a state saved and loaded plays
// on with the same picture as the game it was saved from.
func TestFrontStates(t *testing.T) {
	for _, g := range []string{"nds/testdata/selftest.nds", "ctr/testdata/ctrtest.cxi"} {
		data, err := os.ReadFile(g)
		if err != nil {
			t.Fatal(err)
		}
		dir := t.TempDir()
		o := Options{SDCard: dir, SaveDir: dir}
		e1, err := Open(g, data, o)
		if err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 10; i++ {
			e1.RunFrame()
		}
		st, err := e1.SaveState()
		if err != nil {
			t.Fatalf("%s: %v", g, err)
		}
		e2, err := Open(g, data, o)
		if err != nil {
			t.Fatal(err)
		}
		e2.RunFrame() // a state loads into a game that is already playing
		if err := e2.LoadState(st); err != nil {
			t.Fatalf("%s: %v", g, err)
		}
		for i := 0; i < 10; i++ {
			e1.RunFrame()
			e2.RunFrame()
		}
		a, b := e1.Screen(), e2.Screen()
		for i := range a {
			if a[i] != b[i] {
				t.Fatalf("%s: the picture differs after loading", g)
			}
		}
	}
}
