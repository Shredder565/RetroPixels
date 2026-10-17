package wiiu

import (
	"fmt"
	"os"
	"strings"
	"testing"
)

// TestGX2 runs gx2test.c and checks what it drew.
func TestGX2(t *testing.T) {
	m, log := runRPX(t, buildRPX(t, false, "gx2test.c"), 30)
	if !strings.Contains(log, "gx2 test done") {
		t.Fatalf("log:\n%s", log)
	}
	if m.gpu.ShaderErr != "" {
		t.Fatal(m.gpu.ShaderErr)
	}
	tv := m.screen.TV
	// a point in the 320x240 picture, by its position in clip space
	at := func(x, y float64) uint32 {
		px := int((x + 1) * 160)
		py := int((1 - y) * 120)
		return tv[(py*3+1)*TVWidth+px*4+1]
	}
	rgb := func(v uint32) (int, int, int) { return int(v >> 16 & 255), int(v >> 8 & 255), int(v & 255) }
	near := func(v uint32, r, g, b int) bool {
		vr, vg, vb := rgb(v)
		d := func(a, b int) bool { return a-b <= 20 && b-a <= 20 }
		return d(vr, r) && d(vg, g) && d(vb, b)
	}
	clear := [3]int{26, 51, 102}
	type check struct {
		what    string
		x, y    float64
		r, g, b int
	}
	checks := []check{
		{"background", 0, 0.6, clear[0], clear[1], clear[2]},
		{"triangle's red corner", -0.92, -0.78, 255, 0, 0},
		{"triangle's blue tip", -0.675, 0.75, 0, 0, 255},
		{"texture's first square (yellow)", 0.43, 0.77, 255, 255, 0},
		{"texture's second square (red)", 0.43 + 0.1375, 0.77, 255, 0, 0},
		{"green quad in front", 0, 0, 0, 255, 0},
		{"red quad where it isn't hidden", 0.25, 0.25, 255, 0, 0},
		{"blended band", 0, 0.92, 140, 153, 179},
		{"indexed cyan quad", 0.7, -0.93, 0, 255, 255},
	}
	bad := false
	for _, c := range checks {
		v := at(c.x, c.y)
		if !near(v, c.r, c.g, c.b) {
			r, g, b := rgb(v)
			t.Errorf("%s: got %d,%d,%d want %d,%d,%d", c.what, r, g, b, c.r, c.g, c.b)
			bad = true
		}
	}
	// the display list's small triangle: a mix of all three colours
	if r, g, b := rgb(at(0.0, -0.667)); r < 40 || g < 40 || b < 40 || [3]int{r, g, b} == clear {
		t.Errorf("display list triangle: got %d,%d,%d", r, g, b)
		bad = true
	}
	if bad {
		if dir := os.Getenv("WIIU_DUMP"); dir != "" {
			writePPM(dir+"/gx2test.ppm", tv, TVWidth, TVHeight)
			fmt.Println("wrote", dir+"/gx2test.ppm")
		}
	}
}

func writePPM(path string, px []uint32, w, h int) {
	var b strings.Builder
	fmt.Fprintf(&b, "P6\n%d %d\n255\n", w, h)
	out := []byte(b.String())
	for _, v := range px[:w*h] {
		out = append(out, byte(v>>16), byte(v>>8), byte(v))
	}
	os.WriteFile(path, out, 0o644)
}
