package arm7

import (
	"encoding/binary"
	"errors"
)

// Saving and restoring the processor (what arm7.js's saveState / loadState
// keep: registers, banks, status, PC, cycle count, pipeline).

// stateWords is how many 32-bit words a saved processor takes.
const stateWords = 16 + 6*3 + 5*2 + 1 + 1 + 2 + 2 + 2

// StateSize is the length of what AppendState adds.
const StateSize = stateWords * 4

// ErrState is returned by LoadStateBytes for data that isn't a saved processor.
var ErrState = errors.New("arm7: bad saved state")

func (c *CPU) words(f func(p *uint32)) {
	for i := range c.R {
		f(&c.R[i])
	}
	for i := range 6 {
		f(&c.bank13[i])
		f(&c.bank14[i])
		f(&c.spsrBank[i])
	}
	for i := range 5 {
		f(&c.fiqR[i])
		f(&c.usrR[i])
	}
	f(&c.PC)
	f(&c.Mode)
	f(&c.pipe0)
	f(&c.pipe1)
}

// AppendState appends the processor's state to b.
func (c *CPU) AppendState(b []byte) []byte {
	c.words(func(p *uint32) { b = binary.LittleEndian.AppendUint32(b, *p) })
	b = binary.LittleEndian.AppendUint64(b, uint64(c.Cycles))
	b = binary.LittleEndian.AppendUint32(b, c.CPSR())
	var flags uint32
	if c.Halted {
		flags |= 1
	}
	if c.IRQLine {
		flags |= 2
	}
	return binary.LittleEndian.AppendUint32(b, flags)
}

// LoadStateBytes restores a state AppendState made (StateSize bytes).
func (c *CPU) LoadStateBytes(b []byte) error {
	if len(b) < StateSize {
		return ErrState
	}
	c.words(func(p *uint32) { *p = binary.LittleEndian.Uint32(b); b = b[4:] })
	c.Cycles = int64(binary.LittleEndian.Uint64(b))
	v := binary.LittleEndian.Uint32(b[8:])
	c.N, c.Z, c.C, c.V = v>>31&1, v>>30&1, v>>29&1, v>>28&1
	c.I, c.F, c.T = v>>7&1, v>>6&1, v>>5&1
	c.Mode = v & 0x1f
	flags := binary.LittleEndian.Uint32(b[12:])
	c.Halted, c.IRQLine = flags&1 != 0, flags&2 != 0
	return nil
}
