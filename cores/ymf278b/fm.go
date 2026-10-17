package ymf278b

// The chip's FM part: an OPL3 (YMF262). 36 operators make 18 two-operator
// channels; pairs of channels can join into four-operator voices, and
// channels 6-8 of bank 0 can play the five rhythm sounds instead.
//
// Registers (bank 0 at 000-0ff, bank 1 at 100-1ff):
//
//	02, 03    timer 1 (80 µs) and timer 2 (320 µs) counts
//	04        IRQ reset (7), timer masks (6, 5), timer starts (1, 0)
//	104       four-operator voice enables (bits 5-0)
//	105       OPL3 mode ("NEW", bit 0)
//	08        note select (6)
//	20-35     tremolo, vibrato, sustain, key scale rate, multiple
//	40-55     key scale level (7-6), total level (5-0)
//	60-75     attack rate, decay rate
//	80-95     sustain level, release rate
//	a0-a8     F-number low 8 bits
//	b0-b8     key on (5), block (4-2), F-number high 2 bits
//	bd        tremolo depth (7), vibrato depth (6), rhythm (5), drums (4-0)
//	c0-c8     outputs D C B A (7-4), feedback (3-1), connection (0)
//	e0-f5     waveform (2-0)
//
// Operators sit at register offsets 0-5, 8-d and 10-15; the operator at
// offset o belongs to channel o%8%3 + o/8*3 and is its first operator for
// o%8 < 3, its second otherwise.

import "math"

type fmOp struct {
	phase   uint32 // 10.9 fixed point
	env     int    // attenuation, 0.1875 dB units (0-511)
	state   int
	key     uint8 // bit 0 the channel's key, bit 1 a rhythm key
	am, vib bool
	egt     bool // sustain: hold at the sustain level
	ksr     bool
	mult    int
	ksl, tl int
	ar, dr  int
	sl, rr  int
	ws      int
	out     [2]int32 // the last two outputs (for feedback)
}

type fmCh struct {
	fnum, block int
	fb, cnt     int
	outs        uint8 // output enables, bit 0 = A ... bit 3 = D
}

type opl3 struct {
	addr  [2]int
	regs  [0x200]uint8
	ops   [36]fmOp
	ch    [18]fmCh
	isNew bool  // OPL3 mode
	four  uint8 // four-operator enables (register 104)
	bd    uint8 // register bd
	nts   bool

	egCount uint32
	amPos   int // tremolo triangle position, 0-209
	amCount int
	noise   uint32

	t1, t2   uint8 // timer reload values
	t1n, t2n int   // counts to overflow
	tick     int   // FM samples, for the timer clocks
	ctrl, st uint8
}

// Envelope states of an FM operator (egAttack ... come from the
// wavetable part); fmSustain holds or releases at the sustain level.
const fmSustain = egRelease + 10

var (
	multTab = [16]uint32{1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30}
	kslTab  = [16]int{0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64}
	// key scale level 0, 3, 1.5, 6 dB an octave: right shifts
	kslShift = [4]uint{31, 1, 2, 0}
	logSin   [256]int // -log2(sin) of a quarter wave, 8.8 fixed point
	expTab   [256]int // (2^x - 1) * 1024 for x in [0, 1)
)

func init() {
	for i := range logSin {
		s := math.Sin((float64(i) + 0.5) * math.Pi / 512)
		logSin[i] = int(math.Round(-math.Log2(s) * 256))
		expTab[i] = int(math.Round((math.Pow(2, float64(i)/256) - 1) * 1024))
	}
}

func (f *opl3) reset() {
	*f = opl3{noise: 1}
	for i := range f.ops {
		f.ops[i].env = envMax
		f.ops[i].state = egOff
	}
}

func (f *opl3) address(bank int, v uint8) {
	f.addr[bank&1] = int(v)
}

func (f *opl3) status() uint8 { return f.st }

// opSlot: the operator (0-17 within a bank) at a register offset, or -1.
func opSlot(o int) int {
	if o >= 0x16 || o&7 >= 6 {
		return -1
	}
	return o>>3*6 + o&7
}

// chOps: the two operators (0-35) of channel c (0-17).
func chOps(c int) (int, int) {
	b, n := c/9*18, c%9
	m := b + n/3*6 + n%3
	return m, m + 3
}

