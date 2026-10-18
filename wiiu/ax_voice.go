// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/snd_core/ax_voice.cpp,
// ax_ist.cpp, ax_exports.cpp and ax.h, rewritten in Go for RetroPixels.

package wiiu

import "math"

// More AXVPB fields, as Cemu lays them out.
const (
	axvDepop      = 0x2c
	axvOffsets    = 0x34 // AXPBOFFSET: the program reads its voice's offsets here
	axvCallbackEx = 0x48 // AXAcquireVoiceEx's callback
	axvDropReason = 0x4c

	axPriorityMax = 32

	axAppFrameCallbacks = 64
)

// axSyncOffsets mirrors a voice's offsets into its AXVPB.
func (m *Machine) axSyncOffsets(v *axVoice) {
	mem := m.Mem
	p := v.addr + axvOffsets
	mem.Write16(p, v.format)
	mem.Write16(p+2, uint16(b2u(v.loop)))
	mem.Write32(p+4, v.loopOff)
	mem.Write32(p+8, v.endOff)
	mem.Write32(p+12, v.cur)
	mem.Write32(p+16, v.data)
}

// axAcquire gives the program a voice, dropping a lower-priority one when
// none is free. AXAcquireVoice's callback takes (voice); AXAcquireVoiceEx's
// takes (voice, user, reason).
func (m *Machine) axAcquire(cpu *CPU, priority, callback, callbackEx, user uint32) {
	if priority == 0 || priority >= axPriorityMax {
		ret(cpu, 0)
		return
	}
	a := m.ax()
	var pick *axVoice
	for i := range a.voices {
		if !a.voices[i].used {
			pick = &a.voices[i]
			break
		}
	}
	mem := m.Mem
	if pick == nil {
		// drop the lowest-priority voice below this one
		for i := range a.voices {
			v := &a.voices[i]
			if v.priority < priority && (pick == nil || v.priority < pick.priority) {
				pick = v
			}
		}
		if pick == nil {
			ret(cpu, 0)
			return
		}
		if pick.playing {
			mem.Write32(pick.addr+axvDepop, 1)
		}
		m.queueCallback(1, pick.callback, pick.addr)
		mem.Write32(pick.addr+axvDropReason, 0)
		m.queueCallback(1, pick.callbackEx, pick.addr, pick.user, 0)
	}
	*pick = axVoice{addr: pick.addr, used: true, priority: priority, callback: callback, callbackEx: callbackEx,
		user: user, ratio: 1, volume: 1}
	mem.Write32(pick.addr+axvState, 0)
	mem.Write32(pick.addr+axvPriority, priority)
	mem.Write32(pick.addr+axvCallback, callback)
	mem.Write32(pick.addr+axvUser, user)
	mem.Write32(pick.addr+axvCallbackEx, callbackEx)
	ret(cpu, pick.addr)
}

// axFree gives a voice back.
func (m *Machine) axFree(v *axVoice) {
	if !v.used {
		return
	}
	mem := m.Mem
	if v.playing {
		mem.Write32(v.addr+axvDepop, 1)
	}
	*v = axVoice{addr: v.addr}
	mem.Write32(v.addr+axvState, 0)
	mem.Write32(v.addr+axvPriority, 0)
	mem.Write32(v.addr+axvCallback, 0)
	mem.Write32(v.addr+axvCallbackEx, 0)
}

// axLpfCoefs is AXComputeLpfCoefs: a one-pole low-pass filter at freq Hz.
func axLpfCoefs(freq uint32) (a0, b0 uint16) {
	t1 := math.Cos(float64(float32(freq) / 32000 * 6.2831855))
	t2 := 2 - t1
	r := uint16(uint32((t2 - math.Sqrt(t2*t2-1)) * 32768))
	return 0x7fff - r, r
}

// axSrcRatio is the voice's 16.16 sample rate ratio, at most 8.
func axSrcRatio(f float64) float64 {
	r := f * 65536
	switch {
	case r < 0 || math.IsNaN(r):
		r = 0
	case r > 0x80000:
		r = 0x80000
	}
	return float64(int32(r)) / 65536
}

func init() {
	for _, s := range []string{"sndcore2", "snd_core"} {
		reg(s, "AXAcquireVoice", func(m *Machine, cpu *CPU, t *Thread) {
			m.axAcquire(cpu, arg(cpu, 0), arg(cpu, 1), 0, arg(cpu, 2))
		})
		reg(s, "AXAcquireVoiceEx", func(m *Machine, cpu *CPU, t *Thread) {
			m.axAcquire(cpu, arg(cpu, 0), 0, arg(cpu, 1), arg(cpu, 2))
		})
		reg(s, "AXFreeVoice", func(m *Machine, cpu *CPU, t *Thread) {
			if v := m.axVoiceAt(arg(cpu, 0)); v != nil {
				m.axFree(v)
			}
			ret(cpu, 0)
		})
		reg(s, "AXRegisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
			fn := arg(cpu, 0)
			a := m.ax()
			switch {
			case fn == 0:
				ret(cpu, 0xffffffef) // -17
			case len(a.appCBs) >= axAppFrameCallbacks:
				ret(cpu, 0xfffffff1) // -15: full
			default:
				a.appCBs = append(a.appCBs, fn)
				ret(cpu, 0)
			}
		})
		reg(s, "AXDeregisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
			fn := arg(cpu, 0)
			a := m.ax()
			if fn == 0 {
				ret(cpu, 0xffffffef)
				return
			}
			for i, f := range a.appCBs {
				if f == fn {
					a.appCBs = append(a.appCBs[:i], a.appCBs[i+1:]...)
					ret(cpu, 0)
					return
				}
			}
			ret(cpu, 0xfffffff0) // -16: not found
		})
		reg(s, "AXComputeLpfCoefs", func(m *Machine, cpu *CPU, t *Thread) {
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
