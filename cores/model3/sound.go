package model3

// The sound board (a 68000 with two SCSP sound chips) and the MPEG music
// board. The main board talks to it only through MIDI bytes.
type soundBoard struct {
	midi []byte
}

func newSoundBoard(r *ROMs, g *GameDef) *soundBoard { return &soundBoard{} }

func (s *soundBoard) reset()            { s.midi = s.midi[:0] }
func (s *soundBoard) writeMIDI(v uint8) { s.midi = append(s.midi, v) }
func (s *soundBoard) runFrame()         { s.midi = s.midi[:0] }
