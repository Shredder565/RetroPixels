// Package msm6295 is the OKI MSM6295 4-voice ADPCM speech/sample chip used
// by many arcade boards (Capcom CPS-1 and others).
//
// The chip plays "phrases" from its sample ROM: the ROM starts with a table
// of 128 eight-byte entries (an 18-bit start and end address each; entry 0 is
// unused), and the phrases are 4-bit OKI ADPCM, high nibble first. The CPU
// talks to it through one byte port:
//
//   - a write with bit 7 set picks phrase (v & 0x7f); the next write's high
//     nibble says which voice(s) play it (bit 4 = voice 0 ... bit 7 = voice
//     3) and its low nibble the attenuation (3 dB steps);
//   - any other write stops the voices in bits 3-6 (bit 3 = voice 0);
//   - a read gives 0xf0 | one bit per voice that is playing.
//
// The chip makes one sample every clock/132 (pin 7 high) or clock/165 (pin 7
// low) cycles; Sample makes one.
//
//	oki := msm6295.New(rom)
//	oki.Write(v)
//	s := oki.Sample() // about -8192..8191 (four voices of 12 bits)
package msm6295

import (
	"encoding/binary"
	"errors"
)

// steps is the ADPCM step size table (16 * 1.1^n).
var steps [49]int

// diffs: the difference each nibble makes at each step size.
var diffs [49 * 16]int

var indexShift = [8]int{-1, -1, -1, -1, 2, 4, 6, 8}

// volume: the 16 attenuation settings as fractions of 32 (3 dB apart; the
// last 7 are silent).
var volume = [16]int{32, 22, 16, 11, 8, 6, 4, 3, 2, 0, 0, 0, 0, 0, 0, 0}

func init() {
	v := 16.0
	for i := range steps {
		steps[i] = int(v)
		v *= 1.1
	}
	for s := 0; s < 49; s++ {
		for n := 0; n < 16; n++ {
			// (2 * magnitude + 1) * step / 8, signed by bit 3
			d := steps[s] * ((n&7)*2 + 1) / 8
			if n&8 != 0 {
				d = -d
			}
			diffs[s*16+n] = d
		}
	}
}

type voice struct {
	playing bool
	addr    int // nibble address being played
	end     int // last nibble address
	signal  int // 12-bit decoder output
	step    int // step table index
	vol     int // 0..32
}

// Chip is one MSM6295.
type Chip struct {
	rom    []byte
	voices [4]voice
	phrase int // phrase waiting for its voice byte, or -1
}

// New makes a chip that plays from rom (up to 256 KB is addressed).
func New(rom []byte) *Chip {
	c := &Chip{rom: rom}
	c.Reset()
	return c
}

// Reset silences every voice.
func (c *Chip) Reset() {
	c.voices = [4]voice{}
	c.phrase = -1
}

func (c *Chip) byteAt(a int) int {
	a &= 0x3ffff
	if a < len(c.rom) {
		return int(c.rom[a])
	}
	return 0
}

// Read is the status: 0xf0 | bit n set while voice n plays.
func (c *Chip) Read() uint8 {
	v := uint8(0xf0)
	for i := range c.voices {
		if c.voices[i].playing {
			v |= 1 << i
		}
	}
	return v
}

// Write is a command byte from the CPU.
func (c *Chip) Write(v uint8) {
	if c.phrase >= 0 {
		p := c.phrase
		c.phrase = -1
		at := p * 8
		start := c.byteAt(at)<<16 | c.byteAt(at+1)<<8 | c.byteAt(at+2)
		end := c.byteAt(at+3)<<16 | c.byteAt(at+4)<<8 | c.byteAt(at+5)
		start &= 0x3ffff
		end &= 0x3ffff
		for i := 0; i < 4; i++ {
			if v&(0x10<<i) == 0 {
				continue
			}
			vc := &c.voices[i]
			if vc.playing || start > end {
				// a voice that is busy ignores the command
				continue
			}
			*vc = voice{playing: true, addr: start * 2, end: end*2 + 1, vol: volume[v&15]}
		}
		return
	}
	if v&0x80 != 0 {
		c.phrase = int(v & 0x7f)
		return
	}
	for i := 0; i < 4; i++ {
		if v&(0x08<<i) != 0 {
			c.voices[i].playing = false
		}
	}
}

// Sample decodes one nibble on every playing voice and returns their sum.
func (c *Chip) Sample() int {
	out := 0
	for i := range c.voices {
		vc := &c.voices[i]
		if !vc.playing {
			continue
		}
		b := c.byteAt(vc.addr >> 1)
		n := b >> 4
		if vc.addr&1 != 0 {
			n = b & 15
		}
		vc.signal += diffs[vc.step*16+n]
		if vc.signal > 2047 {
			vc.signal = 2047
		} else if vc.signal < -2048 {
			vc.signal = -2048
		}
		vc.step += indexShift[n&7]
		if vc.step < 0 {
			vc.step = 0
		} else if vc.step > 48 {
			vc.step = 48
		}
		out += vc.signal * vc.vol / 32
		vc.addr++
		if vc.addr > vc.end {
			vc.playing = false
		}
	}
	return out
}

// ---- save states ----

// stateSize is the length of a saved chip state.
const stateSize = 4*6*4 + 4

var errState = errors.New("msm6295: damaged save state")

// AppendState appends the chip's state to b.
func (c *Chip) AppendState(b []byte) []byte {
	put := func(v int) { b = binary.LittleEndian.AppendUint32(b, uint32(int32(v))) }
	for i := range c.voices {
		vc := &c.voices[i]
		p := 0
		if vc.playing {
			p = 1
		}
		put(p)
		put(vc.addr)
		put(vc.end)
		put(vc.signal)
		put(vc.step)
		put(vc.vol)
	}
	put(c.phrase)
	return b
}

// LoadState reads what AppendState wrote and returns the rest of b.
func (c *Chip) LoadState(b []byte) ([]byte, error) {
	if len(b) < stateSize {
		return b, errState
	}
	get := func() int {
		v := int(int32(binary.LittleEndian.Uint32(b)))
		b = b[4:]
		return v
	}
	for i := range c.voices {
		vc := &c.voices[i]
		vc.playing = get() != 0
		vc.addr = get()
		vc.end = get()
		vc.signal = get()
		vc.step = get()
		vc.vol = get()
	}
	c.phrase = get()
	return b, nil
}
