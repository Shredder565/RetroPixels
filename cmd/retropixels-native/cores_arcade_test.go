package main

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/core"
)

// invadersZip: a Space Invaders-shaped set whose program fills the screen.
func invadersZip(t *testing.T, names ...string) []byte {
	prog := make([]byte, 0x2000)
	// LXI H,2400; loop: MVI M,55; INX H; MOV A,H; CPI 40; JNZ loop; HLT
	copy(prog, []byte{0x21, 0x00, 0x24, 0x36, 0x55, 0x23, 0x7c, 0xfe, 0x40, 0xc2, 0x03, 0x00, 0x76})
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for i, n := range []string{"invaders.h", "invaders.g", "invaders.f", "invaders.e"} {
		if len(names) > i {
			n = names[i]
		}
		w, _ := zw.Create(n)
		w.Write(prog[i*0x800 : (i+1)*0x800])
	}
	zw.Close()
	return buf.Bytes()
}

func TestArcadeCore(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "invaders.zip")
	os.WriteFile(p, invadersZip(t), 0o644)
	g, err := openGame(p)
	if err != nil || g.System != "arcade" {
		t.Fatalf("openGame: %v %v", g, err)
	}
	g.Dips = map[string]int{"overlay": 0}
	c, err := coreMakers["arcade"](g.ROM, core.Options{SampleRate: 48000, Name: g.Name, Dips: g.Dips})
	if err != nil {
		t.Fatal(err)
	}
	// the monitor is on its side: the picture is turned upright
	if c.Width() != 224 || c.Height() != 256 || c.Info().Aspect != 0.75 || c.Info().Players != 2 {
		t.Fatalf("%dx%d aspect %v", c.Width(), c.Height(), c.Info().Aspect)
	}
	for i := 0; i < 12; i++ {
		c.RunFrame()
	}
	// black and white (the strips are off), every other pixel lit
	lit := 0
	for _, px := range c.Screen() {
		if px == 0xffffff {
			lit++
		}
	}
	if lit != 224*256/2 {
		t.Fatalf("%d pixels lit", lit)
	}
	m := loadMap("arcade", nil)
	if m["Coin"].Keys[0] != "5" || m["Start (1P)"].Pad[0] != 9 || len(m["Service"].Keys) != 1 {
		t.Fatalf("controls: %+v", m)
	}

	// an arcade set inside a .zip with another name, files in a folder
	p2 := filepath.Join(dir, "Space Invaders (1978).zip")
	os.WriteFile(p2, invadersZip(t, "sub/invaders.h", "sub/invaders.g", "sub/invaders.f", "sub/invaders.e"), 0o644)
	if g, err := openGame(p2); err != nil || g.System != "arcade" {
		t.Fatalf("other name: %v", err)
	}
	// a set missing a chip: the core says which
	p3 := filepath.Join(dir, "invaders (2).zip")
	os.WriteFile(p3, invadersZip(t, "invaders.h", "invaders.g", "invaders.f", "other.bin"), 0o644)
	g, err = openGame(p3)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := coreMakers["arcade"](g.ROM, core.Options{SampleRate: 48000, Name: g.Name}); err == nil || !strings.Contains(err.Error(), "invaders (2).zip is missing this chip file for Space Invaders: invaders.e.") {
		t.Fatalf("error: %v", err)
	}
	// rezip (for .7z sets) keeps the files
	z, err := rezip([]archiveEntry{{name: "a/invaders.h", size: 3, read: func() ([]byte, error) { return []byte{1, 2, 3}, nil }}})
	if err != nil {
		t.Fatal(err)
	}
	zr, _ := zip.NewReader(bytes.NewReader(z), int64(len(z)))
	if len(zr.File) != 1 || zr.File[0].Name != "a/invaders.h" {
		t.Fatal("rezip")
	}
}
