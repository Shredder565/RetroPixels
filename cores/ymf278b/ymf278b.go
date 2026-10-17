// Package ymf278b is RetroPixels' Yamaha YMF278B (OPL4): a 24-channel
// wavetable (PCM) synthesizer that plays 8, 12 or 16-bit samples from its
// wave memory, together with the OPL3 FM synthesizer (fm.go), mixed into
// one stereo output. One sample comes out every 768 clocks.
//
// Ports (A2-A0): 0/1 FM bank 0 address/data, 2/3 FM bank 1 address/data,
// 4/5 wavetable address/data; reading port 0 gives the status.
//
// Wavetable registers:
//
//	02        wave table header (bits 4-2), memory access mode (bit 0)
//	03-05     memory address for 06; 06 memory data
//	08-1f     wave number, low 8 bits (writing it loads the wave's header)
//	20-37     F-number low 7 bits (7-1), wave number bit 8 (0)
//	38-4f     octave (7-4, signed), F-number high 3 bits (2-0)
//	50-67     total level (7-1, 0.375 dB steps), level direct (0)
//	68-7f     key on (7), damp (6), LFO reset (5), output select (4), pan (3-0)
//	80-97     LFO speed (5-3), vibrato depth (2-0)
//	98-af     attack rate (7-4), decay 1 rate (3-0)
//	b0-c7     decay level (7-4), decay 2 rate (3-0)
//	c8-df     rate correction (7-4), release rate (3-0)
//	e0-f7     tremolo depth (2-0)
//	f8, f9    FM and wavetable mix levels: right (5-3), left (2-0), 3 dB steps
//
// A wave's 12-byte header: sample format (bits 7-6: 8, 12 or 16 bits) and
// start address, loop point, end point (inverted), then the values of
// registers 80, 98, b0, c8 and e0.
package ymf278b

import "math"

// Rate is the output sample rate for a clock: clock/768.
func Rate(clock float64) float64 { return clock / 768 }

// Chip is one YMF278B.
type Chip struct {
	mem []byte // wave memory (the sample ROMs)

	pcmAddr int
	regs    [0x100]uint8
	memAddr uint32
	slots   [24]slot
	egCount uint32 // envelope generator clock (one tick a sample)

	fm     opl3
	fmAcc  int         // clocks since the last FM sample
	fmLast [2][2]int32 // the last two FM samples (left, right)
}

// FM samples come every 684 clocks; fmSample interpolates them to the
// output's 768.
const (
	pcmClocks = 768
	fmClocks  = 684
)

func (c *Chip) fmSample() (l, r int32) {
	for c.fmAcc += pcmClocks; c.fmAcc >= fmClocks; c.fmAcc -= fmClocks {
		o := c.fm.sample()
		c.fmLast[0] = c.fmLast[1]
		c.fmLast[1] = [2]int32{o[0] + o[2], o[1] + o[3]}
	}
	a, b := c.fmLast[0], c.fmLast[1]
	t := int32(c.fmAcc)
	l = a[0] + (b[0]-a[0])*t/fmClocks
	r = a[1] + (b[1]-a[1])*t/fmClocks
	return l * fmGain >> 8, r * fmGain >> 8
}

type slot struct {
	wave         int
	format       int // 0 8-bit, 1 12-bit, 2 16-bit
	start        uint32
	loop, end    uint32
	pos          uint64 // sample index, 32.32 fixed point
	fnum, oct    int
	tl, tlTarget int // total level now and the level it moves to (0.375 dB units)
	pan          int
	on           bool // making sound
	key          bool
	damp         bool
	lfo, vib, am int
	ar, d1r, dl  int
	d2r, rc, rr  int
	env          int // attenuation, 0.1875 dB units (0-511)
	state        int
	lfoPhase     uint32
	lfoHold      bool
}

// Envelope states.
const (
	egAttack = iota
	egDecay1
	egDecay2
	egRelease
	egOff
)

const (
	envMax    = 511
	envSilent = 256 // the envelope stops the voice once this low (48 dB)
)

// New makes a chip with the given wave memory (sample ROM).
func New(mem []byte) *Chip {
	c := &Chip{mem: mem}
	c.Reset()
	return c
}

