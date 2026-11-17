package model3

// The sound board's two Yamaha YMF292 (SCSP) sound chips.
//
// Each chip has 32 voices ("slots") that play 8- or 16-bit samples from the
// chip's own 1 MB of RAM, each with an envelope, a pitch and an amplitude
// LFO, frequency modulation from the other voices' recent output (the sound
// stack), and sends to the direct output and to an effects DSP that runs a
// 128-step program on every sample. The master chip also has three timers,
// a MIDI input and the interrupt controller wired to the sound 68000.
//
// Written for RetroPixels from the chip's documented behaviour (register
// layout, envelope and LFO rate tables, DSP instruction format). The chip RAM
// is kept in the 68000's byte order (big endian).

import "math"

const (
	scspRate = 44100

	// phase: whole samples in the high bits, 18 bits of fraction
	scspFracBits = 18

	// envelope: attenuation 0 (loudest) to 0x3ff (silent), 16 bits of fraction
	scspEGBits = 16
	scspEGMax  = 0x3ff << scspEGBits
)

// envelope states, as the slot monitor (SGC) reports them
const (
	scspAttack = iota
	scspDecay1
	scspDecay2
	scspRelease
)

// rate tables from the chip's documentation: the time in milliseconds the
// envelope takes over its whole range, for effective rates 2 to 63 (0 and 1
// never move)
var scspAttackMS = [62]float64{
	8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500, 1200, 1000, 860, 760, 600,
	500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 63, 55, 47, 38,
	31, 27, 24, 19, 15, 13, 12, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
	2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0, 0,
}

var scspDecayMS = [62]float64{
	118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700, 14800, 12700, 11100, 8900,
	7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100, 920, 790, 690, 550,
	460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49, 43, 34,
	28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1,
}

// LFO frequencies in Hz for LFOF 0-31
var scspLFOHz = [32]float64{
	0.17, 0.19, 0.23, 0.27, 0.34, 0.39, 0.45, 0.55, 0.68, 0.78, 0.92, 1.10, 1.39, 1.60, 1.87, 2.27,
	2.87, 3.31, 3.92, 4.79, 6.15, 7.18, 8.60, 10.8, 14.4, 17.2, 21.5, 28.7, 43.1, 57.4, 86.1, 172.3,
}

// pitch LFO depth in cents and amplitude LFO depth in dB, for PLFOS/ALFOS 0-7
var (
	scspPLFOCents = [8]float64{0, 3.378, 6.755, 13.51, 27.02, 54.04, 108.1, 216.2}
	scspALFODB    = [8]float64{0, 0.4, 0.8, 1.5, 3, 6, 12, 24}
)

type scspLUT struct {
	attackK [64]int64 // attack: share of the remaining attenuation removed per sample (16.16)
	decay   [64]int64 // decay/release: attenuation added per sample (10.16)
	amp     [0x1000]int32
	lfoStep [32]uint32 // LFO phase step per sample (8.24)
	plfo    [8][256]int32
	alfo    [8][256]int32
	level   [8]int32 // send levels 0-7: 0 is off, each step down is -6 dB (1.12)
	pan     [32][2]int32
}

