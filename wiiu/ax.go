package wiiu

// AX, the Wii U's sound library (sndcore2, and snd_core, its older name).
//
// A program acquires voices, points each at sound data in memory (16-bit or
// 8-bit samples, or DSP ADPCM), sets its pitch (the sample rate converter
// ratio), volume and how loud it is in each speaker, and starts it. Every
// AX frame (3 ms) the sound chip mixes the playing voices, then AX calls the
// program's frame callback so it can set up the next frame.
//
// RetroPixels mixes the TV's sound as 48 kHz stereo at each AX frame of
// guest time, so the sound keeps pace with the program.

import "math"

const (
	axVoices        = 96
	axVoiceSize     = 0x100
	axOutRate       = 48000
	axFrameSamples  = axOutRate * 3 / 1000 // 144
	axTicksPer1000F = TimerHz * 3          // ticks per 1000 AX frames

	// AXVoice fields programs read
	axvIndex    = 0x00
	axvState    = 0x04
	axvPriority = 0x1c
	axvCallback = 0x20
	axvUser     = 0x24

	axFmtADPCM = 0x00
	axFmtPCM16 = 0x0a
	axFmtPCM8  = 0x19
)

type axVoice struct {
	addr     uint32
	used     bool
	priority uint32
	callback uint32
	user     uint32
	playing  bool

	format           uint16
	loop             bool
	loopOff, endOff  uint32
	cur              uint32 // the next sample (for ADPCM, nibble) to decode
	data             uint32
	loops            uint32
	coefs            [16]int32
	predScale        uint16
	hist1, hist2     int32
	loopPS           uint16
	loopH1, loopH2   int32
	loopContext      bool
	ratio            float64 // input samples per renderer sample
	frac             float64
	s0, s1           float64 // the samples either side of the position
	primed           bool
	ended            bool // past the end: the last samples are still sounding
	tail             int
	volume, volDelta float64
	mix              [2][6]float64 // TV and GamePad, by channel (main bus)
}

// AX is the sound library's state.
type AX struct {
	inited   bool
	renderer int // 32000 or 48000
	voices   [axVoices]axVoice
	frameCB  uint32
	appCB    uint32
	master   float64
	frames   uint64 // AX frames mixed
	start    uint64 // the time base when the first frame was due
	out      []float32
	// the time of the next frame
	next uint64
}

func (m *Machine) ax() *AX {
	if m.audio == nil {
		m.audio = &AX{renderer: 32000, master: 1}
		m.audio.next = m.tb + axTicksPer1000F/1000
		m.audio.start = m.tb
	}
	return m.audio
}

// audioTick mixes the AX frames that are due.
func (m *Machine) audioTick() {
	a := m.ax()
	for m.tb >= a.next {
		m.axMix(a)
		a.frames++
		a.next = a.start + (a.frames+1)*axTicksPer1000F/1000
		if a.inited {
			m.queueCallback(1, a.frameCB)
			m.queueCallback(1, a.appCB)
		}
	}
}

// nextAudio is when the next AX frame needs the program (0 = never).
func (m *Machine) nextAudio() uint64 {
	if m.audio == nil || !m.audio.inited || m.audio.frameCB == 0 && m.audio.appCB == 0 {
		return 0
	}
	return m.audio.next
}

// TakeAudio returns the sound mixed since the last call: 48 kHz stereo,
// interleaved left and right.
func (m *Machine) TakeAudio() []float32 {
	a := m.ax()
	out := a.out
	a.out = nil
	return out
}

