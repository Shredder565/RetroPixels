package nds

import "encoding/binary"

// spu is the ARM7's sound unit: 16 channels of 8-bit or 16-bit PCM or
// IMA-ADPCM samples, square waves (channels 8-13) and noise (14-15), mixed
// with panning and the master volume. It makes samples straight at the
// player's rate.
type spu struct {
	m    *Machine
	ch   [16]sndChannel
	cnt  uint16
	bias uint16
	capt [2]struct {
		cnt      uint8
		dad, len uint32
	}

	rate   int
	period float64 // ARM9 cycles per output sample
	at     int64   // time of the last output sample
	frac   float64
	out    []float32
}

type sndChannel struct {
	cnt      uint32
	sad      uint32
	tmr      uint16
	pnt      uint16
	length   uint32
	pos      int     // sample index
	ticks    float64 // ARM9 cycles toward the next sample
	sample   int32
	adpcmVal int32
	adpcmIdx int
	loopVal  int32
	loopIdx  int
	lfsr     uint16
}

func newSPU(m *Machine, rate int) *spu {
	return &spu{m: m, rate: rate, period: float64(ARM9Hz) / float64(rate)}
}

func (s *spu) take() []float32 {
	s.runTo(s.m.now)
	o := s.out
	s.out = nil
	return o
}

func (s *spu) read8(off uint32) uint8 {
	switch {
	case off < 0x100:
		c := &s.ch[off>>4]
		switch o := off & 15; {
		case o < 4:
			return uint8(c.cnt >> (o * 8))
		}
		return 0
	case off == 0x100:
		return uint8(s.cnt)
	case off == 0x101:
		return uint8(s.cnt >> 8)
	case off == 0x104:
		return uint8(s.bias)
	case off == 0x105:
		return uint8(s.bias >> 8)
	case off == 0x108:
		return s.capt[0].cnt
	case off == 0x109:
		return s.capt[1].cnt
	}
	return 0
}

func (s *spu) read32(off uint32) uint32 {
	return uint32(s.read8(off)) | uint32(s.read8(off+1))<<8 | uint32(s.read8(off+2))<<16 | uint32(s.read8(off+3))<<24
}

func (s *spu) write8(off uint32, v uint8) {
	s.runTo(s.m.arm7.Cycles * 2)
	switch {
	case off < 0x100:
		c := &s.ch[off>>4]
		o := off & 15
		switch {
		case o < 4:
			old := c.cnt
			sh := o * 8
			c.cnt = c.cnt&^(0xff<<sh) | uint32(v)<<sh
			if old&(1<<31) == 0 && c.cnt&(1<<31) != 0 {
				s.start(int(off >> 4))
			}
		case o < 8:
			sh := (o - 4) * 8
			c.sad = (c.sad&^(0xff<<sh) | uint32(v)<<sh) & 0x07fffffc
		case o < 10:
			sh := (o - 8) * 8
			c.tmr = c.tmr&^(0xff<<sh) | uint16(v)<<sh
		case o < 12:
			sh := (o - 10) * 8
			c.pnt = c.pnt&^(0xff<<sh) | uint16(v)<<sh
		default:
			sh := (o - 12) * 8
			c.length = (c.length&^(0xff<<sh) | uint32(v)<<sh) & 0x3fffff
		}
	case off == 0x100:
		s.cnt = s.cnt&0xff00 | uint16(v)
	case off == 0x101:
		s.cnt = s.cnt&0xff | uint16(v)<<8
	case off == 0x104:
		s.bias = s.bias&0xff00 | uint16(v)
	case off == 0x105:
		s.bias = (s.bias&0xff | uint16(v)<<8) & 0x3ff
	case off == 0x108, off == 0x109:
		s.capt[off-0x108].cnt = v
	case off >= 0x110 && off < 0x120:
		k := (off - 0x110) / 8
		sh := (off & 3) * 8
		if off&4 == 0 {
			s.capt[k].dad = s.capt[k].dad&^(0xff<<sh) | uint32(v)<<sh
		} else {
			s.capt[k].len = s.capt[k].len&^(0xff<<sh) | uint32(v)<<sh
		}
	}
}

func (s *spu) write32(off, v uint32) {
	for i := uint32(0); i < 4; i++ {
		s.write8(off+i, uint8(v>>(8*i)))
	}
}

func (s *spu) start(n int) {
	c := &s.ch[n]
	c.pos = 0
	c.ticks = 0
	c.sample = 0
	c.lfsr = 0x7fff
	if c.cnt>>29&3 == 2 { // ADPCM header
		h := s.m.bus7.Read32(c.sad)
		c.adpcmVal = int32(int16(h))
		c.adpcmIdx = int(h >> 16 & 0x7f)
		if c.adpcmIdx > 88 {
			c.adpcmIdx = 88
		}
		c.loopVal, c.loopIdx = c.adpcmVal, c.adpcmIdx
		c.pos = 8 // nibbles: the header's 4 bytes come first
	}
	c.fetch(s, n)
}

var adpcmSteps = func() (t [89]int32) {
	// the IMA-ADPCM step table: 7 growing by ~10% each step, capped at 32767
	v := 7.0
	for i := range t {
		t[i] = int32(v + 0.5)
		v *= 1.1
	}
	// the standard table's exact values
	copy(t[:], []int32{7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
		50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
		337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
		1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
		7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
		24623, 27086, 29794, 32767})
	return
}()

