package nes

// APU (sound), from web/emu/nes.js.

var (
	lengths     = [32]int{10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30}
	dutyTab     = [4][8]int{{0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}}
	triTab      [32]int
	noisePeriod = [16]int{4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068}
	dmcRate     = [16]int{428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54}
	pulseTab    [31]float32
	tndTab      [203]float32
)

func init() {
	for i := 0; i < 16; i++ {
		triTab[i] = 15 - i
		triTab[16+i] = i
	}
	// mixer lookup tables (non-linear mix of the analog output)
	for i := 1; i < 31; i++ {
		pulseTab[i] = float32(95.52 / (8128/float64(i) + 100))
	}
	for i := 1; i < 203; i++ {
		tndTab[i] = float32(163.67 / (24329/float64(i) + 100))
	}
}

type envelope struct {
	start          bool
	divider, decay int
	loop, constant bool
	vol            int
}

func (e *envelope) clock() {
	if e.start {
		e.start = false
		e.decay = 15
		e.divider = e.vol
		return
	}
	if e.divider > 0 {
		e.divider--
		return
	}
	e.divider = e.vol
	if e.decay > 0 {
		e.decay--
	} else if e.loop {
		e.decay = 15
	}
}

func (e *envelope) out() int {
	if e.constant {
		return e.vol
	}
	return e.decay
}

func (e *envelope) state(s *sio) {
	s.Bools(&e.start, &e.loop, &e.constant)
	s.Ints(&e.divider, &e.decay, &e.vol)
}

type pulse struct {
	second                            bool
	env                               envelope
	enabled                           bool
	len, duty, step, period, timer    int
	sweepOn, sweepNeg, sweepReload    bool
	sweepPeriod, sweepShift, sweepDiv int
}

func (p *pulse) write(r, v int) {
	switch r {
	case 0:
		p.duty = v >> 6
		p.env.loop = v&0x20 != 0
		p.env.constant = v&0x10 != 0
		p.env.vol = v & 15
	case 1:
		p.sweepOn = v&0x80 != 0
		p.sweepPeriod = (v >> 4) & 7
		p.sweepNeg = v&8 != 0
		p.sweepShift = v & 7
		p.sweepReload = true
	case 2:
		p.period = p.period&0x700 | v
	case 3:
		p.period = p.period&0xff | (v&7)<<8
		if p.enabled {
			p.len = lengths[v>>3]
		}
		p.step = 0
		p.env.start = true
	}
}

func (p *pulse) target() int {
	d := p.period >> p.sweepShift
	if p.sweepNeg {
		if p.second {
			return p.period - d
		}
		return p.period - d - 1
	}
	return p.period + d
}

func (p *pulse) clockTimer() {
	if p.timer == 0 {
		p.timer = p.period
		p.step = (p.step + 1) & 7
	} else {
		p.timer--
	}
}

func (p *pulse) clockSweep() {
	t := p.target()
	if p.sweepDiv == 0 && p.sweepOn && p.sweepShift != 0 && p.period >= 8 && t <= 0x7ff {
		p.period = t
	}
	if p.sweepDiv == 0 || p.sweepReload {
		p.sweepDiv = p.sweepPeriod
		p.sweepReload = false
	} else {
		p.sweepDiv--
	}
}

func (p *pulse) clockLength() {
	if p.len > 0 && !p.env.loop {
		p.len--
	}
}

func (p *pulse) out() int {
	if p.len == 0 || p.period < 8 || p.target() > 0x7ff || dutyTab[p.duty][p.step] == 0 {
		return 0
	}
	return p.env.out()
}

func (p *pulse) state(s *sio) {
	p.env.state(s)
	s.Bools(&p.enabled, &p.sweepOn, &p.sweepNeg, &p.sweepReload)
	s.Ints(&p.len, &p.duty, &p.step, &p.period, &p.timer, &p.sweepPeriod, &p.sweepShift, &p.sweepDiv)
}

type triangle struct {
	enabled, control, reloadFlag bool
	len, linReload, lin          int
	period, timer, step          int
}

func (t *triangle) write(r, v int) {
	switch r {
	case 0:
		t.control = v&0x80 != 0
		t.linReload = v & 0x7f
	case 2:
		t.period = t.period&0x700 | v
	case 3:
		t.period = t.period&0xff | (v&7)<<8
		if t.enabled {
			t.len = lengths[v>>3]
		}
		t.reloadFlag = true
	}
}

func (t *triangle) clockTimer() {
	if t.timer == 0 {
		t.timer = t.period
		if t.len != 0 && t.lin != 0 && t.period > 1 {
			t.step = (t.step + 1) & 31
		}
	} else {
		t.timer--
	}
}

func (t *triangle) clockLinear() {
	if t.reloadFlag {
		t.lin = t.linReload
	} else if t.lin > 0 {
		t.lin--
	}
	if !t.control {
		t.reloadFlag = false
	}
}

func (t *triangle) clockLength() {
	if t.len > 0 && !t.control {
		t.len--
	}
}

func (t *triangle) out() int { return triTab[t.step] }

