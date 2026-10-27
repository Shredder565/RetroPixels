package model3

import (
	"math"
	"testing"
)

func TestFastLog2(t *testing.T) {
	worst := 0.0
	for i := 0; i < 2000000; i++ {
		x := float32(math.Exp2(float64(i)/20000 - 50))
		want := float32(math.Log2(float64(x)))
		got := fastLog2(x)
		if d := math.Abs(float64(got - want)); d > worst {
			worst = d
		}
	}
	if worst > 1e-5 {
		t.Errorf("fastLog2 is off by up to %g", worst)
	}
}