var scspTab = func() *scspLUT {
	t := &scspLUT{}
	for r := 2; r < 64; r++ {
		ar, dr := scspAttackMS[r-2], scspDecayMS[r-2]
		if ar == 0 {
			t.attackK[r] = 1 << 32 // instant
		} else {
			t.attackK[r] = int64(math.Log(1024) / (ar * scspRate / 1000) * 65536)
		}
		t.decay[r] = int64(float64(0x400<<scspEGBits) / (dr * scspRate / 1000))
	}
	// one unit of attenuation is 96 dB / 1024
	for i := range t.amp {
		t.amp[i] = int32(math.Round(4096 * math.Pow(10, -float64(i)*96/1024/20)))
	}
	for i, hz := range scspLFOHz {
		t.lfoStep[i] = uint32(hz * 256 / scspRate * (1 << 24))
	}
	for d := range 8 {
		for p := range 256 {
			// waveforms are picked at run time; these map a signed LFO
			// value (-128..127) to a pitch factor, and an unsigned one
			// (0..255) to added attenuation
			cents := float64(int8(uint8(p))) / 128 * scspPLFOCents[d]
			t.plfo[d][p] = int32(math.Round(4096 * math.Pow(2, cents/1200)))
			t.alfo[d][p] = int32(math.Round(float64(p) / 256 * scspALFODB[d] * 1024 / 96))
		}
	}
	// a voice at full level is a quarter of the 18-bit DAC's range, times
	// four: the loudness RetroPixels has always played these chips at
	for l := 1; l < 8; l++ {
		t.level[l] = 4 * 4096 >> (7 - l)
	}
	for p := range 32 {
		a := p & 15
		f := int32(math.Round(4096 * math.Pow(2, -float64(a)/2)))
		if a == 15 {
			f = 0
		}
		if p < 16 { // 0-15 turn the sound right (the left side gets quieter), 16-31 left
			t.pan[p] = [2]int32{f, 4096}
		} else {
			t.pan[p] = [2]int32{4096, f}
		}
	}
	return t
}()

// scspVoice is a slot's playing state; its settings stay in the registers.
type scspVoice struct {
	on      bool  // keyed on (or releasing)
	ended   bool  // a non-looping sample ran past its end
	looped  bool  // has reached the loop start
	back    bool  // playing backward
	pos     int32 // sample number from the start address
	frac    uint32
	egState int
	egAtt   int64 // 10.16
	lfo     uint32
	noise   uint32
}

type scspChipState struct {
	ram    []byte
	master bool
	slot   [32][16]uint16 // the slot registers, 0x00-0x3ff
	reg    [0x18]uint16   // the common registers, 0x400-0x42f
	voice  [32]scspVoice
	stack  [64]int16 // the sound stack: the last two samples of every slot
	stackW int

	// the effects DSP
	coef  [64]int16
	madrs [32]uint16
	mpro  [128][4]uint16
	temp  [128]int32
	mems  [32]int32
	mixs  [16]int32
	efreg [16]int16
	exts  [2]int16
	dec   uint32
	frc   int32
	adrs  int32
	yreg  int32
	mrd   int32
	last  int // the program's last step that does anything

	// timers (master only)
	timDiv [3]uint32
}

type scsp struct {
	chips  [2]scspChipState
	midi   []uint8 // MIDI bytes from the main board, not yet read
	setIRQ func(level int)
	irq    int
}

func newSCSP(ram1, ram2 []byte, setIRQ func(int)) *scsp {
	s := &scsp{setIRQ: setIRQ}
	s.chips[0].ram, s.chips[1].ram = ram1, ram2
	s.reset()
	return s
}

func (s *scsp) reset() {
	for i := range s.chips {
		ram := s.chips[i].ram
		s.chips[i] = scspChipState{ram: ram, master: i == 0}
		for v := range s.chips[i].voice { // silent, as after a release
			s.chips[i].voice[v] = scspVoice{egState: scspRelease, egAtt: scspEGMax}
		}
	}
	s.midi = s.midi[:0]
	s.irq = 0
}

// ---- slot register fields ----

func (c *scspChipState) f(sl, w int, shift, bits uint) int {
	return int(c.slot[sl][w]>>shift) & (1<<bits - 1)
}

// ---- register access ----

func (s *scsp) read8(chip int, addr uint32) uint8 {
	v := s.read16(chip, addr&^1)
	if addr&1 == 0 {
		return uint8(v >> 8)
	}
	return uint8(v)
}

func (s *scsp) write8(chip int, addr uint32, v uint8) {
	a := addr &^ 1
	old := s.peek16(chip, a)
	if addr&1 == 0 {
		old = old&0xff | uint16(v)<<8
	} else {
		old = old&0xff00 | uint16(v)
	}
	s.write16(chip, a, old)
}

