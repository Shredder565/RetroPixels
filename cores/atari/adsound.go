package atari

import (
	"math"

	"retropixels/cores/arcade"
)

// Asteroids Deluxe's two discrete sounds, RetroPixels' own synthesis of
// what the circuits do: both are the board's noise generator, filtered.

// noiseStep advances a 17-bit noise generator.
func noiseStep(n uint32) uint32 {
	if n == 0 {
		n = 1
	}
	bit := (n ^ n>>3) & 1
	return n>>1 | bit<<16
}

// explosion: the game writes $3600 as an explosion fades: bits 2-5 are the
// loudness and bits 6-7 how fast the noise is clocked (the low numbers
// fastest), so a big explosion is a deep rumble and a small one a crackle.
type explosion struct {
	vol   float64 // target loudness 0..1
	cur   float64 // loudness now (the circuit's capacitor follows the target)
	rate  float64 // noise clocks per sample
	phase float64
	noise uint32
	out   float64 // the noise bit, filtered
	lp    float64
}

var boomClock = [4]float64{6000, 3000, 1500, 750} // Hz, by bits 6-7

func (e *explosion) set(v uint8) {
	e.vol = float64(v>>2&15) / 15
	e.rate = boomClock[v>>6] / adRate
}

func (e *explosion) next() float64 {
	e.phase += e.rate
	for e.phase >= 1 {
		e.phase--
		e.noise = noiseStep(e.noise)
	}
	bit := float64(e.noise&1)*2 - 1
	// two filters: the noise's edges rounded, then a gentle low pass
	e.out += (bit - e.out) * 0.12
	e.lp += (e.out - e.lp) * 0.05
	e.cur += (e.vol - e.cur) * 0.004
	return e.lp * e.cur * 0.3
}

func (e *explosion) save(w *arcade.StateW) {
	w.F64s([]float64{e.vol, e.cur, e.rate, e.phase, e.out, e.lp})
	w.U32(e.noise)
}

func (e *explosion) load(r *arcade.StateR) {
	f := make([]float64, 6)
	r.F64s(f)
	e.vol, e.cur, e.rate, e.phase, e.out, e.lp = f[0], f[1], f[2], f[3], f[4], f[5]
	e.noise = r.U32()
}

// thrust: while latch bit 3 is on, low-passed noise: the engine's rumble.
type thrust struct {
	on    bool
	env   float64
	phase float64
	noise uint32
	lp1   float64
	lp2   float64
	lp3   float64
	low   float64 // what the 50 Hz high pass takes away
}

var (
	thrustCut = 1 - math.Exp(-2*math.Pi*120/adRate) // a 120 Hz low pass
	thrustLow = 1 - math.Exp(-2*math.Pi*50/adRate)  // and a 50 Hz high pass
)

func (t *thrust) next() float64 {
	t.phase += 6000.0 / adRate
	for t.phase >= 1 {
		t.phase--
		t.noise = noiseStep(t.noise)
	}
	bit := float64(t.noise&1)*2 - 1
	t.lp1 += (bit - t.lp1) * thrustCut
	t.lp2 += (t.lp1 - t.lp2) * thrustCut
	t.lp3 += (t.lp2 - t.lp3) * thrustCut
	target := 0.0
	if t.on {
		target = 1
	}
	t.env += (target - t.env) * 0.0015
	t.low += (t.lp3 - t.low) * thrustLow
	return (t.lp3 - t.low) * t.env * 0.55
}

func (t *thrust) save(w *arcade.StateW) {
	w.Bool(t.on)
	w.F64s([]float64{t.env, t.phase, t.lp1, t.lp2, t.lp3, t.low})
	w.U32(t.noise)
}

func (t *thrust) load(r *arcade.StateR) {
	t.on = r.Bool()
	f := make([]float64, 6)
	r.F64s(f)
	t.env, t.phase, t.lp1, t.lp2, t.lp3, t.low = f[0], f[1], f[2], f[3], f[4], f[5]
	t.noise = r.U32()
}
