package model3

// The NCR 53C810 SCSI controller. The Model 3 has no SCSI devices: games use
// its SCRIPTS processor as a DMA engine (Memory Move) to copy data, and its
// interrupt instruction to signal the end.

type scsi struct {
	regs  [0x60]uint8
	istat uint8
	dstat uint8
	dien  uint8
	dmode uint8
	dcntl uint8
	dcmd  uint8
	dbc   uint32
	dsp   uint32
	dsps  uint32
	temp  uint32
	halt  bool
}

func (s *scsi) reset() { *s = scsi{halt: true} }

func setByte(v *uint32, n uint, b uint8) { *v = *v&^(0xFF<<(8*n)) | uint32(b)<<(8*n) }

func (m *Machine) scsiFetch(off uint32) uint32 { return bswap32(m.busRead32(m.scsi.dsp + off)) }

// busRead32 / busWrite32: memory as a bus master sees it
func (m *Machine) busRead32(a uint32) uint32 {
	if a < uint32(len(m.ram))-3 {
		return uint32(m.ram[a])<<24 | uint32(m.ram[a+1])<<16 | uint32(m.ram[a+2])<<8 | uint32(m.ram[a+3])
	}
	if a >= 0xFF800000 {
		o := a - 0xFF800000
		return uint32(m.crom[o])<<24 | uint32(m.crom[o+1])<<16 | uint32(m.crom[o+2])<<8 | uint32(m.crom[o+3])
	}
	return m.Read32(a)
}

func (m *Machine) busWrite32(a, v uint32) {
	if a < uint32(len(m.ram))-3 {
		m.ram[a], m.ram[a+1], m.ram[a+2], m.ram[a+3] = byte(v>>24), byte(v>>16), byte(v>>8), byte(v)
		m.cpu.InvalidateRAM(a, 4)
		return
	}
	m.Write32(a, v)
}

func (m *Machine) busRead8(a uint32) uint8 {
	if a < uint32(len(m.ram)) {
		return m.ram[a]
	}
	if a >= 0xFF800000 {
		return m.crom[a-0xFF800000]
	}
	return m.Read8(a)
}

func (m *Machine) busWrite8(a uint32, v uint8) {
	if a < uint32(len(m.ram)) {
		m.ram[a] = v
		m.cpu.InvalidateRAM(a, 1)
		return
	}
	m.Write8(a, v)
}

// scsiStep runs one SCRIPTS instruction; false = stop.
func (m *Machine) scsiStep() bool {
	s := &m.scsi
	op := m.scsiFetch(0)
	s.dbc = op & 0xFFFFFF
	s.dcmd = uint8(op >> 24)
	s.dsps = m.scsiFetch(4)
	switch {
	case s.dcmd&0xE0 == 0xC0: // Memory Move
		src := s.dsps
		dst := m.scsiFetch(8)
		n := s.dbc
		for i := uint32(0); i < n/4; i++ {
			m.busWrite32(dst, m.busRead32(src))
			dst += 4
			src += 4
		}
		for k := n & 3; k > 0; k-- {
			m.busWrite8(dst, m.busRead8(src))
			dst++
			src++
		}
		s.dbc = 0
		s.dsps = src
		s.temp = dst
		s.dsp += 12
		return true
	case s.dcmd&0xF8 == 0x98: // INT
		s.halt = true
		s.istat |= 1
		s.dstat |= 4
		if s.dien&4 != 0 {
			m.irq.assert(irqDMA)
		}
		return false
	}
	return false
}

func (m *Machine) scsiRun(single bool) {
	s := &m.scsi
	if single {
		m.scsiStep()
		s.istat |= 1
		s.dstat |= 8
		if s.dien&8 != 0 {
			m.irq.assert(irqDMA)
		}
		return
	}
	for i := 0; i < 100 && !s.halt; i++ {
		if !m.scsiStep() {
			break
		}
	}
}

func (m *Machine) scsiWrite(r uint32, v uint8) {
	s := &m.scsi
	if r >= 0x60 {
		return
	}
	s.regs[r] = v
	switch r {
	case 0x14:
		s.istat = v
	case 0x1C, 0x1D, 0x1E, 0x1F:
		setByte(&s.temp, uint(r-0x1C), v)
	case 0x24, 0x25, 0x26:
		setByte(&s.dbc, uint(r-0x24), v)
	case 0x27:
		s.dcmd = v
	case 0x2C, 0x2D, 0x2E:
		setByte(&s.dsp, uint(r-0x2C), v)
	case 0x2F:
		setByte(&s.dsp, 3, v)
		s.halt = false
		if s.dmode&1 == 0 {
			m.scsiRun(false)
		}
	case 0x30, 0x31, 0x32, 0x33:
		setByte(&s.dsps, uint(r-0x30), v)
	case 0x38:
		s.dmode = v
	case 0x39:
		s.dien = v
	case 0x3B:
		s.dcntl = v
		if v&0x14 == 0x14 {
			m.scsiRun(true)
		} else if v&4 != 0 {
			m.scsiRun(false)
		}
	}
}

func (m *Machine) scsiRead(r uint32) uint8 {
	s := &m.scsi
	if r >= 0x60 {
		return 0
	}
	switch r {
	case 0x0C:
		v := s.dstat
		s.istat &= 0xFE
		s.dstat &= 0xF7
		m.irq.deassert(irqDMA)
		return v
	case 0x14:
		return s.istat
	case 0x1C, 0x1D, 0x1E, 0x1F:
		return uint8(s.temp >> (8 * (r - 0x1C)))
	case 0x24, 0x25, 0x26:
		return uint8(s.dbc >> (8 * (r - 0x24)))
	case 0x27:
		return s.dcmd
	case 0x2C, 0x2D, 0x2E, 0x2F:
		return uint8(s.dsp >> (8 * (r - 0x2C)))
	case 0x30, 0x31, 0x32, 0x33:
		return uint8(s.dsps >> (8 * (r - 0x30)))
	case 0x38:
		return s.dmode
	case 0x39:
		return s.dien
	case 0x3B:
		return s.dcntl
	}
	return s.regs[r]
}
