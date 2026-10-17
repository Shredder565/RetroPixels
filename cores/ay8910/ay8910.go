// Package ay8910 is RetroPixels' General Instrument AY-3-8910 programmable
// sound generator (PSG), also the SSG half of Yamaha's YM2203 / YM2149
// (New with YM set): three square-wave tone channels, one noise generator,
// one envelope generator, a mixer and two 8-bit I/O ports.
//
//	psg := ay8910.New(false)
//	psg.WriteAddr(r); psg.WriteData(v) // or Write(r, v)
//	psg.Run(clocks)                    // advance by input-clock cycles
//	v := psg.Take()                    // average output since the last Take (0..3)
//
// Timing: the chip divides its input clock by 8 for its internal step; a
// tone channel flips every period steps (so its frequency is clock / (16 *
// period)), the noise shift register moves every 2 * period steps and the
// envelope takes one of its 16 levels (32 on YM parts) every 2 * period
// steps (1 * period on YM parts).
package ay8910

import (
	"encoding/binary"
	"errors"
	"math"
)

// Output levels of one channel for volumes 0-15 (AY-3-8910), as fractions of
// the loudest; about 3 dB apart, from measurements of the real chip.
var ayLevels = [16]float64{0, 0.0137, 0.0205, 0.0291, 0.0423, 0.0618, 0.0847, 0.1369,
	0.1691, 0.2647, 0.3527, 0.4499, 0.5704, 0.6873, 0.8482, 1}

// ymLevels are the YM parts' 32 envelope levels, 1.5 dB apart.
var ymLevels [32]float64

func init() {
	for i := 1; i < 32; i++ {
		ymLevels[i] = math.Pow(10, float64(i-31)*1.5/20)
	}
}

// register masks: unused bits read back as 0
var regMask = [16]uint8{0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff}

// PSG is one chip.
type PSG struct {
	ym   bool // YM2149-style 32-step envelope and volume curve
	regs [16]uint8
	addr uint8

	toneCnt  [3]int
	toneOut  [3]uint8
	noiseCnt int
	noiseDiv uint8 // the noise and AY envelope step on every other tick
	rng      uint32
	envCnt   int
	envPos   int // 0..envSteps-1
	envAtt   bool
	envHold  bool
	frac     int // input clocks not yet used (< 8)

	sum float64
	n   int

	// PortIn, when set, gives the value read from port A (0) or B (1) when
	// it is an input; otherwise inputs read 0xff.
	PortIn func(port int) uint8
	// PortOut, when set, is told when the game writes a port.
	PortOut func(port int, v uint8)
}

// New makes a chip, reset. ym picks the YM2149 / YM2203 SSG envelope (32
// steps) and volume curve.
func New(ym bool) *PSG {
	p := &PSG{ym: ym}
	p.Reset()
	return p
}

// Reset clears the registers (all channels silent).
func (p *PSG) Reset() {
	in, out, ym := p.PortIn, p.PortOut, p.ym
	*p = PSG{ym: ym, PortIn: in, PortOut: out}
	p.rng = 1
}

func (p *PSG) envSteps() int {
	if p.ym {
		return 32
	}
	return 16
}

// WriteAddr selects a register.
func (p *PSG) WriteAddr(v uint8) { p.addr = v }

// WriteData writes the selected register.
func (p *PSG) WriteData(v uint8) { p.Write(int(p.addr), v) }

// ReadData reads the selected register.
func (p *PSG) ReadData() uint8 { return p.Read(int(p.addr)) }

// Write writes register r (0-15; others are ignored).
func (p *PSG) Write(r int, v uint8) {
	if r < 0 || r > 15 {
		return
	}
	p.regs[r] = v & regMask[r]
	switch r {
	case 13:
		// a new envelope shape restarts the envelope
		p.envCnt, p.envPos, p.envHold = 0, 0, false
		p.envAtt = v&4 != 0
	case 14, 15:
		if p.PortOut != nil && p.regs[7]&(0x40<<(r-14)) != 0 {
			p.PortOut(r-14, v)
		}
	}
}

// Read reads register r.
func (p *PSG) Read(r int) uint8 {
	if r < 0 || r > 15 {
		return 0xff
	}
	if r >= 14 && p.regs[7]&(0x40<<(r-14)) == 0 {
		if p.PortIn != nil {
			return p.PortIn(r - 14)
		}
		return 0xff
	}
	return p.regs[r]
}

func (p *PSG) tonePeriod(ch int) int {
	v := int(p.regs[ch*2]) | int(p.regs[ch*2+1])<<8
	return max(v, 1)
}

// envLevel is the envelope's output level, 0..envSteps-1.
func (p *PSG) envLevel() int {
	if p.envAtt {
		return p.envPos
	}
	return p.envSteps() - 1 - p.envPos
}

// step is one internal step (8 input clocks).
func (p *PSG) step() {
	for ch := 0; ch < 3; ch++ {
		p.toneCnt[ch]++
		if p.toneCnt[ch] >= p.tonePeriod(ch) {
			p.toneCnt[ch] = 0
			p.toneOut[ch] ^= 1
		}
	}
	p.noiseDiv ^= 1
	if p.noiseDiv == 0 {
		p.noiseCnt++
		if p.noiseCnt >= max(int(p.regs[6]), 1) {
			p.noiseCnt = 0
			// 17-bit shift register, taps at bits 0 and 3
			bit := (p.rng ^ p.rng>>3) & 1
			p.rng = p.rng>>1 | bit<<16
		}
	}
	if p.ym || p.noiseDiv == 0 {
		p.envCnt++
		if p.envCnt >= max(int(p.regs[11])|int(p.regs[12])<<8, 1) {
			p.envCnt = 0
			p.envStep()
		}
	}
	p.sum += p.output()
	p.n++
}

