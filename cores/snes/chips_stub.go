package snes

// Temporary stand-ins while the chips are ported one by one.

func newDSPn(s *SNES, o *chipOpts, chip string, b0, b1 int) (coproc, error) {
	return nil, errUnsupported(chip)
}
func newST01x(s *SNES, o *chipOpts, chip string) (coproc, error) { return nil, errUnsupported(chip) }
func newDSPChip(s *SNES, kind int, o *chipOpts) (coproc, error) {
	if kind == 2 {
		return nil, errUnsupported("DSP-2")
	}
	return nil, errUnsupported("DSP-1")
}
func newCx4(s *SNES) coproc                         { return nil }
func newSDD1(s *SNES) coproc                        { return nil }
func newSPC7110(s *SNES, o *chipOpts) coproc        { return nil }
func newOBC1(s *SNES) coproc                        { return nil }
func newSRTC(s *SNES, o *chipOpts) coproc           { return nil }
func newST018(s *SNES, o *chipOpts) (coproc, error) { return nil, errUnsupported("ST018") }
