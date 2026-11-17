package wiiu

// sndcore2's voice calls: handing out and taking back voices, the app frame
// callbacks, and the low-pass filter helper.

import "math"

// AXVoice fields beyond those in ax.go (the SDK's layout, as in wut's
// sndcore2/voice.h)
const (
	axvDepop      = 0x2c // set when a playing voice is cut off
	axvOffsets    = 0x34 // AXVoiceOffsets: format, loop flag, loop, end, current, data
	axvCallbackEx = 0x48
	axvDropReason = 0x4c

	axTopPriority  = 31 // priorities run from 1 to 31
	axMaxFrameCBs  = 64
	axErrNullCB    = 0xffffffef // -17
	axErrCBsFull   = 0xfffffff1 // -15
	axErrCBMissing = 0xfffffff0 // -16
)

// axSyncOffsets writes a voice's playing position and loop settings into
// its AXVoice, where the program reads them.
func (m *Machine) axSyncOffsets(v *axVoice) {
	p := v.addr + axvOffsets
	m.Mem.Write16(p, v.format)
	m.Mem.Write16(p+2, uint16(b2u(v.loop)))
	for i, x := range [4]uint32{v.loopOff, v.endOff, v.cur, v.data} {
		m.Mem.Write32(p+4+4*uint32(i), x)
	}
}

// writeVoiceOwner records who owns a voice in its AXVoice.
func (m *Machine) writeVoiceOwner(v *axVoice) {
	m.Mem.Write32(v.addr+axvState, 0)
	m.Mem.Write32(v.addr+axvPriority, v.priority)
	m.Mem.Write32(v.addr+axvCallback, v.callback)
	m.Mem.Write32(v.addr+axvUser, v.user)
	m.Mem.Write32(v.addr+axvCallbackEx, v.callbackEx)
}

// axAcquire hands out a voice of the given priority (1-31), and returns its
// address, or 0. With every voice taken, the one with the lowest priority
// below this one is taken from its owner, who hears about it through its
// callback: AXAcquireVoice's gets (voice), AXAcquireVoiceEx's gets (voice,
// user, reason 0).
func (m *Machine) axAcquire(cpu *CPU, priority, callback, callbackEx, user uint32) {
	if priority < 1 || priority > axTopPriority {
		ret(cpu, 0)
		return
	}
	a := m.ax()
	var v *axVoice
	for i := range a.voices {
		if !a.voices[i].used {
			v = &a.voices[i]
			break
		}
	}
	if v == nil {
		for i := range a.voices {
			c := &a.voices[i]
			if c.priority < priority && (v == nil || c.priority < v.priority) {
				v = c
			}
		}
		if v == nil {
			ret(cpu, 0)
			return
		}
		if v.playing {
			m.Mem.Write32(v.addr+axvDepop, 1)
		}
		m.queueCallback(1, v.callback, v.addr)
		m.Mem.Write32(v.addr+axvDropReason, 0)
		m.queueCallback(1, v.callbackEx, v.addr, v.user, 0)
	}
	*v = axVoice{addr: v.addr, used: true, priority: priority, callback: callback,
		callbackEx: callbackEx, user: user, ratio: 1, volume: 1}
	m.writeVoiceOwner(v)
	ret(cpu, v.addr)
}

// axFree takes a voice back.
func (m *Machine) axFree(v *axVoice) {
	if !v.used {
		return
	}
	if v.playing {
		m.Mem.Write32(v.addr+axvDepop, 1)
	}
	*v = axVoice{addr: v.addr}
	m.writeVoiceOwner(v)
}

// axLpfCoefs is AXComputeLpfCoefs: the one-pole low-pass filter
// y[n] = a0*x[n] + b0*y[n-1] (1.15 fixed point) with its cutoff at freq Hz
// for 32 kHz sound. The pole is the root inside the unit circle of
// b^2 - 2(2 - cos w)b + 1 = 0.
func axLpfCoefs(freq uint32) (a0, b0 uint16) {
	w := float64(float32(freq) / 32000 * float32(2*math.Pi))
	k := 2 - math.Cos(w)
	b := uint16(uint32((k - math.Sqrt(k*k-1)) * 32768))
	return 0x7fff - b, b
}

// axSrcRatio clamps a voice's playback rate ratio to what the 16.16
// register holds, 0 to 8.
func axSrcRatio(f float64) float64 {
	if !(f > 0) {
		return 0
	}
	return float64(int32(min(f*65536, 8*65536))) / 65536
}

func init() {
	for _, lib := range []string{"sndcore2", "snd_core"} {
		// AXAcquireVoice(priority, callback, user)
		reg(lib, "AXAcquireVoice", func(m *Machine, cpu *CPU, t *Thread) {
			m.axAcquire(cpu, arg(cpu, 0), arg(cpu, 1), 0, arg(cpu, 2))
		})
		// AXAcquireVoiceEx(priority, callbackEx, user)
		reg(lib, "AXAcquireVoiceEx", func(m *Machine, cpu *CPU, t *Thread) {
			m.axAcquire(cpu, arg(cpu, 0), 0, arg(cpu, 1), arg(cpu, 2))
		})
		reg(lib, "AXFreeVoice", func(m *Machine, cpu *CPU, t *Thread) {
			if v := m.axVoiceAt(arg(cpu, 0)); v != nil {
				m.axFree(v)
			}
			ret(cpu, 0)
		})
		reg(lib, "AXRegisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
			fn, a := arg(cpu, 0), m.ax()
			switch {
			case fn == 0:
				ret(cpu, axErrNullCB)
			case len(a.appCBs) >= axMaxFrameCBs:
				ret(cpu, axErrCBsFull)
			default:
				a.appCBs = append(a.appCBs, fn)
				ret(cpu, 0)
			}
		})
		reg(lib, "AXDeregisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
			fn, a := arg(cpu, 0), m.ax()
			if fn == 0 {
				ret(cpu, axErrNullCB)
				return
			}
			for i, f := range a.appCBs {
				if f == fn {
					a.appCBs = append(a.appCBs[:i], a.appCBs[i+1:]...)
					ret(cpu, 0)
					return
				}
			}
			ret(cpu, axErrCBMissing)
		})
		// AXComputeLpfCoefs(freq, &a0, &b0)
		reg(lib, "AXComputeLpfCoefs", func(m *Machine, cpu *CPU, t *Thread) {
			a0, b0 := axLpfCoefs(arg(cpu, 0))
			if p := arg(cpu, 1); p != 0 {
				m.Mem.Write16(p, a0)
			}
			if p := arg(cpu, 2); p != 0 {
				m.Mem.Write16(p, b0)
			}
		})
	}
}
