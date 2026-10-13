package n64

import (
	"encoding/binary"
	"errors"
)

const (
	rdramSize = 8 << 20 // with the Expansion Pak
	cpuHz     = 93750000
)

// MI interrupt bits
const (
	intSP = 1 << 0
	intSI = 1 << 1
	intAI = 1 << 2
	intVI = 1 << 3
	intPI = 1 << 4
	intDP = 1 << 5
)

// Machine is a whole Nintendo 64.
type Machine struct {
	CPU   CPU
	RSP   RSP
	RDP   RDP
	rdram []byte
	spmem [0x2000]byte // DMEM (0x0000) and IMEM (0x1000)
	rom   []byte
	pif   [64]byte

	miMode, miIntr, miMask uint32
	vi                     [14]uint32
	viLine                 uint32
	ai                     aiState
	pi                     [13]uint32
	piBusyUntil            uint64
	siDram, siStatus       uint32
	siBusyUntil            uint64
	sp                     spRegs
	dp                     dpRegs
	ri                     [8]uint32

	Save    *Saves
	Pad     [4]PadState
	cic     int
	pal     bool
	pending []event

	// output
	Frame          []uint32 // ABGR, FrameW x FrameH
	FrameW, FrameH int
	Audio          []int16 // interleaved stereo at AudioRate
	AudioRate      int

	// settings
	CPUSpeed float64 // instructions per cycle multiplier (1 = one instruction per CPU cycle)
	idleSkip bool
	idle     bool   // the CPU is spinning in a wait loop
	stop     bool   // end the CPU's current time slice early
	rspTime  uint64 // CPU time the RSP has run up to
	rspFrac  uint64
	viField  uint32
}

type event struct {
	at   uint64
	kind int
}

const (
	evPI = iota
	evSI
	evAI
	evSPDMA
)

type spRegs struct {
	memAddr, dramAddr uint32 // addresses for the next DMA (as written)
	curMem, curDram   uint32 // where the last DMA got to (as read)
	length            uint32 // length register as read back
	status, semaphore uint32
}

type dpRegs struct {
	start, end, current, status, clock uint32
}

// PadState is a controller's buttons (N64 bit layout) and stick position.
type PadState struct {
	Buttons uint16
	X, Y    int8
	Present bool
	Pak     bool // a Controller Pak is plugged in
}

// New builds a machine for the given ROM (any byte order).
func New(romIn []byte) (*Machine, error) {
	rom, err := normalizeROM(romIn)
	if err != nil {
		return nil, err
	}
	ram := new([rdramSize + 8]byte)
	m := &Machine{rom: rom, rdram: ram[:rdramSize], CPUSpeed: 1, idleSkip: true}
	m.CPU.m = m
	m.CPU.ram = ram
	m.RSP.m = m
	m.RDP.m = m
	m.Save = newSaves(rom)
	m.pal = isPAL(rom)
	m.Pad[0].Present = true
	m.Pad[0].Pak = true
	m.Reset()
	return m, nil
}

func normalizeROM(b []byte) ([]byte, error) {
	if len(b) < 0x1000 {
		return nil, errors.New("that file is too small to be a Nintendo 64 game")
	}
	out := make([]byte, (len(b)+3)&^3)
	copy(out, b)
	switch binary.BigEndian.Uint32(out) {
	case 0x80371240: // .z64 (big-endian)
	case 0x37804012: // .v64 (16-bit byte-swapped)
		for i := 0; i+1 < len(out); i += 2 {
			out[i], out[i+1] = out[i+1], out[i]
		}
	case 0x40123780: // .n64 (little-endian 32-bit)
		for i := 0; i+3 < len(out); i += 4 {
			out[i], out[i+1], out[i+2], out[i+3] = out[i+3], out[i+2], out[i+1], out[i]
		}
	default:
		return nil, errors.New("that file doesn't look like a Nintendo 64 game")
	}
	return out, nil
}

func isPAL(rom []byte) bool {
	switch rom[0x3e] {
	case 'D', 'F', 'I', 'P', 'S', 'U', 'X', 'Y', 'L':
		return true
	}
	return false
}

// GameID returns the 4-character game code from the header (e.g. "NSME").
func (m *Machine) GameID() string { return string(m.rom[0x3b:0x3f]) }

// Title returns the internal name from the header.
func (m *Machine) Title() string {
	t := []byte{}
	for _, c := range m.rom[0x20:0x34] {
		if c >= 0x20 && c < 0x7f {
			t = append(t, c)
		}
	}
	return string(t)
}

