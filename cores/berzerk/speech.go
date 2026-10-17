package berzerk

import (
	"retropixels/cores/arcade"
	"retropixels/cores/s14001a"
)

// speech is the board's S14001A speech chip on port $44.
//
// A write with bit 6 set sets the chip's volume (bits 3-5, 0 = off) and
// speed (bits 0-2); any other write starts word number (bits 0-5). Reading
// the port gives bit 6 = the chip is idle; the game sends the next word of
// a phrase from its NMI when it sees that.
//
// The speed sets the chip's clock: one sample every (16 - speed) * 64
// cycles of the 10 MHz crystal (9.8-17.4 kHz), which is how the robots get
// their different voices. The volume scales the output linearly. Both
// measured on MAME's Berzerk board as a black box.
//
// The chip needs its two speech ROMs (berzerk_r_vo_1c/2c). Many Berzerk
// sets (the user's too) don't have them: then there's no chip and the port
// always reads idle, as if the speech board had nothing to say, so the
// game runs through its phrases at once and plays on silently.
type speech struct {
	chip   *s14001a.Chip // nil without the speech ROMs
	params uint8         // the last volume/speed setting
	word   uint8         // the last word asked for
	frac   float64       // speech samples owed, in E clocks
	level  int           // the chip's DAC level
}

// speechHz / (16 - speed) is the chip's sample rate.
const speechHz = 10e6 / 64

func newSpeech(rom []byte) *speech {
	s := &speech{}
	if rom != nil {
		s.chip = s14001a.New(rom)
	}
	s.reset()
	return s
}

func (s *speech) reset() {
	s.params, s.word, s.frac, s.level = 0, 0, 0, 7
	if s.chip != nil {
		s.chip.Reset()
	}
}

func (s *speech) write(v uint8) {
	if v&0x40 != 0 {
		s.params = v & 0x3f
		return
	}
	s.word = v & 0x3f
	if s.chip != nil {
		s.chip.Start(int(s.word))
	}
}

// busyForTests makes the chip look busy forever (as MAME's does with
// blank speech ROMs), so a run can be compared with MAME's sound.
var busyForTests bool

func (s *speech) status() uint8 {
	if busyForTests || (s.chip != nil && s.chip.Busy()) {
		return 0
	}
	return 0x40
}

// tick runs the chip for one E clock and returns its output.
func (s *speech) tick() float64 {
	if s.chip == nil {
		return 0
	}
	s.frac += speechHz / float64(16-int(s.params&7))
	for s.frac >= eClock {
		s.frac -= eClock
		s.level = s.chip.Next()
	}
	// DAC level 15 at volume 7 is half of full scale in MAME's mix
	return float64(s.level-7) / 16 * float64(s.params>>3&7) / 7
}

func (s *speech) save(w *arcade.StateW) {
	w.U8(s.params)
	w.U8(s.word)
	if s.chip == nil {
		return
	}
	w.F64(s.frac)
	w.Int(s.level)
	s.chip.Save(w)
}

func (s *speech) load(r *arcade.StateR) {
	s.params = r.U8()
	s.word = r.U8()
	if s.chip == nil {
		return
	}
	s.frac = r.F64()
	s.level = r.Int()
	s.chip.Load(r)
}
