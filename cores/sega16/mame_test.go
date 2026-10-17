package sega16

import (
	"encoding/binary"
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// TestMAMEDumps renders RAM dumps taken from MAME (SEGA16_DUMPS: a folder of
// f*/ folders with tile.bin, text.bin, spr.bin, pal.bin and snap.png) with
// this package's video code and compares with MAME's picture. It needs the
// user's ROMs and the dumps, so it only runs when asked.
func TestMAMEDumps(t *testing.T) {
	dir := os.Getenv("SEGA16_DUMPS")
	if dir == "" {
		t.Skip("SEGA16_DUMPS not set")
	}
	c := loadSet(t, "altbeast")
	b := c.Board().(*board)
	ents, _ := os.ReadDir(dir)
	var names []string
	for _, e := range ents {
		if e.IsDir() && (strings.HasPrefix(e.Name(), "f") || strings.HasPrefix(e.Name(), "t")) {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	out := os.Getenv("SEGA16_OUT")
	total, exact := 0, 0
	for _, n := range names {
		d := filepath.Join(dir, n)
		v := b.v
		if !readWords(t, filepath.Join(d, "tile.bin"), v.tileRAM[:]) ||
			!readWords(t, filepath.Join(d, "text.bin"), v.textRAM[:]) ||
			!readWords(t, filepath.Join(d, "spr.bin"), v.sprRAM[:]) ||
			!readWords(t, filepath.Join(d, "pal.bin"), v.palRAM[:]) {
			continue
		}
		for i := range v.palRAM {
			v.setColor(i)
		}
		v.enable = true
		v.bank = [2]int{0, 1}
		v.render()
		f, err := os.Open(filepath.Join(d, "snap.png"))
		if err != nil {
			continue
		}
		img, err := png.Decode(f)
		f.Close()
		if err != nil {
			t.Fatal(err)
		}
		diff := 0
		for y := 0; y < scrH; y++ {
			for x := 0; x < scrW; x++ {
				r, g, bl, _ := img.At(x, y).RGBA()
				want := uint32(r>>8)<<16 | uint32(g>>8)<<8 | uint32(bl>>8)
				if v.screen[y*scrW+x] != want {
					diff++
				}
			}
		}
		total++
		if diff == 0 {
			exact++
		}
		fmt.Printf("%s %d\n", n, diff)
		if out != "" {
			savePNG(t, filepath.Join(out, n+".png"), v.screen, scrW, scrH)
		}
	}
	t.Logf("%d of %d frames pixel-exact", exact, total)
}

func readWords(t *testing.T, path string, dst []uint16) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	for i := range dst {
		if 2*i+1 < len(data) {
			dst[i] = binary.BigEndian.Uint16(data[2*i:])
		}
	}
	return true
}
