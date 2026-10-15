// Package sms is the native Sega Master System / Mark III core: a Go port of
// web/emu/sms.js that plays exactly like it (same picture and sound, frame
// for frame).
//
// Machine: the Zilog Z80 (cores/z80), the SMS VDP in mode 4 and the older
// TMS9918 modes, the SN76489 PSG (cores/sn76489), two pads plus the console's
// Pause and Reset buttons, and the Sega cartridge mapper with battery-backed
// cartridge RAM (also the Codemasters mapper). The BIOS isn't needed: the
// cartridge starts directly.
package sms

import (
	"errors"
	"math"

	"retropixels/core"
	"retropixels/cores/sn76489"
	"retropixels/cores/z80"
)

const (
	cpuHz    = 3579545
	lineCyc  = 228 // processor cycles per scan line
	active   = 171 // ...of which the picture takes this many
	audioDiv = 64  // processor cycles per native audio sample (the PSG ticks every 16)
	fps      = float64(cpuHz) / (lineCyc * 262)
)

// Buttons: input bit i is Buttons[i] (as in sms.js).
var Buttons = []string{"Up", "Down", "Left", "Right", "1", "2", "Pause", "Reset"}

const (
	bUp = 1 << iota
	bDown
	bLeft
	bRight
	b1
	b2
	bPause
	bReset
)

// isCodemasters: Codemasters carts carry their own header at $7FE0 with a
// checksum and its complement.
func isCodemasters(rom []uint8) bool {
	if len(rom) < 0x8000 {
		return false
	}
	sum := int(rom[0x7fe6]) | int(rom[0x7fe7])<<8
	inv := int(rom[0x7fe8]) | int(rom[0x7fe9])<<8
	return sum != 0 && (sum+inv)&0xffff == 0
}

// SMS is one running Master System.
type SMS struct {
	rom         []uint8
	nBanks      int
	codemasters bool
	sampleRate  int

	ram         [0x2000]uint8
	cartRam     [0x8000]uint8
	cartRamUsed bool
	cmRam       bool

	vdp vdp
	psg *sn76489.PSG
	cpu *z80.CPU

	height int
	screen []uint32
	input  [2]uint32

	ctrl           uint8
	slot           [3]int
	port3F, hLatch uint8

	line, cyc, lineStart, psgCyc int
	sampleAcc                    float64
	sampleN                      int
	pauseWas                     bool

	audio                       []float32 // native-rate samples not yet taken
	rsPos, prev, lp, hp, hpPrev float64
	out                         []float32
}

// bus is what the processor sees.
type bus struct{ s *SMS }

func (b bus) Read(a uint16) uint8     { return b.s.read(int(a)) }
func (b bus) Write(a uint16, v uint8) { b.s.write(int(a), v) }
func (b bus) In(p uint16) uint8       { return b.s.portIn(uint8(p)) }
func (b bus) Out(p uint16, v uint8)   { b.s.portOut(uint8(p), v) }
func (b bus) Vector() uint8           { return 0xff }

// New starts a Master System game.
func New(romIn []byte, opts core.Options) (core.Core, error) {
	if len(romIn) == 0 {
		return nil, errors.New("this Master System game file is empty")
	}
	data := romIn
	// Some dumps carry a 512-byte copier header.
	if len(data)%0x4000 == 512 {
		data = data[512:]
	}
	size := 0x4000
	for size < len(data) {
		size <<= 1
	}
	s := &SMS{rom: make([]uint8, size), sampleRate: opts.SampleRate}
	copy(s.rom, data)
	n := len(data)
	if n < 1 {
		n = 1
	}
	for i := len(data); i < size; i++ {
		s.rom[i] = s.rom[i%n]
	}
	s.nBanks = size >> 14
	s.codemasters = isCodemasters(s.rom)
	if s.sampleRate <= 0 {
		s.sampleRate = 48000
	}
	if len(opts.SRAM) > 0 {
		copy(s.cartRam[:], opts.SRAM)
		s.cartRamUsed = true
	}
	s.vdp.sms = s
	s.psg = sn76489.New()
	s.cpu = z80.New(bus{s})
	s.height = 192
	s.screen = make([]uint32, 256*240)
	s.audio = make([]float32, 0, 4096)
	s.Reset()
	return s, nil
}

