package ctr

import (
	"encoding/binary"
	"math"
)

// The DSP's sound mixing. Games describe up to 24 voices in DSP memory:
// each has a queue of buffers of samples (8-bit, 16-bit or 4-bit ADPCM,
// mono or stereo) in FCRAM, a playback rate, filters and gains into three
// mixers. Every audio frame (160 samples at about 32.7 kHz) the DSP reads
// the region holding the newer frame counter, plays each voice for a
// frame, reports where each voice is in the other region, and mixes the
// three mixers into the final stereo output (mixers 1 and 2 can go
// through the game first, for its effects: the "aux" buses).

const (
	dspRate        = 32728.498
	samplesInFrame = 160
	voices         = 24
	regionBytes    = 0x8000
)

// Where the structures are, as DSP word addresses in the first region,
// in the order the audio pipe reports them: frame counter, voice
// configurations, voice statuses, ADPCM coefficients, mixer
// configuration, DSP status, final samples, aux bus samples, compressor
// table, debug, and five more the games don't need from us.
var dspStructs = [15]uint16{
	0xbfff, 0x9e92, 0x8680, 0xa792, 0x9430, 0x8400, 0x8540, 0x9492,
	0x8710, 0x8410, 0xa912, 0xaa12, 0xaad2, 0xac52, 0xac5c,
}

// byte offsets in a region
func structOff(i int) int { return int(dspStructs[i]-0x8000) * 2 }

var (
	offFrameCounter = structOff(0)
	offVoiceConfig  = structOff(1)
	offVoiceStatus  = structOff(2)
	offCoefs        = structOff(3)
	offMixConfig    = structOff(4)
	offFinal        = structOff(6)
	offAux          = structOff(7)
)

// DSP words are 16 bits wide, so its 32-bit numbers have their halves the
// other way round from the ARM11's.
func dspU32(b []byte) uint32 { v := binary.LittleEndian.Uint32(b); return v<<16 | v>>16 }
func putDspU32(b []byte, v uint32) {
	binary.LittleEndian.PutUint32(b, v<<16|v>>16)
}

type dspBuffer struct {
	addr, length uint32
	adpcmPS      uint8
	adpcmY       [2]int16
	adpcmSet     bool
	loop         bool
	id           uint16
	stereo       bool
	format       uint8 // 0 PCM8, 1 PCM16, 2 ADPCM
	fromQueue    bool
	start        uint32
	played       bool
}

type voice struct {
	enabled   bool
	sync      uint16
	rate      float64
	interp    uint8 // 0 polyphase, 1 linear, 2 none
	gain      [3][4]float32
	rampFrom  [3][4]float32
	ramp      [3]bool
	coefs     [16]int16
	stereo    bool
	format    uint8
	queue     []dspBuffer // lowest id first
	cur       dspBuffer
	playing   bool
	pos       uint32 // samples into cur
	y1, y2    int16  // ADPCM history
	ps        uint8  // ADPCM predictor and scale
	curID     uint16
	lastID    uint16
	idChanged bool
	x0, x1    [2]int32 // interpolation: the two samples around the position
	frac      float64
	primed    bool
	filters   uint16
	simple    [2]int16
	biquad    [5]int16
	sy        [2]int32    // simple filter's last output
	bx, by    [2][2]int32 // biquad's last inputs and outputs
	out       [samplesInFrame][2]int32
}

type dspMixer struct {
	volume [3]float32
	aux    [2]bool
	mono   bool
	frame  [samplesInFrame][2]int32
}

// audio is the DSP's sound state.
type audio struct {
	m      *Machine
	v      [voices]voice
	mix    dspMixer
	out    []float32 // stereo samples at the output rate
	rate   float64   // output rate
	resamp float64   // position between DSP samples
	last   [2]float32
}

func newAudio(m *Machine, rate int) *audio {
	if rate <= 0 {
		rate = 48000
	}
	a := &audio{m: m, rate: float64(rate)}
	for i := range a.v {
		a.v[i].rate = 1
	}
	return a
}

