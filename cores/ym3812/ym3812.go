// Package ym3812 is RetroPixels' Yamaha YM3812 (OPL2) FM sound chip: nine
// channels of two operators (FM or additive), each with its own envelope
// (attack, decay, sustain, release; key scaling of rate and level), four
// waveforms, tremolo and vibrato from two global LFOs, feedback on the first
// operator, the rhythm mode (bass drum, snare, tom-tom, cymbal and hi-hat
// from channels 7-9 and a noise generator) and timers 1 / 2 with their
// interrupt. (The CSM speech mode is not emulated.)
//
//	opl := ym3812.New()
//	opl.Write(port, v) // port 0: address, 1: data
//	opl.Run(cycles)    // advance by input-clock cycles (a sample every 72)
//	v := opl.Take()    // average output since the last Take
//	opl.IRQ()          // the interrupt line
//
// The chip works in the log domain, like the real one: a quarter-wave
// log-sine table and an exponential table turn phase plus attenuation into
// the operator's 13-bit output; attenuation is 9 bits of 0.1875 dB.
package ym3812

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
)

var (
	logSin [256]uint16 // -log2(sin) of a quarter wave, in 1/256 octaves
	expTab [256]uint16 // 2^(-i/256) * 2048
	// envelope increments for the four rates of each shift (8 steps)
	egSlow = [4][8]uint8{{0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}}
	egFast = [4][8]uint8{{1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2}}
	// frequency multipliers, times two
	multTab = [16]uint32{1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30}
	// key scaling of level: attenuation for the top four F-number bits
	// at block 7, in 0.75 dB
	kslTab = [16]int{0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64}
	// KSL setting -> how far the key-scale attenuation is shifted down
	// (off, 3 dB, 1.5 dB, 6 dB an octave)
	kslShift = [4]uint{8, 1, 2, 0}
)

func init() {
	for i := range logSin {
		logSin[i] = uint16(math.Round(-math.Log2(math.Sin((float64(i)+0.5)*math.Pi/512)) * 256))
		expTab[i] = uint16(math.Round(2048 * math.Pow(2, -float64(i)/256)))
	}
}

// envelope stages
const (
	attack = iota
	decay
	sustain
	release
)

// slot is one operator (fixed-size fields: the state is written as is).
type slot struct {
	Phase                         uint32 // 19 bits: a cycle is 2^19
	Env                           int16  // attenuation, 0..511
	Stage                         uint8
	Key                           uint8 // bit 0: the channel's key, bit 1: rhythm key
	Prev                          [2]int16
	AM, Vib, EGT, KSR, Mult       uint8
	KSL, TL, AR, DR, SL, RR, Wave uint8
}

type channel struct {
	Fnum         uint16
	Block, KeyOn uint8
	FB, Additive uint8
}

// regs is everything the chip keeps (written to save states as is).
type regs struct {
	Slots                         [18]slot
	Chans                         [9]channel
	Addr, WaveEnable, NoteSel, BD uint8
	T1, T2, Ctrl, Status          uint8
	Left1, Left2                  int32 // samples to the timers' overflows
	Count                         uint32
	Trem                          uint16
	Noise                         uint32
	Clk                           int32
	Out                           int32
}

// Chip is one YM3812.
type Chip struct {
	r   regs
	sum int64
	n   int
}

// New makes a chip, reset.
func New() *Chip {
	c := &Chip{}
	c.Reset()
	return c
}

// Reset puts the chip in its power-on state.
func (c *Chip) Reset() {
	c.r = regs{Noise: 1}
	for i := range c.r.Slots {
		c.r.Slots[i].Env = 511
		c.r.Slots[i].Stage = release
	}
	c.sum, c.n = 0, 0
}

// IRQ tells whether the interrupt line is on (a timer flag is set).
func (c *Chip) IRQ() bool { return c.r.Status&0x80 != 0 }

// Read reads the status (port 0): bit 7 interrupt, 6 timer 1, 5 timer 2.
func (c *Chip) Read(port int) uint8 {
	if port&1 == 0 {
		return c.r.Status | 6
	}
	return 0xff
}

// slotFor is the operator a register offset (0x00-0x15) is for, or -1.
func slotFor(off int) int {
	if off&7 > 5 || off > 0x15 {
		return -1
	}
	ch := off>>3*3 + off&7%3
	return ch*2 + off&7/3
}

