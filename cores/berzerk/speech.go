package berzerk

import "retropixels/cores/arcade"

// speech is the board's side of the S14001A speech chip on port $44.
//
// A write with bit 6 set sets the chip's volume (bits 3-5) and speed
// (bits 0-2); any other write starts word number (bits 0-5). Reading the
// port gives bit 6 = the chip is idle; the game only sends a word then.
//
// The chip itself isn't emulated: its two ROMs (berzerk_r_vo_1c/2c) are
// missing from most sets and there's no recording of the real chip here to
// check an emulation against, so the robots stay silent. The chip always
// reports idle, which is what the game sees between words on a real board.
type speech struct {
	params uint8 // the last volume/speed setting
	word   uint8 // the last word asked for
}

func (s *speech) reset() { *s = speech{} }

func (s *speech) write(v uint8) {
	if v&0x40 != 0 {
		s.params = v & 0x3f
	} else {
		s.word = v & 0x3f
	}
}

// busyForTests makes the chip look busy forever (as MAME's does with
// blank speech ROMs), so a run can be compared with MAME's sound.
var busyForTests bool

func (s *speech) status() uint8 {
	if busyForTests {
		return 0
	}
	return 0x40
}

func (s *speech) save(w *arcade.StateW) {
	w.U8(s.params)
	w.U8(s.word)
}

func (s *speech) load(r *arcade.StateR) {
	s.params = r.U8()
	s.word = r.U8()
}
