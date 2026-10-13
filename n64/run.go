package n64

// Frame loop: runs the CPU, the RSP and the timed events scan line by scan line.

const chunk = 1024 // CPU cycles run between RSP / event checks

// RunFrame emulates one video field and leaves the picture in Frame and the
// sound in Audio.
func (m *Machine) RunFrame() {
	lines := int((m.vi[6]&0x3ff)+1) / 2 // V_SYNC is in half-lines
	if lines < 200 || lines > 330 {
		if m.pal {
			lines = 313
		} else {
			lines = 263
		}
	}
	fps := 60.0
	if m.pal {
		fps = 50
	}
	perLine := uint64(float64(cpuHz)/fps/float64(lines) + 0.5)
	field := uint32(0)
	if m.vi[0]&0x40 != 0 { // interlaced: alternate fields
		m.viField ^= 1
		field = m.viField
	}
	for line := 0; line < lines; line++ {
		m.viLine = uint32(line*2) | field
		if m.viLine&^1 == m.vi[3]&0x3fe {
			m.raise(intVI)
		}
		m.runCycles(perLine)
	}
	m.renderVI()
}

func (m *Machine) runCycles(n uint64) {
	c := &m.CPU
	end := c.Cycles + n
	for c.Cycles < end {
		limit := end
		if limit-c.Cycles > chunk {
			limit = c.Cycles + chunk
		}
		if ne := m.nextEvent(); ne < limit {
			limit = ne
		}
		if c.compareAt < limit {
			limit = c.compareAt
		}
		start := c.Cycles
		m.idle = false
		for c.Cycles < limit {
			if c.irqPending {
				c.irqPending = false
				c.checkInterrupts()
			}
			c.Step()
			c.Cycles++
			if m.idle {
				c.Cycles = limit
				break
			}
		}
		// the RSP runs at 2/3 of the CPU clock
		if m.sp.status&1 == 0 {
			m.RSP.run(int((c.Cycles - start) * 2 / 3))
		}
		if c.Cycles >= c.compareAt {
			c.cop0[c0Cause] |= 1 << 15
			c.compareAt += 1 << 33
			c.irqPending = true
		}
		if m.nextEvent() <= c.Cycles {
			m.runEvents(c.Cycles)
			c.irqPending = true
		}
	}
}
