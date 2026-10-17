// Package pokey is Atari's POKEY chip: four sound channels, the random
// number generator and the pot (paddle) inputs. The Atari 7800 core and the
// Atari arcade boards share it.
//
// The chip is clocked at the rate its machine gives it (1.79 MHz on the
// 7800, 1.512 MHz on the arcade boards); Run takes that many clocks.
package pokey

import (
	"encoding/binary"
	"errors"
)

func lfsr(n, tap int) []uint8 {
	size := (1 << n) - 1
	out := make([]uint8, size)
	r := size
	for i := range out {
		out[i] = uint8(r & 1)
		fb := (r ^ (r >> tap)) & 1
		r = (r >> 1) | (fb << (n - 1))
	}
	return out
}

var (
	p4  = lfsr(4, 1)
	p5  = lfsr(5, 2)
	p9  = lfsr(9, 4)
	p17 = lfsr(17, 5)
)

// Chip is one POKEY. It holds no pointers, so a copy is an independent chip.
type Chip struct {
	audf, audc [4]int
	audctl     int
	cnt, out   [4]int
	hp         [2]int
	t          int64 // clocks run (for the random generators)
	base       int

	// Pot is what the pot registers read: 0-7 are POT0-POT7, 8 is ALLPOT.
	// The machine sets them (an arcade board wires switches to them); they
	// read $FF when unused. They are inputs, not saved state.
	Pot [9]uint8

	// HoldOnReset: SKCTL's low two bits at 0 hold the chip in its reset
	// state, so RANDOM stops changing (Centipede checks this at power-on).
	// The 7800 core leaves it off, as its reference player does.
	HoldOnReset bool
	skctl       uint8
	held        uint8 // what RANDOM reads while held
	rel         int64 // the clock the chip last left reset (WriteAt)

	// SyncEvery[0] > 0 makes RANDOM read the way MAME's POKEY does, so a
	// game that draws a random screen at power-on draws MAME's: the
	// generator's value as of the last sync point, not as of the read. Sync
	// points are every access through Read or WriteAt, every Sync call, and
	// every SyncEvery[0] and SyncEvery[1] (if set) clocks counted from
	// SyncBase: the moments MAME brings the chip up to date (the real chip's
	// generator is read as it runs). The 7800 core leaves it off.
	SyncEvery [2]int64
	SyncBase  int64
	lastSync  int64
}

// New makes a chip.
func New() *Chip {
	c := &Chip{cnt: [4]int{1, 1, 1, 1}}
	for i := range c.Pot {
		c.Pot[i] = 0xff
	}
	return c
}

// Write writes register r (0-15).
func (p *Chip) Write(r int, v uint8) {
	switch r & 15 {
	case 0, 2, 4, 6:
		p.audf[(r&15)>>1] = int(v)
	case 1, 3, 5, 7:
		p.audc[(r&15)>>1] = int(v)
	case 8:
		p.audctl = int(v)
	case 9: // STIMER
		for i := 0; i < 4; i++ {
			p.cnt[i] = p.period(i)
		}
	case 15: // SKCTL
		p.skctl = v
		p.held = 0xff
	}
}

// WriteAt is Write at clock now: it records the moment the chip leaves
// reset (HoldOnReset) and is a sync point (SyncEvery).
func (p *Chip) WriteAt(r int, v uint8, now int64) {
	if r&15 == 15 && p.HoldOnReset && p.skctl&3 == 0 && v&3 != 0 {
		p.rel = now
	}
	p.Write(r, v)
	p.Sync(now)
}

// Sync marks a sync point at clock now (see SyncEvery).
func (p *Chip) Sync(now int64) {
	if p.SyncEvery[0] > 0 && now > p.lastSync {
		p.lastSync = now
	}
}

// Read reads register r: the pots, ALLPOT and RANDOM. now is the chip's
// clocks since power-on (the random generator runs all the time).
func (p *Chip) Read(r int, now int64) uint8 {
	defer p.Sync(now)
	switch r &= 15; {
	case r <= 8:
		return p.Pot[r]
	case r == 10: // RANDOM
		if p.HoldOnReset && p.skctl&3 == 0 {
			return p.held
		}
		t := now
		if p.SyncEvery[0] > 0 {
			t = p.lastSync
			for _, e := range p.SyncEvery {
				if e > 0 {
					t = max(t, now-(now-p.SyncBase)%e)
				}
			}
		}
		if p.HoldOnReset { // the generators restart from all ones
			t -= p.rel - 1
		}
		var v uint8
		if p.audctl&0x80 != 0 {
			t = (t%511 + 511) % 511
			for i := int64(0); i < 8; i++ {
				v |= p9[(t+i)%511] << i
			}
		} else {
			t = (t%131071 + 131071) % 131071
			for i := int64(0); i < 8; i++ {
				v |= p17[(t+i)%131071] << i
			}
		}
		return v
	}
	return 0xff
}

func (p *Chip) period(ch int) int {
	ac, f := p.audctl, &p.audf
	if ch == 1 && ac&0x10 != 0 {
		if ac&0x40 != 0 {
			return (f[1]<<8 | f[0]) + 7
		}
		return (f[1]<<8 | f[0]) + 1
	}
	if ch == 3 && ac&0x08 != 0 {
		if ac&0x20 != 0 {
			return (f[3]<<8 | f[2]) + 7
		}
		return (f[3]<<8 | f[2]) + 1
	}
	if (ch == 0 && ac&0x40 != 0) || (ch == 2 && ac&0x20 != 0) {
		return f[ch] + 4
	}
	return f[ch] + 1
}

