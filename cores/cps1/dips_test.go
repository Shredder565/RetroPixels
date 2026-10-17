package cps1

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// TestMAMEDips plays from power on with DIP switches set (CPS1_DIPS:
// "id=value,...") and no inputs, and compares the frames with a MAME run's
// dumps (CPS1_DIPDUMPS). MAME's picture at frame N is the one drawn at the
// end of frame N-1.
func TestMAMEDips(t *testing.T) {
	dir := os.Getenv("CPS1_DIPDUMPS")
	if dir == "" {
		t.Skip("CPS1_DIPDUMPS not set")
	}
	data, err := os.ReadFile(realZip)
	if err != nil {
		t.Skip("1941.zip not available")
	}
	dips := map[string]int{}
	for _, kv := range strings.Split(os.Getenv("CPS1_DIPS"), ",") {
		var k string
		var v int
		if i := strings.IndexByte(kv, '='); i > 0 {
			k = kv[:i]
			fmt.Sscan(kv[i+1:], &v)
			dips[k] = v
		}
	}
	cc, err := arcade.New(data, core.Options{Name: "1941", SampleRate: 48000, Dips: dips})
	if err != nil {
		t.Fatal(err)
	}
	c := cc.(*arcade.Core)
	out := os.Getenv("CPS1_OUT")
	for f := 1; f <= 1000; f++ {
		runRange(c, f, nil)
		d, err := readDump(filepath.Join(dir, fmt.Sprintf("f%05d.bin", f+1)))
		if err != nil {
			continue
		}
		b := c.Board().(*board)
		diff := 0
		for i, px := range b.vid.screen {
			if px != d.pix[i] {
				diff++
			}
		}
		t.Logf("frame %d: %d pixels differ", f, diff)
		if out != "" {
			side := make([]uint32, ScreenW*2*ScreenH)
			for y := 0; y < ScreenH; y++ {
				copy(side[y*ScreenW*2:], b.vid.screen[y*ScreenW:(y+1)*ScreenW])
				copy(side[y*ScreenW*2+ScreenW:], d.pix[y*ScreenW:(y+1)*ScreenW])
			}
			savePNG(filepath.Join(out, fmt.Sprintf("dip%05d.png", f)), side, ScreenW*2, ScreenH)
		}
	}
}
