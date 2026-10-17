package capcom

// Checks the video against MAME: MAME's RAM and register writes at chosen
// frames (dumped by a Lua script) are loaded into the board, the board draws
// them, and the picture is compared with MAME's snapshot of the same frame.
// Run with RP_DUMPCMP=set:dumpdir:snapdir (snapshots numbered in dump order).

import (
	"bufio"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"

	"retropixels/cores/arcade"
)

type mameDump struct {
	frame  int
	ranges map[int][]byte
	regs   map[int]byte
	pal    []uint32
}

func readDump(path string) (*mameDump, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	d := &mameDump{ranges: map[int][]byte{}, regs: map[int]byte{}}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	for sc.Scan() {
		w := strings.Fields(sc.Text())
		if len(w) < 2 {
			continue
		}
		switch w[0] {
		case "RANGE":
			a, _ := strconv.ParseInt(w[1], 16, 32)
			b := make([]byte, len(w)-2)
			for i, x := range w[2:] {
				v, _ := strconv.ParseUint(x, 16, 8)
				b[i] = byte(v)
			}
			d.ranges[int(a)] = b
		case "REG":
			a, _ := strconv.ParseInt(w[1], 16, 32)
			v, _ := strconv.ParseUint(w[2], 16, 8)
			d.regs[int(a)] = byte(v)
		case "PAL":
			for _, x := range w[2:] {
				v, _ := strconv.ParseUint(x, 16, 32)
				d.pal = append(d.pal, uint32(v))
			}
		}
	}
	return d, sc.Err()
}

// dumpLoader is a board that can take a MAME dump and draw it.
type dumpLoader interface {
	loadDump(d *mameDump)
}

func readPNG(path string) (image.Image, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return png.Decode(f)
}

func TestDumpCompare(t *testing.T) {
	spec := os.Getenv("RP_DUMPCMP")
	if spec == "" {
		t.Skip("RP_DUMPCMP not set")
	}
	p := strings.Split(spec, ":")
	c := openReal(t, p[0])
	dumps, _ := filepath.Glob(filepath.Join(p[1], "dump_*.txt"))
	sort.Strings(dumps)
	out := os.Getenv("RP_DEVOUT")
	total := 0
	for k, dp := range dumps {
		d, err := readDump(dp)
		if err != nil {
			t.Fatal(err)
		}
		c.Board().(dumpLoader).loadDump(d)
		pic := make([]uint32, c.Width()*c.Height())
		arcade.RotateInto(c.Board().Screen(), scrW, scrH, 270, pic)
		img, err := readPNG(filepath.Join(p[2], strconv.Itoa(10000 + k)[1:]+".png"))
		if err != nil {
			t.Fatal(err)
		}
		diff := 0
		for i, v := range pic {
			r, g, b, _ := img.At(i%c.Width(), i/c.Width()).RGBA()
			if uint32(r>>8)<<16|uint32(g>>8)<<8|uint32(b>>8) != v {
				diff++
			}
		}
		total += diff
		t.Logf("%s: %d pixels differ", filepath.Base(dp), diff)
		if out != "" {
			os.MkdirAll(out, 0o755)
			writePix(t, pic, c.Width(), c.Height(), filepath.Join(out, "dump_"+strconv.Itoa(k)+".png"))
		}
	}
	t.Logf("total %d", total)
}

func (b *b1942) loadDump(d *mameDump) {
	for a, v := range d.ranges {
		for i, x := range v {
			b.write(uint16(a+i), x)
		}
	}
	for a, v := range d.regs {
		if a != 0xc804 {
			b.write(uint16(a), v)
		}
	}
	b.c804 = d.regs[0xc804]
	b.render()
}
