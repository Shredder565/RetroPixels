package dec8

// Tests with the user's own ROM set and MAME reference dumps; they skip when
// those files are absent (as in CI).

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const (
	romZip  = "/mnt/project-files/roms-private/arcade-batch2/ghostb.zip"
	dumpDir = "/tmp/claude-0/ghostb/dump"
	outDir  = "/tmp/claude-0/ghostb/compare"
)

// realFiles reads the user's set, or skips the test.
func realFiles(t *testing.T) (arcade.Files, *arcade.Game) {
	t.Helper()
	data, err := os.ReadFile(romZip)
	if err != nil {
		t.Skip("no ROM set:", err)
	}
	names, _, contents, err := arcade.ReadZip(data)
	if err != nil {
		t.Fatal(err)
	}
	files := arcade.Files{}
	for i, n := range names {
		files[arcade.BaseName(n)] = contents[i]
	}
	g := &arcade.Game{ID: "ghostb", Files: filesGhostb}
	return files, g
}

func readPNG(path string) (image.Image, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return png.Decode(f)
}

// writeSideBySide writes ours | MAME's | differences (in red).
func writeSideBySide(path string, ours []uint32, ref image.Image) int {
	out := image.NewRGBA(image.Rect(0, 0, scrW*3, scrH))
	diff := 0
	for y := 0; y < scrH; y++ {
		for x := 0; x < scrW; x++ {
			p := ours[y*scrW+x]
			c := color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255}
			out.Set(x, y, c)
			r, g, b, _ := ref.At(x, y).RGBA()
			rc := color.RGBA{uint8(r >> 8), uint8(g >> 8), uint8(b >> 8), 255}
			out.Set(x+scrW, y, rc)
			if rc != c {
				diff++
				out.Set(x+2*scrW, y, color.RGBA{255, 0, 0, 255})
			} else {
				out.Set(x+2*scrW, y, color.RGBA{c.R / 3, c.G / 3, c.B / 3, 255})
			}
		}
	}
	os.MkdirAll(filepath.Dir(path), 0o755)
	if f, err := os.Create(path); err == nil {
		png.Encode(f, out)
		f.Close()
	}
	return diff
}

// TestDumpFrames renders MAME's RAM and video registers at chosen frames and
// compares with MAME's snapshots of the same frames.
func TestDumpFrames(t *testing.T) {
	files, g := realFiles(t)
	dumpDir := dumpDir
	if d := os.Getenv("RP_DUMPDIR"); d != "" {
		dumpDir = d
	}
	if _, err := os.Stat(dumpDir); err != nil {
		t.Skip("no MAME dumps")
	}
	v := newVideo(arcade.Region(g, files, "chars", 0x8000), arcade.Region(g, files, "tiles", 0x40000),
		arcade.Region(g, files, "sprites", 0x80000), arcade.Region(g, files, "proms", 0x800)[:0x400],
		arcade.Region(g, files, "proms", 0x800)[0x400:])
	pngs, _ := filepath.Glob(filepath.Join(dumpDir, "f*.png"))
	for _, p := range pngs {
		var n int
		fmt.Sscanf(filepath.Base(p), "f%05d.png", &n)
		// the state at the end of the vertical blank (MAME draws the frame
		// then); the sprites from the start of the blank (the copy the
		// sprite chip got during the blank)
		dump, err := os.ReadFile(filepath.Join(dumpDir, fmt.Sprintf("f%05dd.bin", n)))
		if err != nil {
			continue
		}
		spr := dump[0x3000:0x3800]
		if sp, err := os.ReadFile(filepath.Join(dumpDir, fmt.Sprintf("f%05dn.bin", n-1))); err == nil && os.Getenv("SPRD") == "" {
			spr = sp[0x3000:0x3800]
		}
		ref, err := readPNG(p)
		if err != nil {
			t.Fatal(err)
		}
		var r frameRegs
		for i := 0; i < 4; i++ {
			r.ctrl[i] = dump[0x3820+2*i]
		}
		copy(r.scroll[:], dump[0x3830:0x3840])
		r.flip = dump[0x383f]&8 != 0
		v.render(dump[0x1800:0x2000], dump[0x2000:0x2800], dump[0x2c00:0x3000], spr, &r)
		d := writeSideBySide(filepath.Join(outDir, fmt.Sprintf("dump%05d.png", n)), v.screen, ref)
		t.Logf("frame %d: %d pixels differ", n, d)
	}
}

// TestRealBoot plays the user's set from a cold boot: the attract mode
// comes up, a coin starts a game, and a save state taken in play runs on
// the same as the original.
func TestRealBoot(t *testing.T) {
	data, err := os.ReadFile(romZip)
	if err != nil {
		t.Skip("no ROM set")
	}
	cc, err := arcade.New(data, core.Options{Name: "ghostb", SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	c := cc.(*arcade.Core)
	b := c.Board().(*ghostb)
	idx := map[string]int{}
	for i, n := range c.Info().Buttons {
		idx[n] = i
	}
	colours := func() int {
		seen := map[uint32]bool{}
		for _, p := range c.Screen() {
			seen[p] = true
		}
		return len(seen)
	}
	for f := 1; f <= 1000; f++ {
		var in uint32
		if f >= 300 && f < 306 {
			in = 1 << idx["Coin"]
		}
		if f >= 400 && f < 420 {
			in = 1 << idx["Button 1"]
		}
		c.SetInput(0, in)
		c.RunFrame()
		if f == 100 && colours() < 3 {
			t.Errorf("title: %d colours on screen", colours())
		}
		if f == 400 && colours() < 30 {
			t.Errorf("after the coin: %d colours on screen", colours())
		}
	}
	if b.ram[0x19]|b.ram[0x1a] == 0 {
		t.Error("the coin didn't start a game")
	}
	if a := c.TakeAudio(); len(a) == 0 {
		t.Error("no sound")
	}
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	run := func() []uint32 {
		for i := 0; i < 120; i++ {
			c.SetInput(0, uint32(i/10%4)<<idx["Up"])
			c.RunFrame()
		}
		return append([]uint32(nil), c.Screen()...)
	}
	want := run()
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	got := run()
	for i := range want {
		if got[i] != want[i] {
			t.Fatal("the picture differs after loading the state")
		}
	}
}
