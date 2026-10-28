// Package snes is the Super Nintendo emulator: a Go port of web/emu/snes.js
// (with w65816.js, spc700.js, sdsp.js and the cartridge chips in the
// snes_*.js files) that behaves exactly like it, frame for frame and sample
// for sample.
package snes

import (
	"math"
	"time"

	"retropixels/core"
	"retropixels/cores/w65816"
)

const (
	lineClocks = 1364 // master clocks per scan line
	hblankAt   = 1096 // render point / HDMA
	ntscHz     = 21477272
	palHz      = 21281370
	stateMagic = "RPSNES1"
)

// Buttons are the controller's buttons in input bit order (BUTTONS in snes.js).
var Buttons = []string{"R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"}

var dmaPattern = [8][]int{{0}, {0, 1}, {0, 0}, {0, 0, 1, 1}, {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0}, {0, 0, 1, 1}}

type hdmaCh struct {
	active, doTransfer bool
	line, addr, ind    int
}

// SNES is one running game.
type SNES struct {
	cart      *cart
	chipName  string
	chipPages [4096]bool
	coproc    coproc
	gsu       *superFX
	sa1       *sa1

	pal        bool
	masterHz   float64
	lines      int
	fps        float64
	spcRatio   float64
	sampleRate int

	wram [0x20000]uint8
	apu  *apu
	ppu  *ppu
	cpu  *w65816.CPU

	clock, cost            int
	dma                    [0x80]uint8
	tr                     *tracer // debug tracer (EnableTrace), nil when off
	hdma                   [8]hdmaCh
	input                  [2]int
	width, height          int
	screen                 []uint32
	out                    []float32
	line, lineStart        int
	frameStart             int
	vblank, hblank         bool
	mdr                    uint8
	nmitimen, wrio, wrmpya int
	wrdiv, htime, vtime    int
	memsel, hdmaen         int
	nmiFlag, irqFlag       bool
	autoJoyUntil           int
	rddiv, rdmpy           int
	joy                    [4]uint16
	ser                    [2]int
	strobe, wmadd          int
	rsPos                  float64
	prevL, prevR           int

	now func() float64 // the computer's clock in ms (Date.now), for the clock chips
}

func wallClock() float64 { return float64(time.Now().UnixMilli()) }

// New opens a game.
func New(rom []byte, opts core.Options) (core.Core, error) { return newSNES(rom, opts, wallClock) }

type cpuBus struct{ s *SNES }

func (b cpuBus) Read(a uint32) uint8 {
	s := b.s
	v := s.decode(int(a), false, 0)
	s.clock += s.cost
	s.mdr = v
	return v
}

func (b cpuBus) Write(a uint32, v uint8) {
	s := b.s
	s.decode(int(a), true, v)
	s.clock += s.cost
	s.mdr = v
}

func (b cpuBus) Idle() { b.s.clock += 6 }

func newSNES(rom []byte, opts core.Options, now func() float64) (*SNES, error) {
	s := &SNES{now: now}
	s.cart = newCart(rom, opts.SRAM)
	chip := s.cart.special()
	if chip == "" {
		if e := findChip(s.cart); e != nil && e.create != nil {
			c, err := e.create(s, &chipOpts{sram: opts.SRAM, firmware: opts.Firmware})
			if err != nil {
				return nil, err
			}
			if c == nil {
				return nil, errUnsupported(e.name)
			}
			s.coproc = c
		}
	}
	if chip != "" && chip != "SA-1" && chip != "Super FX" {
		return nil, errUnsupported(chip)
	}
	s.chipName = chip
	s.pal = s.cart.pal
	s.masterHz = ntscHz
	s.lines = 262
	if s.pal {
		s.masterHz = palHz
		s.lines = 312
	}
	s.fps = s.masterHz / float64(lineClocks*s.lines)
	s.spcRatio = 1024000 / s.masterHz
	s.sampleRate = opts.SampleRate
	if s.sampleRate == 0 {
		s.sampleRate = 48000
	}
	s.apu = newAPU()
	s.ppu = newPPU(s)
	s.cpu = w65816.New(cpuBus{s})
	if chip == "SA-1" {
		s.sa1 = newSA1(s)
	}
	s.width, s.height = 256, 224
	s.screen = s.ppu.frame256[:]
	s.out = make([]float32, 8192)
	if chip == "Super FX" {
		s.gsu = newSuperFX(s.cart, s, opts.SRAM)
	}
	s.Reset()
	return s, nil
}

