package n64

// AI (audio output) and VI (video output).

type aiState struct {
	dramNext                  uint32
	queue                     [2]struct{ addr, length uint32 }
	queued                    int
	control, dacrate, bitrate uint32
	playStart, playEnd        uint64
	sent                      uint32 // bytes of the front buffer already output
}

func (m *Machine) videoClock() float64 {
	if m.pal {
		return 49656530
	}
	return 48681812
}

func (m *Machine) aiRead(r uint32) uint32 {
	a := &m.ai
	switch r {
	case 1: // remaining length of the current buffer
		if a.queued == 0 {
			return 0
		}
		cur := a.queue[0].length
		if a.playEnd > a.playStart && m.CPU.Cycles < a.playEnd {
			left := float64(a.playEnd-m.CPU.Cycles) / float64(a.playEnd-a.playStart)
			return uint32(float64(cur)*left) &^ 7
		}
		return 0
	case 3:
		st := uint32(0x00100000) // DAC counter running
		if a.queued >= 2 {
			st |= 0x80000001
		}
		if a.queued >= 1 {
			st |= 0x40000000
		}
		if a.control&1 != 0 {
			st |= 0x02000000
		}
		return st
	}
	return 0
}

func (m *Machine) aiWrite(r uint32, v uint32) {
	a := &m.ai
	switch r {
	case 0:
		a.dramNext = v & 0xfffff8
	case 1:
		n := v & 0x3fff8
		if n == 0 || a.queued >= 2 {
			return
		}
		a.queue[a.queued].addr = a.dramNext
		a.queue[a.queued].length = n
		a.queued++
		if a.queued == 1 {
			m.aiStart()
		}
	case 2:
		a.control = v & 1
	case 3:
		m.clear(intAI)
	case 4:
		a.dacrate = v & 0x3fff
	case 5:
		a.bitrate = v & 15
	}
}

func (m *Machine) aiFreq() int {
	f := int(m.videoClock() / float64(m.ai.dacrate+1))
	if f < 4000 {
		f = 4000
	}
	return f
}

// aiStart begins playing the front buffer; an interrupt tells the game it
// can queue the next one.
func (m *Machine) aiStart() {
	a := &m.ai
	b := a.queue[0]
	freq := m.aiFreq()
	m.AudioRate = freq
	samples := uint64(b.length / 4)
	dur := samples * cpuHz / uint64(freq)
	if dur == 0 {
		dur = 1
	}
	a.playStart = m.CPU.Cycles
	a.playEnd = m.CPU.Cycles + dur
	a.sent = 0
	m.schedule(a.playEnd, evAI)
	m.raise(intAI)
}

// aiOutput sends the samples of the front buffer that have played by now.
func (m *Machine) aiOutput(now uint64) {
	a := &m.ai
	if a.queued == 0 {
		return
	}
	b := a.queue[0]
	upto := b.length
	if now < a.playEnd && a.playEnd > a.playStart {
		upto = uint32(uint64(b.length)*(now-a.playStart)/(a.playEnd-a.playStart)) &^ 3
	}
	if a.control&1 != 0 {
		for i := a.sent; i+3 < upto; i += 4 {
			p := b.addr + i
			if p+3 >= rdramSize {
				break
			}
			l := int16(uint16(m.rdram[p])<<8 | uint16(m.rdram[p+1]))
			r := int16(uint16(m.rdram[p+2])<<8 | uint16(m.rdram[p+3]))
			m.Audio = append(m.Audio, l, r)
		}
	}
	if upto > a.sent {
		a.sent = upto
	}
}

func (m *Machine) aiDone() {
	a := &m.ai
	if a.queued == 0 {
		return
	}
	m.aiOutput(a.playEnd)
	a.queue[0] = a.queue[1]
	a.queued--
	if a.queued > 0 {
		m.aiStart()
	}
}

// ---------------------------------------------------------------------
// VI: turn the frame buffer into the picture
// ---------------------------------------------------------------------

func (m *Machine) renderVI() {
	vi := &m.vi
	typ := vi[0] & 3
	origin := vi[1] & 0xffffff
	width := vi[2] & 0xfff
	hs, he := (vi[9]>>16)&0x3ff, vi[9]&0x3ff
	vs, ve := (vi[10]>>16)&0x3ff, vi[10]&0x3ff
	xs, ys := vi[12]&0xfff, vi[13]&0xfff
	// The picture is the frame buffer area the VI shows, at the frame
	// buffer's own resolution: the screen window (in screen pixels and
	// half-lines) times the scale factors (2.10 fixed point, frame buffer
	// pixels per screen pixel / per field line).
	lines := 240
	if ve > vs {
		lines = int(ve-vs) >> 1
	}
	// a window close to the full screen shows the whole standard area
	std := 240
	if m.pal {
		std = 288
	}
	if lines >= std-16 && lines < std {
		lines = std
	}
	cols := 640
	if he > hs {
		cols = int(he - hs)
	}
	if xs == 0 {
		xs = 0x400
	}
	if ys == 0 {
		ys = 0x400
	}
	outW := (cols*int(xs) + 0x200) >> 10
	outH := (lines*int(ys) + 0x200) >> 10
	if outW <= 0 || outW > 1280 || outH <= 0 || outH > 1024 {
		outW, outH = 320, 240
	}
	if len(m.Frame) < outW*outH {
		m.Frame = make([]uint32, outW*outH)
	}
	m.FrameW, m.FrameH = outW, outH
	f := m.Frame[:outW*outH]
	if typ < 2 || width == 0 {
		for i := range f {
			f[i] = 0xff000000
		}
		return
	}
	rd := m.rdram
	for y := 0; y < outH; y++ {
		o := y * outW
		row := f[o : o+outW]
		if typ == 2 {
			a := origin + uint32(y)*width*2
			for x := range row {
				p := a + uint32(x)*2
				if p+1 >= rdramSize {
					row[x] = 0xff000000
					continue
				}
				v := uint32(rd[p])<<8 | uint32(rd[p+1])
				r := (v >> 11) & 31
				g := (v >> 6) & 31
				b := (v >> 1) & 31
				row[x] = 0xff000000 | (b<<3|b>>2)<<16 | (g<<3|g>>2)<<8 | (r<<3 | r>>2)
			}
		} else {
			a := origin + uint32(y)*width*4
			for x := range row {
				p := a + uint32(x)*4
				if p+3 >= rdramSize {
					row[x] = 0xff000000
					continue
				}
				row[x] = 0xff000000 | uint32(rd[p+2])<<16 | uint32(rd[p+1])<<8 | uint32(rd[p])
			}
		}
	}
}

// VIRegs returns the VI registers (for debugging tools).
func (m *Machine) VIRegs() [14]uint32 { return m.vi }
