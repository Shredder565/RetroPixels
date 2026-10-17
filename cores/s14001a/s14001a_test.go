package s14001a

import (
	"bytes"
	"os"
	"strings"
	"testing"

	"retropixels/cores/arcade"
)

// testROM is a synthetic speech ROM: pseudo-random sample data and six words
// that cover voiced, unvoiced, silent and multi-syllable words.
// testdata/mame_words.txt holds what MAME's S14001A (on Berzerk's board,
// speed 4) output for the same ROM, recorded at 192 kHz and read back one
// DAC level per sample, the leading and trailing silence cut off.
func testROM() []byte {
	rom := make([]byte, 0x1000)
	x := uint32(1)
	for a := 0x200; a < 0x1000; a++ {
		x = (x*1103515245 + 12345) & 0x7fffffff
		rom[a] = uint8(x >> 16)
	}
	syl := [][]byte{
		{0x20, 0x9c},                         // unvoiced, 4 periods
		{0x40, 0xc0},                         // voiced, 8 stored x 4
		{0x60, 0xc9},                         // voiced, 6 stored x 3
		{0x80, 0x9a},                         // unvoiced, 6 periods
		{0xa0, 0x1f, 0xa8, 0x7c, 0xb0, 0xd1}, // unvoiced, silent voiced, voiced
		{0xb8, 0xdf},                         // voiced, 1 period
	}
	for w, s := range syl {
		rom[2*w] = uint8(0x10 + w)
		copy(rom[0x100+16*w:], s)
	}
	return rom
}

// speak runs word w to its end and returns the levels, silence trimmed.
func speak(c *Chip, w int) []int {
	c.Start(w)
	var out []int
	for n := 0; c.Busy() && n < 1<<20; n++ {
		out = append(out, c.Next())
	}
	for len(out) > 0 && out[0] == 7 {
		out = out[1:]
	}
	for len(out) > 0 && out[len(out)-1] == 7 {
		out = out[:len(out)-1]
	}
	return out
}

func TestMatchesMAME(t *testing.T) {
	data, err := os.ReadFile("testdata/mame_words.txt")
	if err != nil {
		t.Fatal(err)
	}
	c := New(testROM())
	for w, line := range strings.Fields(string(data)) {
		got := speak(c, w)
		if len(got) != len(line) {
			t.Errorf("word %d: %d samples, MAME %d", w, len(got), len(line))
		}
		for i := 0; i < len(got) && i < len(line); i++ {
			want := int(line[i] - '0')
			if line[i] >= 'a' {
				want = int(line[i]-'a') + 10
			}
			if got[i] != want {
				t.Errorf("word %d: sample %d is %d, MAME %d", w, i, got[i], want)
				break
			}
		}
	}
}

// A voiced period is its first quarter, then the same mirrored, then silence.
func TestMirror(t *testing.T) {
	rom := make([]byte, 0x1000)
	rom[0], rom[1] = 0x10, 0x00
	rom[0x100], rom[0x101] = 0x20, 0xdf // voiced, one period
	// small steps only, so the level never reaches 0 or 15
	copy(rom[0x200:], []byte{0xaa, 0x99, 0x66, 0x99, 0xa5, 0x99, 0x5a, 0x99})
	c := New(rom)
	c.Start(0)
	var p []int
	for c.Busy() {
		p = append(p, c.Next())
	}
	p = p[4:] // the fetches
	if len(p) != 128 {
		t.Fatalf("period of %d samples", len(p))
	}
	for i := 0; i < 32; i++ {
		if p[i] != p[63-i] {
			t.Fatalf("not mirrored at %d: %v", i, p[:64])
		}
	}
	for _, v := range p[64:] {
		if v != 7 {
			t.Fatalf("second half not silent: %v", p[64:])
		}
	}
}

// Blank ROMs never end a word (no syllable has the last-syllable bit): the
// chip stays busy, as MAME's does with zero-filled speech ROMs.
func TestBlankROMStaysBusy(t *testing.T) {
	c := New(make([]byte, 0x1000))
	c.Start(5)
	for i := 0; i < 100000; i++ {
		c.Next()
	}
	if !c.Busy() {
		t.Fatal("blank ROM word ended")
	}
}

func TestState(t *testing.T) {
	c := New(testROM())
	c.Start(2)
	for i := 0; i < 777; i++ {
		c.Next()
	}
	w := &arcade.StateW{}
	c.Save(w)
	var a []int
	for i := 0; i < 500; i++ {
		a = append(a, c.Next())
	}
	d := New(testROM())
	d.Load(&arcade.StateR{B: bytes.Clone(w.B)})
	for i := 0; i < 500; i++ {
		if v := d.Next(); v != a[i] {
			t.Fatalf("after load, sample %d is %d, want %d", i, v, a[i])
		}
	}
}
