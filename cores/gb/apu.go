package gb

// Sound: the four-channel stereo APU (the APU class in gb.js).

var (
	duty     = [4]int{0x01, 0x81, 0x87, 0x7e}
	noiseDiv = [8]int{8, 16, 32, 48, 64, 80, 96, 112}
	readOr   = [...]int{0x80, 0x3f, 0x00, 0xff, 0xbf, 0xff, 0x3f, 0x00, 0xff, 0xbf, 0x7f, 0xff, 0x9f, 0xff, 0xbf, 0xff, 0xff, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x70}
)

const audioCap = 8192 // the web core's output buffer: at most this many samples between TakeAudio calls

type channel struct {
	on, dac, lenOn                   bool
	len, vol, envDir, envPeriod      int
	envTimer, freq, timer, pos, duty int
	lfsr, sweepPeriod, sweepTimer    int
	sweepShift                       int
	sweepNeg, sweepOn, negUsed       bool
	shadow, sample                   int
	fetched                          int64
}

func (c *channel) state(s *sio) {
	s.Bools(&c.on, &c.dac, &c.lenOn)
	s.Ints(&c.len, &c.vol, &c.envDir, &c.envPeriod, &c.envTimer, &c.freq, &c.timer, &c.pos, &c.duty,
		&c.lfsr, &c.sweepPeriod, &c.sweepTimer, &c.sweepShift)
	s.Bools(&c.sweepNeg, &c.sweepOn, &c.negUsed)
	s.Ints(&c.shadow, &c.sample)
	s.I64(&c.fetched)
}

type apu struct {
	gb         *GB
	regs       [0x30]byte
	wave       [16]byte
	on         bool
	ch         [4]channel
	seqStep    int
	acc, step  float64
	sumL, sumR float64
	n          int
	hpL, hpR   float64
	prevL      float64
	prevR      float64
	out        []float32
	now        int64
}

func newAPU(sampleRate int, gb *GB) *apu {
	a := &apu{gb: gb, on: true, step: 4194304 / float64(sampleRate), out: make([]float32, 0, audioCap)}
	for i := range a.ch {
		a.ch[i].lfsr = 0x7fff
	}
	return a
}

func (a *apu) cgb() bool { return a.gb != nil && a.gb.cgb }

// waveIndex: while channel 3 plays, the CPU sees wave RAM only through the
// byte the channel is reading (always on the Color; on the original only in
// the moment the channel reads it). -1: not playing, -2: unreadable.
func (a *apu) waveIndex() int {
	c := &a.ch[2]
	if !c.on {
		return -1
	}
	if a.cgb() || a.now-c.fetched < 2 {
		return c.pos >> 1
	}
	return -2
}

func (a *apu) read(addr int) int {
	if addr >= 0xff30 {
		i := a.waveIndex()
		switch {
		case i == -1:
			return int(a.wave[addr-0xff30])
		case i == -2:
			return 0xff
		}
		return int(a.wave[i])
	}
	r := addr - 0xff10
	if addr == 0xff26 {
		v := 0x70
		if a.on {
			v |= 0x80
		}
		for i := range a.ch {
			if a.ch[i].on {
				v |= 1 << i
			}
		}
		return v
	}
	if r >= len(readOr) {
		return 0xff
	}
	return int(a.regs[r]) | readOr[r]
}

func (a *apu) write(addr, v int) {
	if addr >= 0xff30 {
		i := a.waveIndex()
		if i == -1 {
			a.wave[addr-0xff30] = byte(v)
		} else if i >= 0 {
			a.wave[i] = byte(v)
		}
		return
	}
	r := addr - 0xff10
	if addr == 0xff26 {
		on := v&0x80 != 0
		if !on && a.on {
			a.powerOff()
		}
		if on && !a.on {
			a.seqStep = 0
			for i := range a.ch {
				a.ch[i].pos = 0
				a.ch[i].sample = 0
			}
		}
		a.on = on
		return
	}
	if r >= 0x17 {
		return
	}
	if !a.on {
		// powered off: only the original Game Boy still accepts the length counters
		if !a.cgb() && (r == 1 || r == 6 || r == 11 || r == 16) {
			ch := r / 5
			c := &a.ch[ch]
			if ch == 2 {
				c.len = 256 - v
			} else {
				c.len = 64 - (v & 0x3f)
			}
		}
		return
	}
	a.regs[r] = byte(v)
	ch, sub := r/5, r%5
	if ch > 3 {
		return
	}
	c := &a.ch[ch]
	switch sub {
	case 0:
		if ch == 0 {
			c.sweepPeriod = (v >> 4) & 7
			c.sweepShift = v & 7
			neg := v&8 != 0
			if c.sweepNeg && !neg && c.negUsed {
				c.on = false // leaving subtract mode after using it
			}
			c.sweepNeg = neg
		}
		if ch == 2 {
			c.dac = v&0x80 != 0
			if !c.dac {
				c.on = false
			}
		}
	case 1:
		if ch == 2 {
			c.len = 256 - v
		} else {
			c.len = 64 - (v & 0x3f)
			c.duty = v >> 6
		}
	case 2:
		if ch != 2 {
			c.dac = v&0xf8 != 0
			if !c.dac {
				c.on = false
			}
		}
	case 3:
		if ch != 3 {
			c.freq = (c.freq & 0x700) | v
		}
	case 4:
		if ch != 3 {
			c.freq = (c.freq & 0xff) | ((v & 7) << 8)
		}
		wasOn, nowOn := c.lenOn, v&0x40 != 0
		c.lenOn = nowOn
		// Turning the length counter on in the first half of a length period clocks it once.
		extra := a.seqStep&1 == 1
		if extra && nowOn && !wasOn && c.len > 0 {
			c.len--
			if c.len == 0 && v&0x80 == 0 {
				c.on = false
			}
		}
		if v&0x80 != 0 {
			a.trigger(ch, extra && nowOn)
		}
	}
}

