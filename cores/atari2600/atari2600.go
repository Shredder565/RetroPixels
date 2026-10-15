// Package atari2600 is the native Atari 2600 emulator: a Go port of
// web/emu/atari2600.js, atari2600_carts.js and atari2600_dpcplus.js that
// behaves the same way, frame for frame and sample for sample.
//
// 6507 processor (the shared 6502 core with decimal mode), TIA video and sound,
// RIOT (RAM, I/O, timer); the cartridges are in carts.go and dpcplus.go.
// The TIA picture is drawn lazily: pixels are produced up to "now" whenever a
// register that affects them changes and at the end of every line, so the
// result is exact to the colour clock without simulating every clock one by one.
package atari2600

import (
	"bytes"
	"compress/flate"
	"errors"
	"io"
	"math"

	"retropixels/core"
	"retropixels/cores/cpu6502"
)

const (
	clock      = 3579545 // colour clocks per second (NTSC)
	lineClocks = 228     // colour clocks per line
	hblank     = 68
	maxLines   = 330 // lines kept per frame
	outH       = 210 // lines shown
)

// FPS is the frame rate (NTSC, 262 lines).
const FPS = float64(clock) / float64(lineClocks*262)

// ------------------------------------------------------------------
// NTSC palette, computed from the TIA's colour signal: 16 hues (hue 0 is
// grey, hues 1-15 step around the colour wheel from the burst phase) and
// 8 brightness levels. Entries are 0x00RRGGBB.
// ------------------------------------------------------------------
var palette = makePalette()

func makePalette() [128]uint32 {
	luma := [8]float64{0.0, 0.25, 0.4, 0.53, 0.65, 0.75, 0.84, 0.92}
	var out [128]uint32
	c := func(x float64) uint32 {
		v := math.Floor(x*255 + 0.5) // Math.round
		return uint32(math.Max(0, math.Min(255, v)))
	}
	for h := 0; h < 16; h++ {
		for l := 0; l < 8; l++ {
			y, u, v := luma[l], 0.0, 0.0
			if h != 0 {
				deg := float64(167 - (h-1)*27) // hue 1 (the colour burst) is yellow
				a := deg * math.Pi / 180
				const sat = 0.3
				u = math.Cos(a) * sat
				v = math.Sin(a) * sat
				y = 0.1 + float64(y*0.85) // colour levels never reach black
			}
			r := c(y + float64(1.14*v))
			g := c(y - float64(0.395*u) - float64(0.581*v))
			b := c(y + float64(2.032*u))
			out[h*8+l] = r<<16 | g<<8 | b
		}
	}
	return out
}

// ------------------------------------------------------------------
// TIA sound: two channels, clocked twice per line (~31.4 kHz).
// ------------------------------------------------------------------
func polyBits(n int, taps []int) []uint8 {
	out := make([]uint8, (1<<n)-1)
	r := (1 << n) - 1
	for i := range out {
		out[i] = uint8(r & 1)
		fb := 0
		for _, t := range taps {
			fb ^= (r >> t) & 1
		}
		r = (r >> 1) | (fb << (n - 1))
	}
	return out
}

var (
	poly4 = polyBits(4, []int{0, 1}) // period 15
	poly5 = polyBits(5, []int{0, 2}) // period 31
	poly9 = polyBits(9, []int{0, 4}) // period 511
	div31 = func() (d [31]uint8) { d[0], d[18] = 1, 1; return }()
)

type tiaSound struct {
	rate, inRate                           float64
	audc, audf, audv, div, p4, p5, p9, out [2]int
	raw                                    [4096]float32
	rawLen                                 int
	frac, last, hp, prev                   float64
	buf                                    [4096]float32
	bufLen                                 int
}

func newTIASound(sampleRate int) *tiaSound {
	return &tiaSound{rate: float64(sampleRate), inRate: float64(clock) / 114}
}

// write: r 0 AUDC0, 1 AUDC1, 2 AUDF0, 3 AUDF1, 4 AUDV0, 5 AUDV1
func (s *tiaSound) write(r int, v uint8) {
	ch := r & 1
	switch {
	case r < 2:
		s.audc[ch] = int(v & 15)
	case r < 4:
		s.audf[ch] = int(v & 31)
	default:
		s.audv[ch] = int(v & 15)
	}
}

