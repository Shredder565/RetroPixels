package nes

import (
	"math"
	"sync"
)

// Famicom cartridge sound chips (web/emu/nes_audio.js). Each chip is clocked
// once per CPU cycle and its output() is added to the console's mix.

// ---- Konami VRC6 ----

type vrc6Pulse struct {
	vol, duty, mode, period int
	on                      bool
	timer, step             int
}

type vrc6 struct {
	pulses             [2]vrc6Pulse
	sawRate, sawPeriod int
	sawOn              bool
	sawTimer, sawPhase int
	sawAcc             int
	halt               bool
	shift              int
	level              float64
}

func newVRC6() *vrc6 {
	a := &vrc6{}
	a.pulses[0].step, a.pulses[1].step = 15, 15
	return a
}

// ch: 0 = pulse 1 ($9000), 1 = pulse 2 ($A000), 2 = sawtooth ($B000)
func (a *vrc6) write(ch, r, v int) {
	if ch == 0 && r == 3 {
		a.halt = v&1 != 0
		switch {
		case v&4 != 0:
			a.shift = 8
		case v&2 != 0:
			a.shift = 4
		default:
			a.shift = 0
		}
		return
	}
	if ch < 2 {
		p := &a.pulses[ch]
		switch r {
		case 0:
			p.mode, p.duty, p.vol = v>>7, (v>>4)&7, v&15
		case 1:
			p.period = p.period&0xf00 | v
		case 2:
			p.period = p.period&0xff | (v&15)<<8
			p.on = v&0x80 != 0
			if !p.on {
				p.step = 15
			}
		}
	} else {
		switch r {
		case 0:
			a.sawRate = v & 0x3f
		case 1:
			a.sawPeriod = a.sawPeriod&0xf00 | v
		case 2:
			a.sawPeriod = a.sawPeriod&0xff | (v&15)<<8
			a.sawOn = v&0x80 != 0
			if !a.sawOn {
				a.sawAcc, a.sawPhase = 0, 0
			}
		}
	}
}

func (a *vrc6) clock() {
	if a.halt {
		return
	}
	out := 0
	for i := range a.pulses {
		p := &a.pulses[i]
		if p.on {
			p.timer--
			if p.timer < 0 {
				p.timer = p.period >> a.shift
				p.step = (p.step - 1) & 15
			}
			if p.mode != 0 || p.step <= p.duty {
				out += p.vol
			}
		}
	}
	if a.sawOn {
		a.sawTimer--
		if a.sawTimer < 0 {
			a.sawTimer = a.sawPeriod >> a.shift
			a.sawPhase++
			if a.sawPhase == 14 {
				a.sawPhase, a.sawAcc = 0, 0
			} else if a.sawPhase&1 == 0 {
				a.sawAcc = (a.sawAcc + a.sawRate) & 0xff
			}
		}
		out += a.sawAcc >> 3
	}
	a.level = float64(out) * 0.0095
}

func (a *vrc6) output() float64 { return a.level }

func (a *vrc6) state(s *sio) {
	for i := range a.pulses {
		p := &a.pulses[i]
		s.Ints(&p.vol, &p.duty, &p.mode, &p.period, &p.timer, &p.step)
		s.Bool(&p.on)
	}
	s.Ints(&a.sawRate, &a.sawPeriod, &a.sawTimer, &a.sawPhase, &a.sawAcc, &a.shift)
	s.Bools(&a.sawOn, &a.halt)
	s.F64(&a.level)
}

// ---- Namco 163: 128 bytes of internal RAM hold both the channel settings
// and the 4-bit wave samples. One channel is updated every 15 CPU cycles. ----

type n163 struct {
	ram      [128]byte
	addr     int
	disabled bool
	sub, cur int
	outs     [8]float32
	level    float64
}

func newN163() *n163 { return &n163{cur: 7} }

func (a *n163) writeData(v int) {
	a.ram[a.addr&0x7f] = byte(v)
	if a.addr&0x80 != 0 {
		a.addr = 0x80 | (a.addr+1)&0x7f
	}
}