// Reset is the console's reset button.
func (s *SNES) Reset() {
	for i := range s.wram {
		s.wram[i] = 0x55
	}
	if s.tr != nil {
		s.tr.forget()
	}
	s.apu.reset()
	s.ppu.reset()
	for i := range s.dma {
		s.dma[i] = 0xff
	}
	s.clock, s.line, s.lineStart, s.frameStart = 0, 0, 0, 0
	s.vblank, s.hblank, s.mdr = false, false, 0
	s.nmitimen, s.wrio, s.wrmpya, s.wrdiv, s.htime, s.vtime = 0, 0xff, 0xff, 0xffff, 0x1ff, 0x1ff
	s.memsel, s.hdmaen, s.nmiFlag, s.irqFlag, s.autoJoyUntil = 0, 0, false, false, 0
	s.rddiv, s.rdmpy, s.joy, s.ser, s.strobe = 0, 0, [4]uint16{}, [2]int{}, 0
	s.wmadd = 0
	s.rsPos, s.prevL, s.prevR = 0, 0, 0
	if s.sa1 != nil {
		s.sa1.reset()
	}
	if s.coproc != nil {
		s.coproc.reset()
	}
	s.cpu.Reset()
	if s.gsu != nil {
		s.gsu.reset()
	}
}

func (s *SNES) updateIrq() { s.setIrqLine() }

// ---------- bus ----------

// decode reads (or writes) addr and sets s.cost.
func (s *SNES) decode(addr int, write bool, v uint8) uint8 {
	bank, off := addr>>16, addr&0xffff
	if s.chipPages[addr>>12] {
		var r int
		if write {
			if s.coproc.write(bank, off, v) {
				r = 0
			} else {
				r = -1
			}
		} else {
			r = s.coproc.read(bank, off)
		}
		if r >= 0 {
			if bank&0x80 != 0 && (off&0x8000 != 0 || bank >= 0xc0) && s.memsel&1 != 0 {
				s.cost = 6
			} else {
				s.cost = 8
			}
			return uint8(r)
		}
	}
	if bank&0x40 == 0 {
		if off < 0x2000 {
			s.cost = 8
			if write {
				s.wram[off] = v
				if s.tr != nil {
					s.tr.wram(off)
				}
				return 0
			}
			return s.wram[off]
		}
		if off < 0x4000 {
			s.cost = 6
			if s.gsu != nil && off >= 0x3000 && off < 0x3500 {
				if write {
					s.gsu.regWrite(off, v)
					return 0
				}
				return s.gsu.regRead(off)
			}
			if off >= 0x2100 && off < 0x2200 {
				if write {
					s.writeB(off&0xff, v)
					return 0
				}
				return s.readB(off & 0xff)
			}
			if s.sa1 != nil && off >= 0x2200 {
				return s.sa1.snesIO(off, write, v)
			}
			if write {
				return 0
			}
			return s.mdr
		}
		if off < 0x4200 {
			s.cost = 12
			if write {
				if off == 0x4016 {
					s.writeStrobe(v)
				}
				return 0
			}
			return s.readSerial(off)
		}
		if off < 0x6000 {
			s.cost = 6
			if write {
				s.writeIO(off, v)
				return 0
			}
			return s.readIO(off)
		}
		if off >= 0x8000 && bank&0x80 != 0 && s.memsel&1 != 0 {
			s.cost = 6
		} else {
			s.cost = 8
		}
	} else if bank == 0x7e || bank == 0x7f {
		s.cost = 8
		a := (bank&1)<<16 | off
		if write {
			s.wram[a] = v
			if s.tr != nil {
				s.tr.wram(a)
			}
			return 0
		}
		return s.wram[a]
	} else if bank >= 0xc0 && s.memsel&1 != 0 {
		s.cost = 6
	} else {
		s.cost = 8
	}
	if s.gsu != nil {
		if write {
			s.gsu.snesWrite(bank, off, v)
			return 0
		}
		g := s.gsu.snesRead(bank, off)
		if g < 0 {
			return s.mdr
		}
		return uint8(g)
	}
	if write {
		s.cart.write(bank, off, v)
		return 0
	}
	r := s.cart.read(bank, off)
	if r < 0 {
		return s.mdr
	}
	return uint8(r)
}