// one ~31.4 kHz step
func (s *tiaSound) tick() {
	mix := 0
	for ch := 0; ch < 2; ch++ {
		c := s.audc[ch]
		top := s.audf[ch]
		if c&0x0c == 0x0c {
			top = (s.audf[ch]+1)*3 - 1
		}
		s.div[ch]++
		if s.div[ch] > top {
			s.div[ch] = 0
			if c == 0 || c == 0x0b {
				s.out[ch] = 1
			} else {
				s.p5[ch] = (s.p5[ch] + 1) % 31
				p5 := s.p5[ch]
				if c&2 == 0 || (c&1 == 0 && div31[p5] != 0) || (c&1 != 0 && poly5[p5] != 0) {
					if c&4 != 0 {
						s.out[ch] ^= 1
					} else if c&8 != 0 {
						if c == 8 {
							s.p9[ch] = (s.p9[ch] + 1) % 511
							s.out[ch] = int(poly9[s.p9[ch]])
						} else {
							s.out[ch] = int(poly5[p5])
						}
					} else {
						s.p4[ch] = (s.p4[ch] + 1) % 15
						s.out[ch] = int(poly4[s.p4[ch]])
					}
				}
			}
		}
		if s.out[ch] != 0 {
			mix += s.audv[ch]
		}
	}
	if s.rawLen < len(s.raw) {
		s.raw[s.rawLen] = float32(float64(mix) / 30)
		s.rawLen++
	}
}

// flush converts what was produced this frame to the output rate.
func (s *tiaSound) flush() {
	step := s.inRate / s.rate
	pos := s.frac
	n := s.rawLen
	fn := float64(n)
	for pos < fn {
		i := int(pos)
		f := pos - float64(i)
		a := s.last
		if i != 0 {
			a = float64(s.raw[i-1])
		}
		b := float64(s.raw[i])
		x := a + float64((b-a)*f)
		s.hp = 0.995 * (s.hp + x - s.prev)
		s.prev = x
		if s.bufLen < len(s.buf) {
			s.buf[s.bufLen] = float32(s.hp * 0.9)
			s.bufLen++
		}
		pos += step
	}
	s.frac = pos - fn
	if n != 0 {
		s.last = float64(s.raw[n-1])
	}
	s.rawLen = 0
}

func (s *tiaSound) take() []float32 {
	s.flush()
	a := make([]float32, s.bufLen)
	copy(a, s.buf[:s.bufLen])
	s.bufLen = 0
	return a
}

func (s *tiaSound) save(w *stateW) {
	for _, arr := range []*[2]int{&s.audc, &s.audf, &s.audv, &s.div, &s.p4, &s.p5, &s.p9, &s.out} {
		w.ints(arr[:])
	}
	w.f64(s.frac)
	w.f64(s.last)
	w.f64(s.hp)
	w.f64(s.prev)
}

func (s *tiaSound) load(r *stateR) {
	for _, arr := range []*[2]int{&s.audc, &s.audf, &s.audv, &s.div, &s.p4, &s.p5, &s.p9, &s.out} {
		r.intsInto(arr[:])
	}
	s.frac = r.f64()
	s.last = r.f64()
	s.hp = r.f64()
	s.prev = r.f64()
	s.rawLen = 0
	s.bufLen = 0
}

// ------------------------------------------------------------------
// RIOT (6532): 128 bytes of RAM, two I/O ports, interval timer.
// ------------------------------------------------------------------
type riot struct {
	ram                  [128]uint8
	timer                uint8
	interval, sub        int
	flag, pa7            bool
	ddra, ddrb, ora, orb uint8
	portA, portB         uint8 // set by the machine (inputs)
}

func newRIOT() *riot {
	r := &riot{timer: 0xff, interval: 1024, sub: 1024, portA: 0xff, portB: 0x0b}
	for i := range r.ram {
		r.ram[i] = uint8(i*73 + 41) // power-on garbage
	}
	return r
}

func (r *riot) tick() {
	r.sub--
	if r.sub <= 0 {
		r.timer--
		if r.timer == 0xff {
			r.flag = true
		}
		if r.flag {
			r.sub = 1
		} else {
			r.sub = r.interval
		}
	}
}

func (r *riot) read(a int) uint8 {
	if a&4 != 0 {
		if a&1 != 0 {
			var v uint8
			if r.flag {
				v |= 0x80
			}
			if r.pa7 {
				v |= 0x40
			}
			r.pa7 = false
			return v
		}
		r.flag = false
		return r.timer
	}
	switch a & 3 {
	case 0:
		return (r.ora & r.ddra) | (r.portA &^ r.ddra)
	case 1:
		return r.ddra
	case 2:
		return (r.orb & r.ddrb) | (r.portB &^ r.ddrb)
	default:
		return r.ddrb
	}
}

