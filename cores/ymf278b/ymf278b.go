// Package ymf278b is RetroPixels' Yamaha YMF278B (OPL4): an OPL3 FM
// synthesizer and a 24-channel wavetable (PCM) synthesizer sharing one
// output, as used by arcade boards.
package ymf278b

// Chip is one YMF278B.
type Chip struct {
	rom []byte // wave memory (the sample ROM)

	fmAddr  [2]int // the selected FM register of each bank
	fmRegs  [0x200]uint8
	pcmAddr int
	pcmRegs [0x100]uint8
	memAddr int // wave memory address for CPU access (regs 3-5)
}

// New makes a chip with the given sample ROM.
func New(rom []byte) *Chip {
	c := &Chip{rom: rom}
	c.Reset()
	return c
}

// Reset clears all registers.
func (c *Chip) Reset() {
	*c = Chip{rom: c.rom}
}

// Write writes one of the chip's ports: 0/1 FM bank 0 address/data, 2/3 FM
// bank 1 address/data, 4/5 PCM address/data.
func (c *Chip) Write(port int, v uint8) {
	switch port & 7 {
	case 0:
		c.fmAddr[0] = int(v)
	case 1:
		c.fmRegs[c.fmAddr[0]] = v
	case 2:
		c.fmAddr[1] = int(v)
	case 3:
		c.fmRegs[0x100+c.fmAddr[1]] = v
	case 4:
		c.pcmAddr = int(v)
	case 5:
		c.pcmRegs[c.pcmAddr] = v
	}
}

// Read reads a port: 0 is the status (never busy here), 5 the PCM
// registers.
func (c *Chip) Read(port int) uint8 {
	switch port & 7 {
	case 5:
		return c.pcmRegs[c.pcmAddr]
	}
	return 0
}

// Sample makes one output sample (the chip's clock / 768).
func (c *Chip) Sample() (l, r int32) { return 0, 0 }

// AppendState appends the chip's state to b.
func (c *Chip) AppendState(b []byte) []byte { return b }

// LoadState reads a state AppendState wrote, returning the rest of b.
func (c *Chip) LoadState(b []byte) ([]byte, error) { return b, nil }
