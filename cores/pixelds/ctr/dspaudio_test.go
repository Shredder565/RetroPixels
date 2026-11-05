package ctr

import (
	"encoding/binary"
	"math"
	"testing"
)

// TestDSPAudio plays a 16-bit voice and an ADPCM voice through the mixer
// the way a game sets them up.
func TestDSPAudio(t *testing.T) {
	m := newMachine(Options{SampleRate: 32728})
	le := binary.LittleEndian
	a := m.audio
	r0 := a.region(0)
	le.PutUint16(r0[offFrameCounter:], 5) // the game wrote region 0 last
	// 16-bit mono, 400 samples of 1000 at 0x20100000
	for i := 0; i < 400; i++ {
		le.PutUint16(m.mem.fcram[0x100000+2*i:], 1000)
	}
	cfg := r0[offVoiceConfig:]
	putF := func(b []byte, f float32) { le.PutUint32(b, math.Float32bits(f)) }
	putF(cfg[4:], 1)  // mixer 0, front left
	putF(cfg[8:], 1)  // front right
	putF(cfg[52:], 1) // rate
	cfg[56] = 1       // linear
	cfg[160] = 1      // enable
	putDspU32(cfg[172:], 0x20100000)
	putDspU32(cfg[176:], 400)
	le.PutUint16(cfg[180:], 1|1<<2) // mono, PCM16
	le.PutUint16(cfg[190:], 7)      // buffer id
	le.PutUint32(cfg, 1<<16|1<<18|1<<17|1<<25|1<<30|1)
	// ADPCM voice 1: coefficients zero, scale 8, all samples 3
	adp := cfg[192:]
	putF(adp[4:], 1)
	putF(adp[52:], 1)
	adp[56] = 2 // no interpolation
	adp[160] = 1
	for f := 0; f < 20; f++ {
		m.mem.fcram[0x110000+8*f] = 0x08
		for j := 1; j < 8; j++ {
			m.mem.fcram[0x110000+8*f+j] = 0x33
		}
	}
	putDspU32(adp[172:], 0x20110000)
	putDspU32(adp[176:], 280)
	le.PutUint16(adp[180:], 1|2<<2)
	le.PutUint16(adp[188:], 1) // ADPCM history given (zero)
	le.PutUint32(adp, 1<<16|1<<18|1<<17|1<<25|1<<30|1|1<<2)
	mc := r0[offMixConfig:]
	putF(mc[4:], 1)
	le.PutUint16(mc[22:], 1) // stereo
	le.PutUint32(mc, 1<<16|1<<26)

	a.frame()
	w := a.region(1)
	st := w[offVoiceStatus:]
	if st[0] != 1 || dspU32(st[4:]) < 160 || le.Uint16(st[8:]) != 7 {
		t.Errorf("voice status: enabled %d, position %d, buffer %d", st[0], dspU32(st[4:]), le.Uint16(st[8:]))
	}
	// the gains ease in over the first frame: its last sample has them all
	fin := w[offFinal:]
	l, r := int16(le.Uint16(fin[4*159:])), int16(le.Uint16(fin[4*159+2:]))
	if l != 1000+3<<8 || r != 1000 {
		t.Errorf("final sample: %d, %d, want %d, 1000", l, r, 1000+3<<8)
	}
	if le.Uint32(cfg) != 0 {
		t.Errorf("the changes weren't taken")
	}
	out := a.take()
	if len(out) < 300 || out[301] < 0.02 {
		t.Errorf("output: %d samples, %v", len(out), out[300:302])
	}
	// the voices run out and stop: the buffer id moves to "last"
	for f := 0; f < 4; f++ {
		a.frame()
	}
	if st[0] != 0 || le.Uint16(st[10:]) != 7 || le.Uint16(st[8:]) != 0 {
		t.Errorf("after the end: enabled %d, current %d, last %d", st[0], le.Uint16(st[8:]), le.Uint16(st[10:]))
	}
}