func (a *audio) region(i int) []byte {
	off := 0x50000
	if i == 1 {
		off = 0x70000
	}
	return a.m.mem.dsp[off : off+regionBytes]
}

// regions returns the region the game wrote last (the newer frame
// counter, allowing for its wrapping round) and the other.
func (a *audio) regions() (read, write []byte) {
	r0, r1 := a.region(0), a.region(1)
	c0 := binary.LittleEndian.Uint16(r0[offFrameCounter:])
	c1 := binary.LittleEndian.Uint16(r1[offFrameCounter:])
	first := c0 > c1
	if c0 == 0xffff && c1 != 0xfffe {
		first = false
	} else if c1 == 0xffff && c0 != 0xfffe {
		first = true
	}
	if first {
		return r0, r1
	}
	return r1, r0
}

// frame makes one audio frame.
func (a *audio) frame() {
	read, write := a.regions()
	var mixes [3][samplesInFrame][4]int32
	for i := range a.v {
		v := &a.v[i]
		cfg := read[offVoiceConfig+192*i : offVoiceConfig+192*(i+1)]
		v.configure(cfg, read[offCoefs+32*i:])
		if v.enabled {
			a.play(v)
		}
		st := write[offVoiceStatus+12*i:]
		st[0] = b2u(v.enabled)
		st[1] = b2u(v.idChanged)
		v.idChanged = false
		binary.LittleEndian.PutUint16(st[2:], v.sync)
		putDspU32(st[4:], v.pos)
		binary.LittleEndian.PutUint16(st[8:], v.curID)
		binary.LittleEndian.PutUint16(st[10:], v.lastID)
		if v.enabled {
			v.mixInto(&mixes)
		} else {
			v.rampFrom, v.ramp = v.gain, [3]bool{}
		}
	}
	a.mixFrame(read, write, &mixes)
	fin := write[offFinal:]
	for s := 0; s < samplesInFrame; s++ {
		for c := 0; c < 2; c++ {
			binary.LittleEndian.PutUint16(fin[4*s+2*c:], uint16(int16(a.mix.frame[s][c])))
		}
	}
	a.output()
}

func b2u(b bool) uint8 {
	if b {
		return 1
	}
	return 0
}

func f32(b []byte) float32 { return math.Float32frombits(binary.LittleEndian.Uint32(b)) }

