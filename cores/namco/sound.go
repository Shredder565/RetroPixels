package namco

import (
	"math"

	"retropixels/cores/arcade"
)

// wsgHz is the wavetable chip's sample rate: 3.072 MHz / 32.
const wsgHz = cpuHz / 32

// wsg is Namco's 3-voice wavetable sound (the Pac-Man chip), written by the
// sound processor at $6800-$681F, one nibble per register:
//
//	$05, $0A, $0F   waveform of voices 0, 1, 2 (3 bits: one of 8 in the PROM)
//	$10-$14         voice 0 frequency (20 bits, low nibble first)
//	$16-$19         voice 1 frequency (bits 4-19)
//	$1B-$1E         voice 2 frequency (bits 4-19)
//	$15, $1A, $1F   volumes
//
// Each voice adds its frequency to a 20-bit counter at 96 kHz and plays
// sample counter>>15 of its 32-sample waveform. ($00-$04, $06-$09 and
// $0B-$0E are the counters on the original chip; the games write zeros
// there, which changes nothing that can be heard.)
type wsg struct {
	reg   [32]uint8
	acc   [3]uint32
	waves [256]uint8
}

func (s *wsg) reset() {
	s.reg = [32]uint8{}
	s.acc = [3]uint32{}
}

// setWaves takes the waveform PROM (only its low 4 bits count: some dumps
// have the top bits set). Without it, the board uses waveforms of its own
// that are close to the PROM's first ones (sine, square, triangle...).
func (s *wsg) setWaves(prom []uint8, ok bool) {
	if ok {
		for i := range s.waves {
			s.waves[i] = prom[i] & 15
		}
		return
	}
	for w := 0; w < 8; w++ {
		for i := 0; i < 32; i++ {
			ph := float64(i) / 32
			var v float64
			switch w {
			case 0: // sine
				v = math.Sin(2 * math.Pi * ph)
			case 1: // sine with a buzz
				v = math.Sin(2*math.Pi*ph) * 0.6
				if i&1 != 0 {
					v = 0
				}
			case 2: // square
				v = 1
				if ph >= 0.5 {
					v = -1
				}
			case 3: // two sines
				v = 0.6*math.Sin(2*math.Pi*ph) + 0.4*math.Sin(4*math.Pi*ph)
			case 4: // sine and its third harmonic
				v = 0.7*math.Sin(2*math.Pi*ph) + 0.3*math.Sin(6*math.Pi*ph)
			case 5: // triangle
				v = 1 - 4*math.Abs(ph-0.5)
			case 6: // sawtooth
				v = 2*ph - 1
			default: // pulse
				v = -0.6
				if ph < 0.25 {
					v = 1
				}
			}
			s.waves[w*32+i] = uint8(math.Round(7 + v*7))
		}
	}
}

func (s *wsg) write(r int, v uint8) { s.reg[r] = v & 15 }

// sample returns the next 96 kHz sample (about -1 to 1).
func (s *wsg) sample() float64 {
	r := &s.reg
	mix := 0
	for v := 0; v < 3; v++ {
		var f uint32
		var vol, sel int
		switch v {
		case 0:
			f = uint32(r[0x10]) | uint32(r[0x11])<<4 | uint32(r[0x12])<<8 | uint32(r[0x13])<<12 | uint32(r[0x14])<<16
			vol, sel = int(r[0x15]), int(r[0x05]&7)
		case 1:
			f = uint32(r[0x16])<<4 | uint32(r[0x17])<<8 | uint32(r[0x18])<<12 | uint32(r[0x19])<<16
			vol, sel = int(r[0x1a]), int(r[0x0a]&7)
		default:
			f = uint32(r[0x1b])<<4 | uint32(r[0x1c])<<8 | uint32(r[0x1d])<<12 | uint32(r[0x1e])<<16
			vol, sel = int(r[0x1f]), int(r[0x0f]&7)
		}
		s.acc[v] = (s.acc[v] + f) & 0xfffff
		if vol == 0 {
			continue
		}
		mix += (int(s.waves[sel<<5|int(s.acc[v]>>15)]) - 8) * vol
	}
	return float64(mix) * wsgGain
}

// wsgGain: one voice at full volume swings about ±0.18, as loud as MAME's
// recording of the boards.
const wsgGain = 1.0 / 668

