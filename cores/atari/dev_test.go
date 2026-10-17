package atari

// Development helpers (not run in CI): RP_ATARI_DEV=1 go test -run Dev ...

import (
	"bufio"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const devOut = "/tmp/claude-0/atari/out"

func devOnly(t *testing.T) {
	if os.Getenv("RP_ATARI_DEV") == "" {
		t.Skip("development helper")
	}
}

func writePNG(t testing.TB, path string, pix []uint32, w, h int) {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix[:w*h] {
		img.Set(i%w, i/w, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	png.Encode(f, img)
}

func readPNG(t testing.TB, path string) ([]uint32, int, int) {
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		t.Fatal(err)
	}
	r := img.Bounds()
	out := make([]uint32, r.Dx()*r.Dy())
	for y := 0; y < r.Dy(); y++ {
		for x := 0; x < r.Dx(); x++ {
			cr, cg, cb, _ := img.At(r.Min.X+x, r.Min.Y+y).RGBA()
			out[y*r.Dx()+x] = (cr>>8)<<16 | (cg>>8)<<8 | cb>>8
		}
	}
	return out, r.Dx(), r.Dy()
}

func readPal(t testing.TB, path string) []uint32 {
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var out []uint32
	s := bufio.NewScanner(f)
	for s.Scan() {
		v, _ := strconv.ParseUint(strings.TrimSpace(s.Text()), 16, 32)
		out = append(out, uint32(v)&0xffffff)
	}
	return out
}

// sideBySide writes a and b (same size) next to each other with a diff map.
func sideBySide(t testing.TB, path string, a, b []uint32, w, h int) int {
	out := make([]uint32, w*3*h)
	diff := 0
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			pa, pb := a[y*w+x], b[y*w+x]
			out[y*w*3+x] = pa
			out[y*w*3+w+x] = pb
			if pa != pb {
				diff++
				out[y*w*3+2*w+x] = 0xff00ff
			}
		}
	}
	writePNG(t, path, out, w*3, h)
	return diff
}

func coreOpts() core.Options { return core.Options{SampleRate: 48000} }

func openSet(t testing.TB, id string) *arcade.Core {
	path := "/mnt/project-files/roms-private/arcade-new/" + id + ".zip"
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skip("no ROM set")
	}
	names, sizes, contents, err := arcade.ReadZip(data)
	if err != nil {
		t.Fatal(err)
	}
	c, err := arcade.NewFromFiles(names, sizes, contents, id+".zip", coreOpts())
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// TestDevCentipedeRAM renders MAME's RAM dumps with MAME's colours.
func TestDevCentipedeRAM(t *testing.T) {
	devOnly(t)
	c := openSet(t, "centiped")
	b := c.Board().(*centiped)
	dir := os.Getenv("RP_DUMP")
	for i, f := range strings.Split(os.Getenv("RP_FRAMES"), ",") {
		n, _ := strconv.Atoi(f)
		ram, err := os.ReadFile(fmt.Sprintf("%s/d-%05d.ram", dir, n))
		if err != nil {
			t.Fatal(err)
		}
		copy(b.ram[:], ram[:0x400])
		copy(b.vram[:], ram[0x400:0x800])
		pal := readPal(t, fmt.Sprintf("%s/d-%05d.pal", dir, n))
		var pens [16]uint32
		copy(pens[4:8], pal[0:4])
		// sprite colours: colour byte 0x39 picks registers 1, 2, 3; 0x3a picks 2,2,3 ... use 0x24 for register 0
		pens[12] = pal[4+0x24*4+1]
		pens[13] = pal[4+0x39*4+1]
		pens[14] = pal[4+0x39*4+2]
		pens[15] = pal[4+0x39*4+3]
		b.render(pens)
		up := make([]uint32, cnW*cnH)
		arcade.RotateInto(b.screen, cnW, cnH, 270, up)
		snap, w, h := readPNG(t, os.Getenv("RP_SNAP")+"/"+fmt.Sprintf("%04d.png", i))
		if w != cnH || h != cnW {
			t.Fatalf("snapshot %dx%d", w, h)
		}
		d := sideBySide(t, fmt.Sprintf("%s/cmp-%05d.png", devOut, n), up, snap, w, h)
		t.Logf("frame %d: %d pixels differ", n, d)
	}
}