func isBReg(bank, off int) bool {
	return bank&0x40 == 0 && ((off >= 0x2100 && off < 0x2200) || (off >= 0x4000 && off < 0x4400))
}

// A-bus access used by DMA (timing is counted by the DMA itself)
func (s *SNES) peekA(addr int) uint8 {
	if isBReg(addr>>16, addr&0xffff) {
		return s.mdr
	}
	return s.decode(addr, false, 0)
}

func (s *SNES) pokeA(addr int, v uint8) {
	if isBReg(addr>>16, addr&0xffff) {
		return
	}
	s.decode(addr, true, v)
}

func (s *SNES) syncApu() { s.apu.run(int(math.Floor(float64(s.clock) * s.spcRatio))) }

func (s *SNES) readB(r int) uint8 {
	if r >= 0x34 && r < 0x40 {
		return s.ppu.read(r)
	}
	if r >= 0x40 && r < 0x80 {
		s.syncApu()
		return s.apu.portOut[r&3]
	}
	if r == 0x80 {
		v := s.wram[s.wmadd]
		s.wmadd = (s.wmadd + 1) & 0x1ffff
		return v
	}
	return s.mdr
}

func (s *SNES) writeB(r int, v uint8) {
	if r < 0x34 {
		s.ppu.write(r, v)
		return
	}
	if r >= 0x40 && r < 0x80 {
		s.syncApu()
		s.apu.portIn[r&3] = v
		return
	}
	switch r {
	case 0x80:
		s.wram[s.wmadd] = v
		if s.tr != nil {
			s.tr.wram(s.wmadd)
		}
		s.wmadd = (s.wmadd + 1) & 0x1ffff
	case 0x81:
		s.wmadd = s.wmadd&0x1ff00 | int(v)
	case 0x82:
		s.wmadd = s.wmadd&0x100ff | int(v)<<8
	case 0x83:
		s.wmadd = s.wmadd&0xffff | int(v&1)<<16
	}
}

// ---------- joypads ----------

func (s *SNES) writeStrobe(v uint8) {
	st := int(v & 1)
	if s.strobe != 0 && st == 0 {
		s.ser[0], s.ser[1] = s.joyBits(0), s.joyBits(1)
	}
	s.strobe = st
	if st != 0 {
		s.ser[0], s.ser[1] = s.joyBits(0), s.joyBits(1)
	}
}

func (s *SNES) readSerial(off int) uint8 {
	if off != 0x4016 && off != 0x4017 {
		return s.mdr
	}
	p := off - 0x4016
	bit := (s.ser[p] >> 15) & 1
	if s.strobe == 0 {
		s.ser[p] = (s.ser[p]<<1 | 1) & 0xffff
	}
	v := int(s.mdr)&0xfc | bit
	if p != 0 {
		v |= 0x1c
	}
	return uint8(v)
}

func (s *SNES) joyBits(p int) int { return (s.input[p] << 4) & 0xfff0 }

// ---------- CPU I/O registers ----------