// configure takes the changes the game marked in a voice's configuration,
// and clears the marks.
func (v *voice) configure(cfg, coefs []byte) {
	le := binary.LittleEndian
	dirty := le.Uint32(cfg)
	if dirty == 0 {
		return
	}
	bit := func(n uint) bool { return dirty>>n&1 != 0 }
	if bit(29) { // reset
		*v = voice{rate: 1}
	}
	if bit(4) { // partial reset: drop the queue
		v.queue = nil
	}
	if bit(16) {
		v.enabled = cfg[160] != 0
	}
	if bit(28) {
		v.sync = le.Uint16(cfg[162:])
	}
	if bit(18) {
		v.rate = float64(f32(cfg[52:]))
		if !(v.rate > 0) || v.rate > 64 {
			v.rate = 1
		}
	}
	if bit(2) {
		for i := range v.coefs {
			v.coefs[i] = int16(le.Uint16(coefs[2*i:]))
		}
	}
	for g := 0; g < 3; g++ {
		if bit(25 + uint(g)) {
			v.rampFrom[g], v.ramp[g] = v.gain[g], true
			for c := 0; c < 4; c++ {
				v.gain[g][c] = f32(cfg[4+16*g+4*c:])
			}
		}
	}
	if bit(22) {
		v.filters = le.Uint16(cfg[58:])
	}
	if bit(23) {
		v.simple = [2]int16{int16(le.Uint16(cfg[60:])), int16(le.Uint16(cfg[62:]))}
	}
	if bit(24) {
		for i := range v.biquad {
			v.biquad[i] = int16(le.Uint16(cfg[64+2*i:]))
		}
	}
	if bit(17) {
		v.interp = cfg[56]
	}
	flags1 := le.Uint16(cfg[180:])
	if bit(0) || bit(30) {
		v.format = uint8(flags1 >> 2 & 3)
	}
	if bit(1) || bit(30) {
		v.stereo = flags1&3 == 2
	}
	var start uint32
	if bit(21) {
		start = dspU32(cfg[164:])
	}
	if bit(3) && v.playing { // the buffer playing now has grown
		v.cur.length = dspU32(cfg[176:])
		v.cur.loop = le.Uint16(cfg[188:])&2 != 0
	}
	if bit(30) { // the embedded buffer
		if n := dspU32(cfg[176:]); int32(n) >= 0 {
			f2 := le.Uint16(cfg[188:])
			v.push(dspBuffer{
				addr: dspU32(cfg[172:]), length: n, adpcmPS: uint8(le.Uint16(cfg[182:])),
				adpcmY:   [2]int16{int16(le.Uint16(cfg[184:])), int16(le.Uint16(cfg[186:]))},
				adpcmSet: f2&1 != 0, loop: f2&2 != 0, id: le.Uint16(cfg[190:]),
				stereo: v.stereo, format: v.format, start: start,
			})
		}
	}
	if bit(19) { // queued buffers
		mask := le.Uint16(cfg[74:])
		for i := 0; i < 4; i++ {
			if mask>>i&1 == 0 {
				continue
			}
			b := cfg[76+20*i:]
			if n := dspU32(b[4:]); int32(n) >= 0 {
				v.push(dspBuffer{
					addr: dspU32(b), length: n, adpcmPS: uint8(le.Uint16(b[8:])),
					adpcmY:   [2]int16{int16(le.Uint16(b[10:])), int16(le.Uint16(b[12:]))},
					adpcmSet: b[14] != 0, loop: b[15] != 0, id: le.Uint16(b[16:]),
					stereo: v.stereo, format: v.format, fromQueue: true,
				})
			}
		}
		le.PutUint16(cfg[74:], 0)
	}
	le.PutUint32(cfg, 0)
}

func (v *voice) push(b dspBuffer) {
	i := len(v.queue)
	for i > 0 && v.queue[i-1].id > b.id {
		i--
	}
	v.queue = append(v.queue, dspBuffer{})
	copy(v.queue[i+1:], v.queue[i:])
	v.queue[i] = b
}

// next takes the next buffer from the queue (false when it's empty).
func (v *voice) next() bool {
	if len(v.queue) == 0 {
		v.playing = false
		return false
	}
	b := v.queue[0]
	v.queue = v.queue[1:]
	if b.adpcmSet {
		v.y1, v.y2 = b.adpcmY[0], b.adpcmY[1]
	}
	v.ps = b.adpcmPS
	v.cur, v.playing = b, true
	v.pos = 0
	if !b.played {
		v.pos = b.start
	}
	v.curID, v.lastID = b.id, 0
	v.idChanged = b.fromQueue && !b.played
	if b.loop {
		b.played = true
		v.push(b)
	}
	return true
}