func (a *apu) powerOff() {
	for i := 0; i < 0x16; i++ {
		a.regs[i] = 0
	}
	for i := range a.ch {
		c := &a.ch[i]
		c.on, c.dac, c.lenOn = false, false, false
		c.vol, c.envDir, c.envPeriod, c.freq, c.duty = 0, 0, 0, 0, 0
		c.sweepPeriod, c.sweepShift = 0, 0
		c.sweepNeg, c.sweepOn = false, false
		if a.cgb() {
			c.len = 0 // the original keeps its length counters
		}
	}
}

func (a *apu) trigger(i int, extraClock bool) {
	c := &a.ch[i]
	R := &a.regs
	base := i * 5
	// DMG: restarting channel 3 just as it reads wave RAM scrambles the first bytes
	if i == 2 && c.on && !a.cgb() && c.timer <= 2 {
		next := ((c.pos + 1) & 31) >> 1
		if next < 4 {
			a.wave[0] = a.wave[next]
		} else {
			b := next &^ 3
			for k := 0; k < 4; k++ {
				a.wave[k] = a.wave[b+k]
			}
		}
	}
	c.on = c.dac
	if c.len == 0 {
		if i == 2 {
			c.len = 256
		} else {
			c.len = 64
		}
		if extraClock {
			c.len--
		}
	}
	switch i {
	case 2:
		c.timer = (2048-c.freq)*2 + 6
		c.pos = 0
	case 3:
		v := int(R[base+3])
		c.timer = noiseDiv[v&7] << (v >> 4)
		c.lfsr = 0x7fff
	default:
		c.timer = (2048-c.freq)*4 + (c.timer & 3)
	}
	if i != 2 {
		e := int(R[base+2])
		c.vol = e >> 4
		c.envDir = -1
		if e&8 != 0 {
			c.envDir = 1
		}
		c.envPeriod = e & 7
		c.envTimer = c.envPeriod
		if c.envTimer == 0 {
			c.envTimer = 8
		}
	}
	if i == 0 {
		c.shadow = c.freq
		c.sweepTimer = c.sweepPeriod
		if c.sweepTimer == 0 {
			c.sweepTimer = 8
		}
		c.sweepOn = c.sweepPeriod != 0 || c.sweepShift != 0
		c.negUsed = false
		if c.sweepShift != 0 && a.sweepCalc(c) > 2047 {
			c.on = false
		}
	}
}

func (a *apu) sweepCalc(c *channel) int {
	d := c.shadow >> c.sweepShift
	if c.sweepNeg {
		c.negUsed = true
		return c.shadow - d
	}
	return c.shadow + d
}

// clockSeq: the frame sequencer, 512 Hz, from the divider.
func (a *apu) clockSeq() {
	if !a.on {
		return
	}
	s := a.seqStep
	if s&1 == 0 {
		for i := range a.ch {
			c := &a.ch[i]
			if c.lenOn && c.len > 0 {
				c.len--
				if c.len == 0 {
					c.on = false
				}
			}
		}
	}
	if s == 2 || s == 6 {
		c := &a.ch[0]
		c.sweepTimer--
		if c.sweepTimer <= 0 {
			c.sweepTimer = c.sweepPeriod
			if c.sweepTimer == 0 {
				c.sweepTimer = 8
			}
			if c.sweepOn && c.sweepPeriod != 0 {
				f := a.sweepCalc(c)
				if f > 2047 {
					c.on = false
				} else if c.sweepShift != 0 {
					c.shadow = f
					c.freq = f
					if a.sweepCalc(c) > 2047 {
						c.on = false
					}
				}
			}
		}
	}
	if s == 7 {
		for _, i := range [3]int{0, 1, 3} {
			c := &a.ch[i]
			if c.envPeriod != 0 {
				c.envTimer--
				if c.envTimer <= 0 {
					c.envTimer = c.envPeriod
					nv := c.vol + c.envDir
					if nv >= 0 && nv <= 15 {
						c.vol = nv
					}
				}
			}
		}
	}
	a.seqStep = (s + 1) & 7
}

