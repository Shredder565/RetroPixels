package dec8

// Tests on generated data (no ROMs needed).

import (
	"bytes"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

func TestDecodeGfx(t *testing.T) {
	// one 16x16 tile, one plane: line 0 left byte 0x80, right byte 0x01
	rom := make([]byte, 32)
	rom[0], rom[16] = 0x80, 0x01
	msb := decodeGfx(rom, 1, 16, []int{0}, false, false)
	if msb[0] != 1 || msb[15] != 1 || msb[7] != 0 {
		t.Errorf("msb-first: %v", msb[:16])
	}
	lsb := decodeGfx(rom, 1, 16, []int{0}, true, false)
	if lsb[7] != 1 || lsb[8] != 1 || lsb[0] != 0 {
		t.Errorf("lsb-first: %v", lsb[:16])
	}
	sw := decodeGfx(rom, 1, 16, []int{0}, false, true)
	if sw[7] != 1 || sw[8] != 1 {
		t.Errorf("halves swapped: %v", sw[:16])
	}
	// planes: the first listed is the top bit
	rom2 := []byte{0x80, 0, 0, 0, 0, 0, 0, 0, 0x80}
	ch := decodeGfx(rom2, 1, 8, []int{0, 8}, false, false)
	if ch[0] != 3 {
		t.Errorf("two planes: %d", ch[0])
	}
}

func TestMCU(t *testing.T) {
	var m mcu
	m.reset()
	for _, c := range []struct{ q, a uint16 }{{0x00aa, 0x0655}, {0x021a, 0x06e5}, {0x0400, 0x0400}, {0x0533, 0x0533}} {
		m.ask(uint8(c.q), false)
		m.ask(uint8(c.q>>8), true)
		if got := uint16(m.hi)<<8 | uint16(m.lo); got != c.a {
			t.Errorf("%04x: answer %04x, want %04x", c.q, got, c.a)
		}
	}
	// a coin waits until the interrupt is allowed
	m.coins(arcade.Pad{"Coin": true}, arcade.Pad{})
	if m.irq {
		t.Fatal("interrupt while not allowed")
	}
	m.enable(true)
	if !m.irq || m.hi&0xd0 != 0x80 {
		t.Fatalf("coin 1: irq %v, high byte %02x", m.irq, m.hi)
	}
	m.enable(false)
	if m.irq {
		t.Fatal("turning bit 0 off doesn't clear the interrupt")
	}
	m.enable(true)
	m.coins(arcade.Pad{"Coin": true}, arcade.Pad{"Coin": true}) // player 1 still held
	if !m.irq || m.hi != 0x40 {
		t.Fatalf("coin 2: irq %v, high byte %02x", m.irq, m.hi)
	}
}

// swapOp scrambles an opcode as the DECO 222 expects it in ROM.
func swapOp(v uint8) uint8 { return v&0x9f | v>>1&0x20 | v<<1&0x40 }

// fakeSet is a set with a small main program (counts frames in its NMI and
// sends the count to the sound CPU) and sound program (keeps the last
// command).
func fakeSet() ([]string, []int, [][]byte) {
	var names []string
	var sizes []int
	var data [][]byte
	for _, f := range filesGhostb {
		if f.Optional {
			continue
		}
		d := make([]byte, f.Size)
		switch f.Name {
		case "dz-01.rom": // 8000-ffff
			copy(d, []byte{
				0x10, 0xce, 0x18, 0x00, // LDS #$1800
				0x86, 0x03, // LDA #$03
				0xb7, 0x38, 0x40, // STA $3840 (NMI on)
				0x1c, 0xaf, // ANDCC #$AF
				0x7c, 0x00, 0x10, // INC $0010
				0x20, 0xfb, // BRA
				0x7c, 0x00, 0x11, // NMI: INC $0011
				0xb6, 0x00, 0x11, // LDA $0011
				0xb7, 0x38, 0x00, // STA $3800
				0x3b, // RTI
			})
			d[0x7ffc], d[0x7ffd], d[0x7ffe], d[0x7fff] = 0x80, 0x10, 0x80, 0x00
		case "dz-06.rom": // sound, 8000-ffff
			copy(d, []byte{
				swapOp(0x78),                     // SEI
				swapOp(0xa2), 0xff, swapOp(0x9a), // LDX #$FF; TXS
				swapOp(0xe6), 0x10, // INC $10
				swapOp(0x4c), 0x04, 0x80, // JMP $8004
				swapOp(0xad), 0x00, 0x60, // NMI: LDA $6000
				swapOp(0x85), 0x11, // STA $11
				swapOp(0x40), // RTI
			})
			d[0x7ffa], d[0x7ffb], d[0x7ffc], d[0x7ffd], d[0x7ffe], d[0x7fff] = 0x09, 0x80, 0x00, 0x80, 0x09, 0x80
		}
		names = append(names, f.Name)
		sizes = append(sizes, f.Size)
		data = append(data, d)
	}
	return names, sizes, data
}

func TestFakeBoard(t *testing.T) {
	names, sizes, data := fakeSet()
	c, err := arcade.NewFromFiles(names, sizes, data, "ghostb", core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	b := c.Board().(*ghostb)
	for i := 0; i < 10; i++ {
		c.RunFrame()
	}
	if n := b.ram[0x11]; n != 10 {
		t.Errorf("%d NMIs in 10 frames", n)
	}
	if b.ram[0x10] == 0 || b.sndRAM[0x10] == 0 {
		t.Error("a processor isn't running")
	}
	if b.sndRAM[0x11] != b.ram[0x11] {
		t.Errorf("sound CPU got command %d, main sent %d", b.sndRAM[0x11], b.ram[0x11])
	}
	if a := c.TakeAudio(); len(a) < 8000 || len(a) > 8500 {
		t.Errorf("%d samples for 10 frames", len(a))
	}

	// save states: running on from a loaded state gives the same frames
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		c.RunFrame()
	}
	want, _ := c.SaveState()
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		c.RunFrame()
	}
	got, _ := c.SaveState()
	if !bytes.Equal(got, want) {
		t.Error("state after load + 5 frames differs")
	}
	if err := c.LoadState(st[:len(st)/2]); err == nil {
		t.Error("a cut state loaded")
	}

	c.Reset()
	if b.ram[0x11] != 0 {
		t.Error("reset kept RAM")
	}
	c.RunFrame()
	if b.ram[0x11] != 1 {
		t.Errorf("after reset: %d NMIs in a frame", b.ram[0x11])
	}
}