func (m *Machine) Reset() {
	for i := range m.rdram {
		m.rdram[i] = 0
	}
	m.spmem = [0x2000]byte{}
	m.pif = [64]byte{}
	m.miMode, m.miIntr, m.miMask = 0, 0, 0
	m.vi = [14]uint32{}
	m.ai = aiState{}
	m.pi = [13]uint32{}
	m.siStatus = 0
	m.sp = spRegs{status: 1}
	m.dp = dpRegs{}
	m.pending = nil
	m.CPU.reset()
	m.RSP.reset()
	m.RDP.reset()
	m.hleBoot()
}

// ---------------------------------------------------------------------
// interrupts
// ---------------------------------------------------------------------

func (m *Machine) raise(bit uint32) { m.miIntr |= bit; m.CPU.irqPending = true }
func (m *Machine) clear(bit uint32) { m.miIntr &^= bit; m.CPU.irqPending = true }

func (m *Machine) schedule(at uint64, kind int) {
	m.pending = append(m.pending, event{at, kind})
}

func (m *Machine) nextEvent() uint64 {
	n := uint64(1<<63 - 1)
	for _, e := range m.pending {
		if e.at < n {
			n = e.at
		}
	}
	return n
}

func (m *Machine) runEvents(now uint64) {
	for i := 0; i < len(m.pending); {
		e := m.pending[i]
		if e.at > now {
			i++
			continue
		}
		m.pending = append(m.pending[:i], m.pending[i+1:]...)
		switch e.kind {
		case evPI:
			m.pi[4] &^= 3
			m.raise(intPI)
		case evSI:
			m.siStatus &^= 1
			m.siStatus |= 1 << 12
			m.raise(intSI)
		case evAI:
			m.aiDone()
		case evSPDMA:
		}
	}
}

// ---------------------------------------------------------------------
// physical memory
// ---------------------------------------------------------------------

func (m *Machine) read32(p uint32) uint32 {
	if p < rdramSize {
		return binary.BigEndian.Uint32(m.rdram[p:])
	}
	return m.readIO(p &^ 3)
}

func (m *Machine) read16(p uint32) uint16 {
	if p < rdramSize {
		return binary.BigEndian.Uint16(m.rdram[p:])
	}
	if p >= 0x10000000 && p < 0x1fc00000 {
		// the cartridge bus answers 16-bit reads with the upper half of
		// the word the address rounds to
		return uint16(m.readIO((p+2)&^3) >> 16)
	}
	v := m.readIO(p &^ 3)
	return uint16(v >> ((2 - (p & 2)) * 8))
}

func (m *Machine) read8(p uint32) uint8 {
	if p < rdramSize {
		return m.rdram[p]
	}
	if p >= 0x10000000 && p < 0x1fc00000 {
		return uint8(m.read16(p&^1) >> ((1 - (p & 1)) * 8))
	}
	v := m.readIO(p &^ 3)
	return uint8(v >> ((3 - (p & 3)) * 8))
}

func (m *Machine) write32(p uint32, v, mask uint32) {
	if p < rdramSize {
		if mask == 0xffffffff {
			binary.BigEndian.PutUint32(m.rdram[p:], v)
		} else {
			old := binary.BigEndian.Uint32(m.rdram[p:])
			binary.BigEndian.PutUint32(m.rdram[p:], (old&^mask)|(v&mask))
		}
		return
	}
	if mask != 0xffffffff && p >= 0x04000000 && p < 0x04002000 {
		old := m.readIO(p &^ 3)
		v = (old &^ mask) | (v & mask)
	}
	m.writeIO(p&^3, v)
}

func (m *Machine) write16(p uint32, v uint16) {
	if p < rdramSize {
		binary.BigEndian.PutUint16(m.rdram[p:], v)
		return
	}
	sh := (2 - (p & 2)) * 8
	m.write32(p&^3, uint32(v)<<sh, 0xffff<<sh)
}

func (m *Machine) write8(p uint32, v uint8) {
	if p < rdramSize {
		m.rdram[p] = v
		return
	}
	sh := (3 - (p & 3)) * 8
	m.write32(p&^3, uint32(v)<<sh, 0xff<<sh)
}

func (m *Machine) romRead32(off uint32) uint32 {
	if int(off)+4 <= len(m.rom) {
		return binary.BigEndian.Uint32(m.rom[off:])
	}
	return (off & 0xffff) | (off&0xffff)<<16 // open bus pattern
}