func (a *n163) readData() int {
	v := int(a.ram[a.addr&0x7f])
	if a.addr&0x80 != 0 {
		a.addr = 0x80 | (a.addr+1)&0x7f
	}
	return v
}

func (a *n163) clock() {
	if a.disabled {
		return
	}
	a.sub++
	if a.sub < 15 {
		return
	}
	a.sub = 0
	ram := &a.ram
	count := int((ram[0x7f]>>4)&7) + 1
	first := 8 - count
	if a.cur < first {
		a.cur = 7
	}
	b := 0x40 + a.cur*8
	freq := int(ram[b]) | int(ram[b+2])<<8 | int(ram[b+4]&3)<<16
	ln := (256 - int(ram[b+4]&0xfc)) << 16
	phase := int(ram[b+1]) | int(ram[b+3])<<8 | int(ram[b+5])<<16
	phase = (phase + freq) % ln
	ram[b+1], ram[b+3], ram[b+5] = byte(phase), byte(phase>>8), byte(phase>>16)
	idx := ((phase >> 16) + int(ram[b+6])) & 0xff
	sample := int(ram[idx>>1]>>((idx&1)<<2)) & 15
	a.outs[a.cur] = float32((sample - 8) * int(ram[b+7]&15))
	if a.cur == first {
		a.cur = 7
	} else {
		a.cur--
	}
	// The chip plays the active channels one after another; averaging them gives the same mix.
	sum := 0.0
	for i := first; i < 8; i++ {
		sum += float64(a.outs[i])
	}
	a.level = (sum / float64(count)) * 0.0022
}

func (a *n163) output() float64 {
	if a.disabled {
		return 0
	}
	return a.level
}

func (a *n163) state(s *sio) {
	s.Bytes(a.ram[:])
	s.Ints(&a.addr, &a.sub, &a.cur)
	s.Bool(&a.disabled)
	s.F32s(a.outs[:])
	s.F64(&a.level)
}

// ---- Sunsoft 5B (a YM2149-style chip in Gimmick!): 3 square channels, noise,
// and a 32-step volume envelope. Volume steps are logarithmic. ----

var vol5B [32]float32

type s5bTone struct{ count, out int }

type s5b struct {
	regs               [16]byte
	addr, pre          int
	tone               [3]s5bTone
	noiseCount, lfsr   int
	envCount, envPos   int
	envAttack, envHold bool
	envVal             int
	level              float64
}

func newS5B() *s5b { initChipTables(); return &s5b{lfsr: 1} }

func (a *s5b) writeAddr(v int) { a.addr = v }
func (a *s5b) writeData(v int) {
	if a.addr&0xf0 != 0 {
		return
	}
	r := a.addr & 15
	a.regs[r] = byte(v)
	if r == 13 {
		a.envPos, a.envHold, a.envAttack, a.envCount = 0, false, v&4 != 0, 0
		a.envVal = 31
		if a.envAttack {
			a.envVal = 0
		}
	}
}

func (a *s5b) envStep() {
	if a.envHold {
		return
	}
	a.envPos++
	if a.envPos > 31 {
		s := a.regs[13]
		if s&8 == 0 {
			a.envHold, a.envVal = true, 0
			return
		}
		if s&1 != 0 {
			if s&2 != 0 {
				a.envAttack = !a.envAttack
			}
			a.envHold = true
			a.envVal = 0
			if a.envAttack {
				a.envVal = 31
			}
			return
		}
		if s&2 != 0 {
			a.envAttack = !a.envAttack
		}
		a.envPos = 0
	}
	if a.envAttack {
		a.envVal = a.envPos
	} else {
		a.envVal = 31 - a.envPos
	}
}