// Reset clears all registers and silences every voice.
func (c *Chip) Reset() {
	c.pcmAddr = 0
	c.regs = [0x100]uint8{}
	c.memAddr = 0
	c.egCount = 0
	for i := range c.slots {
		c.slots[i] = slot{state: egOff, env: envMax}
	}
	c.fm.reset()
	c.fmAcc = 0
	c.fmLast = [2][2]int32{}
}

// Write writes one of the chip's ports.
func (c *Chip) Write(port int, v uint8) {
	switch port & 7 {
	case 0, 2:
		c.fm.address(port>>1, v)
	case 1, 3:
		c.fm.write(port>>1, v)
	case 4:
		c.pcmAddr = int(v)
	case 5:
		c.writePCM(c.pcmAddr, v)
	}
}

// Read reads a port: 0 is the status (never busy here), 5 the wavetable
// registers.
func (c *Chip) Read(port int) uint8 {
	switch port & 7 {
	case 0:
		return c.fm.status()
	case 5:
		return c.readPCM(c.pcmAddr)
	}
	return 0xff
}

func (c *Chip) readPCM(r int) uint8 {
	switch {
	case r == 2:
		return c.regs[2]&0x1f | 0x20 // device ID 1
	case r == 6:
		v := c.memRead(c.memAddr)
		c.memAddr = (c.memAddr + 1) & 0x3fffff
		return v
	}
	return c.regs[r]
}

func (c *Chip) memRead(a uint32) uint8 {
	a &= 0x3fffff
	if int(a) < len(c.mem) {
		return c.mem[a]
	}
	return 0
}

func (c *Chip) writePCM(r int, v uint8) {
	c.regs[r] = v
	switch {
	case r >= 3 && r <= 5:
		c.memAddr = uint32(c.regs[3]&0x3f)<<16 | uint32(c.regs[4])<<8 | uint32(c.regs[5])
	case r == 6:
		// there is no wave RAM on these boards: writes go nowhere
		c.memAddr = (c.memAddr + 1) & 0x3fffff
	case r >= 0x08 && r < 0xf8:
		ch := (r - 0x08) % 24
		c.writeSlot(&c.slots[ch], (r-0x08)/24, v)
	}
}

// writeSlot writes register group g (0 = wave number ... 9 = tremolo) of
// one voice.
func (c *Chip) writeSlot(s *slot, g int, v uint8) {
	switch g {
	case 0: // wave number
		s.wave = s.wave&0x100 | int(v)
		c.loadWave(s)
	case 1:
		s.wave = s.wave&0xff | int(v&1)<<8
		s.fnum = s.fnum&0x380 | int(v>>1)
	case 2:
		s.fnum = s.fnum&0x7f | int(v&7)<<7
		s.oct = int(int8(v) >> 4)
	case 3:
		s.tlTarget = int(v >> 1)
		if v&1 != 0 {
			s.tl = s.tlTarget
		}
	case 4:
		s.pan = int(v & 15)
		s.damp = v&0x40 != 0
		s.lfoHold = v&0x20 != 0
		if s.lfoHold {
			s.lfoPhase = 0
		}
		key := v&0x80 != 0
		if key && !s.key {
			c.keyOn(s)
		} else if !key && s.key {
			c.keyOff(s)
		}
		s.key = key
		if s.damp && s.state != egOff {
			s.state = egRelease
		}
	case 5:
		s.lfo, s.vib = int(v>>3)&7, int(v&7)
	case 6:
		s.ar, s.d1r = int(v>>4), int(v&15)
	case 7:
		s.dl, s.d2r = int(v>>4), int(v&15)
	case 8:
		s.rc, s.rr = int(v>>4), int(v&15)
	case 9:
		s.am = int(v & 7)
	}
}

