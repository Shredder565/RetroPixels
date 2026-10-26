// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// AX voice allocation (AXAcquireVoice, AXFreeVoice), app frame callbacks,
// the low-pass filter helper, and the AXVPB fields beyond those in ax.go.

import "math"

// AXVPB fields
const (
	axvDepop      = 0x2c // nonzero: ramp the voice out to avoid a click
	axvOffsets    = 0x34 // AXPBOFFSET { u16 format, u16 loop, loop, end, current, data }
	axvCallbackEx = 0x48
	axvDropReason = 0x4c

	axPriorityMax       = 32 // valid priorities are 1..31
	axAppFrameCallbacks = 64

	// AX results
	axErrNullCallback = 0xffffffef // -17
	axErrNotFound     = 0xfffffff0 // -16
	axErrTooMany      = 0xfffffff1 // -15
)

// axSyncOffsets writes the voice's current offsets into its AXVPB, where
// programs look for them.
func (m *Machine) axSyncOffsets(v *axVoice) {
	p := v.addr + axvOffsets
	mem := m.Mem
	mem.Write16(p+0x0, v.format)
	mem.Write16(p+0x2, uint16(b2u(v.loop)))
	for i, w := range [4]uint32{v.loopOff, v.endOff, v.cur, v.data} {
		mem.Write32(p+4+uint32(i)*4, w)
	}
}

// axVictim picks the voice to hand out for a new one of priority prio: a
// free voice when there is one, otherwise the voice with the lowest
// priority below prio (nil when every voice outranks it).
func (a *AX) axVictim(prio uint32) (v *axVoice, stolen bool) {
	for i := range a.voices {
		if !a.voices[i].used {
			return &a.voices[i], false
		}
	}
	for i := range a.voices {
		c := &a.voices[i]
		if c.priority < prio && (v == nil || c.priority < v.priority) {
			v = c
		}
	}
	return v, v != nil
}

// axAcquire is AXAcquireVoice / AXAcquireVoiceEx. When a voice has to be
// taken from its owner, the owner's callbacks hear about it: the plain one
// with (voice), the Ex one with (voice, user, reason 0).
func (m *Machine) axAcquire(cpu *CPU, prio, cb, cbEx, user uint32) {
	if prio == 0 || prio >= axPriorityMax {
		ret(cpu, 0)
		return
	}
	v, stolen := m.ax().axVictim(prio)
	if v == nil {
		ret(cpu, 0)
		return
	}
	mem := m.Mem
	if stolen {
		if v.playing {
			mem.Write32(v.addr+axvDepop, 1)
		}
		m.queueCallback(1, v.callback, v.addr)
		mem.Write32(v.addr+axvDropReason, 0)
		m.queueCallback(1, v.callbackEx, v.addr, v.user, 0)
	}
	*v = axVoice{
		addr: v.addr, used: true, priority: prio,
		callback: cb, callbackEx: cbEx, user: user,
		ratio: 1, volume: 1,
	}
	mem.Write32(v.addr+axvState, 0)
	mem.Write32(v.addr+axvPriority, prio)
	mem.Write32(v.addr+axvCallback, cb)
	mem.Write32(v.addr+axvUser, user)
	mem.Write32(v.addr+axvCallbackEx, cbEx)
	ret(cpu, v.addr)
}

// axFree is AXFreeVoice.
func (m *Machine) axFree(v *axVoice) {
	if !v.used {
		return
	}
	mem := m.Mem
	if v.playing {
		mem.Write32(v.addr+axvDepop, 1)
	}
	*v = axVoice{addr: v.addr}
	for _, f := range [...]uint32{axvState, axvPriority, axvCallback, axvCallbackEx} {
		mem.Write32(v.addr+f, 0)
	}
}

// axLpfCoefs is AXComputeLpfCoefs: the coefficients of a one-pole low-pass
// filter with its cutoff at freq Hz (32 kHz sample rate), as 1.15 fixed
// point a0 and b0 with a0 + b0 = 1.
func axLpfCoefs(freq uint32) (a0, b0 uint16) {
	w := float32(freq) / 32000 * float32(2*math.Pi)
	k := 2 - math.Cos(float64(w))
	pole := k - math.Sqrt(k*k-1)
	b0 = uint16(uint32(pole * 32768))
	return 0x7fff - b0, b0
}

// axSrcRatio converts a pitch ratio to the hardware's 16.16 fixed point
// (0 to 8) and back.
func axSrcRatio(f float64) float64 {
	fixed := f * 65536
	if !(fixed >= 0) { // also catches NaN
		fixed = 0
	}
	fixed = min(fixed, 8*65536)
	return float64(int32(fixed)) / 65536
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
			if fn == 0 {
				ret(cpu, axErrNullCallback)
				return
			}
			if len(a.appCBs) >= axAppFrameCallbacks {
				ret(cpu, axErrTooMany)
				return
			}
			a.appCBs = append(a.appCBs, fn)
			ret(cpu, 0)
		})
		reg(lib, "AXDeregisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
			fn, a := arg(cpu, 0), m.ax()
			if fn == 0 {
				ret(cpu, axErrNullCallback)
				return
			}
			for i := range a.appCBs {
				if a.appCBs[i] == fn {
					a.appCBs = append(a.appCBs[:i], a.appCBs[i+1:]...)
					ret(cpu, 0)
					return
				}
			}
			ret(cpu, axErrNotFound)
		})
		// AXComputeLpfCoefs(freq, u16 *a0, u16 *b0)
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
