package cps1

// Tests with the real 1941 set (skipped when it isn't there) and, for
// development, comparisons with state dumps taken from MAME (CPS1_DUMPS:
// a folder of f<N>.bin files, see the package's dump format below).

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const realZip = "/mnt/project-files/roms-private/arcade-new/1941.zip"

func loadReal(t *testing.T) *arcade.Core {
	t.Helper()
	data, err := os.ReadFile(realZip)
	if err != nil {
		t.Skip("1941.zip not available")
	}
	c, err := arcade.New(data, core.Options{Name: "1941", SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

func savePNG(path string, pix []uint32, w, h int) error {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix[:w*h] {
		img.Set(i%w, i/w, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// press holds a button for frames [from, from+n).
type press struct {
	btn     string
	from, n int
}

func runReal(t *testing.T, c *arcade.Core, frames int, presses []press, out string, every int, audio *[]float32) {
	bits := func(f int) uint32 {
		v := uint32(0)
		info := c.Info()
		for _, p := range presses {
			if f >= p.from && f < p.from+p.n {
				for i, b := range info.Buttons {
					if b == p.btn {
						v |= 1 << i
					}
				}
			}
		}
		return v
	}
	for f := 1; f <= frames; f++ {
		c.SetInput(0, bits(f))
		c.RunFrame()
		a := c.TakeAudio()
		if audio != nil {
			*audio = append(*audio, a...)
		}
		if out != "" && every > 0 && f%every == 0 {
			if err := savePNG(filepath.Join(out, fmt.Sprintf("n%05d.png", f)), c.Screen(), c.Width(), c.Height()); err != nil {
				t.Fatal(err)
			}
		}
	}
}

func TestReal1941Plays(t *testing.T) {
	c := loadReal(t)
	out := os.Getenv("CPS1_OUT")
	var audio []float32
	presses := []press{{"Coin", 120, 6}, {"Start (1P)", 200, 6}, {"Button 1", 400, 600}, {"Left", 500, 60}}
	runReal(t, c, 1200, presses, out, 100, &audio)
	// the picture has many colours
	seen := map[uint32]bool{}
	for _, p := range c.Screen() {
		seen[p] = true
	}
	if len(seen) < 16 {
		t.Errorf("only %d colours on screen", len(seen))
	}
	// and there was sound
	peak := float32(0)
	for _, s := range audio {
		peak = max(peak, s, -s)
	}
	if peak < 0.01 {
		t.Errorf("no sound (peak %v)", peak)
	}
	if out != "" {
		f, _ := os.Create(filepath.Join(out, "audio.raw"))
		binary.Write(f, binary.LittleEndian, audio)
		f.Close()
	}
	// save state round trip
	s, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	runReal(t, c, 30, nil, "", 0, nil)
	want := append([]uint32(nil), c.Screen()...)
	if err := c.LoadState(s); err != nil {
		t.Fatal(err)
	}
	runReal(t, c, 30, nil, "", 0, nil)
	for i, p := range c.Screen() {
		if p != want[i] {
			t.Fatalf("state round trip differs at pixel %d", i)
		}
	}
}

// ---- MAME dumps ----
// f<N>.bin: "CPSD", graphics RAM (0x30000), CPS-A and CPS-B registers (32
// words each, as last written), work RAM (0x10000), width, height (u16 LE),
// then the screen (ARGB, little endian).

type dump struct {
	gfx, wram []byte
	a, b      [32]uint16
	w, h      int
	pix       []uint32
}

func readDump(path string) (*dump, error) {
	d, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	o := 4
	x := &dump{gfx: d[o : o+0x30000]}
	o += 0x30000
	for i := 0; i < 32; i++ {
		x.a[i] = binary.BigEndian.Uint16(d[o+i*2:])
		x.b[i] = binary.BigEndian.Uint16(d[o+64+i*2:])
	}
	x.wram = d[o+128 : o+128+0x10000]
	o += 128 + 0x10000
	x.w, x.h = int(binary.LittleEndian.Uint16(d[o:])), int(binary.LittleEndian.Uint16(d[o+2:]))
	o += 4
	x.pix = make([]uint32, x.w*x.h)
	for i := range x.pix {
		x.pix[i] = binary.LittleEndian.Uint32(d[o+i*4:]) & 0xffffff
	}
	return x, nil
}

// TestMAMEDumps redraws MAME's frames from its dumps (CPS1_DUMPS) and
// compares them with MAME's picture.
func TestMAMEDumps(t *testing.T) {
	dir := os.Getenv("CPS1_DUMPS")
	if dir == "" {
		t.Skip("CPS1_DUMPS not set")
	}
	data, err := os.ReadFile(realZip)
	if err != nil {
		t.Skip("1941.zip not available")
	}
	names, _, contents, err := arcade.ReadZip(data)
	if err != nil {
		t.Fatal(err)
	}
	files := arcade.Files{}
	for i, n := range names {
		files[arcade.BaseName(n)] = contents[i]
	}
	g := games()[0]
	v := newVideo(configs["1941"], loadGfx(&g, files))
	list, _ := filepath.Glob(filepath.Join(dir, "f*.bin"))
	sort.Strings(list)
	out := os.Getenv("CPS1_OUT")
	for _, p := range list {
		var n int
		fmt.Sscanf(filepath.Base(p), "f%d.bin", &n)
		get := func(back int) *dump {
			d, err := readDump(filepath.Join(dir, fmt.Sprintf("f%05d.bin", n-back)))
			if err != nil {
				return nil
			}
			return d
		}
		// MAME's picture at frame n was drawn at the vblank that ended
		// frame n-1: from the RAM and registers then (the palette copied
		// then too), with the sprite list copied at the vblank before.
		cur, prev, prev2 := get(0), get(1), get(2)
		if cur == nil || prev == nil || prev2 == nil {
			continue
		}
		v.reset()
		copy(v.gfxram[:], prev2.gfx)
		v.cpsa, v.cpsb = prev2.a, prev2.b
		v.latchSprites()
		copy(v.gfxram[:], prev.gfx)
		v.cpsa, v.cpsb = prev.a, prev.b
		v.copyPalette()
		v.render()
		diff := 0
		for i, px := range v.screen {
			if px != cur.pix[i] {
				diff++
			}
		}
		t.Logf("frame %d: %d of %d pixels differ", n, diff, len(v.screen))
		if diff != 0 {
			t.Errorf("frame %d differs from MAME", n)
		}
		if out != "" {
			side := make([]uint32, ScreenW*3*ScreenH)
			for y := 0; y < ScreenH; y++ {
				for x := 0; x < ScreenW; x++ {
					a, m := v.screen[y*ScreenW+x], cur.pix[y*ScreenW+x]
					side[y*ScreenW*3+x] = a
					side[y*ScreenW*3+ScreenW+x] = m
					if a != m {
						side[y*ScreenW*3+2*ScreenW+x] = 0xff00ff
					}
				}
			}
			savePNG(filepath.Join(out, fmt.Sprintf("cmp%05d.png", n)), side, ScreenW*3, ScreenH)
		}
	}
}