// Write takes a byte written to port 0 (address) or 1 (data).
func (c *Chip) Write(port int, v uint8) {
	r := &c.r
	if port&1 == 0 {
		r.Addr = v
		return
	}
	a := int(r.Addr)
	switch {
	case a == 0x01:
		r.WaveEnable = v >> 5 & 1
	case a == 0x02:
		r.T1 = v
	case a == 0x03:
		r.T2 = v
	case a == 0x04:
		if v&0x80 != 0 {
			r.Status = 0
			return
		}
		if v&1 != 0 && r.Ctrl&1 == 0 {
			r.Left1 = int32(256-int(r.T1)) * 4
		}
		if v&2 != 0 && r.Ctrl&2 == 0 {
			r.Left2 = int32(256-int(r.T2)) * 16
		}
		r.Ctrl = v
		c.updateIRQ()
	case a == 0x08:
		r.NoteSel = v >> 6 & 1
	case a >= 0x20 && a < 0xa0:
		s := slotFor(a & 0x1f)
		if s < 0 {
			return
		}
		sl := &r.Slots[s]
		switch a & 0xe0 {
		case 0x20:
			sl.AM, sl.Vib, sl.EGT, sl.KSR, sl.Mult = v>>7, v>>6&1, v>>5&1, v>>4&1, v&15
		case 0x40:
			sl.KSL, sl.TL = v>>6, v&63
		case 0x60:
			sl.AR, sl.DR = v>>4, v&15
		case 0x80:
			sl.SL, sl.RR = v>>4, v&15
		}
	case a >= 0xa0 && a <= 0xa8:
		ch := &r.Chans[a-0xa0]
		ch.Fnum = ch.Fnum&0x300 | uint16(v)
	case a >= 0xb0 && a <= 0xb8:
		n := a - 0xb0
		ch := &r.Chans[n]
		ch.Fnum = ch.Fnum&0xff | uint16(v&3)<<8
		ch.Block = v >> 2 & 7
		on := v >> 5 & 1
		if on != ch.KeyOn {
			c.key(n*2, 1, on != 0)
			c.key(n*2+1, 1, on != 0)
		}
		ch.KeyOn = on
	case a == 0xbd:
		old := r.BD
		r.BD = v
		if v&0x20 != 0 || old&0x20 != 0 {
			// rhythm keys: bass drum (both operators of channel 7),
			// hi-hat, snare, tom-tom, cymbal
			rk := func(bit uint8) bool { return v&0x20 != 0 && v&bit != 0 }
			c.key(12, 2, rk(0x10))
			c.key(13, 2, rk(0x10))
			c.key(14, 2, rk(0x01))
			c.key(15, 2, rk(0x08))
			c.key(16, 2, rk(0x04))
			c.key(17, 2, rk(0x02))
		}
	case a >= 0xc0 && a <= 0xc8:
		ch := &r.Chans[a-0xc0]
		ch.FB, ch.Additive = v>>1&7, v&1
	case a >= 0xe0 && a <= 0xf5:
		if s := slotFor(a - 0xe0); s >= 0 {
			r.Slots[s].Wave = v & 3
		}
	}
}

// key turns one of a slot's key sources on or off: the first one on starts
// the attack (and the phase from 0), the last one off the release.
func (c *Chip) key(s int, bit uint8, on bool) {
	sl := &c.r.Slots[s]
	was := sl.Key != 0
	if on {
		sl.Key |= bit
	} else {
		sl.Key &^= bit
	}
	switch {
	case !was && sl.Key != 0:
		sl.Phase = 0
		sl.Stage = attack
		if c.rate(s, sl.AR) >= 60 {
			sl.Env = 0
			sl.Stage = decay
		}
	case was && sl.Key == 0:
		sl.Stage = release
	}
}

func (c *Chip) updateIRQ() {
	r := &c.r
	r.Status &^= 0x80
	if r.Status&0x60 != 0 {
		r.Status |= 0x80
	}
}

// keyCode is a channel's block and top F-number bit (which one by the
// note-select bit), for key scaling.
func (c *Chip) keyCode(ch int) int {
	k := &c.r.Chans[ch]
	b := int(k.Fnum >> 9 & 1)
	if c.r.NoteSel != 0 {
		b = int(k.Fnum >> 8 & 1)
	}
	return int(k.Block)<<1 | b
}

// rate is a slot's effective envelope rate (0-63) for a register rate.
func (c *Chip) rate(s int, r uint8) int {
	if r == 0 {
		return 0
	}
	kc := c.keyCode(s / 2)
	if c.r.Slots[s].KSR == 0 {
		kc >>= 2
	}
	return min(int(r)*4+kc, 63)
}