func (a *s5b) clock() {
	a.pre++
	if a.pre < 16 {
		return
	}
	a.pre = 0
	r := &a.regs
	for i := 0; i < 3; i++ {
		t := &a.tone[i]
		period := int(r[i*2]) | int(r[i*2+1]&15)<<8
		if period == 0 {
			period = 1
		}
		t.count++
		if t.count >= period {
			t.count = 0
			t.out ^= 1
		}
	}
	np := int(r[6] & 0x1f)
	if np == 0 {
		np = 1
	}
	np *= 2
	a.noiseCount++
	if a.noiseCount >= np {
		a.noiseCount = 0
		bit := (a.lfsr ^ a.lfsr>>3) & 1
		a.lfsr = a.lfsr>>1 | bit<<16
	}
	ep := int(r[11]) | int(r[12])<<8
	if ep == 0 {
		ep = 1
	}
	a.envCount++
	if a.envCount >= ep {
		a.envCount = 0
		a.envStep()
	}
	out := 0.0
	noise, mix := a.lfsr&1, int(r[7])
	for i := 0; i < 3; i++ {
		on := (a.tone[i].out | (mix>>i)&1) & (noise | (mix>>(i+3))&1)
		if on == 0 {
			continue
		}
		v := int(r[8+i])
		if v&0x10 != 0 {
			out += float64(vol5B[a.envVal])
		} else if v&15 != 0 {
			out += float64(vol5B[(v&15)*2+1])
		}
	}
	a.level = out * 0.45
}

func (a *s5b) output() float64 { return a.level }

func (a *s5b) state(s *sio) {
	s.Bytes(a.regs[:])
	s.Ints(&a.addr, &a.pre, &a.noiseCount, &a.lfsr, &a.envCount, &a.envPos, &a.envVal)
	for i := range a.tone {
		s.Ints(&a.tone[i].count, &a.tone[i].out)
	}
	s.Bools(&a.envAttack, &a.envHold)
	s.F64(&a.level)
}

// ---- MMC5: two pulse channels like the console's (without sweep) and a raw 8-bit PCM output. ----

var mmc5Duty = [4]int{0x02, 0x06, 0x1e, 0xf9} // 8-step duty patterns as bit masks

type mmc5Pulse struct {
	duty               int
	loop, constant     bool
	vol, period, timer int
	step, len          int
	on, envStart       bool
	envDiv, envDecay   int
}

type mmc5Audio struct {
	p          [2]mmc5Pulse
	pcm        int
	pcmRead    bool
	frame, odd int
	level      float64
}

func newMMC5Audio() *mmc5Audio { return &mmc5Audio{} }

func (a *mmc5Audio) write(addr, v int) {
	switch {
	case addr >= 0x5000 && addr <= 0x5007:
		p := &a.p[(addr>>2)&1]
		switch addr & 3 {
		case 0:
			p.duty, p.loop, p.constant, p.vol = v>>6, v&0x20 != 0, v&0x10 != 0, v&15
		case 2:
			p.period = p.period&0x700 | v
		case 3:
			p.period = p.period&0xff | (v&7)<<8
			if p.on {
				p.len = lengths[v>>3]
			}
			p.step = 0
			p.envStart = true
		}
	case addr == 0x5010:
		a.pcmRead = v&1 != 0
	case addr == 0x5011:
		if !a.pcmRead && v != 0 {
			a.pcm = v
		}
	case addr == 0x5015:
		for i := 0; i < 2; i++ {
			a.p[i].on = v&(1<<i) != 0
			if !a.p[i].on {
				a.p[i].len = 0
			}
		}
	}
}

func (a *mmc5Audio) status() int {
	v := 0
	if a.p[0].len != 0 {
		v |= 1
	}
	if a.p[1].len != 0 {
		v |= 2
	}
	return v
}

func (a *mmc5Audio) clock() {
	a.odd ^= 1
	out := 0
	a.frame++
	quarter := a.frame >= 7457
	if quarter {
		a.frame = 0
	}
	for i := range a.p {
		p := &a.p[i]
		if a.odd != 0 {
			p.timer--
			if p.timer < 0 {
				p.timer = p.period
				p.step = (p.step - 1) & 7
			}
		}
		if quarter { // envelope and length counter run at a steady 240 Hz
			if p.envStart {
				p.envStart = false
				p.envDecay = 15
				p.envDiv = p.vol
			} else if p.envDiv > 0 {
				p.envDiv--
			} else {
				p.envDiv = p.vol
				if p.envDecay > 0 {
					p.envDecay--
				} else if p.loop {
					p.envDecay = 15
				}
			}
			if p.len > 0 && !p.loop {
				p.len--
			}
		}
		if p.len != 0 && (mmc5Duty[p.duty]>>p.step)&1 != 0 {
			if p.constant {
				out += p.vol
			} else {
				out += p.envDecay
			}
		}
	}
	a.level = float64(out)*0.0095 + float64(a.pcm)*0.0021
}

