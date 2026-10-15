// Package atari7800 is the native Atari 7800 emulator: a Go port of
// web/emu/atari7800.js that behaves the same way, frame for frame and sample
// for sample.
//
// 6502 ("Sally") processor, MARIA graphics chip (display-list DMA, all six
// graphics modes, character mode, holey DMA, display-list interrupts and the
// processor time MARIA's DMA takes), TIA sound, RIOT, POKEY sound and the
// cartridge boards the .a78 header (version 3 or 4) describes: SuperGame
// banking with RAM, banked RAM, mirrored RAM or an extra ROM bank at $4000,
// Absolute, Activision, bankset boards, and POKEYs at $4000, $0440, $0450
// (two at once) or $0800. The console's BIOS is not needed: the cartridge
// starts directly in 7800 mode. The TIA sound generator and the RIOT are the
// Atari 2600 core's.
package atari7800

import (
	"bytes"
	"compress/flate"
	"errors"
	"io"
	"math"

	"retropixels/core"
	"retropixels/cores/atari2600"
	"retropixels/cores/cpu6502"
)

const (
	mclkRate = 7159090 // MARIA clock
	lineClk  = 454     // MARIA clocks per line
	lines    = 263
	dmaFirst = 16
	dmaLast  = 258
	outTop   = 17
	outW     = 320
	outH     = 240
)

// FPS is the frame rate (NTSC, 263 lines).
const FPS = float64(mclkRate) / float64(lineClk*lines)

// palette: 16 hues x 16 brightness levels, 0x00RRGGBB; grey is the same with
// every hue turned to grey (colour kill).
var palette, grey = makePalette()

func makePalette() (pal, gr [256]uint32) {
	c := func(x float64) uint32 {
		v := math.Floor(x*255 + 0.5) // Math.round
		return uint32(math.Max(0, math.Min(255, v)))
	}
	for h := 0; h < 16; h++ {
		for l := 0; l < 16; l++ {
			y, u, v := float64(l)/15, 0.0, 0.0
			if h != 0 {
				a := float64(167-(h-1)*27) * math.Pi / 180 // hue 1 yellow; hue 15 near orange
				u = float64(math.Cos(a) * 0.3)
				v = float64(math.Sin(a) * 0.3)
				y = 0.1 + float64(y*0.85)
			} else {
				y = float64(y * 0.93)
			}
			r := c(y + float64(1.14*v))
			g := c(y - float64(0.395*u) - float64(0.581*v))
			b := c(y + float64(2.032*u))
			pal[h*16+l] = r<<16 | g<<8 | b
		}
	}
	for i := range gr {
		gr[i] = pal[i&15]
	}
	return
}

// Button bits (see Info().Buttons).
const (
	btnUp     = 1
	btnDown   = 2
	btnLeft   = 4
	btnRight  = 8
	btnB1     = 16
	btnB2     = 32
	btnPause  = 64
	btnSelect = 128
	btnReset  = 256
	btnLDiff  = 512
	btnRDiff  = 1024
)

var buttons = []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Pause", "Select", "Reset", "Left difficulty", "Right difficulty"}

// Atari is one running Atari 7800 game.
type Atari struct {
	data       []byte
	sampleRate int

	cart  *cart
	cpu   *cpu6502.CPU
	riot  *atari2600.RIOT
	sound *atari2600.TIASound

	clk                 int64 // MARIA clocks since power-on
	ram                 [0x1000]uint8
	maria               [32]uint8 // $20-$3F
	ctrl, charbase      uint8
	dpp                 int
	line, mclk          int
	dmaDone, lineHadDma bool
	audClk, riotFrac    int
	wsync, frameDone    bool
	dll, dl, zoneOff    int
	h16, h8             bool
	wmode               uint8
	zoneActive          bool
	lb                  [512]uint8
	tiaVblank           uint8
	latch               [2]bool
	input               [2]uint32
	screen              []uint32
	pokeyMix            float64
	pokeyList           []*pokey
}

var _ core.Core = (*Atari)(nil)

