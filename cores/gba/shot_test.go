package gba

import (
	"image"
	imgcolor "image/color"
	"image/png"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"retropixels/core"
)

// TestShot saves a picture of a game after some frames, for looking at by
// hand: GBA_SHOT=rom.gba:frames:out.png[:buttons].
func TestShot(t *testing.T) {
	spec := os.Getenv("GBA_SHOT")
	if spec == "" {
		t.Skip("GBA_SHOT not set")
	}
	p := strings.Split(spec, ":")
	rom, err := os.ReadFile(p[0])
	if err != nil {
		t.Fatal(err)
	}
	frames, _ := strconv.Atoi(p[1])
	var bits uint64
	if len(p) > 3 {
		bits, _ = strconv.ParseUint(p[3], 0, 32)
	}
	g, err := newGBA(rom, core.Options{SampleRate: 48000}, time.Now)
	if err != nil {
		t.Fatal(err)
	}
	for f := 0; f < frames; f++ {
		g.SetInput(0, uint32(bits))
		g.RunFrame()
		g.TakeAudio()
	}
	img := image.NewRGBA(image.Rect(0, 0, 240, 160))
	for i, c := range g.Screen() {
		img.Set(i%240, i/240, imgcolor.RGBA{uint8(c >> 16), uint8(c >> 8), uint8(c), 255})
	}
	f, err := os.Create(p[2])
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	png.Encode(f, img)
	if s := g.SRAM(); s != nil {
		t.Logf("battery save %d bytes", len(s))
	}
}