func (a *mmc5Audio) output() float64 { return a.level }

func (a *mmc5Audio) state(s *sio) {
	for i := range a.p {
		p := &a.p[i]
		s.Ints(&p.duty, &p.vol, &p.period, &p.timer, &p.step, &p.len, &p.envDiv, &p.envDecay)
		s.Bools(&p.loop, &p.constant, &p.on, &p.envStart)
	}
	s.Ints(&a.pcm, &a.frame, &a.odd)
	s.Bool(&a.pcmRead)
	s.F64(&a.level)
}

// ---- Konami VRC7: a 6-channel, 2-operator FM synthesizer (a cut-down OPLL).
// Each channel's modulator operator bends the phase of its carrier operator;
// both follow attack/decay/sustain/release envelopes. The chip produces one
// sample every 36 CPU cycles (about 49.7 kHz). ----

// The chip has 15 built-in instruments (instrument 0 is the game's own, set in
// registers $00-$07). RetroPixels uses its own set (see nes_audio.js), packed
// into the chip's 8-byte layout.
type fmOpDef struct{ mult, ar, dr, sl, rr, am, vib, sus, ksr, half, ksl int }

func fmVoice(m, c fmOpDef, tl, fb int) [8]byte {
	flags := func(o fmOpDef) int { return o.am<<7 | o.vib<<6 | o.sus<<5 | o.ksr<<4 | o.mult }
	return [8]byte{byte(flags(m)), byte(flags(c)), byte(m.ksl<<6 | tl), byte(c.ksl<<6 | c.half<<4 | m.half<<3 | fb),
		byte(m.ar<<4 | m.dr), byte(c.ar<<4 | c.dr), byte(m.sl<<4 | m.rr), byte(c.sl<<4 | c.rr)}
}

