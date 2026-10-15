// Package ym2612 is RetroPixels' Yamaha YM2612 (OPN2) FM sound chip, a Go
// port of web/emu/ym2612.js (used by the Genesis). It makes exactly the same
// samples as the JS core: 6 channels of 4 operators, 8 algorithms, the
// envelope generator with key scaling, detune, LFO (AM / PM), channel 3
// special mode, timers A / B, SSG-EG and the DAC.
//
//	ym := ym2612.New()
//	ym.Write(port, v) // port 0/1: address / data of part 1, 2/3: part 2
//	ym.Tick()         // one native sample (the chip's clock / 144) into ym.Out
package ym2612

import (
	"encoding/binary"
	"errors"
	"math"
)

// detune amounts per key code (from the chip's data sheet)
var dtTab = [4][32]int{
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
	{1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
	{2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22},
}

// envelope increments: 64 rates x 8 steps
var egInc [64 * 8]int

var pmMul [8][32]float64

var lfoPeriod = [8]int{108, 77, 71, 67, 62, 44, 8, 5}
var amShift = [4]uint{8, 3, 1, 0}
var keySlot = [4]int{0, 2, 1, 3} // key-on bit (S1,S2,S3,S4) -> register-order slot

func init() {
	p := [4][8]int{{0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}}
	q := [4][8]int{{1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2}}
	for r := 0; r < 64; r++ {
		for s := 0; s < 8; s++ {
			var v int
			switch {
			case r < 2:
				v = 0
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
	for f := range pmMul {
		for s := range pmMul[f] {
			pmMul[f][s] = math.Float64frombits(pmMulBits[f][s])
		}
	}
}

const (
	attack = iota
	decay
	sustain
	release
)

// YM2612 is one chip.
type YM2612 struct {
	// per operator (24 = 6 channels x 4 slots in register order: S1,S3,S2,S4)
	dt, mul, tl, ks, ar, amOn, d1r, d2r, sl, rr, ssg [24]uint8
	phase, inc                                       [24]uint32
	kc                                               [24]uint8
	att                                              [24]int
	egState, key, ssgInv                             [24]uint8
	// per channel
	fnum                                 [6]uint16
	block, fb, alg, panL, panR, ams, fms [6]uint8
	fbOut                                [12]int32 // two previous S1 outputs per channel
	c3fnum                               [3]uint16
	c3block                              [3]uint8
	latch                                [2]uint8
	c3latch                              uint8
	regs                                 [512]uint8
	// Out is the last sample, left and right (each about ±1536).
	Out   [2]int
	chOut [6]int

	addr                                          [2]uint8
	status                                        uint8
	timerA, timerB, cntA, cntB, cntBdiv, mode27   int
	dacOn, dac                                    int
	lfoOn, lfoFreq, lfoCnt, lfoStep, egDiv, egCnt int
	dirty                                         int
}

// New makes a chip, reset.
func New() *YM2612 {
	y := &YM2612{}
	y.Reset()
	return y
}

// Reset puts the chip in its power-on state.
func (y *YM2612) Reset() {
	*y = YM2612{}
	for i := range y.att {
		y.att[i] = 0x3ff
		y.egState[i] = release
	}
	for i := range y.panL {
		y.panL[i], y.panR[i] = 1, 1
	}
	y.dac = 0x80
	y.dirty = 0x3f
}

// ReadStatus is the status byte (timer flags).
func (y *YM2612) ReadStatus() uint8 { return y.status }

// Write takes a byte written to one of the chip's four ports.
func (y *YM2612) Write(port int, v uint8) {
	if port&1 == 0 {
		y.addr[port>>1&1] = v
		return
	}
	part := port >> 1 & 1
	r := int(y.addr[part])
	if part == 0 && r < 0x30 {
		y.writeGlobal(r, v)
		return
	}
	if r < 0x30 {
		return
	}
	y.regs[part*256+r] = v
	chi := r & 3
	if chi == 3 {
		return
	}
	ch := part*3 + chi
	if r < 0xa0 {
		slot := ch*4 + (r>>2)&3
		switch r & 0xf0 {
		case 0x30:
			y.dt[slot], y.mul[slot] = (v>>4)&7, v&15
		case 0x40:
			y.tl[slot] = v & 0x7f
		case 0x50:
			y.ks[slot], y.ar[slot] = v>>6, v&31
		case 0x60:
			y.amOn[slot], y.d1r[slot] = v>>7, v&31
		case 0x70:
			y.d2r[slot] = v & 31
		case 0x80:
			y.sl[slot], y.rr[slot] = v>>4, v&15
		case 0x90:
			y.ssg[slot] = v & 15
		}
		y.dirty |= 1 << ch
		return
	}
	switch r & 0xfc {
	case 0xa0:
		y.fnum[ch] = uint16(y.latch[part]&7)<<8 | uint16(v)
		y.block[ch] = (y.latch[part] >> 3) & 7
		y.dirty |= 1 << ch
	case 0xa4:
		y.latch[part] = v & 0x3f
	case 0xa8:
		if part == 0 {
			y.c3fnum[chi] = uint16(y.c3latch&7)<<8 | uint16(v)
			y.c3block[chi] = (y.c3latch >> 3) & 7
			y.dirty |= 4
		}
	case 0xac:
		if part == 0 {
			y.c3latch = v & 0x3f
		}
	case 0xb0:
		y.fb[ch], y.alg[ch] = (v>>3)&7, v&7
	case 0xb4:
		y.panL[ch], y.panR[ch], y.ams[ch], y.fms[ch] = v>>7, (v>>6)&1, (v>>4)&3, v&7
	}
}

func (y *YM2612) writeGlobal(r int, v uint8) {
	y.regs[r] = v
	switch r {
	case 0x22:
		y.lfoOn, y.lfoFreq = int(v>>3)&1, int(v&7)
		if y.lfoOn == 0 {
			y.lfoCnt, y.lfoStep = 0, 0
		}
		y.dirty = 0x3f
	case 0x24:
		y.timerA = y.timerA&3 | int(v)<<2
	case 0x25:
		y.timerA = y.timerA&0x3fc | int(v&3)
	case 0x26:
		y.timerB = int(v)
	case 0x27:
		vi := int(v)
		if (vi^y.mode27)&0xc0 != 0 {
			y.dirty |= 4
		}
		if vi&1 != 0 && y.mode27&1 == 0 {
			y.cntA = y.timerA
		}
		if vi&2 != 0 && y.mode27&2 == 0 {
			y.cntB, y.cntBdiv = y.timerB, 0
		}
		if vi&0x10 != 0 {
			y.status &^= 1
		}
		if vi&0x20 != 0 {
			y.status &^= 2
		}
		y.mode27 = vi
	case 0x28:
		ch := int(v & 3)
		if ch == 3 {
			return
		}
		if v&4 != 0 {
			ch += 3
		}
		for k := 0; k < 4; k++ {
			y.keyOnOff(ch*4+keySlot[k], (v>>(4+k))&1)
		}
	case 0x2a:
		y.dac = int(v)
	case 0x2b:
		y.dacOn = int(v >> 7)
	}
}

func (y *YM2612) keyOnOff(s int, on uint8) {
	if on != 0 && y.key[s] == 0 {
		y.phase[s], y.ssgInv[s] = 0, 0
		y.egState[s] = attack
		y.updateInc(s >> 2)
		if y.rateFor(s, int(y.ar[s])*2) >= 62 {
			y.att[s] = 0
			y.egState[s] = decay
		}
	} else if on == 0 && y.key[s] != 0 {
		if y.ssg[s]&8 != 0 && (y.ssgInv[s]^((y.ssg[s]>>2)&1)) != 0 {
			y.att[s] = (0x200 - y.att[s]) & 0x3ff
		}
		y.egState[s] = release
	}
	y.key[s] = on
}

func (y *YM2612) rateFor(s, r int) int {
	if r == 0 {
		return 0
	}
	v := r + int(y.kc[s]>>(3-y.ks[s]))
	if v > 63 {
		return 63
	}
	return v
}

// updateInc recomputes the phase increments of one channel.
func (y *YM2612) updateInc(ch int) {
	special := ch == 2 && y.mode27&0xc0 != 0
	pm := 1.0
	if y.lfoOn != 0 && y.fms[ch] != 0 {
		pm = pmMul[y.fms[ch]][y.lfoStep>>2]
	}
	for k := 0; k < 4; k++ {
		s := ch*4 + k
		fn, bl := int(y.fnum[ch]), int(y.block[ch])
		if special && k != 3 {
			i := 2
			switch k {
			case 0:
				i = 1
			case 1:
				i = 0
			}
			fn, bl = int(y.c3fnum[i]), int(y.c3block[i])
		}
		f11 := (fn >> 10) & 1
		n3 := 0
		if (f11 != 0 && fn&0x380 != 0) || (f11 == 0 && fn&0x380 == 0x380) {
			n3 = 1
		}
		y.kc[s] = uint8(bl<<2 | f11<<1 | n3)
		f := fn
		if pm != 1 {
			f = int(math.Floor(float64(fn)*pm)) & 0xfff
		}
		base := (f << bl) >> 1
		d := y.dt[s]
		if d&3 != 0 {
			amt := dtTab[d&3][y.kc[s]]
			if d&4 != 0 {
				base -= amt
			} else {
				base += amt
			}
		}
		base &= 0x1ffff
		if y.mul[s] != 0 {
			y.inc[s] = uint32(base*int(y.mul[s])) & 0xfffff
		} else {
			y.inc[s] = uint32(base>>1) & 0xfffff
		}
	}
}

func (y *YM2612) egStep(s int) {
	st, att := y.egState[s], y.att[s]
	ssgOn := y.ssg[s]&8 != 0
	if ssgOn && att >= 0x200 && st != attack && st != release && y.key[s] != 0 {
		g := y.ssg[s]
		if g&1 != 0 {
			if g&2 != 0 {
				y.ssgInv[s] = 1
			}
		} else if g&2 != 0 {
			y.ssgInv[s] ^= 1
		} else {
			y.phase[s] = 0
		}
		if g&1 == 0 {
			st = attack
			if y.rateFor(s, int(y.ar[s])*2) >= 62 {
				att = 0
				st = decay
			}
		}
	}
	var r int
	switch st {
	case attack:
		r = y.rateFor(s, int(y.ar[s])*2)
	case decay:
		r = y.rateFor(s, int(y.d1r[s])*2)
	case sustain:
		r = y.rateFor(s, int(y.d2r[s])*2)
	default:
		r = y.rateFor(s, int(y.rr[s])*4+2)
	}
	shift := 0
	if r < 48 {
		shift = 11 - r>>2
	}
	if shift > 0 && y.egCnt&(1<<shift-1) != 0 {
		y.egState[s], y.att[s] = st, att
		return
	}
	inc := egInc[r*8+(y.egCnt>>shift)&7]
	switch {
	case st == attack:
		if r >= 62 {
			att = 0
		} else if att != 0 && inc != 0 {
			att += (^att * inc) >> 4
		}
		if att <= 0 {
			att = 0
			st = decay
		}
	case ssgOn:
		if att < 0x200 {
			att += 4 * inc
		}
		if st == release && att >= 0x200 {
			att = 0x3ff
		}
	default:
		att += inc
		if att > 0x3ff {
			att = 0x3ff
		}
	}
	if st == decay {
		slv := int(y.sl[s]) << 5
		if y.sl[s] == 15 {
			slv = 0x3e0
		}
		if att >= slv {
			st = sustain
		}
	}
	y.egState[s], y.att[s] = st, att
}

func (y *YM2612) opOut(s, mod, am int) int {
	a := y.att[s]
	if y.ssg[s]&8 != 0 && y.egState[s] != release && (y.ssgInv[s]^((y.ssg[s]>>2)&1)) != 0 {
		a = (0x200 - a) & 0x3ff
	}
	a += int(y.tl[s]) << 3
	if y.amOn[s] != 0 {
		a += am
	}
	if a >= 0x3ff {
		return 0
	}
	p := (int(y.phase[s]>>10) + mod) & 0x3ff
	q := p & 0xff
	if p&0x100 != 0 {
		q ^= 0xff
	}
	level := int(logSin[q]) + a<<2
	v := 0
	if level < 3328 {
		v = (int(expTab[level&0xff]) << 2) >> (level >> 8)
	}
	if p&0x200 != 0 {
		return -v
	}
	return v
}

// Tick makes one sample into Out.
func (y *YM2612) Tick() {
	// LFO
	if y.lfoOn != 0 {
		y.lfoCnt++
		if y.lfoCnt >= lfoPeriod[y.lfoFreq] {
			y.lfoCnt = 0
			old := y.lfoStep >> 2
			y.lfoStep = (y.lfoStep + 1) & 127
			if y.lfoStep>>2 != old {
				for c := 0; c < 6; c++ {
					if y.fms[c] != 0 {
						y.dirty |= 1 << c
					}
				}
			}
		}
	}
	amBase := 0
	if y.lfoOn != 0 {
		if y.lfoStep < 64 {
			amBase = (y.lfoStep ^ 63) * 2
		} else {
			amBase = (y.lfoStep & 63) * 2
		}
	}
	if y.dirty != 0 {
		for c := 0; c < 6; c++ {
			if y.dirty&(1<<c) != 0 {
				y.updateInc(c)
			}
		}
		y.dirty = 0
	}
	// envelope (every 3 samples)
	y.egDiv++
	if y.egDiv == 3 {
		y.egDiv = 0
		y.egCnt = (y.egCnt + 1) & 0xfff
		for s := 0; s < 24; s++ {
			y.egStep(s)
		}
	}
	L, R := 0, 0
	for ch := 0; ch < 6; ch++ {
		b := ch * 4
		var outv int
		if ch == 5 && y.dacOn != 0 {
			outv = (y.dac - 128) << 6
		} else {
			am := 0
			if y.ams[ch] != 0 {
				am = amBase >> amShift[y.ams[ch]]
			}
			fbn := int(y.fb[ch])
			f0, f1 := int(y.fbOut[ch*2]), int(y.fbOut[ch*2+1])
			mod := 0
			if fbn != 0 {
				mod = (f0 + f1) >> (10 - fbn)
			}
			s1 := y.opOut(b, mod, am)
			y.fbOut[ch*2], y.fbOut[ch*2+1] = int32(f1), int32(s1)
			S2, S3, S4 := b+2, b+1, b+3
			switch y.alg[ch] {
			case 0:
				s2 := y.opOut(S2, s1>>1, am)
				s3 := y.opOut(S3, s2>>1, am)
				outv = y.opOut(S4, s3>>1, am)
			case 1:
				s2 := y.opOut(S2, 0, am)
				s3 := y.opOut(S3, (s1+s2)>>1, am)
				outv = y.opOut(S4, s3>>1, am)
			case 2:
				s2 := y.opOut(S2, 0, am)
				s3 := y.opOut(S3, s2>>1, am)
				outv = y.opOut(S4, (s1+s3)>>1, am)
			case 3:
				s2 := y.opOut(S2, s1>>1, am)
				s3 := y.opOut(S3, 0, am)
				outv = y.opOut(S4, (s2+s3)>>1, am)
			case 4:
				s2 := y.opOut(S2, s1>>1, am)
				s3 := y.opOut(S3, 0, am)
				outv = s2 + y.opOut(S4, s3>>1, am)
			case 5:
				outv = y.opOut(S2, s1>>1, am)
				outv += y.opOut(S3, s1>>1, am)
				outv += y.opOut(S4, s1>>1, am)
			case 6:
				outv = y.opOut(S2, s1>>1, am)
				outv += y.opOut(S3, 0, am)
				outv += y.opOut(S4, 0, am)
			default:
				outv = s1 + y.opOut(S2, 0, am)
				outv += y.opOut(S3, 0, am)
				outv += y.opOut(S4, 0, am)
			}
			if outv > 8191 {
				outv = 8191
			} else if outv < -8192 {
				outv = -8192
			}
		}
		outv >>= 5
		y.chOut[ch] = outv
		if y.panL[ch] != 0 {
			L += outv
		}
		if y.panR[ch] != 0 {
			R += outv
		}
	}
	y.Out[0], y.Out[1] = L, R
	// phase
	for s := 0; s < 24; s++ {
		y.phase[s] = (y.phase[s] + y.inc[s]) & 0xfffff
	}
	// timers
	if y.mode27&1 != 0 {
		y.cntA++
		if y.cntA >= 1024 {
			y.cntA = y.timerA
			if y.mode27&4 != 0 {
				y.status |= 1
			}
			if y.mode27&0xc0 == 0x80 {
				for k := 0; k < 4; k++ {
					s := 8 + k
					if y.key[s] == 0 {
						y.keyOnOff(s, 1)
						y.keyOnOff(s, 0)
						y.egState[s] = attack
					}
				}
			}
		}
	}
	if y.mode27&2 != 0 {
		y.cntBdiv++
		if y.cntBdiv >= 16 {
			y.cntBdiv = 0
			y.cntB++
			if y.cntB >= 256 {
				y.cntB = y.timerB
				if y.mode27&8 != 0 {
					y.status |= 2
				}
			}
		}
	}
}

// ---- save states ----

var errState = errors.New("ym2612: damaged save state")

const stateVersion = 1

// AppendState appends the chip's state to b.
func (y *YM2612) AppendState(b []byte) []byte {
	b = append(b, stateVersion)
	for _, a := range [][]uint8{y.dt[:], y.mul[:], y.tl[:], y.ks[:], y.ar[:], y.amOn[:], y.d1r[:], y.d2r[:], y.sl[:], y.rr[:], y.ssg[:],
		y.kc[:], y.egState[:], y.key[:], y.ssgInv[:], y.block[:], y.fb[:], y.alg[:], y.panL[:], y.panR[:], y.ams[:], y.fms[:],
		y.c3block[:], y.latch[:], y.regs[:], y.addr[:]} {
		b = append(b, a...)
	}
	for i := range y.phase {
		b = binary.LittleEndian.AppendUint32(b, y.phase[i])
		b = binary.LittleEndian.AppendUint32(b, y.inc[i])
		b = binary.LittleEndian.AppendUint16(b, uint16(y.att[i]))
	}
	for _, v := range y.fnum {
		b = binary.LittleEndian.AppendUint16(b, v)
	}
	for _, v := range y.c3fnum {
		b = binary.LittleEndian.AppendUint16(b, v)
	}
	for _, v := range y.fbOut {
		b = binary.LittleEndian.AppendUint32(b, uint32(v))
	}
	b = append(b, y.c3latch, y.status)
	for _, v := range []int{y.timerA, y.timerB, y.cntA, y.cntB, y.cntBdiv, y.mode27, y.dacOn, y.dac,
		y.lfoOn, y.lfoFreq, y.lfoCnt, y.lfoStep, y.egDiv, y.egCnt, y.Out[0], y.Out[1], y.dirty} {
		b = binary.LittleEndian.AppendUint32(b, uint32(int32(v)))
	}
	return b
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (y *YM2612) LoadState(b []byte) ([]byte, error) {
	n := len(y.AppendState(nil))
	if len(b) < n || b[0] != stateVersion {
		return b, errState
	}
	t := *y
	o := 1
	for _, a := range [][]uint8{t.dt[:], t.mul[:], t.tl[:], t.ks[:], t.ar[:], t.amOn[:], t.d1r[:], t.d2r[:], t.sl[:], t.rr[:], t.ssg[:],
		t.kc[:], t.egState[:], t.key[:], t.ssgInv[:], t.block[:], t.fb[:], t.alg[:], t.panL[:], t.panR[:], t.ams[:], t.fms[:],
		t.c3block[:], t.latch[:], t.regs[:], t.addr[:]} {
		o += copy(a, b[o:])
	}
	for i := range t.phase {
		t.phase[i] = binary.LittleEndian.Uint32(b[o:])
		t.inc[i] = binary.LittleEndian.Uint32(b[o+4:])
		t.att[i] = int(binary.LittleEndian.Uint16(b[o+8:]))
		o += 10
	}
	for i := range t.fnum {
		t.fnum[i] = binary.LittleEndian.Uint16(b[o:])
		o += 2
	}
	for i := range t.c3fnum {
		t.c3fnum[i] = binary.LittleEndian.Uint16(b[o:])
		o += 2
	}
	for i := range t.fbOut {
		t.fbOut[i] = int32(binary.LittleEndian.Uint32(b[o:]))
		o += 4
	}
	t.c3latch, t.status = b[o], b[o+1]
	o += 2
	for _, p := range []*int{&t.timerA, &t.timerB, &t.cntA, &t.cntB, &t.cntBdiv, &t.mode27, &t.dacOn, &t.dac,
		&t.lfoOn, &t.lfoFreq, &t.lfoCnt, &t.lfoStep, &t.egDiv, &t.egCnt, &t.Out[0], &t.Out[1], &t.dirty} {
		*p = int(int32(binary.LittleEndian.Uint32(b[o:])))
		o += 4
	}
	if !validState(&t) {
		return b, errState
	}
	t.dirty &= 0x3f // (channels whose increments wait to be worked out)
	*y = t
	return b[n:], nil
}

// validState: values that index tables stay in range.
func validState(t *YM2612) bool {
	if t.lfoFreq < 0 || t.lfoFreq > 7 || t.lfoStep < 0 || t.lfoStep > 127 || t.egCnt < 0 || t.egCnt > 0xfff {
		return false
	}
	for i := range t.att {
		if t.egState[i] > release || t.kc[i] > 31 || t.att[i] > 0x3ff || t.dt[i] > 7 || t.ks[i] > 3 {
			return false
		}
	}
	for i := range t.fms {
		if t.fms[i] > 7 || t.ams[i] > 3 || t.alg[i] > 7 || t.fb[i] > 7 || t.block[i] > 7 {
			return false
		}
	}
	return true
}