func (s *SNES) readIO(off int) uint8 {
	if off >= 0x4300 && off < 0x4380 {
		return s.dma[off-0x4300]
	}
	switch off {
	case 0x4210:
		v := 0x02 | s.mdr&0x70
		if s.nmiFlag {
			v |= 0x80
		}
		s.nmiFlag = false
		return v
	case 0x4211:
		v := s.mdr & 0x7f
		if s.irqFlag {
			v |= 0x80
		}
		s.irqFlag = false
		s.setIrqLine()
		return v
	case 0x4212:
		pos := s.clock - s.lineStart
		v := s.mdr & 0x3e
		if s.vblank {
			v |= 0x80
		}
		if pos >= hblankAt || pos < 4 {
			v |= 0x40
		}
		if s.clock < s.autoJoyUntil {
			v |= 1
		}
		return v
	case 0x4213:
		return uint8(s.wrio)
	case 0x4214:
		return uint8(s.rddiv)
	case 0x4215:
		return uint8(s.rddiv >> 8)
	case 0x4216:
		return uint8(s.rdmpy)
	case 0x4217:
		return uint8(s.rdmpy >> 8)
	case 0x4218, 0x4219, 0x421a, 0x421b, 0x421c, 0x421d, 0x421e, 0x421f:
		k := (off - 0x4218) >> 1
		if off&1 != 0 {
			return uint8(s.joy[k] >> 8)
		}
		return uint8(s.joy[k])
	}
	return s.mdr
}

func (s *SNES) writeIO(off int, val uint8) {
	v := int(val)
	if off >= 0x4300 && off < 0x4380 {
		s.dma[off-0x4300] = val
		if s.tr != nil {
			s.tr.reg(off - 0x4300)
		}
		return
	}
	switch off {
	case 0x4200:
		was := s.nmitimen
		s.nmitimen = v
		if was&0x80 == 0 && v&0x80 != 0 && s.nmiFlag && s.vblank {
			s.cpu.NMIPending = true
		}
		if v&0x30 == 0 {
			s.irqFlag = false
			s.setIrqLine()
		}
	case 0x4201:
		if s.wrio&0x80 != 0 && v&0x80 == 0 {
			s.ppu.latchCounters()
		}
		s.wrio = v
	case 0x4202:
		s.wrmpya = v
	case 0x4203:
		s.rdmpy = s.wrmpya * v
		s.rddiv = v
	case 0x4204:
		s.wrdiv = s.wrdiv&0xff00 | v
	case 0x4205:
		s.wrdiv = s.wrdiv&0xff | v<<8
	case 0x4206:
		if v == 0 {
			s.rddiv = 0xffff
			s.rdmpy = s.wrdiv
		} else {
			s.rddiv = s.wrdiv / v
			s.rdmpy = s.wrdiv % v
		}
	case 0x4207:
		s.htime = s.htime&0x100 | v
	case 0x4208:
		s.htime = s.htime&0xff | (v&1)<<8
	case 0x4209:
		s.vtime = s.vtime&0x100 | v
	case 0x420a:
		s.vtime = s.vtime&0xff | (v&1)<<8
	case 0x420b:
		if v != 0 {
			s.runDma(v)
		}
	case 0x420c:
		s.hdmaen = v
	case 0x420d:
		s.memsel = v
	}
}

// ---------- DMA ----------

func (s *SNES) runDma(mask int) {
	s.clock += 8
	d := &s.dma
	for ch := 0; ch < 8; ch++ {
		if mask&(1<<ch) == 0 {
			continue
		}
		o := ch * 16
		ctrl, bbad := int(d[o]), int(d[o+1])
		a1t := int(d[o+2]) | int(d[o+3])<<8
		a1b := int(d[o+4])
		count := int(d[o+5]) | int(d[o+6])<<8
		if count == 0 {
			count = 0x10000
		}
		pat := dmaPattern[ctrl&7]
		step := 1
		if ctrl&8 != 0 {
			step = 0
		} else if ctrl&0x10 != 0 {
			step = -1
		}
		s.clock += 8
		var feed func() uint8
		if f, ok := s.coproc.(dmaFeeder); ok && s.coproc != nil && ctrl&0x80 == 0 {
			feed = f.dmaFeed(ch, a1b<<16|a1t, count)
		}
		tr := s.tr
		if tr != nil {
			tr.dmaBegin(ch, ctrl, bbad, a1b<<16|a1t, count)
		}
		i := 0
		for count > 0 {
			b := (bbad + pat[i%len(pat)]) & 0xff
			a := a1b<<16 | a1t
			if tr != nil && ctrl&0x80 == 0 && feed == nil {
				tr.from = a
			}
			if ctrl&0x80 != 0 {
				s.pokeA(a, s.readB(b))
			} else if feed != nil {
				s.writeB(b, feed())
			} else {
				s.writeB(b, s.peekA(a))
			}
			s.clock += 8
			a1t = (a1t + step) & 0xffff
			count--
			i++
		}
		if tr != nil {
			tr.dmaEnd()
		}
		d[o+2], d[o+3] = uint8(a1t), uint8(a1t>>8)
		d[o+5], d[o+6] = 0, 0
	}
}