// New starts a game (an .a78 file, or a headerless dump).
func New(rom []byte, opts core.Options) (core.Core, error) {
	if len(rom) == 0 {
		return nil, errors.New("this Atari 7800 game file is empty")
	}
	m := &Atari{data: append([]byte(nil), rom...), sampleRate: opts.SampleRate}
	if m.sampleRate <= 0 {
		m.sampleRate = 48000
	}
	m.screen = make([]uint32, outW*outH)
	if err := m.init(); err != nil {
		return nil, err
	}
	return m, nil
}

func (m *Atari) init() error {
	c, err := newCart(m.data)
	if err != nil {
		return err
	}
	m.cart = c
	m.clk = 0
	c.now = func() int64 { return m.clk / 4 }
	for i := range m.ram {
		m.ram[i] = uint8(i*151 + 7)
	}
	m.riot = atari2600.NewRIOT()
	m.sound = atari2600.NewTIASound(m.sampleRate, float64(mclkRate)/228)
	m.maria = [32]uint8{}
	m.ctrl, m.charbase, m.dpp = 0, 0, 0
	m.line, m.mclk, m.dmaDone, m.lineHadDma = 0, 0, false, false
	m.audClk, m.riotFrac = 0, 0
	m.wsync, m.frameDone = false, false
	m.dll, m.dl, m.zoneOff, m.h16, m.h8, m.wmode, m.zoneActive = 0, 0, 0, false, false, 0, false
	m.lb = [512]uint8{}
	m.tiaVblank, m.latch = 0, [2]bool{}
	m.setPokeys()
	m.cpu = cpu6502.New(m, cpu6502.Options{Decimal: true})
	m.updatePorts()
	m.cpu.Reset()
	return nil
}

func (m *Atari) setPokeys() {
	m.pokeyList = m.pokeyList[:0]
	for _, p := range m.cart.pokeys {
		m.pokeyList = append(m.pokeyList, p.chip)
	}
	m.pokeyMix = 0.9
	if len(m.pokeyList) > 1 {
		m.pokeyMix = 0.6
	}
}

// ---- timing ----

// spend spends c MARIA clocks; runs the line's DMA and line ends as they come.
func (m *Atari) spend(c int) {
	m.pass(c)
	if !m.dmaDone && m.mclk >= 28 {
		m.dma()
	}
	for m.mclk >= lineClk {
		m.endLine()
	}
}

// pass: time passing (sound, timer), also while MARIA holds the processor.
func (m *Atari) pass(c int) {
	m.mclk += c
	m.clk += int64(c)
	m.audClk += c
	for m.audClk >= 228 {
		m.audClk -= 228
		m.sound.Tick()
		for _, p := range m.pokeyList {
			m.sound.AddToLast(p.run(57) * m.pokeyMix)
		}
	}
	m.riotFrac += c
	for m.riotFrac >= 4 {
		m.riotFrac -= 4
		m.riot.Tick()
	}
}

func (m *Atari) access(a int) {
	// TIA and RIOT run off the slow clock: those accesses take 1.5 times as long.
	slow := a < 0x400 && (a&0x3e0 == 0 || a&0x380 == 0x280) || (a >= 0x480 && a < 0x500)
	if slow {
		m.spend(6)
	} else {
		m.spend(4)
	}
}

func (m *Atari) endLine() {
	m.mclk -= lineClk
	// show the line MARIA prepared
	row := m.line + 1 - outTop
	if row >= 0 && row < outH {
		pal := &palette
		if m.ctrl&0x80 != 0 {
			pal = &grey
		}
		o := row * outW
		scr := m.screen[o : o+outW]
		if m.lineHadDma {
			for x := range scr {
				i := m.lb[x]
				if i&3 != 0 {
					scr[x] = pal[m.maria[i]]
				} else {
					scr[x] = pal[m.maria[0]]
				}
			}
		} else {
			c := pal[m.maria[0]]
			for x := range scr {
				scr[x] = c
			}
		}
	}
	m.lineHadDma = false
	m.line++
	if m.line >= lines {
		m.line = 0
		m.frameDone = true
		m.zoneActive = false
	}
	m.dmaDone = false
}

func (m *Atari) vblank() bool { return m.line < dmaFirst || m.line > dmaLast }

// ---- MARIA ----

func (m *Atari) dmaOn() bool { return (m.ctrl>>5)&3 == 2 }

