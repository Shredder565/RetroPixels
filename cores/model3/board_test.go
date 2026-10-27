package model3_test

import (
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
	_ "retropixels/cores/model3"
)

// TestArcadeBoard plays Star Wars Trilogy through the arcade framework, the
// way both apps open it. Set M3_ROMSET to the set's zip.
func TestArcadeBoard(t *testing.T) {
	zip := os.Getenv("M3_ROMSET")
	if zip == "" {
		t.Skip("M3_ROMSET not set")
	}
	data, err := os.ReadFile(zip)
	if err != nil {
		t.Fatal(err)
	}
	c, err := arcade.New(data, core.Options{SampleRate: 48000, Name: "swtrilgya"})
	if err != nil {
		t.Fatal(err)
	}
	if a, ok := c.(interface{ AnalogStick() bool }); !ok || !a.AnalogStick() {
		t.Fatal("no analog stick")
	}
	audio, lit := 0, 0
	for f := 0; f < 900; f++ {
		c.RunFrame()
		audio += len(c.TakeAudio())
	}
	for _, p := range c.Screen() {
		if p != 0 {
			lit++
		}
	}
	t.Logf("%dx%d, %d lit pixels, %d samples in 900 frames", c.Width(), c.Height(), lit, audio)
	if lit == 0 || audio < 900*48000/58 {
		t.Fatal("no picture or sound")
	}
	if len(c.SRAM()) == 0 {
		t.Fatal("no NVRAM")
	}
}