func (f *opl3) write(bank int, v uint8) {
	bank &= 1
	r := f.addr[bank]
	f.regs[bank<<8|r] = v
	if bank == 1 {
		switch r {
		case 0x04:
			f.four = v & 0x3f
			return
		case 0x05:
			f.isNew = v&1 != 0
			return
		}
	} else {
		switch r {
		case 0x02:
			f.t1 = v
			return
		case 0x03:
			f.t2 = v
			return
		case 0x04:
			if v&0x80 != 0 {
				f.st = 0
				return
			}
			if v&1 != 0 && f.ctrl&1 == 0 {
				f.t1n = 256 - int(f.t1)
			}
			if v&2 != 0 && f.ctrl&2 == 0 {
				f.t2n = 256 - int(f.t2)
			}
			f.ctrl = v
			return
		case 0x08:
			f.nts = v&0x40 != 0
			return
		case 0xbd:
			old := f.bd
			f.bd = v
			f.rhythmKeys(old, v)
			return
		}
	}
	switch hi := r & 0xe0; {
	case hi >= 0x20 && hi <= 0x80 || hi == 0xe0:
		s := opSlot(r & 0x1f)
		if s < 0 {
			return
		}
		o := &f.ops[bank*18+s]
		switch hi {
		case 0x20:
			o.am, o.vib, o.egt, o.ksr = v&0x80 != 0, v&0x40 != 0, v&0x20 != 0, v&0x10 != 0
			o.mult = int(v & 15)
		case 0x40:
			o.ksl, o.tl = int(v>>6), int(v&0x3f)
		case 0x60:
			o.ar, o.dr = int(v>>4), int(v&15)
		case 0x80:
			o.sl, o.rr = int(v>>4), int(v&15)
		case 0xe0:
			if f.isNew {
				o.ws = int(v & 7)
			} else {
				o.ws = int(v & 3)
			}
		}
	case r >= 0xa0 && r <= 0xa8:
		c := &f.ch[bank*9+r-0xa0]
		c.fnum = c.fnum&0x300 | int(v)
	case r >= 0xb0 && r <= 0xb8:
		n := bank*9 + r - 0xb0
		c := &f.ch[n]
		c.fnum = c.fnum&0xff | int(v&3)<<8
		c.block = int(v >> 2 & 7)
		m, k := chOps(n)
		f.keyOp(m, 1, v&0x20 != 0)
		f.keyOp(k, 1, v&0x20 != 0)
	case r >= 0xc0 && r <= 0xc8:
		c := &f.ch[bank*9+r-0xc0]
		c.outs = v >> 4
		c.fb, c.cnt = int(v>>1&7), int(v&1)
	}
}

// keyOp sets or clears one of an operator's key bits; a key going on
// restarts it.
func (f *opl3) keyOp(i int, bit uint8, on bool) {
	o := &f.ops[i]
	old := o.key
	if on {
		o.key |= bit
	} else {
		o.key &^= bit
	}
	switch {
	case old == 0 && o.key != 0:
		o.phase = 0
		o.state = egAttack
	case old != 0 && o.key == 0:
		if o.state != egOff {
			o.state = egRelease
		}
	}
}

func (f *opl3) rhythmKeys(old, v uint8) {
	if v&0x20 == 0 {
		v = 0
	}
	if old&0x20 == 0 {
		old = 0
	}
	if old == v {
		return
	}
	// bass drum: both operators of channel 6
	f.keyOp(12, 2, v&0x10 != 0)
	f.keyOp(15, 2, v&0x10 != 0)
	f.keyOp(16, 2, v&0x08 != 0) // snare
	f.keyOp(14, 2, v&0x04 != 0) // tom
	f.keyOp(17, 2, v&0x02 != 0) // cymbal
	f.keyOp(13, 2, v&0x01 != 0) // hi-hat
}