func (r *riot) write(a int, v uint8) {
	if a&4 != 0 {
		if a&0x10 != 0 {
			r.interval = [4]int{1, 8, 64, 1024}[a&3]
			r.timer = v
			r.sub = 1
			r.flag = false
		}
		return
	}
	switch a & 3 {
	case 0:
		r.ora = v
	case 1:
		r.ddra = v
	case 2:
		r.orb = v
	default:
		r.ddrb = v
	}
}

func (r *riot) save(w *stateW) {
	w.bytes(r.ram[:])
	w.u8(r.timer)
	w.int(r.interval)
	w.int(r.sub)
	w.bool(r.flag)
	w.bool(r.pa7)
	w.u8(r.ddra)
	w.u8(r.ddrb)
	w.u8(r.ora)
	w.u8(r.orb)
}

func (r *riot) load(s *stateR) {
	s.bytesInto(r.ram[:])
	r.timer = s.u8()
	r.interval = s.int()
	r.sub = s.int()
	r.flag = s.bool()
	r.pa7 = s.bool()
	r.ddra = s.u8()
	r.ddrb = s.u8()
	r.ora = s.u8()
	r.orb = s.u8()
}

// ------------------------------------------------------------------
// The console
// ------------------------------------------------------------------

// Button bits (see Info().Buttons).
const (
	btnUp     = 1
	btnDown   = 2
	btnLeft   = 4
	btnRight  = 8
	btnFire   = 16
	btnSelect = 32
	btnReset  = 64
	btnLDiff  = 128
	btnRDiff  = 256
	btnBW     = 512
)

var buttons = []string{"Up", "Down", "Left", "Right", "Fire", "Select", "Reset", "Left difficulty", "Right difficulty", "Color / B&W"}

// Atari is one running Atari 2600 game.
type Atari struct {
	romData    []byte
	name       string
	sram       []byte
	sampleRate int

	cart    cart
	snoop   snooper
	watcher watcher
	tiaHook tiaHooker

	riot  *riot
	sound *tiaSound
	tia   *tia
	cpu   *cpu6502.CPU

	cycles int64
	bus    uint8

	input        [2]uint32
	analog       [2]float64
	paddle       [4]float64
	paddleMode   bool
	paddleReads  int
	paddleFrames int

	isOp, opNext bool
}

var _ core.Core = (*Atari)(nil)
var _ core.Analog = (*Atari)(nil)

// New starts a game. The scheme is detected from the file (and a scheme tag
// in opts.Name such as "(F8SC)" or "[3E+]").
func New(rom []byte, opts core.Options) (core.Core, error) {
	if len(rom) == 0 {
		return nil, errors.New("this Atari 2600 game file is empty")
	}
	m := &Atari{romData: append([]byte(nil), rom...), name: opts.Name, sampleRate: opts.SampleRate}
	if m.sampleRate <= 0 {
		m.sampleRate = 48000
	}
	if len(opts.SRAM) > 0 {
		m.sram = append([]byte(nil), opts.SRAM...)
	}
	if err := m.init(); err != nil {
		return nil, err
	}
	return m, nil
}

func (m *Atari) init() error {
	c, err := createCart(m.romData, m.name, m.sram)
	if err != nil {
		return err
	}
	m.cart = c
	m.snoop, _ = c.(snooper)
	m.watcher, _ = c.(watcher)
	m.tiaHook, _ = c.(tiaHooker)
	m.riot = newRIOT()
	m.sound = newTIASound(m.sampleRate)
	m.tia = newTIA(m)
	m.cycles = 0
	m.bus = 0
	m.input = [2]uint32{}
	m.analog = [2]float64{}
	m.paddle = [4]float64{0.5, 0.5, 0.5, 0.5}
	m.paddleMode = false
	m.paddleReads = 0
	m.paddleFrames = 0
	m.isOp, m.opNext = false, false
	m.cpu = cpu6502.New(m, cpu6502.Options{Decimal: true})
	m.updatePorts()
	m.cpu.Reset()
	return nil
}

func (m *Atari) cycle() {
	m.cycles++
	m.tia.clock3()
	m.riot.tick()
}