func (s *wsg) save(w *arcade.StateW) {
	w.Bytes(s.reg[:])
	w.U32s(s.acc[:])
}

func (s *wsg) load(r *arcade.StateR) {
	r.Bytes(s.reg[:])
	r.U32s(s.acc[:])
}

// ---------------------------------------------------------------- 54xx

// n54xx is Galaga's explosion noise. The real chip is a microcontroller that
// shapes noise into an analog filter network; MAME can't run it without its
// program either, so these are RetroPixels' own noises: A is the short
// bright burst of an enemy blowing up, B the deeper and longer one that
// goes with it, C (unused by Galaga) a middle one. Each is filtered noise
// with a falling level.
type n54xx struct {
	params    [3][5]uint8
	cmd, args int
	env       [3]float64
	lp        [3][2]float64
	lfsr      uint32
	volC      float64
	decay     [3]float64
	cut       [3]float64
	clock     int
}

func (n *n54xx) init(rate float64) {
	// level falls to 1/e in: A 0.12 s, B 0.30 s, C 0.20 s
	for i, t := range []float64{0.12, 0.30, 0.20} {
		n.decay[i] = math.Exp(-1 / (t * rate))
	}
	for i, f := range []float64{2500, 900, 1500} {
		n.cut[i] = 1 - math.Exp(-2*math.Pi*f/rate)
	}
}

func (n *n54xx) reset() {
	n.params = [3][5]uint8{}
	n.cmd, n.args = 0, 0
	n.env = [3]float64{}
	n.lp = [3][2]float64{}
	n.lfsr = 1
	n.volC = 1
	n.clock = 0
}

func (n *n54xx) write(v uint8) {
	if n.args > 0 {
		k := (n.cmd >> 4) - 3
		if n.cmd>>4 == 6 {
			k = 2
		}
		size := 4
		if k == 2 {
			size = 5
		}
		n.params[k][size-n.args] = v
		n.args--
		return
	}
	n.cmd = int(v)
	switch v >> 4 {
	case 1:
		n.env[0] = 1
	case 2:
		n.env[1] = 1
	case 3, 4:
		n.args = 4
	case 5:
		n.env[2] = 1
	case 6:
		n.args = 5
	case 7:
		n.volC = float64(v&15) / 15
	}
}

// sample returns the next 96 kHz sample of the noises.
func (n *n54xx) sample() float64 {
	if n.env[0] < 1e-4 && n.env[1] < 1e-4 && n.env[2] < 1e-4 {
		return 0
	}
	// the noise changes at 24 kHz
	n.clock++
	if n.clock&3 == 0 {
		bit := (n.lfsr ^ n.lfsr>>3) & 1
		n.lfsr = n.lfsr>>1 | bit<<16
	}
	x := float64(n.lfsr&1)*2 - 1
	out := 0.0
	for i := range n.env {
		if n.env[i] < 1e-4 {
			n.env[i] = 0
			continue
		}
		// two one-pole low-pass stages
		n.lp[i][0] += (x - n.lp[i][0]) * n.cut[i]
		n.lp[i][1] += (n.lp[i][0] - n.lp[i][1]) * n.cut[i]
		g := n.env[i]
		if i == 2 {
			g *= n.volC
		}
		out += n.lp[i][1] * g
		n.env[i] *= n.decay[i]
	}
	return out * 0.35
}

func (n *n54xx) save(w *arcade.StateW) {
	for i := range n.params {
		w.Bytes(n.params[i][:])
	}
	w.Ints([]int{n.cmd, n.args, n.clock})
	w.F64s(n.env[:])
	for i := range n.lp {
		w.F64s(n.lp[i][:])
	}
	w.U32(n.lfsr)
	w.F64(n.volC)
}

func (n *n54xx) load(r *arcade.StateR) {
	for i := range n.params {
		r.Bytes(n.params[i][:])
	}
	v := make([]int, 3)
	r.Ints(v)
	n.cmd, n.args, n.clock = v[0], v[1], v[2]
	r.F64s(n.env[:])
	for i := range n.lp {
		r.F64s(n.lp[i][:])
	}
	n.lfsr = r.U32()
	n.volC = r.F64()
}