func (s *scsp) write32(chip int, addr uint32, v uint32) {
	s.write16(chip, addr, uint16(v>>16))
	s.write16(chip, addr+2, uint16(v))
}

// peek16 reads a register without the side effects of reading it.
func (s *scsp) peek16(chip int, addr uint32) uint16 {
	c := &s.chips[chip]
	addr &= 0xffe
	switch {
	case addr < 0x400:
		return c.slot[addr>>5][addr>>1&15]
	case addr < 0x430:
		return c.reg[(addr-0x400)>>1]
	case addr >= 0x600 && addr < 0x680:
		return uint16(c.stack[(addr-0x600)>>1])
	case addr >= 0x700 && addr < 0x780:
		return uint16(c.coef[(addr-0x700)>>1])
	case addr >= 0x780 && addr < 0x800:
		return c.madrs[(addr-0x780)>>1&31]
	case addr >= 0x800 && addr < 0xc00:
		i := (addr - 0x800) >> 1
		return c.mpro[i>>2][i&3]
	}
	return 0
}

func (s *scsp) read16(chip int, addr uint32) uint16 {
	c := &s.chips[chip]
	addr &= 0xffe
	switch {
	case addr >= 0x400 && addr < 0x430:
		switch addr - 0x400 {
		case 0x04: // MIDI input: status flags and the next byte
			st := uint16(0x0800) // the output is always empty
			var b uint8
			if !c.master || len(s.midi) == 0 {
				st |= 0x100
			} else {
				b = s.midi[0]
				s.midi = s.midi[1:]
			}
			s.updateIRQ()
			return st | uint16(b)
		case 0x08: // slot monitor
			sl := int(c.reg[4] >> 11)
			v := &c.voice[sl]
			ca := uint16(v.pos>>12) & 15
			eg := uint16(v.egAtt >> (scspEGBits + 5)) // attenuation: 0 is loudest
			return c.reg[4]&0xf800 | ca<<7 | uint16(v.egState)<<5 | eg&0x1f
		case 0x20:
			s.updatePending(c)
		}
	case addr >= 0xc00 && addr < 0xe00:
		t := c.temp[(addr-0xc00)>>2]
		if addr&2 != 0 {
			return uint16(t)
		}
		return uint16(t >> 16 & 0xff)
	case addr >= 0xe00 && addr < 0xe80:
		t := c.mems[(addr-0xe00)>>2]
		if addr&2 != 0 {
			return uint16(t)
		}
		return uint16(t >> 16 & 0xff)
	case addr >= 0xe80 && addr < 0xec0:
		t := c.mixs[(addr-0xe80)>>2]
		if addr&2 != 0 {
			return uint16(t)
		}
		return uint16(t >> 16 & 0xf)
	case addr >= 0xec0 && addr < 0xee0:
		return uint16(c.efreg[(addr-0xec0)>>1])
	case addr >= 0xee0 && addr < 0xee4:
		return uint16(c.exts[(addr-0xee0)>>1])
	}
	return s.peek16(chip, addr)
}