func (m *Machine) readIO(p uint32) uint32 {
	switch {
	case p >= 0x04000000 && p < 0x04040000:
		return binary.BigEndian.Uint32(m.spmem[p&0x1ffc:])
	case p >= 0x04040000 && p < 0x04040020:
		return m.spRead((p >> 2) & 7)
	case p == 0x04080000:
		return m.RSP.pc & 0xffc
	case p >= 0x04100000 && p < 0x04100020:
		return m.dpRead((p >> 2) & 7)
	case p >= 0x04300000 && p < 0x04300010:
		switch p & 0xf {
		case 0:
			return m.miMode
		case 4:
			return 0x02020102
		case 8:
			return m.miIntr
		default:
			return m.miMask
		}
	case p >= 0x04400000 && p < 0x04400038:
		r := (p >> 2) & 15
		if r == 4 {
			return m.viLine
		}
		if int(r) < len(m.vi) {
			return m.vi[r]
		}
		return 0
	case p >= 0x04500000 && p < 0x04500018:
		return m.aiRead((p >> 2) & 7)
	case p >= 0x04600000 && p < 0x04600034:
		r := (p >> 2) & 15
		if r == 4 {
			st := uint32(0)
			if m.CPU.Cycles < m.piBusyUntil {
				st |= 1
			}
			if m.miIntr&intPI != 0 {
				st |= 8
			}
			return st
		}
		if r == 2 || r == 3 { // length registers read back as 0x7f
			return 0x7f
		}
		return m.pi[r]
	case p >= 0x04700000 && p < 0x04700020:
		return m.ri[(p>>2)&7]
	case p >= 0x04800000 && p < 0x0480001c:
		switch p & 0x1f {
		case 0x00:
			return m.siDram
		case 0x18:
			st := m.siStatus &^ 0x1000
			if m.miIntr&intSI != 0 {
				st |= 0x1000
			}
			return st
		}
		return 0
	case p >= 0x08000000 && p < 0x10000000:
		return m.Save.cartRead32(p)
	case p >= 0x10000000 && p < 0x1fc00000:
		return m.romRead32(p - 0x10000000)
	case p >= 0x1fc007c0 && p < 0x1fc00800:
		return binary.BigEndian.Uint32(m.pif[p&0x3c:])
	}
	return 0
}

func (m *Machine) writeIO(p uint32, v uint32) {
	switch {
	case p >= 0x04000000 && p < 0x04040000:
		binary.BigEndian.PutUint32(m.spmem[p&0x1ffc:], v)
	case p >= 0x04040000 && p < 0x04040020:
		m.spWrite((p>>2)&7, v)
	case p == 0x04080000:
		m.RSP.setPC(v)
	case p >= 0x04100000 && p < 0x04100020:
		m.dpWrite((p>>2)&7, v)
	case p >= 0x04300000 && p < 0x04300010:
		switch p & 0xf {
		case 0:
			m.miMode = (m.miMode &^ 0x7f) | (v & 0x7f)
			if v&(1<<7) != 0 {
				m.miMode &^= 1 << 7
			}
			if v&(1<<8) != 0 {
				m.miMode |= 1 << 7
			}
			if v&(1<<9) != 0 {
				m.miMode &^= 1 << 8
			}
			if v&(1<<10) != 0 {
				m.miMode |= 1 << 8
			}
			if v&(1<<11) != 0 {
				m.clear(intDP)
			}
			if v&(1<<12) != 0 {
				m.miMode &^= 1 << 9
			}
			if v&(1<<13) != 0 {
				m.miMode |= 1 << 9
			}
		case 0xc:
			for i := uint32(0); i < 6; i++ {
				if v&(1<<(i*2)) != 0 {
					m.miMask &^= 1 << i
				}
				if v&(2<<(i*2)) != 0 {
					m.miMask |= 1 << i
				}
			}
			m.CPU.irqPending = true
		}
	case p >= 0x04400000 && p < 0x04400038:
		r := (p >> 2) & 15
		if r == 4 {
			m.clear(intVI)
			return
		}
		if int(r) < len(m.vi) {
			m.vi[r] = v
		}
	case p >= 0x04500000 && p < 0x04500018:
		m.aiWrite((p>>2)&7, v)
	case p >= 0x04600000 && p < 0x04600034:
		m.piWrite((p>>2)&15, v)
	case p >= 0x04700000 && p < 0x04700020:
		m.ri[(p>>2)&7] = v
	case p >= 0x04800000 && p < 0x0480001c:
		m.siWrite(p&0x1f, v)
	case p >= 0x08000000 && p < 0x10000000:
		m.Save.cartWrite32(p, v)
	case p >= 0x1fc007c0 && p < 0x1fc00800:
		binary.BigEndian.PutUint32(m.pif[p&0x3c:], v)
		m.pifCommand()
	}
}

