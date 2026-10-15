package atari2600

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// TestDetectMatchesJS runs the Go and the web scheme detection on thousands of
// made-up cartridge files (random bytes with start vectors, code that touches
// each scheme's hotspots, text markers, Superchip fills and scheme tags in
// the name) and checks they agree. Skipped without node.
func TestDetectMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	n := 3000
	if testing.Short() {
		n = 500
	}
	dir := t.TempDir()
	s := uint64(0x9e3779b97f4a7c15)
	rnd := func(k int) int {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		return int(s % uint64(k))
	}
	sizes := []int{512, 1024, 2048, 3000, 4096, 6144, 8192, 8192, 8192, 8448, 10240, 10495, 12288, 16384, 16384, 16896, 24 << 10, 28 << 10, 29 << 10,
		32768, 32768, 65536, 65536, 131072, 262144, 81920, 50000, 2048 * 3}
	snippets := [][]byte{
		{0xad, 0xe0, 0xff}, {0xad, 0xe9, 0xff}, {0xad, 0xf3, 0xff}, {0xad, 0xf8, 0xff}, {0x2c, 0xf9, 0x1f}, {0x8d, 0xf6, 0xff},
		{0xad, 0xe3, 0xff}, {0xad, 0xea, 0xff}, {0xad, 0xfa, 0xff}, {0x8d, 0x40, 0x08}, {0x8d, 0x00, 0x08}, {0xad, 0x40, 0x02},
		{0xad, 0x20, 0x02}, {0x85, 0x3f}, {0x85, 0x3e}, {0x86, 0x3f}, {0x9d, 0x00, 0xf4}, {0x99, 0x10, 0xf5}, {0x20, 0x00, 0xd0},
		{0x20, 0x40, 0xf0}, {0x4c, 0x00, 0xf1}, {0xbd, 0x0d, 0x08}, {0xbd, 0xfd, 0x08}, {0xad, 0xf0, 0xff}, {0xad, 0xc5, 0xff},
		{0xad, 0xa0, 0xff}, {0xd0, 0x04}, {0xf0, 0xf0}, {0x60}, {0xea}, {0xa9, 0x00}, {0xbd, 0xe0, 0xff}, {0xb9, 0xf8, 0x1f},
	}
	texts := []string{"TJ3E", "MDMC", "DPC+", "CDFJ", "CDF", "EFEF", "EFSC", "DFSC", "DFDF", "BFSC", "BFBF"}
	tags := []string{"(F8SC)", "[3E+]", "(BS-F6)", "(SC)", "(DPCP)", "(e0)", "(bs_FE)", "(Supercharger)", "(4A50)", "(CDFJ+)", "(x, F4)", "[MB]", "(nothing)", "(UASW)"}
	var list strings.Builder
	names := make([]string, n)
	roms := make([][]byte, n)
	for i := 0; i < n; i++ {
		size := sizes[rnd(len(sizes))]
		rom := make([]byte, size)
		switch rnd(3) {
		case 0:
			for k := range rom {
				rom[k] = byte(rnd(256))
			}
		case 1:
			for k := range rom {
				rom[k] = 0xea // NOPs: only the planted code is seen
			}
		default:
			for k := range rom {
				rom[k] = byte(k * 7)
			}
		}
		bank := 4096
		if size < 4096 {
			bank = max(1024, min(2048, size))
		}
		for end := size; end-bank >= 0; end -= bank {
			base := end - bank
			// start vector into this block, built for $F000 / $D000 / $1000 / elsewhere
			homes := []int{0xf000, 0xd000, 0x1000, 0x7000, 0xf800}
			home := homes[rnd(len(homes))] & -bank & 0xffff
			entry := 0x40 + rnd(0x100)
			rom[end-4], rom[end-3] = byte(home+entry), byte((home+entry)>>8)
			if rnd(2) == 0 {
				rom[end-2], rom[end-1] = rom[end-4], rom[end-3]
			}
			pc := base + entry
			for k := 0; k < 4+rnd(12) && pc < end-8; k++ {
				sn := snippets[rnd(len(snippets))]
				copy(rom[pc:], sn)
				pc += len(sn)
			}
			if rnd(4) == 0 { // Superchip-style fill
				for k := 0; k < 256 && base+k < end; k++ {
					rom[base+k] = 0xff
				}
			}
		}
		if rnd(6) == 0 {
			tx := texts[rnd(len(texts))]
			copy(rom[rnd(max(1, min(size, 4096)-8)):], tx)
		}
		name := fmt.Sprintf("game%d.bin", i)
		if rnd(5) == 0 {
			name = "game " + tags[rnd(len(tags))] + ".a26"
		}
		p := filepath.Join(dir, fmt.Sprintf("%d.bin", i))
		if err := os.WriteFile(p, rom, 0o644); err != nil {
			t.Fatal(err)
		}
		names[i], roms[i] = name, rom
		fmt.Fprintf(&list, "%s\t%s\n", p, name)
	}
	lp := filepath.Join(dir, "list.txt")
	os.WriteFile(lp, []byte(list.String()), 0o644)
	emu, _ := filepath.Abs("../../web/emu")
	ref, _ := filepath.Abs("testdata/detect.js")
	out, err := exec.Command(node, ref, emu, lp).Output()
	if err != nil {
		t.Fatal(err)
	}
	js := strings.Split(strings.TrimSuffix(string(out), "\n"), "\n")
	if len(js) != n {
		t.Fatalf("%d answers from JS for %d files", len(js), n)
	}
	counts := map[string]int{}
	for i := 0; i < n; i++ {
		var got string
		typ, err := detectType(roms[i], names[i])
		switch {
		case err != nil:
			got = "ERR:" + err.Error()
		case unsupported[typ] != "":
			got = "ERR:" + unsupported[typ]
		default:
			got = typ
		}
		if got != js[i] {
			t.Errorf("file %d (%d bytes, %q): go %q, js %q", i, len(roms[i]), names[i], got, js[i])
		}
		counts[got]++
	}
	t.Logf("%d files agree; schemes seen: %v", n, counts)
}
