package model3

// The sound board's two Yamaha YMF292 "SCSP" sound chips: 32 voices each
// playing samples from the chip's RAM, with envelopes, LFOs, FM through a
// ring buffer, and an effects DSP. The sound board's 68000 drives them
// through their registers, and the master chip's timers and MIDI port
// interrupt it.
//
// This follows the SCSP emulation used by Supermodel (written by ElSemi,
// with fixes from MAME by R. Belmont and others), including its quirks, so
// the games sound the way they do there. The chip RAM is kept in the 68000's
// byte order (big endian).

import (
	"math"
	"sync"
)

const (
	scspShift    = 12
	scspEGShift  = 16
	scspLFOShift = 8
)

const (
	egAttack = iota
	egDecay1
	egDecay2
	egRelease
)

var scspTables struct {
	once      sync.Once
	arTable   [64]int32
	drTable   [64]int32
	egTable   [0x400]int32
	lpan      [0x10000]int32
	rpan      [0x10000]int32
	plfo      [4][256]int32 // saw, square, triangle, noise
	alfo      [4][256]int32
	lfoScales [16][256]int32 // 0-7 pitch scales, 8-15 amplitude scales
}

func scspInitTables() {
	t := &scspTables
	t.once.Do(func() {
		for i := 0; i < 0x400; i++ {
			envDB := float32(3*(i-0x3ff)) / 32.0
			scale := float32(1 << scspShift)
			t.egTable[i] = int32(math.Pow(10.0, float64(envDB)/20.0) * float64(scale))
		}
		sdlt := [8]float32{-1000000.0, -36.0, -30.0, -24.0, -18.0, -12.0, -6.0, 0.0}
		for i := 0; i < 0x10000; i++ {
			iTL := i & 0xff
			iPAN := (i >> 8) & 0x1f
			iSDL := (i >> 13) & 7
			var db float32
			for b, v := range []float32{0.4, 0.8, 1.5, 3.0, 6.0, 12.0, 24.0, 48.0} {
				if iTL&(1<<b) != 0 {
					db -= v
				}
			}
			tl := float32(math.Pow(10.0, float64(db/20.0)))
			db = 0
			for b, v := range []float32{3.0, 6.0, 12.0, 24.0} {
				if iPAN&(1<<b) != 0 {
					db -= v
				}
			}
			var pan float32
			if iPAN == 0xf || iPAN == 0x1f {
				pan = 0
			} else {
				pan = float32(math.Pow(10.0, float64(db/20.0)))
			}
			lp, rp := float32(1), float32(1)
			if iPAN < 0x10 {
				lp = pan
			} else {
				rp = pan
			}
			var sdl float32
			if iSDL != 0 {
				sdl = float32(math.Pow(10.0, float64(sdlt[iSDL]/20.0)))
			}
			t.lpan[i] = int32(uint32(float64(1<<scspShift) * (4.0 * float64(lp*tl*sdl))))
			t.rpan[i] = int32(uint32(float64(1<<scspShift) * (4.0 * float64(rp*tl*sdl))))
		}
		arTimes := [64]float64{100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
			1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
			76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
			2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0}
		drTimes := [64]float64{100000, 100000, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0, 17700.0,
			14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0,
			920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0,
			28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1}
		for i := 2; i < 64; i++ {
			scale := float64(1 << scspEGShift)
			if tm := arTimes[i]; tm != 0 {
				t.arTable[i] = int32((1023 * 1000.0) / (44100 * tm) * scale)
			} else {
				t.arTable[i] = 1024 << scspEGShift
			}
			t.drTable[i] = int32((1023 * 1000.0) / (44100 * drTimes[i]) * scale)
		}

		// LFO waveforms
		seed := uint32(12345)
		for i := 0; i < 256; i++ {
			var a, p int32
			a = int32(255 - i)
			if i < 128 {
				p = int32(i)
			} else {
				p = int32(i - 256)
			}
			t.alfo[0][i], t.plfo[0][i] = a, p
			if i < 128 {
				a, p = 255, 127
			} else {
				a, p = 0, -128
			}
			t.alfo[1][i], t.plfo[1][i] = a, p
			if i < 128 {
				a = int32(255 - i*2)
			} else {
				a = int32(i*2 - 256)
			}
			switch {
			case i < 64:
				p = int32(i * 2)
			case i < 128:
				p = int32(255 - i*2)
			case i < 192:
				p = int32(256 - i*2)
			default:
				p = int32(i*2 - 511)
			}
			t.alfo[2][i], t.plfo[2][i] = a, p
			seed = seed*1103515245 + 12345
			a = int32(seed>>16) & 0xff
			t.alfo[3][i], t.plfo[3][i] = a, 128-a
		}
		pscale := [8]float32{0.0, 7.0, 13.5, 27.0, 55.0, 112.0, 230.0, 494.0}
		ascale := [8]float32{0.0, 0.4, 0.8, 1.5, 3.0, 6.0, 12.0, 24.0}
		for s := 0; s < 8; s++ {
			limit := pscale[s]
			for i := -128; i < 128; i++ {
				v := math.Exp2((float64(limit) * float64(i) / 128.0) / 1200.0)
				t.lfoScales[s][i+128] = int32(uint32(float64(1<<scspLFOShift) * v))
			}
			limit = -ascale[s]
			for i := 0; i < 256; i++ {
				v := math.Pow(10.0, (float64(limit)*float64(i)/256.0)/20.0)
				t.lfoScales[8+s][i] = int32(uint32(float64(1<<scspLFOShift) * v))
			}
		}
	})
}