// TestDevRun runs RP_SET from power-on for RP_N frames (coin at 120, start at
// 200, as tools/play.lua does), saving PNGs at RP_FRAMES and a WAV.
func TestDevRun(t *testing.T) {
	devOnly(t)
	id := os.Getenv("RP_SET")
	c := openSet(t, id)
	n, _ := strconv.Atoi(os.Getenv("RP_N"))
	want := map[int]bool{}
	for _, f := range strings.Split(os.Getenv("RP_FRAMES"), ",") {
		v, _ := strconv.Atoi(f)
		want[v] = true
	}
	btn := func(name string) uint32 {
		for i, b := range c.Info().Buttons {
			if b == name {
				return 1 << i
			}
		}
		return 0
	}
	var audio []float32
	for f := 1; f <= n; f++ {
		var bits uint32
		if f%900 >= 121 && f%900 < 127 {
			bits |= btn("Coin")
		}
		if f%900 >= 201 && f%900 < 207 {
			bits |= btn("Start (1P)")
		}
		if s := os.Getenv("RP_HOLD"); s != "" && f > 260 {
			for _, b := range strings.Split(s, ",") {
				bits |= btn(b)
			}
		}
		// RP_SCRIPT "a-b:Button;...": held in frames a+1..b+1 (MAME's dump.lua
		// sets inputs at the end of frame a)
		for _, s := range strings.Split(os.Getenv("RP_SCRIPT"), ";") {
			var a, z int
			var name string
			if i := strings.IndexByte(s, ':'); i > 0 {
				fmt.Sscanf(s[:i], "%d-%d", &a, &z)
				name = s[i+1:]
			}
			if name != "" && f > a && f <= z+1 {
				bits |= btn(name)
			}
		}
		if os.Getenv("RP_AUTO") != "" && f > 201 { // ad2.lua's play
			bits |= btn("Left")
			if (f-1)%8 < 2 {
				bits |= btn("Fire")
			}
		}
		c.SetInput(0, bits)
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
		if b, ok := c.Board().(*astdelux); ok && want[f] {
			x0, x1, y0, y1 := float32(9999), float32(-9999), float32(9999), float32(-9999)
			for _, l := range b.lines {
				x0, x1 = min(x0, l.x0, l.x1), max(x1, l.x0, l.x1)
				y0, y1 = min(y0, l.y0, l.y1), max(y1, l.y0, l.y1)
			}
			t.Logf("frame %d: %d lines, x %.0f-%.0f y %.0f-%.0f", f, len(b.lines), x0, x1, y0, y1)
		}
		if want[f] {
			writePNG(t, fmt.Sprintf("%s/%s-%05d.png", devOut, id, f), c.Screen(), c.Width(), c.Height())
			if b, ok := c.Board().(*centiped); ok {
				os.WriteFile(fmt.Sprintf("%s/%s-%05d.ram", devOut, id, f), append(b.ram[:], b.vram[:]...), 0o644)
			}
			if b, ok := c.Board().(*astdelux); ok {
				var m []byte
				for a := 0; a < 0x400; a++ {
					m = append(m, b.ram[b.ramAt(a)])
				}
				os.WriteFile(fmt.Sprintf("%s/%s-%05d.ram", devOut, id, f), append(m, b.vram[:]...), 0o644)
			}
		}
	}
	writeWAV(t, fmt.Sprintf("%s/%s.wav", devOut, id), audio, 48000)
}

func writeWAV(t testing.TB, path string, s []float32, rate int) {
	b := make([]byte, 44+2*len(s))
	copy(b, "RIFF")
	le := func(o int, v uint32, n int) {
		for i := 0; i < n; i++ {
			b[o+i] = byte(v >> (8 * i))
		}
	}
	le(4, uint32(36+2*len(s)), 4)
	copy(b[8:], "WAVEfmt ")
	le(16, 16, 4)
	le(20, 1, 2)
	le(22, 1, 2)
	le(24, uint32(rate), 4)
	le(28, uint32(rate*2), 4)
	le(32, 2, 2)
	le(34, 16, 2)
	copy(b[36:], "data")
	le(40, uint32(2*len(s)), 4)
	for i, v := range s {
		x := int(v * 32767)
		x = max(-32768, min(32767, x))
		le(44+2*i, uint32(int16(x)), 2)
	}
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// TestDevCentipedeIRQ logs where the IRQ handler starts.
func TestDevCentipedeIRQ(t *testing.T) {
	devOnly(t)
	c := openSet(t, "centiped")
	b := c.Board().(*centiped)
	for f := 0; f < 12; f++ {
		t.Logf("frame %d PC=%04x P=%02x 8a=%02x", f, b.cpu.PC, b.cpu.P, b.ram[0x8a])
		b.frameCyc = b.cpu.Cycles
		for line := 0; line < cnLines; line++ {
			if line&63 == 48 {
				b.cpu.IRQLine = true
			}
			end := b.frameCyc + int64(line+1)*cnFrameCyc/cnLines
			for b.cpu.Cycles < end {
				b.cpu.Step()
				if b.cpu.PC == 0x38c0 {
					t.Logf("frame %d IRQ at line %d (asserted at %d) I=%v", f, b.line(), line, b.cpu.P&4 != 0)
				}
				if b.cpu.PC == 0x38dc {
					t.Logf("  vblank path")
				}
			}
		}
	}
}

// TestDevCentipedeSheet draws all sprite pictures upright, 8 to a row.
func TestDevCentipedeSheet(t *testing.T) {
	devOnly(t)
	c := openSet(t, "centiped")
	b := c.Board().(*centiped)
	const cols = 8
	w, h := cols*18, 16*10
	pix := make([]uint32, w*h)
	pal := []uint32{0x202020, 0xff0000, 0x00ff00, 0xffffff}
	for s := 0; s < 128; s++ {
		ox, oy := (s%cols)*18, (s/cols)*10
		for y := 0; y < 16; y++ {
			for x := 0; x < 8; x++ {
				pix[(oy+7-x)*w+ox+y] = pal[b.spr[s*128+y*8+x]]
			}
		}
	}
	writePNG(t, devOut+"/sheet.png", pix, w, h)
}
