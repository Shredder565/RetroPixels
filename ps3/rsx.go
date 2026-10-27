package ps3

// The RSX's command processor: it reads the command buffer from get to
// put, runs the methods (rsx_methods.go draws), and flips the display.

type rsxState struct {
	regs       [0x10000 / 4]uint32
	callStack  []uint32
	stalled    bool   // waiting on a semaphore or a flip
	flipWait   bool   // a flip is waiting for the vblank
	flipID     uint32 // what's on screen (display buffer, or 0x100+resc source)
	shownID    uint32
	flips      uint64
	semOffset  uint32 // NV406E
	semOffset2 uint32 // NV4097
	ran        uint64
	fault      bool
	draw       *rsxDraw
}

func (m *Machine) rsxInit() {
	if m.misc["rsx"] == nil {
		m.misc["rsx"] = &rsxState{shownID: 0xffffffff}
	}
}

func (m *Machine) rsx() *rsxState {
	r, _ := m.misc["rsx"].(*rsxState)
	return r
}

// rsxRunUntil runs the RSX until cond is true or it can't go on.
func (m *Machine) rsxRunUntil(cond func() bool) {
	for i := 0; i < 1000 && !cond(); i++ {
		if !m.rsxRun() {
			return
		}
	}
}

// rsxRun processes commands until get reaches put or the RSX must wait;
// it reports whether it did anything.
func (m *Machine) rsxRun() bool {
	g := m.gcm
	r := m.rsx()
	if g == nil || r == nil || r.fault {
		return false
	}
	if r.flipWait {
		return false
	}
	ctrl := g.control
	did := false
	for n := 0; n < 1<<20; n++ {
		put := m.Mem.Read32(ctrl)
		get := m.Mem.Read32(ctrl + 4)
		if get == put {
			return did
		}
		a := g.ioToAddr(get)
		if a == 0 {
			m.Logf("RSX: command buffer offset %x isn't mapped", get)
			r.fault = true
			return did
		}
		cmd := m.Mem.Read32(a)
		switch {
		case cmd&0xe0000003 == 0x20000000: // old jump
			m.Mem.Write32(ctrl+4, cmd&0x1ffffffc)
			did = true
			continue
		case cmd&3 == 1: // new jump
			m.Mem.Write32(ctrl+4, cmd&0xfffffffc)
			did = true
			continue
		case cmd&3 == 2: // call
			r.callStack = append(r.callStack, get+4)
			m.Mem.Write32(ctrl+4, cmd&0x1ffffffc)
			did = true
			continue
		case cmd&0xffff0003 == 0x00020000: // return
			if len(r.callStack) == 0 {
				m.Logf("RSX: return with nothing to return to")
				r.fault = true
				return did
			}
			ret := r.callStack[len(r.callStack)-1]
			r.callStack = r.callStack[:len(r.callStack)-1]
			m.Mem.Write32(ctrl+4, ret)
			did = true
			continue
		case cmd == 0:
			m.Mem.Write32(ctrl+4, get+4)
			did = true
			continue
		}
		count := cmd >> 18 & 0x7ff
		method := cmd & 0xfffc
		inc := cmd&0x40000000 == 0
		// run each method; one may stall (then resume from that word)
		for i := uint32(0); i < count; i++ {
			v := m.Mem.Read32(a + 4 + 4*i)
			reg := method
			if inc {
				reg += 4 * i
			}
			if !m.rsxMethod(r, reg, v) {
				// stalled: rewrite the command to start at this method
				if i > 0 {
					left := count - i
					nreg := method
					if inc {
						nreg += 4 * i
					}
					// put a fresh header just before the data word
					m.Mem.Write32(a+4*i, left<<18|cmd&0x40000000|nreg)
					m.Mem.Write32(ctrl+4, get+4*i)
				}
				return did
			}
			did = true
		}
		m.Mem.Write32(ctrl+4, get+4+4*count)
		r.ran++
	}
	return did
}

// rsxMethod runs one method; false = stall (run it again later).
func (m *Machine) rsxMethod(r *rsxState, reg, v uint32) bool {
	g := m.gcm
	switch reg {
	case 0x0050: // NV406E reference
		m.Mem.Write32(g.control+8, v)
	case 0x0060: // NV406E semaphore context
	case 0x0064: // NV406E semaphore offset
		r.semOffset = v
	case 0x0068: // NV406E semaphore acquire
		if m.Mem.Read32(g.labels+r.semOffset) != v {
			return false
		}
	case 0x006C: // NV406E semaphore release
		m.Mem.Write32(g.labels+r.semOffset, v)
	case 0x1D6C: // NV4097 semaphore offset
		r.semOffset2 = v
	case 0x1D70: // back end write semaphore release
		m.Mem.Write32(g.labels+r.semOffset2, v&0xff00ff00|(v&0xff)<<16|(v>>16)&0xff)
	case 0x1D74: // texture read semaphore release
		m.Mem.Write32(g.labels+r.semOffset2, v)
	case 0x1800: // get report
		idx := v & 0xffffff
		a := g.reports + (idx & 0xfff0)
		m.Mem.Write64(a, m.tb*1000000000/TBFreq)
		m.Mem.Write32(a+8, 0)
		m.Mem.Write32(a+12, 0)
	case 0xFEAC: // flip
		r.flipID = v
		r.flipWait = true
		m.regs(r)[reg/4] = v
		return true
	case 0xE000: // driver object
	default:
		if reg < 0x10000 {
			r.regs[reg/4] = v
		}
		m.rsxDraw(r, reg, v)
	}
	if reg < 0x10000 {
		r.regs[reg/4] = v
	}
	return true
}

func (m *Machine) regs(r *rsxState) []uint32 { return r.regs[:] }

// vblank: show a waiting flip and call the game's handlers.
func (m *Machine) vblank() {
	g := m.gcm
	r := m.rsx()
	m.audioVBlank()
	if g == nil || r == nil {
		return
	}
	if g.intr != nil {
		g.intr.post(g.vblHandler, 1)
		g.intr.post(g.rescVBlank, 1)
	}
	if r.flipWait {
		r.flipWait = false
		r.shownID = r.flipID
		r.flips++
		m.present(r.flipID)
		if g.intr != nil {
			g.intr.post(g.flipHandler, 1)
			g.intr.post(g.rescFlip, 1)
		}
	}
	m.rsxRun()
}
