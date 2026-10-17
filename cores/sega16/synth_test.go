package sega16

import (
	"testing"

	"retropixels/cores/arcade"
)

// These tests use made-up ROMs, so they run everywhere (CI included).

func TestMapperRegions(t *testing.T) {
	var m mapper
	m.reset()
	// after reset every region is 64 KB at 0: region 0 answers there
	if r, _ := m.region(0x001234); r != 0 {
		t.Fatalf("0x001234 in region %d after reset", r)
	}
	if r, _ := m.region(0xfe0007); r != -1 {
		t.Fatalf("0xfe0007 in region %d, want the mapper", r)
	}
	// region 5: 128 KB at 0x40 (a base inside the size is ignored);
	// region 3: 64 KB at 0xff
	m.write(0xfe0035, 1)
	m.write(0xfe0037, 0x41)
	m.write(0xfe002d, 0)
	m.write(0xfe002f, 0xff)
	for _, c := range []struct {
		a   uint32
		r   int
		off uint32
	}{{0x400002, 5, 2}, {0x410ffe, 5, 0x10ffe}, {0x420000, -1, 0x420000}, {0xffc010, 3, 0xc010}} {
		if r, off := m.region(c.a); r != c.r || off != c.off {
			t.Errorf("%06x: region %d offset %x, want %d %x", c.a, r, off, c.r, c.off)
		}
	}
	if !m.write(0xfe0007, 0x41) || m.write(0xfe0009, 1) {
		t.Error("only register 3 is the sound latch")
	}
	if m.read(0xfe0037) != 0x41 {
		t.Error("registers don't read back")
	}
}

// synthVideo: tiles 1 and 0x81 are pen 1 everywhere, tile 2 pen 7 (a
// tile layer's tile code shares bits with its colour: 1 in colour 2 is 0x81); sprite bank 0 holds
// a line 1,2,3,4 5,6,7,15 at word 1.
func synthVideo() *video {
	var tiles [3][]byte
	for p := range tiles {
		tiles[p] = make([]byte, 0x2000)
		for i := 0; i < 8; i++ {
			if p == 0 {
				tiles[p][8+i] = 0xff
				tiles[p][0x81*8+i] = 0xff
			}
			tiles[p][16+i] = 0xff
		}
	}
	spr := make([]uint16, 0x20000)
	spr[1], spr[2] = 0x1234, 0x567f
	var banks [16]int
	for i := range banks {
		banks[i] = -1
	}
	banks[0] = 0
	v := newVideo(tiles, spr, banks)
	v.reset()
	v.enable = true
	return v
}

func (v *video) pal(i int, w uint16) {
	v.palRAM[i] = w
	v.setColor(i)
}

func (v *video) at(x, y int) uint32 { return v.screen[y*scrW+x] }

func TestVideoLayers(t *testing.T) {
	v := synthVideo()
	v.pal(0, 0x0444)     // backdrop
	v.pal(2*8+1, 0x000f) // background colour 2, pen 1
	v.pal(1*8+7, 0x00f0) // text colour 1, pen 7
	// background (layer 1) tile at the screen's top left: the visible area
	// starts at column 24 of page 0 with no scroll
	v.tileRAM[24] = 1 | 2<<6
	v.textRAM[0x740] = 0x1111 // the foreground shows (empty) page 1
	// text over it, one cell to the right
	v.textRAM[25] = 2 | 1<<9
	v.render()
	if v.at(0, 0) != v.normal[17] || v.at(7, 7) != v.normal[17] {
		t.Errorf("background tile: %06x", v.at(0, 0))
	}
	if v.at(8, 0) != v.normal[15] {
		t.Errorf("text: %06x", v.at(8, 0))
	}
	if v.at(100, 100) != v.normal[0] {
		t.Errorf("backdrop: %06x", v.at(100, 100))
	}
	// scroll the background 8 pixels left (hscroll is subtracted)
	v.textRAM[0x74c+1] = 0x3f8
	v.render()
	if v.at(0, 0) != v.normal[0] || v.at(8, 0) != v.normal[15] {
		t.Errorf("after scrolling: %06x %06x", v.at(0, 0), v.at(8, 0))
	}
	// the display enable bit blanks the picture
	v.enable = false
	v.render()
	if v.at(8, 0) != 0 {
		t.Error("picture shown while disabled")
	}
}