func (m *Atari) loadZone() bool {
	b0 := m.peek(m.dll)
	m.zoneOff = int(b0 & 15)
	m.h16 = b0&0x40 != 0
	m.h8 = b0&0x20 != 0
	m.dl = int(m.peek(m.dll+1))<<8 | int(m.peek(m.dll+2))
	m.dll = (m.dll + 3) & 0xffff
	return b0&0x80 != 0 // DLI
}

func (m *Atari) dma() {
	m.dmaDone = true
	line := m.line
	if line == dmaFirst-1 && m.dmaOn() {
		// start of the display: fetch the first zone
		m.dll = m.dpp
		m.zoneActive = true
		dli := m.loadZone()
		m.pass(24)
		if dli {
			m.cpu.NMIPending = true
		}
		return
	}
	if line < dmaFirst || line > dmaLast || !m.dmaOn() || !m.zoneActive {
		return
	}
	cost := 16
	m.lb = [512]uint8{}
	cost += m.drawLine()
	m.lineHadDma = true
	dli := false
	if m.zoneOff == 0 {
		dli = m.loadZone()
		cost += 24
	} else {
		m.zoneOff--
	}
	m.pass(min(cost, 420))
	if dli {
		m.cpu.NMIPending = true
	}
}

func (m *Atari) drawLine() int {
	p, cost := m.dl, 0
	off := m.zoneOff
	ctrl := m.ctrl
	rm, kang := ctrl&3, ctrl&4 != 0
	cw := 1
	if ctrl&0x10 != 0 {
		cw = 2
	}
	for guard := 0; guard < 128; guard++ {
		b1 := m.peek(p + 1)
		if b1&0x5f == 0 {
			break // end of display list
		}
		var lo, hi, pw, hpos int
		ind := false
		if b1&0x1f == 0 { // 5-byte header
			lo, hi, pw, hpos = int(m.peek(p)), int(m.peek(p+2)), int(m.peek(p+3)), int(m.peek(p+4))
			m.wmode = b1 & 0x80
			ind = b1&0x20 != 0
			p += 5
			cost += 12
		} else {
			lo, hi, pw, hpos = int(m.peek(p)), int(m.peek(p+2)), int(b1), int(m.peek(p+3))
			p += 4
			cost += 8
		}
		width := 32 - (pw & 0x1f)
		if width == 0 {
			width = 32
		}
		pal := pw >> 5
		x := hpos * 2
		if !ind {
			addr := ((hi+off)&0xff)<<8 | lo
			if m.holey(addr) {
				cost += width * 3 // nothing fetched
				continue
			}
			for i := 0; i < width; i++ {
				x = m.plot(m.peek((addr+i)&0xffff), x, pal, rm, kang)
			}
			cost += width * 3
		} else {
			cbase := ((int(m.charbase) + off) & 0xff) << 8
			for i := 0; i < width; i++ {
				ch := int(m.peek((((hi << 8) | lo) + i) & 0xffff))
				ga := (cbase + ch) & 0xffff
				cost += 3
				if m.holey(ga) {
					cost += 3 * cw
					x = (x + m.byteWidth(rm)*cw) & 511
					continue
				}
				for k := 0; k < cw; k++ {
					x = m.plot(m.peek((ga+k)&0xffff), x, pal, rm, kang)
				}
				cost += 3 * cw
			}
		}
	}
	return cost
}

// byteWidth: hi-res pixels covered by one graphics byte in each mode.
func (m *Atari) byteWidth(rm uint8) int {
	if m.wmode != 0 {
		if rm == 3 || rm == 0 {
			return 4
		}
		return 8
	}
	if rm == 2 {
		return 4
	}
	return 8
}

func (m *Atari) holey(a int) bool {
	return (m.h16 && a&0x9000 == 0x9000) || (m.h8 && a&0x8800 == 0x8800)
}

