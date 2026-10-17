// Package nes is the native NES / Famicom emulator: a Go port of
// web/emu/nes.js, nes_mappers.js and nes_audio.js that behaves the same way,
// frame for frame and sample for sample.
//
// Machine = 6502 CPU (cores/cpu6502) + PPU (graphics, emulated dot by dot)
// + APU (sound) + cartridge mapper. Every CPU memory access advances the PPU
// three dots and the APU one cycle, so timing-based effects behave as on hardware.
package nes

import (
	"bytes"
	"errors"
	"fmt"
	"hash/crc32"

	"retropixels/core"
	"retropixels/cores/cpu6502"
)

const (
	cpuHz = 1789773.0 // NTSC CPU clock
	fps   = 60.0988
)

// Buttons in input bit order, as in the web core.
var Buttons = []string{"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"}

// NES is one running game.
type NES struct {
	cpu  *cpu6502.CPU
	ppu  *ppu
	apu  *apu
	cart *cart
	m    mapper
	mb   *base

	// optional parts of the board
	a12 a12Mapper
	nt  ntMapper
	vbl vblanker

	ram            [0x800]byte
	pads, padShift [2]int
	strobe         int
	stall          int
	dmaPage        int
	lastPoll       int64
	openBus        int
	dmcReq         bool
	battery        bool
	romCRC         uint32
	hooks          *ArcadeHooks // an arcade board's additions, or nil
}

// New opens a game (.nes / .unf data).
func New(rom []byte, opts core.Options) (core.Core, error) { return newNES(rom, opts) }

func newNES(rom []byte, opts core.Options) (n *NES, err error) {
	c, err := parseROM(rom)
	if err != nil {
		return nil, err
	}
	initPalette()
	c.vram = make([]byte, 0x1000)
	ctor, ok := mappers[c.mapper]
	if !ok {
		return nil, fmt.Errorf("This NES game uses cartridge hardware RetroPixels doesn't support yet (mapper %d).", c.mapper)
	}
	sr := opts.SampleRate
	if sr <= 0 {
		sr = 48000
	}
	n = &NES{cart: c, dmaPage: -1}
	n.romCRC = crc32.ChecksumIEEE(rom)
	n.cpu = cpu6502.New(n, cpu6502.Options{Decimal: false, MachinePolls: true})
	defer func() {
		if r := recover(); r != nil {
			n, err = nil, fmt.Errorf("This NES game couldn't be started (%v).", r)
		}
	}()
	n.m = ctor(n, c)
	n.mb = n.m.b()
	n.a12, _ = n.m.(a12Mapper)
	n.nt, _ = n.m.(ntMapper)
	n.vbl, _ = n.m.(vblanker)
	bm, hasBat := n.m.(batteryer)
	hasBat = hasBat && bm.batteryData() != nil
	n.battery = c.battery || hasBat
	if c.trainer != nil {
		copy(n.mb.prgRam[0x1000:], c.trainer)
	}
	n.ppu = newPPU(n)
	n.ppu.vram = c.vram
	n.apu = newAPU(n, sr)
	if opts.SRAM != nil && n.battery {
		if hasBat {
			bm.loadBattery(opts.SRAM)
		} else {
			copy(n.mb.prgRam, opts.SRAM)
		}
	}
	n.apu.ext = n.mb.audio // expansion sound on the cartridge
	n.cpu.S = 0            // the stack pointer powers up at 0; the reset sequence moves it to $FD
	n.apu.reset(true)
	n.cpu.Reset()
	return n, nil
}

// clock advances everything by one CPU cycle.
func (n *NES) clock() {
	p := n.ppu
	// The CPU samples its interrupt lines one PPU dot into each cycle.
	p.tick()
	n.cpu.Poll()
	n.lastPoll = p.ticks
	p.tick()
	p.tick()
	n.apu.tick()
	n.m.cpuClock()
	n.cpu.IRQLine = n.apu.irq() || n.m.irq()
}

// Read is the CPU's read cycle (cpu6502.Bus).
func (n *NES) Read(a uint16) uint8 {
	if n.dmcReq {
		n.dmcDma(int(a))
	}
	n.clock()
	v := n.cpuRead(int(a))
	n.openBus = v
	return uint8(v)
}

// Write is the CPU's write cycle (cpu6502.Bus).
func (n *NES) Write(a uint16, v uint8) {
	n.clock()
	n.openBus = int(v)
	n.cpuWrite(int(a), int(v))
}

// peek reads without spending a cycle (used by the DMC).
func (n *NES) peek(a int) int {
	if a < 0x2000 {
		return int(n.ram[a&0x7ff])
	}
	v := n.m.cpuRead(a)
	if v < 0 {
		return n.openBus
	}
	return v
}

func (n *NES) cpuRead(a int) int {
	switch {
	case a < 0x2000:
		return int(n.ram[a&0x7ff])
	case a < 0x4000:
		return n.ppu.cpuRead(a & 7)
	case a == 0x4015:
		return n.apu.readStatus() | n.openBus&0x20
	case a == 0x4016 || a == 0x4017:
		i := a & 1
		var bit int
		if n.strobe != 0 {
			bit = n.pads[i] & 1
		} else {
			bit = n.padShift[i] & 1
			n.padShift[i] = n.padShift[i]>>1 | 0x80
		}
		if n.hooks != nil && n.hooks.Port != nil {
			return n.hooks.Port(a, bit) & 0xff
		}
		return n.openBus&0xe0 | bit
	case a < 0x4020:
		return n.openBus
	}
	v := n.m.cpuRead(a)
	if v < 0 {
		return n.openBus
	}
	return v & 0xff
}

