// Package ym2151 is RetroPixels' Yamaha YM2151 (OPM) FM sound chip, a Go
// port of web/emu/ym2151.js (used by arcade boards). It makes exactly the
// samples the JS chip makes: 8 channels of 4 operators, 8 algorithms,
// feedback, detune, the envelope generator, the LFO, noise and the timers.
//
//	ym := ym2151.New(3579545)
//	ym.Write(port, v)  // port 0 = register address, 1 = data
//	ym.Tick()          // one sample at clock / 64 into ym.Out[0] / ym.Out[1]
package ym2151

import (
	"encoding/binary"
	"errors"
	"math"
)

var dt1Tab = [4][32]int{
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
	{1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
	{2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22},
}
var dt2Cents = [4]float64{0, 600, 781, 950}
var pmsCents = [8]float64{0, 5, 10, 20, 50, 100, 400, 700}
var noteTab = [16]int{0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 8, 9, 10, 11, 11}
var egInc [64 * 8]int

func init() {
	p := [4][8]int{{0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}}
	q := [4][8]int{{1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2}}
	for r := 0; r < 64; r++ {
		for s := 0; s < 8; s++ {
			v := 0
			switch {
			case r < 2:
			case r < 48:
				v = p[r&3][s]
			case r < 60:
				v = q[r&3][s] * (1 << ((r - 48) >> 2))
			default:
				v = 8
			}
			egInc[r*8+s] = v
		}
	}
}

const (
	attack = iota
	decay
	sustain
	release
)

const (
	m1 = 0
	m2 = 8
	c1 = 16
	c2 = 24
)

var keyGroup = [4]int{0, 2, 1, 3}

// YM2151 is one chip.
type YM2151 struct {
	clock, rate float64
	// OnIRQ is called when the IRQ pin changes; OnPort when CT1/CT2 change.
	OnIRQ  func(on bool)
	OnPort func(ct int)

	dt1, mul, tl, ks, ar, amOn, d1r, dt2, d2r, d1l, rr [32]int
	phase, inc, att, egState, key                      [32]int
	kc, kf, fb, alg, panL, panR, pms, ams              [8]int
	fbOut                                              [16]int
	regs                                               [256]uint8

	Out [2]int // the last sample, left and right

	addr, status               int
	IRQ                        bool
	timerA, timerB, cntA, cntB int
	cntBdiv, ctrl14            int
	lfrq, amd, pmd, lfoWave    int
	lfoPhase                   float64
	lfoAM, lfoPM, lfoRand, ct  int
	noiseOn, nfrq, noiseCnt    int
	noiseLfsr, noiseBit        int
	egDiv, egCnt, clk          int
	dirty                      int
}

// New makes a chip running from a clock of clock Hz.
func New(clock float64) *YM2151 {
	if clock == 0 {
		clock = 3579545
	}
	y := &YM2151{clock: clock, rate: clock / 64}
	y.Reset()
	return y
}

// Rate is the native sample rate (clock / 64).
func (y *YM2151) Rate() float64 { return y.rate }

// Reset puts the chip in its power-on state.
func (y *YM2151) Reset() {
	for _, a := range []*[32]int{&y.dt1, &y.mul, &y.tl, &y.ks, &y.ar, &y.amOn, &y.d1r, &y.dt2, &y.d2r, &y.d1l, &y.rr, &y.phase, &y.inc, &y.key} {
		*a = [32]int{}
	}
	for _, a := range []*[8]int{&y.kc, &y.kf, &y.fb, &y.alg, &y.pms, &y.ams} {
		*a = [8]int{}
	}
	y.fbOut = [16]int{}
	y.regs = [256]uint8{}
	for i := range y.att {
		y.att[i] = 0x3ff
		y.egState[i] = release
	}
	for i := range y.panL {
		y.panL[i], y.panR[i] = 1, 1
	}
	y.addr, y.status, y.IRQ = 0, 0, false
	y.timerA, y.timerB, y.cntA, y.cntB, y.cntBdiv, y.ctrl14 = 0, 0, 0, 0, 0, 0
	y.lfrq, y.amd, y.pmd, y.lfoWave, y.lfoPhase, y.lfoAM, y.lfoPM = 0, 0, 0, 0, 0, 0, 0
	y.lfoRand, y.ct = 0x1ffff, 0
	y.noiseOn, y.nfrq, y.noiseCnt, y.noiseLfsr, y.noiseBit = 0, 0, 0, 1, 0
	y.egDiv, y.egCnt, y.clk = 0, 0, 0
	y.dirty = 0xff
}

// Read is the status: bit 0 timer A flag, bit 1 timer B flag.
func (y *YM2151) Read() uint8 { return uint8(y.status) }

func (y *YM2151) setIrq() {
	on := y.status&3 != 0
	if on != y.IRQ {
		y.IRQ = on
		if y.OnIRQ != nil {
			y.OnIRQ(on)
		}
	}
}

// Write: port 0 selects a register, port 1 writes it.
func (y *YM2151) Write(port int, val uint8) {
	v := int(val)
	if port&1 == 0 {
		y.addr = v
		return
	}
	r := y.addr
	y.regs[r] = val
	if r < 0x20 {
		y.writeGlobal(r, v)
		return
	}
	if r < 0x40 {
		ch := r & 7
		switch r & 0x38 {
		case 0x20:
			y.panR[ch], y.panL[ch], y.fb[ch], y.alg[ch] = v>>7, (v>>6)&1, (v>>3)&7, v&7
		case 0x28:
			y.kc[ch] = v & 0x7f
			y.dirty |= 1 << ch
		case 0x30:
			y.kf[ch] = v >> 2
			y.dirty |= 1 << ch
		case 0x38:
			y.pms[ch], y.ams[ch] = (v>>4)&7, v&3
			y.dirty |= 1 << ch
		}
		return
	}
	s := r & 0x1f
	switch r & 0xe0 {
	case 0x40:
		y.dt1[s], y.mul[s] = (v>>4)&7, v&15
	case 0x60:
		y.tl[s] = v & 0x7f
	case 0x80:
		y.ks[s], y.ar[s] = v>>6, v&31
	case 0xa0:
		y.amOn[s], y.d1r[s] = v>>7, v&31
	case 0xc0:
		y.dt2[s], y.d2r[s] = v>>6, v&31
	case 0xe0:
		y.d1l[s], y.rr[s] = v>>4, v&15
	}
	y.dirty |= 1 << (s & 7)
}

func (y *YM2151) writeGlobal(r, v int) {
	switch r {
	case 0x01:
		if v&2 != 0 {
			y.lfoPhase = 0
		}
	case 0x08:
		ch := v & 7
		for k := 0; k < 4; k++ {
			y.keyOnOff(keyGroup[k]*8+ch, (v>>(3+k))&1)
		}
	case 0x0f:
		y.noiseOn, y.nfrq = v>>7, v&31
	case 0x10:
		y.timerA = (y.timerA & 3) | (v << 2)
	case 0x11:
		y.timerA = (y.timerA & 0x3fc) | (v & 3)
	case 0x12:
		y.timerB = v
	case 0x14:
		if v&1 != 0 && y.ctrl14&1 == 0 {
			y.cntA = y.timerA
		}
		if v&2 != 0 && y.ctrl14&2 == 0 {
			y.cntB = y.timerB
			y.cntBdiv = 0
		}
		if v&0x10 != 0 {
			y.status &^= 1
		}
		if v&0x20 != 0 {
			y.status &^= 2
		}
		y.ctrl14 = v
		y.setIrq()
	case 0x18:
		y.lfrq = v
	case 0x19:
		if v&0x80 != 0 {
			y.pmd = v & 0x7f
		} else {
			y.amd = v & 0x7f
		}
	case 0x1b:
		y.lfoWave = v & 3
		if ct := v >> 6; ct != y.ct {
			y.ct = ct
			if y.OnPort != nil {
				y.OnPort(ct)
			}
		}
	}
}

func (y *YM2151) keyOnOff(s, on int) {
	if on != 0 && y.key[s] == 0 {
		y.phase[s] = 0
		y.egState[s] = attack
		y.updateInc(s & 7)
		if y.rateFor(s, y.ar[s]*2) >= 62 {
			y.att[s] = 0
			y.egState[s] = decay
		}
	} else if on == 0 && y.key[s] != 0 {
		y.egState[s] = release
	}
	y.key[s] = on
}

func (y *YM2151) keyCode(ch int) int { return (y.kc[ch] >> 2) & 31 }

func (y *YM2151) rateFor(s, r int) int {
	if r == 0 {
		return 0
	}
	v := r + (y.keyCode(s&7) >> (3 - y.ks[s]))
	if v > 63 {
		return 63
	}
	return v
}

// jsRound is JavaScript's Math.round (halves go up).
func jsRound(x float64) float64 {
	f := math.Floor(x)
	if x-f >= 0.5 {
		f++
	}
	return f
}

func (y *YM2151) updateInc(ch int) {
	kc := y.kc[ch]
	oct := (kc >> 4) & 7
	semis := float64(oct*12+noteTab[kc&15]) + float64(y.kf[ch])/64 - float64(4*12+8)
	if y.pms[ch] != 0 && y.pmd != 0 {
		semis += pmsCents[y.pms[ch]] * (float64(y.lfoPM) / 128) * (float64(y.pmd) / 127) / 100
	}
	code := y.keyCode(ch)
	ratio := y.clock / 3579545
	for g := 0; g < 4; g++ {
		s := g*8 + ch
		hz := 440 * math.Pow(2, semis/12+dt2Cents[y.dt2[s]]/1200) * ratio
		base := int(jsRound(hz * 1048576 / y.rate))
		if d := y.dt1[s]; d&3 != 0 {
			amt := dt1Tab[d&3][code]
			if d&4 != 0 {
				base -= amt
			} else {
				base += amt
			}
		}
		if base < 0 {
			base = 0
		}
		if y.mul[s] != 0 {
			y.inc[s] = (base * y.mul[s]) & 0xfffff
		} else {
			y.inc[s] = (base >> 1) & 0xfffff
		}
	}
}

func (y *YM2151) egStep(s int) {
	st, att := y.egState[s], y.att[s]
	var r int
	switch st {
	case attack:
		r = y.rateFor(s, y.ar[s]*2)
	case decay:
		r = y.rateFor(s, y.d1r[s]*2)
	case sustain:
		r = y.rateFor(s, y.d2r[s]*2)
	default:
		r = y.rateFor(s, y.rr[s]*4+2)
	}
	shift := 0
	if r < 48 {
		shift = 11 - (r >> 2)
	}
	if shift > 0 && y.egCnt&((1<<shift)-1) != 0 {
		return
	}
	inc := egInc[r*8+((y.egCnt>>shift)&7)]
	if st == attack {
		if r >= 62 {
			att = 0
		} else if att != 0 && inc != 0 {
			att += (^att * inc) >> 4
		}
		if att <= 0 {
			att = 0
			st = decay
		}
	} else {
		att += inc
		if att > 0x3ff {
			att = 0x3ff
		}
	}
	if st == decay {
		lv := y.d1l[s] << 5
		if y.d1l[s] == 15 {
			lv = 0x3e0
		}
		if att >= lv {
			st = sustain
		}
	}
	y.egState[s], y.att[s] = st, att&0xffff
}

func (y *YM2151) opOut(s, mod, am int) int {
	a := y.att[s] + (y.tl[s] << 3)
	if y.amOn[s] != 0 {
		a += am
	}
	if a >= 0x3ff {
		return 0
	}
	p := ((y.phase[s] >> 10) + mod) & 0x3ff
	q := p & 0xff
	if p&0x100 != 0 {
		q ^= 0xff
	}
	level := logsin[q] + (a << 2)
	v := 0
	if level < 3328 {
		v = (expTab[level&0xff] << 2) >> (level >> 8)
	}
	if p&0x200 != 0 {
		return -v
	}
	return v
}

func (y *YM2151) noiseOut(am int) int {
	a := y.att[31] + (y.tl[31] << 3)
	if y.amOn[31] != 0 {
		a += am
	}
	if a >= 0x3ff {
		return 0
	}
	v := (0x3ff - a) << 1
	if y.noiseBit != 0 {
		return v
	}
	return -v
}

func (y *YM2151) stepLFO() {
	hz := float64(16+(y.lfrq&15)) / 32 * math.Pow(2, float64(y.lfrq>>4)) * 0.0016666 * (y.clock / 3579545)
	old := y.lfoPhase
	y.lfoPhase += hz / y.rate
	if y.lfoPhase >= 1 {
		y.lfoPhase -= math.Floor(y.lfoPhase)
	}
	p := y.lfoPhase
	var am, pm int
	switch y.lfoWave {
	case 0:
		am = 255 - int(math.Floor(p*256))
		if p < 0.5 {
			pm = int(math.Floor(p * 256))
		} else {
			pm = int(math.Floor(p*256)) - 256
		}
	case 1:
		if p < 0.5 {
			am, pm = 255, 127
		} else {
			am, pm = 0, -128
		}
	case 2:
		if p < 0.5 {
			am = 255 - int(math.Floor(p*510))
		} else {
			am = int(math.Floor((p - 0.5) * 510))
		}
		if p < 0.25 {
			pm = int(math.Floor(p * 508))
		} else if p < 0.75 {
			pm = int(math.Floor((0.5 - p) * 508))
		} else {
			pm = int(math.Floor((p - 1) * 508))
		}
	default:
		if math.Floor(p*32) != math.Floor(old*32) {
			b := (y.lfoRand ^ (y.lfoRand >> 3)) & 1
			y.lfoRand = (y.lfoRand >> 1) | (b << 16)
		}
		am = y.lfoRand & 0xff
		pm = (y.lfoRand & 0xff) - 128
	}
	y.lfoAM = am
	if pm != y.lfoPM {
		y.lfoPM = pm
		if y.pmd != 0 {
			for c := 0; c < 8; c++ {
				if y.pms[c] != 0 {
					y.dirty |= 1 << c
				}
			}
		}
	}
}

// Tick makes one sample (at clock / 64) into Out.
func (y *YM2151) Tick() {
	y.stepLFO()
	if y.dirty != 0 {
		for c := 0; c < 8; c++ {
			if y.dirty&(1<<c) != 0 {
				y.updateInc(c)
			}
		}
		y.dirty = 0
	}
	if y.egDiv++; y.egDiv == 3 {
		y.egDiv = 0
		y.egCnt = (y.egCnt + 1) & 0xfff
		for s := 0; s < 32; s++ {
			y.egStep(s)
		}
	}
	y.noiseCnt += 2
	np := 32 - y.nfrq
	for y.noiseCnt >= np {
		y.noiseCnt -= np
		b := (y.noiseLfsr ^ (y.noiseLfsr >> 3)) & 1
		y.noiseLfsr = (y.noiseLfsr >> 1) | (b << 16)
		y.noiseBit = y.noiseLfsr & 1
	}
	amBase := (y.lfoAM * y.amd) >> 7
	L, R := 0, 0
	for ch := 0; ch < 8; ch++ {
		am := 0
		if y.ams[ch] != 0 {
			am = amBase << (y.ams[ch] - 1)
		}
		fbn := y.fb[ch]
		f0, f1 := y.fbOut[ch*2], y.fbOut[ch*2+1]
		mod := 0
		if fbn != 0 {
			mod = (f0 + f1) >> (10 - fbn)
		}
		o1 := y.opOut(m1+ch, mod, am)
		y.fbOut[ch*2], y.fbOut[ch*2+1] = f1, o1
		noise := ch == 7 && y.noiseOn != 0
		car2 := func(mod int) int {
			if noise {
				return y.noiseOut(am)
			}
			return y.opOut(c2+ch, mod, am)
		}
		var o int
		switch y.alg[ch] {
		case 0:
			k1 := y.opOut(c1+ch, o1>>1, am)
			k2 := y.opOut(m2+ch, k1>>1, am)
			o = car2(k2 >> 1)
		case 1:
			k1 := y.opOut(c1+ch, 0, am)
			k2 := y.opOut(m2+ch, (o1+k1)>>1, am)
			o = car2(k2 >> 1)
		case 2:
			k1 := y.opOut(c1+ch, 0, am)
			k2 := y.opOut(m2+ch, k1>>1, am)
			o = car2((o1 + k2) >> 1)
		case 3:
			k1 := y.opOut(c1+ch, o1>>1, am)
			k2 := y.opOut(m2+ch, 0, am)
			o = car2((k1 + k2) >> 1)
		case 4:
			k1 := y.opOut(c1+ch, o1>>1, am)
			k2 := y.opOut(m2+ch, 0, am)
			o = k1 + car2(k2>>1)
		case 5:
			o = y.opOut(c1+ch, o1>>1, am)
			o += y.opOut(m2+ch, o1>>1, am)
			o += car2(o1 >> 1)
		case 6:
			o = y.opOut(c1+ch, o1>>1, am)
			o += y.opOut(m2+ch, 0, am)
			o += car2(0)
		default:
			o = o1 + y.opOut(c1+ch, 0, am)
			o += y.opOut(m2+ch, 0, am)
			o += car2(0)
		}
		if y.panL[ch] != 0 {
			L += o
		}
		if y.panR[ch] != 0 {
			R += o
		}
	}
	y.Out[0] = clamp16(L)
	y.Out[1] = clamp16(R)
	for s := 0; s < 32; s++ {
		y.phase[s] = (y.phase[s] + y.inc[s]) & 0xfffff
	}
	y.timers()
}

func clamp16(v int) int {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return v
}

func (y *YM2151) timers() {
	c := y.ctrl14
	if c&1 != 0 {
		if y.cntA++; y.cntA >= 1024 {
			y.cntA = y.timerA
			if c&4 != 0 {
				y.status |= 1
				y.setIrq()
			}
			if c&0x80 != 0 {
				for ch := 0; ch < 8; ch++ {
					for g := 0; g < 4; g++ {
						s := g*8 + ch
						if y.key[s] == 0 {
							y.keyOnOff(s, 1)
							y.keyOnOff(s, 0)
							y.egState[s] = attack
						}
					}
				}
			}
		}
	}
	if c&2 != 0 {
		if y.cntBdiv++; y.cntBdiv >= 16 {
			y.cntBdiv = 0
			if y.cntB++; y.cntB >= 256 {
				y.cntB = y.timerB
				if c&8 != 0 {
					y.status |= 2
					y.setIrq()
				}
			}
		}
	}
}

// ---- save states ----

var errState = errors.New("ym2151: damaged save state")

const stateVersion = 1

// AppendState appends the chip's state to b.
func (y *YM2151) AppendState(b []byte) []byte {
	b = append(b, stateVersion)
	put := func(v int) { b = binary.LittleEndian.AppendUint32(b, uint32(int32(v))) }
	for _, a := range y.arrays32() {
		for _, v := range a {
			put(v)
		}
	}
	for _, a := range y.arrays8() {
		for _, v := range a {
			put(v)
		}
	}
	for _, v := range y.fbOut {
		put(v)
	}
	b = append(b, y.regs[:]...)
	for _, p := range y.scalars() {
		put(*p)
	}
	irq := 0
	if y.IRQ {
		irq = 1
	}
	put(irq)
	return binary.LittleEndian.AppendUint64(b, math.Float64bits(y.lfoPhase))
}

// LoadState reads a state written by AppendState and returns the rest of b.
func (y *YM2151) LoadState(b []byte) ([]byte, error) {
	n := 1 + (11+5)*32*4 + 8*8*4 + 16*4 + 256 + (len(y.scalars())+1)*4 + 8
	if len(b) < n || b[0] != stateVersion {
		return b, errState
	}
	o := 1
	get := func() int { v := int(int32(binary.LittleEndian.Uint32(b[o:]))); o += 4; return v }
	for _, a := range y.arrays32() {
		for i := range a {
			a[i] = get()
		}
	}
	for _, a := range y.arrays8() {
		for i := range a {
			a[i] = get()
		}
	}
	for i := range y.fbOut {
		y.fbOut[i] = get()
	}
	copy(y.regs[:], b[o:o+256])
	o += 256
	for _, p := range y.scalars() {
		*p = get()
	}
	y.IRQ = get() != 0
	y.lfoPhase = math.Float64frombits(binary.LittleEndian.Uint64(b[o:]))
	o += 8
	y.dirty = 0xff
	return b[o:], nil
}

func (y *YM2151) arrays32() [][]int {
	return [][]int{y.dt1[:], y.mul[:], y.tl[:], y.ks[:], y.ar[:], y.amOn[:], y.d1r[:], y.dt2[:], y.d2r[:], y.d1l[:], y.rr[:],
		y.phase[:], y.inc[:], y.att[:], y.egState[:], y.key[:]}
}
func (y *YM2151) arrays8() [][]int {
	return [][]int{y.kc[:], y.kf[:], y.fb[:], y.alg[:], y.panL[:], y.panR[:], y.pms[:], y.ams[:]}
}
func (y *YM2151) scalars() []*int {
	return []*int{&y.addr, &y.status, &y.timerA, &y.timerB, &y.cntA, &y.cntB, &y.cntBdiv, &y.ctrl14, &y.lfrq, &y.amd, &y.pmd,
		&y.lfoWave, &y.lfoAM, &y.lfoPM, &y.lfoRand, &y.ct, &y.noiseOn, &y.nfrq, &y.noiseCnt, &y.noiseLfsr, &y.noiseBit,
		&y.egDiv, &y.egCnt, &y.clk}
}