func (s *SNES) hdmaInit() {
	d := &s.dma
	for ch := 0; ch < 8; ch++ {
		h := &s.hdma[ch]
		h.active = false
		if s.hdmaen&(1<<ch) == 0 {
			continue
		}
		o := ch * 16
		h.addr = int(d[o+2]) | int(d[o+3])<<8
		s.hdmaLoad(ch)
		s.clock += 8
	}
}

func (s *SNES) hdmaLoad(ch int) {
	d, o, h := &s.dma, ch*16, &s.hdma[ch]
	bank := int(d[o+4]) << 16
	h.line = int(s.peekA(bank | h.addr))
	h.addr = (h.addr + 1) & 0xffff
	if d[o]&0x40 != 0 {
		lo, hi := s.peekA(bank|h.addr), s.peekA(bank|(h.addr+1)&0xffff)
		h.addr = (h.addr + 2) & 0xffff
		d[o+5], d[o+6] = lo, hi
		s.clock += 16
	}
	h.active = h.line != 0
	h.doTransfer = true
	d[o+8], d[o+9], d[o+10] = uint8(h.addr), uint8(h.addr>>8), uint8(h.line)
}

func (s *SNES) hdmaLine() {
	d := &s.dma
	any := false
	if s.tr != nil {
		s.tr.inHdma = true
		defer func() { s.tr.inHdma = false }()
	}
	for ch := 0; ch < 8; ch++ {
		h := &s.hdma[ch]
		if s.hdmaen&(1<<ch) == 0 || !h.active {
			continue
		}
		any = true
		o := ch * 16
		ctrl, bbad := int(d[o]), int(d[o+1])
		if h.doTransfer {
			pat := dmaPattern[ctrl&7]
			for i := range pat {
				var a int
				if ctrl&0x40 != 0 {
					ia := int(d[o+5]) | int(d[o+6])<<8
					a = int(d[o+7])<<16 | ia
					n := (ia + 1) & 0xffff
					d[o+5], d[o+6] = uint8(n), uint8(n>>8)
				} else {
					a = int(d[o+4])<<16 | h.addr
					h.addr = (h.addr + 1) & 0xffff
				}
				b := (bbad + pat[i]) & 0xff
				if ctrl&0x80 != 0 {
					s.pokeA(a, s.readB(b))
				} else {
					s.writeB(b, s.peekA(a))
				}
				s.clock += 8
			}
		}
		h.line = (h.line - 1) & 0xff
		h.doTransfer = h.line&0x80 != 0
		if h.line&0x7f == 0 {
			s.hdmaLoad(ch)
		}
		d[o+8], d[o+9], d[o+10] = uint8(h.addr), uint8(h.addr>>8), uint8(h.line)
	}
	if any {
		s.clock += 18
	}
}

// ---------- running ----------

func (s *SNES) runTo(t int) {
	cpu := s.cpu
	if s.sa1 != nil {
		for s.clock < t {
			cpu.Step()
			s.sa1.catchUp(s.clock)
		}
		return
	}
	if s.gsu != nil {
		for s.clock < t {
			end := min(t, s.clock+256)
			for s.clock < end {
				cpu.Step()
			}
			s.gsu.run(s.clock)
		}
		return
	}
	for s.clock < t {
		cpu.Step()
	}
}