// sample reads the next sample of the voice (false when it has run out).
func (v *voice) sample(mem *memory) ([2]int32, bool) {
	for !v.playing || v.pos >= v.cur.length {
		if !v.next() {
			return [2]int32{}, false
		}
	}
	b := &v.cur
	ch := uint32(1)
	if b.stereo {
		ch = 2
	}
	var s [2]int32
	switch b.format {
	case 0: // 8-bit
		p := mem.physSlice(b.addr+v.pos*ch, ch)
		if p == nil {
			v.playing = false
			return s, false
		}
		s[0] = int32(int8(p[0])) << 8
		s[1] = int32(int8(p[ch-1])) << 8
	case 1: // 16-bit
		p := mem.physSlice(b.addr&^1+v.pos*2*ch, 2*ch)
		if p == nil {
			v.playing = false
			return s, false
		}
		s[0] = int32(int16(binary.LittleEndian.Uint16(p)))
		s[1] = int32(int16(binary.LittleEndian.Uint16(p[2*(ch-1):])))
	case 2: // ADPCM: 8-byte frames of a header and 14 4-bit samples
		frame, n := v.pos/14, v.pos%14
		p := mem.physSlice(b.addr+frame*8, 8)
		if p == nil {
			v.playing = false
			return s, false
		}
		if n == 0 {
			v.ps = p[0]
		}
		nib := int32(p[1+n/2])
		if n&1 == 0 {
			nib >>= 4
		}
		nib = nib & 15
		if nib >= 8 {
			nib -= 16
		}
		pred := (v.ps >> 4) & 7
		c1, c2 := int32(v.coefs[2*pred]), int32(v.coefs[2*pred+1])
		x := (nib<<(v.ps&15))<<11 + 1024 + c1*int32(v.y1) + c2*int32(v.y2)
		x >>= 11
		x = max(-32768, min(32767, x))
		v.y2, v.y1 = v.y1, int16(x)
		s[0], s[1] = x, x
	default:
		v.playing = false
		return s, false
	}
	v.pos++
	return s, true
}

// play makes the voice's frame.
func (a *audio) play(v *voice) {
	v.out = [samplesInFrame][2]int32{}
	mem := a.m.mem
	if !v.playing || v.pos >= v.cur.length {
		if !v.next() {
			// nothing left: the voice stops
			v.enabled = false
			v.lastID, v.curID = v.curID, 0
			v.idChanged = true
			v.primed = false
			return
		}
	}
	if !v.primed {
		v.x0, _ = v.sample(mem)
		v.x1, _ = v.sample(mem)
		v.frac, v.primed = 0, true
	}
	ended := false
	for i := 0; i < samplesInFrame && !ended; i++ {
		if v.interp == 2 {
			v.out[i] = v.x0
		} else {
			for c := 0; c < 2; c++ {
				v.out[i][c] = v.x0[c] + int32(float64(v.x1[c]-v.x0[c])*v.frac)
			}
		}
		v.frac += v.rate
		for v.frac >= 1 {
			v.frac--
			v.x0 = v.x1
			var ok bool
			if v.x1, ok = v.sample(mem); !ok {
				ended = true
				v.primed = false
				break
			}
		}
	}
	v.filter()
}

func clamp16(x int32) int32 { return max(-32768, min(32767, x)) }

// filter runs the voice's frame through its simple (one-pole, 15
// fraction bits) and biquad (14 fraction bits) filters when they're on.
func (v *voice) filter() {
	if v.filters&1 != 0 {
		b0, a1 := int32(v.simple[0]), int32(v.simple[1])
		for i := range v.out {
			for c := 0; c < 2; c++ {
				y := clamp16((b0*v.out[i][c] + a1*v.sy[c]) >> 15)
				v.sy[c], v.out[i][c] = y, y
			}
		}
	}
	if v.filters&2 != 0 {
		a2, a1, b2, b1, b0 := int64(v.biquad[0]), int64(v.biquad[1]), int64(v.biquad[2]), int64(v.biquad[3]), int64(v.biquad[4])
		for i := range v.out {
			for c := 0; c < 2; c++ {
				x := int64(v.out[i][c])
				y := clamp16(int32((b0*x + b1*int64(v.bx[c][0]) + b2*int64(v.bx[c][1]) + a1*int64(v.by[c][0]) + a2*int64(v.by[c][1])) >> 14))
				v.bx[c][1], v.bx[c][0] = v.bx[c][0], int32(x)
				v.by[c][1], v.by[c][0] = v.by[c][0], y
				v.out[i][c] = y
			}
		}
	}
}