func (s *scsp) write16(chip int, addr uint32, v uint16) {
	c := &s.chips[chip]
	addr &= 0xffe
	switch {
	case addr < 0x400:
		sl, w := int(addr>>5), int(addr>>1&15)
		c.slot[sl][w] = v
		if w == 0 && v&0x1000 != 0 { // KYONEX: every slot follows its KYONB
			c.slot[sl][0] &^= 0x1000
			for i := range c.voice {
				if c.slot[i][0]&0x800 != 0 {
					if !c.voice[i].on || c.voice[i].egState == scspRelease {
						c.keyOn(i)
					}
				} else if c.voice[i].on && c.voice[i].egState != scspRelease {
					c.voice[i].egState = scspRelease
				}
			}
		}
		if w == 9 && v&0x8000 != 0 { // LFORE
			c.voice[sl].lfo = 0
		}
	case addr < 0x430:
		r := (addr - 0x400) >> 1
		switch addr - 0x400 {
		case 0x04, 0x06: // MIDI: input is read only, output goes nowhere
			return
		case 0x08:
			v &= 0xf800
		case 0x18, 0x1a, 0x1c:
			c.timDiv[r-0x0c] = 0
		case 0x20: // only the CPU's own request (bit 5) can be set
			c.reg[r] |= v & 0x20
			s.updateIRQ()
			return
		case 0x22: // SCIRE: clears pending bits
			c.reg[0x10] &^= v
			s.updateIRQ()
			return
		case 0x2c:
			c.reg[r] |= v & 0x20
			return
		case 0x2e: // MCIRE
			c.reg[0x16] &^= v
			return
		}
		c.reg[r] = v
		if r >= 0x0f && r <= 0x14 {
			s.updateIRQ()
		}
	case addr >= 0x600 && addr < 0x680:
		c.stack[(addr-0x600)>>1] = int16(v)
	case addr >= 0x700 && addr < 0x780:
		c.coef[(addr-0x700)>>1] = int16(v)
	case addr >= 0x780 && addr < 0x800:
		c.madrs[(addr-0x780)>>1&31] = v
	case addr >= 0x800 && addr < 0xc00:
		i := (addr - 0x800) >> 1
		c.mpro[i>>2][i&3] = v
		c.findLastStep()
	case addr >= 0xc00 && addr < 0xe00:
		t := &c.temp[(addr-0xc00)>>2]
		if addr&2 != 0 {
			*t = *t&^0xffff | int32(v)
		} else {
			*t = int32(uint32(*t)&0xffff|uint32(v&0xff)<<16) << 8 >> 8
		}
	case addr >= 0xe00 && addr < 0xe80:
		t := &c.mems[(addr-0xe00)>>2]
		if addr&2 != 0 {
			*t = *t&^0xffff | int32(v)
		} else {
			*t = int32(uint32(*t)&0xffff|uint32(v&0xff)<<16) << 8 >> 8
		}
	case addr >= 0xee0 && addr < 0xee4:
		c.exts[(addr-0xee0)>>1] = int16(v)
	}
}

// midiIn is a byte from the main board.
func (s *scsp) midiIn(v uint8) {
	if len(s.midi) < 256 {
		s.midi = append(s.midi, v)
	}
	s.updateIRQ()
}

// ---- interrupts ----

// updatePending sets the pending bits that follow a level: MIDI input waiting.
func (s *scsp) updatePending(c *scspChipState) {
	if !c.master {
		return
	}
	if len(s.midi) > 0 {
		c.reg[0x10] |= 8
	} else {
		c.reg[0x10] &^= 8
	}
}

// updateIRQ works out the 68000's interrupt level from the master chip's
// pending and enabled sources and their levels (SCILV0-2: one bit of the
// level each, per source; sources 8 and up share source 7's level).
func (s *scsp) updateIRQ() {
	m := &s.chips[0]
	s.updatePending(m)
	act := m.reg[0x10] & m.reg[0x0f] & 0x7ff
	level := 0
	for b := 0; b < 11; b++ {
		if act&(1<<b) == 0 {
			continue
		}
		bit := min(b, 7)
		l := 0
		for k := range 3 {
			if m.reg[0x12+k]>>bit&1 != 0 {
				l |= 1 << k
			}
		}
		level = max(level, l)
	}
	if level != s.irq {
		s.irq = level
		s.setIRQ(level)
	}
}

// tickTimers advances the master chip's three timers by one sample. Each
// counts up every 2^TxCTL samples and flags its interrupt as it wraps.
func (s *scsp) tickTimers() {
	m := &s.chips[0]
	m.reg[0x10] |= 0x400 // one sample has passed
	for i := range 3 {
		r := &m.reg[0x0c+i]
		m.timDiv[i]++
		if m.timDiv[i] < 1<<(*r>>8&7) {
			continue
		}
		m.timDiv[i] = 0
		cnt := (*r + 1) & 0xff
		*r = *r&0xff00 | cnt
		if cnt == 0 {
			m.reg[0x10] |= 0x40 << i
		}
	}
	s.updateIRQ()
}

