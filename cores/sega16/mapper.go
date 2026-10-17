package sega16

// The 315-5195 memory mapper sits between the 68000 and everything else. The
// game programs it: eight regions (chip selects), each with a base address
// (A23-A16) and a size, at mapper registers 0x10-0x1f (two per region: size,
// then base). Accesses that hit no region reach the mapper's own 32 byte
// registers (at the odd bytes, mirrored everywhere else). Register 3 is the
// sound latch: writing it hands a command to the sound CPU.
//
// Which chip each region selects is fixed by the board; on System 16B:
// 0 ROM 0, 1 ROM 1, 2 ROM board extras (tile banking), 3 work RAM,
// 4 sprite RAM, 5 tile and text RAM, 6 palette RAM, 7 I/O.
type mapper struct {
	regs  [32]uint8
	table [256]int8 // A23-A16 -> region, or -1 for the mapper itself
	mask  [8]uint32 // each region's size - 1
}

// regionBytes: the sizes a region's size code selects.
var regionBytes = [4]uint32{0x10000, 0x20000, 0x80000, 0x200000}

func (m *mapper) reset() {
	m.regs = [32]uint8{}
	m.rebuild()
}

// rebuild works out the address table after a region register changed.
// When regions overlap, the lower-numbered one answers.
func (m *mapper) rebuild() {
	for i := range m.table {
		m.table[i] = -1
	}
	for r := 7; r >= 0; r-- {
		size := regionBytes[m.regs[0x10+2*r]&3]
		m.mask[r] = size - 1
		ignore := int(size>>16) - 1
		base := int(m.regs[0x11+2*r]) &^ ignore
		for hi := base; hi <= base|ignore; hi++ {
			m.table[hi] = int8(r)
		}
	}
}

// region: which region an address is in (-1: the mapper) and the offset in it.
func (m *mapper) region(a uint32) (int, uint32) {
	r := int(m.table[(a>>16)&0xff])
	if r < 0 {
		return -1, a
	}
	return r, a & m.mask[r]
}

// write sets a register; it returns true when it was the sound latch.
func (m *mapper) write(a uint32, v uint8) bool {
	r := (a >> 1) & 0x1f
	m.regs[r] = v
	if r >= 0x10 {
		m.rebuild()
	}
	return r == 3
}

func (m *mapper) read(a uint32) uint8 { return m.regs[(a>>1)&0x1f] }
