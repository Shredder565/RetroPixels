package wiiu

// MIX (snd_user, and snduser2 on newer programs), the mixer library that
// sits on top of AX. Instead of setting a voice's volume and its level in
// each speaker by hand, a program gives each voice an input level, a fader,
// a pan (left to right) and a span (front to back), all in tenths of a
// decibel, and MIXUpdateSettings turns them into the voice's AX settings.
//
// Only the main mix is used here: aux sends feed effects (reverb, chorus)
// that RetroPixels doesn't run.

import "math"

const mixMute = -904 // tenths of a dB: the quietest level, treated as off

type mixChannel struct {
	input, fader int32
	pan, span    int32 // 0..127, 64 is the middle
	drcInput     int32
	drcPan       int32
	drc          bool // set up for the GamePad too
}

// mixGain turns tenths of a decibel into a linear level.
func mixGain(db int32) float64 {
	if db <= mixMute {
		return 0
	}
	return math.Pow(10, float64(db)/200)
}

// mixPan is the level of the left and right speakers for a pan of 0..127,
// keeping the loudness the same across the range.
func mixPan(p int32) (l, r float64) {
	a := float64(max(0, min(127, p))) / 127 * math.Pi / 2
	return math.Cos(a), math.Sin(a)
}

func (a *AX) mixCh(addr uint32) *mixChannel {
	if a.mix == nil {
		a.mix = map[uint32]*mixChannel{}
	}
	c := a.mix[addr]
	if c == nil {
		c = &mixChannel{pan: 64, drcPan: 64}
		a.mix[addr] = c
	}
	return c
}

// mixApply sets a voice's volume and speaker levels from its mixer settings.
func (m *Machine) mixApply(addr uint32, c *mixChannel) {
	v := m.axVoiceAt(addr)
	if v == nil {
		return
	}
	v.volume, v.volDelta = mixGain(c.input), 0
	f := mixGain(c.fader)
	l, r := mixPan(c.pan)
	// span: 127 is all front, 0 all back (the surround speakers)
	front := float64(max(0, min(127, c.span))) / 127
	v.mix[0] = [6]float64{f * l * front, f * r * front, f * l * (1 - front), f * r * (1 - front), 0, 0}
	if c.drc {
		g := mixGain(c.drcInput)
		dl, dr := mixPan(c.drcPan)
		v.mix[1] = [6]float64{g * dl, g * dr}
	}
}

func init() {
	for _, lib := range []string{"snd_user", "snduser2"} {
		lib := lib
		ok := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
		for _, n := range []string{"MIXInit", "MIXQuit", "MIXSetSoundMode", "MIXSetDRCSoundMode",
			"MIXAuxAPostFader", "MIXAuxBPostFader", "MIXAuxAPreFader", "MIXAuxBPreFader",
			"MIXSetAuxA", "MIXSetAuxB", "MIXSetAuxC", "MIXAdjustAuxA", "MIXAdjustAuxB", "MIXAdjustAuxC"} {
			reg(lib, n, ok)
		}
		reg(lib, "MIXGetSoundMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) }) // stereo
		// (voice, mode, input, auxA, auxB, auxC, pan, span, fader)
		reg(lib, "MIXInitChannel", func(m *Machine, cpu *CPU, t *Thread) {
			a := m.ax()
			c := a.mixCh(arg(cpu, 0))
			c.input = int32(arg(cpu, 2))
			c.pan = int32(arg(cpu, 6))
			c.span = int32(arg(cpu, 7))
			c.fader = int32(stackArg(m, cpu, 8))
			m.mixApply(arg(cpu, 0), c)
		})
		// (voice, aux, input, pan)
		reg(lib, "MIXDRCInitChannel", func(m *Machine, cpu *CPU, t *Thread) {
			c := m.ax().mixCh(arg(cpu, 0))
			c.drc = true
			c.drcInput = int32(arg(cpu, 2))
			c.drcPan = int32(arg(cpu, 3))
			m.mixApply(arg(cpu, 0), c)
		})
		reg(lib, "MIXReleaseChannel", func(m *Machine, cpu *CPU, t *Thread) {
			if a := m.ax(); a.mix != nil {
				delete(a.mix, arg(cpu, 0))
			}
		})
		set := func(name string, f func(c *mixChannel, v int32)) {
			reg(lib, name, func(m *Machine, cpu *CPU, t *Thread) {
				f(m.ax().mixCh(arg(cpu, 0)), int32(arg(cpu, 1)))
			})
		}
		set("MIXSetInput", func(c *mixChannel, v int32) { c.input = v })
		set("MIXAdjustInput", func(c *mixChannel, v int32) { c.input += v })
		set("MIXSetFader", func(c *mixChannel, v int32) { c.fader = v })
		set("MIXAdjustFader", func(c *mixChannel, v int32) { c.fader += v })
		set("MIXSetPan", func(c *mixChannel, v int32) { c.pan = v })
		set("MIXAdjustPan", func(c *mixChannel, v int32) { c.pan += v })
		set("MIXSetSPan", func(c *mixChannel, v int32) { c.span = v })
		set("MIXAdjustSPan", func(c *mixChannel, v int32) { c.span += v })
		set("MIXDRCSetInput", func(c *mixChannel, v int32) { c.drcInput = v })
		set("MIXDRCAdjustInput", func(c *mixChannel, v int32) { c.drcInput += v })
		set("MIXDRCSetPan", func(c *mixChannel, v int32) { c.drcPan = v })
		get := func(name string, f func(c *mixChannel) int32) {
			reg(lib, name, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(f(m.ax().mixCh(arg(cpu, 0))))) })
		}
		get("MIXGetInput", func(c *mixChannel) int32 { return c.input })
		get("MIXGetFader", func(c *mixChannel) int32 { return c.fader })
		get("MIXGetPan", func(c *mixChannel) int32 { return c.pan })
		get("MIXGetSPan", func(c *mixChannel) int32 { return c.span })
		reg(lib, "MIXUpdateSettings", func(m *Machine, cpu *CPU, t *Thread) {
			a := m.ax()
			for addr, c := range a.mix {
				m.mixApply(addr, c)
			}
		})
	}
}
