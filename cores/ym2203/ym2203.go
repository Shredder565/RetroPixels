// Package ym2203 is RetroPixels' Yamaha YM2203 (OPN) sound chip: three FM
// channels of four operators (8 algorithms, envelope generator with key
// scaling, detune, SSG-EG, channel 3 special mode), timers A / B with their
// interrupt, and an AY-3-8910-compatible SSG (cores/ay8910 in YM mode).
//
// The FM engine is the OPN core shared with the YM2612 (cores/ym2612) without
// the YM2612's LFO, DAC, stereo and 9-bit output stage.
//
//	ym := ym2203.New()
//	ym.Write(port, v) // port 0: address, 1: data
//	ym.Run(cycles)    // advance by input-clock cycles
//	fm, ssg := ym.Take() // average outputs since the last Take
//
// The prescaler (registers 0x2d-0x2f) picks how the input clock is divided:
// after reset FM samples come every 72 clocks and the SSG gets clock / 4; a
// write to 0x2e (after 0x2d) makes that 36 and clock / 2, 0x2f 24 and clock.
// The SSG steps every 4 of its clocks (an AY-3-8910 every 8), so a tone is
// (clock / divider) / (8 * period) Hz.
package ym2203

import (
	"encoding/binary"
	"errors"
	"math"

	"retropixels/cores/ay8910"
)

// detune amounts per key code (from the chip's data sheet)
var dtTab = [4][32]int{
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
	{1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
	{2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22},
}

var (
	egInc   [64 * 8]int          // envelope increments: 64 rates x 8 steps
	logSin  [256]uint16          // -log2(sin) of a quarter wave, 8.8 fixed point
	expTab  [256]uint16          // 2^(-i/256) * 2048
	keySlot = [4]int{0, 2, 1, 3} // key-on bit (S1,S2,S3,S4) -> register-order slot
)

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
	for i := range logSin {
		logSin[i] = uint16(math.Round(-math.Log2(math.Sin((float64(i)+0.5)*math.Pi/512)) * 256))
		expTab[i] = uint16(math.Round(2048 * math.Pow(2, -float64(i)/256)))
	}
}

const (
	attack = iota
	decay
	sustain
	release
)

// Chip is one YM2203.
type Chip struct {
	// per operator (12 = 3 channels x 4 slots in register order: S1,S3,S2,S4)
	dt, mul, tl, ks, ar, amOn, d1r, d2r, sl, rr, ssg [12]uint8
	phase, inc                                       [12]uint32
	kc                                               [12]uint8
	att                                              [12]int
	egState, key, ssgInv                             [12]uint8
	// per channel
	fnum           [3]uint16
	block, fb, alg [3]uint8
	fbOut          [6]int32 // two previous S1 outputs per channel
	c3fnum         [3]uint16
	c3block        [3]uint8
	latch, c3latch uint8
	dirty          int

	addr                                uint8
	status                              uint8
	timerA, timerB, cntA, cntB, cntBdiv int
	mode27                              int
	egDiv, egCnt                        int
	fmDiv, ssgDiv                       int // prescaler: 6/4, 3/2 or 2/1
	fmClk, ssgClk                       int // input clocks not yet used
	out                                 int // last FM sample (3 channels, each ±8191)

	fmSum   int64
	fmN     int
	SSG     *ay8910.PSG
	ssgPend int
}

// New makes a chip, reset.
func New() *Chip {
	y := &Chip{SSG: ay8910.New(true)}
	y.Reset()
	return y
}

// Reset puts the chip in its power-on state.
func (y *Chip) Reset() {
	s := y.SSG
	*y = Chip{SSG: s}
	for i := range y.att {
		y.att[i] = 0x3ff
		y.egState[i] = release
	}
	y.fmDiv, y.ssgDiv = 6, 4
	y.dirty = 7
	y.SSG.Reset()
}

// IRQ tells whether the chip's interrupt line is on (a timer flag is set).
func (y *Chip) IRQ() bool { return y.status&3 != 0 }

// Read reads port 0 (status) or 1 (the SSG register selected, if any).
func (y *Chip) Read(port int) uint8 {
	if port&1 == 0 {
		return y.status
	}
	if y.addr < 16 {
		return y.SSG.Read(int(y.addr))
	}
	return 0
}

// Write takes a byte written to port 0 (address) or 1 (data).
func (y *Chip) Write(port int, v uint8) {
	if port&1 == 0 {
		y.addr = v
		// the prescaler registers act on the address write
		switch v {
		case 0x2d:
			y.fmDiv, y.ssgDiv = 6, 4
		case 0x2e:
			if y.fmDiv == 6 {
				y.fmDiv, y.ssgDiv = 3, 2
			}
		case 0x2f:
			y.fmDiv, y.ssgDiv = 2, 1
		}
		return
	}
	r := int(y.addr)
	switch {
	case r < 0x10:
		y.SSG.Write(r, v)
		return
	case r < 0x30:
		y.writeGlobal(r, v)
		return
	}
	ch := r & 3
	if ch == 3 {
		return
	}
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
		y.fnum[ch] = uint16(y.latch&7)<<8 | uint16(v)
		y.block[ch] = (y.latch >> 3) & 7
		y.dirty |= 1 << ch
	case 0xa4:
		y.latch = v & 0x3f
	case 0xa8:
		y.c3fnum[ch] = uint16(y.c3latch&7)<<8 | uint16(v)
		y.c3block[ch] = (y.c3latch >> 3) & 7
		y.dirty |= 4
	case 0xac:
		y.c3latch = v & 0x3f
	case 0xb0:
		y.fb[ch], y.alg[ch] = (v>>3)&7, v&7
	}
}