func (p *PSG) envStep() {
	if p.envHold {
		return
	}
	p.envPos++
	if p.envPos < p.envSteps() {
		return
	}
	shape := p.regs[13]
	switch {
	case shape&8 == 0: // no continue: drop to 0 and stay
		p.envPos, p.envAtt, p.envHold = p.envSteps()-1, false, true
	case shape&1 != 0: // hold: at the ramp's end, or the other end if alternate
		p.envPos, p.envAtt, p.envHold = p.envSteps()-1, p.envAtt != (shape&2 != 0), true
	default:
		p.envPos = 0
		if shape&2 != 0 {
			p.envAtt = !p.envAtt
		}
	}
}

// Level returns channel ch's current output (0..1).
func (p *PSG) Level(ch int) float64 {
	en := p.regs[7]
	tone := p.toneOut[ch] | (en >> ch & 1)
	noise := uint8(p.rng&1) | (en >> (ch + 3) & 1)
	if tone&noise == 0 {
		return 0
	}
	vol := p.regs[8+ch]
	if vol&0x10 != 0 {
		l := p.envLevel()
		if p.ym {
			return ymLevels[l]
		}
		return ayLevels[l]
	}
	if p.ym {
		if vol&15 == 0 {
			return 0
		}
		return ymLevels[int(vol&15)*2+1]
	}
	return ayLevels[vol&15]
}

func (p *PSG) output() float64 { return p.Level(0) + p.Level(1) + p.Level(2) }

// Run advances the chip by clocks input-clock cycles.
func (p *PSG) Run(clocks int) {
	p.frac += clocks
	for p.frac >= 8 {
		p.frac -= 8
		p.step()
	}
}

// Take returns the average output (0..3, the three channels summed) since
// the last Take; with no step since then, the current output.
func (p *PSG) Take() float64 {
	if p.n == 0 {
		return p.output()
	}
	v := p.sum / float64(p.n)
	p.sum, p.n = 0, 0
	return v
}

// Regs returns the 16 registers.
func (p *PSG) Regs() [16]uint8 { return p.regs }

// ---- save states ----

var errState = errors.New("ay8910: damaged save state")

const stateVersion = 1

// StateSize is the length of a saved chip state.
const StateSize = 1 + 16 + 1 + 3*4 + 3 + 4 + 1 + 4 + 4 + 4 + 1 + 1

// AppendState appends the chip's state to b.
func (p *PSG) AppendState(b []byte) []byte {
	b = append(b, stateVersion)
	b = append(b, p.regs[:]...)
	b = append(b, p.addr)
	for _, c := range p.toneCnt {
		b = binary.LittleEndian.AppendUint32(b, uint32(c))
	}
	b = append(b, p.toneOut[:]...)
	b = binary.LittleEndian.AppendUint32(b, uint32(p.noiseCnt))
	b = append(b, p.noiseDiv)
	b = binary.LittleEndian.AppendUint32(b, p.rng)
	b = binary.LittleEndian.AppendUint32(b, uint32(p.envCnt))
	b = binary.LittleEndian.AppendUint32(b, uint32(p.envPos))
	var fl uint8
	if p.envAtt {
		fl |= 1
	}
	if p.envHold {
		fl |= 2
	}
	return append(b, fl, uint8(p.frac))
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest of b.
func (p *PSG) LoadState(b []byte) ([]byte, error) {
	if len(b) < StateSize || b[0] != stateVersion {
		return b, errState
	}
	o := 1
	copy(p.regs[:], b[o:])
	o += 16
	p.addr = b[o]
	o++
	for i := range p.toneCnt {
		p.toneCnt[i] = int(binary.LittleEndian.Uint32(b[o:]))
		o += 4
	}
	copy(p.toneOut[:], b[o:o+3])
	o += 3
	p.noiseCnt = int(binary.LittleEndian.Uint32(b[o:]))
	o += 4
	p.noiseDiv = b[o] & 1
	o++
	p.rng = binary.LittleEndian.Uint32(b[o:]) & 0x1ffff
	o += 4
	p.envCnt = int(binary.LittleEndian.Uint32(b[o:]))
	o += 4
	p.envPos = int(binary.LittleEndian.Uint32(b[o:]))
	o += 4
	fl := b[o]
	p.envAtt, p.envHold = fl&1 != 0, fl&2 != 0
	p.frac = int(b[o+1] & 7)
	o += 2
	if p.envPos < 0 || p.envPos >= p.envSteps() || p.envCnt < 0 || p.envCnt > 0xffff || p.noiseCnt < 0 || p.noiseCnt > 0x1f {
		return b, errState
	}
	for i := range p.toneCnt {
		p.toneCnt[i] &= 0xfff
		p.toneOut[i] &= 1
	}
	if p.rng == 0 {
		p.rng = 1
	}
	p.sum, p.n = 0, 0
	return b[o:], nil
}