var adpcmIndex = [8]int{-1, -1, -1, -1, 2, 4, 6, 8}

// fetch loads the sample at c.pos.
func (c *sndChannel) fetch(s *spu, n int) {
	bus := s.m.bus7
	switch c.cnt >> 29 & 3 {
	case 0:
		c.sample = int32(int8(bus.Read8(c.sad+uint32(c.pos)))) << 8
	case 1:
		c.sample = int32(int16(bus.Read16(c.sad + uint32(c.pos)*2)))
	case 2:
		b := bus.Read8(c.sad + uint32(c.pos)/2)
		nib := int32(b>>(uint32(c.pos)&1*4)) & 15
		step := adpcmSteps[c.adpcmIdx]
		diff := step >> 3
		if nib&1 != 0 {
			diff += step >> 2
		}
		if nib&2 != 0 {
			diff += step >> 1
		}
		if nib&4 != 0 {
			diff += step
		}
		if nib&8 != 0 {
			c.adpcmVal -= diff
			if c.adpcmVal < -0x7fff {
				c.adpcmVal = -0x7fff
			}
		} else {
			c.adpcmVal += diff
			if c.adpcmVal > 0x7fff {
				c.adpcmVal = 0x7fff
			}
		}
		c.adpcmIdx += adpcmIndex[nib&7]
		if c.adpcmIdx < 0 {
			c.adpcmIdx = 0
		}
		if c.adpcmIdx > 88 {
			c.adpcmIdx = 88
		}
		c.sample = c.adpcmVal
		if c.pos == int(c.pnt)*8 { // the loop start: remember the state
			c.loopVal, c.loopIdx = c.adpcmVal, c.adpcmIdx
		}
	case 3:
		switch {
		case n >= 8 && n <= 13:
			duty := int(c.cnt >> 24 & 7)
			if c.pos&7 > 6-duty {
				c.sample = 0x7fff
			} else {
				c.sample = -0x7fff
			}
		case n >= 14:
			bit := c.lfsr & 1
			c.lfsr >>= 1
			if bit != 0 {
				c.lfsr ^= 0x6000
				c.sample = -0x7fff
			} else {
				c.sample = 0x7fff
			}
		default:
			c.sample = 0
		}
	}
}

// end: the number of samples in a channel's data (PNT+LEN words).
func (c *sndChannel) end() int {
	words := int(c.pnt) + int(c.length)
	switch c.cnt >> 29 & 3 {
	case 0:
		return words * 4
	case 1:
		return words * 2
	case 2:
		return words * 8
	}
	return 1 << 30
}

func (c *sndChannel) loopStart() int {
	switch c.cnt >> 29 & 3 {
	case 0:
		return int(c.pnt) * 4
	case 1:
		return int(c.pnt) * 2
	case 2:
		return int(c.pnt) * 8
	}
	return 0
}

func (c *sndChannel) advance(s *spu, n int) {
	c.pos++
	if c.pos >= c.end() {
		switch c.cnt >> 27 & 3 {
		case 1: // loop
			c.pos = c.loopStart()
			if c.cnt>>29&3 == 2 {
				c.adpcmVal, c.adpcmIdx = c.loopVal, c.loopIdx
				if c.pnt == 0 {
					c.pos = 8
				}
			}
		default: // one shot
			if c.cnt&(1<<15) == 0 {
				c.cnt &^= 1 << 31
			}
			c.sample = 0
			c.pos = c.end()
			return
		}
	}
	c.fetch(s, n)
}

var divShift = [4]uint{0, 1, 2, 4}

// runTo makes the output samples up to time t.
func (s *spu) runTo(t int64) {
	for float64(s.at)+s.period-s.frac <= float64(t) {
		next := float64(s.at) + s.period - s.frac
		ni := int64(next)
		s.frac = next - float64(ni)
		dt := float64(ni - s.at)
		s.at = ni
		var l, r int64
		on := s.cnt&0x8000 != 0
		for n := range s.ch {
			c := &s.ch[n]
			if c.cnt&(1<<31) == 0 {
				continue
			}
			per := float64(0x10000-int(c.tmr)) * 4
			if per <= 0 {
				per = 4
			}
			c.ticks += dt
			for c.ticks >= per && c.cnt&(1<<31) != 0 {
				c.ticks -= per
				c.advance(s, n)
			}
			if !on {
				continue
			}
			vol := int64(c.cnt & 0x7f)
			if vol == 127 {
				vol = 128
			}
			v := int64(c.sample) * vol >> divShift[c.cnt>>8&3]
			pan := int64(c.cnt >> 16 & 0x7f)
			if pan == 127 {
				pan = 128
			}
			l += v * (128 - pan) >> 7
			r += v * pan >> 7
		}
		master := int64(s.cnt & 0x7f)
		if master == 127 {
			master = 128
		}
		lf := float32(l*master>>14) / 32768
		rf := float32(r*master>>14) / 32768
		if !on {
			lf, rf = 0, 0
		}
		s.out = append(s.out, clampf(lf), clampf(rf))
	}
}

func clampf(v float32) float32 {
	if v > 1 {
		return 1
	}
	if v < -1 {
		return -1
	}
	return v
}

var _ = binary.LittleEndian
