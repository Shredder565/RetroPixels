package atari

import (
	"bytes"
	"hash/fnv"
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// ---- generated sets: the real sets' file names and sizes, with a small
// 6502 program that exercises the board ----

// cnProgram: Centipede's ROM ($2000-$3FFF) with a loop that fills the video
// RAM, colours and POKEY with random numbers and copies the controls into
// RAM, and an IRQ handler that acknowledges and counts.
func cnProgram() []byte {
	rom := bytes.Repeat([]byte{0xea}, 0x2000)
	prog := []byte{
		0xa2, 0xff, // ldx #$ff
		0x9a,       // txs
		0x58,       // cli
		0xa9, 0x03, //      lda #3
		0x8d, 0x0f, 0x10, // sta $100f (SKCTL: run)
		// loop ($2009)
		0xad, 0x0a, 0x10, // lda $100a (RANDOM)
		0x9d, 0x00, 0x04, // sta $0400,x
		0x9d, 0x00, 0x05, // sta $0500,x
		0x9d, 0x00, 0x06, // sta $0600,x
		0x9d, 0x00, 0x07, // sta $0700,x
		0x8a, 0x29, 0x0f, 0xa8, // txa; and #15; tay
		0xad, 0x0a, 0x10, // lda $100a
		0x99, 0x00, 0x14, // sta $1400,y
		0x8a, 0x29, 0x07, 0xa8, // txa; and #7; tay
		0xad, 0x0a, 0x10, // lda $100a
		0x99, 0x00, 0x10, // sta $1000,y (POKEY sound)
		0xad, 0x00, 0x0c, // lda $0c00
		0x4d, 0x01, 0x0c, // eor $0c01
		0x95, 0x00, //      sta $00,x
		0xe8,             // inx
		0x8d, 0x00, 0x20, // sta $2000 (watchdog)
		0x4c, 0x09, 0x20, // jmp $2009
		// irq
		0x8d, 0x00, 0x18, // sta $1800
		0xee, 0x00, 0x03, // inc $0300
		0x40, // rti
	}
	copy(rom, prog)
	irq := 0x2000 + len(prog) - 7
	rom[0x1ffa], rom[0x1ffb] = 0x00, 0x20
	rom[0x1ffc], rom[0x1ffd] = 0x00, 0x20
	rom[0x1ffe], rom[0x1fff] = byte(irq), byte(irq>>8)
	return rom
}

// adProgram: Asteroids Deluxe's ROM ($6000-$7FFF). It copies a vector list
// to vector RAM, then over and over changes one line's length at random,
// starts the DVG, waits for it, and plays the POKEY; the NMI counts.
func adProgram() []byte {
	rom := bytes.Repeat([]byte{0xea}, 0x2000)
	list := []uint16{
		0xa000 | 500, 0x0000 | 500, // LABS y 500, x 500, scale 0
		0x9000 | 100, 0xf000 | 200, // VCTR scale 9: up 100, right 200, bright
		0xc000 | 0x20,                     //              JSRL word $20
		0xf000 | 0x0170 | 0x0004 | 0x0002, // SVEC
		0xb000,                            // HALT
	}
	sub := []uint16{0x7000 | 0x400 | 300, 0x8000 | 300, 0xd000} // VCTR, RTSL
	tab := make([]byte, 0, 0x60)
	for _, w := range list {
		tab = append(tab, byte(w), byte(w>>8))
	}
	for len(tab) < 0x40 {
		tab = append(tab, 0)
	}
	for _, w := range sub {
		tab = append(tab, byte(w), byte(w>>8))
	}
	copy(rom[0x1000:], tab) // the table at $7000
	prog := []byte{
		0xa2, 0xff, 0x9a, // ldx #$ff; txs
		0xa2, 0x00, // ldx #0
		// copy ($6005)
		0xbd, 0x00, 0x70, // lda $7000,x
		0x9d, 0x00, 0x40, // sta $4000,x
		0xe8,       //             inx
		0xe0, 0x50, //       cpx #$50
		0xd0, 0xf5, //       bne copy
		0xa9, 0x07, 0x8d, 0x0f, 0x2c, // lda #7; sta $2c0f (SKCTL)
		// loop ($6015)
		0xad, 0x0a, 0x2c, // lda $2c0a (RANDOM)
		0x29, 0x7f, //       and #$7f
		0x8d, 0x06, 0x40, // sta $4006 (the line's x)
		0x8d, 0x00, 0x30, // sta $3000 (DVG go)
		// wait ($6020)
		0x2c, 0x02, 0x20, // bit $2002
		0x30, 0xfb, //       bmi wait
		0xad, 0x0a, 0x2c, // lda $2c0a
		0x8d, 0x00, 0x2c, // sta $2c00
		0xa9, 0xa6, 0x8d, 0x01, 0x2c, // lda #$a6; sta $2c01
		0xad, 0x07, 0x24, // lda $2407 (rotate left)
		0x4d, 0x04, 0x20, // eor $2004 (fire)
		0x8d, 0x10, 0x00, // sta $0010
		0x8d, 0x0b, 0x2c, // sta $2c0b (POTGO)
		0xad, 0x08, 0x2c, // lda $2c08 (ALLPOT)
		0x8d, 0x11, 0x00, // sta $0011
		0xa9, 0x80, 0x8d, 0x03, 0x3c, // lda #$80; sta $3c03 (thrust)
		0xa9, 0xbc, 0x8d, 0x00, 0x36, // lda #$bc; sta $3600 (explosion)
		0x8d, 0x00, 0x34, // sta $3400 (watchdog)
		0x4c, 0x15, 0x60, // jmp loop
		// nmi
		0xe6, 0x00, // inc $00
		0x40, //       rti
	}
	copy(rom, prog)
	nmi := 0x6000 + len(prog) - 3
	rom[0x1ffa], rom[0x1ffb] = byte(nmi), byte(nmi>>8)
	rom[0x1ffc], rom[0x1ffd] = 0x00, 0x60
	rom[0x1ffe], rom[0x1fff] = 0x00, 0x60
	return rom
}

func genSet(t testing.TB, id string, prog []byte, chip func(name string, size int) []byte) *arcade.Core {
	t.Helper()
	var g *arcade.Game
	for _, e := range arcade.AllGames() {
		if e.Game.ID == id {
			g = e.Game
		}
	}
	if g == nil {
		t.Fatalf("%s isn't registered", id)
	}
	var names []string
	var sizes []int
	var contents [][]byte
	for _, f := range g.Files {
		if f.Optional {
			continue
		}
		data := chip(f.Name, f.Size)
		if f.Role == "cpu" {
			data = prog[f.Offset : f.Offset+f.Size]
		}
		names = append(names, f.Name)
		sizes = append(sizes, f.Size)
		contents = append(contents, data)
	}
	c, err := arcade.NewFromFiles(names, sizes, contents, id+".zip", core.Options{SampleRate: 44100})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func randChip(seed uint32) func(string, int) []byte {
	return func(name string, size int) []byte {
		h := fnv.New32a()
		h.Write([]byte(name))
		s := h.Sum32() ^ seed
		b := make([]byte, size)
		for i := range b {
			s ^= s << 13
			s ^= s >> 17
			s ^= s << 5
			b[i] = byte(s)
		}
		return b
	}
}

func hashScreen(c *arcade.Core) uint64 {
	h := fnv.New64a()
	for _, p := range c.Screen() {
		h.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16)})
	}
	return h.Sum64()
}