var vrc7Patches = [16][8]byte{
	{},
	// 1 bowed strings: slow swell, vibrato, held
	fmVoice(fmOpDef{mult: 1, vib: 1, sus: 1, ar: 7, dr: 4, sl: 2, rr: 4}, fmOpDef{mult: 1, vib: 1, sus: 1, ar: 7, dr: 2, sl: 1, rr: 6}, 22, 5),
	// 2 plucked guitar: bright attack that dies away
	fmVoice(fmOpDef{mult: 3, ksr: 1, ar: 15, dr: 5, sl: 3, rr: 4}, fmOpDef{mult: 1, ar: 15, dr: 3, sl: 5, rr: 4}, 24, 6),
	// 3 piano: hammered, darker towards the top
	fmVoice(fmOpDef{mult: 1, ksr: 1, ar: 14, dr: 6, sl: 4, rr: 3}, fmOpDef{mult: 1, ksr: 1, ksl: 1, ar: 15, dr: 3, sl: 6, rr: 5}, 16, 3),
	// 4 flute: soft, breathy start
	fmVoice(fmOpDef{mult: 1, vib: 1, sus: 1, ar: 6, dr: 1, sl: 3, rr: 7}, fmOpDef{mult: 1, vib: 1, sus: 1, ar: 8, dr: 1, sl: 0, rr: 7}, 34, 6),
	// 5 clarinet: hollow (modulator an octave up, half-wave)
	fmVoice(fmOpDef{mult: 2, sus: 1, half: 1, ar: 9, dr: 2, sl: 1, rr: 6}, fmOpDef{mult: 1, vib: 1, sus: 1, ar: 9, dr: 1, sl: 0, rr: 7}, 20, 2),
	// 6 bell: inharmonic ring
	fmVoice(fmOpDef{mult: 5, ar: 15, dr: 4, sl: 6, rr: 3}, fmOpDef{mult: 1, ar: 15, dr: 2, sl: 8, rr: 2}, 18, 0),
	// 7 trumpet: brassy, with vibrato
	fmVoice(fmOpDef{mult: 1, vib: 1, sus: 1, ar: 8, dr: 4, sl: 3, rr: 6}, fmOpDef{mult: 1, vib: 1, sus: 1, ar: 9, dr: 2, sl: 1, rr: 7}, 14, 7),
	// 8 organ: steady, quick on and off
	fmVoice(fmOpDef{mult: 2, sus: 1, ar: 13, dr: 0, sl: 0, rr: 8}, fmOpDef{mult: 1, sus: 1, ar: 13, dr: 0, sl: 0, rr: 8}, 26, 4),
	// 9 celesta: soft bell
	fmVoice(fmOpDef{mult: 4, ar: 15, dr: 6, sl: 7, rr: 3}, fmOpDef{mult: 2, ar: 15, dr: 3, sl: 9, rr: 3}, 28, 1),
	// 10 xylophone: short and woody
	fmVoice(fmOpDef{mult: 7, ar: 15, dr: 8, sl: 10, rr: 6}, fmOpDef{mult: 3, ar: 15, dr: 6, sl: 12, rr: 5}, 20, 0),
	// 11 vibraphone: tremolo on both operators
	fmVoice(fmOpDef{mult: 1, am: 1, ar: 15, dr: 5, sl: 5, rr: 3}, fmOpDef{mult: 1, am: 1, half: 1, ar: 15, dr: 3, sl: 6, rr: 3}, 30, 2),
	// 12 brass section: fuller and slower than the trumpet
	fmVoice(fmOpDef{mult: 1, vib: 1, sus: 1, ar: 7, dr: 5, sl: 4, rr: 5}, fmOpDef{mult: 1, vib: 1, sus: 1, ar: 10, dr: 3, sl: 2, rr: 6}, 12, 6),
	// 13 bass: plucked low end
	fmVoice(fmOpDef{mult: 0, ksr: 1, half: 1, ar: 15, dr: 7, sl: 5, rr: 4}, fmOpDef{mult: 1, sus: 1, ar: 15, dr: 4, sl: 4, rr: 5}, 18, 5),
	// 14 synth lead: buzzy and held
	fmVoice(fmOpDef{mult: 1, sus: 1, ar: 15, dr: 2, sl: 2, rr: 7}, fmOpDef{mult: 1, sus: 1, half: 1, ar: 15, dr: 1, sl: 1, rr: 8}, 10, 7),
	// 15 chorus pad: slow, shimmering
	fmVoice(fmOpDef{mult: 1, am: 1, vib: 1, sus: 1, ar: 5, dr: 2, sl: 2, rr: 4}, fmOpDef{mult: 2, am: 1, vib: 1, sus: 1, ar: 6, dr: 1, sl: 1, rr: 5}, 26, 3),
}

const (
	fmRate = 1789773.0 / 36
	dbMax  = 48.0 // the envelope's range; quieter than this is silence
)

var (
	fmMult   = [16]float64{0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15}
	kslDB    [16]float64
	fmSin    [4096]float64
	dbToAmp  [4097]float64 // attenuation in 1/64 dB steps up to 64 dB
	fmDecay  [64]float64
	fmAttack [64]float64
	kslMul   = [4]float64{0, 0.25, 0.5, 1}
)

var chipTablesOnce sync.Once

// initChipTables builds the sound chip tables (on first use: they take a
// moment to compute exactly).
func initChipTables() { chipTablesOnce.Do(buildChipTables) }