func (y *Chip) writeGlobal(r int, v uint8) {
	switch r {
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
		for k := 0; k < 4; k++ {
			y.keyOnOff(ch*4+keySlot[k], (v>>(4+k))&1)
		}
	}
}

func (y *Chip) keyOnOff(s int, on uint8) {
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

func (y *Chip) rateFor(s, r int) int {
	if r == 0 {
		return 0
	}
	return min(r+int(y.kc[s]>>(3-y.ks[s])), 63)
}

// updateInc recomputes the phase increments of one channel.
func (y *Chip) updateInc(ch int) {
	special := ch == 2 && y.mode27&0xc0 != 0
	for k := 0; k < 4; k++ {
		s := ch*4 + k
		fn, bl := int(y.fnum[ch]), int(y.block[ch])
		if special && k != 3 {
			// operators S1, S3, S2 take their own frequencies (registers A9, AA, A8)
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
		base := (fn << bl) >> 1
		if d := y.dt[s]; d&3 != 0 {
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

func (y *Chip) egStep(s int) {
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
		att = min(att+inc, 0x3ff)
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

func (y *Chip) opOut(s, mod int) int {
	a := y.att[s]
	if y.ssg[s]&8 != 0 && y.egState[s] != release && (y.ssgInv[s]^((y.ssg[s]>>2)&1)) != 0 {
		a = (0x200 - a) & 0x3ff
	}
	a += int(y.tl[s]) << 3
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

// tick makes one FM sample.
func (y *Chip) tick() {
	if y.dirty != 0 {
		for c := 0; c < 3; c++ {
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
		for s := 0; s < 12; s++ {
			y.egStep(s)
		}
	}
	sum := 0
	for ch := 0; ch < 3; ch++ {
		b := ch * 4
		f0, f1 := int(y.fbOut[ch*2]), int(y.fbOut[ch*2+1])
		mod := 0
		if fbn := int(y.fb[ch]); fbn != 0 {
			mod = (f0 + f1) >> (10 - fbn)
		}
		s1 := y.opOut(b, mod)
		y.fbOut[ch*2], y.fbOut[ch*2+1] = int32(f1), int32(s1)
		S2, S3, S4 := b+2, b+1, b+3
		var outv int
		switch y.alg[ch] {
		case 0:
			s2 := y.opOut(S2, s1>>1)
			s3 := y.opOut(S3, s2>>1)
			outv = y.opOut(S4, s3>>1)
		case 1:
			s2 := y.opOut(S2, 0)
			s3 := y.opOut(S3, (s1+s2)>>1)
			outv = y.opOut(S4, s3>>1)
		case 2:
			s2 := y.opOut(S2, 0)
			s3 := y.opOut(S3, s2>>1)
			outv = y.opOut(S4, (s1+s3)>>1)
		case 3:
			s2 := y.opOut(S2, s1>>1)
			s3 := y.opOut(S3, 0)
			outv = y.opOut(S4, (s2+s3)>>1)
		case 4:
			s2 := y.opOut(S2, s1>>1)
			s3 := y.opOut(S3, 0)
			outv = s2 + y.opOut(S4, s3>>1)
		case 5:
			outv = y.opOut(S2, s1>>1) + y.opOut(S3, s1>>1) + y.opOut(S4, s1>>1)
		case 6:
			outv = y.opOut(S2, s1>>1) + y.opOut(S3, 0) + y.opOut(S4, 0)
		default:
			outv = s1 + y.opOut(S2, 0) + y.opOut(S3, 0) + y.opOut(S4, 0)
		}
		sum += max(-8192, min(8191, outv))
	}
	y.out = sum
	for s := 0; s < 12; s++ {
		y.phase[s] = (y.phase[s] + y.inc[s]) & 0xfffff
	}
	// timers count FM samples (timer B every 16)
	if y.mode27&1 != 0 {
		y.cntA++
		if y.cntA >= 1024 {
			y.cntA = y.timerA
			if y.mode27&4 != 0 {
				y.status |= 1
			}
			if y.mode27&0xc0 == 0x80 { // CSM: timer A keys channel 3 on
				for k := 0; k < 4; k++ {
					if s := 8 + k; y.key[s] == 0 {
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

// Run advances the chip by cycles input-clock cycles.
func (y *Chip) Run(cycles int) {
	y.fmClk += cycles
	per := y.fmDiv * 12
	for y.fmClk >= per {
		y.fmClk -= per
		y.tick()
		y.fmSum += int64(y.out)
		y.fmN++
	}
	y.ssgClk += cycles
	if n := y.ssgClk / y.ssgDiv; n > 0 {
		y.ssgClk -= n * y.ssgDiv
		y.SSG.Run(2 * n) // the SSG steps twice as often as an AY at the same clock
	}
}

// FMRate is the FM sample rate for an input clock with the current prescaler.
func (y *Chip) FMRate(clock float64) float64 { return clock / float64(y.fmDiv*12) }

// Take returns the average FM output (three channels summed, each -1..1)
// and SSG output (three channels summed, each 0..1) since the last Take.
func (y *Chip) Take() (fm, ssg float64) {
	if y.fmN == 0 {
		fm = float64(y.out) / 8192
	} else {
		fm = float64(y.fmSum) / float64(y.fmN) / 8192
		y.fmSum, y.fmN = 0, 0
	}
	return fm, y.SSG.Take()
}

// ---- save states ----

var errState = errors.New("ym2203: damaged save state")

const stateVersion = 1

// AppendState appends the chip's state to b.
func (y *Chip) AppendState(b []byte) []byte {
	b = append(b, stateVersion)
	for _, a := range [][]uint8{y.dt[:], y.mul[:], y.tl[:], y.ks[:], y.ar[:], y.amOn[:], y.d1r[:], y.d2r[:], y.sl[:], y.rr[:], y.ssg[:],
		y.kc[:], y.egState[:], y.key[:], y.ssgInv[:], y.block[:], y.fb[:], y.alg[:], y.c3block[:]} {
		b = append(b, a...)
	}
	for i := range y.phase {
		b = binary.LittleEndian.AppendUint32(b, y.phase[i])
		b = binary.LittleEndian.AppendUint32(b, y.inc[i])
		b = binary.LittleEndian.AppendUint16(b, uint16(y.att[i]))
	}
	for i := range y.fnum {
		b = binary.LittleEndian.AppendUint16(b, y.fnum[i])
		b = binary.LittleEndian.AppendUint16(b, y.c3fnum[i])
	}
	for _, v := range y.fbOut {
		b = binary.LittleEndian.AppendUint32(b, uint32(v))
	}
	b = append(b, y.latch, y.c3latch, y.addr, y.status)
	for _, v := range []int{y.dirty, y.timerA, y.timerB, y.cntA, y.cntB, y.cntBdiv, y.mode27, y.egDiv, y.egCnt,
		y.fmDiv, y.ssgDiv, y.fmClk, y.ssgClk, y.out} {
		b = binary.LittleEndian.AppendUint32(b, uint32(int32(v)))
	}
	return y.SSG.AppendState(b)
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (y *Chip) LoadState(b []byte) ([]byte, error) {
	n := len(y.AppendState(nil)) - ay8910.StateSize
	if len(b) < n || b[0] != stateVersion {
		return b, errState
	}
	t := *y
	o := 1
	for _, a := range [][]uint8{t.dt[:], t.mul[:], t.tl[:], t.ks[:], t.ar[:], t.amOn[:], t.d1r[:], t.d2r[:], t.sl[:], t.rr[:], t.ssg[:],
		t.kc[:], t.egState[:], t.key[:], t.ssgInv[:], t.block[:], t.fb[:], t.alg[:], t.c3block[:]} {
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
		t.c3fnum[i] = binary.LittleEndian.Uint16(b[o+2:])
		o += 4
	}
	for i := range t.fbOut {
		t.fbOut[i] = int32(binary.LittleEndian.Uint32(b[o:]))
		o += 4
	}
	t.latch, t.c3latch, t.addr, t.status = b[o], b[o+1], b[o+2], b[o+3]
	o += 4
	for _, p := range []*int{&t.dirty, &t.timerA, &t.timerB, &t.cntA, &t.cntB, &t.cntBdiv, &t.mode27, &t.egDiv, &t.egCnt,
		&t.fmDiv, &t.ssgDiv, &t.fmClk, &t.ssgClk, &t.out} {
		*p = int(int32(binary.LittleEndian.Uint32(b[o:])))
		o += 4
	}
	if !validState(&t) {
		return b, errState
	}
	rest, err := t.SSG.LoadState(b[o:])
	if err != nil {
		return b, err
	}
	t.dirty &= 7
	t.fmSum, t.fmN = 0, 0
	*y = t
	return rest, nil
}

// validState: values that index tables or divide stay in range.
func validState(t *Chip) bool {
	if t.egCnt < 0 || t.egCnt > 0xfff || t.egDiv < 0 || t.egDiv > 2 || t.fmClk < 0 || t.ssgClk < 0 || t.fmClk > 1000 || t.ssgClk > 1000 {
		return false
	}
	if !(t.fmDiv == 6 && t.ssgDiv == 4 || t.fmDiv == 3 && t.ssgDiv == 2 || t.fmDiv == 2 && t.ssgDiv == 1) {
		return false
	}
	for i := range t.att {
		if t.egState[i] > release || t.kc[i] > 31 || t.att[i] > 0x3ff || t.dt[i] > 7 || t.ks[i] > 3 {
			return false
		}
	}
	for i := range t.alg {
		if t.alg[i] > 7 || t.fb[i] > 7 || t.block[i] > 7 {
			return false
		}
	}
	return true
}