func (m *Machine) axMix(a *AX) {
	n := len(a.out)
	a.out = append(a.out, make([]float32, axFrameSamples*2)...)
	buf := a.out[n:]
	if !a.inited {
		return
	}
	scale := float64(a.renderer) / axOutRate
	for i := range a.voices {
		v := &a.voices[i]
		if !v.used || !v.playing {
			continue
		}
		step := v.ratio * scale
		tv := v.mix[0]
		// TV channels: left, right, surround left, surround right, centre, LFE
		l := tv[0] + 0.7*tv[2] + 0.7*tv[4]
		r := tv[1] + 0.7*tv[3] + 0.7*tv[4]
		if !v.primed {
			v.s0 = 0
			v.s1 = m.axNext(v)
			v.primed = true
		}
		for s := 0; s < axFrameSamples && v.playing; s++ {
			x := (v.s0 + (v.s1-v.s0)*v.frac) * v.volume * a.master
			buf[2*s] += float32(x * l)
			buf[2*s+1] += float32(x * r)
			v.volume = math.Max(0, v.volume+v.volDelta)
			v.frac += step
			for v.frac >= 1 && v.playing {
				v.frac--
				v.s0 = v.s1
				v.s1 = m.axNext(v)
			}
		}
		if !v.playing {
			m.Mem.Write32(v.addr+axvState, 0)
		}
	}
	for i, x := range buf {
		buf[i] = float32(math.Max(-1, math.Min(1, float64(x))))
	}
}

// axNext decodes a voice's next sample (-1..1), moving past its end or
// looping as it says.
func (m *Machine) axNext(v *axVoice) float64 {
	mem := m.Mem
	if v.ended {
		if v.tail++; v.tail >= 2 {
			v.playing = false
		}
		return 0
	}
	var s int32
	switch v.format {
	case axFmtPCM16:
		s = int32(int16(mem.Read16(v.data + v.cur*2)))
	case axFmtPCM8:
		s = int32(int8(mem.Read8(v.data+v.cur))) << 8
	default: // DSP ADPCM: 8-byte frames of a header and 14 4-bit samples
		if v.cur&15 < 2 {
			v.cur = v.cur&^15 + 2
			v.predScale = uint16(mem.Read8(v.data + (v.cur-2)/2))
		}
		b := mem.Read8(v.data + v.cur/2)
		nib := int32(b >> 4)
		if v.cur&1 != 0 {
			nib = int32(b & 15)
		}
		if nib >= 8 {
			nib -= 16
		}
		p := int(v.predScale>>4) & 7
		scale := int32(1) << (v.predScale & 15)
		x := (nib*scale)<<11 + 1024 + v.coefs[2*p]*v.hist1 + v.coefs[2*p+1]*v.hist2
		s = max(-32768, min(32767, x>>11))
		v.hist2, v.hist1 = v.hist1, s
	}
	if v.cur == v.endOff {
		if v.loop {
			v.cur = v.loopOff
			v.loops++
			if v.format == axFmtADPCM && v.loopContext {
				v.predScale, v.hist1, v.hist2 = v.loopPS, v.loopH1, v.loopH2
			}
		} else {
			v.ended = true
		}
	} else {
		v.cur++
	}
	return float64(s) / 32768
}

func (m *Machine) axVoiceAt(addr uint32) *axVoice {
	a := m.ax()
	for i := range a.voices {
		if a.voices[i].addr == addr && addr != 0 {
			return &a.voices[i]
		}
	}
	return nil
}

func init() {
	for _, lib := range []string{"sndcore2", "snd_core"} {
		axRegister(lib)
	}
}