// egStep moves one slot's envelope by one sample.
func (c *Chip) egStep(s int) {
	sl := &c.r.Slots[s]
	var rr uint8
	switch sl.Stage {
	case attack:
		rr = sl.AR
	case decay:
		rr = sl.DR
	case sustain:
		if sl.EGT != 0 {
			return
		}
		rr = sl.RR
	default:
		rr = sl.RR
	}
	rate := c.rate(s, rr)
	if rate == 0 {
		return
	}
	cnt := c.r.Count
	var inc int
	switch {
	case rate < 52:
		shift := uint(12 - rate>>2)
		if cnt&(1<<shift-1) != 0 {
			return
		}
		inc = int(egSlow[rate&3][cnt>>shift&7])
	case rate < 60:
		inc = int(egFast[rate&3][cnt&7]) << uint((rate-52)>>2)
	default:
		inc = 4
	}
	env := int(sl.Env)
	if sl.Stage == attack {
		if rate >= 60 {
			env = 0
		} else {
			env += ((-env - 1) * inc) >> 3
		}
		if env <= 0 {
			env = 0
			sl.Stage = decay
		}
		sl.Env = int16(env)
		return
	}
	env = min(env+inc, 511)
	if sl.Stage == decay {
		lvl := int(sl.SL) << 4
		if sl.SL == 15 {
			lvl = 31 << 4
		}
		if env >= lvl {
			sl.Stage = sustain
		}
	}
	sl.Env = int16(env)
}

// atten is a slot's total attenuation (envelope, total level, key scaling,
// tremolo), 0..511.
func (c *Chip) atten(s int) int {
	sl := &c.r.Slots[s]
	ch := &c.r.Chans[s/2]
	ksl := kslTab[ch.Fnum>>6]*4 - (8-int(ch.Block))*32
	if ksl < 0 {
		ksl = 0
	}
	a := int(sl.Env) + int(sl.TL)<<2 + ksl>>kslShift[sl.KSL]
	if sl.AM != 0 {
		t := int(c.r.Trem)
		if t >= 105 {
			t = 209 - t
		}
		if c.r.BD&0x80 != 0 {
			a += t >> 2
		} else {
			a += t >> 4
		}
	}
	return min(a, 511)
}

// wave is one operator's output for a 10-bit phase and an attenuation.
func (c *Chip) wave(s int, phase int, att int) int {
	p := phase & 1023
	w := c.r.Slots[s].Wave
	if c.r.WaveEnable == 0 {
		w = 0
	}
	switch {
	case w == 1 && p&512 != 0, w == 3 && p&256 != 0:
		return 0
	}
	i := p & 255
	if p&256 != 0 {
		i = 255 - i
	}
	l := int(logSin[i]) + att<<3
	if l >= 13<<8 {
		return 0
	}
	v := int(expTab[l&255]) << 1 >> uint(l>>8)
	if w == 0 && p&512 != 0 {
		return -v
	}
	return v
}

// advance moves a slot's phase by one sample.
func (c *Chip) advance(s int) {
	sl := &c.r.Slots[s]
	ch := &c.r.Chans[s/2]
	f := int(ch.Fnum)
	if sl.Vib != 0 {
		// vibrato: up to an eighth (deep) or sixteenth of the top three
		// F-number bits, in an 8-step triangle
		d := f >> 7 & 7
		pos := c.r.Count >> 10 & 7
		switch {
		case pos&3 == 0:
			d = 0
		case pos&1 != 0:
			d >>= 1
		}
		if c.r.BD&0x40 == 0 {
			d >>= 1
		}
		if pos&4 != 0 {
			d = -d
		}
		f += d
	}
	sl.Phase = (sl.Phase + uint32(f<<ch.Block>>1)*multTab[sl.Mult]>>1) & 0x7ffff
}

// op is a slot's output with phase modulation mod.
func (c *Chip) op(s, mod int) int {
	return c.wave(s, int(c.r.Slots[s].Phase>>9)+mod, c.atten(s))
}

// channel2 is a two-operator channel's output.
func (c *Chip) channel2(n int) int {
	ch := &c.r.Chans[n]
	m := &c.r.Slots[n*2]
	var fb int
	if ch.FB != 0 {
		fb = (int(m.Prev[0]) + int(m.Prev[1])) >> (9 - ch.FB)
	}
	o1 := c.op(n*2, fb)
	m.Prev[1], m.Prev[0] = m.Prev[0], int16(o1)
	if ch.Additive != 0 {
		return o1 + c.op(n*2+1, 0)
	}
	return c.op(n*2+1, o1)
}