// ---- voices ----

func (c *scspChipState) keyOn(sl int) {
	v := &c.voice[sl]
	*v = scspVoice{on: true, egState: scspAttack, egAtt: scspEGMax, lfo: v.lfo, noise: v.noise | 1}
	if c.f(sl, 9, 15, 1) != 0 {
		v.lfo = 0
	}
}

// rate is a slot's effective envelope rate for a 5-bit rate setting.
func (c *scspChipState) rate(sl, r int) int {
	if r == 0 {
		return 0
	}
	e := 2 * r
	if krs := c.f(sl, 5, 10, 4); krs != 0xf {
		oct := int(int8(uint8(c.f(sl, 8, 11, 4))<<4) >> 4)
		e += oct + 2*krs + c.f(sl, 8, 9, 1)
	}
	return max(0, min(63, e))
}

// envelope advances a slot's envelope by a sample and returns the
// attenuation it applies (0-0x3ff).
func (c *scspChipState) envelope(sl int) int32 {
	v := &c.voice[sl]
	t := scspTab
	switch v.egState {
	case scspAttack:
		k := t.attackK[c.rate(sl, c.f(sl, 4, 0, 5))]
		if k >= 1<<32 {
			v.egAtt = 0
		} else {
			v.egAtt -= ((v.egAtt + 1<<scspEGBits) * k) >> 16
		}
		if v.egAtt <= 0 {
			v.egAtt = 0
			if c.f(sl, 5, 14, 1) == 0 || v.looped {
				v.egState = scspDecay1
			}
		}
		if c.f(sl, 4, 5, 1) != 0 { // EGHOLD: full volume while attacking
			return 0
		}
	case scspDecay1:
		v.egAtt += t.decay[c.rate(sl, c.f(sl, 4, 6, 5))]
		if int(v.egAtt>>(scspEGBits+5)) >= c.f(sl, 5, 5, 5) {
			v.egState = scspDecay2
		}
	case scspDecay2:
		v.egAtt += t.decay[c.rate(sl, c.f(sl, 4, 11, 5))]
	case scspRelease:
		v.egAtt += t.decay[c.rate(sl, c.f(sl, 5, 0, 5))]
		if v.egAtt >= scspEGMax {
			v.on = false
		}
	}
	if v.egAtt > scspEGMax {
		v.egAtt = scspEGMax
	}
	return int32(v.egAtt >> scspEGBits)
}

// lfoValue returns a waveform's value at phase p: signed for pitch
// (-128..127, as a byte), unsigned for amplitude (0..255).
func lfoWave(w int, p uint8, noise uint32, signed bool) uint8 {
	var u uint8 // 0..255
	switch w {
	case 0: // sawtooth
		u = p
	case 1: // square
		if p >= 128 {
			u = 255
		}
	case 2: // triangle
		if p < 128 {
			u = p * 2
		} else {
			u = 255 - (p-128)*2
		}
	case 3:
		u = uint8(noise >> 8)
	}
	if signed {
		return u - 128
	}
	return u
}

// sampleAt reads sample number n of a slot (with SBCTL's bit flips) from RAM.
func (c *scspChipState) sampleAt(sl int, n int32) int32 {
	sa := uint32(c.f(sl, 0, 0, 4))<<16 | uint32(c.slot[sl][1])
	var x int32
	if c.f(sl, 0, 4, 1) != 0 { // 8-bit
		x = int32(int8(c.ram[(sa+uint32(n))&0xfffff])) << 8
	} else {
		a := (sa + uint32(n)*2) & 0xffffe
		x = int32(int16(uint16(c.ram[a])<<8 | uint16(c.ram[a+1])))
	}
	sb := c.f(sl, 0, 9, 2)
	if sb&1 != 0 {
		x ^= 0x7fff
	}
	if sb&2 != 0 {
		x ^= -0x8000
	}
	return int32(int16(x))
}