type scspLFO struct {
	phase uint16
	step  uint32
	table *[256]int32
	scale *[256]int32
}

func (l *scspLFO) compute(lfof, lfows, lfos uint16, amp bool) {
	lfoFreq := [32]float32{0.17, 0.19, 0.23, 0.27, 0.34, 0.39, 0.45, 0.55, 0.68, 0.78, 0.92, 1.10, 1.39, 1.60, 1.87, 2.27,
		2.87, 3.31, 3.92, 4.79, 6.15, 7.18, 8.60, 10.8, 14.4, 17.2, 21.5, 28.7, 43.1, 57.4, 86.1, 172.3}
	step := lfoFreq[lfof] * 256.0 / 44100.0
	l.step = uint32(float32(1<<scspLFOShift) * step)
	t := &scspTables
	if amp {
		l.table = &t.alfo[lfows&3]
		l.scale = &t.lfoScales[8+lfos&7]
	} else {
		l.table = &t.plfo[lfows&3]
		l.scale = &t.lfoScales[lfos&15]
	}
}

func (l *scspLFO) plfoStep() int32 {
	l.phase += uint16(l.step)
	if l.table == nil {
		return 1 << scspShift
	}
	p := l.table[l.phase>>scspLFOShift]
	return l.scale[(p+128)&255] << (scspShift - scspLFOShift)
}

func (l *scspLFO) alfoStep() int32 {
	l.phase += uint16(l.step)
	if l.table == nil {
		return 1 << scspShift
	}
	p := l.table[l.phase>>scspLFOShift]
	return l.scale[p&255] << (scspShift - scspLFOShift)
}

type scspEG struct {
	volume           int32
	state            int
	ar, d1r, d2r, rr int32
	dl               int32
	eghold           bool
}

type scspSlot struct {
	data    [16]uint16
	active  bool
	base    uint32 // where the sample starts in the chip's RAM
	curAddr uint32
	nxtAddr uint32
	step    uint32
	back    bool
	eg      scspEG
	plfo    scspLFO
	alfo    scspLFO
}

func (s *scspSlot) keyonex() bool  { return s.data[0]&0x1000 != 0 }
func (s *scspSlot) keyonb() bool   { return s.data[0]&0x0800 != 0 }
func (s *scspSlot) sbctl() uint16  { return s.data[0] >> 9 & 3 }
func (s *scspSlot) ssctl() uint16  { return s.data[0] >> 7 & 3 }
func (s *scspSlot) lpctl() uint16  { return s.data[0] >> 5 & 3 }
func (s *scspSlot) pcm8b() bool    { return s.data[0]&0x10 != 0 }
func (s *scspSlot) sa() uint32     { return uint32(s.data[0]&0xF)<<16 | uint32(s.data[1]) }
func (s *scspSlot) lsa() uint32    { return uint32(s.data[2]) }
func (s *scspSlot) lea() uint32    { return uint32(s.data[3]) }
func (s *scspSlot) d2r() int       { return int(s.data[4] >> 11 & 0x1F) }
func (s *scspSlot) d1r() int       { return int(s.data[4] >> 6 & 0x1F) }
func (s *scspSlot) egHold() bool   { return s.data[4]&0x20 != 0 }
func (s *scspSlot) ar() int        { return int(s.data[4] & 0x1F) }
func (s *scspSlot) lpslnk() bool   { return s.data[5]&0x4000 != 0 }
func (s *scspSlot) krs() int       { return int(s.data[5] >> 10 & 0xF) }
func (s *scspSlot) dl() int        { return int(s.data[5] >> 5 & 0x1F) }
func (s *scspSlot) rr() int        { return int(s.data[5] & 0x1F) }
func (s *scspSlot) stwinh() bool   { return s.data[6]&0x200 != 0 }
func (s *scspSlot) sdir() bool     { return s.data[6]&0x100 != 0 }
func (s *scspSlot) tl() uint16     { return s.data[6] & 0xFF }
func (s *scspSlot) mdl() uint16    { return s.data[7] >> 12 & 0x1E }
func (s *scspSlot) mdxsl() uint16  { return s.data[7] >> 6 & 0x3F }
func (s *scspSlot) mdysl() uint16  { return s.data[7] & 0x3F }
func (s *scspSlot) oct() int       { return int(s.data[8] >> 11 & 0xF) }
func (s *scspSlot) fns() uint32    { return uint32(s.data[8] & 0x3FF) }
func (s *scspSlot) lfof() uint16   { return s.data[9] >> 10 & 0x1F }
func (s *scspSlot) plfows() uint16 { return s.data[9] >> 8 & 3 }
func (s *scspSlot) plfos() uint16  { return s.data[9] >> 5 & 0xE }
func (s *scspSlot) alfows() uint16 { return s.data[9] >> 3 & 3 }
func (s *scspSlot) alfos() uint16  { return s.data[9] & 7 }
func (s *scspSlot) isel() int      { return int(s.data[10] >> 3 & 0xF) }
func (s *scspSlot) imxl() uint16   { return s.data[10] & 7 }
func (s *scspSlot) disdl() uint16  { return s.data[11] >> 13 & 7 }
func (s *scspSlot) dipan() uint16  { return s.data[11] >> 8 & 0x1F }
func (s *scspSlot) efsdl() uint16  { return s.data[11] >> 5 & 7 }
func (s *scspSlot) efpan() uint16  { return s.data[11] & 0x1F }