func (t *triangle) state(s *sio) {
	s.Bools(&t.enabled, &t.control, &t.reloadFlag)
	s.Ints(&t.len, &t.linReload, &t.lin, &t.period, &t.timer, &t.step)
}

type noise struct {
	env                       envelope
	enabled, mode             bool
	len, period, timer, shift int
}

func (n *noise) write(r, v int) {
	switch r {
	case 0:
		n.env.loop = v&0x20 != 0
		n.env.constant = v&0x10 != 0
		n.env.vol = v & 15
	case 2:
		n.mode = v&0x80 != 0
		n.period = noisePeriod[v&15]
	case 3:
		if n.enabled {
			n.len = lengths[v>>3]
		}
		n.env.start = true
	}
}

func (n *noise) clockTimer() {
	if n.timer == 0 {
		n.timer = n.period
		sh := 1
		if n.mode {
			sh = 6
		}
		fb := (n.shift & 1) ^ ((n.shift >> sh) & 1)
		n.shift = n.shift>>1 | fb<<14
	} else {
		n.timer--
	}
}

func (n *noise) clockLength() {
	if n.len > 0 && !n.env.loop {
		n.len--
	}
}

func (n *noise) out() int {
	if n.shift&1 != 0 || n.len == 0 {
		return 0
	}
	return n.env.out()
}

func (n *noise) state(s *sio) {
	n.env.state(s)
	s.Bools(&n.enabled, &n.mode)
	s.Ints(&n.len, &n.period, &n.timer, &n.shift)
}

type dmc struct {
	apu                       *apu
	irqOn, loop               bool
	rate, timer, level        int
	addr0, len0, addr, remain int
	buffer, shift, bits       int
	silent, irq               bool
}

func (d *dmc) write(r, v int) {
	switch r {
	case 0:
		d.irqOn = v&0x80 != 0
		d.loop = v&0x40 != 0
		d.rate = dmcRate[v&15]
		if !d.irqOn {
			d.irq = false
		}
	case 1:
		d.level = v & 0x7f
	case 2:
		d.addr0 = 0xc000 | v<<6
	case 3:
		d.len0 = v<<4 | 1
	}
}

func (d *dmc) restart() { d.addr = d.addr0; d.remain = d.len0 }

// fill asks for the next sample byte: the machine pauses the CPU at its next read cycle to fetch it.
func (d *dmc) fill() {
	if d.buffer < 0 && d.remain > 0 {
		d.apu.nes.dmcReq = true
	}
}

func (d *dmc) take(v int) {
	d.buffer = v
	if d.addr == 0xffff {
		d.addr = 0x8000
	} else {
		d.addr++
	}
	d.remain--
	if d.remain == 0 {
		if d.loop {
			d.restart()
		} else if d.irqOn {
			d.irq = true
		}
	}
}

func (d *dmc) clockTimer() {
	if d.timer > 0 {
		d.timer--
		return
	}
	d.timer = d.rate - 1
	if !d.silent {
		if d.shift&1 != 0 {
			if d.level <= 125 {
				d.level += 2
			}
		} else if d.level >= 2 {
			d.level -= 2
		}
	}
	d.shift >>= 1
	d.bits--
	if d.bits <= 0 {
		d.bits = 8
		if d.buffer < 0 {
			d.silent = true
		} else {
			d.silent = false
			d.shift = d.buffer
			d.buffer = -1
		}
	}
	d.fill()
}

func (d *dmc) state(s *sio) {
	s.Bools(&d.irqOn, &d.loop, &d.silent, &d.irq)
	s.Ints(&d.rate, &d.timer, &d.level, &d.addr0, &d.len0, &d.addr, &d.remain, &d.buffer, &d.shift, &d.bits)
}

type apu struct {
	nes                  *NES
	p1, p2               pulse
	tri                  triangle
	noise                noise
	dmc                  dmc
	frameMode            int
	irqInhibit, frameIrq bool
	frameCycle           int
	odd                  bool
	frameReset           int
	last4017             int
	step, acc, sum       float64
	n                    int
	hp, hpPrev, lp       float64
	out                  []float32
	ext                  extAudio
}

func newAPU(n *NES, sampleRate int) *apu {
	a := &apu{nes: n}
	a.p2.second = true
	a.noise.period, a.noise.shift = 4, 1
	a.dmc = dmc{apu: a, rate: 428, addr0: 0xc000, len0: 1, addr: 0xc000, buffer: -1, bits: 8, silent: true}
	a.step = cpuHz / float64(sampleRate)
	a.out = make([]float32, 0, 4096)
	return a
}

