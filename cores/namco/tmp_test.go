package namco

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"retropixels/cores/arcade"
)

// RP_STARS=dir: lists MAME's star pixels (raw coordinates) per dump: pixels
// where MAME isn't black and this board draws nothing.
func TestTmpStars(t *testing.T) {
	dir := os.Getenv("RP_STARS")
	if dir == "" {
		t.Skip()
	}
	c := openReal(t, "galaga")
	b := c.Board().(*board)
	bins, _ := filepath.Glob(filepath.Join(work, "ref", dir, "*.bin"))
	sort.Strings(bins)
	out, _ := os.Create(filepath.Join(work, "stars-"+dir+".txt"))
	defer out.Close()
	for _, bin := range bins {
		d, _ := os.ReadFile(bin)
		want, _, _, err := readPix(strings.TrimSuffix(bin, ".bin") + ".png")
		if err != nil {
			continue
		}
		raw := make([]uint32, scrW*scrH)
		arcade.RotateInto(want, scrH, scrW, 270, raw)
		copy(b.vram[:], d[0:0x800])
		for i := 0; i < 3; i++ {
			copy(b.ram[i][:], d[0x800+i*0x800:0xc00+i*0x800])
		}
		copy(b.vlat[:], d[0x2000:0x2008])
		b.vid.render(b)
		fmt.Fprintf(out, "F %s %v\n", filepath.Base(bin), d[0x2000:0x2008])
		for i, v := range raw {
			if v != 0 && b.screen[i] == 0 {
				fmt.Fprintf(out, "%d %d %06x\n", i%scrW, i/scrW, v)
			}
		}
	}
}