func getRate(table *[64]int32, base, r int) int32 {
	rate := base + r<<1
	if rate > 63 {
		rate = 63
	}
	if rate < 0 {
		rate = 0
	}
	return table[rate]
}

func (s *scspSlot) computeEG() {
	octave := (s.oct() ^ 8) - 8
	rate := 0
	if s.krs() != 0xf {
		rate = octave + 2*s.krs() + int(s.fns()>>9&1)
	}
	t := &scspTables
	s.eg.volume = 0x17F << scspEGShift
	s.eg.ar = getRate(&t.arTable, rate, s.ar())
	s.eg.d1r = getRate(&t.drTable, rate, s.d1r())
	s.eg.d2r = getRate(&t.drTable, rate, s.d2r())
	s.eg.rr = getRate(&t.drTable, rate, s.rr())
	s.eg.dl = int32(0x1f - s.dl())
	s.eg.eghold = s.egHold()
}

func (s *scspSlot) egUpdate() int32 {
	switch s.eg.state {
	case egAttack:
		s.eg.volume += s.eg.ar
		if s.eg.volume >= 0x3ff<<scspEGShift {
			if !s.lpslnk() {
				s.eg.state = egDecay1
				if s.eg.d1r >= 1024<<scspEGShift {
					s.eg.state = egDecay2
				}
			}
			s.eg.volume = 0x3ff << scspEGShift
		}
		if s.eg.eghold {
			return 0x3ff << (scspShift - 10)
		}
	case egDecay1:
		s.eg.volume -= s.eg.d1r
		if s.eg.volume <= 0 {
			s.eg.volume = 0
		}
		if s.eg.volume>>(scspEGShift+5) <= s.eg.dl {
			s.eg.state = egDecay2
		}
	case egDecay2:
		if s.d2r() == 0 {
			return (s.eg.volume >> scspEGShift) << (scspShift - 10)
		}
		s.eg.volume -= s.eg.d2r
		if s.eg.volume <= 0 {
			s.eg.volume = 0
		}
	case egRelease:
		s.eg.volume -= s.eg.rr
		if s.eg.volume <= 0 {
			s.eg.volume = 0
			s.stop(false)
		}
	}
	return (s.eg.volume >> scspEGShift) << (scspShift - 10)
}

func (s *scspSlot) pitchStep() uint32 {
	octave := (s.oct() ^ 8) - 8 + scspShift - 10
	fn := s.fns() + 1<<10
	if octave >= 0 {
		return fn << uint(octave)
	}
	return fn >> uint(-octave)
}

func (s *scspSlot) computeLFO() {
	if s.plfos() != 0 {
		s.plfo.compute(s.lfof(), s.plfows(), s.plfos(), false)
	}
	if s.alfos() != 0 {
		s.alfo.compute(s.lfof(), s.alfows(), s.alfos(), true)
	}
}

func (s *scspSlot) start() {
	s.active = true
	s.back = false
	s.nxtAddr = 1 << scspShift
	s.curAddr = 0
	if s.pcm8b() {
		s.base = s.sa()
	} else {
		s.base = s.sa() & 0x7FFFE
	}
	s.step = s.pitchStep()
	s.computeEG()
	s.eg.state = egAttack
	s.eg.volume = 0x17F << scspEGShift
	s.computeLFO()
}

func (s *scspSlot) stop(keyoff bool) {
	if keyoff {
		s.eg.state = egRelease
	} else {
		s.active = false
	}
	s.data[0] &^= 0x800
}

// ---------------------------------------------------------------- DSP

type scspDSP struct {
	ram      []byte // the chip's RAM, read and written as 16-bit words
	rbp, rbl uint32
	coef     [64]int16
	madrs    [32]uint16
	mpro     [128 * 4]uint16
	temp     [128]int32
	mems     [32]int32
	dec      uint32
	mixs     [16]int32
	exts     [2]int16
	efreg    [16]int16
	stopped  bool
	lastStep int
}

func (d *scspDSP) reset() {
	*d = scspDSP{ram: d.ram, rbl: 8 * 1024, stopped: true}
}

func dspPack(val int32) uint16 {
	sign := val >> 23 & 1
	temp := uint32(val^val<<1) & 0xFFFFFF
	exponent := int32(0)
	for k := 0; k < 12; k++ {
		if temp&0x800000 != 0 {
			break
		}
		temp <<= 1
		exponent++
	}
	if exponent < 12 {
		val = (val << uint(exponent)) & 0x3FFFFF
	} else {
		val <<= 11
	}
	val >>= 11
	val &= 0x7FF
	val |= sign << 15
	val |= exponent << 11
	return uint16(val)
}

func dspUnpack(val uint16) int32 {
	sign := int32(val >> 15 & 1)
	exponent := int32(val >> 11 & 0xF)
	mantissa := int32(val & 0x7FF)
	uval := mantissa << 11
	if exponent > 11 {
		exponent = 11
		uval |= sign << 22
	} else {
		uval |= (sign ^ 1) << 22
	}
	uval |= sign << 23
	uval <<= 8
	uval >>= 8
	uval >>= uint(exponent)
	return uval
}

func (d *scspDSP) start() {
	d.stopped = false
	i := 127
	for ; i >= 0; i-- {
		p := d.mpro[i*4 : i*4+4]
		if p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0 {
			break
		}
	}
	d.lastStep = i + 1
}

func sx24(v int32) int32 { return v << 8 >> 8 }

