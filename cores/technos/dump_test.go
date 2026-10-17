package technos

// Development check: renders main-CPU memory dumped from MAME (Lua, at the
// frames of a reference run) with this package's video code and compares
// the result with MAME's snapshot of the same frame. Skips without the dumps
// (RP_DUMPDIR, default /tmp/claude-0/ddragon/dumps).

import (
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"testing"
)

func dumpDir() string {
	if d := os.Getenv("RP_DUMPDIR"); d != "" {
		return d
	}
	return "/tmp/claude-0/ddragon/dumps"
}

// readPNG reads a picture as 0x00RRGGBB pixels.
func readPNG(path string) ([]uint32, int, int, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, 0, 0, err
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		return nil, 0, 0, err
	}
	r := img.Bounds()
	pix := make([]uint32, r.Dx()*r.Dy())
	for y := 0; y < r.Dy(); y++ {
		for x := 0; x < r.Dx(); x++ {
			cr, cg, cb, _ := img.At(r.Min.X+x, r.Min.Y+y).RGBA()
			pix[y*r.Dx()+x] = (cr>>8)<<16 | (cg>>8)<<8 | cb>>8
		}
	}
	return pix, r.Dx(), r.Dy(), nil
}

// renderDump draws a Double Dragon memory dump (0000-37ff, then 3808-380a).
func renderDump(b *ddragon, d []byte) []uint32 {
	copy(b.ram[:], d[0:0x1000])
	copy(b.palRAM[:], d[0x1000:0x1400])
	copy(b.fgRAM[:], d[0x1800:0x2000])
	copy(b.shared[:], d[0x2000:0x2200])
	copy(b.sprRAM[:], d[0x2800:0x3000])
	copy(b.bgRAM[:], d[0x3000:0x3800])
	b.ctrl, b.scrollX, b.scrollY = d[0x3800], d[0x3801], d[0x3802]
	b.vid.scrollX = int(b.scrollX) | int(b.ctrl&1)<<8
	b.vid.scrollY = int(b.scrollY) | int(b.ctrl&2)<<7
	b.vid.flip = b.ctrl&4 == 0
	b.vid.render()
	return b.vid.screen
}

func TestDumpCompare(t *testing.T) {
	files, _ := filepath.Glob(filepath.Join(dumpDir(), "dd_*.bin"))
	if len(files) == 0 {
		t.Skip("no MAME dumps")
	}
	sort.Strings(files)
	c := openReal(t, "ddragon")
	b := c.Board().(*ddragon)
	out := os.Getenv("RP_OUT")
	for _, f := range files {
		d, err := os.ReadFile(f)
		if err != nil || len(d) < 0x3803 {
			continue
		}
		want, w, h, err := readPNG(f[:len(f)-4] + ".png")
		if err != nil || w != scrW || h != scrH {
			continue
		}
		got := renderDump(b, d)
		diff := 0
		for i := range got {
			if got[i] != want[i] {
				diff++
			}
		}
		t.Logf("%s: %d pixels differ", filepath.Base(f), diff)
		if out != "" {
			writePix(t, got, scrW, scrH, filepath.Join(out, fmt.Sprintf("dump-%s.png", filepath.Base(f[:len(f)-4]))))
		}
	}
}