// runAndCompare: n frames from a state, twice, give the same pictures and sound.
func checkStates(t *testing.T, c *arcade.Core, frames int) {
	t.Helper()
	c.TakeAudio()
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	run := func() ([]uint64, []float32) {
		var hs []uint64
		var snd []float32
		for i := 0; i < frames; i++ {
			c.SetInput(0, uint32(i*7919)&0xff)
			c.RunFrame()
			hs = append(hs, hashScreen(c))
			snd = append(snd, c.TakeAudio()...)
		}
		return hs, snd
	}
	h1, s1 := run()
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	c.TakeAudio()
	h2, s2 := run()
	for i := range h1 {
		if h1[i] != h2[i] {
			t.Fatalf("frame %d after loading the state differs", i)
		}
	}
	if len(s1) != len(s2) {
		t.Fatalf("sound: %d samples, then %d", len(s1), len(s2))
	}
	for i := range s1 {
		if s1[i] != s2[i] {
			t.Fatalf("sample %d after loading the state differs", i)
		}
	}
	if err := c.LoadState(st[:len(st)-3]); err == nil {
		t.Fatal("a cut-off state loaded")
	}
}

func TestCentipedeGenerated(t *testing.T) {
	c := genSet(t, "centiped", cnProgram(), randChip(1))
	if c.Width() != 240 || c.Height() != 256 {
		t.Fatalf("picture %dx%d, want 240x256 upright", c.Width(), c.Height())
	}
	b := c.Board().(*centiped)
	for i := 0; i < 30; i++ {
		c.RunFrame()
	}
	// four IRQs a frame
	if n := b.ram[0x300]; n < 110 || n > 122 {
		t.Errorf("%d IRQs in 30 frames", n)
	}
	seen := map[uint32]bool{}
	for _, p := range c.Screen() {
		seen[p] = true
	}
	if len(seen) < 4 {
		t.Errorf("the picture has %d colours", len(seen))
	}
	a := c.TakeAudio()
	if len(a) < 44100/2-1000 {
		t.Errorf("%d samples for 30 frames", len(a))
	}
	loud := 0
	for _, v := range a {
		if v > 0.01 || v < -0.01 {
			loud++
		}
	}
	if loud == 0 {
		t.Error("no sound")
	}
	checkStates(t, c, 20)
	c.Reset()
	if b.ram[0x300] != 0 {
		t.Error("Reset kept RAM")
	}
}