// mixInto adds the voice to the three mixers' four channels (front left,
// front right, back left, back right), easing to new gains over the frame.
func (v *voice) mixInto(mixes *[3][samplesInFrame][4]int32) {
	for g := 0; g < 3; g++ {
		to, from := v.gain[g], v.rampFrom[g]
		if !v.ramp[g] {
			from = to
		}
		if to == [4]float32{} && from == to {
			continue
		}
		for i := 0; i < samplesInFrame; i++ {
			t := float32(i) / (samplesInFrame - 1)
			for c := 0; c < 4; c++ {
				gain := from[c] + (to[c]-from[c])*t
				mixes[g][i][c] += int32(gain * float32(v.out[i][c&1]))
			}
		}
		v.rampFrom[g], v.ramp[g] = to, false
	}
}

// mixFrame mixes the three mixers into the final frame. Mixers 1 and 2
// go to the game's aux buffers when it asks for them, and come back from
// there a frame later.
func (a *audio) mixFrame(read, write []byte, mixes *[3][samplesInFrame][4]int32) {
	le := binary.LittleEndian
	cfg := read[offMixConfig:]
	mx := &a.mix
	if dirty := le.Uint32(cfg); dirty != 0 {
		bit := func(n uint) bool { return dirty>>n&1 != 0 }
		if bit(8) {
			mx.aux[0] = le.Uint16(cfg[40:]) != 0
		}
		if bit(9) {
			mx.aux[1] = le.Uint16(cfg[42:]) != 0
		}
		if bit(16) {
			mx.volume[0] = f32(cfg[4:])
		}
		if bit(24) {
			mx.volume[1] = f32(cfg[8:])
		}
		if bit(25) {
			mx.volume[2] = f32(cfg[12:])
		}
		if bit(26) {
			mx.mono = le.Uint16(cfg[22:]) == 0
		}
		le.PutUint32(cfg, 0)
	}
	src := *mixes
	for b := 0; b < 2; b++ {
		if !mx.aux[b] {
			continue
		}
		in := read[offAux+2560*b:]
		outp := write[offAux+2560*b:]
		for c := 0; c < 4; c++ {
			for s := 0; s < samplesInFrame; s++ {
				o := 4 * (c*samplesInFrame + s)
				le.PutUint32(outp[o:], uint32(mixes[b+1][s][c]))
				src[b+1][s][c] = int32(le.Uint32(in[o:]))
			}
		}
	}
	for s := 0; s < samplesInFrame; s++ {
		var l, r float32
		for g := 0; g < 3; g++ {
			q := src[g][s]
			vol := mx.volume[g]
			l += vol * float32(q[0]+q[2])
			r += vol * float32(q[1]+q[3])
		}
		if mx.mono {
			l = (l + r) / 2
			r = l
		}
		mx.frame[s] = [2]int32{clamp16(int32(l)), clamp16(int32(r))}
	}
}

// output resamples the final frame to the output rate.
func (a *audio) output() {
	step := dspRate / a.rate
	f := &a.mix.frame
	for a.resamp < samplesInFrame {
		i := int(a.resamp)
		t := float32(a.resamp - float64(i))
		var cur [2]float32
		for c := 0; c < 2; c++ {
			s0 := a.last[c]
			if i > 0 {
				s0 = float32(f[i-1][c]) / 32768
			}
			s1 := float32(f[i][c]) / 32768
			cur[c] = s0 + (s1-s0)*t
		}
		a.out = append(a.out, cur[0], cur[1])
		a.resamp += step
	}
	a.resamp -= samplesInFrame
	a.last = [2]float32{float32(f[samplesInFrame-1][0]) / 32768, float32(f[samplesInFrame-1][1]) / 32768}
	if len(a.out) > 48000*2 { // nobody is taking it
		a.out = a.out[len(a.out)-48000:]
	}
}

func (a *audio) take() []float32 {
	o := a.out
	a.out = nil
	return o
}