// clock advances the timers by one FM sample.
func (f *opl3) clock() {
	f.tick++
	if f.ctrl&1 != 0 && f.tick&3 == 0 {
		if f.t1n--; f.t1n <= 0 {
			f.t1n = 256 - int(f.t1)
			if f.ctrl&0x40 == 0 {
				f.st |= 0xc0
			}
		}
	}
	if f.ctrl&2 != 0 && f.tick&15 == 0 {
		if f.t2n--; f.t2n <= 0 {
			f.t2n = 256 - int(f.t2)
			if f.ctrl&0x20 == 0 {
				f.st |= 0xa0
			}
		}
	}
}

// opOut: an operator's output (about ±4084) for a phase (10 bits) and an
// attenuation.
func opOut(ws int, p uint32, att int) int32 {
	p &= 0x3ff
	neg := p&0x200 != 0
	var lv int
	quarter := func(q uint32) int {
		if q&0x100 != 0 {
			return logSin[^q&0xff]
		}
		return logSin[q&0xff]
	}
	switch ws {
	case 0:
		lv = quarter(p)
	case 1:
		if neg {
			return 0
		}
		lv = quarter(p)
	case 2:
		lv, neg = quarter(p), false
	case 3:
		if p&0x100 != 0 {
			return 0
		}
		lv, neg = quarter(p), false
	case 4:
		if neg {
			return 0
		}
		lv, neg = quarter(p<<1), p&0x100 != 0
	case 5:
		if neg {
			return 0
		}
		lv, neg = quarter(p<<1), false
	case 6:
		lv = 0
	default:
		q := p & 0x1ff
		if neg {
			q = ^p & 0x1ff
		}
		lv = int(q) << 3
	}
	lv += att << 3
	if lv >= 0x1000 {
		return 0
	}
	v := int32((expTab[^lv&0xff]+1024)<<1) >> uint(lv>>8)
	if neg {
		return -v
	}
	return v
}

// sample makes one FM sample: the A, B, C and D outputs.
func (f *opl3) sample() (out [4]int32) {
	f.clock()
	f.egCount++
	// tremolo: a triangle stepping every 64 samples
	if f.amCount++; f.amCount == 64 {
		f.amCount = 0
		if f.amPos++; f.amPos == 210 {
			f.amPos = 0
		}
	}
	trem := f.amPos
	if trem >= 105 {
		trem = 209 - trem
	}
	if f.bd&0x80 != 0 {
		trem >>= 2
	} else {
		trem >>= 4
	}
	vibPos := f.egCount >> 10 & 7
	// noise
	f.noise = f.noise>>1 | ((f.noise^f.noise>>14)&1)<<22
	noise := f.noise & 1

	// nothing sounding: nothing to do (an operator's phase restarts when
	// it is keyed on, so frozen phases do not matter)
	idle := true
	for i := range f.ops {
		if f.ops[i].state != egOff {
			idle = false
			break
		}
	}
	if idle {
		for i := range f.ops {
			f.ops[i].out = [2]int32{}
		}
		return out
	}

	rhythm := f.bd&0x20 != 0
	// operators: envelopes and phases first
	var ph [36]uint32
	var att [36]int
	for i := range f.ops {
		o := &f.ops[i]
		c := &f.ch[i/18*9+i%18/6*3+i%18%3]
		fnum, block := c.fnum, c.block
		if f.isFourSecond(i/18*9 + i%18/6*3 + i%18%3) {
			p := &f.ch[i/18*9+i%18/6*3+i%18%3-3]
			fnum, block = p.fnum, p.block
		}
		f.envelope(o, fnum, block)
		if o.vib {
			r := fnum >> 7 & 7
			if f.bd&0x40 == 0 {
				r >>= 1
			}
			switch {
			case vibPos&3 == 0:
				r = 0
			case vibPos&1 != 0:
				r >>= 1
			}
			if vibPos&4 != 0 {
				r = -r
			}
			fnum += r
		}
		ph[i] = o.phase >> 9
		o.phase += uint32(fnum<<uint(block)>>1) * multTab[o.mult] >> 1
		a := o.env + o.tl<<2
		if o.ksl != 0 {
			k := kslTab[fnum>>6]<<2 - (8-block)<<5
			if k > 0 {
				a += k >> kslShift[o.ksl]
			}
		}
		if o.am {
			a += trem
		}
		att[i] = min(a, envMax)
	}
	if rhythm {
		hh, cy := ph[13], ph[17]
		x := (hh>>2 ^ hh>>7 | hh>>3 ^ cy>>5 | cy>>3 ^ cy>>5) & 1
		if x^noise != 0 {
			ph[13] = x<<9 | 0xd0
		} else {
			ph[13] = x<<9 | 0x34
		}
		ph[16] = hh>>8&1<<9 | (hh>>8^noise)&1<<8
		ph[17] = x<<9 | 0x100
	}
	run := func(i int, mod int32) int32 {
		o := &f.ops[i]
		v := opOut(o.ws, ph[i]+uint32(mod), att[i])
		o.out[1], o.out[0] = o.out[0], v
		return v
	}
	fbMod := func(i int, fb int) int32 {
		if fb == 0 {
			return 0
		}
		o := &f.ops[i]
		return (o.out[0] + o.out[1]) >> uint(9-fb)
	}
	for n := 0; n < 18; n++ {
		c := &f.ch[n]
		m, k := chOps(n)
		var v int32
		switch {
		case f.isFourSecond(n):
			continue
		case f.isFourFirst(n):
			c2 := &f.ch[n+3]
			m2, k2 := chOps(n + 3)
			a := run(m, fbMod(m, c.fb))
			switch c.cnt<<1 | c2.cnt {
			case 0:
				v = run(k2, run(m2, run(k, a)))
			case 2:
				v = a + run(k2, run(m2, run(k, 0)))
			case 1:
				v = run(k, a) + run(k2, run(m2, 0))
			default:
				v = a + run(m2, run(k, 0)) + run(k2, 0)
			}
		case rhythm && n == 6:
			a := run(m, fbMod(m, c.fb))
			if c.cnt != 0 {
				a = 0
			}
			v = run(k, a) * 2
		case rhythm && n == 7:
			v = (run(m, 0) + run(k, 0)) * 2
		case rhythm && n == 8:
			v = (run(m, 0) + run(k, 0)) * 2
		default:
			a := run(m, fbMod(m, c.fb))
			if c.cnt != 0 {
				v = a + run(k, 0)
			} else {
				v = run(k, a)
			}
		}
		v = max(-32768, min(32767, v))
		outs := c.outs
		if !f.isNew {
			outs = 15
		}
		for j := range out {
			if outs>>uint(j)&1 != 0 {
				out[j] += v
			}
		}
	}
	return out
}