// tick advances n T-cycles (4.19 MHz).
func (a *apu) tick(n int) {
	t0 := a.now
	a.now += int64(n)
	if !a.on {
		a.mix(n, 0, 0)
		return
	}
	for i := 0; i < 4; i++ {
		c := &a.ch[i]
		c.timer -= n
		for c.timer <= 0 {
			switch i {
			case 2:
				at := a.now + int64(c.timer) // when this read happened
				c.timer += (2048 - c.freq) * 2
				c.pos = (c.pos + 1) & 31
				b := int(a.wave[c.pos>>1])
				if c.pos&1 != 0 {
					c.sample = b & 15
				} else {
					c.sample = b >> 4
				}
				if at > t0 {
					c.fetched = at
				} else {
					c.fetched = t0
				}
			case 3:
				v := int(a.regs[3*5+3])
				c.timer += noiseDiv[v&7] << (v >> 4)
				x := (c.lfsr ^ (c.lfsr >> 1)) & 1
				c.lfsr = (c.lfsr >> 1) | (x << 14)
				if v&8 != 0 {
					c.lfsr = (c.lfsr &^ 0x40) | (x << 6)
				}
			default:
				c.timer += (2048 - c.freq) * 4
				c.pos = (c.pos + 1) & 7
			}
		}
	}
	// channel outputs (0..15) -> DAC (-1..1)
	c0, c1, c2, c3 := &a.ch[0], &a.ch[1], &a.ch[2], &a.ch[3]
	var os [4]int
	if c0.on && (duty[c0.duty]>>(7-c0.pos))&1 != 0 {
		os[0] = c0.vol
	}
	if c1.on && (duty[c1.duty]>>(7-c1.pos))&1 != 0 {
		os[1] = c1.vol
	}
	if c2.on {
		if vc := (int(a.regs[2*5+2]) >> 5) & 3; vc != 0 {
			os[2] = c2.sample >> (vc - 1)
		}
	}
	if c3.on && c3.lfsr&1 == 0 {
		os[3] = c3.vol
	}
	pan, vol := int(a.regs[0x15]), int(a.regs[0x14])
	L, R := 0.0, 0.0
	for i := 0; i < 4; i++ {
		dac := 0.0
		if a.ch[i].dac {
			dac = float64(os[i])/7.5 - 1
		}
		if pan&(0x10<<i) != 0 {
			L += dac
		}
		if pan&(1<<i) != 0 {
			R += dac
		}
	}
	L *= float64(((vol>>4)&7)+1) / 8
	R *= float64((vol&7)+1) / 8
	a.mix(n, L/4, R/4)
}

func (a *apu) mix(n int, L, R float64) {
	fn := float64(n)
	a.sumL += L * fn
	a.sumR += R * fn
	a.n += n
	a.acc += fn
	for a.acc >= a.step {
		a.acc -= a.step
		l, r := 0.0, 0.0
		if a.n != 0 {
			l, r = a.sumL/float64(a.n), a.sumR/float64(a.n)
		}
		a.sumL, a.sumR, a.n = 0, 0, 0
		a.hpL = 0.996 * (a.hpL + l - a.prevL)
		a.prevL = l
		a.hpR = 0.996 * (a.hpR + r - a.prevR)
		a.prevR = r
		if len(a.out) < audioCap-1 {
			a.out = append(a.out, float32(a.hpL*0.6), float32(a.hpR*0.6))
		}
	}
}

func (a *apu) state(s *sio) {
	s.Bytes(a.regs[:])
	s.Bytes(a.wave[:])
	s.Bool(&a.on)
	for i := range a.ch {
		a.ch[i].state(s)
	}
	s.Int(&a.seqStep)
	s.I64(&a.now)
	// (the web core leaves these out; keeping them makes a loaded state continue exactly)
	s.F64(&a.acc)
	s.F64(&a.sumL)
	s.F64(&a.sumR)
	s.Int(&a.n)
	s.F64(&a.hpL)
	s.F64(&a.hpR)
	s.F64(&a.prevL)
	s.F64(&a.prevR)
}