func (a *apu) write(addr, v int) {
	switch {
	case addr < 0x4004:
		a.p1.write(addr&3, v)
	case addr < 0x4008:
		a.p2.write(addr&3, v)
	case addr < 0x400c:
		a.tri.write(addr&3, v)
	case addr < 0x4010:
		a.noise.write(addr&3, v)
	case addr < 0x4014:
		a.dmc.write(addr&3, v)
	case addr == 0x4015:
		a.p1.enabled = v&1 != 0
		if !a.p1.enabled {
			a.p1.len = 0
		}
		a.p2.enabled = v&2 != 0
		if !a.p2.enabled {
			a.p2.len = 0
		}
		a.tri.enabled = v&4 != 0
		if !a.tri.enabled {
			a.tri.len = 0
		}
		a.noise.enabled = v&8 != 0
		if !a.noise.enabled {
			a.noise.len = 0
		}
		a.dmc.irq = false
		if v&16 != 0 {
			if a.dmc.remain == 0 {
				a.dmc.restart()
				a.dmc.fill()
			}
		} else {
			a.dmc.remain = 0
		}
	case addr == 0x4017:
		a.last4017 = v
		a.frameMode = v >> 7
		a.irqInhibit = v&0x40 != 0
		if a.irqInhibit {
			a.frameIrq = false
		}
		// The new mode takes effect 3 or 4 CPU cycles later, depending on alignment.
		a.frameReset = 4
		if a.odd {
			a.frameReset = 5
		}
	}
}

// reset: power and the reset button turn the sound off, clear the IRQ flags,
// and write $4017 again ($00 at power, the last value at reset).
func (a *apu) reset(power bool) {
	a.write(0x4015, 0)
	a.frameIrq = false
	a.dmc.irq = false
	if power {
		a.write(0x4017, 0)
		// At power the write lands a couple of cycles before the CPU starts its reset sequence.
		a.tick()
		a.tick()
	} else {
		a.write(0x4017, a.last4017)
	}
}

func (a *apu) readStatus() int {
	v := 0
	if a.p1.len != 0 {
		v |= 1
	}
	if a.p2.len != 0 {
		v |= 2
	}
	if a.tri.len != 0 {
		v |= 4
	}
	if a.noise.len != 0 {
		v |= 8
	}
	if a.dmc.remain != 0 {
		v |= 16
	}
	if a.frameIrq {
		v |= 0x40
	}
	if a.dmc.irq {
		v |= 0x80
	}
	a.frameIrq = false
	return v
}

func (a *apu) irq() bool { return a.frameIrq || a.dmc.irq }

func (a *apu) quarter() {
	a.p1.env.clock()
	a.p2.env.clock()
	a.noise.env.clock()
	a.tri.clockLinear()
}

func (a *apu) half() {
	a.p1.clockLength()
	a.p2.clockLength()
	a.tri.clockLength()
	a.noise.clockLength()
	a.p1.clockSweep()
	a.p2.clockSweep()
}

// tick runs one CPU cycle.
func (a *apu) tick() {
	a.tri.clockTimer()
	a.odd = !a.odd
	if a.odd {
		a.p1.clockTimer()
		a.p2.clockTimer()
		a.noise.clockTimer()
	}
	a.dmc.clockTimer()
	if a.frameReset > 0 {
		a.frameReset--
		if a.frameReset == 0 {
			a.frameCycle = 0
			if a.frameMode != 0 {
				a.quarter()
				a.half()
			}
		}
	}
	a.frameCycle++
	f := a.frameCycle
	if a.frameMode == 0 {
		switch {
		case f == 7457 || f == 22371:
			a.quarter()
		case f == 14913:
			a.quarter()
			a.half()
		case f == 29828:
			if !a.irqInhibit {
				a.frameIrq = true
			}
		case f == 29829:
			a.quarter()
			a.half()
			if !a.irqInhibit {
				a.frameIrq = true
			}
		case f >= 29830:
			if !a.irqInhibit {
				a.frameIrq = true
			}
			a.frameCycle = 0
		}
	} else {
		if f == 7457 || f == 22371 {
			a.quarter()
		} else if f == 14913 || f == 37281 {
			a.quarter()
			a.half()
		}
		if f >= 37282 {
			a.frameCycle = 0
		}
	}
	// mix and resample
	s := float64(pulseTab[a.p1.out()+a.p2.out()]) + float64(tndTab[3*a.tri.out()+2*a.noise.out()+a.dmc.level])
	if a.ext != nil {
		s += a.ext.output()
	}
	a.sum += s
	a.n++
	a.acc++
	if a.acc >= a.step {
		a.acc -= a.step
		x := a.sum / float64(a.n)
		a.sum, a.n = 0, 0
		// ~37 Hz high-pass (removes DC like the console's output capacitor) and gentle low-pass
		a.hp = 0.9952 * (a.hp + x - a.hpPrev)
		a.hpPrev = x
		a.lp += (a.hp - a.lp) * 0.75
		if len(a.out) < cap(a.out) {
			a.out = append(a.out, float32(a.lp))
		}
	}
}

func (a *apu) state(s *sio) {
	a.p1.state(s)
	a.p2.state(s)
	a.tri.state(s)
	a.noise.state(s)
	a.dmc.state(s)
	s.Ints(&a.frameMode, &a.frameCycle, &a.frameReset, &a.last4017, &a.n)
	s.Bools(&a.irqInhibit, &a.frameIrq, &a.odd)
	// The web version leaves the resampler and filters out of its states; they
	// are kept here so a loaded state continues exactly.
	s.F64(&a.acc)
	s.F64(&a.sum)
	s.F64(&a.hp)
	s.F64(&a.hpPrev)
	s.F64(&a.lp)
}