// plot draws one graphics byte at hi-res position x; returns the next position.
func (m *Atari) plot(d uint8, x, P int, rm uint8, kang bool) int {
	lb := &m.lb
	switch rm {
	case 0:
		if m.wmode == 0 { // 160A: 4 pixels x 2 bits
			for i := 0; i < 4; i++ {
				c := int(d>>(6-2*i)) & 3
				if c != 0 || kang {
					v := uint8(0)
					if c != 0 {
						v = uint8(P*4 + c)
					}
					lb[x&511] = v
					lb[(x+1)&511] = v
				}
				x += 2
			}
		} else { // 160B: 2 pixels x (2 colour + 2 palette bits)
			for i := 0; i < 2; i++ {
				sh, sp := 6, 2
				if i != 0 {
					sh, sp = 4, 0
				}
				c, pp := int(d>>sh)&3, int(d>>sp)&3
				if c != 0 || kang {
					v := uint8(0)
					if c != 0 {
						v = uint8(((P&4)|pp)*4 + c)
					}
					lb[x&511] = v
					lb[(x+1)&511] = v
				}
				x += 2
			}
		}
	case 2:
		if m.wmode == 0 { // 320B: 4 pixels x 2 bits
			for i := 0; i < 4; i++ {
				c := (int(d>>(7-i))&1)<<1 | int(d>>(3-i))&1
				if c != 0 || kang {
					v := uint8(0)
					if c != 0 {
						v = uint8(P*4 + c)
					}
					lb[x&511] = v
				}
				x++
			}
		} else { // 320D: 8 pixels; the second colour bit comes from the palette number
			for i := 0; i < 8; i++ {
				sh := 1
				if i&1 != 0 {
					sh = 0
				}
				c := (int(d>>(7-i))&1)<<1 | (P>>sh)&1
				if c != 0 || kang {
					v := uint8(0)
					if c != 0 {
						v = uint8((P&4)*4 + c)
					}
					lb[x&511] = v
				}
				x++
			}
		}
	case 3:
		if m.wmode == 0 { // 320A: 8 pixels x 1 bit, colour 2
			for i := 0; i < 8; i++ {
				if (d>>(7-i))&1 != 0 {
					lb[x&511] = uint8(P*4 + 2)
				} else if kang {
					lb[x&511] = 0
				}
				x++
			}
		} else { // 320C: 4 pixels on/off, each pair has its own palette
			for i := 0; i < 4; i++ {
				sp := 0
				if i < 2 {
					sp = 2
				}
				pp := int(d>>sp) & 3
				if (d>>(7-i))&1 != 0 {
					lb[x&511] = uint8(((P&4)|pp)*4 + 2)
				} else if kang {
					lb[x&511] = 0
				}
				x++
			}
		}
	default:
		x += 8
	}
	return x & 511
}

// ---- memory ----

// peek reads without side effects or time (MARIA's DMA).
func (m *Atari) peek(a int) uint8 {
	a &= 0xffff
	if a >= 0x4000 {
		return m.cart.read(a, true)
	}
	if a >= 0x1800 {
		if a < 0x2800 {
			return m.ram[a-0x1800]
		}
		return m.ram[0x800+(a&0x7ff)]
	}
	if a < 0x400 {
		if a&0xff >= 0x40 && a&0x200 == 0 {
			return m.ram[0x800+(a&0x1ff)]
		}
	}
	if a >= 0x480 && a < 0x500 {
		return m.riot.RAM()[a&0x7f]
	}
	return 0
}

// Read is the processor's bus.
func (m *Atari) Read(addr uint16) uint8 {
	if m.wsync {
		m.wsync = false
		m.spend(lineClk - m.mclk)
	}
	a := int(addr)
	m.access(a)
	if a >= 0x4000 {
		return m.cart.read(a, false)
	}
	if a >= 0x1800 {
		if a < 0x2800 {
			return m.ram[a-0x1800]
		}
		return m.ram[0x800+(a&0x7ff)]
	}
	if a < 0x400 {
		lo := a & 0xff
		if a < 0x200 {
			if lo < 0x20 {
				return m.tiaRead(lo)
			}
			if lo < 0x40 {
				return m.mariaRead(lo)
			}
			return m.ram[0x800+(a&0x1ff)]
		}
		if a >= 0x280 && a < 0x300 {
			return m.riot.Read(a)
		}
		return 0
	}
	if a >= 0x480 && a < 0x500 {
		return m.riot.RAM()[a&0x7f]
	}
	if chip := m.cart.ioChip(a); chip != nil {
		return chip.read(a, m.clk/4)
	}
	return 0
}

