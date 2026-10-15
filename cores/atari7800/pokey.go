package atari7800

// POKEY sound (4 channels), clocked at the processor rate (1.79 MHz).

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

type pokey struct {
	audf, audc [4]int
	audctl     int
	cnt, out   [4]int
	hp         [2]int
	t          int64 // clocks run (for the random generators)
	base       int
}

func newPOKEY() *pokey {
	return &pokey{cnt: [4]int{1, 1, 1, 1}}
}

func (p *pokey) write(r int, v uint8) {
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
	}
}

// read: now is processor clocks since power-on (the random generator runs all the time).
func (p *pokey) read(r int, now int64) uint8 {
	if r&15 == 10 { // RANDOM
		t := now
		var v uint8
		if p.audctl&0x80 != 0 {
			for i := int64(0); i < 8; i++ {
				v |= p9[(t+i)%511] << i
			}
		} else {
			for i := int64(0); i < 8; i++ {
				v |= p17[(t+i)%131071] << i
			}
		}
		return v
	}
	return 0xff
}

func (p *pokey) period(ch int) int {
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

func (p *pokey) pulse(ch int) {
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

// run runs n processor clocks and returns the average output (0..1).
func (p *pokey) run(n int) float64 {
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

func (p *pokey) save(w *stateW) {
	w.ints(p.audf[:])
	w.ints(p.audc[:])
	w.int(p.audctl)
	w.ints(p.cnt[:])
	w.ints(p.out[:])
	w.ints(p.hp[:])
	w.i64(p.t)
	w.int(p.base)
}

func (p *pokey) load(r *stateR) {
	r.intsInto(p.audf[:])
	r.intsInto(p.audc[:])
	p.audctl = r.int()
	r.intsInto(p.cnt[:])
	r.intsInto(p.out[:])
	r.intsInto(p.hp[:])
	p.t = r.i64()
	p.base = r.int()
}