// advance moves a slot's playback position on by step (in 1/2^18 samples),
// following its loop mode.
func (c *scspChipState) advance(sl int, step uint32) {
	v := &c.voice[sl]
	lsa, lea := int32(c.slot[sl][2]), int32(c.slot[sl][3])
	f := v.frac + step
	n := int32(f >> scspFracBits)
	v.frac = f & (1<<scspFracBits - 1)
	for ; n > 0 && !v.ended; n-- {
		if v.back {
			v.pos--
			if v.pos < lsa {
				if c.f(sl, 0, 5, 2) == 3 { // alternating: forward again
					v.back = false
					v.pos = lsa
				} else {
					v.pos = lea
				}
			}
			continue
		}
		v.pos++
		if !v.looped && v.pos >= lsa {
			v.looped = true
			if c.f(sl, 0, 5, 2) == 2 { // reverse: jump to the end
				v.back = true
				v.pos = lea
				continue
			}
		}
		if v.pos > lea {
			switch c.f(sl, 0, 5, 2) {
			case 0:
				v.ended = true
				v.pos = lea
			case 1:
				v.pos = lsa + (v.pos - lea - 1)
			case 2, 3:
				v.back = true
				v.pos = lea
			}
		}
	}
}

// voiceOut makes a slot's next sample (after its envelope and level), and
// writes it to the sound stack.
func (c *scspChipState) voiceOut(sl int) int32 {
	v := &c.voice[sl]
	t := scspTab
	if !v.on {
		if c.f(sl, 6, 9, 1) == 0 {
			c.stack[c.stackW] = 0
		}
		return 0
	}
	// the LFO
	lfoW := int(c.slot[sl][9])
	v.lfo += t.lfoStep[lfoW>>10&31]
	ph := uint8(v.lfo >> 24)
	v.noise ^= v.noise << 13
	v.noise ^= v.noise >> 17
	v.noise ^= v.noise << 5
	// the pitch: (1024+FNS) << OCT, with the pitch LFO
	oct := int(int8(uint8(c.f(sl, 8, 11, 4))<<4) >> 4)
	step := uint32(0x400+c.f(sl, 8, 0, 10)) << uint(8+oct)
	if d := lfoW >> 5 & 7; d != 0 {
		step = uint32(uint64(step) * uint64(t.plfo[d][lfoWave(lfoW>>8&3, ph, v.noise, true)]) >> 12)
	}
	// the sample, with modulation from the sound stack
	var x int32
	switch c.f(sl, 0, 7, 2) {
	case 0:
		pos := v.pos
		if mdl := c.f(sl, 7, 12, 4); mdl >= 5 {
			mx := int32(c.stack[(c.stackW+c.f(sl, 7, 6, 6))&63])
			my := int32(c.stack[(c.stackW+c.f(sl, 7, 0, 6))&63])
			pos += ((mx + my) / 2) << 10 >> (26 - mdl)
		}
		if !v.ended {
			a := c.sampleAt(sl, pos)
			nx := pos + 1
			if v.back {
				nx = pos - 1
			}
			b := c.sampleAt(sl, nx)
			fr := int32(v.frac >> (scspFracBits - 12))
			if v.back {
				fr = 4096 - fr
			}
			x = a + (b-a)*fr>>12
		}
	case 1:
		x = int32(int16(v.noise))
	}
	c.advance(sl, step)
	// level: envelope, TL and the amplitude LFO, unless the slot goes direct
	att := c.envelope(sl)
	if c.f(sl, 6, 8, 1) == 0 {
		att += int32(c.f(sl, 6, 0, 8)) << 2
		if d := lfoW & 7; d != 0 {
			att += t.alfo[d][lfoWave(lfoW>>3&3, ph, v.noise, false)]
		}
		x = x * t.amp[min(att, 0xfff)] >> 12
	}
	if c.f(sl, 6, 9, 1) == 0 {
		c.stack[c.stackW] = int16(x)
	}
	return x
}