func (d *scspDSP) step() {
	if d.stopped {
		return
	}
	var acc, shifted, x, y, b, inputs, memval, frcReg, yReg int32
	var addr, adrsReg uint32
	d.efreg = [16]int16{}
	for step := 0; step < d.lastStep; step++ {
		ip := d.mpro[step*4 : step*4+4]
		tra := uint32(ip[0] >> 8 & 0x7F)
		twt := ip[0]>>7&1 != 0
		twa := uint32(ip[0] & 0x7F)

		xsel := ip[1]>>15&1 != 0
		ysel := ip[1] >> 13 & 3
		ira := uint32(ip[1] >> 6 & 0x3F)
		iwt := ip[1]>>5&1 != 0
		iwa := uint32(ip[1] & 0x1F)

		table := ip[2]>>15&1 != 0
		mwt := ip[2]>>14&1 != 0
		mrd := ip[2]>>13&1 != 0
		ewt := ip[2]>>12&1 != 0
		ewa := ip[2] >> 8 & 0xF
		adrl := ip[2]>>7&1 != 0
		frcl := ip[2]>>6&1 != 0
		shift := ip[2] >> 4 & 3
		yrl := ip[2]>>3&1 != 0
		negb := ip[2]>>2&1 != 0
		zero := ip[2]>>1&1 != 0
		bsel := ip[2]&1 != 0

		nofl := ip[3]>>15&1 != 0
		coef := ip[3] >> 9 & 0x3f
		masa := ip[3] >> 2 & 0x1f
		adreb := ip[3]>>1&1 != 0
		nxadr := ip[3]&1 != 0

		switch {
		case ira <= 0x1f:
			inputs = d.mems[ira]
		case ira <= 0x2F:
			inputs = d.mixs[ira-0x20] << 4
		case ira <= 0x31:
			inputs = int32(d.exts[ira-0x30]) << 8
		default:
			return
		}
		inputs = sx24(inputs)

		if iwt {
			d.mems[iwa] = memval
			if ira == iwa {
				inputs = memval
			}
		}

		if !zero {
			if bsel {
				b = acc
			} else {
				b = sx24(d.temp[(tra+d.dec)&0x7F])
			}
			if negb {
				b = -b
			}
		} else {
			b = 0
		}

		if xsel {
			x = inputs
		} else {
			x = sx24(d.temp[(tra+d.dec)&0x7F])
		}

		switch ysel {
		case 0:
			y = frcReg
		case 1:
			y = int32(d.coef[coef]) >> 3
		case 2:
			y = (yReg >> 11) & 0x1FFF
		case 3:
			y = (yReg >> 4) & 0x0FFF
		}

		if yrl {
			yReg = inputs
		}

		switch shift {
		case 0, 1:
			shifted = acc
			if shift == 1 {
				shifted = acc * 2
			}
			if shifted > 0x007FFFFF {
				shifted = 0x007FFFFF
			}
			if shifted < -0x00800000 {
				shifted = -0x00800000
			}
		case 2:
			shifted = sx24(acc * 2)
		case 3:
			shifted = sx24(acc)
		}

		y = y << 19 >> 19
		acc = int32((int64(x)*int64(y))>>12) + b

		if twt {
			d.temp[(twa+d.dec)&0x7F] = shifted
		}

		if frcl {
			if shift == 3 {
				frcReg = shifted & 0x0FFF
			} else {
				frcReg = (shifted >> 11) & 0x1FFF
			}
		}

		if mrd || mwt {
			addr = uint32(d.madrs[masa])
			if !table {
				addr += d.dec
			}
			if adreb {
				addr += adrsReg & 0x0FFF
			}
			if nxadr {
				addr++
			}
			if !table {
				addr &= d.rbl - 1
			} else {
				addr &= 0xFFFF
			}
			addr += d.rbp << 12
			if addr > 0x7ffff {
				addr = 0
			}
			if mrd && step&1 != 0 {
				w := uint16(d.ram[addr*2])<<8 | uint16(d.ram[addr*2+1])
				if nofl {
					memval = int32(w) << 8
				} else {
					memval = dspUnpack(w)
				}
			}
			if mwt && step&1 != 0 {
				var w uint16
				if nofl {
					w = uint16(shifted >> 8)
				} else {
					w = dspPack(shifted)
				}
				d.ram[addr*2] = uint8(w >> 8)
				d.ram[addr*2+1] = uint8(w)
			}
		}

		if adrl {
			if shift == 3 {
				adrsReg = uint32(shifted>>12) & 0xFFF
			} else {
				adrsReg = uint32(inputs >> 16)
			}
		}

		if ewt {
			d.efreg[ewa] += int16(shifted >> 8)
		}
	}
	d.dec--
	d.mixs = [16]int32{}
}

// ---------------------------------------------------------------- chips

type scspChip struct {
	data    [0x18]uint16
	slots   [32]scspSlot
	ringbuf [64]int16
	bufptr  uint8
	ram     []byte // 1 MB
	master  bool
	dsp     scspDSP
}

// scsp is the pair of chips with what they share: the master's timers and
// interrupts, and the MIDI port the main board writes to.
type scsp struct {
	chips [2]scspChip
	cur   *scspChip

	irqTimA, irqTimBC, irqMidi int
	mciEB, mciPD               uint16

	midiStack          [256]uint8
	midiW, midiR       uint8
	midiOutW, midiOutR uint8
	midiOutFill        uint8
	midiInFill         uint8

	timCnt [3]int32

	setIRQ func(level int)
}

