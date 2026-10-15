package atari2600

// The TIA sound generator and the RIOT, shared with the Atari 7800 core (as
// web/emu/atari7800.js uses them from atari2600.js).

// TIASound is the TIA's two sound channels with the web core's resampler.
type TIASound struct{ s tiaSound }

// NewTIASound makes a TIA sound generator stepped inRate times a second
// (Tick), producing sampleRate samples a second.
func NewTIASound(sampleRate int, inRate float64) *TIASound {
	t := &TIASound{}
	t.s.rate = float64(sampleRate)
	t.s.inRate = inRate
	return t
}

// Write sets a sound register: r 0 AUDC0, 1 AUDC1, 2 AUDF0, 3 AUDF1, 4 AUDV0, 5 AUDV1.
func (t *TIASound) Write(r int, v uint8) { t.s.write(r, v) }

// Tick runs the generator one step and stores one raw sample.
func (t *TIASound) Tick() { t.s.tick() }

// AddToLast adds v to the last raw sample (as the 7800 mixes its POKEYs in:
// raw[rawLen-1] += v on a Float32Array).
func (t *TIASound) AddToLast(v float64) {
	i := t.s.rawLen - 1
	t.s.raw[i] = float32(float64(t.s.raw[i]) + v)
}

// Take returns the samples made since the last call, at the output rate.
func (t *TIASound) Take() []float32 { return t.s.take() }

// AppendState adds the generator's state to b.
func (t *TIASound) AppendState(b []byte) []byte {
	w := &stateW{b: b}
	t.s.save(w)
	return w.b
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest.
func (t *TIASound) LoadState(b []byte) ([]byte, error) {
	n := t.s
	r := &stateR{b: b}
	n.load(r)
	if r.err != nil {
		return nil, r.err
	}
	t.s = n
	return r.b, nil
}

// RIOT is the 6532: 128 bytes of RAM, two I/O ports and the interval timer.
type RIOT struct{ r riot }

// NewRIOT makes a RIOT as it is at power-on.
func NewRIOT() *RIOT { return &RIOT{r: *newRIOT()} }

// RAM is the chip's 128 bytes of RAM.
func (r *RIOT) RAM() *[128]uint8 { return &r.r.ram }

// SetPorts sets what the controllers put on ports A and B.
func (r *RIOT) SetPorts(a, b uint8) { r.r.portA, r.r.portB = a, b }

// Tick runs the timer one processor clock (of the 2600's 1.19 MHz).
func (r *RIOT) Tick() { r.r.tick() }

// Read reads an I/O or timer register.
func (r *RIOT) Read(a int) uint8 { return r.r.read(a) }

// Write writes an I/O or timer register.
func (r *RIOT) Write(a int, v uint8) { r.r.write(a, v) }

// AppendState adds the chip's state to b (not the ports, which the machine sets).
func (r *RIOT) AppendState(b []byte) []byte {
	w := &stateW{b: b}
	r.r.save(w)
	return w.b
}

// LoadState reads a state written by AppendState from the front of b and
// returns the rest.
func (r *RIOT) LoadState(b []byte) ([]byte, error) {
	n := r.r
	s := &stateR{b: b}
	n.load(s)
	if s.err != nil {
		return nil, s.err
	}
	r.r = n
	return s.b, nil
}
