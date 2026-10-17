package sega16

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/upd7759"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// The System 16B sound board: a Z80 with its program ROM at 0x0000-0x7fff, a
// 16 KB window on the sample ROMs at 0x8000, 2 KB of RAM at 0xf800, a YM2151
// and a uPD7759 in slave mode. The 68000's commands arrive through a latch
// (read at port 0xc0, or at 0xe800) that also raises the Z80's IRQ. The
// uPD7759's DRQ is the Z80's NMI: the NMI handler copies the next sample
// byte from the window to port 0x80.
//
// Ports: 0x00/0x01 YM2151 (address, data/status); 0x40 write: the window's
// bank in the low bits and the uPD7759's RESET (bit 7, low = reset);
// 0x80 write: uPD7759 data (a write while it is idle starts a sample);
// 0xc0 read: the command latch.

const (
	z80Hz = 5000000
	ymHz  = 4000000
	updHz = 640000
	// mix: one output sample per YM2151 sample (clock / 64), counted in
	// 20 MHz ticks: a Z80 cycle is 4 ticks, a YM2151 sample 320.
	ymTicks = 64 * 5
	// the YM2151's and the uPD7759's levels in the mono mix (the board's
	// mixing resistors as MAME has them: 0.43 per YM channel, 0.48 for the
	// uPD7759, whose full scale is half of the YM's), scaled to leave headroom
	fmGain  = 0.43 * 0.62
	updGain = 0.48 * 0.5 * 0.62
)

var updPerSample = float64(updHz) / (float64(ymHz) / 64)

type sound struct {
	z80  *z80.CPU
	ym   *ym2151.YM2151
	upd  *upd7759.UPD7759
	rom  []byte // Z80 program
	pcm  []byte // sample ROMs
	bank func(v uint8) int
	ram  [0x800]uint8
	out  *arcade.Resampler

	latch  uint8
	ctrl   uint8 // last port 0x40 write
	window int   // the sample window's offset in pcm
	mixClk int
	updAcc float64
	updV   float64
	dcPrev float64
	dcOut  float64
	zcyc   float64
}

type soundBus struct{ s *sound }

func newSound(rom, pcm []byte, bank func(v uint8) int, sampleRate int) *sound {
	s := &sound{rom: rom, pcm: pcm, bank: bank}
	s.ym = ym2151.New(ymHz)
	s.upd = upd7759.NewSlave(updHz)
	s.upd.OnDRQ = func(on bool) {
		if on {
			s.z80.NMI()
		}
	}
	s.z80 = z80.New(soundBus{s})
	s.out = arcade.NewResampler(float64(ymHz)/64, float64(sampleRate))
	return s
}

func (s *sound) reset() {
	s.ram = [0x800]uint8{}
	s.ym.Reset()
	s.upd.Reset()
	s.z80.Reset()
	s.z80.IRQ = false
	s.latch, s.ctrl, s.window = 0, 0, 0
	s.upd.ResetLine(false)
	s.mixClk, s.updAcc, s.updV, s.dcPrev, s.dcOut, s.zcyc = 0, 0, 0, 0, 0, 0
	s.out.Clear()
}

// command is the 68000 writing the latch.
func (s *sound) command(v uint8) {
	s.latch = v
	s.z80.IRQ = true
}

func (b soundBus) Read(a uint16) uint8 {
	s := b.s
	switch {
	case a < 0x8000:
		return s.rom[int(a)%len(s.rom)]
	case a < 0xe000:
		if len(s.pcm) == 0 {
			return 0xff
		}
		return s.pcm[(s.window+int(a-0x8000))%len(s.pcm)]
	case a >= 0xf800:
		return s.ram[a&0x7ff]
	case a&0xf800 == 0xe800:
		s.z80.IRQ = false
		return s.latch
	}
	return 0xff
}

func (b soundBus) Write(a uint16, v uint8) {
	if a >= 0xf800 {
		b.s.ram[a&0x7ff] = v
	}
}

func (b soundBus) In(port uint16) uint8 {
	s := b.s
	switch port & 0xc0 {
	case 0x00:
		if port&1 != 0 {
			return s.ym.Read()
		}
	case 0xc0:
		s.z80.IRQ = false
		return s.latch
	}
	return 0xff
}

func (b soundBus) Out(port uint16, v uint8) {
	s := b.s
	switch port & 0xc0 {
	case 0x00:
		s.ym.Write(int(port&1), v)
	case 0x40:
		s.ctrl = v
		s.window = s.bank(v)
		s.upd.ResetLine(v&0x80 != 0)
	case 0x80:
		s.upd.Port(v)
	}
}

func (b soundBus) Ack() {}

// runTo runs the Z80 (and the sound chips with it) to cycle target of the frame.
func (s *sound) runTo(target float64) {
	for s.zcyc < target {
		c := s.z80.Step()
		s.zcyc += float64(c)
		s.clocks(c)
	}
}

func (s *sound) clocks(c int) {
	s.mixClk += c * 4
	for s.mixClk >= ymTicks {
		s.mixClk -= ymTicks
		s.ym.Tick()
		s.updAcc += updPerSample
		n := math.Floor(s.updAcc)
		s.updAcc -= n
		if n != 0 {
			s.upd.Clocks(int(n))
			if s.upd.AccN != 0 {
				s.updV = s.upd.Acc / float64(s.upd.AccN)
				s.upd.Acc, s.upd.AccN = 0, 0
			}
		}
		// the uPD7759's output is AC coupled
		s.dcOut = s.updV - s.dcPrev + 0.9995*s.dcOut
		s.dcPrev = s.updV
		v := float64(s.ym.Out[0]+s.ym.Out[1])/32768*fmGain + s.dcOut*updGain
		if v > 0.8 {
			v = 0.8 + 0.2*math.Tanh((v-0.8)*5)
		} else if v < -0.8 {
			v = -0.8 - 0.2*math.Tanh((-v-0.8)*5)
		}
		s.out.Add(v)
	}
}

func (s *sound) appendState(w *arcade.StateW) {
	w.B = s.z80.AppendState(w.B)
	w.B = s.ym.AppendState(w.B)
	s.upd.AppendState(w)
	w.Bytes(s.ram[:])
	w.U8(s.latch)
	w.U8(s.ctrl)
	w.Int(s.window)
	w.Int(s.mixClk)
	w.F64s([]float64{s.updAcc, s.updV, s.dcPrev, s.dcOut, s.zcyc})
	s.out.AppendState(w)
}

func (s *sound) loadState(r *arcade.StateR) error {
	var err error
	if r.B, err = s.z80.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = s.ym.LoadState(r.B); err != nil {
		return err
	}
	s.upd.LoadState(r)
	r.Bytes(s.ram[:])
	s.latch = r.U8()
	s.ctrl = r.U8()
	s.window = r.Int()
	s.mixClk = r.Int()
	f := make([]float64, 5)
	r.F64s(f)
	s.updAcc, s.updV, s.dcPrev, s.dcOut, s.zcyc = f[0], f[1], f[2], f[3], f[4]
	s.out.LoadState(r)
	return r.Err
}