func newSCSP(ram1, ram2 []byte, setIRQ func(int)) *scsp {
	scspInitTables()
	s := &scsp{setIRQ: setIRQ}
	s.chips[0].ram, s.chips[1].ram = ram1, ram2
	s.reset()
	return s
}

func (s *scsp) reset() {
	for i := range s.chips {
		c := &s.chips[i]
		ram := c.ram
		*c = scspChip{ram: ram, master: i == 0}
		c.dsp.ram = ram
		c.dsp.reset()
	}
	s.cur = &s.chips[0]
	s.irqTimA, s.irqTimBC, s.irqMidi = 0, 0, 0
	s.mciEB, s.mciPD = 0, 0
	s.midiStack = [256]uint8{}
	s.midiW, s.midiR, s.midiOutW, s.midiOutR = 0, 0, 0, 0
	s.midiOutFill, s.midiInFill = 0, 0
	s.timCnt = [3]int32{0xffff, 0xffff, 0xffff}
}

func (s *scsp) decodeSCI(irq uint) int {
	d := &s.chips[0].data
	v := 0
	if d[0x24/2]&(1<<irq) != 0 {
		v |= 1
	}
	if d[0x26/2]&(1<<irq) != 0 {
		v |= 2
	}
	if d[0x28/2]&(1<<irq) != 0 {
		v |= 4
	}
	return v
}

func (s *scsp) checkPendingIRQ() {
	m := &s.chips[0]
	pend := m.data[0x20/2]
	en := m.data[0x1e/2]
	if s.midiW != s.midiR {
		s.cur.data[0x20/2] |= 8
		pend |= 8
	}
	level := 0
	if pend&en&0x40 != 0 {
		level = s.irqTimA
	}
	if pend&en&0x80 != 0 && s.irqTimBC > level {
		level = s.irqTimBC
	}
	if pend&en&0x100 != 0 && s.irqTimBC > level {
		level = s.irqTimBC
	}
	if pend&en&8 != 0 {
		if s.irqMidi > level {
			level = s.irqMidi
		}
		s.cur.data[0x20/2] &^= 8
	}
	s.setIRQ(level)
}

func (s *scsp) updateSlotReg(sl int, r int) {
	c := s.cur
	slot := &c.slots[sl]
	switch r & 0x3f {
	case 0, 1:
		if slot.keyonex() {
			for i := range c.slots {
				s2 := &c.slots[i]
				if s2.keyonb() && s2.eg.state == egRelease {
					s2.start()
				}
				if !s2.keyonb() {
					s2.stop(true)
				}
			}
			slot.data[0] &^= 0x1000
		}
	case 0x10, 0x11:
		slot.step = slot.pitchStep()
	case 0xA, 0xB:
		slot.eg.rr = getRate(&scspTables.drTable, 0, slot.rr())
		slot.eg.dl = int32(0x1f - slot.dl())
	case 0x12, 0x13:
		slot.computeLFO()
	}
}

func (s *scsp) updateReg(reg int) {
	c := s.cur
	m := &s.chips[0]
	switch reg & 0x3f {
	case 0, 2, 3:
		c.dsp.rbl = (8 * 1024) << (c.data[1] >> 7 & 3)
		c.dsp.rbp = uint32(c.data[1] & 0x7F)
	case 6, 7:
		s.midiOutWrite(uint8(c.data[0x6/2]))
	case 8, 9:
		c.data[0x8/2] &= 0xf800
	case 0x18, 0x19:
		if c.master {
			s.timCnt[0] = int32(m.data[0x18/2]&0xfe) << 8
		}
	case 0x1a, 0x1b:
		if c.master {
			s.timCnt[1] = int32(m.data[0x1A/2]&0xfe) << 8
		}
	case 0x1c, 0x1d:
		if c.master {
			s.timCnt[2] = int32(m.data[0x1C/2]&0xfe) << 8
		}
	case 0x22, 0x23:
		if c.master {
			c.data[0x20/2] &^= c.data[0x22/2]
			if s.timCnt[0] == 0xffff {
				c.data[0x20/2] |= 0x40
			}
			if s.timCnt[1] == 0xffff {
				c.data[0x20/2] |= 0x80
			}
			if s.timCnt[2] == 0xffff {
				c.data[0x20/2] |= 0x100
			}
			s.checkPendingIRQ()
		}
	case 0x24, 0x25, 0x26, 0x27, 0x28, 0x29:
		if c.master {
			s.irqTimA = s.decodeSCI(6)
			s.irqTimBC = s.decodeSCI(7)
			s.irqMidi = s.decodeSCI(3)
		}
	case 0x2b:
		s.mciEB = c.data[0x2a/2]
	case 0x2e, 0x2f:
		s.mciPD &^= c.data[0x2e/2]
	}
}

func (s *scsp) updateRegR(reg int) {
	c := s.cur
	switch reg & 0x3f {
	case 4, 5:
		v := c.data[0x4/2] & 0xff00
		v |= uint16(s.midiStack[s.midiR])
		if s.midiR != s.midiW {
			s.midiR++
		}
		s.midiInFill--
		c.data[0x4/2] = v
		s.checkPendingIRQ()
	case 8, 9:
		mslc := c.data[0x8/2] >> 11 & 0x1f
		slot := &c.slots[mslc]
		sgc := uint16(slot.eg.state & 3)
		ca := uint16(slot.curAddr>>(scspShift+12)) & 0xf
		eg := uint16(0x1f-(slot.eg.volume>>(scspEGShift+5))) & 0x1f
		c.data[0x8/2] = ca<<7 | sgc<<5 | eg
	case 0x2a, 0x2b:
		c.data[0x2a/2] = s.mciEB
	case 0x2c, 0x2d:
		c.data[0x2c/2] = s.mciPD
	}
}