// Write is the processor's bus.
func (m *Atari) Write(addr uint16, v uint8) {
	a := int(addr)
	m.access(a)
	if a >= 0x4000 {
		m.cart.write(a, v)
		return
	}
	if a >= 0x1800 {
		if a < 0x2800 {
			m.ram[a-0x1800] = v
		} else {
			m.ram[0x800+(a&0x7ff)] = v
		}
		return
	}
	if a < 0x400 {
		lo := a & 0xff
		if a < 0x200 {
			if lo < 0x20 {
				m.tiaWrite(lo, v)
				return
			}
			if lo < 0x40 {
				m.mariaWrite(lo, v)
				return
			}
			m.ram[0x800+(a&0x1ff)] = v
			return
		}
		if a >= 0x280 && a < 0x300 {
			m.riot.Write(a, v)
		}
		return
	}
	if a >= 0x480 && a < 0x500 {
		m.riot.RAM()[a&0x7f] = v
		return
	}
	if chip := m.cart.ioChip(a); chip != nil {
		chip.write(a, v)
	}
}

func (m *Atari) mariaRead(r int) uint8 {
	if r == 0x28 && m.vblank() {
		return 0x80
	}
	return 0
}

func (m *Atari) mariaWrite(r int, v uint8) {
	switch r {
	case 0x24:
		m.wsync = true
		return
	case 0x2c:
		m.dpp = int(v)<<8 | m.dpp&0xff
		return
	case 0x30:
		m.dpp = m.dpp&0xff00 | int(v)
		return
	case 0x34:
		m.charbase = v
		return
	case 0x3c:
		m.ctrl = v
		return
	case 0x28, 0x38:
		return
	}
	m.maria[r-0x20] = v
}

func (m *Atari) tiaRead(r int) uint8 {
	r &= 15
	p := 0
	if r >= 10 {
		p = 1
	}
	switch r {
	case 8, 10: // right button
		if m.input[p]&btnB2 != 0 {
			return 0x80
		}
		return 0
	case 9, 11: // left button
		if m.input[p]&btnB1 != 0 {
			return 0x80
		}
		return 0
	case 12, 13:
		q := r - 12
		down := m.input[q]&(btnB1|btnB2) != 0
		if m.tiaVblank&0x40 != 0 {
			if down {
				m.latch[q] = true
			}
			if m.latch[q] {
				return 0
			}
			return 0x80
		}
		if down {
			return 0
		}
		return 0x80
	}
	return 0
}

func (m *Atari) tiaWrite(r int, v uint8) {
	if r == 1 {
		m.tiaVblank = v
		if v&0x40 == 0 {
			m.latch = [2]bool{}
		}
		return
	}
	if r >= 0x15 && r <= 0x1a {
		m.sound.Write(r-0x15, v)
	}
}

// ---- controllers ----

func (m *Atari) updatePorts() {
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
	a := uint8(0xff) &^ (dirs(m.input[0]) << 4) &^ dirs(m.input[1])
	s := m.input[0]
	b := uint8(0x34)
	if s&btnReset == 0 {
		b |= 1
	}
	if s&btnSelect == 0 {
		b |= 2
	}
	if s&btnPause == 0 {
		b |= 8
	}
	if s&btnLDiff != 0 {
		b |= 0x40
	}
	if s&btnRDiff != 0 {
		b |= 0x80
	}
	m.riot.SetPorts(a, b)
}

// SetInput sets player p's buttons (bit i = Info().Buttons[i]); the console
// switches are read from player 0.
func (m *Atari) SetInput(p int, bits uint32) {
	if p >= 0 && p < 2 {
		m.input[p] = bits
		m.updatePorts()
	}
}

// ---- core.Core ----

// Info describes the console's controls and picture.
func (m *Atari) Info() core.Info {
	return core.Info{System: "atari7800", Buttons: append([]string(nil), buttons...), Players: 2, Aspect: 4.0 / 3.0}
}

// FPS is the NTSC frame rate.
func (m *Atari) FPS() float64 { return FPS }

// Width of the picture.
func (m *Atari) Width() int { return outW }

// Height of the picture.
func (m *Atari) Height() int { return outH }