// Reset is the console's power button (as sms.js's reset()).
func (s *SMS) Reset() {
	s.ram = [0x2000]uint8{}
	s.vdp.reset()
	s.psg.Reset()
	s.cpu.Reset()
	s.ctrl = 0
	s.slot = [3]int{0, 1, 2}
	if s.codemasters {
		s.slot = [3]int{0, 1, 0}
	}
	s.port3F, s.hLatch = 0xff, 0
	s.line, s.cyc, s.psgCyc, s.sampleAcc, s.sampleN = 0, 0, 0, 0, 0
	s.pauseWas = false
	s.rsPos, s.prev, s.lp, s.hp, s.hpPrev = 0, 0, 0, 0, 0
	// As the BIOS leaves it: the stack near the top of RAM.
	r := s.cpu.Regs()
	r.SP = 0xdff0
	s.cpu.SetRegs(r)
}

// ---- memory ----

func (s *SMS) read(a int) uint8 {
	if a >= 0xc000 {
		return s.ram[a&0x1fff]
	}
	if a < 0x400 && !s.codemasters {
		return s.rom[a]
	}
	if a >= 0x8000 && s.ctrl&8 != 0 {
		return s.cartRam[int(s.ctrl&4)<<12|(a&0x3fff)]
	}
	if s.codemasters && a >= 0xa000 && a < 0xc000 && s.cmRam {
		return s.cartRam[a&0x1fff]
	}
	return s.rom[(s.slot[a>>14]%s.nBanks)*0x4000+(a&0x3fff)]
}

func (s *SMS) write(a int, v uint8) {
	if a >= 0xc000 {
		s.ram[a&0x1fff] = v
		if a >= 0xfffc && !s.codemasters {
			if a == 0xfffc {
				s.ctrl = v
			} else {
				s.slot[a-0xfffd] = int(v)
			}
		}
		return
	}
	if s.codemasters {
		switch {
		case a == 0x0000:
			s.slot[0] = int(v)
		case a == 0x4000:
			s.slot[1] = int(v & 0x7f)
			s.cmRam = v&0x80 != 0
		case a == 0x8000:
			s.slot[2] = int(v)
		case s.cmRam && a >= 0xa000 && a < 0xc000:
			s.cartRam[a&0x1fff] = v
			s.cartRamUsed = true
		}
		return
	}
	if a >= 0x8000 && s.ctrl&8 != 0 {
		s.cartRam[int(s.ctrl&4)<<12|(a&0x3fff)] = v
		s.cartRamUsed = true
	}
}

// ---- I/O ports ----

func (s *SMS) portIn(p uint8) uint8 {
	switch p & 0xc1 {
	case 0x00, 0x01:
		return 0xff
	case 0x40:
		return s.vCounter()
	case 0x41:
		return s.hLatch
	case 0x80:
		return s.vdp.readData()
	case 0x81:
		return s.vdp.readStatus()
	case 0xc0: // pad 1, and up / down of pad 2
		a, b := s.input[0], s.input[1]
		v := uint8(0xff)
		if a&bUp != 0 {
			v &^= 1
		}
		if a&bDown != 0 {
			v &^= 2
		}
		if a&bLeft != 0 {
			v &^= 4
		}
		if a&bRight != 0 {
			v &^= 8
		}
		if a&b1 != 0 {
			v &^= 0x10
		}
		if a&b2 != 0 {
			v &^= 0x20
		}
		if b&bUp != 0 {
			v &^= 0x40
		}
		if b&bDown != 0 {
			v &^= 0x80
		}
		return v
	case 0xc1: // the rest of pad 2, the Reset button and the TH lines
		b := s.input[1]
		v := uint8(0x3f)
		if b&bLeft != 0 {
			v &^= 1
		}
		if b&bRight != 0 {
			v &^= 2
		}
		if b&b1 != 0 {
			v &^= 4
		}
		if b&b2 != 0 {
			v &^= 8
		}
		if s.input[0]&bReset != 0 {
			v &^= 0x10
		}
		// TH pins: what the game drives on them when they're outputs (an export console)
		f := s.port3F
		if f&2 != 0 {
			v |= 0x40
		} else {
			v |= (f & 0x20) << 1
		}
		if f&8 != 0 {
			v |= 0x80
		} else {
			v |= f & 0x80
		}
		return v
	}
	return 0xff
}

func th(x uint8) uint8 {
	var a, b uint8
	if x&2 != 0 {
		a = 1
	} else {
		a = (x >> 5) & 1
	}
	if x&8 != 0 {
		b = 1
	} else {
		b = (x >> 7) & 1
	}
	return a | b<<1
}

func (s *SMS) portOut(p, v uint8) {
	switch p & 0xc1 {
	case 0x01: // I/O control: a TH line going high latches the H counter
		old := s.port3F
		s.port3F = v
		if ^th(old)&th(v) != 0 {
			s.hLatch = s.hCounter()
		}
	case 0x40, 0x41:
		s.syncAudio()
		s.psg.Write(v)
	case 0x80:
		s.vdp.writeData(v)
	case 0x81:
		s.vdp.writeControl(v)
	}
}

