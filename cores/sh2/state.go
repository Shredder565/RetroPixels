package sh2

import (
	"encoding/binary"
	"errors"
)

const stateVersion = 1

// ErrState is returned for a damaged state.
var ErrState = errors.New("sh2: bad state")

// AppendState appends the CPU's state (registers and on-chip modules; not
// the mapped memory) to b.
func (c *CPU) AppendState(b []byte) []byte {
	be := binary.BigEndian
	b = append(b, stateVersion)
	for _, r := range c.R {
		b = be.AppendUint32(b, r)
	}
	for _, r := range []uint32{c.PC, c.SR, c.GBR, c.VBR, c.MACH, c.MACL, c.PR} {
		b = be.AppendUint32(b, r)
	}
	b = be.AppendUint64(b, uint64(c.Cycles))
	b = be.AppendUint32(b, uint32(c.irl))
	b = append(b, bool8(c.sleeping))
	o := &c.on
	b = append(b, o.regs[:]...)
	b = be.AppendUint16(b, o.icr)
	b = be.AppendUint16(b, o.frc)
	b = be.AppendUint64(b, uint64(o.frcAt))
	b = be.AppendUint16(b, o.ocra)
	b = be.AppendUint16(b, o.ocrb)
	b = append(b, o.ftcsr, o.frcTemp, o.ocrTemp, o.wtcnt, o.wtcsr, o.rstcsr)
	b = be.AppendUint64(b, uint64(o.wtAt))
	for _, p := range o.pendSrc {
		b = append(b, bool8(p))
	}
	b = append(b, o.cacheRAM[:]...)
	return b
}

func bool8(v bool) uint8 {
	if v {
		return 1
	}
	return 0
}

// LoadState reads what AppendState wrote and returns the rest of b.
func (c *CPU) LoadState(b []byte) ([]byte, error) {
	const size = 1 + 16*4 + 7*4 + 8 + 4 + 1 + 0x200 + 2 + 2 + 8 + 2 + 2 + 6 + 8 + nSources + 0x1000
	if len(b) < size || b[0] != stateVersion {
		return b, ErrState
	}
	be := binary.BigEndian
	p := 1
	u32 := func() uint32 { v := be.Uint32(b[p:]); p += 4; return v }
	u16 := func() uint16 { v := be.Uint16(b[p:]); p += 2; return v }
	u64 := func() uint64 { v := be.Uint64(b[p:]); p += 8; return v }
	for i := range c.R {
		c.R[i] = u32()
	}
	c.PC, c.SR, c.GBR, c.VBR, c.MACH, c.MACL, c.PR = u32(), u32(), u32(), u32(), u32(), u32(), u32()
	c.Cycles = int64(u64())
	c.irl = int(u32())
	c.sleeping = b[p] != 0
	p++
	o := &c.on
	p += copy(o.regs[:], b[p:])
	o.icr = u16()
	o.frc = u16()
	o.frcAt = int64(u64())
	o.ocra = u16()
	o.ocrb = u16()
	o.ftcsr, o.frcTemp, o.ocrTemp, o.wtcnt, o.wtcsr, o.rstcsr = b[p], b[p+1], b[p+2], b[p+3], b[p+4], b[p+5]
	p += 6
	o.wtAt = int64(u64())
	for i := range o.pendSrc {
		o.pendSrc[i] = b[p] != 0
		p++
	}
	p += copy(o.cacheRAM[:], b[p:])
	c.inSlot, c.slotFault = false, false
	o.schedule()
	c.irqCheck = true
	return b[p:], nil
}
