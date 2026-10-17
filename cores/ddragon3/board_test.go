package ddragon3

import (
	"bytes"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// A synthetic Double Dragon 3-shaped set: a small 68000 program that counts
// its two interrupts, sets up a background tile, a foreground tile and a
// sprite, and sends a sound command; a Z80 program that plays an MSM6295
// phrase when told to and keys a YM2151 note; made-up graphics.

func put16(b []byte, at int, ws ...int) int {
	for _, w := range ws {
		b[at], b[at+1] = byte(w>>8), byte(w)
		at += 2
	}
	return at
}

const (
	cnt5 = 0x1c0010 // where the test program counts interrupts 5 and 6
	cnt6 = 0x1c0012
)

func testProgram() []byte {
	p := make([]byte, 0x80000)
	put16(p, 0, 0x001c, 0x4000, 0x0000, 0x0400) // stack, start
	put16(p, 0x74, 0x0000, 0x0600)              // level 5
	put16(p, 0x78, 0x0000, 0x0700)              // level 6
	at := 0x400
	mv := func(v, a int) { at = put16(p, at, 0x33fc, v, a>>16, a&0xffff) } // move.w #v,a
	mv(0x001f, 0x140000+0x211*2)                                           // background colour 1 pen 1: red
	mv(0x03e0, 0x140000+0x121*2)                                           // foreground colour 2 pen 1: green
	mv(0x7c00, 0x140000+0x031*2)                                           // sprite colour 3 pen 1: blue
	mv(0x1001, 0x082000)                                                   // background tile 1 at (0,0)
	mv(0x0002, 0x080084)                                                   // foreground (1,1): colour 2
	mv(0x0001, 0x080086)                                                   // tile 1
	mv(0x0084, 0x180000)                                                   // sprite 0: y
	mv(0x0001, 0x180002)                                                   // shown
	mv(0x0001, 0x180004)                                                   // tile 1
	mv(0x0003, 0x180008)                                                   // colour 3
	mv(0x0064, 0x18000a)                                                   // x 100
	mv(0x01f8, 0x0c0002)                                                   // y scrolls: -8
	mv(0x01f8, 0x0c0006)
	mv(0x005f, 0x100000)                                // sprites in front
	at = put16(p, at, 0x13fc, 0x0001, 0x0010, 0x0003)   // move.b #1,$100003: sound command
	at = put16(p, at, 0x46fc, 0x2000)                   // move #$2000,sr
	at = put16(p, at, 0x6000, (at-(at+2))&0xffff)       // bra *
	at = put16(p, 0x600, 0x5279, cnt5>>16, cnt5&0xffff) // addq.w #1,cnt5
	at = put16(p, at, 0x33c0, 0x0010, 0x0006)           // move.w d0,$100006
	put16(p, at, 0x4e73)                                // rte
	at = put16(p, 0x700, 0x5279, cnt6>>16, cnt6&0xffff) // addq.w #1,cnt6
	at = put16(p, at, 0x33c0, 0x0010, 0x0004)           // move.w d0,$100004
	put16(p, at, 0x4e73)                                // rte
	return p
}

func testSound() []byte {
	z := make([]byte, 0x10000)
	copy(z, []byte{
		0xf3,             // di
		0x31, 0x00, 0xc8, // ld sp,$c800
		0x3e, 0x28, 0x32, 0x00, 0xc8, // YM: register $28 (key code ch 0)
		0x3e, 0x4a, 0x32, 0x01, 0xc8, // = $4a
		0x3e, 0x08, 0x32, 0x00, 0xc8, // key on
		0x3e, 0x78, 0x32, 0x01, 0xc8,
		0x18, 0xfe, // jr $
	})
	// NMI: play phrase 1 on voice 0 when the command is 1
	copy(z[0x66:], []byte{
		0x3a, 0x00, 0xe0, // ld a,($e000)
		0xfe, 0x01, // cp 1
		0x20, 0x0a, // jr nz,done
		0x3e, 0x81, 0x32, 0x00, 0xd8, // phrase 1
		0x3e, 0x10, 0x32, 0x00, 0xd8, // voice 0
		0xed, 0x45, // done: retn
	})
	return z
}

func testOKI() []byte {
	o := make([]byte, 0x80000)
	o[8], o[9], o[10] = 0, 0x04, 0x00 // phrase 1: 0x400-0x1400
	o[11], o[12], o[13] = 0, 0x14, 0x00
	for i := 0x400; i <= 0x1400; i++ {
		o[i] = 0x7f ^ byte(i)
	}
	return o
}

// testSet makes the set's files: tile 1 of both graphics sets is solid pen 1.
func testSet() (names []string, sizes []int, data [][]byte) {
	prog := testProgram()
	g := &games()[0]
	for _, f := range g.Files {
		d := make([]byte, f.Size)
		switch f.Role {
		case "cpu_even", "cpu_odd":
			o := 0
			if f.Role == "cpu_odd" {
				o = 1
			}
			for i := range d {
				d[i] = prog[o+i*2]
			}
		case "audio":
			d = testSound()
		case "oki":
			d = testOKI()
		case "tiles", "sprites":
			if f.Lane == 0 && f.Offset == 0 {
				for i := 32; i < 64; i++ {
					d[i] = 0xff
				}
			}
		}
		names = append(names, f.Name)
		sizes = append(sizes, len(d))
		data = append(data, d)
	}
	return
}

func newTest(t *testing.T) *arcade.Core {
	t.Helper()
	names, sizes, data := testSet()
	c, err := arcade.NewFromFiles(names, sizes, data, "ddragon3.zip", core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	if c.GameID() != "ddragon3" {
		t.Fatalf("identified as %s", c.GameID())
	}
	return c
}

func wramWord(b *board, a int) int {
	a -= 0x1c0000
	return int(b.wram[a])<<8 | int(b.wram[a+1])
}

func TestInterruptsAndPicture(t *testing.T) {
	c := newTest(t)
	b := c.Board().(*board)
	var audio []float32
	for i := 0; i < 10; i++ {
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
	}
	// interrupt 6 once a frame (power on is at the start of vblank, as in MAME);
	// interrupt 5 every 16 lines
	if n := wramWord(b, cnt6); n != 10 {
		t.Errorf("%d vblank interrupts in 10 frames, want 10", n)
	}
	if n := wramWord(b, cnt5); n != 10*17 {
		t.Errorf("%d timer interrupts, want %d", n, 10*17)
	}
	scr := b.Screen()
	at := func(x, y int) uint32 { return scr[y*ScreenW+x] }
	for _, p := range []struct {
		x, y int
		want uint32
	}{
		{0, 0, 0xff0000}, {15, 15, 0xff0000}, {16, 0, 0}, // background tile
		{16, 16, 0x00ff00}, {31, 31, 0x00ff00}, {32, 16, 0}, // foreground tile
		{100, 100, 0x0000ff}, {115, 115, 0x0000ff}, {99, 100, 0}, {100, 116, 0}, // sprite
	} {
		if got := at(p.x, p.y); got != p.want {
			t.Errorf("pixel (%d,%d) = %06x, want %06x", p.x, p.y, got, p.want)
		}
	}
	loud := 0.0
	for _, s := range audio {
		loud = max(loud, float64(s), -float64(s))
	}
	if loud < 0.01 {
		t.Errorf("no sound (peak %f)", loud)
	}
}

func TestSaveStateAndReset(t *testing.T) {
	c := newTest(t)
	for i := 0; i < 5; i++ {
		c.RunFrame()
	}
	c.TakeAudio()
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	play := func() ([]uint32, []float32) {
		var a []float32
		for i := 0; i < 5; i++ {
			c.RunFrame()
			a = append(a, c.TakeAudio()...)
		}
		return append([]uint32(nil), c.Screen()...), a
	}
	s1, a1 := play()
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	s2, a2 := play()
	if !equalU32(s1, s2) || len(a1) != len(a2) {
		t.Fatal("the game ran differently after loading the state")
	}
	for i := range a1 {
		if a1[i] != a2[i] {
			t.Fatal("the sound differs after loading the state")
		}
	}
	st2, _ := c.SaveState()
	if err := c.LoadState(st2[:len(st2)-3]); err == nil {
		t.Error("a cut-short state loaded")
	}
	c.Reset()
	b := c.Board().(*board)
	if wramWord(b, cnt6) != 0 {
		t.Error("reset kept work RAM")
	}
	c.RunFrame()
	c.RunFrame()
	if wramWord(b, cnt6) != 2 {
		t.Errorf("after reset, %d vblanks in 2 frames", wramWord(b, cnt6))
	}
	s3, _ := c.SaveState()
	if bytes.Equal(s3, st) {
		t.Error("state didn't change")
	}
}

func equalU32(a, b []uint32) bool {
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

func TestInputsAndDips(t *testing.T) {
	c := newTest(t)
	b := c.Board().(*board)
	if v := b.ioRead(0x100004); v != 0xffff {
		t.Errorf("default DIP switches read %04x, want ffff", v)
	}
	if v := b.ioRead(0x100000); v != 0xffff {
		t.Errorf("idle controls read %04x", v)
	}
	b.SetInput([]arcade.Pad{{"Right": true, "Button 3": true, "Start (2P)": true}, {"Up": true}, {"Down": true, "Coin": true, "Start (1P)": true}})
	if v := b.ioRead(0x100000); v != 0x7bbe {
		t.Errorf("players 1 and 2 read %04x, want 7bbe", v)
	}
	if v := b.ioRead(0x100006) & 0xff; v != 0x77 {
		t.Errorf("player 3 reads %02x, want 77", v)
	}
	if v := b.ioRead(0x100002) & 7; v != 5 {
		t.Errorf("coins read %x, want 5 (coin 2)", v)
	}
	if !c.SetDip("players", 0) || !c.SetDip("difficulty", 0x0100) {
		t.Fatal("SetDip failed")
	}
	if v := b.ioRead(0x100004); v != 0x7dff {
		t.Errorf("DIP switches read %04x, want 7dff", v)
	}
}
