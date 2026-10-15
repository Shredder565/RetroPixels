// Package sn76489 is Sega's SN76489-compatible sound chip (PSG): three
// square-wave channels and a noise channel. A Go port of web/emu/sn76489.js,
// used by the Master System (and the Genesis).
package sn76489

import (
	"errors"
	"math"
)

// vol: the chip's 2 dB volume steps (15 = off), as 32-bit floats like the
// JS core's Float32Array.
var vol [16]float32

func init() {
	for i := 0; i < 15; i++ {
		vol[i] = float32(math.Pow(10, float64(-i*2)/20))
	}
}

// PSG is one chip.
type PSG struct {
	reg   [8]uint16 // tone 0, vol 0, tone 1, vol 1, tone 2, vol 2, noise, vol 3
	cnt   [4]int32
	pol   [4]int8
	lfsr  int
	latch int
}

// New makes a chip, reset.
func New() *PSG {
	p := &PSG{}
	p.Reset()
	return p
}

// Reset silences the chip.
func (p *PSG) Reset() {
	p.reg = [8]uint16{}
	for c := 0; c < 4; c++ {
		p.reg[c*2+1] = 15
	}
	p.cnt = [4]int32{}
	p.pol = [4]int8{1, 1, 1, 1}
	p.lfsr = 0x8000
	p.latch = 0
}

// Write takes a byte written to the chip.
func (p *PSG) Write(v uint8) {
	if v&0x80 != 0 {
		p.latch = int(v>>4) & 7
		r := p.latch
		switch {
		case r&1 != 0:
			p.reg[r] = uint16(v & 15)
		case r == 6:
			p.reg[6] = uint16(v & 7)
			p.lfsr = 0x8000
		default:
			p.reg[r] = (p.reg[r] & 0x3f0) | uint16(v&15)
		}
		return
	}
	r := p.latch
	switch {
	case r&1 != 0:
		p.reg[r] = uint16(v & 15)
	case r == 6:
		p.reg[6] = uint16(v & 7)
		p.lfsr = 0x8000
	default:
		p.reg[r] = (p.reg[r] & 15) | uint16(v&0x3f)<<4
	}
}

// Step advances the chip one step (16 processor cycles on the Master System)
// and returns the mixed output.
func (p *PSG) Step() float64 {
	out := 0.0
	for c := 0; c < 3; c++ {
		per := int32(p.reg[c*2])
		p.cnt[c]--
		if p.cnt[c] <= 0 {
			p.cnt[c] = per
			p.pol[c] = -p.pol[c]
		}
		s := 1.0
		if per > 1 {
			s = float64(p.pol[c])
		}
		out += s * float64(vol[p.reg[c*2+1]])
	}
	nr := p.reg[6] & 3
	nper := int32(0x10) << nr
	if nr == 3 {
		nper = int32(p.reg[4])
	}
	p.cnt[3]--
	if p.cnt[3] <= 0 {
		p.cnt[3] = nper
		p.pol[3] = -p.pol[3]
		if p.pol[3] > 0 {
			var fb int
			if p.reg[6]&4 != 0 {
				fb = (p.lfsr ^ p.lfsr>>3) & 1
			} else {
				fb = p.lfsr & 1
			}
			p.lfsr = p.lfsr>>1 | fb<<15
		}
	}
	n := -1.0
	if p.lfsr&1 != 0 {
		n = 1
	}
	out += n * float64(vol[p.reg[7]]) * 0.5
	return out
}

// StateSize is the length of a saved chip state.
const StateSize = 16 + 16 + 4 + 4 + 1

var errState = errors.New("sn76489: damaged save state")

// AppendState appends the chip's state to b.
func (p *PSG) AppendState(b []byte) []byte {
	for _, r := range p.reg {
		b = append(b, byte(r), byte(r>>8))
	}
	for _, c := range p.cnt {
		b = append(b, byte(c), byte(c>>8), byte(c>>16), byte(c>>24))
	}
	for _, v := range p.pol {
		b = append(b, byte(v))
	}
	return append(b, byte(p.lfsr), byte(p.lfsr>>8), 0, 0, byte(p.latch))
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest.
func (p *PSG) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize {
		return b, errState
	}
	for i := range p.reg {
		p.reg[i] = uint16(b[i*2]) | uint16(b[i*2+1])<<8
	}
	for i := range p.cnt {
		o := 16 + i*4
		p.cnt[i] = int32(uint32(b[o]) | uint32(b[o+1])<<8 | uint32(b[o+2])<<16 | uint32(b[o+3])<<24)
	}
	for i := range p.pol {
		p.pol[i] = int8(b[32+i])
	}
	p.lfsr = int(b[36]) | int(b[37])<<8
	p.latch = int(b[40]) & 7
	return b[StateSize:], nil
}
