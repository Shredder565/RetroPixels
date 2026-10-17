package berzerk

import "retropixels/cores/arcade"

// The sound board: a 6840 timer making three square waves, each with its
// own 3-bit volume, mixed together. The timers count either the E clock
// (3.579545 MHz / 4) or, on their external clock pins, the rising edges of
// a noise generator shifted at the E clock (so about a quarter of it, with
// jitter: that's how the board makes its rougher sounds). Measured on MAME's
// board as a black box: the output of a channel at volume v is a unipolar
// square wave of height v * 0.055/7, and the three add up.
//
// Port $46 holds four 6-bit registers picked by its top two bits: 0 is a
// control register, 1-3 are the channels' volumes (bits 0-2). Control bit 0
// feeds the clock pins with the noise sampled at each rising edge of timer
// 1's output instead of the noise itself (so timer 1 sets how fast the
// noise changes: the game sweeps it for its rumbles), and bit 1 mutes
// channel 1 (used with bit 0, when timer 1 only drives the others).

const (
	eClock   = 3579545.0 / 4
	chanGain = 0.055 / 7
)

type sound struct {
	ptm   ptm
	ctrl  uint8    // port $46 register 0
	vol   [3]uint8 // port $46 registers 1-3
	noise uint32   // the noise generator's shift register
	held  bool     // the noise as sampled by timer 1's output
	out1  bool     // timer 1's output last tick
	extB  bool     // the clock pins last tick
	frac  float64  // E-clock ticks owed (in CPU cycles, scaled)
	sp    *speech  // the speech chip, mixed in
	rs    *arcade.Resampler
}

func newSound(rate int, sp *speech) *sound {
	s := &sound{rs: arcade.NewResampler(eClock, float64(rate)), sp: sp}
	s.reset()
	return s
}

func (s *sound) reset() {
	s.ptm.reset()
	s.ctrl = 0
	s.vol = [3]uint8{}
	s.noise = 1
	s.held, s.out1, s.extB = false, false, false
	s.frac = 0
}

// write is a write to ports $40-$47 other than $44 (the speech chip).
func (s *sound) write(port int, v uint8) {
	if port == 6 {
		if sel := v >> 6; sel == 0 {
			s.ctrl = v & 0x3f
		} else {
			s.vol[sel-1] = v & 7
		}
		return
	}
	s.ptm.write(port, v)
}

// run makes the sound for n CPU cycles (2.5 MHz).
func (s *sound) run(n int) {
	s.frac += float64(n) * (eClock / cpuHz)
	for s.frac >= 1 {
		s.frac--
		s.tick()
	}
}

// tick is one E clock.
func (s *sound) tick() {
	// noise: a 17-bit maximal-length shift register
	n := s.noise
	bit := (n ^ n>>3) & 1
	s.noise = n>>1 | bit<<16
	nb := s.noise&1 != 0
	p := &s.ptm
	// the clock pins: the noise itself, or (control bit 0) the noise
	// sampled at each rising edge of timer 1's output
	ext := nb
	if s.ctrl&1 != 0 {
		if p.out[0] && !s.out1 {
			s.held = nb
		}
		ext = s.held
	}
	s.out1 = p.out[0]
	rise := ext && !s.extB
	s.extB = ext
	for t := 0; t < 3; t++ {
		if !p.external(t) || rise {
			p.clock(t)
		}
	}
	out := 0
	for t := 0; t < 3; t++ {
		if p.level(t) && !(t == 0 && s.ctrl&2 != 0) {
			out += int(s.vol[t])
		}
	}
	s.rs.Add(float64(out)*chanGain + s.sp.tick())
}

func (s *sound) save(w *arcade.StateW) {
	s.ptm.save(w)
	w.U8(s.ctrl)
	w.Bytes(s.vol[:])
	w.U32(s.noise)
	w.Bool(s.held)
	w.Bool(s.out1)
	w.Bool(s.extB)
	w.F64(s.frac)
	s.rs.AppendState(w)
}

func (s *sound) load(r *arcade.StateR) {
	s.ptm.load(r)
	s.ctrl = r.U8()
	r.Bytes(s.vol[:])
	s.noise = r.U32()
	s.held = r.Bool()
	s.out1 = r.Bool()
	s.extB = r.Bool()
	s.frac = r.F64()
	s.rs.LoadState(r)
}
