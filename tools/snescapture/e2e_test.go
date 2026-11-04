package main

import (
	"encoding/json"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"retropixels/core"
	"retropixels/cores/snes"
)

// TestSwapEndToEnd: the whole SNES Debugger path on the test programs. The
// Go core runs each one with the tracer and captures it; the Rust tool
// (tools/snes-debugger) swaps BG1 for a picture; the patched ROM then runs
// and must show exactly the tool's preview. "direct" and "table" need more
// tiles than the ROM had, so they also check moving the tiles to free space
// and repointing (an immediate load and a pointer table).
func TestSwapEndToEnd(t *testing.T) {
	cargo, err := exec.LookPath("cargo")
	if err != nil {
		t.Skip("cargo not installed")
	}
	if testing.Short() {
		t.Skip("builds the Rust tool")
	}
	root, _ := filepath.Abs(filepath.Join("..", ".."))
	tmp := t.TempDir()
	build := exec.Command(cargo, "build", "--no-default-features", "--manifest-path", filepath.Join(root, "tools", "snes-debugger", "Cargo.toml"), "--target-dir", filepath.Join(root, "tools", "snes-debugger", "target"))
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("cargo build: %v\n%s", err, out)
	}
	tool := filepath.Join(root, "tools", "snes-debugger", "target", "debug", "retropixels-snes-debugger")
	if _, err := os.Stat(tool); err != nil {
		tool += ".exe"
	}
	// a picture: sky, sun, hills (more different tiles than the test programs have)
	pic := image.NewNRGBA(image.Rect(0, 0, 256, 256))
	for y := 0; y < 256; y++ {
		for x := 0; x < 256; x++ {
			c := color.NRGBA{uint8(40 + 100*y/255), uint8(20 + 60*math.Pow(math.Sin(float64(x)/40), 2)), uint8(120 + 120*(255-y)/255), 255}
			if dx, dy := x-190, y-70; dx*dx+dy*dy < 40*40 {
				c = color.NRGBA{255, 220, 60, 255}
			}
			if y > 180+int(20*math.Sin(float64(x)/23)) {
				c = color.NRGBA{40, uint8(100 + x/4), 40, 255}
			}
			pic.SetNRGBA(x, y, c)
		}
	}
	picPath := filepath.Join(tmp, "pic.png")
	f, _ := os.Create(picPath)
	png.Encode(f, pic)
	f.Close()

	for _, k := range []string{"direct", "table", "wram"} {
		t.Run(k, func(t *testing.T) {
			rom, err := os.ReadFile(filepath.Join(root, "tools", "snes-debugger", "testroms", "bgtest-"+k+".sfc"))
			if err != nil {
				t.Fatal(err)
			}
			dir := filepath.Join(tmp, k)
			capDir := filepath.Join(dir, "cap")
			s := run(t, rom, 10)
			cp, err := s.DebugCapture()
			if err != nil {
				t.Fatal(err)
			}
			os.MkdirAll(capDir, 0o755)
			meta, _ := json.Marshal(cp.Meta)
			os.WriteFile(filepath.Join(capDir, "capture.json"), meta, 0o644)
			for n, b := range cp.Files {
				os.WriteFile(filepath.Join(capDir, n), b, 0o644)
			}
			os.WriteFile(filepath.Join(capDir, "rom.sfc"), rom, 0o644)
			patched, prev := filepath.Join(dir, "patched.sfc"), filepath.Join(dir, "preview.png")
			cmd := exec.Command(tool, "swap", capDir, "--bg", "1", "--image", picPath, "--out", patched, "--preview", prev)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("swap: %v\n%s", err, out)
			}
			t.Logf("%s", out)
			pr, _ := os.ReadFile(patched)
			got := run(t, pr, 10)
			pf, err := os.Open(prev)
			if err != nil {
				t.Fatal(err)
			}
			want, err := png.Decode(pf)
			pf.Close()
			if err != nil {
				t.Fatal(err)
			}
			screen, w := got.Screen(), got.Width()
			bad := 0
			for y := 0; y < got.Height() && y < 224; y++ {
				for x := 0; x < 256; x++ {
					// the SNES shows the background's line 1 on the screen's first line (scroll 0)
					r, g, b, a := want.At(x, y+1).RGBA()
					exp := uint32(0) // transparent: the backdrop (black here)
					if a != 0 {
						exp = (r>>8)<<16 | (g>>8)<<8 | b>>8
					}
					if screen[y*w+x]&0xffffff != exp {
						if bad < 5 {
							t.Logf("(%d,%d): game %06x, preview %06x", x, y, screen[y*w+x]&0xffffff, exp)
						}
						bad++
					}
				}
			}
			if bad > 0 {
				t.Errorf("%d pixels differ between the patched game and the preview", bad)
			}
			// and it changed: the picture's sun is there
			if c := screen[69*w+190] & 0xffffff; c>>16 < 200 {
				t.Errorf("no sun at (190,70): %06x", c)
			}
		})
	}
}

func run(t *testing.T, rom []byte, frames int) *snes.SNES {
	c, err := snes.New(rom, core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	s := c.(*snes.SNES)
	s.EnableTrace()
	for i := 0; i < frames; i++ {
		s.RunFrame()
		s.TakeAudio()
	}
	return s
}
