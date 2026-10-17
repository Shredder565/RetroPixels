package cps1

import (
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// A synthetic 1941-shaped set: a small 68000 program that counts its
// vblank interrupts, copies the controls into graphics RAM, sends sound
// commands, sets up the video; a Z80 program that plays an MSM6295 phrase
// when told to and keys a YM2151 note; random-free made-up graphics.

func be16(b []byte, at int, ws ...int) int {
	for _, w := range ws {
		b[at], b[at+1] = byte(w>>8), byte(w)
		at += 2
	}
	return at
}

func testProgram() []byte {
	p := make([]byte, 0x100000)
	be16(p, 0, 0x00ff, 0x8000, 0x0000, 0x0400) // stack, start
	be16(p, 0x68, 0x0000, 0x0500)              // level 2 interrupt
	// main: move.w #imm,(abs).l for the video set-up, then the loop
	at := 0x400
	mv := func(v, a int) { at = be16(p, at, 0x33fc, v, a>>16, a&0xffff) }
	mv(0x9000, 0x800100)             // sprites
	mv(0x9080, 0x800102)             // scroll 1
	mv(0x90c0, 0x800104)             // scroll 2
	mv(0x9100, 0x800106)             // scroll 3
	mv(0x003f, 0x800172)             // palette control
	mv(0x12ea, 0x800168)             // layer control
	mv(0xff00, 0x900006)             // end of sprite list
	mv(0x0123, 0x914002)             // a colour
	mv(0x0001, 0x800180)             // sound command 1
	at = be16(p, at, 0x46fc, 0x2000) // move #$2000,sr
	loop := at
	// move.w $800000,$908000 (controls); move.w $ff0000,$908002 (count)
	at = be16(p, at, 0x33f9, 0x0080, 0x0000, 0x0090, 0x8000)
	at = be16(p, at, 0x33f9, 0x00ff, 0x0000, 0x0090, 0x8002)
	at = be16(p, at, 0x6000, (loop-(at+2))&0xffff) // bra loop
	// vblank: addq.w #1,$ff0000; palette base; rte
	at = be16(p, 0x500, 0x5279, 0x00ff, 0x0000)
	at = be16(p, at, 0x33fc, 0x9140, 0x0080, 0x010a)
	be16(p, at, 0x4e73)
	return p
}

func testSound() []byte {
	z := make([]byte, 0x10000)
	code := []byte{
		0xf3,             // di
		0x31, 0x00, 0xd8, // ld sp,$d800
		0x3e, 0x28, 0x32, 0x00, 0xf0, // YM: register $28 (key code ch 0)
		0x3e, 0x4a, 0x32, 0x01, 0xf0, // = $4a
		0x3e, 0x08, 0x32, 0x00, 0xf0, // key on
		0x3e, 0x78, 0x32, 0x01, 0xf0,
		// wait for the command, then play phrase 1 on voice 0
		0x3a, 0x08, 0xf0, // loop: ld a,($f008)
		0xfe, 0x01, // cp 1
		0x20, 0xf9, // jr nz,loop
		0x3e, 0x81, 0x32, 0x02, 0xf0, // phrase 1
		0x3e, 0x10, 0x32, 0x02, 0xf0, // voice 0
		0x76, // halt
	}
	copy(z, code)
	return z
}

func testOKI() []byte {
	o := make([]byte, 0x40000)
	o[8], o[9], o[10] = 0, 0x04, 0x00 // phrase 1: 0x400-0x1400
	o[11], o[12], o[13] = 0, 0x14, 0x00
	for i := 0x400; i <= 0x1400; i++ {
		o[i] = 0x7f ^ byte(i)
	}
	return o
}

func testSet(t *testing.T) (names []string, sizes []int, contents [][]byte) {
	t.Helper()
	g := games()[0]
	prog := testProgram()
	for _, f := range g.Files {
		if f.Optional {
			continue
		}
		d := make([]byte, f.Size)
		switch f.Role {
		case "cpu_even", "cpu_odd":
			o := f.Offset
			if f.Role == "cpu_odd" {
				o++
			}
			for i := range d {
				d[i] = prog[o+i*2]
			}
		case "cpu_word":
			for i := 0; i < len(d); i += 2 {
				d[i], d[i+1] = prog[f.Offset+i+1], prog[f.Offset+i]
			}
		case "audio":
			copy(d, testSound())
		case "oki":
			copy(d, testOKI()[f.Offset:])
		case "gfx":
			for i := range d {
				d[i] = byte(i * (f.Lane + 3))
			}
		}
		names = append(names, f.Name)
		sizes = append(sizes, f.Size)
		contents = append(contents, d)
	}
	return
}

func TestSyntheticBoard(t *testing.T) {
	names, sizes, contents := testSet(t)
	c, err := arcade.NewFromFiles(names, sizes, contents, "1941.zip", core.Options{SampleRate: 44100})
	if err != nil {
		t.Fatal(err)
	}
	if c.GameID() != "1941" || c.Width() != ScreenH || c.Height() != ScreenW {
		t.Fatalf("game %s %dx%d", c.GameID(), c.Width(), c.Height())
	}
	b := c.Board().(*board)
	var audio []float32
	for f := 0; f < 60; f++ {
		if f == 30 {
			c.SetInput(0, 1<<4) // Button 1
		}
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
	}
	// 60 vblank interrupts (the last one is taken next frame)
	if n := int(b.wram[0])<<8 | int(b.wram[1]); n != 59 {
		t.Errorf("%d interrupts, want 59", n)
	}
	// the controls, active low, as the program copied them: button 1 is bit 4
	if v := b.vid.word(0x8000); v != 0xffef {
		t.Errorf("controls read %#04x, want 0xffef", v)
	}
	// the colour reached the palette (sprite palette 0, pen 1)
	if b.vid.pal[1] != colour(0x0123) {
		t.Errorf("palette entry 1 = %#06x", b.vid.pal[1])
	}
	if want := 44100 * 60 / frameRate; len(audio) < int(want)-100 || len(audio) > int(want)+100 {
		t.Errorf("%d samples for 60 frames, want about %.0f", len(audio), want)
	}
	if b.oki.Read()&1 == 0 {
		t.Error("the MSM6295 isn't playing")
	}
	peak := float32(0)
	for _, s := range audio {
		peak = max(peak, s, -s)
	}
	if peak < 0.01 {
		t.Error("no sound")
	}

	// save states: the same frames after loading
	s, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	run := func() ([]uint32, []float32) {
		var a []float32
		for f := 0; f < 20; f++ {
			c.RunFrame()
			a = append(a, c.TakeAudio()...)
		}
		return append([]uint32(nil), c.Screen()...), a
	}
	pic, snd := run()
	if err := c.LoadState(s); err != nil {
		t.Fatal(err)
	}
	pic2, snd2 := run()
	for i := range pic {
		if pic[i] != pic2[i] {
			t.Fatal("picture differs after loading a state")
		}
	}
	if len(snd) != len(snd2) {
		t.Fatalf("%d samples after loading, want %d", len(snd2), len(snd))
	}
	for i := range snd {
		if snd[i] != snd2[i] {
			t.Fatal("sound differs after loading a state")
		}
	}
	if err := c.LoadState(s[:len(s)-3]); err == nil {
		t.Error("a cut-off state loaded")
	}

	// reset starts over
	c.Reset()
	c.RunFrame()
	if n := int(b.wram[0])<<8 | int(b.wram[1]); n != 0 {
		t.Errorf("%d interrupts after reset", n)
	}
}

func TestIdentify(t *testing.T) {
	g := games()[0]
	for _, mame := range []bool{false, true} {
		var names []string
		var sizes []int
		for _, f := range g.Files {
			if f.Optional {
				continue
			}
			n := f.Name
			if mame {
				n = f.Alt[0]
			}
			names = append(names, n)
			sizes = append(sizes, f.Size)
		}
		e, ok := arcade.Identify(names, sizes, "")
		if !ok || e.Game.ID != "1941" {
			t.Errorf("mame names %v: identified as %v", mame, e.Game)
		}
	}
}

func TestDips(t *testing.T) {
	b := &board{game: &games()[0], dips: map[string]int{}}
	for _, d := range b.game.Dips {
		b.dips[d.ID] = d.Default
		if _, ok := dipMasks[d.ID]; !ok {
			t.Errorf("setting %s has no switches", d.ID)
		}
		for _, o := range d.Options {
			if o.Value&^dipMasks[d.ID].mask != 0 {
				t.Errorf("%s option %q outside its switches", d.ID, o.Label)
			}
		}
	}
	// MAME's defaults for 1941
	if a, bb, c := b.dsw(0), b.dsw(1), b.dsw(2); a != 0xff || bb != 0xfc || c != 0x9f {
		t.Errorf("switches %#02x %#02x %#02x, want 0xff 0xfc 0x9f", a, bb, c)
	}
	b.dips["test"] = 0
	if b.dsw(2) != 0x1f {
		t.Error("test switch")
	}
}
