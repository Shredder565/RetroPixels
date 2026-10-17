package berzerk

import (
	"bytes"
	"hash/crc32"
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// The magic circuit's results as MAME's board gives them (probed from Lua):
// each function writing $AA over $CC with no shift.
func TestMagicFunctions(t *testing.T) {
	want := []uint8{0xaa, 0xee, 0xbb, 0xff, 0x88, 0xcc, 0x99, 0xdd, 0x22, 0x66, 0x33, 0x77, 0x00, 0x44, 0x11, 0x55}
	b := &board{}
	for f, w := range want {
		b.magicCtrl = uint8(f << 4)
		b.vram[0x1a00] = 0xcc
		b.magicWrite(0x1a00, 0xaa)
		if got := b.vram[0x1a00]; got != w {
			t.Errorf("function %d: got %02x, want %02x", f, got, w)
		}
		if !b.collision {
			t.Errorf("function %d: no collision", f)
		}
	}
}

// Shifts and flips: the bytes 00 81 C3 00 00 written through function 15
// (NOT A) with each shift, as MAME's board gives them.
func TestMagicShift(t *testing.T) {
	want := map[int]string{
		0: "ff7e3cffff", 1: "ffbf1e7fff", 2: "ffdf8f3fff", 3: "ffefc79fff", 4: "fff7e3cfff", 5: "fffbf1e7ff", 6: "fffdf8f3ff", 7: "fffefc79ff",
		8: "ff7e3cffff", 9: "fffd78feff", 10: "fffbf1fcff", 11: "fff7e3f9ff", 12: "ffefc7f3ff", 13: "ffdf8fe7ff", 14: "ffbf1fcfff", 15: "ff7f3f9eff",
	}
	b := &board{}
	for ctl, w := range want {
		b.magicCtrl = uint8(0xf0 | ctl)
		var got []byte
		for i, v := range []uint8{0, 0x81, 0xc3, 0, 0} {
			b.vram[0x1a10+i] = 0
			b.magicWrite(0x1a10+i, v)
			got = append(got, b.vram[0x1a10+i])
		}
		if s := hexs(got); s != w {
			t.Errorf("control %02x: got %s, want %s", 0xf0|ctl, s, w)
		}
	}
}

func hexs(b []byte) string {
	const d = "0123456789abcdef"
	s := ""
	for _, v := range b {
		s += string(d[v>>4]) + string(d[v&15])
	}
	return s
}

// The 6840's waves: 16-bit mode toggles every latch+1 clocks; dual 8-bit
// mode is high for the last LSB+1 of (MSB+1)*(LSB+1) clocks.
func TestPTM(t *testing.T) {
	var p ptm
	p.reset()
	p.write(1, 1)    // CR2, next CR write goes to CR1
	p.write(0, 0x82) // CR1: output on, E clock, 16-bit, reload on latch writes (leaves reset)
	p.write(2, 0)
	p.write(3, 9) // latch 9: 10 clocks a half wave
	high, edges := 0, 0
	last := p.level(0)
	for i := 0; i < 2000; i++ {
		p.clock(0)
		if p.level(0) {
			high++
		}
		if p.level(0) != last {
			edges++
			last = p.level(0)
		}
	}
	if edges != 200 || high != 1000 {
		t.Errorf("16-bit: %d edges, %d high clocks", edges, high)
	}
	p.write(0, 0x86) // dual 8-bit
	p.write(2, 3)
	p.write(3, 0x10) // (3+1)*(16+1) = 68 clocks, 17 high
	high = 0
	for i := 0; i < 68*30; i++ {
		p.clock(0)
		if p.level(0) {
			high++
		}
	}
	if high != 17*30 {
		t.Errorf("dual 8-bit: %d high clocks, want %d", high, 17*30)
	}
}

// A made-up set (same names and sizes, a program of random bytes) loads,
// runs, saves and restores its state, and resets, without trouble.
func TestSyntheticSet(t *testing.T) {
	var names []string
	var sizes []int
	var data [][]byte
	seed := uint32(1)
	for _, f := range []string{"1c-0", "1d-1", "3d-2", "5d-3", "6d-4", "5c-5"} {
		b := make([]byte, 0x800)
		for i := range b {
			seed = seed*1103515245 + 12345
			b[i] = byte(seed >> 16)
		}
		names, sizes, data = append(names, f), append(sizes, len(b)), append(data, b)
	}
	c, err := arcade.NewFromFiles(names, sizes, data, "berzerk.zip", core.Options{SampleRate: 44100})
	if err != nil {
		t.Fatal(err)
	}
	if c.GameID() != "berzerk" || c.Width() != 256 || c.Height() != 224 {
		t.Fatalf("got %s %dx%d", c.GameID(), c.Width(), c.Height())
	}
	for i := 0; i < 30; i++ {
		c.SetInput(0, uint32(i))
		c.RunFrame()
	}
	secs := 30 / FPS
	if n, want := len(c.TakeAudio()), int(secs*44100); n < want-50 || n > want+50 {
		t.Errorf("%d samples for 30 frames", n)
	}
	roundTrip(t, c, 20)
	c.Reset()
	c.RunFrame()
}

// roundTrip: a saved state, restored, runs on exactly as the original did.
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

const userSet = "/mnt/project-files/roms-private/arcade-new/berzerk.zip"

// The user's own set (skipped without it): boots, takes a coin, starts a
// game, and draws the maze in the right colours.
func TestRealSet(t *testing.T) {
	zip, err := os.ReadFile(userSet)
	if err != nil {
		t.Skip("the Berzerk set isn't here")
	}
	cc, err := arcade.New(zip, core.Options{SampleRate: 48000, Name: "berzerk"})
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
	press := map[int]uint32{601: btn("Coin"), 701: btn("Start (1P)")}
	for f := 1; f <= 900; f++ {
		in := uint32(0)
		for at, b := range press {
			if f >= at && f < at+6 {
				in |= b
			}
		}
		c.SetInput(0, in)
		c.RunFrame()
		c.TakeAudio()
	}
	// a game is on: the maze's blue walls are drawn
	blue := 0
	for _, p := range c.Screen() {
		if p == arcade.RGB(0, 0, 108) {
			blue++
		}
	}
	if blue < 1000 {
		t.Errorf("no maze on the screen (%d wall pixels)", blue)
	}
	if len(c.SRAM()) != 0x400 {
		t.Errorf("SRAM is %d bytes", len(c.SRAM()))
	}
	roundTrip(t, c, 60)
}