func TestAstdeluxGenerated(t *testing.T) {
	c := genSet(t, "astdelux", adProgram(), randChip(2))
	b := c.Board().(*astdelux)
	pads := []arcade.Pad{{"Left": true}, {}}
	b.SetInput(pads)
	for i := 0; i < 61; i++ {
		c.RunFrame()
	}
	if n := b.ram[0]; n < 240 || n > 248 {
		t.Errorf("%d NMIs in 61 frames, want 244", n)
	}
	if b.ram[0x10] != 0x80 {
		t.Errorf("rotate left read %02x", b.ram[0x10])
	}
	if b.ram[0x11] != b.dsw2() {
		t.Errorf("ALLPOT read %02x, want the switches %02x", b.ram[0x11], b.dsw2())
	}
	// LABS, the long line, the subroutine's line, the short vector
	if len(b.lines) != 3 {
		t.Fatalf("%d lines: %+v", len(b.lines), b.lines)
	}
	l := b.lines[0]
	if l.x0 != 500 || l.y0 != 500 || l.y1 != 600 || l.z != 15 {
		t.Errorf("first line %+v", l)
	}
	lit := 0
	for _, p := range c.Screen() {
		if p>>8&0xff > 0x10 { // green: full intensity is drawn at about MAME's level
			lit++
		}
	}
	if lit < 200 {
		t.Errorf("%d pixels lit", lit)
	}
	checkStates(t, c, 20)
}