func (s *SNES) fireIrq() {
	s.irqFlag = true
	s.setIrqLine()
}

// setIrqLine: the CPU's IRQ input is the timer IRQ, or a cartridge chip (SA-1,
// Super FX) asking for attention.
func (s *SNES) setIrqLine() {
	s.cpu.IRQLine = s.irqFlag || (s.sa1 != nil && s.sa1.snesIrq) || (s.gsu != nil && s.gsu.irqOut())
}

// RunFrame runs one frame.
func (s *SNES) RunFrame() {
	p := s.ppu
	visible := 224
	if p.setini&4 != 0 {
		visible = 239
	}
	vbl := visible + 1
	p.hiresUsed = false
	for line := 0; line < s.lines; line++ {
		s.line = line
		s.lineStart = s.frameStart + line*lineClocks
		if s.clock < s.lineStart {
			s.clock = s.lineStart
		}
		if line == 0 {
			s.vblank, s.nmiFlag = false, false
			p.timeOver, p.rangeOver = false, false
			p.field ^= 1
			s.hdmaInit()
		}
		if line == vbl {
			s.vblank, s.nmiFlag = true, true
			p.vblankStart()
			if s.nmitimen&0x80 != 0 {
				s.cpu.NMIPending = true
			}
			if s.nmitimen&1 != 0 {
				for k := 0; k < 2; k++ {
					s.joy[k] = uint16(s.joyBits(k))
				}
				s.autoJoyUntil = s.clock + 4224
			}
		}
		s.clock += 40 // memory refresh
		mode := (s.nmitimen >> 4) & 3
		irqAt := -1
		switch {
		case mode == 1:
			irqAt = s.htime*4 + 14
		case mode == 2 && line == s.vtime:
			irqAt = 10
		case mode == 3 && line == s.vtime:
			irqAt = s.htime*4 + 14
		}
		if irqAt >= 0 && irqAt < hblankAt {
			s.runTo(s.lineStart + irqAt)
			s.fireIrq()
		}
		s.runTo(s.lineStart + hblankAt)
		s.hblank = true
		if line >= 1 && line <= visible {
			p.renderLine(line)
		}
		if line < vbl {
			s.hdmaLine()
		}
		if irqAt >= hblankAt {
			s.runTo(s.lineStart + irqAt)
			s.fireIrq()
		}
		s.runTo(s.lineStart + lineClocks)
		s.hblank = false
	}
	s.frameStart += lineClocks * s.lines
	if s.tr != nil {
		s.tr.frame++
	}
	s.syncApu()
	s.height = visible
	if p.hiresUsed {
		s.width = 512
		s.screen = p.frame512[:]
	} else {
		s.width = 256
		s.screen = p.frame256[:]
	}
}

// SetInput sets the buttons held on controller player (bit i = Buttons[i]).
func (s *SNES) SetInput(player int, bits uint32) {
	if player >= 0 && player < 2 {
		s.input[player] = int(bits & 0xfff)
	}
}

// TakeAudio returns the stereo samples made since the last call, resampled
// from the DSP's 32 kHz to the output rate.
func (s *SNES) TakeAudio() []float32 {
	a := s.apu
	n, src := a.nSamp>>1, a.samples[:]
	ratio := 32040 / float64(s.sampleRate)
	maxOut := int(math.Ceil(float64(n)/ratio)) + 2
	if len(s.out) < maxOut*2 {
		s.out = make([]float32, maxOut*2)
	}
	out := s.out
	k, pos := 0, s.rsPos
	for pos < float64(n) {
		i := int(math.Floor(pos))
		f := pos - float64(i)
		var l0, r0 float64
		if i == 0 {
			l0, r0 = float64(s.prevL), float64(s.prevR)
		} else {
			l0, r0 = float64(src[(i-1)*2]), float64(src[(i-1)*2+1])
		}
		out[k] = float32((l0 + (float64(src[i*2])-l0)*f) / 32768)
		out[k+1] = float32((r0 + (float64(src[i*2+1])-r0)*f) / 32768)
		k += 2
		pos += ratio
	}
	s.rsPos = pos - float64(n)
	if n != 0 {
		s.prevL, s.prevR = int(src[(n-1)*2]), int(src[(n-1)*2+1])
	}
	a.nSamp = 0
	return append([]float32(nil), out[:k]...)
}