func TestVideoSprites(t *testing.T) {
	v := synthVideo()
	v.pal(2*8+1, 0x000f)
	v.tileRAM[24+2] = 1 | 2<<6 // background tile under x 16..23, y 0..7
	for p := 1; p < 8; p++ {
		v.pal(0x400|5<<4|p, uint16(p)<<4)
	}
	put := func(e int, top, bottom, x int, w2, w4, w5 uint16) {
		d := v.sprRAM[e*8:]
		d[0] = uint16(bottom<<8 | top)
		d[1] = uint16(x + 0xb8)
		d[2] = w2
		d[3] = 0
		d[4] = w4
		d[5] = w5
	}
	// sprite 0: one line at y 10, x 20, pitch 1 (the address steps before
	// the line), colour 5, priority 3
	put(0, 10, 11, 20, 1, 3<<6|5, 0)
	// sprite 1: the same line zoomed (hz 16 drops every 4th pixel) at y 20
	put(1, 20, 21, 20, 1, 3<<6|5, 16)
	// sprite 2: a shadow (colour 0x3f) over the background tile
	put(2, 2, 3, 16, 1, 3<<6|0x3f, 0)
	v.sprRAM[3*8+2] = 0x8000 // end of the list
	v.render()
	for i, p := range []int{1, 2, 3, 4, 5, 6, 7} {
		if got := v.at(20+i, 10); got != v.normal[0x400|5<<4|p] {
			t.Errorf("sprite pixel %d: %06x", i, got)
		}
	}
	if v.at(27, 10) != v.normal[0] {
		t.Error("pen 15 drawn")
	}
	for i, p := range []int{1, 2, 3, 5, 6, 7} {
		if got := v.at(20+i, 20); got != v.normal[0x400|5<<4|p] {
			t.Errorf("zoomed sprite pixel %d: %06x", i, got)
		}
	}
	if v.at(18, 2) != v.shadow[17] || v.shadow[17] == v.normal[17] {
		t.Errorf("shadow: %06x (normal %06x)", v.at(18, 2), v.normal[17])
	}
	// a background tile with its priority bit set covers priority 1 sprites
	v.tileRAM[24+2] |= 0x8000
	v.sprRAM[2*8+4] = 1<<6 | 5
	v.render()
	if v.at(18, 2) != v.normal[17] {
		t.Error("priority background tile under a priority 1 sprite")
	}
}

// asm68k assembles the few instructions the synthetic program needs.
type asm68k []byte

func (p *asm68k) w(ws ...uint32) {
	for _, w := range ws {
		*p = append(*p, byte(w>>8), byte(w))
	}
}
func (p *asm68k) moveB(v uint8, a uint32)  { p.w(0x13fc, uint32(v), a>>16, a&0xffff) }
func (p *asm68k) moveW(v uint16, a uint32) { p.w(0x33fc, uint32(v), a>>16, a&0xffff) }

