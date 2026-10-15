package snes

// Temporary stand-ins while the chips are ported one by one.

func newCx4(s *SNES) coproc                         { return nil }
func newSDD1(s *SNES) coproc                        { return nil }
func newSPC7110(s *SNES, o *chipOpts) coproc        { return nil }
func newOBC1(s *SNES) coproc                        { return nil }
func newSRTC(s *SNES, o *chipOpts) coproc           { return nil }
func newST018(s *SNES, o *chipOpts) (coproc, error) { return nil, errUnsupported("ST018") }