func (f *opl3) isFourFirst(n int) bool {
	return f.isNew && n%9 < 3 && f.four>>uint(n/9*3+n%9)&1 != 0
}

func (f *opl3) isFourSecond(n int) bool {
	return f.isNew && n%9 >= 3 && n%9 < 6 && f.four>>uint(n/9*3+n%9-3)&1 != 0
}

// envelope advances an operator's envelope by one sample.
func (f *opl3) envelope(o *fmOp, fnum, block int) {
	if o.state == egOff {
		return
	}
	ks := block<<1 | fnum>>9&1
	if f.nts {
		ks = block<<1 | fnum>>8&1
	}
	if !o.ksr {
		ks >>= 2
	}
	rate := func(r int) int {
		if r == 0 {
			return 0
		}
		return min(63, r*4+ks)
	}
	switch o.state {
	case egAttack:
		r := rate(o.ar)
		if r >= 60 {
			o.env = 0
		} else if n := egStepAt(f.egCount, r); n != 0 {
			o.env += (^o.env * n) >> 3
		}
		if o.env <= 0 {
			o.env = 0
			o.state = egDecay1
		}
	case egDecay1:
		sl := o.sl << 4
		if o.sl == 15 {
			sl = 31 << 4
		}
		if o.env >= sl {
			o.state = fmSustain
			break
		}
		o.env += egStepAt(f.egCount, rate(o.dr))
		if o.env >= sl {
			o.env = sl
			o.state = fmSustain
		}
	case fmSustain:
		if !o.egt {
			o.env += egStepAt(f.egCount, rate(o.rr))
		}
	case egRelease:
		o.env += egStepAt(f.egCount, rate(o.rr))
	}
	if o.env >= envMax {
		o.env = envMax
		if o.state != egAttack && o.state != egDecay1 {
			o.state = egOff
		}
	}
}

// fmGain scales the FM output against the wavetable voices (/256).
const fmGain = 2048