func (s *SMS) vCounter() uint8 {
	l := s.line
	if s.vdp.lines() == 224 {
		if l > 0xea {
			l -= 6
		}
	} else if l > 0xda {
		l -= 6
	}
	return uint8(l)
}

// hCounter: the H counter only changes the value games can read when a TH
// line latches it.
func (s *SMS) hCounter() uint8 {
	d := (s.cyc - s.lineStart) * 171
	h := d / lineCyc
	if d%lineCyc != 0 && d < 0 { // Math.floor
		h--
	}
	if h >= 0x94 {
		h += 0x55
	}
	return uint8(h)
}

func (s *SMS) updateIrq() { s.cpu.IRQ = s.vdp.irqLine() }

// ---- sound ----

func (s *SMS) syncAudio() {
	for s.psgCyc+16 <= s.cyc {
		s.psgCyc += 16
		s.sampleAcc += s.psg.Step()
		s.sampleN++
		if s.sampleN == audioDiv/16 {
			s.audio = append(s.audio, float32(s.sampleAcc/float64(s.sampleN)*0.25))
			s.sampleAcc, s.sampleN = 0, 0
		}
	}
}

// TakeAudio returns the sound made since the last call, at the sample rate
// asked for: softened, without the DC offset, and resampled as sms.js does.
func (s *SMS) TakeAudio() []float32 {
	A := s.audio
	n := len(A)
	ratio := float64(cpuHz) / audioDiv / float64(s.sampleRate)
	for i := 0; i < n; i++ { // soften aliasing, then remove the DC offset
		s.lp += (float64(A[i]) - s.lp) * 0.5
		s.hp = 0.999 * (s.hp + s.lp - s.hpPrev)
		s.hpPrev = s.lp
		A[i] = float32(s.hp)
	}
	out := s.out[:0]
	pos := s.rsPos
	for pos < float64(n) {
		i := int(math.Floor(pos))
		f := pos - float64(i)
		a := s.prev
		if i != 0 {
			a = float64(A[i-1])
		}
		out = append(out, float32(a+(float64(A[i])-a)*f))
		pos += ratio
	}
	s.rsPos = pos - float64(n)
	if n > 0 {
		s.prev = float64(A[n-1])
	}
	s.audio = A[:0]
	s.out = out
	return append([]float32(nil), out...)
}

// ---- running ----

// RunFrame runs one frame (262 lines).
func (s *SMS) RunFrame() {
	v, cpu := &s.vdp, s.cpu
	// Pause is wired to the processor's NMI: pressing it interrupts once.
	pause := s.input[0]&bPause != 0
	if pause && !s.pauseWas {
		cpu.NMI()
	}
	s.pauseWas = pause
	H := v.lines()
	s.height = H
	for line := 0; line < 262; line++ {
		s.line, s.lineStart = line, s.cyc
		if line == 0 {
			v.vscroll = int(v.reg[9]) // vertical scroll is taken once per frame
		}
		if line < H {
			v.renderLine(line, s.screen, 256)
		}
		// the visible part of the line; the line and frame interrupts come at
		// its end, so a handler running in the horizontal blank changes the next line
		end := s.lineStart + active
		for s.cyc < end {
			s.cyc += cpu.Step()
		}
		v.lineDone(line)
		if line == H {
			v.status |= 0x80 // frame interrupt at the end of the picture
		}
		s.updateIrq()
		end = s.lineStart + lineCyc
		for s.cyc < end {
			s.cyc += cpu.Step()
		}
	}
	s.syncAudio()
	// keep the counters small
	s.psgCyc -= s.cyc
	s.cyc = 0
}

func (s *SMS) Info() core.Info {
	return core.Info{System: "sms", Buttons: Buttons, Players: 2, Aspect: 4.0 / 3}
}
func (s *SMS) FPS() float64     { return fps }
func (s *SMS) Width() int       { return 256 }
func (s *SMS) Height() int      { return s.height }
func (s *SMS) Screen() []uint32 { return s.screen[:256*s.height] }

// SetInput: bits in the order of Buttons. The console's Pause and Reset
// buttons are on player 1's bits.
func (s *SMS) SetInput(player int, bits uint32) {
	if player >= 0 && player < 2 {
		s.input[player] = bits
	}
}

// SRAM is the cartridge's battery RAM (32 KB), once the game has used it.
func (s *SMS) SRAM() []byte {
	if !s.cartRamUsed {
		return nil
	}
	return append([]byte(nil), s.cartRam[:]...)
}
