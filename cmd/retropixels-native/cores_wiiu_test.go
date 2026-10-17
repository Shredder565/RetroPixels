package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"

	"retropixels/core"
)

// The Wii U: RetroPixels' own OSScreen test program (wiiu/testdata/progs/screen.c)
// opens as a .rpx, draws on both screens and ends after two presses of A.
func TestWiiUWiring(t *testing.T) {
	rpx, err := os.ReadFile("../../wiiu/testdata/screen.rpx")
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(t.TempDir(), "Screen.rpx")
	os.WriteFile(p, rpx, 0o644)
	g, err := openGame(p)
	if err != nil || g.System != "wiiu" {
		t.Fatalf("%+v %v", g, err)
	}
	if !slices.Contains(availableExts(), ".rpx") {
		t.Error(".rpx isn't listed")
	}
	c, err := coreMakers["wiiu"](g.ROM, core.Options{SampleRate: 48000, Name: "Screen.rpx"})
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(c.Info().Buttons, systemButtons["wiiu"]) || c.Info().Aspect != 16.0/9 {
		t.Fatalf("info %+v", c.Info())
	}
	for name := range defaultMaps["wiiu"] {
		if !slices.Contains(systemButtons["wiiu"], name) {
			t.Errorf("default control for %q, which the core hasn't got", name)
		}
	}
	for i := 0; i < 10; i++ {
		c.RunFrame()
	}
	if c.Width() != 1280 || c.Height() != 720 || len(c.TakeAudio()) == 0 {
		t.Fatalf("frame %dx%d", c.Width(), c.Height())
	}
	if c.Screen()[640*1280+10] == 0 {
		t.Error("the TV is black")
	}

	// Xbox B = the GamePad's A (as on the GamePad's diamond)
	m := loadMap("wiiu", nil)
	buttons := systemButtons["wiiu"]
	in := newInputState()
	pad := &PadState{Connected: true}
	pad.Buttons[padB] = true
	a := in.bits(0, buttons, m, nil, pad, true)
	if a != 1<<slices.Index(buttons, "A") {
		t.Fatalf("Xbox B: bits %b", a)
	}
	for i := 0; i < 2; i++ {
		c.SetInput(0, a)
		c.RunFrame()
		c.SetInput(0, 0)
		c.RunFrame()
	}
	for i := 0; i < 5; i++ {
		c.RunFrame()
	}
	// the "TV / GamePad" button shows the GamePad
	sw := uint32(1) << slices.Index(buttons, "TV / GamePad")
	c.SetInput(0, sw)
	c.RunFrame()
	if c.Width() != 854 || c.Height() != 480 {
		t.Errorf("GamePad picture %dx%d", c.Width(), c.Height())
	}
	if _, err := c.SaveState(); err == nil {
		t.Error("save states")
	}
}
