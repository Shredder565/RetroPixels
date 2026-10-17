package ddragon3

// Development comparisons with state dumps taken from MAME (DD3_DUMPS: a
// folder of f<N>.bin files written by a MAME Lua script; skipped when it
// isn't set). A dump is "DD3E", the video registers (16 bytes), the I/O
// registers (8), work RAM, foreground, background, palette, the sprite RAM
// at that frame and at the frame before, then the picture MAME showed
// (width, height, then 32-bit pixels).

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

type dump struct {
	frame        int
	regs         [16]byte
	io           [8]byte
	wram         []byte
	fg, bg, pal  []byte
	spr, sprPrev []byte
	w, h         int
	pix          []uint32
}

func readDump(path string) (*dump, error) {
	d, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(d) < 4 || string(d[:4]) != "DD3E" {
		return nil, fmt.Errorf("%s: not a dump", path)
	}
	x := &dump{}
	o := 4
	take := func(n int) []byte { v := d[o : o+n]; o += n; return v }
	copy(x.regs[:], take(16))
	copy(x.io[:], take(8))
	x.wram = take(0x4000)
	x.fg = take(0x1000)
	x.bg = take(0x800)
	x.pal = take(0x600)
	x.spr = take(0x1000)
	x.sprPrev = take(0x1000)
	x.w = int(binary.LittleEndian.Uint32(take(4)))
	x.h = int(binary.LittleEndian.Uint32(take(4)))
	p := take(x.w * x.h * 4)
	x.pix = make([]uint32, x.w*x.h)
	for i := range x.pix {
		x.pix[i] = binary.LittleEndian.Uint32(p[i*4:]) & 0xffffff
	}
	base := filepath.Base(path)
	x.frame, _ = strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(base, "f"), ".bin"))
	return x, nil
}

func dumpFiles(t *testing.T) []string {
	dir := os.Getenv("DD3_DUMPS")
	if dir == "" {
		t.Skip("DD3_DUMPS not set")
	}
	files, _ := filepath.Glob(filepath.Join(dir, "f*.bin"))
	sort.Slice(files, func(i, j int) bool {
		a, _ := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(filepath.Base(files[i]), "f"), ".bin"))
		b, _ := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(filepath.Base(files[j]), "f"), ".bin"))
		return a < b
	})
	return files
}

// loadDump puts a dump's video state into v.
func (v *video) loadDump(x *dump, prevSprites bool) {
	copy(v.regs[:], x.regs[:])
	copy(v.prio[:], x.io[0:2])
	copy(v.fgram[:], x.fg)
	copy(v.bgram[:], x.bg)
	copy(v.palram[:], x.pal)
	for i := range v.pal {
		v.updateColour(i)
	}
	if prevSprites {
		copy(v.spbuf[:], x.sprPrev)
	} else {
		copy(v.spbuf[:], x.spr)
	}
}