func buildChipTables() {
	for i := 1; i < 32; i++ {
		vol5B[i] = float32(jsPow(10, -float64(31-i)*1.5/20))
	}
	// Key-level scaling: extra attenuation for higher notes, in dB at block 7 for the
	// top 4 bits of the frequency number: 3 dB per octave above the lowest, on
	// the chip's 0.375 dB step (rounded up), starting at 9 dB.
	for i := 1; i < 16; i++ {
		kslDB[i] = math.Ceil((9+3*jsLog2(float64(i)))/0.375-1e-9) * 0.375
	}
	for i := range fmSin {
		fmSin[i] = jsSin(float64(i) * (math.Pi * 2) / 4096)
	}
	for i := range dbToAmp {
		dbToAmp[i] = jsPow(10, -(float64(i)/64)/20)
	}
	for r := 0; r < 64; r++ {
		// decay: dB per sample, doubling every 4 rate steps
		if r >= 4 {
			fmDecay[r] = 1.61 * math.Pow(2, float64((r>>2)-15)) * (1 + float64(r&3)/4)
		}
		// attack: exponential approach to full volume; time halves every 4 rate steps
		ms := 2826 / jsPow(2, float64(r-4)/4)
		switch {
		case r < 4:
			fmAttack[r] = 0
		case r >= 60:
			fmAttack[r] = 1
		default:
			fmAttack[r] = math.Min(1, 4.85/(ms/1000*fmRate))
		}
	}
}

func fmAmp(db float64) float64 {
	if db >= 64 {
		return 0
	}
	return dbToAmp[int32(db*64)]
}

type fmOp struct {
	phase, env float64
	state      int
	out, prev  float64
}

type vrc7 struct {
	regs    [0x40]byte
	addr    int
	mute    bool
	div     int
	level   float64
	amPhase float64
	pmPhase float64
	ops     [12]fmOp
}

func newVRC7() *vrc7 {
	initChipTables()
	a := &vrc7{}
	for i := range a.ops {
		a.ops[i].env = dbMax
	}
	return a
}

func (a *vrc7) writeAddr(v int) { a.addr = v & 0x3f }
func (a *vrc7) writeData(v int) {
	r := a.addr
	old := int(a.regs[r])
	a.regs[r] = byte(v)
	if r >= 0x20 && r <= 0x25 {
		ch := r & 7
		if v&0x10 != 0 && old&0x10 == 0 { // key on: restart both operators
			for _, op := range []*fmOp{&a.ops[ch*2], &a.ops[ch*2+1]} {
				op.state = 1
				op.phase = 0
			}
		} else if v&0x10 == 0 && old&0x10 != 0 {
			for _, op := range []*fmOp{&a.ops[ch*2], &a.ops[ch*2+1]} {
				if op.state != 0 {
					op.state = 4
				}
			}
		}
	}
}

func (a *vrc7) patch(ch int) []byte {
	inst := a.regs[0x30+ch] >> 4
	if inst != 0 {
		return vrc7Patches[inst][:]
	}
	return a.regs[:]
}

// envelope advances one operator's envelope; returns its attenuation in dB.
func (a *vrc7) envelope(op *fmOp, p []byte, isCar, ch, fnum, block int) float64 {
	flags, adr, slrr := int(p[isCar]), int(p[4+isCar]), int(p[6+isCar])
	rks := block >> 1
	if flags&0x10 != 0 {
		rks = block<<1 | fnum>>8
	}
	rate := func(r int) int {
		if r == 0 {
			return 0
		}
		return min(63, r*4+rks)
	}
	switch op.state {
	case 1: // attack
		k := fmAttack[rate(adr>>4)]
		d := 0.0
		if k > 0 {
			d = 0.001
		}
		op.env -= op.env*k + d
		if op.env <= 0.05 {
			op.env = 0
			op.state = 2
		}
	case 2: // decay to the sustain level
		op.env += fmDecay[rate(adr&15)]
		sl := float64((slrr >> 4) * 3)
		if op.env >= sl {
			op.env = sl
			op.state = 3
		}
	case 3: // sustain: held for sustained instruments, otherwise keeps fading at the release rate
		if flags&0x20 == 0 {
			op.env += fmDecay[rate(slrr&15)]
		}
	case 4: // released
		var r int
		switch {
		case a.regs[0x20+ch]&0x20 != 0:
			r = 5
		case flags&0x20 != 0:
			r = slrr & 15
		default:
			r = 7
		}
		op.env += fmDecay[rate(r)]
	}
	if op.env >= dbMax {
		op.env = dbMax
		if op.state == 4 {
			op.state = 0
		}
	}
	return op.env
}