// Screen is the picture, 320x240, 0x00RRGGBB.
func (m *Atari) Screen() []uint32 { return m.screen }

// RunFrame runs until MARIA finishes the frame (or for at most 200000
// instructions, as the web core does).
func (m *Atari) RunFrame() {
	m.frameDone = false
	for guard := 0; !m.frameDone && guard < 200000; guard++ {
		m.cpu.Step()
	}
}

// TakeAudio returns the mono samples made since the last call.
func (m *Atari) TakeAudio() []float32 { return m.sound.Take() }

// Reset is the console's power switch: the game starts again (keeping the
// buttons held).
func (m *Atari) Reset() {
	keep := m.input
	if err := m.init(); err != nil {
		// The same file was accepted before; this cannot happen.
		panic(err)
	}
	m.input = keep
	m.updatePorts()
}

// SRAM is nil: the 7800 core keeps no battery saves.
func (m *Atari) SRAM() []byte { return nil }

const stateMagic = "RP7800\x01"

// SaveState captures the whole machine.
func (m *Atari) SaveState() ([]byte, error) {
	w := &stateW{}
	w.b = append(w.b, stateMagic...)
	w.bytes(m.cpu.SaveState())
	w.bytes(m.ram[:])
	w.bytes(m.maria[:])
	w.b = m.riot.AppendState(w.b)
	w.b = m.sound.AppendState(w.b)
	m.cart.save(w)
	w.u8(m.ctrl)
	w.u8(m.charbase)
	w.int(m.dpp)
	w.int(m.line)
	w.int(m.mclk)
	w.bool(m.dmaDone)
	w.bool(m.lineHadDma)
	w.int(m.audClk)
	w.int(m.riotFrac)
	w.bool(m.wsync)
	w.int(m.dll)
	w.int(m.dl)
	w.int(m.zoneOff)
	w.bool(m.h16)
	w.bool(m.h8)
	w.u8(m.wmode)
	w.bool(m.zoneActive)
	w.u8(m.tiaVblank)
	w.bytes(m.lb[:])
	w.bool(m.latch[0])
	w.bool(m.latch[1])
	// Not in the web core's states: without it the POKEY's random numbers
	// carry on from wherever the game was when the state was loaded.
	w.i64(m.clk)
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
	n.cart = m.cart.clone()
	n.riot = new(atari2600.RIOT)
	*n.riot = *m.riot
	n.sound = new(atari2600.TIASound)
	*n.sound = *m.sound
	n.cpu = cpu6502.New(&n, cpu6502.Options{Decimal: true})
	n.pokeyList = nil

	r := &stateR{b: raw[len(stateMagic):]}
	cpuState := r.bytes()
	if r.err == nil {
		if err := n.cpu.LoadState(cpuState); err != nil {
			return errBadState
		}
	}
	r.bytesInto(n.ram[:])
	r.bytesInto(n.maria[:])
	if r.err == nil {
		if r.b, err = n.riot.LoadState(r.b); err != nil {
			return errBadState
		}
		if r.b, err = n.sound.LoadState(r.b); err != nil {
			return errBadState
		}
	}
	n.cart.load(r)
	n.ctrl = r.u8()
	n.charbase = r.u8()
	n.dpp = r.int() & 0xffff
	n.line = r.int()
	n.mclk = r.int()
	n.dmaDone = r.bool()
	n.lineHadDma = r.bool()
	n.audClk = r.int()
	n.riotFrac = r.int()
	n.wsync = r.bool()
	n.dll = r.int() & 0xffff
	n.dl = r.int()
	n.zoneOff = r.int()
	n.h16 = r.bool()
	n.h8 = r.bool()
	n.wmode = r.u8()
	n.zoneActive = r.bool()
	n.tiaVblank = r.u8()
	r.bytesInto(n.lb[:])
	n.latch[0] = r.bool()
	n.latch[1] = r.bool()
	n.clk = r.i64()
	if r.err != nil || len(r.b) != 0 || n.line < 0 || n.line >= lines || n.mclk < 0 || n.mclk >= lineClk {
		return errBadState
	}
	*m = n
	m.cart.now = func() int64 { return m.clk / 4 }
	m.cpu.SetBus(m)
	m.setPokeys()
	m.updatePorts()
	return nil
}
