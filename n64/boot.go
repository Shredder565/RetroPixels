package n64

import (
	"encoding/binary"
	"hash/crc32"
)

// RetroPixels starts games itself instead of running the console's boot ROM:
// it sets up memory and registers the way the boot process leaves them and
// jumps to the game's entry point.

// CIC chips, identified by the CRC32 of the boot code in the ROM header area.
const (
	cic6101 = 6101
	cic6102 = 6102
	cic6103 = 6103
	cic6105 = 6105
	cic6106 = 6106
)

var cicByCRC = map[uint32]int{
	0x6170a4a1: cic6101, 0x009e9ea3: cic6101,
	0x90bb6cb5: cic6102,
	0x0b050ee0: cic6103,
	0x98bc2c86: cic6105,
	0xacc8580a: cic6106,
}

var cicSeed = map[int]uint64{cic6101: 0x3f, cic6102: 0x3f, cic6103: 0x78, cic6105: 0x91, cic6106: 0x85}

func detectCIC(rom []byte) int {
	sum := crc32.ChecksumIEEE(rom[0x40:0x1000])
	if c, ok := cicByCRC[sum]; ok {
		return c
	}
	return cic6102
}

func (m *Machine) hleBoot() {
	c := &m.CPU
	m.cic = detectCIC(m.rom)
	// the first 4 KB of the cartridge sit in DMEM after booting
	copy(m.spmem[:0x1000], m.rom[:0x1000])
	entry := binary.BigEndian.Uint32(m.rom[8:])
	switch m.cic {
	case cic6103:
		entry -= 0x100000
	case cic6106:
		entry -= 0x200000
	}
	// copy the first megabyte of game code
	dst := entry & 0x1fffffff
	n := 0x100000
	if len(m.rom) < 0x1000+n {
		n = len(m.rom) - 0x1000
	}
	if int(dst)+n <= len(m.rdram) {
		copy(m.rdram[dst:], m.rom[0x1000:0x1000+n])
	}
	// registers as the boot code leaves them
	g := &c.gpr
	for i := range g {
		g[i] = 0
	}
	g[11] = 0xffffffffa4000040
	g[19] = 0 // cartridge
	if m.pal {
		g[20] = 0
	} else {
		g[20] = 1
	}
	g[21] = 0 // cold boot
	g[22] = cicSeed[m.cic]
	g[23] = 0
	g[29] = 0xffffffffa4001ff0
	g[31] = 0xffffffffa4001550
	// COP0 as the boot code leaves it
	c.cop0[c0Status] = 0x241000e0
	c.cop0[c0Config] = 0x7006e463
	c.cop0[c0Cause] = 0xb000007c
	c.cop0[c0EPC] = 0xffffffffffffffff
	c.cop0[c0ErrorEPC] = 0xffffffffffffffff
	c.writeCop0(c0Count, 0x5000)
	c.cop0[c0Compare] = 0
	c.updateCompare()
	// memory size the game reads
	binary.BigEndian.PutUint32(m.rdram[0x318:], rdramSize)
	if m.cic == cic6105 { // the 6105 boot code keeps it here as well
		binary.BigEndian.PutUint32(m.rdram[0x3f0:], rdramSize)
	}
	// PI timing from the header
	m.pi[5] = uint32(m.rom[3])      // DOM1 latency
	m.pi[6] = uint32(m.rom[2])      // DOM1 pulse width
	m.pi[7] = uint32(m.rom[1]) & 15 // DOM1 page size
	m.pi[8] = uint32(m.rom[1]) >> 4 // DOM1 release
	// PI registers as the boot code's last transfer leaves them
	m.pi[0] = dst + 0x100000
	m.pi[1] = 0x1000000c
	m.sp.status = 1 // RSP halted
	m.vi[0] = 0
	m.ri = [8]uint32{0x0e, 0x40, 0x336, 0x14, 0, 0, 0, 0}
	// PIF RAM: the 6105 boot leaves a value here that some games check
	if m.cic == cic6105 {
		m.pif[0x26] = 0x3f
		m.pif[0x27] = 0x3f
	}
	c.pc = uint64(int64(int32(entry)))
	c.next = c.pc + 4
}