func (p *Chip) pulse(ch int) {
	c, t := p.audc[ch], p.t
	if c&0x80 == 0 && p5[t%31] == 0 {
		return
	}
	switch {
	case c&0x20 != 0:
		p.out[ch] ^= 1
	case c&0x40 != 0:
		p.out[ch] = int(p4[t%15])
	case p.audctl&0x80 != 0:
		p.out[ch] = int(p9[t%511])
	default:
		p.out[ch] = int(p17[t%131071])
	}
}

// Run runs n clocks and returns the average output (0..1).
func (p *Chip) Run(n int) float64 {
	ac := p.audctl
	j12, j34, f0, f2 := ac&0x10 != 0, ac&0x08 != 0, ac&0x40 != 0, ac&0x20 != 0
	baseDiv := 28
	if ac&1 != 0 {
		baseDiv = 114
	}
	sum := 0
	for i := 0; i < n; i++ {
		p.t++
		p.base++
		base := p.base >= baseDiv
		if base {
			p.base = 0
		}
		// channel 1 (and the 1+2 pair)
		if f0 || base {
			if j12 {
				p.cnt[1]--
				if p.cnt[1] <= 0 {
					p.cnt[1] = p.period(1)
					p.pulse(1)
				}
			} else {
				p.cnt[0]--
				if p.cnt[0] <= 0 {
					p.cnt[0] = p.period(0)
					p.pulse(0)
				}
			}
		}
		if !j12 && base {
			p.cnt[1]--
			if p.cnt[1] <= 0 {
				p.cnt[1] = p.period(1)
				p.pulse(1)
			}
		}
		// channel 3 (and the 3+4 pair)
		if f2 || base {
			if j34 {
				p.cnt[3]--
				if p.cnt[3] <= 0 {
					p.cnt[3] = p.period(3)
					p.pulse(3)
				}
			} else {
				p.cnt[2]--
				if p.cnt[2] <= 0 {
					p.cnt[2] = p.period(2)
					p.pulse(2)
					if ac&4 != 0 {
						p.hp[0] = p.out[0]
					}
				}
			}
		}
		if !j34 && base {
			p.cnt[3]--
			if p.cnt[3] <= 0 {
				p.cnt[3] = p.period(3)
				p.pulse(3)
				if ac&2 != 0 {
					p.hp[1] = p.out[1]
				}
			}
		}
		if i&3 == 0 || i == n-1 {
			s := 0
			for ch := 0; ch < 4; ch++ {
				c := p.audc[ch]
				vol := c & 15
				if vol == 0 {
					continue
				}
				if (j12 && ch == 0) || (j34 && ch == 2) {
					continue
				}
				bit := p.out[ch]
				if c&0x10 != 0 {
					bit = 1
				}
				if ch == 0 && ac&4 != 0 {
					bit ^= p.hp[0]
				}
				if ch == 1 && ac&2 != 0 {
					bit ^= p.hp[1]
				}
				if bit != 0 {
					s += vol
				}
			}
			if i == n-1 {
				sum += s * ((n-1)%4 + 1)
			} else {
				sum += s * 4
			}
		}
	}
	return float64(sum) / float64(n) / 60
}

// ErrBadState is LoadState's error for a short or damaged state.
var ErrBadState = errors.New("pokey: bad saved state")

// AppendState appends the chip's state to b: each list as a 32-bit count and
// 64-bit values, then the clock count and the base divider (the layout the
// 7800 core's states have always used).
func (p *Chip) AppendState(b []byte) []byte {
	ints := func(v []int) {
		b = binary.LittleEndian.AppendUint32(b, uint32(len(v)))
		for _, x := range v {
			b = binary.LittleEndian.AppendUint64(b, uint64(int64(x)))
		}
	}
	ints(p.audf[:])
	ints(p.audc[:])
	b = binary.LittleEndian.AppendUint64(b, uint64(int64(p.audctl)))
	ints(p.cnt[:])
	ints(p.out[:])
	ints(p.hp[:])
	b = binary.LittleEndian.AppendUint64(b, uint64(p.t))
	b = binary.LittleEndian.AppendUint64(b, uint64(int64(p.base)))
	if p.HoldOnReset {
		b = append(b, p.skctl, p.held)
		b = binary.LittleEndian.AppendUint64(b, uint64(p.rel))
		b = binary.LittleEndian.AppendUint64(b, uint64(p.lastSync))
		b = binary.LittleEndian.AppendUint64(b, uint64(p.SyncBase))
	}
	return b
}

// LoadState reads what AppendState wrote from the start of b and returns
// the rest of b.
func (p *Chip) LoadState(b []byte) ([]byte, error) {
	bad := false
	i64 := func() int64 {
		if len(b) < 8 {
			bad = true
			return 0
		}
		v := int64(binary.LittleEndian.Uint64(b))
		b = b[8:]
		return v
	}
	ints := func(dst []int) {
		if len(b) < 4 || int(binary.LittleEndian.Uint32(b)) != len(dst) {
			bad = true
			return
		}
		b = b[4:]
		for i := range dst {
			dst[i] = int(i64())
		}
	}
	n := *p
	ints(n.audf[:])
	ints(n.audc[:])
	n.audctl = int(i64())
	ints(n.cnt[:])
	ints(n.out[:])
	ints(n.hp[:])
	n.t = i64()
	n.base = int(i64())
	if p.HoldOnReset {
		if len(b) < 2 {
			bad = true
		} else {
			n.skctl, n.held, b = b[0], b[1], b[2:]
		}
		n.rel = i64()
		n.lastSync = i64()
		n.SyncBase = i64()
	}
	if bad {
		return b, ErrBadState
	}
	*p = n
	return b, nil
}