// Read is the 6507's bus (one call per cycle).
func (m *Atari) Read(addr uint16) uint8 {
	t := m.tia
	if t.wsync {
		t.wsync = false
		for t.hclock != 0 {
			m.cycle()
		}
	}
	m.cycle()
	a := int(addr) & 0x1fff
	m.isOp = m.opNext // is this the opcode fetch of an instruction?
	m.opNext = false
	if m.snoop != nil {
		m.snoop.access(a)
	}
	var v uint8
	if a&0x1000 != 0 {
		v = m.cart.read(a, m)
	} else if a&0x80 != 0 {
		if a&0x200 != 0 {
			v = m.riot.read(a)
		} else {
			v = m.riot.ram[a&0x7f]
		}
	} else {
		v = (t.read(a) & 0xc0) | (m.bus & 0x3f)
		if a&15 >= 8 && a&15 < 12 {
			m.paddleReads++
		}
	}
	if m.watcher != nil {
		m.watcher.watch(a, v)
	}
	m.bus = v
	return v
}

// Write is the 6507's bus (one call per cycle).
func (m *Atari) Write(addr uint16, v uint8) {
	m.cycle()
	a := int(addr) & 0x1fff
	m.bus = v
	m.isOp, m.opNext = false, false
	if m.snoop != nil {
		m.snoop.access(a)
	}
	if a&0x1000 != 0 {
		m.cart.write(a, v, m)
	} else if a&0x80 != 0 {
		if a&0x200 != 0 {
			m.riot.write(a, v)
		} else {
			m.riot.ram[a&0x7f] = v
		}
	} else {
		m.tia.write(a&0x3f, v)
		if m.tiaHook != nil && a < 0x40 {
			m.tiaHook.tiaWrite(a, v)
		}
	}
	if m.watcher != nil {
		m.watcher.watch(a, v)
	}
}

// ---- controllers ----

func (m *Atari) fireDown(p int) bool { return !m.paddleMode && m.input[p]&btnFire != 0 }

func (m *Atari) paddleBit(i int) uint8 {
	t := m.tia
	if t.vblank&0x80 != 0 {
		return 0
	}
	// the paddle's capacitor charges at a rate set by the knob
	need := 76 * (40 + float64((1-m.paddle[i])*120))
	if float64(m.cycles-t.dumpStart) >= need {
		return 0x80
	}
	return 0
}

func (m *Atari) updatePorts() {
	a := uint8(0xff)
	dirs := func(b uint32) uint8 {
		var d uint8
		if b&btnUp != 0 {
			d |= 1
		}
		if b&btnDown != 0 {
			d |= 2
		}
		if b&btnLeft != 0 {
			d |= 4
		}
		if b&btnRight != 0 {
			d |= 8
		}
		return d
	}
	if m.paddleMode {
		if m.input[0]&btnFire != 0 {
			a &^= 0x80
		}
		if m.input[1]&btnFire != 0 {
			a &^= 0x40
		}
	} else {
		a &^= dirs(m.input[0]) << 4
		a &^= dirs(m.input[1])
	}
	m.riot.portA = a
	s := m.input[0]
	b := uint8(0x34)
	if s&btnReset == 0 {
		b |= 1
	}
	if s&btnSelect == 0 {
		b |= 2
	}
	if s&btnBW == 0 {
		b |= 8
	}
	if s&btnLDiff != 0 {
		b |= 0x40
	}
	if s&btnRDiff != 0 {
		b |= 0x80
	}
	m.riot.portB = b
}

// SetInput sets player p's buttons (bit i = Info().Buttons[i]); the console
// switches are read from player 0.
func (m *Atari) SetInput(p int, bits uint32) {
	if p >= 0 && p < 2 {
		m.input[p] = bits
		m.updatePorts()
	}
}

// SetAnalog turns player p's paddle knob. As in the web core, v is the stick's
// X axis, -1..1 (it is mapped to the knob's 0..1 travel); |v| <= 0.1 counts as
// "no analog input", and then Left / Right turn the knob. Only axis 0 is used.
func (m *Atari) SetAnalog(p, axis int, v float32) {
	if p >= 0 && p < 2 && axis == 0 {
		m.analog[p] = float64(v)
	}
}

// setAnalog64 is SetAnalog with the web host's double value (for tests).
func (m *Atari) setAnalog64(p int, v float64) {
	if p >= 0 && p < 2 {
		m.analog[p] = v
	}
}

func (m *Atari) movePaddles() {
	for p := 0; p < 2; p++ {
		x := m.analog[p]
		if math.Abs(x) > 0.1 {
			m.paddle[p] = math.Max(0, math.Min(1, (x+1)/2))
		} else {
			if m.input[p]&btnLeft != 0 {
				m.paddle[p] = math.Max(0, m.paddle[p]-0.02)
			}
			if m.input[p]&btnRight != 0 {
				m.paddle[p] = math.Min(1, m.paddle[p]+0.02)
			}
		}
	}
}

