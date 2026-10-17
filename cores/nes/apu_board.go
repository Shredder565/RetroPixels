package nes

// APU is the RP2A03's sound part on its own, for arcade boards that use the
// chip as a sound processor without the rest of the console (Nintendo's
// Donkey Kong 3). It is the console's APU unchanged: the board's CPU calls
// Clock once a cycle and passes its writes to $4000-$4017 to Write.
type APU struct {
	host       *NES // holds the DMC's fetch request
	a          *apu
	sampleRate int
	raw        bool
	// Fetch reads a sample byte for the DMC from the board's memory (nil:
	// the DMC reads 0).
	Fetch func(addr uint16) uint8
}

// NewAPU makes an APU, powered on, giving sampleRate samples a second.
func NewAPU(sampleRate int) *APU {
	if sampleRate <= 0 {
		sampleRate = 48000
	}
	x := &APU{host: &NES{}, sampleRate: sampleRate}
	x.Power()
	return x
}

// Power puts the APU in its power-on state.
func (x *APU) Power() {
	x.host.dmcReq = false
	x.a = newAPU(x.host, x.sampleRate)
	x.a.raw = x.raw
	x.a.noise.everyCycle = x.raw
	if x.raw {
		x.a.tri.step = 15 // a triangle that hasn't played yet adds nothing (as MAME has it)
	}
	x.a.reset(true)
}

// Reset is the chip's reset line: the sound is turned off and the frame
// counter starts again (its mode as last written).
func (x *APU) Reset() {
	x.host.dmcReq = false
	x.a.reset(false)
}

// Write is a CPU write to $4000-$4013, $4015 or $4017.
func (x *APU) Write(addr uint16, v uint8) { x.a.write(int(addr), int(v)) }

// ReadStatus is a CPU read of $4015.
func (x *APU) ReadStatus() uint8 { return uint8(x.a.readStatus()) }

// IRQ: the frame counter's or the DMC's interrupt is asserted.
func (x *APU) IRQ() bool { return x.a.irq() }

// Clock runs one CPU cycle. It returns how many cycles the CPU is halted
// for a DMC sample fetch (0 when there's none); the caller runs Clock for
// those cycles too.
func (x *APU) Clock() int {
	x.a.tick()
	if !x.host.dmcReq {
		return 0
	}
	x.host.dmcReq = false
	d := &x.a.dmc
	if d.buffer >= 0 || d.remain <= 0 {
		return 0
	}
	v := uint8(0)
	if x.Fetch != nil {
		v = x.Fetch(uint16(d.addr))
	}
	d.take(int(v))
	return 4
}

// SetRaw: true gives the chip's own mixed output (DC included, no filtering),
// for boards with their own output stage, and runs the noise channel's
// timer at the rate its period table gives (the console core runs it at
// half that, as web/emu/nes.js does); false (the default) is the console's
// behaviour.
func (x *APU) SetRaw(raw bool) {
	x.raw = raw
	x.a.raw = raw
	x.a.noise.everyCycle = raw
}

// TakeAudio returns the samples made since the last call.
func (x *APU) TakeAudio() []float32 {
	out := append([]float32(nil), x.a.out...)
	x.a.out = x.a.out[:0]
	return out
}

// SaveState returns the APU's state.
func (x *APU) SaveState() []byte {
	s := &sio{save: true}
	x.a.state(s)
	return s.buf
}

// LoadState restores a state made by SaveState.
func (x *APU) LoadState(b []byte) error {
	s := &sio{buf: b}
	x.a.state(s)
	if s.err == nil && s.pos != len(b) {
		return errState
	}
	x.host.dmcReq = false
	x.a.out = x.a.out[:0]
	return s.err
}
