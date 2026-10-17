package berzerk

import "retropixels/cores/arcade"

// ptm is a Motorola 6840 programmable timer as the Berzerk sound board uses
// it: three counters whose outputs are square waves. Only what the board
// can see is modelled (no interrupts, gates or reads): each counter runs in
// continuous mode, as a 16-bit counter (half a wave every latch+1 clocks) or
// as two 8-bit counters (output high for the last LSB+1 of (MSB+1)*(LSB+1)
// clocks), from the E clock or from its external clock pin, and the third
// counter can divide its clock by 8 first.
type ptm struct {
	cr    [3]uint8  // control registers 1-3
	latch [3]uint16 // time constants
	count [3]uint16 // counters (in 8-bit mode: MSB and LSB counters)
	out   [3]bool   // output pins (before the output-enable bit)
	pre   uint8     // timer 3's divide-by-8 prescaler
	msb   uint8     // the MSB buffer register
}

// reset is the chip's reset: the counters held at their (maximum) latches,
// the outputs low.
func (p *ptm) reset() {
	*p = ptm{}
	p.cr[0] = 1
	for i := range p.latch {
		p.latch[i] = 0xffff
		p.count[i] = 0xffff
	}
}

// write is a write to register r (0-7).
func (p *ptm) write(r int, v uint8) {
	switch r {
	case 0: // control register 1 or 3, picked by bit 0 of control register 2
		if p.cr[1]&1 != 0 {
			old := p.cr[0]
			p.cr[0] = v
			if v&1 != 0 && old&1 == 0 { // internal reset: all counters back to their latches
				for i := range p.count {
					p.count[i] = p.latch[i]
					p.out[i] = false
				}
			}
		} else {
			p.cr[2] = v
		}
	case 1:
		p.cr[1] = v
	case 2, 4, 6:
		p.msb = v
	case 3, 5, 7:
		t := (r - 3) / 2
		p.latch[t] = uint16(p.msb)<<8 | uint16(v)
		// "initialise on latch write" mode (bit 4 clear) reloads the counter
		if p.cr[t]&0x10 == 0 || p.cr[0]&1 != 0 {
			p.count[t] = p.latch[t]
			if p.cr[0]&1 == 0 {
				p.out[t] = false
			}
		}
	}
}

// external reports whether timer t counts its external clock pin.
func (p *ptm) external(t int) bool { return p.cr[t]&2 == 0 }

// clock is one clock edge for timer t.
func (p *ptm) clock(t int) {
	if p.cr[0]&1 != 0 {
		return // held in reset
	}
	if t == 2 && p.cr[2]&1 != 0 { // divide by 8
		p.pre++
		if p.pre&7 != 0 {
			return
		}
	}
	c := p.count[t]
	if p.cr[t]&4 == 0 { // 16-bit mode
		if c == 0 {
			p.count[t] = p.latch[t]
			p.out[t] = !p.out[t]
		} else {
			p.count[t] = c - 1
		}
		return
	}
	// dual 8-bit mode: the LSB counts clocks, the MSB counts LSB time-outs
	lsb, msb := uint8(c), uint8(c>>8)
	if lsb == 0 {
		lsb = uint8(p.latch[t])
		if msb == 0 {
			msb = uint8(p.latch[t] >> 8)
		} else {
			msb--
		}
	} else {
		lsb--
	}
	p.count[t] = uint16(msb)<<8 | uint16(lsb)
	p.out[t] = msb == 0
}

// level is timer t's output pin: its output bit, if enabled.
func (p *ptm) level(t int) bool { return p.out[t] && p.cr[t]&0x80 != 0 }

func (p *ptm) save(w *arcade.StateW) {
	w.Bytes(p.cr[:])
	w.U16s(p.latch[:])
	w.U16s(p.count[:])
	for _, o := range p.out {
		w.Bool(o)
	}
	w.U8(p.pre)
	w.U8(p.msb)
}

func (p *ptm) load(r *arcade.StateR) {
	r.Bytes(p.cr[:])
	r.U16s(p.latch[:])
	r.U16s(p.count[:])
	for i := range p.out {
		p.out[i] = r.Bool()
	}
	p.pre = r.U8()
	p.msb = r.U8()
}
