package model3

import "testing"

// A big polygon that passes close by the eye, like the planet in Star Wars
// Trilogy's Death Star battle, must stay within a sane distance of the screen
// once clipped, or it smears across the starfield.
func TestClipKeepsBigPolygonsNearTheScreen(t *testing.T) {
	r := &renderer{}
	vp := &viewport{px: 1, py: 1, glW: 496, glH: 384, rx1: 496, ry1: 384}
	in := [3]cvert{
		{x: 2000, y: 900, z: -50000},
		{x: -40000, y: -60000, z: 60000},
		{x: 30000, y: 50000, z: -30000},
	}
	var list []tri
	r.clipAndAdd(vp, &tri{}, in, &list)
	if len(list) == 0 {
		t.Fatal("the polygon crosses the view, so some of it should be drawn")
	}
	for _, tr := range list {
		for k := 0; k < 3; k++ {
			if tr.x[k] < -10000 || tr.x[k] > 10000 || tr.y[k] < -10000 || tr.y[k] > 10000 {
				t.Fatalf("vertex at %v, %v is far off the screen", tr.x[k], tr.y[k])
			}
		}
	}
}