func axRegister(s string) {
	ok := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	start := func(m *Machine, renderer int) {
		a := m.ax()
		if a.inited {
			return
		}
		a.inited, a.renderer, a.master = true, renderer, 1
		for i := range a.voices {
			v := &a.voices[i]
			*v = axVoice{addr: m.sysAlloc(axVoiceSize, 32)}
			m.Mem.Write32(v.addr+axvIndex, uint32(i))
		}
	}
	reg(s, "AXInit", func(m *Machine, cpu *CPU, t *Thread) { start(m, 32000); ret(cpu, 0) })
	reg(s, "AXInitWithParams", func(m *Machine, cpu *CPU, t *Thread) {
		// AXInitParams: renderer (0 = 32 kHz, 1 = 48 kHz), ..., pipeline
		r := 32000
		if p := arg(cpu, 0); p != 0 && m.Mem.Read32(p) == 1 {
			r = 48000
		}
		start(m, r)
		ret(cpu, 0)
	})
	reg(s, "AXInitEx", func(m *Machine, cpu *CPU, t *Thread) { start(m, 32000); ret(cpu, 0) })
	reg(s, "AXIsInit", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.ax().inited) })
	reg(s, "AXQuit", func(m *Machine, cpu *CPU, t *Thread) {
		a := m.ax()
		a.inited, a.frameCB, a.appCB = false, 0, 0
		ret(cpu, 0)
	})
	reg(s, "AXGetInputSamplesPerSec", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(m.ax().renderer)) })
	reg(s, "AXGetInputSamplesPerFrame", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, uint32(m.ax().renderer*3/1000))
	})
	reg(s, "AXGetMaxVoices", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, axVoices) })
	reg(s, "AXRegisterFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
		a := m.ax()
		old := a.frameCB
		a.frameCB = arg(cpu, 0)
		ret(cpu, old)
	})
	reg(s, "AXRegisterCallback", func(m *Machine, cpu *CPU, t *Thread) {
		a := m.ax()
		old := a.frameCB
		a.frameCB = arg(cpu, 0)
		ret(cpu, old)
	})
	reg(s, "AXRegisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
		m.ax().appCB = arg(cpu, 0)
		ret(cpu, 0)
	})
	reg(s, "AXDeregisterAppFrameCallback", func(m *Machine, cpu *CPU, t *Thread) {
		m.ax().appCB = 0
		ret(cpu, 0)
	})
	for _, n := range []string{"AXVoiceBegin", "AXVoiceEnd", "AXUserBegin", "AXUserEnd", "AXSetVoiceType",
		"AXSetVoiceLpf", "AXSetVoiceLpfCoefs", "AXSetVoiceBiquad", "AXSetVoiceBiquadCoefs", "AXSetVoiceRmtOn",
		"AXSetVoiceRmtMix", "AXSetVoiceRmtSrc", "AXSetVoiceRmtIIR", "AXSetVoiceRmtIIRCoefs", "AXSetVoiceSrcType",
		"AXSetVoiceMixerSelect", "AXSetVoiceInitialTimeDelay", "AXSetDeviceMode", "AXSetDeviceCompressor",
		"AXSetDeviceUpsampleStage", "AXSetDeviceLinearUpsampler", "AXSetDeviceVolume", "AXSetDeviceRemixMatrix",
		"AXRegisterAuxCallback", "AXSetAuxReturnVolume", "AXSetAuxCallback", "AXRegisterDeviceFinalMixCallback",
		"AXSetDefaultMixerSelect", "AXInitProfile", "AXSetMaxDspCycles", "AXMakeCompressorTable", "AXSetDRCVSMode",
		"AXSetDRCVSDownmixBalance", "AXSetDRCVSSurroundDepth", "AXSetDRCVSSpeakerPosition", "AXSetDRCVSLC",
		"AXSetDRCVSLimiter", "AXSetDRCVSLimiterThreshold", "AXSetDRCVSOutputGain", "AXSetDRCVSSurroundLevelGain",
		"AXSetDRCVSDecodingGain", "AXSetStepMode", "AXSetMultiVoiceType"} {
		reg(s, n, ok)
	}
	reg(s, "AXVoiceIsProtected", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "AXGetDeviceMode", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
		ret(cpu, 0)
	})
	reg(s, "AXGetDeviceFinalMixCallback", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
		ret(cpu, 0)
	})
	reg(s, "AXGetAuxCallback", func(m *Machine, cpu *CPU, t *Thread) {
		for i := 3; i < 5; i++ {
			if p := arg(cpu, i); p != 0 {
				m.Mem.Write32(p, 0)
			}
		}
		ret(cpu, 0)
	})
	reg(s, "AXGetSwapProfile", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "AXGetDspLoad", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 10) })
	reg(s, "AXGetPPCLoad", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 10) })
	reg(s, "AXGetMasterVolume", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(m.ax().master*0x8000)) })
	reg(s, "AXSetMasterVolume", func(m *Machine, cpu *CPU, t *Thread) {
		m.ax().master = float64(arg(cpu, 0)&0xffff) / 0x8000
		ret(cpu, 0)
	})

	// voices
	reg(s, "AXAcquireVoice", func(m *Machine, cpu *CPU, t *Thread) {
		m.axAcquire(cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(s, "AXAcquireVoiceEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.axAcquire(cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(s, "AXFreeVoice", func(m *Machine, cpu *CPU, t *Thread) {
		if v := m.axVoiceAt(arg(cpu, 0)); v != nil {
			v.used, v.playing = false, false
			m.Mem.Write32(v.addr+axvState, 0)
		}
		ret(cpu, 0)
	})
	voice := func(name string, fn func(m *Machine, cpu *CPU, v *axVoice)) {
		reg(s, name, func(m *Machine, cpu *CPU, t *Thread) {
			v := m.axVoiceAt(arg(cpu, 0))
			ret(cpu, 0)
			if v != nil {
				fn(m, cpu, v)
			}
		})
	}
	voice("AXSetVoicePriority", func(m *Machine, cpu *CPU, v *axVoice) {
		v.priority = arg(cpu, 1)
		m.Mem.Write32(v.addr+axvPriority, v.priority)
	})
	voice("AXSetVoiceState", func(m *Machine, cpu *CPU, v *axVoice) {
		v.playing = arg(cpu, 1)&0xffff != 0
		m.Mem.Write32(v.addr+axvState, b2u(v.playing))
	})
	voice("AXIsVoiceRunning", func(m *Machine, cpu *CPU, v *axVoice) { retBool(cpu, v.playing) })
	voice("AXGetVoiceLoopCount", func(m *Machine, cpu *CPU, v *axVoice) { ret(cpu, v.loops) })
	voice("AXSetVoiceLoopCount", func(m *Machine, cpu *CPU, v *axVoice) { v.loops = arg(cpu, 1) })
	voice("AXSetVoiceOffsets", func(m *Machine, cpu *CPU, v *axVoice) {
		// AXVoiceOffsets: u16 format, u16 loop, loop offset, end offset,
		// current offset, data
		p := arg(cpu, 1)
		mem := m.Mem
		v.format = mem.Read16(p)
		v.loop = mem.Read16(p+2) != 0
		v.loopOff, v.endOff, v.cur, v.data = mem.Read32(p+4), mem.Read32(p+8), mem.Read32(p+12), mem.Read32(p+16)
		v.primed, v.frac, v.ended, v.tail = false, 0, false, 0
	})
	voice("AXGetVoiceOffsets", func(m *Machine, cpu *CPU, v *axVoice) {
		p := arg(cpu, 1)
		mem := m.Mem
		mem.Write16(p, v.format)
		mem.Write16(p+2, uint16(b2u(v.loop)))
		mem.Write32(p+4, v.loopOff)
		mem.Write32(p+8, v.endOff)
		mem.Write32(p+12, v.cur)
		mem.Write32(p+16, v.data)
	})
	voice("AXSetVoiceCurrentOffset", func(m *Machine, cpu *CPU, v *axVoice) {
		v.cur, v.primed, v.ended, v.tail = arg(cpu, 1), false, false, 0
	})
	voice("AXSetVoiceEndOffset", func(m *Machine, cpu *CPU, v *axVoice) { v.endOff = arg(cpu, 1) })
	voice("AXSetVoiceLoopOffset", func(m *Machine, cpu *CPU, v *axVoice) { v.loopOff = arg(cpu, 1) })
	voice("AXSetVoiceLoop", func(m *Machine, cpu *CPU, v *axVoice) { v.loop = arg(cpu, 1)&0xffff != 0 })
	voice("AXGetVoiceCurrentOffsetEx", func(m *Machine, cpu *CPU, v *axVoice) { ret(cpu, v.cur) })
	voice("AXSetVoiceAdpcm", func(m *Machine, cpu *CPU, v *axVoice) {
		// AXVoiceAdpcm: 16 coefficients, gain, predictor/scale, 2 history samples
		p := arg(cpu, 1)
		for i := range v.coefs {
			v.coefs[i] = int32(int16(m.Mem.Read16(p + uint32(i)*2)))
		}
		v.predScale = m.Mem.Read16(p + 0x22)
		v.hist1 = int32(int16(m.Mem.Read16(p + 0x24)))
		v.hist2 = int32(int16(m.Mem.Read16(p + 0x26)))
	})
	voice("AXSetVoiceAdpcmLoop", func(m *Machine, cpu *CPU, v *axVoice) {
		p := arg(cpu, 1)
		v.loopPS = m.Mem.Read16(p)
		v.loopH1 = int32(int16(m.Mem.Read16(p + 2)))
		v.loopH2 = int32(int16(m.Mem.Read16(p + 4)))
		v.loopContext = true
	})
	voice("AXSetVoiceSrc", func(m *Machine, cpu *CPU, v *axVoice) {
		// AXVoiceSrc: ratio (16.16), position fraction, last samples
		p := arg(cpu, 1)
		v.ratio = float64(m.Mem.Read32(p)) / 65536
		v.frac = float64(m.Mem.Read16(p+4)) / 65536
	})
	voice("AXSetVoiceSrcRatio", func(m *Machine, cpu *CPU, v *axVoice) {
		v.ratio = math.Max(0, argF(cpu, 0))
	})
	voice("AXSetVoiceVe", func(m *Machine, cpu *CPU, v *axVoice) {
		p := arg(cpu, 1)
		v.volume = float64(m.Mem.Read16(p)) / 0x8000
		v.volDelta = float64(int16(m.Mem.Read16(p+2))) / 0x8000
	})
	voice("AXSetVoiceVeDelta", func(m *Machine, cpu *CPU, v *axVoice) {
		v.volDelta = float64(int16(arg(cpu, 1))) / 0x8000
	})
	voice("AXSetVoiceDeviceMix", func(m *Machine, cpu *CPU, v *axVoice) {
		// (voice, device, controller, mix): per channel, 4 buses of
		// {u16 volume, s16 delta}; only the main bus is mixed
		dev, p := arg(cpu, 1), arg(cpu, 3)
		if dev > 1 || p == 0 {
			return
		}
		n := 6
		if dev == 1 {
			n = 4
		}
		for ch := 0; ch < n; ch++ {
			v.mix[dev][ch] = float64(m.Mem.Read16(p+uint32(ch)*16)) / 0x8000
		}
	})
}

func (m *Machine) axAcquire(cpu *CPU, priority, callback, user uint32) {
	a := m.ax()
	var pick *axVoice
	for i := range a.voices {
		if !a.voices[i].used {
			pick = &a.voices[i]
			break
		}
	}
	if pick == nil {
		// take the lowest-priority voice below this one
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
		if pick.callback != 0 {
			m.queueCallback(1, pick.callback, pick.addr)
		}
	}
	*pick = axVoice{addr: pick.addr, used: true, priority: priority, callback: callback, user: user, ratio: 1, volume: 1}
	mem := m.Mem
	mem.Write32(pick.addr+axvState, 0)
	mem.Write32(pick.addr+axvPriority, priority)
	mem.Write32(pick.addr+axvCallback, callback)
	mem.Write32(pick.addr+axvUser, user)
	ret(cpu, pick.addr)
}
