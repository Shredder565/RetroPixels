package n64

// RDP: the Reality Display Processor (rasterizer). Commands arrive through
// the DP registers from RDRAM or DMEM.

type RDP struct {
	m *Machine
}

func (d *RDP) reset() {}

func (m *Machine) dpRead(r uint32) uint32 {
	dp := &m.dp
	switch r {
	case 0:
		return dp.start
	case 1:
		return dp.end
	case 2:
		return dp.current
	case 3:
		return dp.status | 0x80 // command buffer ready
	case 4:
		return dp.clock
	}
	return 0
}

func (m *Machine) dpWrite(r uint32, v uint32) {
	dp := &m.dp
	switch r {
	case 0:
		if dp.status&0x400 == 0 {
			dp.start = v & 0xfffff8
			dp.current = dp.start
		}
	case 1:
		dp.end = v & 0xfffff8
		m.RDP.process()
	case 3:
		s := dp.status
		if v&1 != 0 {
			s &^= 1
		}
		if v&2 != 0 {
			s |= 1
		}
		if v&4 != 0 {
			s &^= 2
		}
		if v&8 != 0 {
			s |= 2
		}
		if v&16 != 0 {
			s &^= 4
		}
		if v&32 != 0 {
			s |= 4
		}
		if v&0x200 != 0 {
			dp.clock = 0
		}
		dp.status = s
	}
}

// process runs the commands between DPC_CURRENT and DPC_END.
func (d *RDP) process() {
	m := d.m
	dp := &m.dp
	for dp.current < dp.end {
		var w uint64
		if dp.status&1 != 0 { // from DMEM
			a := dp.current & 0xff8
			w = uint64(m.read32(0x04000000|a))<<32 | uint64(m.read32(0x04000000|(a+4)))
		} else {
			a := dp.current & 0xfffff8
			if a+8 > rdramSize {
				break
			}
			w = uint64(m.read32(a))<<32 | uint64(m.read32(a+4))
		}
		n := d.command(w, dp.current)
		dp.current += 8 * uint32(n)
	}
}

// command handles one command and returns its length in 64-bit words.
func (d *RDP) command(w uint64, addr uint32) int {
	op := (w >> 56) & 0x3f
	switch op {
	case 0x29: // full sync
		d.m.raise(intDP)
	}
	return rdpCmdLen(op)
}

func rdpCmdLen(op uint64) int {
	switch op {
	case 0x08:
		return 4
	case 0x09:
		return 6
	case 0x0a:
		return 12
	case 0x0b:
		return 14
	case 0x0c:
		return 12
	case 0x0d:
		return 14
	case 0x0e:
		return 20
	case 0x0f:
		return 22
	case 0x24, 0x25:
		return 2
	}
	return 1
}