// loadWave reads a wave's header into the voice.
func (c *Chip) loadWave(s *slot) {
	a := uint32(s.wave) * 12
	if s.wave >= 384 {
		if h := c.regs[2] >> 2 & 7; h != 0 {
			a = uint32(h)*0x80000 + uint32(s.wave-384)*12
		}
	}
	var h [12]uint8
	for i := range h {
		h[i] = c.memRead(a + uint32(i))
	}
	s.format = int(h[0] >> 6)
	s.start = uint32(h[0]&0x3f)<<16 | uint32(h[1])<<8 | uint32(h[2])
	s.loop = uint32(h[3])<<8 | uint32(h[4])
	s.end = (uint32(h[5])<<8 | uint32(h[6])) ^ 0xffff
	s.lfo, s.vib = int(h[7]>>3)&7, int(h[7]&7)
	s.ar, s.d1r = int(h[8]>>4), int(h[8]&15)
	s.dl, s.d2r = int(h[9]>>4), int(h[9]&15)
	s.rc, s.rr = int(h[10]>>4), int(h[10]&15)
	s.am = int(h[11] & 7)
	s.pos = 0
}

func (c *Chip) keyOn(s *slot) {
	s.pos = 0
	s.env = envMax
	s.state = egAttack
	s.on = true
}

func (c *Chip) keyOff(s *slot) {
	if s.state != egOff {
		s.state = egRelease
	}
}

// sample reads the voice's current sample (16-bit range).
func (c *Chip) sample(s *slot) int32 {
	n := uint32(s.pos >> 32)
	switch s.format {
	case 0:
		return int32(int8(c.memRead(s.start+n))) << 8
	case 1:
		a := s.start + n/2*3
		if n&1 == 0 {
			return int32(int16(uint16(c.memRead(a))<<8 | uint16(c.memRead(a+1)&0xf0)))
		}
		return int32(int16(uint16(c.memRead(a+2))<<8 | uint16(c.memRead(a+1)&0x0f)<<4))
	default:
		a := s.start + n*2
		return int32(int16(uint16(c.memRead(a))<<8 | uint16(c.memRead(a+1))))
	}
}

// LFO periods in samples (the chip's eight speeds) and depths.
var (
	lfoPeriod = [8]float64{262486, 21841, 13795, 10483, 8455, 7622, 6925, 5972}
	vibCents  = [8]float64{0, 3.378, 5.065, 6.750, 10.114, 20.170, 40.180, 79.307}
	amDepth   = [8]int{0, 10, 16, 20, 24, 32, 40, 64} // 0.1875 dB units
)

// gain: attenuation (0.1875 dB units) to a 1.15 fixed-point factor.
var gain [2048]int32

// lfoStep: each LFO speed's phase step a sample (a full period is 1<<32).
var lfoStep [8]uint32

func init() {
	for i := range gain {
		gain[i] = int32(math.Round(32768 * math.Pow(10, -float64(i)*0.1875/20)))
	}
	gain[len(gain)-1] = 0
	for i, p := range lfoPeriod {
		lfoStep[i] = uint32(math.Round((1 << 32) / p))
	}
}

// Sample makes one output sample (left, right; 16-bit range).
func (c *Chip) Sample() (l, r int32) {
	c.egCount++
	mixL, mixR := int(c.regs[0xf9]&7), int(c.regs[0xf9]>>3&7)
	for i := range c.slots {
		s := &c.slots[i]
		// each voice's LFO runs all the time, held only by its reset bit
		if !s.lfoHold {
			s.lfoPhase += lfoStep[s.lfo]
		}
		if !s.on {
			continue
		}
		c.envelope(s)
		if !s.on {
			continue
		}
		// level slides towards a total level written without "direct"
		if s.tl != s.tlTarget && c.egCount%54 == 0 {
			if s.tl < s.tlTarget {
				s.tl++
			} else {
				s.tl--
			}
		}
		v := c.sample(s)
		// LFO: a triangle
		lfoTri := 0.0
		if s.vib != 0 || s.am != 0 {
			ph := float64(s.lfoPhase) / (1 << 32)
			lfoTri = 1 - 4*math.Abs(ph-0.5) // -1..1
		}
		att := s.tl*2 + s.env
		if s.am != 0 {
			att += int(float64(amDepth[s.am]) * (lfoTri + 1) / 2)
		}
		pl, pr := panAtt(s.pan)
		al, ar := att+pl+mixL*16, att+pr+mixR*16
		if mixL == 7 {
			al = len(gain) - 1
		}
		if mixR == 7 {
			ar = len(gain) - 1
		}
		l += v * gain[min(al, len(gain)-1)] >> 15
		r += v * gain[min(ar, len(gain)-1)] >> 15
		c.advance(s, lfoTri)
	}
	fl, fr := c.fmSample()
	fmL, fmR := int(c.regs[0xf8]&7), int(c.regs[0xf8]>>3&7)
	if fmL != 7 {
		l += fl * gain[fmL*16] >> 15
	}
	if fmR != 7 {
		r += fr * gain[fmR*16] >> 15
	}
	return l, r
}

