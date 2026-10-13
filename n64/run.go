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
		// The RSP runs first, up to the end of this slice (at 2/3 of the
		// CPU clock), so work the CPU hands it is done by the time the CPU
		// looks again.
		m.runRSP(limit)
		m.stop = false
		for c.Cycles < limit {
			if c.irqPending {
				c.irqPending = false
				c.checkInterrupts()
			}
			c.Step()
			c.Cycles++
			if m.stop {
				break
			}
		}
		if m.idle {
			// waiting in a loop for an interrupt: skip ahead
			m.idle = false
			if c.Cycles < limit {
				c.Cycles = limit
			}
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

// runRSP lets the RSP catch up with CPU time t.
func (m *Machine) runRSP(t uint64) {
	if m.sp.status&1 != 0 || t <= m.rspTime {
		if m.rspTime < t && m.sp.status&1 != 0 {
			m.rspTime = t
		}
		return
	}
	d := t - m.rspTime
	m.rspTime = t
	m.rspFrac += d * 2
	m.RSP.run(int(m.rspFrac / 3))
	m.rspFrac %= 3
}