// ---- core.Core ----

// Info describes the console's controls and picture.
func (m *Atari) Info() core.Info {
	return core.Info{System: "atari2600", Buttons: append([]string(nil), buttons...), Players: 2, Aspect: 4.0 / 3.0}
}

// FPS is the NTSC frame rate.
func (m *Atari) FPS() float64 { return FPS }

// Width of the picture.
func (m *Atari) Width() int { return 160 }

// Height of the picture.
func (m *Atari) Height() int { return outH }

// Screen is the last finished frame, 160x210, 0x00RRGGBB.
func (m *Atari) Screen() []uint32 { return m.tia.screen }

// RunFrame runs until the game starts its next frame (VSYNC), or for at most
// 400 lines. A DPC+ game whose ARM code never returns makes it panic with an
// error, as the web core throws.
func (m *Atari) RunFrame() {
	t := m.tia
	t.frameDone = false
	if m.paddleMode {
		m.movePaddles()
	}
	m.paddleReads = 0
	limit := m.cycles + 76*400
	for !t.frameDone {
		m.opNext = true
		m.cpu.Step()
		if m.cycles > limit {
			t.endFrame()
			break
		}
	}
	// Paddle games read the paddle ports constantly; joystick games never do.
	if !m.paddleMode {
		if m.paddleReads > 40 {
			m.paddleFrames++
			if m.paddleFrames >= 2 {
				m.paddleMode = true
				m.updatePorts()
			}
		} else {
			m.paddleFrames = 0
		}
	}
}

// TakeAudio returns the mono samples made since the last call.
func (m *Atari) TakeAudio() []float32 { return m.sound.take() }

// Reset is the console's power switch: the game starts again (keeping its
// battery save and the buttons held).
func (m *Atari) Reset() {
	if s := m.cart.sram(); s != nil {
		m.sram = s
	}
	keep := m.input
	if err := m.init(); err != nil {
		// The same file was accepted before; this cannot happen.
		panic(err)
	}
	m.input = keep
	m.updatePorts()
}

// SRAM is the battery save (FA2 cartridges' flash), or nil.
func (m *Atari) SRAM() []byte { return m.cart.sram() }

const stateMagic = "RP2600\x01"

// SaveState captures the whole machine.
func (m *Atari) SaveState() ([]byte, error) {
	w := &stateW{}
	w.b = append(w.b, stateMagic...)
	w.bytes(m.cpu.SaveState())
	m.riot.save(w)
	m.tia.save(w)
	m.sound.save(w)
	m.cart.save(w)
	w.i64(m.cycles)
	w.u8(m.bus)
	w.bool(m.paddleMode)
	for _, p := range m.paddle {
		w.f64(p)
	}
	w.int(m.paddleFrames)
	var out bytes.Buffer
	fw, _ := flate.NewWriter(&out, flate.BestSpeed)
	fw.Write(w.b)
	fw.Close()
	return out.Bytes(), nil
}

// LoadState restores a state made by SaveState for the same game.
func (m *Atari) LoadState(data []byte) error {
	raw, err := io.ReadAll(flate.NewReader(bytes.NewReader(data)))
	if err != nil || len(raw) < len(stateMagic) || string(raw[:len(stateMagic)]) != stateMagic {
		return errBadState
	}
	// Load into a copy first so a damaged state leaves the game as it was.
	n := *m
	n.riot = new(riot)
	*n.riot = *m.riot
	n.tia = m.tia.clone(&n)
	n.sound = new(tiaSound)
	*n.sound = *m.sound
	n.cart = m.cart.clone()
	n.snoop, _ = n.cart.(snooper)
	n.watcher, _ = n.cart.(watcher)
	n.tiaHook, _ = n.cart.(tiaHooker)
	n.cpu = cpu6502.New(&n, cpu6502.Options{Decimal: true})

	r := &stateR{b: raw[len(stateMagic):]}
	cpuState := r.bytes()
	if r.err == nil {
		if err := n.cpu.LoadState(cpuState); err != nil {
			return errBadState
		}
	}
	n.riot.load(r)
	n.tia.load(r)
	n.sound.load(r)
	n.cart.load(r)
	n.cycles = r.i64()
	n.bus = r.u8()
	n.paddleMode = r.bool()
	for i := range n.paddle {
		n.paddle[i] = r.f64()
	}
	n.paddleFrames = r.int()
	if r.err != nil || len(r.b) != 0 {
		return errBadState
	}
	*m = n
	m.tia.m = m
	m.cpu.SetBus(m)
	m.updatePorts()
	return nil
}
