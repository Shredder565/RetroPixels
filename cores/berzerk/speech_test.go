package berzerk

import (
	"math"
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// testSpeechROM is a made-up speech ROM (no real speech): every word is one
// voiced syllable of 8 periods, of pseudo-random data.
func testSpeechROM() []byte {
	rom := make([]byte, 0x1000)
	x := uint32(7)
	for a := 0x400; a < 0x1000; a++ {
		x = (x*1103515245 + 12345) & 0x7fffffff
		rom[a] = uint8(x >> 16)
	}
	for w := 0; w < 64; w++ {
		a := 0x100 + 2*w
		rom[2*w], rom[2*w+1] = uint8(a>>4), uint8(a<<4)
		rom[a], rom[a+1] = uint8(0x40+w), 0xd8
	}
	return rom
}

// With speech ROMs the chip speaks the game's phrases; without them the
// port reads idle and the game plays on (TestRealSet).
func TestSpeechWithROMs(t *testing.T) {
	zip, err := os.ReadFile(userSet)
	if err != nil {
		t.Skip("the Berzerk set isn't here")
	}
	names, sizes, data, err := arcade.ReadZip(zip)
	if err != nil {
		t.Fatal(err)
	}
	vo := testSpeechROM()
	names = append(names, "berzerk_r_vo_1c.1c", "berzerk_r_vo_2c.2c")
	sizes = append(sizes, 0x800, 0x800)
	data = append(data, vo[:0x800], vo[0x800:])
	c, err := arcade.NewFromFiles(names, sizes, data, "berzerk.zip", core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	b := c.Board().(*board)
	if b.speech.chip == nil {
		t.Fatal("no speech chip with the speech ROMs")
	}
	btn := func(name string) uint32 {
		for i, n := range c.Info().Buttons {
			if n == name {
				return 1 << i
			}
		}
		t.Fatalf("no button %s", name)
		return 0
	}
	busyFrames, peak := 0, 0.0
	for f := 1; f <= 900; f++ {
		in := uint32(0)
		if f >= 601 && f < 607 {
			in = btn("Coin")
		}
		if f >= 701 && f < 707 {
			in = btn("Start (1P)")
		}
		c.SetInput(0, in)
		c.RunFrame()
		if b.speech.chip.Busy() {
			busyFrames++
		}
		for _, v := range c.TakeAudio() {
			peak = math.Max(peak, math.Abs(float64(v)))
		}
	}
	if busyFrames < 10 {
		t.Errorf("the chip spoke for only %d frames", busyFrames)
	}
	if peak < 0.2 {
		t.Errorf("no speech heard (peak %.3f)", peak)
	}
	roundTrip(t, c, 60)
}
