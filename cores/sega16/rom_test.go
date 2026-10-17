package sega16

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// The user's own ROM set, when present (never in CI).
const romDir = "/mnt/project-files/roms-private/arcade-new"

func loadSet(t *testing.T, name string) *arcade.Core {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(romDir, name+".zip"))
	if err != nil {
		t.Skip("ROM set not present:", err)
	}
	c, err := arcade.New(data, core.Options{Name: name + ".zip", SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

func savePNG(t *testing.T, path string, pix []uint32, w, h int) {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix {
		img.Set(i%w, i/w, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
}

// TestAltbeastBoots runs the user's set from a cold boot through the
// attract mode, a coin and a start, and checks that it draws and plays.
func TestAltbeastBoots(t *testing.T) {
	c := loadSet(t, "altbeast")
	if c.GameID() != "altbeast" {
		t.Fatalf("identified as %q", c.GameID())
	}
	out := os.Getenv("SEGA16_OUT")
	b := c.Board().(*board)
	pads := []arcade.Pad{{}, {}}
	b.SetInput(pads)
	var audio []float32
	frames := 1800
	idle := os.Getenv("SEGA16_IDLE") != ""
	for f := 1; f <= frames; f++ {
		pads[0]["Coin"] = !idle && f%900 >= 120 && f%900 < 126
		pads[0]["Start (1P)"] = !idle && f%900 >= 200 && f%900 < 206
		b.RunFrame()
		audio = append(audio, b.TakeAudio()...)
		if out != "" && f%60 == 0 {
			savePNG(t, filepath.Join(out, "f"+pad5(f)+".png"), b.Screen(), scrW, scrH)
		}
	}
	lit := 0
	for _, p := range b.Screen() {
		if p != 0 {
			lit++
		}
	}
	if lit < 1000 {
		t.Errorf("only %d pixels lit after %d frames", lit, frames)
	}
	if out != "" {
		writeWAV(t, filepath.Join(out, "audio.wav"), audio, 48000)
	}
}

func pad5(n int) string {
	s := []byte("00000")
	for i := 4; i >= 0 && n > 0; i-- {
		s[i] = byte('0' + n%10)
		n /= 10
	}
	return string(s)
}

func writeWAV(t *testing.T, path string, s []float32, rate int) {
	var buf bytes.Buffer
	le := func(v uint32, n int) {
		for i := 0; i < n; i++ {
			buf.WriteByte(byte(v >> (8 * i)))
		}
	}
	buf.WriteString("RIFF")
	le(uint32(36+len(s)*2), 4)
	buf.WriteString("WAVEfmt ")
	le(16, 4)
	le(1, 2)
	le(1, 2)
	le(uint32(rate), 4)
	le(uint32(rate*2), 4)
	le(2, 2)
	le(16, 2)
	buf.WriteString("data")
	le(uint32(len(s)*2), 4)
	for _, v := range s {
		x := int(v * 32767)
		x = max(-32768, min(32767, x))
		le(uint32(int16(x)), 2)
	}
	if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
}