// run makes one sample of a chip's output (18-bit scale).
func (c *scspChipState) run() (l, r int32) {
	t := scspTab
	for i := range c.mixs {
		c.mixs[i] = 0
	}
	for sl := 0; sl < 32; sl++ {
		x := c.voiceOut(sl)
		c.stackW = (c.stackW + 1) & 63
		if x == 0 {
			continue
		}
		w := c.slot[sl][10]
		if imxl := w & 7; imxl != 0 {
			c.mixs[w>>3&15] += (x << 4) >> (7 - imxl)
		}
		w = c.slot[sl][11]
		if lv := t.level[w>>13]; lv != 0 {
			p := t.pan[w>>8&31]
			l += x * lv >> 12 * p[0] >> 12
			r += x * lv >> 12 * p[1] >> 12
		}
	}
	c.dspRun()
	for i := range 18 {
		var x int32
		if i < 16 {
			x = int32(c.efreg[i])
		} else {
			x = int32(c.exts[i-16])
		}
		w := c.slot[i][11]
		if lv := t.level[w>>5&7]; lv != 0 && x != 0 {
			p := t.pan[w&31]
			l += x * lv >> 12 * p[0] >> 12
			r += x * lv >> 12 * p[1] >> 12
		}
	}
	return
}

func clampOut(v int32, bits uint) int32 {
	hi := int32(1)<<(bits-1) - 1
	return max(-hi-1, min(hi, v))
}

// chipOut scales a chip's mix for its DAC: 18 bits when DAC18B is set,
// otherwise the top 16.
func (c *scspChipState) chipOut(l, r int32) (int32, int32) {
	if c.reg[0]&0x100 != 0 {
		return clampOut(l, 18), clampOut(r, 18)
	}
	return clampOut(l>>2, 16), clampOut(r>>2, 16)
}

// sample makes one 44.1 kHz output sample of both chips: the master's pair
// for the front speakers, the slave's for the rear.
func (s *scsp) sample() (fl, fr, rl, rr int32) {
	fl, fr = s.chips[0].chipOut(s.chips[0].run())
	rl, rr = s.chips[1].chipOut(s.chips[1].run())
	s.tickTimers()
	return
}

// ---- the effects DSP ----

func (c *scspChipState) findLastStep() {
	c.last = -1
	for i := 127; i >= 0; i-- {
		if c.mpro[i] != [4]uint16{} {
			c.last = i
			return
		}
	}
}

func sat24(v int32) int32 { return max(-0x800000, min(0x7fffff, v)) }
func sx24(v int32) int32  { return v << 8 >> 8 }

// dspPack turns a 24-bit value into the 16-bit float the DSP stores in RAM:
// sign, a 4-bit count of leading sign bits removed (up to 11), 11 bits of
// mantissa.
func dspPack(v int32) uint16 {
	sign := uint32(v>>23) & 1
	t := uint32(v^v<<1) & 0xffffff
	e := 0
	for e < 12 && t&0x800000 == 0 {
		t <<= 1
		e++
	}
	var m uint32
	if e < 12 {
		m = uint32(v<<e) & 0x3fffff >> 11
	} else {
		m = uint32(v<<11) >> 11
	}
	return uint16(sign<<15 | uint32(e)<<11 | m&0x7ff)
}

func dspUnpack(v uint16) int32 {
	sign := int32(v>>15) & 1
	e := int32(v>>11) & 15
	x := int32(v&0x7ff) << 11
	if e > 11 {
		e = 11
		x |= sign << 22
	} else {
		x |= (sign ^ 1) << 22
	}
	x |= sign << 23
	return sx24(x) >> e
}