func setByte16(w *uint16, hi bool, v uint8) {
	if hi {
		*w = *w&0x00FF | uint16(v)<<8
	} else {
		*w = *w&0xFF00 | uint16(v)
	}
}

func getByte16(w uint16, hi bool) uint8 {
	if hi {
		return uint8(w >> 8)
	}
	return uint8(w)
}

func (s *scsp) dspWrite16(addr uint32, v uint16) {
	d := &s.cur.dsp
	switch {
	case addr < 0x780:
		d.coef[(addr-0x700)/2] = int16(v)
	case addr < 0x7c0:
		d.madrs[(addr-0x780)/2] = v
	case addr < 0x800:
		d.madrs[(addr-0x7c0)/2] = v
	case addr < 0xC00:
		d.mpro[(addr-0x800)/2] = v
	}
	if addr == 0xBF0 {
		d.start()
	}
}

func (s *scsp) write8(chip int, addr uint32, v uint8) {
	s.cur = &s.chips[chip]
	c := s.cur
	addr &= 0xffff
	switch {
	case addr < 0x400:
		sl := int(addr / 0x20)
		r := addr & 0x1f
		setByte16(&c.slots[sl].data[r>>1], r&1 == 0, v)
		s.updateSlotReg(sl, int(r^1)&0x1f)
	case addr < 0x600:
		r := addr & 0xff
		if r < 0x30 {
			setByte16(&c.data[r>>1], r&1 == 0, v)
		}
		s.updateReg(int(r^1) & 0xff)
	case addr < 0x700:
		c.ringbuf[(addr-0x600)/2] = int16(v)
	default:
		// the DSP's registers take bytes in place
		d := &c.dsp
		hi := addr&1 == 0
		switch {
		case addr < 0x780:
			w := uint16(d.coef[(addr-0x700)/2])
			setByte16(&w, hi, v)
			d.coef[(addr-0x700)/2] = int16(w)
		case addr < 0x7C0:
			setByte16(&d.madrs[(addr-0x780)/2], hi, v)
		case addr < 0x800:
			setByte16(&d.madrs[(addr-0x7c0)/2], hi, v)
		case addr < 0xC00:
			setByte16(&d.mpro[(addr-0x800)/2], hi, v)
		}
		if addr == 0xBF0 {
			d.start()
		}
	}
}

func (s *scsp) write16(chip int, addr uint32, v uint16) {
	s.cur = &s.chips[chip]
	c := s.cur
	addr &= 0xffff
	switch {
	case addr < 0x400:
		sl := int(addr / 0x20)
		r := addr & 0x1f
		c.slots[sl].data[r>>1] = v
		s.updateSlotReg(sl, int(r)&0x1f)
	case addr < 0x600:
		if addr < 0x430 {
			r := addr & 0x3f
			if r < 0x30 {
				c.data[r>>1] = v
			}
			s.updateReg(int(r))
		}
	case addr < 0x700:
		c.ringbuf[(addr-0x600)/2] = int16(v)
	default:
		s.dspWrite16(addr, v)
	}
}

func (s *scsp) write32(chip int, addr uint32, v uint32) {
	s.cur = &s.chips[chip]
	c := s.cur
	addr &= 0xffff
	switch {
	case addr < 0x400:
		sl := int(addr / 0x20)
		r := addr & 0x1f
		c.slots[sl].data[r>>1] = uint16(v >> 16)
		c.slots[sl].data[(r>>1+1)&15] = uint16(v)
		s.updateSlotReg(sl, int(r)&0x1f)
		s.updateSlotReg(sl, int(r+2)&0x1f)
	case addr < 0x600:
		r := addr & 0xff
		if r < 0x30 {
			c.data[r>>1] = uint16(v >> 16)
		}
		if r+2 < 0x30 {
			c.data[(r+2)>>1] = uint16(v)
		}
		s.updateReg(int(r))
		s.updateReg(int(r + 2))
	case addr < 0x700:
	default:
		s.dspWrite16(addr, uint16(v>>16))
		d := &c.dsp
		switch {
		case addr < 0x780:
			d.coef[((addr-0x700)/2+1)&63] = int16(v)
		case addr < 0x7c0:
			d.madrs[((addr-0x780)/2+1)&31] = uint16(v)
		case addr < 0x800:
			d.madrs[((addr-0x7c0)/2+1)&31] = uint16(v)
		case addr < 0xC00:
			d.mpro[((addr-0x800)/2+1)&511] = uint16(v)
		}
	}
}

func (s *scsp) read8(chip int, addr uint32) uint8 {
	s.cur = &s.chips[chip]
	c := s.cur
	addr &= 0xffff
	switch {
	case addr < 0x400:
		sl := addr / 0x20
		r := addr & 0x1f
		return getByte16(c.slots[sl].data[r>>1], r&1 == 0)
	case addr < 0x600:
		r := addr & 0xff
		s.updateRegR(int(r))
		if r < 0x30 {
			return getByte16(c.data[r>>1], r&1 == 0)
		}
	}
	return 0
}