// ---------------------------------------------------------------------
// PI: cartridge DMA
// ---------------------------------------------------------------------

func (m *Machine) piWrite(r uint32, v uint32) {
	switch r {
	case 0:
		m.pi[0] = v & 0xfffffe
	case 1:
		m.pi[1] = v &^ 1
	case 2: // RDRAM -> cart
		n := (v & 0xffffff) + 1
		dram, cart := m.pi[0], m.pi[1]
		if cart >= 0x08000000 && cart < 0x10000000 && dram+n <= rdramSize {
			m.Save.dmaWrite(cart, m.rdram[dram:dram+n])
		}
		m.piDone(cart, n)
	case 3: // cart -> RDRAM
		n := (v & 0xffffff) + 1
		dram, cart := m.pi[0], m.pi[1]
		// The PI moves data in blocks of up to 128 bytes. The first block
		// is shortened by the RDRAM address's misalignment, and its last
		// misaligned bytes never reach memory.
		first := 128 - dram&7
		if first > n {
			first = n
		}
		m.piCopy(dram, cart, first-dram&7)
		if n > first {
			m.piCopy(dram+first, cart+first, n-first)
		}
		m.pi[0] = (dram + n + 7) &^ 7
		m.pi[1] = (cart + n + 1) &^ 1
		m.piDone(cart, n)
	case 4:
		if v&2 != 0 {
			m.clear(intPI)
		}
	default:
		if r < 13 {
			m.pi[r] = v & 0xff
		}
	}
}

// piCopy copies n bytes from the cartridge bus to RDRAM.
func (m *Machine) piCopy(dram, cart, n uint32) {
	if dram >= rdramSize || n == 0 {
		return
	}
	if dram+n > rdramSize {
		n = rdramSize - dram
	}
	dst := m.rdram[dram : dram+n]
	if cart >= 0x10000000 && cart < 0x1fc00000 {
		o := int(cart - 0x10000000)
		k := 0
		if o < len(m.rom) {
			k = copy(dst, m.rom[o:])
		}
		for ; k < len(dst); k++ {
			dst[k] = 0
		}
	} else if cart >= 0x08000000 && cart < 0x10000000 {
		m.Save.dmaRead(cart, dst)
	}
}

// piDone marks the PI busy for as long as moving n bytes over the cartridge
// bus takes with the domain's timing settings, then interrupts.
func (m *Machine) piDone(cart, n uint32) {
	// domain 2 (SRAM / FlashRAM, 64DD registers) has its own timing registers
	r := uint32(5)
	if (cart >= 0x05000000 && cart < 0x06000000) || (cart >= 0x08000000 && cart < 0x10000000) {
		r = 9
	}
	lat, pwd, pgs, rls := uint64(m.pi[r]&0xff), uint64(m.pi[r+1]&0xff), uint64(m.pi[r+2]&15), uint64(m.pi[r+3]&3)
	page := uint64(4) << pgs // bytes per page
	pages := (uint64(n) + page - 1) / page
	// each page costs the latency, then every 16-bit word a pulse and a
	// release, in RCP clocks (2/3 of the CPU clock)
	rcp := pages*(lat+1) + (uint64(n)+1)/2*(pwd+1+rls+1)
	dur := rcp*3/2 + 50
	m.piBusyUntil = m.CPU.Cycles + dur
	m.schedule(m.piBusyUntil, evPI)
}

// ---------------------------------------------------------------------
// SI: serial interface to the PIF (controllers, EEPROM)
// ---------------------------------------------------------------------

func (m *Machine) siWrite(r uint32, v uint32) {
	switch r {
	case 0x00:
		m.siDram = v & 0xfffffc
	case 0x04: // PIF -> RDRAM
		m.pifCommand()
		for i := uint32(0); i < 64; i++ {
			if m.siDram+i < rdramSize {
				m.rdram[m.siDram+i] = m.pif[i]
			}
		}
		m.siStart()
	case 0x10: // RDRAM -> PIF
		for i := uint32(0); i < 64; i++ {
			if m.siDram+i < rdramSize {
				m.pif[i] = m.rdram[m.siDram+i]
			}
		}
		m.pifCommand()
		m.siStart()
	case 0x18:
		m.clear(intSI)
		m.siStatus &^= 0x1000
	}
}

func (m *Machine) siStart() {
	m.siStatus |= 1
	m.siBusyUntil = m.CPU.Cycles + 6000
	m.schedule(m.siBusyUntil, evSI)
}
