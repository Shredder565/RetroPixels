package boards

import (
	"testing"

	"retropixels/cores/arcade"
)

// X (Button 3) attacks too in TMNT, Turtles in Time and The Simpsons, as A
// (Button 1) does.
func TestButton3Attacks(t *testing.T) {
	pads := []arcade.Pad{{"Button 3": true}, {"Button 1": true}}
	t1 := &tmnt{inputs: pads}
	t2 := &tmnt2{inputs: pads}
	sp := &simpsons{inputs: pads}
	for _, c := range []struct {
		name string
		f    func(int) int
	}{{"tmnt", t1.playerByte}, {"tmnt2", t2.playerByte}, {"simpsons", sp.playerByte}} {
		x, a := c.f(0), c.f(1)
		if x != a || x&0x10 != 0 || x|0x10 != 0xff {
			t.Errorf("%s: X %#x, A %#x", c.name, x, a)
		}
	}
}