// panAtt: the attenuation of each side for a pan setting (3 dB steps; 8
// silences both).
func panAtt(p int) (l, r int) {
	const off = 2047
	switch {
	case p == 8:
		return off, off
	case p < 8:
		if p == 7 {
			return 0, off
		}
		return 0, p * 16
	default:
		if p == 9 {
			return off, 0
		}
		return (16 - p) * 16, 0
	}
}

// advance steps the voice's sample position.
func (c *Chip) advance(s *slot, lfoTri float64) {
	inc := uint64(1024+s.fnum) << uint(21+s.oct)
	if s.vib != 0 {
		inc = uint64(float64(inc) * math.Pow(2, vibCents[s.vib]*lfoTri/1200))
	}
	s.pos += inc
	if uint32(s.pos>>32) > s.end {
		// past the end: back to the loop point
		over := s.pos - (uint64(s.end)+1)<<32
		s.pos = uint64(s.loop)<<32 + over
		if s.end < s.loop {
			s.pos = uint64(s.loop) << 32
		}
	}
}

// rate: an envelope rate (0-15) with the rate correction applied (0-63).
func (s *slot) rate(r int) int {
	switch r {
	case 0:
		return 0
	case 15:
		return 63
	}
	rate := r * 4
	if s.rc != 15 {
		rate += 2 * (s.oct + s.rc)
	}
	return max(0, min(63, rate))
}

// egStep: how much the envelope moves this sample at a rate (0 = not this
// sample).
var egPattern = [4][8]int{
	{0, 1, 0, 1, 0, 1, 0, 1},
	{0, 1, 0, 1, 1, 1, 0, 1},
	{0, 1, 1, 1, 0, 1, 1, 1},
	{0, 1, 1, 1, 1, 1, 1, 1},
}
var egPatternFast = [4][8]int{
	{1, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 2, 1, 1, 1, 2},
	{1, 2, 1, 2, 1, 2, 1, 2},
	{1, 2, 2, 2, 1, 2, 2, 2},
}

func (c *Chip) egStep(rate int) int { return egStepAt(c.egCount, rate) }

// egStepAt: the envelope step at a rate for envelope clock count n.
func egStepAt(n uint32, rate int) int {
	if rate == 0 {
		return 0
	}
	hi, lo := rate>>2, rate&3
	if hi < 13 {
		shift := uint(12 - min(hi, 12))
		if n&(1<<shift-1) != 0 {
			return 0
		}
		return egPattern[lo][n>>shift&7]
	}
	return egPatternFast[lo][n&7] << uint(hi-13)
}

func (c *Chip) envelope(s *slot) {
	switch s.state {
	case egAttack:
		rate := s.rate(s.ar)
		if rate >= 60 {
			s.env = 0
		} else if n := c.egStep(rate); n != 0 {
			s.env += (^s.env * n) >> 3
		}
		if s.env <= 0 {
			s.env = 0
			s.state = egDecay1
		}
	case egDecay1:
		dl := s.dl * 16
		if s.dl == 15 {
			dl = envMax
		}
		if s.env >= dl {
			s.state = egDecay2
			break
		}
		if s.env += c.egStep(s.rate(s.d1r)); s.env >= dl {
			s.env = dl
			s.state = egDecay2
		}
	case egDecay2:
		s.env += c.egStep(s.rate(s.d2r))
	case egRelease:
		if s.damp {
			s.env += c.egStep(56)
		} else {
			s.env += c.egStep(s.rate(s.rr))
		}
	}
	if s.env >= envSilent && s.state != egAttack {
		s.env = envMax
		s.state = egOff
		s.on = false
	}
}