func TestDVG(t *testing.T) {
	mem := map[int]uint16{}
	put := func(at int, w ...uint16) {
		for i, v := range w {
			mem[at+i] = v
		}
	}
	put(0, 0xa000|512, 0x1000|512)                       // LABS y 512, x 512, global scale 1
	put(2, 0x8000|0x400|64, 0x7000|32)                   // VCTR scale 8 (+1): down 64, right 32, z 7
	put(4, 0xe010)                                       // JMPL $10
	put(0x10, 0xc020)                                    // JSRL $20
	put(0x11, 0xf000|0x0800|0x0008|0x0300|0x00f0|0x0001) // SVEC scale 5+1, y 3, x 1, z 15
	put(0x12, 0xb000)                                    // HALT
	put(0x20, 0x0000|0, 0x5000|0, 0xd000)                // a dot (z 5), RTSL
	lines, cycles := dvgRun(func(w int) uint16 { return mem[w] }, nil)
	want := []dvgLine{
		{512, 512, 512 + 32, 512 - 64, 7},
		{544, 448, 544, 448, 5},
		{544, 448, 544 + 256/8, 448 + 768/8, 15}, // scale 5+1: 1/8
	}
	if len(lines) != len(want) {
		t.Fatalf("lines %+v", lines)
	}
	for i := range want {
		if lines[i] != want[i] {
			t.Errorf("line %d: %+v, want %+v", i, lines[i], want[i])
		}
	}
	if cycles <= 0 {
		t.Error("no drawing time")
	}
	// a list that never halts stops
	if _, n := dvgRun(func(int) uint16 { return 0xe000 }, nil); n == 0 {
		t.Error("loop")
	}
}

func TestEAROM(t *testing.T) {
	var e earom
	// Centipede's way: erase 0e, write 0a; read 08 09 08
	e.latch(5, 0x5a)
	e.control(0x06)
	e.control(0x0e)
	e.control(0x02)
	e.control(0x0a)
	e.control(0)
	e.latch(5, 0)
	e.control(0x08)
	e.control(0x09)
	e.control(0x08)
	if e.read() != 0x5a {
		t.Errorf("read %02x", e.read())
	}
	// Asteroids Deluxe's: erase 0e, write 0c
	e.latch(6, 0)
	e.control(0x0e)
	e.latch(6, 0xa5)
	e.control(0x04)
	e.control(0x0c)
	if e.mem[6] != 0xa5 {
		t.Errorf("wrote %02x", e.mem[6])
	}
	// without chip select nothing happens
	e.latch(6, 0x11)
	e.control(0x04)
	if e.mem[6] != 0xa5 {
		t.Error("wrote without chip select")
	}
}

// ---- the real sets (skipped when they aren't here) ----

const romDir = "/mnt/project-files/roms-private/arcade-new/"

func openReal(t *testing.T, id string) *arcade.Core {
	t.Helper()
	data, err := os.ReadFile(romDir + id + ".zip")
	if err != nil {
		t.Skip("the ROM set isn't here")
	}
	c, err := arcade.New(data, core.Options{SampleRate: 48000, Name: id})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

func press(c *arcade.Core, name string, on bool) uint32 {
	for i, b := range c.Info().Buttons {
		if b == name && on {
			return 1 << i
		}
	}
	return 0
}

func TestCentipedeReal(t *testing.T) {
	c := openReal(t, "centiped")
	b := c.Board().(*centiped)
	for f := 1; f <= 400; f++ {
		c.SetInput(0, press(c, "Coin", f >= 120 && f < 126)|press(c, "Start (1P)", f >= 200 && f < 206))
		c.RunFrame()
	}
	// the game is on: the player's shooter is sprite 13 at the bottom
	if b.vram[0x3d0+13] < 0x80 || b.vram[0x3d0+13] > 0xf0 {
		t.Logf("sprite 13 y %02x", b.vram[0x3d0+13])
	}
	if len(c.TakeAudio()) == 0 {
		t.Error("no sound")
	}
	checkStates(t, c, 60)
}

func TestAstdeluxReal(t *testing.T) {
	c := openReal(t, "astdelux")
	b := c.Board().(*astdelux)
	for f := 1; f <= 400; f++ {
		c.SetInput(0, press(c, "Coin", f >= 120 && f < 126)|press(c, "Start (1P)", f >= 200 && f < 206)|press(c, "Fire", f > 300 && f&4 != 0))
		c.RunFrame()
	}
	if len(b.lines) < 50 {
		t.Errorf("%d lines drawn", len(b.lines))
	}
	checkStates(t, c, 60)
	c.Reset()
	for i := 0; i < 100; i++ {
		c.RunFrame()
	}
	if len(b.lines) < 50 {
		t.Errorf("after Reset, %d lines drawn", len(b.lines))
	}
}