// jsMod1 is JS's x % 1.
func jsMod1(x float64) float64 { return math.Mod(x, 1) }

func sinIndex(x float64) int { return int(int32((jsMod1(x)+1)*4096)) & 4095 }

func (a *vrc7) sample() float64 {
	a.amPhase += 3.7 / fmRate
	if a.amPhase >= 1 {
		a.amPhase--
	}
	a.pmPhase += 6.4 / fmRate
	if a.pmPhase >= 1 {
		a.pmPhase--
	}
	amDb := 4.8 * (1 + fmSin[int32(a.amPhase*4096)]) / 2
	pm := 1 + 0.0081*fmSin[int32(a.pmPhase*4096)]
	mix := 0.0
	for ch := 0; ch < 6; ch++ {
		mod, car := &a.ops[ch*2], &a.ops[ch*2+1]
		if mod.state == 0 && car.state == 0 {
			continue
		}
		r := &a.regs
		p := a.patch(ch)
		fnum, block := int(r[0x10+ch])|int(r[0x20+ch]&1)<<8, int(r[0x20+ch]>>1)&7
		base := float64(fnum) * math.Pow(2, float64(block-1)) / 262144
		kslBase := math.Max(0, kslDB[fnum>>5]-float64(6*(7-block)))
		// modulator
		inc := base * fmMult[p[0]&15]
		if p[0]&0x40 != 0 {
			inc *= pm
		}
		mod.phase = jsMod1(mod.phase + inc)
		att := a.envelope(mod, p, 0, ch, fnum, block) + float64(p[2]&0x3f)*0.75 + kslBase*kslMul[p[2]>>6]
		if p[0]&0x80 != 0 {
			att += amDb
		}
		fb := int(p[3] & 7)
		// feedback: the modulator's last two outputs bend its own phase by up to 4 pi (in cycles: 2)
		fbShift := 0.0
		if fb != 0 {
			fbShift = (mod.out + mod.prev) / 2 * math.Pow(2, float64(fb-6))
		}
		s := fmSin[sinIndex(mod.phase+fbShift)]
		if p[3]&0x08 != 0 && s < 0 {
			s = 0
		}
		mod.prev = mod.out
		mod.out = s * fmAmp(att)
		// carrier
		inc = base * fmMult[p[1]&15]
		if p[1]&0x40 != 0 {
			inc *= pm
		}
		car.phase = jsMod1(car.phase + inc)
		att = a.envelope(car, p, 1, ch, fnum, block) + float64(r[0x30+ch]&15)*3 + kslBase*kslMul[p[3]>>6]
		if p[1]&0x80 != 0 {
			att += amDb
		}
		s = fmSin[sinIndex(car.phase+mod.out*2)] // modulation depth: up to 4 pi
		if p[3]&0x10 != 0 && s < 0 {
			s = 0
		}
		car.out = s * fmAmp(att)
		mix += car.out
	}
	return mix
}

func (a *vrc7) clock() {
	a.div++
	if a.div < 36 {
		return
	}
	a.div = 0
	if a.mute {
		a.level = 0
	} else {
		a.level = a.sample() * 0.075
	}
}

func (a *vrc7) output() float64 { return a.level }

func (a *vrc7) state(s *sio) {
	s.Bytes(a.regs[:])
	s.Ints(&a.addr, &a.div)
	s.Bool(&a.mute)
	s.F64(&a.level)
	s.F64(&a.amPhase)
	s.F64(&a.pmPhase)
	for i := range a.ops {
		op := &a.ops[i]
		s.F64(&op.phase)
		s.F64(&op.env)
		s.Int(&op.state)
		s.F64(&op.out)
		s.F64(&op.prev)
	}
}
