package dkong3

import (
	"bytes"
	"math"
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// synthetic: a made-up set. The main program enables the frame interrupt,
// sends a command to each sound processor, sets up the DMA chip to copy
// 0x1a0 bytes from 6900 to the sprite memory and starts it; its interrupt
// handler counts frames at 6000. Sound processor A starts a square wave and
// keeps copying its latch at $4016 to $00; B copies its latch to $01. Every
// tile is solid colour 3, which the colour PROMs make red.
func synthetic() ([]string, []int, [][]byte) {
	main := make([]byte, 0x2000)
	dmaProg := []byte{0x7d, 0x00, 0x69, 0x9f, 0x01, 0x14, 0x10, 0xad, 0x00, 0x70, 0x8a, 0xcf, 0x87}
	code := []byte{
		0x31, 0x00, 0x6c, // LD SP,6C00
		0x3e, 0x01, 0x32, 0x84, 0x7e, // LD A,1; LD (7E84),A  frame interrupt on
		0x3e, 0x42, 0x32, 0x00, 0x7d, // LD A,42; LD (7D00),A  to B
		0x3e, 0x17, 0x32, 0x00, 0x7c, // LD A,17; LD (7C00),A  to A
		0x3e, 0x55, 0x32, 0x00, 0x69, // LD A,55; LD (6900),A
		0x3e, 0x66, 0x32, 0x9f, 0x6a, // LD A,66; LD (6A9F),A
		0x21, 0x00, 0x01, // LD HL,0100
		0x01, 0x00, 0x0d, // LD BC,0D00
		0xed, 0xb3, // OTIR
		0x3e, 0x01, 0x32, 0x85, 0x7e, // LD A,1; LD (7E85),A  DMA ready
		0xaf, 0x32, 0x85, 0x7e, // XOR A; LD (7E85),A
		0x18, 0xfe, // JR $
	}
	copy(main, code)
	copy(main[0x100:], dmaProg)
	copy(main[0x66:], []byte{
		0xf5, 0x3a, 0x00, 0x60, 0x3c, 0x32, 0x00, 0x60, 0xf1, // PUSH AF; LD A,(6000); INC A; LD (6000),A; POP AF
		0xed, 0x45, // RETN
	})
	sound := func(a bool) []byte {
		rom := make([]byte, 0x2000)
		var c []byte
		zp := byte(1)
		if a {
			zp = 0
			c = append(c, 0xa9, 0x0f, 0x8d, 0x15, 0x40, // LDA #0F; STA $4015
				0xa9, 0xbf, 0x8d, 0x00, 0x40, // LDA #BF; STA $4000
				0xa9, 0xfd, 0x8d, 0x02, 0x40, // LDA #FD; STA $4002
				0xa9, 0x00, 0x8d, 0x03, 0x40) // LDA #0; STA $4003
		}
		loop := 0xe000 + len(c)
		c = append(c, 0xad, 0x16, 0x40, 0x85, zp, 0x4c, byte(loop), byte(loop>>8)) // LDA $4016; STA zp; JMP loop
		copy(rom, c)
		rom[0x1ffa], rom[0x1ffb] = 0x00, 0xf0 // NMI: an RTI at F000
		rom[0x1000] = 0x40
		rom[0x1ffc], rom[0x1ffd] = 0x00, 0xe0
		return rom
	}
	fill := func(n int, v byte) []byte { return bytes.Repeat([]byte{v}, n) }
	pal1d, pal1c := fill(0x200, 0xff), fill(0x200, 0x0f)
	pal1d[3] = 0x0f // colour 3: red 15, green 0 (inverted)
	names := []string{"dk3c.7b", "dk3c.7c", "dk3c.7d", "dk3c.7e", "dk3c.5l", "dk3c.6h", "dk3v.3n", "dk3v.3p",
		"dk3v.7c", "dk3v.7d", "dk3v.7e", "dk3v.7f", "dkc1-c.1d", "dkc1-c.1c", "dkc1-v.2n"}
	data := [][]byte{main, fill(0x2000, 0), fill(0x2000, 0), fill(0x2000, 0), sound(true), sound(false),
		fill(0x1000, 0xff), fill(0x1000, 0xff), fill(0x1000, 0), fill(0x1000, 0), fill(0x1000, 0), fill(0x1000, 0),
		pal1d, pal1c, fill(0x100, 0)}
	sizes := make([]int, len(data))
	for i, d := range data {
		sizes[i] = len(d)
	}
	return names, sizes, data
}

func newSynthetic(t *testing.T) (*arcade.Core, *board) {
	t.Helper()
	names, sizes, data := synthetic()
	c, err := arcade.NewFromFiles(names, sizes, data, "dkong3.zip", core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	b, ok := c.Board().(*board)
	if !ok {
		t.Fatal("not the Donkey Kong 3 board")
	}
	return c, b
}

// TestSynthetic: the set runs without the address-decoder PROM; the
// interrupt, latches, DMA, picture and sound work; states and Reset repeat.
func TestSynthetic(t *testing.T) {
	c, b := newSynthetic(t)
	var audio []float32
	for f := 0; f < 60; f++ {
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
	}
	if n := b.ram[0]; n < 55 || n > 60 {
		t.Errorf("%d frame interrupts in 60 frames", n)
	}
	if b.snd[0].ram[0] != 0x17 || b.snd[1].ram[1] != 0x42 {
		t.Errorf("sound latches read %02x %02x", b.snd[0].ram[0], b.snd[1].ram[1])
	}
	if b.spriteRam[0] != 0x55 || b.spriteRam[0x19f] != 0x66 || b.spriteRam[0x1a0] != 0 {
		t.Errorf("DMA copy: %02x %02x %02x", b.spriteRam[0], b.spriteRam[0x19f], b.spriteRam[0x1a0])
	}
	px := b.screen[100*scrW+100]
	if r, g, bl := px>>16&255, px>>8&255, px&255; r < 200 || g > 20 || bl > 20 {
		t.Errorf("tile colour %06x, want red", px)
	}
	if len(audio) < 60*780 || len(audio) > 60*800 {
		t.Errorf("%d samples in 60 frames", len(audio))
	}
	rms := 0.0
	for _, v := range audio[len(audio)/2:] {
		rms += float64(v) * float64(v)
	}
	if rms = math.Sqrt(rms / float64(len(audio)/2)); rms < 0.02 || rms > 0.06 {
		t.Errorf("square wave rms %g", rms)
	}
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	run := func() ([]uint32, []float32) {
		var a []float32
		for f := 0; f < 30; f++ {
			c.RunFrame()
			a = append(a, c.TakeAudio()...)
		}
		return append([]uint32(nil), c.Screen()...), a
	}
	p1, a1 := run()
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	p2, a2 := run()
	if !equal(p1, p2) || len(a1) != len(a2) || b.ram[0] == 0 {
		t.Error("a loaded state plays differently")
	}
	for i := range a1 {
		if a1[i] != a2[i] {
			t.Fatalf("a loaded state sounds different at sample %d", i)
		}
	}
	if err := c.LoadState(st[:len(st)-3]); err == nil {
		t.Error("a cut-off state loaded")
	}
	c.Reset()
	r1, s1 := run()
	c.Reset()
	r2, s2 := run()
	if !equal(r1, r2) || len(s1) != len(s2) {
		t.Error("Reset doesn't start the same way twice")
	}
}

// TestDips: the switches as the game reads them.
func TestDips(t *testing.T) {
	_, b := newSynthetic(t)
	b.dips["coins"], b.dips["service"], b.dips["cabinet"] = 6, 1, 1
	b.dips["lives"], b.dips["bonus"], b.dips["bonus2"], b.dips["difficulty"] = 2, 3, 1, 2
	if v := b.read(0x7d00); v != 0xc6 {
		t.Errorf("7d00 = %02x", v)
	}
	if v := b.read(0x7d80); v != 0x9e {
		t.Errorf("7d80 = %02x", v)
	}
	b.p1 = arcade.Pad{"Right": true, "Fire": true, "Start (2P)": true, "Coin": true}
	if v := b.read(0x7c00); v != 0x51 {
		t.Errorf("7c00 = %02x", v)
	}
	if v := b.read(0x7c80); v != 0x20 {
		t.Errorf("7c80 = %02x", v)
	}
}

func equal(a, b []uint32) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestReal plays the user's own set (skipped when it isn't there): a coin, a
// game started and played; the picture and sound must be alive, a state
// must play on exactly, and Reset must start the same way.
func TestReal(t *testing.T) {
	data, err := os.ReadFile("/mnt/project-files/roms-private/arcade-batch2/dkong3.zip")
	if err != nil {
		t.Skip("no dkong3.zip")
	}
	cc, err := arcade.New(data, core.Options{Name: "dkong3.zip", SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	c := cc.(*arcade.Core)
	idx := map[string]int{}
	for i, b := range c.Info().Buttons {
		idx[b] = i
	}
	s := uint32(7)
	input := func(f int) uint32 {
		var bits uint32
		if m := f % 900; m >= 120 && m < 126 {
			bits |= 1 << idx["Coin"]
		}
		if m := f % 900; m >= 200 && m < 206 {
			bits |= 1 << idx["Start (1P)"]
		}
		if f > 260 {
			s = s*1103515245 + 12345
			for i, b := range []string{"Up", "Down", "Left", "Right", "Fire"} {
				if (s>>(16+i))&3 == 0 {
					bits |= 1 << idx[b]
				}
			}
		}
		return bits
	}
	run := func(from, n int) ([]uint32, []float32) {
		var audio []float32
		for f := from; f < from+n; f++ {
			c.SetInput(0, input(f))
			c.RunFrame()
			audio = append(audio, c.TakeAudio()...)
		}
		return append([]uint32(nil), c.Screen()...), audio
	}
	pic, audio := run(1, 1200)
	rms := 0.0
	for _, v := range audio {
		rms += float64(v) * float64(v)
	}
	if rms = math.Sqrt(rms / float64(len(audio))); rms < 0.005 {
		t.Errorf("sound rms %g", rms)
	}
	colours := map[uint32]bool{}
	for _, px := range pic {
		colours[px] = true
	}
	if len(colours) < 8 {
		t.Errorf("%d colours on screen", len(colours))
	}
	b := c.Board().(*board)
	if b.ram[0x05] == 0 { // the game's "playing" flag (6005)
		t.Error("no game started")
	}
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	saved := s
	a, aa := run(1201, 120)
	s = saved
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	if b2, ba := run(1201, 120); !equal(a, b2) || len(aa) != len(ba) {
		t.Error("a loaded state plays differently")
	}
	c.Reset()
	r1, _ := run(1, 400)
	c.Reset()
	r2, _ := run(1, 400)
	if !equal(r1, r2) {
		t.Error("Reset doesn't start the same way twice")
	}
}