// synthBoard builds Altered Beast's file set with a small 68000 program:
// it maps the I/O, tile/text RAM and palette, puts a text tile on the
// screen, turns the picture on, sends sound command 0x41, then counts in D0
// and writes the count to palette entry 1 forever.
func synthBoard(t *testing.T) *board {
	t.Helper()
	gd := games[0]
	files := arcade.Files{}
	for _, f := range gd.game.Files {
		if !f.Optional {
			files[f.Name] = make([]byte, f.Size)
		}
	}
	p := make(asm68k, 0x400)
	p[6] = 0x04             // the reset PC: 0x400
	p.moveB(0xc4, 0xfe003f) // region 7: I/O at 0xc40000
	p.moveB(0x84, 0xfe003b) // region 6: palette at 0x840000
	p.moveB(1, 0xfe0035)    // region 5: 128 KB at 0x400000
	p.moveB(0x40, 0xfe0037)
	p.moveW(1|1<<9, 0x410000+2*24) // text: tile 1, colour 1, top left
	p.moveW(0x00f0, 0x840000+2*9)  // its colour
	p.moveB(0x20, 0xc40001)        // picture on
	p.moveB(0x41, 0xfe0007)        // sound command
	p.w(0x5240, 0x33c0, 0x84, 0x0002, 0x60f6)
	rom := files["11705"]
	lo := files["11704"]
	for i := 0; i < len(p)/2; i++ {
		rom[i], lo[i] = p[2*i], p[2*i+1]
	}
	files["11671"][0], files["11671"][1], files["11671"][2] = 0xf3, 0x18, 0xfe // di; jr $
	for i := 0; i < 8; i++ {
		files["11674"][8+i] = 0xff
	}
	g := gd.game
	bd, err := newBoard(gd, &g, files, arcade.Opts{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return bd.(*board)
}

func TestSynthBoard(t *testing.T) {
	b := synthBoard(t)
	b.SetInput([]arcade.Pad{{}, {}})
	b.RunFrame()
	if b.Screen()[0] != b.v.normal[9] || b.v.normal[9] == 0 {
		t.Fatalf("text tile not drawn: %06x", b.Screen()[0])
	}
	if b.snd.latch != 0x41 || !b.snd.z80.IRQ {
		t.Errorf("sound latch %02x, irq %v", b.snd.latch, b.snd.z80.IRQ)
	}
	if n := len(b.TakeAudio()); n < 790 || n > 810 {
		t.Errorf("%d audio samples in a frame", n)
	}
	// inputs and DIP switches through the I/O chip (active low)
	b.SetInput([]arcade.Pad{{"Coin": true, "Left": true}, {}})
	if v := b.ioRead(0x1000); v != 0xfe {
		t.Errorf("service port %02x", v)
	}
	if v := b.ioRead(0x1002); v != 0x7f {
		t.Errorf("player 1 %02x", v)
	}
	if v := b.ioRead(0x2002); v != 0xff {
		t.Errorf("DIP switches A %02x, want ff (1 coin 1 credit both)", v)
	}
	if v := b.ioRead(0x2000); v != 0xfd {
		t.Errorf("DIP switches B %02x, want fd (defaults)", v)
	}
}

func TestSynthStateAndReset(t *testing.T) {
	b := synthBoard(t)
	b.SetInput([]arcade.Pad{{}, {}})
	for i := 0; i < 5; i++ {
		b.RunFrame()
	}
	st, err := b.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	frames := func() []uint16 {
		var p []uint16
		for i := 0; i < 3; i++ {
			b.RunFrame()
			p = append(p, b.v.palRAM[1])
		}
		return p
	}
	want := frames()
	if want[0] == want[1] {
		t.Fatal("the program isn't running")
	}
	if err := b.LoadState(st); err != nil {
		t.Fatal(err)
	}
	if got := frames(); got[0] != want[0] || got[2] != want[2] {
		t.Errorf("after loading: %v, want %v", got, want)
	}
	if err := b.LoadState([]byte("junk")); err == nil {
		t.Error("loaded a bad state")
	}
	// Reset clears the mapper, video and sound, and the program runs again
	// (the 68000's own reset leaves D0 alone, so the count carries on)
	b.Reset()
	if b.m.regs != [32]uint8{} || b.v.enable || b.v.palRAM[9] != 0 || b.snd.latch != 0 {
		t.Fatal("Reset left the board's state")
	}
	b.RunFrame()
	if b.Screen()[0] != b.v.normal[9] || b.snd.latch != 0x41 {
		t.Error("the program didn't run again after Reset")
	}
}
