package wiiu

import (
	"math"
	"testing"
)

func TestMixLevels(t *testing.T) {
	near := func(a, b float64) bool { return math.Abs(a-b) < 0.01 }
	if g := mixGain(0); !near(g, 1) {
		t.Errorf("0 dB: %v", g)
	}
	if g := mixGain(-60); !near(g, 0.501) {
		t.Errorf("-6 dB: %v", g)
	}
	if g := mixGain(mixMute); g != 0 {
		t.Errorf("mute: %v", g)
	}
	if l, r := mixPan(0); !near(l, 1) || !near(r, 0) {
		t.Errorf("left: %v %v", l, r)
	}
	if l, r := mixPan(127); !near(l, 0) || !near(r, 1) {
		t.Errorf("right: %v %v", l, r)
	}
	if l, r := mixPan(64); !near(l*l+r*r, 1) || math.Abs(l-r) > 0.02 {
		t.Errorf("middle: %v %v", l, r)
	}
}