// tick makes one sample.
func (c *Chip) tick() {
	r := &c.r
	r.Count++
	if r.Count&63 == 0 {
		r.Trem++
		if r.Trem >= 210 {
			r.Trem = 0
		}
	}
	// noise: a 23-bit shift register
	r.Noise = r.Noise>>1 | (r.Noise^r.Noise>>14)&1<<22
	// timers
	if r.Ctrl&1 != 0 {
		if r.Left1--; r.Left1 <= 0 {
			r.Left1 = int32(256-int(r.T1)) * 4
			if r.Ctrl&0x40 == 0 {
				r.Status |= 0x40
			}
		}
	}
	if r.Ctrl&2 != 0 {
		if r.Left2--; r.Left2 <= 0 {
			r.Left2 = int32(256-int(r.T2)) * 16
			if r.Ctrl&0x20 == 0 {
				r.Status |= 0x20
			}
		}
	}
	c.updateIRQ()
	for s := range r.Slots {
		c.egStep(s)
		c.advance(s)
	}
	out := 0
	rhythm := r.BD&0x20 != 0
	for n := 0; n < 9; n++ {
		if rhythm && n >= 6 {
			break
		}
		out += c.channel2(n)
	}
	if rhythm {
		out += c.rhythm()
	}
	r.Out = int32(out)
}

// rhythm is the five drums' output: the bass drum is channel 7 as usual;
// the hi-hat, snare and cymbal take their phases from bits of the hi-hat's
// and cymbal's oscillators and the noise; the tom-tom is a plain operator.
func (c *Chip) rhythm() int {
	r := &c.r
	out := c.channel2(6) * 2
	p7 := int(r.Slots[14].Phase >> 9) // hi-hat oscillator
	p8 := int(r.Slots[17].Phase >> 9) // cymbal oscillator
	noise := int(r.Noise & 1)
	bit := (p7>>2^p7>>7|p7>>3)&1 | (p8>>3^p8>>5)&1
	// hi-hat
	ph := bit << 9
	if bit^noise != 0 {
		ph |= 0xd0
	} else {
		ph |= 0x34
	}
	out += c.wave(14, ph, c.atten(14)) * 2
	// snare
	sd := (p7 >> 8 & 1) << 9
	sd |= ((p7 >> 8 & 1) ^ noise) << 8
	out += c.wave(15, sd, c.atten(15)) * 2
	// tom-tom
	out += c.op(16, 0) * 2
	// cymbal
	out += c.wave(17, bit<<9|0x100, c.atten(17)) * 2
	return out
}

// Run advances the chip by cycles input-clock cycles.
func (c *Chip) Run(cycles int) {
	c.r.Clk += int32(cycles)
	for c.r.Clk >= 72 {
		c.r.Clk -= 72
		c.tick()
		c.sum += int64(c.r.Out)
		c.n++
	}
}

// Rate is the sample rate for an input clock.
func Rate(clock float64) float64 { return clock / 72 }

// Take returns the average output since the last Take (a channel's full
// scale is 1).
func (c *Chip) Take() float64 {
	if c.n == 0 {
		return float64(c.r.Out) / 4096
	}
	v := float64(c.sum) / float64(c.n) / 4096
	c.sum, c.n = 0, 0
	return v
}

// ---- save states ----

var errState = errors.New("ym3812: damaged save state")

const stateVersion = 1

// StateSize is the length of a saved state.
var StateSize = 1 + binary.Size(regs{})

// AppendState appends the chip's state to b.
func (c *Chip) AppendState(b []byte) []byte {
	var w bytes.Buffer
	w.WriteByte(stateVersion)
	binary.Write(&w, binary.LittleEndian, &c.r)
	return append(b, w.Bytes()...)
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (c *Chip) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != stateVersion {
		return b, errState
	}
	var t regs
	if err := binary.Read(bytes.NewReader(b[1:StateSize]), binary.LittleEndian, &t); err != nil {
		return b, errState
	}
	for i := range t.Slots {
		s := &t.Slots[i]
		if s.Env < 0 || s.Env > 511 || s.Stage > release || s.Mult > 15 || s.KSL > 3 || s.Wave > 3 || s.SL > 15 {
			return b, errState
		}
	}
	for i := range t.Chans {
		if t.Chans[i].Fnum > 0x3ff || t.Chans[i].Block > 7 || t.Chans[i].FB > 7 {
			return b, errState
		}
	}
	if t.Clk < 0 || t.Clk >= 72 || t.Trem >= 210 {
		return b, errState
	}
	c.r = t
	c.sum, c.n = 0, 0
	return b[StateSize:], nil
}