// dspRun runs the effects program once (one sample).
func (c *scspChipState) dspRun() {
	for i := range c.efreg {
		c.efreg[i] = 0
	}
	rbl := uint32(0x2000) << (c.reg[1] >> 7 & 3)
	rbp := uint32(c.reg[1]&0x7f) << 12
	var acc int32
	for st := 0; st <= c.last; st++ {
		w := c.mpro[st]
		ins := uint64(w[0])<<48 | uint64(w[1])<<32 | uint64(w[2])<<16 | uint64(w[3])
		bf := func(lo, n uint) uint32 { return uint32(ins>>lo) & (1<<n - 1) }
		tra, twt, twa := bf(56, 7), bf(55, 1), bf(48, 7)
		xsel, ysel, ira := bf(47, 1), bf(45, 2), bf(38, 6)
		iwt, iwa := bf(37, 1), bf(32, 5)
		table, mwt, mrd, ewt, ewa := bf(31, 1), bf(30, 1), bf(29, 1), bf(28, 1), bf(24, 4)
		adrl, frcl, shift, yrl := bf(23, 1), bf(22, 1), bf(20, 2), bf(19, 1)
		negb, zero, bsel, nofl := bf(18, 1), bf(17, 1), bf(16, 1), bf(15, 1)
		coef, masa, adreb, nxadr := bf(9, 6), bf(2, 5), bf(1, 1), bf(0, 1)

		var in int32
		switch {
		case ira < 0x20:
			in = c.mems[ira]
		case ira < 0x30:
			in = c.mixs[ira-0x20] << 4
		case ira < 0x32:
			in = int32(c.exts[ira-0x30]) << 8
		}
		in = sx24(in)
		if iwt != 0 {
			c.mems[iwa] = c.mrd
			if ira == iwa {
				in = c.mrd
			}
		}
		var b int32
		if zero == 0 {
			if bsel != 0 {
				b = acc
			} else {
				b = sx24(c.temp[(tra+c.dec)&0x7f])
			}
			if negb != 0 {
				b = -b
			}
		}
		x := in
		if xsel == 0 {
			x = sx24(c.temp[(tra+c.dec)&0x7f])
		}
		var y int32
		switch ysel {
		case 0:
			y = c.frc
		case 1:
			y = int32(c.coef[coef]) >> 3
		case 2:
			y = c.yreg >> 11 & 0x1fff
		case 3:
			y = c.yreg >> 4 & 0xfff
		}
		y = y << 19 >> 19
		if yrl != 0 {
			c.yreg = in
		}
		var sh int32
		switch shift {
		case 0:
			sh = sat24(acc)
		case 1:
			sh = sat24(acc * 2)
		case 2:
			sh = sx24(acc * 2)
		case 3:
			sh = sx24(acc)
		}
		acc = int32((int64(x)*int64(y))>>12) + b
		acc = sx24(acc)
		if twt != 0 {
			c.temp[(twa+c.dec)&0x7f] = sh
		}
		if frcl != 0 {
			if shift == 3 {
				c.frc = sh & 0xfff
			} else {
				c.frc = sh >> 11 & 0x1fff
			}
		}
		if (mrd != 0 || mwt != 0) && st&1 != 0 {
			a := uint32(c.madrs[masa])
			if table == 0 {
				a += c.dec
			}
			if adreb != 0 {
				a += uint32(c.adrs) & 0xfff
			}
			if nxadr != 0 {
				a++
			}
			if table == 0 {
				a &= rbl - 1
			} else {
				a &= 0xffff
			}
			a = (a + rbp) & 0x7ffff
			ra := a * 2
			if mrd != 0 {
				m := uint16(c.ram[ra])<<8 | uint16(c.ram[ra+1])
				if nofl != 0 {
					c.mrd = int32(int16(m)) << 8
				} else {
					c.mrd = dspUnpack(m)
				}
			}
			if mwt != 0 {
				var m uint16
				if nofl != 0 {
					m = uint16(sh >> 8)
				} else {
					m = dspPack(sh)
				}
				c.ram[ra], c.ram[ra+1] = uint8(m>>8), uint8(m)
			}
		}
		if adrl != 0 {
			if shift == 3 {
				c.adrs = sh >> 12 & 0xfff
			} else {
				c.adrs = in >> 16
			}
		}
		if ewt != 0 {
			c.efreg[ewa] += int16(sh >> 8)
		}
	}
	c.dec--
}