func writePNG(path string, w, h int, pix ...[]uint32) error {
	img := image.NewRGBA(image.Rect(0, 0, w*len(pix), h))
	for k, p := range pix {
		for i, c := range p[:w*h] {
			img.Set(k*w+i%w, i/w, color.RGBA{uint8(c >> 16), uint8(c >> 8), uint8(c), 255})
		}
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// TestDumpPictures draws each dump and counts the pixels that differ from
// MAME's picture; DD3_OUT gets side-by-side PNGs (ours, MAME's, the
// differences in red).
func TestDumpPictures(t *testing.T) {
	files := dumpFiles(t)
	g := &games()[0]
	set := loadRealFiles(t)
	vid := newVideo(planes(g, set, "tiles"), planes(g, set, "sprites"))
	out := os.Getenv("DD3_OUT")
	prev := os.Getenv("DD3_PREVSPR") != "0"
	total, bad := 0, 0
	for _, f := range files {
		x, err := readDump(f)
		if err != nil {
			t.Fatal(err)
		}
		vid.loadDump(x, prev)
		vid.render()
		diff := make([]uint32, len(vid.screen))
		n := 0
		for i, c := range vid.screen {
			if c != x.pix[i] {
				n++
				diff[i] = 0xff0000
			}
		}
		total += len(vid.screen)
		bad += n
		t.Logf("frame %5d: %6d pixels differ (prio %02x regs % x)", x.frame, n, x.io[1], x.regs[:])
		if out != "" && (n > 0 || os.Getenv("DD3_ALL") != "") {
			writePNG(filepath.Join(out, fmt.Sprintf("d%05d.png", x.frame)), ScreenW, ScreenH, vid.screen, x.pix, diff)
		}
	}
	t.Logf("%d of %d pixels differ (%.4f%%)", bad, total, 100*float64(bad)/float64(max(total, 1)))
}

// schedPresses reads a play schedule shared with the MAME run: lines of
// "frame mask" (bits: right, left, up, down, buttons 1-3, coin, start). MAME
// sets the inputs when frame N ends, so they count from our frame N+1.
func schedPresses(t *testing.T, path string) []press {
	d, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	names := []string{"Right", "Left", "Up", "Down", "Button 1", "Button 2", "Button 3", "Coin", "Start (1P)"}
	var out []press
	for _, l := range strings.Split(string(d), "\n") {
		var f, m int
		if _, err := fmt.Sscanf(l, "%d %d", &f, &m); err != nil {
			continue
		}
		for i, n := range names {
			if m>>i&1 != 0 {
				out = append(out, press{n, f + 1, 1})
			}
		}
	}
	return out
}

// TestColdBootVsMAME plays the attract mode from power on and compares our
// pictures with MAME's at the dumps' frames (DD3_INPUT=coin presses a coin
// at frame 600 and start at 700, as the matching MAME run did).
func TestColdBootVsMAME(t *testing.T) {
	files := dumpFiles(t)
	var dumps []*dump
	last := 0
	for _, f := range files {
		x, err := readDump(f)
		if err != nil {
			t.Fatal(err)
		}
		dumps = append(dumps, x)
		last = max(last, x.frame)
	}
	c := loadReal(t)
	var presses []press
	if os.Getenv("DD3_INPUT") == "coin" {
		presses = []press{{"Coin", 601, 6}, {"Start (1P)", 701, 6}}
	}
	if p := os.Getenv("DD3_SCHED"); p != "" {
		presses = schedPresses(t, p)
	}
	out := os.Getenv("DD3_OUT")
	off, _ := strconv.Atoi(os.Getenv("DD3_OFFSET")) // compare our frame f with MAME's f+off
	k, total, bad, exact := 0, 0, 0, 0
	for f := 1; f <= last; f++ {
		c.SetInput(0, buttonBits(c, presses, f))
		c.RunFrame()
		c.TakeAudio()
		if os.Getenv("DD3_RAM") != "" {
			// work RAM is dumped at the same moment our frame ends
			b := c.Board().(*board)
			for _, x := range dumps {
				if x.frame == f {
					n, first := 0, -1
					for i := range b.wram {
						if b.wram[i] != x.wram[i] {
							n++
							if first < 0 {
								first = i
							}
						}
					}
					t.Logf("frame %5d: %d work RAM bytes differ (first at %x) coin timer %02x%02x mame %02x%02x", f, n, first, b.wram[0x32], b.wram[0x33], x.wram[0x32], x.wram[0x33])
				}
			}
		}
		for k < len(dumps) && dumps[k].frame-off <= f {
			x := dumps[k]
			k++
			scr := c.Screen()
			diff := make([]uint32, len(scr))
			n := 0
			for i, p := range scr {
				if p != x.pix[i] {
					n++
					diff[i] = 0xff0000
				}
			}
			if n == 0 {
				exact++
			}
			total += len(scr)
			bad += n
			t.Logf("frame %5d: %6d pixels differ", f, n)
			if out != "" && (n > 0 || os.Getenv("DD3_ALL") != "") {
				writePNG(filepath.Join(out, fmt.Sprintf("b%05d.png", f)), ScreenW, ScreenH, scr, x.pix, diff)
			}
		}
	}
	t.Logf("%d of %d frames exact; %d of %d pixels differ (%.4f%%)", exact, len(dumps), bad, total, 100*float64(bad)/float64(max(total, 1)))
}