func (s *scsp) read16(chip int, addr uint32) uint16 {
	s.cur = &s.chips[chip]
	c := s.cur
	d := &c.dsp
	addr &= 0xffff
	switch {
	case addr < 0x400:
		sl := addr / 0x20
		r := addr & 0x1f
		return c.slots[sl].data[r>>1]
	case addr < 0x600:
		if addr < 0x430 {
			r := addr & 0x3f
			s.updateRegR(int(r))
			if r < 0x30 {
				return c.data[r>>1]
			}
		}
		return 0
	case addr < 0x700:
		return uint16(c.ringbuf[(addr-0x600)/2])
	case addr < 0x780:
		return uint16(d.coef[(addr-0x700)/2])
	case addr < 0x7c0:
		return d.madrs[(addr-0x780)/2]
	case addr < 0x800:
		return d.madrs[(addr-0x7c0)/2]
	case addr < 0xC00:
		return d.mpro[(addr-0x800)/2]
	case addr < 0xE00:
		if addr&2 != 0 {
			return uint16(d.temp[(addr>>2)&0x7f])
		}
		return uint16(d.temp[(addr>>2)&0x7f] >> 16)
	case addr < 0xE80:
		if addr&2 != 0 {
			return uint16(d.mems[(addr>>2)&0x1f])
		}
		return uint16(d.mems[(addr>>2)&0x1f] >> 16)
	case addr < 0xEC0:
		if addr&2 != 0 {
			return uint16(d.mixs[(addr>>2)&0xf])
		}
		return uint16(d.mixs[(addr>>2)&0xf] >> 16)
	case addr < 0xEE0:
		return uint16(d.efreg[(addr-0xec0)/2])
	case addr < 0xEE4:
		return uint16(d.exts[(addr-0xee0)/2])
	}
	return 0
}

func (s *scsp) timersAddTicks(ticks int32) {
	m := &s.chips[0]
	for i, reg := range [3]int{0x18 / 2, 0x1a / 2, 0x1c / 2} {
		if s.timCnt[i] <= 0xff00 {
			s.timCnt[i] += ticks << (8 - (m.data[reg] >> 8 & 7))
			if s.timCnt[i] > 0xFF00 {
				s.timCnt[i] = 0xFFFF
				m.data[0x20/2] |= 0x40 << uint(i)
			}
			m.data[reg] &= 0xff00
			m.data[reg] |= uint16(s.timCnt[i] >> 8)
		}
	}
}

// midiIn is a byte from the main board.
func (s *scsp) midiIn(v uint8) {
	s.midiStack[s.midiW] = v
	s.midiW++
	s.midiInFill++
}

// midiOutWrite is the 68000 writing the MIDI out port; as in Supermodel it
// lands in the MIDI in buffer (nothing reads MIDI out on Model 3).
func (s *scsp) midiOutWrite(v uint8) {
	s.midiStack[s.midiOutW] = v
	s.midiOutW = (s.midiOutW + 1) & 31
	s.midiOutFill++
}

// updateSlot makes a voice's next sample, and writes it to the FM ring
// buffer at dst.
func (s *scsp) updateSlot(c *scspChip, slot *scspSlot, dst *int16) int32 {
	if slot.ssctl() != 0 {
		return 0
	}
	step := int32(slot.step)
	if slot.plfos() != 0 {
		step = step * slot.plfo.plfoStep()
		step >>= scspShift
	}
	var addr1, addr2 uint32
	if slot.pcm8b() {
		addr1 = slot.curAddr >> scspShift
		addr2 = slot.nxtAddr >> scspShift
	} else {
		addr1 = (slot.curAddr >> (scspShift - 1)) & 0x7fffe
		addr2 = (slot.nxtAddr >> (scspShift - 1)) & 0x7fffe
	}
	if slot.mdl() != 0 || slot.mdxsl() != 0 || slot.mdysl() != 0 {
		m := &s.chips[0] // Supermodel reads the master chip's ring buffer for both
		smp := (int32(m.ringbuf[(uint16(m.bufptr)+slot.mdxsl())&63]) + int32(m.ringbuf[(uint16(m.bufptr)+slot.mdysl())&63])) / 2
		smp <<= 0xA
		smp >>= uint(0x18-int(slot.mdl())) & 31
		if !slot.pcm8b() {
			smp <<= 1
		}
		addr1 += uint32(smp)
		addr2 += uint32(smp)
		if !slot.pcm8b() {
			addr1 &= 0x7fffe
			addr2 &= 0x7fffe
		} else {
			addr1 &= 0x7ffff
			addr2 &= 0x7ffff
		}
	}
	ram := c.ram
	var sample int32
	fpart := int32(slot.curAddr & (1<<scspShift - 1))
	if slot.pcm8b() {
		p1 := int32(int8(ram[((slot.base+(addr1^1))^1)&0xFFFFF]))
		p2 := int32(int8(ram[((slot.base+(addr2^1))^1)&0xFFFFF]))
		v := (p1<<8)*(1<<scspShift-fpart) + (p2<<8)*fpart
		sample = v >> scspShift
	} else {
		a1 := (slot.base + addr1) & 0xFFFFE
		a2 := (slot.base + addr2) & 0xFFFFE
		p1 := int32(int16(uint16(ram[a1])<<8 | uint16(ram[a1+1])))
		p2 := int32(int16(uint16(ram[a2])<<8 | uint16(ram[a2+1])))
		v := p1*(1<<scspShift-fpart) + p2*fpart
		sample = v >> scspShift
	}
	if slot.sbctl()&1 != 0 {
		sample ^= 0x7FFF
	}
	if slot.sbctl()&2 != 0 {
		sample = int32(int16(sample ^ 0x8000))
	}

	if slot.back {
		slot.curAddr -= uint32(step)
	} else {
		slot.curAddr += uint32(step)
	}
	slot.nxtAddr = slot.curAddr + 1<<scspShift
	addr1 = slot.curAddr >> scspShift
	addr2 = slot.nxtAddr >> scspShift

	if addr1 >= slot.lsa() && !slot.back {
		if slot.lpslnk() && slot.eg.state == egAttack {
			slot.eg.state = egDecay1
		}
	}

	addrs := [2]uint32{addr1, addr2}
	slotAddr := [2]*uint32{&slot.curAddr, &slot.nxtAddr}
	lsa, lea := slot.lsa(), slot.lea()
	for i := 0; i < 2; i++ {
		a := addrs[i]
		sa := slotAddr[i]
		switch slot.lpctl() {
		case 0:
			if a >= lsa && a >= lea {
				slot.stop(false)
			}
		case 1:
			if a >= lea {
				rem := int32(*sa - lea<<scspShift)
				*sa = lsa<<scspShift + uint32(rem)
			}
		case 2:
			if a >= lsa && !slot.back {
				rem := int32(*sa - lsa<<scspShift)
				*sa = lea<<scspShift - uint32(rem)
				slot.back = true
			} else if (a < lsa || *sa&0x80000000 != 0) && slot.back {
				rem := int32(lsa<<scspShift - *sa)
				*sa = lea<<scspShift - uint32(rem)
			}
		case 3:
			if a >= lea {
				rem := int32(*sa - lea<<scspShift)
				*sa = lea<<scspShift - uint32(rem)
				slot.back = true
			} else if (a < lsa || *sa&0x80000000 != 0) && slot.back {
				rem := int32(lsa<<scspShift - *sa)
				*sa = lsa<<scspShift + uint32(rem)
				slot.back = false
			}
		}
	}

	if !slot.sdir() {
		if slot.alfos() != 0 {
			sample = sample * slot.alfo.alfoStep()
			sample >>= scspShift
		}
		if slot.eg.state == egAttack {
			sample = (sample * slot.egUpdate()) >> scspShift
		} else {
			sample = (sample * scspTables.egTable[(slot.egUpdate()>>(scspShift-10))&0x3ff]) >> scspShift
		}
	}

	if !slot.stwinh() {
		enc := uint16(7 << 0xd)
		if !slot.sdir() {
			enc |= slot.tl()
		}
		*dst = int16((sample * scspTables.lpan[enc]) >> (scspShift + 1))
	}
	return sample
}

