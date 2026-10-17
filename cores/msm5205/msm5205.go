// Package msm5205 is the OKI MSM5205 ADPCM speech synthesizer: one voice
// that turns 4-bit (or 3-bit) ADPCM codes into a 12-bit signal.
//
// The chip has no memory of its own. A clock (384 kHz typically) is divided
// by 96, 64 or 48 (the S1/S2 pins) into the VCK signal; at each VCK the chip
// decodes the code on its data pins, and the machine (a CPU interrupt or a
// counter reading a ROM) puts the next code there. RESET holds the decoder
// at zero.
//
//	adpcm := msm5205.New(384000, 48, 4)
//	adpcm.SetReset(false)
//	adpcm.Write(code)       // the data pins
//	s := adpcm.Clock()      // one VCK: decode; returns the signal
package msm5205

import (
	"encoding/binary"
	"errors"
)

// steps is the ADPCM step size table (16 * 1.1^n, as the chip has it).
var steps = [49]int{
	16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
	1552,
}

var shift4 = [8]int{-1, -1, -1, -1, 2, 4, 6, 8}
var shift3 = [4]int{-1, -1, 2, 4}

// Chip is one MSM5205.
type Chip struct {
	clock float64
	div   int
	bits  int

	data   uint8
	reset  bool
	signal int // -2048..2047
	step   int // index into steps
}

// New makes a chip on a clock of clock Hz with the VCK divider div (96, 64
// or 48) and bits-bit codes (4 or 3). It starts out of reset.
func New(clock float64, div, bits int) *Chip {
	if bits != 3 {
		bits = 4
	}
	return &Chip{clock: clock, div: div, bits: bits}
}

// Rate is the VCK frequency: how often Clock should be called.
func (c *Chip) Rate() float64 { return c.clock / float64(c.div) }

// SetDivider changes the S1/S2 setting (96, 64 or 48) and the code size.
func (c *Chip) SetDivider(div, bits int) {
	c.div = div
	if bits == 3 {
		c.bits = 3
	} else {
		c.bits = 4
	}
}

// Reset clears the decoder (the chip's power-on state).
func (c *Chip) Reset() {
	c.signal, c.step, c.data = 0, 0, 0
}

// SetReset sets the RESET pin: while it is high the output is held at zero.
func (c *Chip) SetReset(on bool) {
	c.reset = on
	if on {
		c.signal, c.step = 0, 0
	}
}

// InReset tells whether RESET is high.
func (c *Chip) InReset() bool { return c.reset }

// Write puts a code on the data pins.
func (c *Chip) Write(code uint8) { c.data = code & 0x0f }

// Clock is one VCK: the code on the data pins is decoded (unless RESET is
// high) and the new signal returned.
func (c *Chip) Clock() int {
	if c.reset {
		c.signal, c.step = 0, 0
		return 0
	}
	st := steps[c.step]
	var d int
	if c.bits == 4 {
		n := int(c.data)
		d = st >> 3
		if n&1 != 0 {
			d += st >> 2
		}
		if n&2 != 0 {
			d += st >> 1
		}
		if n&4 != 0 {
			d += st
		}
		if n&8 != 0 {
			d = -d
		}
		c.step += shift4[n&7]
	} else {
		n := int(c.data & 7)
		d = st >> 2
		if n&1 != 0 {
			d += st >> 1
		}
		if n&2 != 0 {
			d += st
		}
		if n&4 != 0 {
			d = -d
		}
		c.step += shift3[n&3]
	}
	c.signal += d
	if c.signal > 2047 {
		c.signal = 2047
	} else if c.signal < -2048 {
		c.signal = -2048
	}
	if c.step < 0 {
		c.step = 0
	} else if c.step > 48 {
		c.step = 48
	}
	return c.signal
}

// Output is the current signal (-2048..2047).
func (c *Chip) Output() int { return c.signal }

// ---- save states ----

// StateSize is the length of a saved chip state.
const StateSize = 12

var errState = errors.New("msm5205: damaged save state")

// AppendState appends the chip's state to b.
func (c *Chip) AppendState(b []byte) []byte {
	var fl uint8
	if c.reset {
		fl = 1
	}
	b = append(b, 1, c.data, fl, uint8(c.div), uint8(c.bits), uint8(c.step))
	b = binary.LittleEndian.AppendUint16(b, uint16(int16(c.signal)))
	return append(b, 0, 0, 0, 0)
}

// LoadState reads a state written by AppendState and returns the rest of b.
func (c *Chip) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != 1 || b[5] > 48 {
		return b, errState
	}
	c.data, c.reset, c.div, c.bits, c.step = b[1], b[2] != 0, int(b[3]), int(b[4]), int(b[5])
	c.signal = int(int16(binary.LittleEndian.Uint16(b[6:])))
	return b[StateSize:], nil
}
