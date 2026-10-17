package vsnes

import (
	"bytes"
	"hash/crc32"
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// syntheticPRG: a program that sets the backdrop to RP2C04-0002 colour
// $0C (NES colour $20, white) and the first background colour to $05 (NES
// $25, pink), then waits.
func syntheticPRG() []byte {
	prg := make([]byte, 0x20000)
	code := []byte{
		0xa9, 0x3f, 0x8d, 0x06, 0x20, // LDA #$3F; STA $2006
		0xa9, 0x00, 0x8d, 0x06, 0x20, // LDA #$00; STA $2006
		0xa9, 0x0c, 0x8d, 0x07, 0x20, // LDA #$0C; STA $2007
		0xa9, 0x05, 0x8d, 0x07, 0x20, // LDA #$05; STA $2007
		0xad, 0x17, 0x40, 0x85, 0x10, // LDA $4017; STA $10
		0x4c, 0x19, 0xc0, // JMP $C019
	}
	copy(prg[0x1c000:], code)
	prg[0x1fffc], prg[0x1fffd] = 0x00, 0xc0
	return prg
}

func newSynthetic(t *testing.T, dips map[string]int) *arcade.Core {
	t.Helper()
	c, err := arcade.NewFromFiles([]string{"mds-cv.prg"}, []int{0x20000}, [][]byte{syntheticPRG()}, "cstlevna.zip",
		core.Options{SampleRate: 44100, Dips: dips})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestSyntheticSet(t *testing.T) {
	c := newSynthetic(t, map[string]int{"difficulty": 1, "coinage": 7})
	if c.GameID() != "cstlevna" || c.Width() != 256 || c.Height() != 240 {
		t.Fatalf("got %s %dx%d", c.GameID(), c.Width(), c.Height())
	}
	for i := 0; i < 10; i++ {
		c.RunFrame()
	}
	// the backdrop shows the RGB PPU's colour, not the NES's
	if p := c.Screen()[120*256+128]; p != arcade.RGB(255, 255, 255) {
		t.Errorf("backdrop %06x, want white", p)
	}
	// $4017: serial bit 0, then DIP switches 3-8 on bits 2-7
	b := c.Board().(*board)
	if v := b.port(0x4017, 1); v != 1|0x04|0x40 {
		t.Errorf("$4017 = %02x", v)
	}
	c.TakeAudio()
	n := 0
	for i := 0; i < 60; i++ {
		c.RunFrame()
		n += len(c.TakeAudio())
	}
	if n < 43000 || n > 45000 { // 60 frames: about a second
		t.Errorf("%d samples for 60 frames", n)
	}
	roundTrip(t, c, 10)
	c.Reset()
	c.RunFrame()
}

// The coin, service and switch bits of $4016.
func TestPort4016(t *testing.T) {
	c := newSynthetic(t, map[string]int{"coinage": 2})
	b := c.Board().(*board)
	b.SetInput([]arcade.Pad{{"Coin": true, "Service": true}, {"Coin": true}})
	if v := b.port(0x4016, 0); v != 0x04|0x10|0x20|0x40 {
		t.Errorf("$4016 = %02x", v)
	}
}

// The palette: RP2C04-0002 colour n is NES colour order2C04_0002[n] on the
// RGB PPU's 0-7 scale; emphasis turns channels fully on.
func TestPalette(t *testing.T) {
	p := buildPalette(palette2C04_0002())
	if p[0x0c] != arcade.RGB(255, 255, 255) || p[0x32] != arcade.RGB(0, 255, 0) || p[0x10] != 0 {
		t.Errorf("colours %06x %06x %06x", p[0x0c], p[0x32], p[0x10])
	}
	if p[1*64+0x10] != arcade.RGB(255, 0, 0) {
		t.Errorf("red emphasis on black: %06x", p[64+0x10])
	}
	if promOK([]byte{8}) || !promOK([]byte{7, 0}) {
		t.Error("promOK")
	}
}

func roundTrip(t *testing.T, c *arcade.Core, frames int) {
	t.Helper()
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	run := func() (uint32, []byte) {
		for i := 0; i < frames; i++ {
			c.RunFrame()
		}
		px := make([]byte, 0, len(c.Screen())*4)
		for _, p := range c.Screen() {
			px = append(px, byte(p), byte(p>>8), byte(p>>16))
		}
		s, _ := c.SaveState()
		return crc32.ChecksumIEEE(px), s
	}
	c1, s1 := run()
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	c2, s2 := run()
	if c1 != c2 || !bytes.Equal(s1, s2) {
		t.Error("the restored state ran differently")
	}
	if err := c.LoadState(st[:len(st)-3]); err == nil {
		t.Error("a cut-short state was accepted")
	}
}

const userSet = "/mnt/project-files/roms-private/arcade-new/cstlevna.zip"

// The user's own set (skipped without it): boots past its ROM check to the
// title, takes a coin and starts the game at stage 1.
func TestRealSet(t *testing.T) {
	zip, err := os.ReadFile(userSet)
	if err != nil {
		t.Skip("the Vs. Castlevania set isn't here")
	}
	cc, err := arcade.New(zip, core.Options{SampleRate: 48000, Name: "cstlevna"})
	if err != nil {
		t.Fatal(err)
	}
	c := cc.(*arcade.Core)
	btn := func(name string) uint32 {
		for i, b := range c.Info().Buttons {
			if b == name {
				return 1 << i
			}
		}
		t.Fatalf("no button %s", name)
		return 0
	}
	// the same coin and start as the MAME comparison run that started a game
	press := map[int]uint32{3001: btn("Coin"), 3101: btn("Start (1P)")}
	var audio int
	for f := 1; f <= 3400; f++ {
		in := uint32(0)
		for at, b := range press {
			if f >= at && f < at+6 {
				in |= b
			}
		}
		c.SetInput(0, in)
		c.RunFrame()
		audio += len(c.TakeAudio())
	}
	// stage 1's night sky: the dark teal of the backdrop fills the top
	teal := 0
	for _, p := range c.Screen()[40*256 : 120*256] {
		if p == arcade.RGB(0, 73, 73) {
			teal++
		}
	}
	if teal < 5000 {
		t.Errorf("not at stage 1 (%d sky pixels)", teal)
	}
	roundTrip(t, c, 60)
}