func clip18(v int32) int32 {
	if v < -131072 {
		return -131072
	}
	if v > 131071 {
		return 131071
	}
	return v
}

func clip16(v int32) int32 {
	if v < -32768 {
		return -32768
	}
	if v > 32767 {
		return 32767
	}
	return v
}

// sample makes one output sample (44.1 kHz) of both chips: front (master)
// and rear (slave) pairs.
func (s *scsp) sample() (fl, fr, rl, rr int32) {
	t := &scspTables
	m, sv := &s.chips[0], &s.chips[1]
	for sl := 0; sl < 32; sl++ {
		dst := &m.ringbuf[m.bufptr]
		if slot := &m.slots[sl]; slot.active {
			smp := s.updateSlot(m, slot, dst)
			enc := slot.tl() | slot.imxl()<<0xd
			m.dsp.mixs[slot.isel()] += (smp * t.lpan[enc]) >> (scspShift - 2)
			enc = slot.tl() | slot.dipan()<<0x8 | slot.disdl()<<0xd
			fl += (smp * t.lpan[enc]) >> scspShift
			fr += (smp * t.rpan[enc]) >> scspShift
		}
		m.bufptr = (m.bufptr + 1) & 63

		dst = &sv.ringbuf[sv.bufptr]
		if slot := &sv.slots[sl]; slot.active {
			smp := s.updateSlot(sv, slot, dst)
			enc := slot.tl() | slot.imxl()<<0xd
			sv.dsp.mixs[slot.isel()] += (smp * t.lpan[enc]) >> (scspShift - 2)
			enc = slot.tl() | slot.dipan()<<0x8 | slot.disdl()<<0xd
			rl += (smp * t.lpan[enc]) >> scspShift
			rr += (smp * t.rpan[enc]) >> scspShift
		}
		sv.bufptr = (sv.bufptr + 1) & 63
	}
	m.dsp.step()
	sv.dsp.step()
	for i := 0; i < 16; i++ {
		if slot := &m.slots[i]; slot.efsdl() != 0 {
			enc := slot.efpan()<<0x8 | slot.efsdl()<<0xd
			fl += (int32(m.dsp.efreg[i]) * t.lpan[enc]) >> scspShift
			fr += (int32(m.dsp.efreg[i]) * t.rpan[enc]) >> scspShift
		}
		if slot := &sv.slots[i]; slot.efsdl() != 0 {
			enc := slot.efpan()<<0x8 | slot.efsdl()<<0xd
			rl += (int32(sv.dsp.efreg[i]) * t.lpan[enc]) >> scspShift
			rr += (int32(sv.dsp.efreg[i]) * t.rpan[enc]) >> scspShift
		}
	}
	if m.data[0]&0x100 != 0 {
		fl, fr = clip18(fl), clip18(fr)
	} else {
		fl, fr = clip16(fl>>2), clip16(fr>>2)
	}
	if sv.data[0]&0x100 != 0 {
		rl, rr = clip18(rl), clip18(rr)
	} else {
		rl, rr = clip16(rl>>2), clip16(rr>>2)
	}
	s.timersAddTicks(1)
	s.checkPendingIRQ()
	return
}
