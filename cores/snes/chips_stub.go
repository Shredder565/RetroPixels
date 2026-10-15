package snes

// Temporary stand-ins while the chips are ported one by one.

const chipsStub = true

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

type sa1 struct{ snesIrq bool }

func newSA1(s *SNES) *sa1                                { return &sa1{} }
func (c *sa1) snesRead(bank, off int) int                { return -1 }
func (c *sa1) snesWrite(bank, off int, v uint8)          {}
func (c *sa1) snesIO(off int, write bool, v uint8) uint8 { return 0 }
func (c *sa1) catchUp(t int)                             {}
func (c *sa1) reset()                                    {}
func (c *sa1) state(s *sio)                              {}

type superFX struct{}

func newSuperFX(c *cart, s *SNES, sram []byte) *superFX { return &superFX{} }
func (g *superFX) regRead(off int) uint8                { return 0 }
func (g *superFX) regWrite(off int, v uint8)            {}
func (g *superFX) snesRead(bank, off int) int           { return -1 }
func (g *superFX) snesWrite(bank, off int, v uint8)     {}
func (g *superFX) run(t int)                            {}
func (g *superFX) irqOut() bool                         { return false }
func (g *superFX) getSRAM() []byte                      { return nil }
func (g *superFX) reset()                               {}
func (g *superFX) state(s *sio)                         {}