func (n *NES) cpuWrite(a, v int) {
	switch {
	case a < 0x2000:
		n.ram[a&0x7ff] = byte(v)
	case a < 0x4000:
		n.ppu.cpuWrite(a&7, v)
	case a == 0x4014:
		n.dmaPage = v
	case a == 0x4016:
		n.strobe = v & 1
		if n.strobe != 0 {
			n.padShift = n.pads
		}
	case a < 0x4018:
		n.apu.write(a, v)
	default:
		n.m.cpuWrite(a, v)
	}
}

// dmcDma is the sample fetch for the DMC: the CPU is halted on a read cycle and
// keeps repeating that read (which matters for $2007 and the controller
// ports), then the sample byte is fetched.
func (n *NES) dmcDma(a int) {
	n.dmcReq = false
	d := &n.apu.dmc
	if d.buffer >= 0 || d.remain <= 0 {
		return
	}
	pad := a == 0x4016 || a == 0x4017
	stalls := 2 // halt, dummy, and an alignment cycle when needed
	if n.apu.odd {
		stalls = 3
	}
	for i := 0; i < stalls; i++ {
		n.clock()
		n.cpu.Cycles++
		if !pad || i == 0 {
			n.openBus = n.cpuRead(a) // the controller sees one long read
		}
	}
	n.clock()
	n.cpu.Cycles++
	d.take(n.peek(d.addr))
}

// doDma is sprite DMA: the CPU stops while 256 bytes are copied to the PPU.
func (n *NES) doDma() {
	page := n.dmaPage << 8
	n.dmaPage = -1
	n.clock()
	n.cpu.Cycles++
	if n.cpu.Cycles&1 != 0 {
		n.clock()
		n.cpu.Cycles++
	}
	for i := 0; i < 256; i++ {
		if n.dmcReq { // a sample fetch slips in between, costing two cycles
			n.dmcReq = false
			d := &n.apu.dmc
			if d.buffer < 0 && d.remain > 0 {
				n.clock()
				n.cpu.Cycles++
				d.take(n.peek(d.addr))
				n.clock()
				n.cpu.Cycles++
			}
		}
		n.clock()
		n.cpu.Cycles++
		v := n.cpuRead(page | i)
		n.clock()
		n.cpu.Cycles++
		n.ppu.cpuWrite(4, v)
	}
}

// ---- core.Core ----

func (n *NES) Info() core.Info {
	return core.Info{System: "nes", Buttons: Buttons, Players: 2, Aspect: 4.0 / 3}
}
func (n *NES) FPS() float64     { return fps }
func (n *NES) Width() int       { return 256 }
func (n *NES) Height() int      { return 240 }
func (n *NES) Screen() []uint32 { return n.ppu.screen }

// SetInput: bits in the order of Buttons (A, B, Select, Start, Up, Down, Left, Right).
func (n *NES) SetInput(player int, bits uint32) {
	if player < 0 || player >= 2 {
		return
	}
	n.pads[player] = int(bits & 0xff)
	if n.strobe != 0 {
		n.padShift[player] = n.pads[player]
	}
}

func (n *NES) RunFrame() {
	p := n.ppu
	p.frameDone = false
	for guard := 0; !p.frameDone && guard < 200000; guard++ {
		n.cpu.Step()
		if n.dmaPage >= 0 {
			n.doDma()
		}
		for n.stall > 0 {
			n.stall--
			n.clock()
			n.cpu.Cycles++
		}
	}
}

func (n *NES) TakeAudio() []float32 {
	a := append([]float32(nil), n.apu.out...)
	n.apu.out = n.apu.out[:0]
	return a
}

func (n *NES) Reset() {
	if r, ok := n.m.(resetter); ok {
		r.reset()
	}
	n.apu.reset(false)
	n.cpu.Reset()
}

func (n *NES) SRAM() []byte {
	if !n.battery {
		return nil
	}
	if bm, ok := n.m.(batteryer); ok && bm.batteryData() != nil {
		return append([]byte(nil), bm.batteryData()...)
	}
	return append([]byte(nil), n.mb.prgRam...)
}

var stateMagic = []byte("RPNES\x01")

func (n *NES) state(s *sio) {
	cpu := n.cpu.SaveState()
	s.Bytes(cpu)
	if !s.save && s.err == nil {
		if err := n.cpu.LoadState(cpu); err != nil {
			s.err = err
		}
	}
	s.Bytes(n.ram[:])
	s.Bytes(n.cart.vram)
	n.ppu.state(s)
	n.apu.state(s)
	n.m.state(s)
	s.Ints(&n.strobe, &n.padShift[0], &n.padShift[1], &n.stall, &n.dmaPage, &n.openBus)
	s.Bool(&n.dmcReq)
	s.I64(&n.lastPoll)
}

func (n *NES) SaveState() ([]byte, error) {
	s := &sio{save: true, buf: append([]byte(nil), stateMagic...)}
	crc := int(n.romCRC)
	s.Int(&crc)
	n.state(s)
	return s.buf, s.err
}

func (n *NES) LoadState(b []byte) error {
	if !bytes.HasPrefix(b, stateMagic) {
		return errors.New("nes: not an NES saved state")
	}
	s := &sio{buf: b, pos: len(stateMagic)}
	crc := 0
	s.Int(&crc)
	if s.err != nil || uint32(crc) != n.romCRC {
		return errors.New("nes: this saved state is from a different game")
	}
	// Restore into a copy first, so a damaged state leaves the game untouched.
	backup, err := n.SaveState()
	if err != nil {
		return err
	}
	n.state(s)
	if s.err == nil && s.pos != len(b) {
		s.err = errState
	}
	if s.err != nil {
		r := &sio{buf: backup, pos: len(stateMagic) + 8}
		n.state(r)
		return s.err
	}
	return nil
}
