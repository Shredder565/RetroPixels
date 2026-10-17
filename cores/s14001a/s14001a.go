// Package s14001a is the TSI S14001A speech chip (Berzerk, Frenzy, the
// Stern/Exidy speech boards and TSI's talking calculators).
//
// The chip speaks words from a 4 KB ROM of Mozer-compressed speech: 2-bit
// delta modulation, pitch periods stored as a quarter and played forwards
// then mirrored, the second half of a voiced period left silent, and each
// stored period played several times. The output is a 4-bit DAC level
// (7 = silence).
//
// The ROM holds, in order:
//
//   - a word table: word n's two bytes at 2n, 2n+1, read as a big-endian
//     16-bit value shifted right 4, give the address of its syllable list;
//   - syllable lists: two bytes a syllable. The first is the address of its
//     sample data divided by 16. The second is G B Y S S S R R: G = the
//     word's last syllable, B = voiced (mirrored), Y = silent (decoded but
//     the DAC holds 7), SSS and RR count the periods (below);
//   - the sample data: four 2-bit codes a byte, high bits first.
//
// A period is 128 samples. Voiced: 32 samples decoded forwards from the
// next 8 bytes, the same 32 mirrored (the last level once more, then the
// codes read backwards with the delta reversed), then 64 samples of
// silence; 8-SSS stored periods, each played 4-RR times. Unvoiced: 128
// samples decoded forwards from the next 32 bytes, 32-SSSRR periods. The
// decoder starts again (level 7, previous code 2) at each period, and the
// chip spends two sample times fetching each syllable's two bytes (the DAC
// at 7).
//
// The delta of a code depends on the high bit of the code before it:
//
//	previous code 0x: -3 -1  0 +1   (codes 0-3)
//	previous code 1x: -1  0 +1 +3
//
// The level stops at 0 and 15.
//
// This was worked out from the published descriptions of Mozer's method and
// the chip, then checked sample for sample against MAME's S14001A run as a
// black box on synthetic speech ROMs (no real speech data was used).
package s14001a

import "retropixels/cores/arcade"

var delta = [2][4]int8{{-3, -1, 0, 1}, {-1, 0, 1, 3}}

// what the chip is doing
const (
	stIdle  = iota
	stFetch // reading a word table entry or a syllable's two bytes
	stPlay
)

// Chip is one S14001A with its speech ROM.
type Chip struct {
	rom []byte // 4 KB (shorter ROMs are mirrored)

	state int
	wait  int    // sample times left in stFetch
	list  uint16 // address of the next syllable's two bytes
	data  uint16 // address of the current period's data
	param uint8  // the syllable's G B Y S S S R R byte
	count int    // periods played in this syllable
	pos   int    // sample in the period (0-127)
	level int    // the decoder's level (0-15)
	prev  uint8  // the code before
	out   int    // the DAC's level
}

// New makes the chip. rom is its speech ROM (up to 4 KB; nil = no ROM, and
// every word is silence).
func New(rom []byte) *Chip {
	if len(rom) == 0 {
		rom = []byte{0}
	}
	c := &Chip{rom: rom}
	c.Reset()
	return c
}

// Reset stops speaking.
func (c *Chip) Reset() {
	c.state, c.wait, c.list, c.data, c.param = stIdle, 0, 0, 0, 0
	c.count, c.pos, c.level, c.prev, c.out = 0, 0, 7, 2, 7
}

// Start begins speaking word w (0-63), stopping any word being spoken.
func (c *Chip) Start(w int) {
	w &= 63
	a := uint16(c.byteAt(uint16(2*w)))<<8 | uint16(c.byteAt(uint16(2*w+1)))
	c.list = a >> 4
	c.state, c.wait = stFetch, 4 // the word table entry, then the syllable
	c.out = 7
}

// Busy is true while the chip speaks (its /BUSY pin is low).
func (c *Chip) Busy() bool { return c.state != stIdle }

func (c *Chip) byteAt(a uint16) uint8 { return c.rom[int(a&0xfff)%len(c.rom)] }

// code is the 2-bit code k (0-127) of the period's data.
func (c *Chip) code(k int) uint8 {
	return c.byteAt(c.data+uint16(k>>2)) >> (6 - 2*(k&3)) & 3
}

func (c *Chip) voiced() bool { return c.param&0x40 != 0 }

// periods is how many periods the syllable plays, and stored how many of
// them are new data (voiced: each stored period is played reps times).
func (c *Chip) periods() (total, reps int) {
	s, r := int(c.param>>2&7), int(c.param&3)
	if c.voiced() {
		return (8 - s) * (4 - r), 4 - r
	}
	return 32 - int(c.param&0x1f), 1
}

// Next makes one sample and returns the DAC level (0-15, 7 = silence).
func (c *Chip) Next() int {
	switch c.state {
	case stIdle:
		c.out = 7
	case stFetch:
		c.out = 7
		if c.wait--; c.wait <= 0 {
			c.data = uint16(c.byteAt(c.list)) << 4
			c.param = c.byteAt(c.list + 1)
			c.list += 2
			c.count, c.pos = 0, 0
			c.state = stPlay
		}
	case stPlay:
		c.play()
	}
	return c.out
}

func (c *Chip) step(code uint8, sign int) {
	c.level += sign * int(delta[c.prev>>1][code])
	c.level = min(max(c.level, 0), 15)
}

func (c *Chip) play() {
	k := c.pos
	if k == 0 {
		c.level, c.prev = 7, 2
	}
	audible := true
	switch {
	case !c.voiced() || k < 32: // forwards
		code := c.code(k)
		c.step(code, 1)
		c.prev = code
	case k == 32: // the mirror point: the last level again
	case k < 64: // backwards: undo the steps in reverse order
		code := c.code(63 - k)
		next := c.prev
		c.prev = code
		c.level -= int(delta[code>>1][next])
		c.level = min(max(c.level, 0), 15)
	default: // the zeroed half
		audible = false
	}
	c.out = 7
	if audible && c.param&0x20 == 0 {
		c.out = c.level
	}
	if c.pos++; c.pos < 128 {
		return
	}
	// end of the period
	c.pos = 0
	c.count++
	total, reps := c.periods()
	if !c.voiced() {
		c.data += 32
	} else if c.count%reps == 0 {
		c.data += 8
	}
	if c.count < total {
		return
	}
	if c.param&0x80 != 0 {
		c.state = stIdle
		return
	}
	c.state, c.wait = stFetch, 2
}

// Save appends the chip's state.
func (c *Chip) Save(w *arcade.StateW) {
	w.U8(uint8(c.state))
	w.Int(c.wait)
	w.U16(c.list)
	w.U16(c.data)
	w.U8(c.param)
	w.Int(c.count)
	w.Int(c.pos)
	w.Int(c.level)
	w.U8(c.prev)
	w.Int(c.out)
}

// Load restores what Save wrote.
func (c *Chip) Load(r *arcade.StateR) {
	c.state = int(r.U8())
	c.wait = r.Int()
	c.list = r.U16()
	c.data = r.U16()
	c.param = r.U8()
	c.count = r.Int()
	c.pos = r.Int() & 127
	c.level = min(max(r.Int(), 0), 15)
	c.prev = r.U8() & 3
	c.out = min(max(r.Int(), 0), 15)
	if c.state > stPlay {
		c.state = stIdle
	}
}