// SRAM is the battery save (with any clock data the chips keep), or nil.
func (s *SNES) SRAM() []byte {
	if s.gsu != nil {
		return s.gsu.getSRAM()
	}
	if c, ok := s.coproc.(sramChip); ok && s.coproc != nil {
		return c.getSRAM()
	}
	if s.cart.sramSize != 0 {
		return append([]byte(nil), s.cart.sram[:s.cart.sramSize]...)
	}
	return nil
}

// Info describes the controls and picture.
func (s *SNES) Info() core.Info {
	return core.Info{System: "snes", Buttons: Buttons, Players: 2, Stereo: true, Aspect: 4.0 / 3}
}

// FPS is the frame rate (60.1 NTSC, 50.0 PAL).
func (s *SNES) FPS() float64 { return s.fps }

// Width of the picture (512 when the game used a high-resolution mode this frame).
func (s *SNES) Width() int { return s.width }

// Height of the picture: 224 or 239 lines.
func (s *SNES) Height() int { return s.height }

// Screen is the picture, Width*Height pixels.
func (s *SNES) Screen() []uint32 { return s.screen[:s.width*s.height] }

func (s *SNES) state(st *sio) {
	cs := s.cpu.GetState()
	for i := range cs {
		st.Int(&cs[i])
	}
	if !st.save {
		s.cpu.SetState(cs)
	}
	st.Bytes(s.wram[:])
	s.apu.state(st)
	s.ppu.state(st)
	st.Bytes(s.cart.sram)
	st.Bytes(s.dma[:])
	for i := range s.hdma {
		h := &s.hdma[i]
		st.Bools(&h.active, &h.doTransfer)
		st.Ints(&h.line, &h.addr, &h.ind)
	}
	st.U16s(s.joy[:])
	if s.gsu != nil {
		s.gsu.state(st)
	}
	if s.coproc != nil {
		s.coproc.state(st)
	}
	st.Ints(&s.clock, &s.frameStart)
	st.U8(&s.mdr)
	st.Ints(&s.nmitimen, &s.wrio, &s.wrmpya, &s.wrdiv, &s.htime, &s.vtime, &s.memsel, &s.hdmaen)
	st.Bools(&s.nmiFlag, &s.irqFlag)
	st.Ints(&s.autoJoyUntil, &s.rddiv, &s.rdmpy, &s.ser[0], &s.ser[1], &s.strobe, &s.wmadd)
	st.F64(&s.rsPos)
	st.Ints(&s.prevL, &s.prevR)
	if s.sa1 != nil {
		s.sa1.state(st)
	}
	// (not in the web core's state, which is only taken between frames; kept
	// so a state loaded into a fresh game continues exactly)
	st.Ints(&s.line, &s.lineStart)
	st.Bools(&s.vblank, &s.hblank)
}

// SaveState returns the whole machine state.
func (s *SNES) SaveState() ([]byte, error) {
	st := &sio{save: true, buf: []byte(stateMagic)}
	s.state(st)
	return st.buf, nil
}

// LoadState restores a state from SaveState.
func (s *SNES) LoadState(b []byte) error {
	if len(b) < len(stateMagic) || string(b[:len(stateMagic)]) != stateMagic {
		return errState
	}
	// load into a copy first, so a bad state leaves the game as it was
	old, err := s.SaveState()
	if err != nil {
		return err
	}
	st := &sio{buf: b, pos: len(stateMagic)}
	s.state(st)
	if st.err == nil && st.pos != len(b) {
		st.err = errState
	}
	if st.err != nil {
		s.state(&sio{buf: old, pos: len(stateMagic)})
		return st.err
	}
	if s.sa1 != nil {
		s.setIrqLine()
	}
	s.apu.nSamp = 0
	return nil
}
