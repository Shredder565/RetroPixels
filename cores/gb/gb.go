// Package gb is the native Game Boy / Game Boy Color emulator: a Go port of
// web/emu/gb.js and gb_carts.js that behaves the same way, frame for frame and
// sample for sample.
//
// SM83 CPU timed per memory access, PPU with DMG and CGB modes, four-channel
// stereo APU, timers, serial port and OAM/HDMA transfers, and the cartridge
// chips (carts.go). No boot ROM is used; the machine starts in the state the
// boot ROM leaves it in.
package gb

import (
	"bytes"
	"errors"
	"fmt"
	"hash/crc32"
	"math"

	"retropixels/core"
)

const (
	clockHz = 4194304
	fps     = clockHz / 70224.0 // 59.7275
)

// Buttons in input bit order, as in the web core (bits 8-11, the tilt keys,
// are only listed for tilt cartridges).
var Buttons = []string{"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"}

var tiltButtons = []string{"Tilt Up", "Tilt Down", "Tilt Left", "Tilt Right"}

// GB is one running game.
type GB struct {
	clock int64 // real time in 4.19 MHz clocks (for cartridge clocks and the camera)

	tiltX, tiltY   float64
	stickX, stickY float64
	tiltKeys       int

	dateNow func() float64 // the computer's clock in ms, for the cartridge clocks
	cart    cart
	cb      *cartBase
	cgb     bool
	battery bool
	dmgPal  *[4]uint32
	romCRC  uint32

	vram  [0x4000]byte
	vbank int
	wram  [0x8000]byte
	wbank int
	oam   [0xa0]byte
	hram  [0x7f]byte
	io    [0x80]byte
	bgPal [64]byte
	obPal [64]byte
	bcps  int
	ocps  int
	ie    int
	ifl   int // the IF register

	apu    *apu
	screen []uint32
	keys   int // bit set = pressed: A B Select Start Right Left Up Down (GB order)

	serialBits int

	// CPU
	a, f, b, c, d, e, h, l int
	sp, pc                 int
	ime                    bool
	eiDelay                int
	halted, haltBug        bool
	stopped, locked        bool
	iduRead                bool

	// timers
	div, tima, tma, tac int
	timaReload          int
	timaJustLoaded      bool

	// PPU
	ly, lineDot, mode       int
	statLine                bool
	winLine                 int
	frameDone               bool
	lyReg, lyCmp, m3end     int
	firstLine               bool
	oamLock, vramLock       bool
	oamWLock, vramWLock     bool
	lycFlag                 bool
	lcdOffDots              int
	ppuNewIf                int
	lineBg, lineBgPri, sprD [160]byte

	double bool
	key1   int

	hdmaSrc, hdmaDst, hdmaLen int
	hdmaActive, hdmaPending   bool

	cycles int64

	dmaActive                 bool
	dmaDelay, dmaIdx, dmaBase int
	dmaReq, dmaByte, joyp     int
}

// New opens a game (.gb / .gbc data).
func New(rom []byte, opts core.Options) (core.Core, error) { return newGB(rom, opts, wallClock) }

func newGB(rom []byte, opts core.Options, dateNow func() float64) (g *GB, err error) {
	if len(rom) < 0x150 {
		return nil, errors.New("This isn't a Game Boy game file")
	}
	sr := opts.SampleRate
	if sr <= 0 {
		sr = 48000
	}
	g = &GB{romCRC: crc32.ChecksumIEEE(rom), dateNow: dateNow}
	defer func() {
		if r := recover(); r != nil {
			g, err = nil, fmt.Errorf("This Game Boy game couldn't be started (%v).", r)
		}
	}()
	g.cart, err = createCart(rom, g)
	if err != nil {
		return nil, err
	}
	g.cb = g.cart.base()
	g.cgb = g.cb.cgb
	g.battery = g.cb.battery
	if opts.SRAM != nil && g.battery {
		cartLoadSram(g.cart, opts.SRAM)
	}
	g.dmgPal = &dmgGreen
	g.wbank = 1
	g.ifl = 0xe1
	g.apu = newAPU(sr, g)
	g.screen = make([]uint32, 160*144)
	g.Reset()
	return g, nil
}

// Reset: the registers as the boot ROM leaves them (reset() in gb.js; like
// the web core it keeps the memory and the cartridge as they are).
func (g *GB) Reset() {
	if g.cgb {
		g.a, g.f, g.b, g.c, g.d, g.e, g.h, g.l = 0x11, 0x80, 0x00, 0x00, 0xff, 0x56, 0x00, 0x0d
	} else {
		g.a, g.f, g.b, g.c, g.d, g.e, g.h, g.l = 0x01, 0xb0, 0x00, 0x13, 0x00, 0xd8, 0x01, 0x4d
	}
	g.sp, g.pc = 0xfffe, 0x100
	g.ime, g.eiDelay, g.halted, g.haltBug, g.stopped = false, 0, false, false, false
	if g.cgb {
		g.div = 0x1ea0
	} else {
		g.div = 0xabc8
	}
	g.tima, g.tma, g.tac, g.timaReload = 0, 0, 0xf8, 0
	g.ly, g.lineDot, g.mode, g.statLine, g.winLine, g.frameDone = 0, 0, 2, false, 0, false
	g.lyReg, g.lyCmp, g.m3end, g.firstLine = 0, 0, 256, false
	g.oamLock, g.vramLock, g.oamWLock, g.vramWLock, g.lycFlag = false, false, false, false, false
	g.double, g.key1 = false, 0
	g.hdmaSrc, g.hdmaDst, g.hdmaLen, g.hdmaActive = 0, 0, 0, false
	g.cycles = 0
	g.dmaActive, g.dmaDelay, g.dmaIdx, g.dmaBase, g.dmaReq, g.dmaByte = false, 0, 0, 0, 0, 0xff
	g.io = [0x80]byte{}
	g.joyp = 0xcf
	g.io[0x40], g.io[0x41], g.io[0x47], g.io[0x48], g.io[0x49] = 0x91, 0x85, 0xfc, 0xff, 0xff
	// APU post-boot values
	apuInit := [...][2]int{{0xff10, 0x80}, {0xff11, 0xbf}, {0xff12, 0xf3}, {0xff14, 0xbf}, {0xff16, 0x3f}, {0xff17, 0x00}, {0xff19, 0xbf},
		{0xff1a, 0x7f}, {0xff1b, 0xff}, {0xff1c, 0x9f}, {0xff1e, 0xbf}, {0xff20, 0xff}, {0xff21, 0x00}, {0xff22, 0x00}, {0xff23, 0xbf},
		{0xff24, 0x77}, {0xff25, 0xf3}}
	g.apu.powerOff()
	g.apu.on = true
	g.apu.seqStep = 0
	for _, av := range apuInit {
		r := av[0] - 0xff10
		if r%5 == 4 && r < 0x14 {
			g.apu.regs[r] = byte(av[1]) // no triggers
		} else {
			g.apu.write(av[0], av[1])
		}
	}
	g.apu.ch[0].on = true // the start-up chime's channel is left on, silent
	g.apu.ch[0].vol = 0
	if g.cgb {
		for i := range g.bgPal {
			g.bgPal[i] = 0xff
		}
	}
}

// ---- timing: every memory access costs one M-cycle (4 T-cycles) ----

func (g *GB) tick() {
	t := 4
	if g.double {
		t = 2 // the PPU and APU see half as much time per M-cycle at double speed
	}
	g.cycles += 4
	g.clock += int64(t)
	if g.dmaActive || g.dmaDelay != 0 {
		g.oamDmaStep()
	}
	g.timerTick()
	ifBefore := g.ifl
	g.ppuTick(t)
	g.ppuNewIf = g.ifl &^ ifBefore
	g.apu.tick(t)
}

// timerTick: DIV is a 16-bit counter; TIMA counts falling edges of one of its bits.
func (g *GB) timerTick() {
	// after TIMA overflows it reads 0 for one M-cycle, then TMA is loaded
	g.timaJustLoaded = false
	if g.timaReload != 0 {
		g.timaReload--
		if g.timaReload == 0 {
			g.tima = g.tma
			g.ifl |= 4
			g.timaJustLoaded = true
		}
	}
	old := g.div
	g.div = (g.div + 4) & 0xffff
	g.timerEdge(old, g.div)
	g.apuEdge(old, g.div)
	// serial port with its own clock: one bit per falling edge of a divider bit (8192 Hz, or 262144 Hz on Color)
	if g.serialBits != 0 {
		bit := 8
		if g.cgb && g.io[2]&2 != 0 {
			bit = 3
		}
		if (old>>bit)&1 != 0 && (g.div>>bit)&1 == 0 {
			g.serialBits--
			if g.serialBits == 0 {
				g.io[1] = 0xff
				g.io[2] &= 0x7f
				g.ifl |= 8
			}
		}
	}
}

// apuEdge: the sound frame sequencer steps on falling edges of divider bit 12 (bit 13 at double speed).
func (g *GB) apuEdge(old, now int) {
	b := 12
	if g.double {
		b = 13
	}
	if (old>>b)&1 != 0 && (now>>b)&1 == 0 {
		g.apu.clockSeq()
	}
}

var timerBits = [4]int{9, 3, 5, 7}

func (g *GB) timerBit() int { return timerBits[g.tac&3] }

func (g *GB) timerEdge(old, now int) {
	if g.tac&4 == 0 {
		return
	}
	bit := g.timerBit()
	if (old>>bit)&1 != 0 && (now>>bit)&1 == 0 {
		g.incTima()
	}
}

func (g *GB) incTima() {
	g.tima = (g.tima + 1) & 0xff
	if g.tima == 0 {
		g.timaReload = 1
	}
}

func (g *GB) rd(a int) int {
	g.tick()
	if a >= 0xfe00 && a < 0xff00 {
		kind := 1
		if g.iduRead {
			kind = 2
		}
		g.oamBug(a, kind)
	}
	if g.dmaActive && a < 0xff00 {
		return g.dmaBusRead(a)
	}
	return g.read(a)
}

func (g *GB) wr(a, v int) {
	g.tick()
	if a >= 0xfe00 && a < 0xff00 {
		g.oamBug(a, 0)
	}
	if g.dmaActive && a < 0xff00 && g.dmaBusBlocked(a) {
		return
	}
	g.write(a, v)
}

// ---- the original Game Boy's OAM corruption bug ----
// While the video chip scans OAM (mode 2) it reads one 8-byte row per M-cycle.
// If the CPU puts an address in FE00-FEFF on the bus then (a read, a write, or
// the 16-bit increment/decrement unit of INC rr, DEC rr, PUSH, POP,
// LD A,(HL+/-)), that row gets mixed with the one before it. kind: 0 write
// (or inc/dec), 1 read, 2 read + inc/dec together.
func (g *GB) oamScanRow() int {
	if g.cgb || !g.lcdOn() || g.ly >= 144 || g.firstLine {
		return -1
	}
	r := g.lineDot >> 2 // row r is read in the M-cycle that ends at dot 4r
	if r >= 0 && r < 20 {
		return r
	}
	return -1
}

func (g *GB) oamW(r, i int) int { return int(g.oam[r*8+i*2]) | int(g.oam[r*8+i*2+1])<<8 }
func (g *GB) oamSetW(r, i, v int) {
	g.oam[r*8+i*2] = byte(v)
	g.oam[r*8+i*2+1] = byte(v >> 8)
}
func (g *GB) oamCopyRow(to, from int) { copy(g.oam[to*8:to*8+8], g.oam[from*8:from*8+8]) }

func (g *GB) oamBug(a, kind int) {
	if g.cgb || a < 0xfe00 || a >= 0xff00 {
		return
	}
	row := g.oamScanRow()
	if row < 1 {
		return // the first row (objects 0 and 1) is never damaged
	}
	if kind == 2 && row >= 4 && row < 19 {
		a2, b, c, d := g.oamW(row-2, 0), g.oamW(row-1, 0), g.oamW(row, 0), g.oamW(row-1, 2)
		g.oamSetW(row-1, 0, (b&(a2|c|d))|(a2&c&d))
		g.oamCopyRow(row, row-1)
		g.oamCopyRow(row-2, row-1)
	}
	a0, b, c := g.oamW(row, 0), g.oamW(row-1, 0), g.oamW(row-1, 2)
	var first int
	if kind == 0 {
		first = ((a0 ^ c) & (b ^ c)) ^ c
	} else {
		first = b | (a0 & c)
	}
	g.oamCopyRow(row, row-1)
	g.oamSetW(row, 0, first)
}

// ---- OAM DMA: one byte per M-cycle after a cycle of set-up; meanwhile the
// CPU can't use the bus the copy reads from (it sees the byte being copied), nor OAM.
func (g *GB) oamDmaStep() {
	if g.dmaDelay != 0 {
		g.dmaDelay--
		if g.dmaDelay == 0 {
			g.dmaActive = true
			g.dmaIdx = 0
			g.dmaBase = g.dmaReq
		}
	}
	if g.dmaActive {
		if g.dmaIdx >= 160 {
			g.dmaActive = false
		} else {
			a := g.dmaBase + g.dmaIdx
			if a >= 0xe000 {
				a -= 0x2000
			}
			g.dmaByte = g.dmaRead(a)
			g.oam[g.dmaIdx] = byte(g.dmaByte)
			g.dmaIdx++
		}
	}
}

func (g *GB) dmaRead(a int) int {
	switch {
	case a < 0x8000:
		return g.cart.romRead(a)
	case a < 0xa000:
		return int(g.vram[g.vbank*0x2000+(a&0x1fff)])
	case a < 0xc000:
		return g.cart.ramRead(a)
	case a < 0xd000:
		return int(g.wram[a&0xfff])
	}
	return int(g.wram[(g.wbank*0x1000+(a&0xfff))&0x7fff])
}

// dmaBusBlocked: the copy's source is on the video bus (8000-9FFF) or the cartridge / work RAM bus.
func (g *GB) dmaBusBlocked(a int) bool {
	if a >= 0xfe00 {
		return true
	}
	srcVram := g.dmaBase >= 0x8000 && g.dmaBase < 0xa000
	isVram := a >= 0x8000 && a < 0xa000
	if srcVram != isVram {
		return false
	}
	// the Color model has work RAM on a bus of its own
	if g.cgb && !srcVram && ((a >= 0xc000) != (g.dmaBase >= 0xc000)) {
		return false
	}
	return true
}

func (g *GB) dmaBusRead(a int) int {
	if !g.dmaBusBlocked(a) {
		return g.read(a)
	}
	if a >= 0xfe00 {
		return 0xff
	}
	return g.dmaByte
}

func (g *GB) idle() { g.tick() }

func (g *GB) read(a int) int {
	for {
		switch {
		case a < 0x8000:
			return g.cart.romRead(a)
		case a < 0xa000:
			if g.vramLock {
				return 0xff
			}
			return int(g.vram[g.vbank*0x2000+(a&0x1fff)])
		case a < 0xc000:
			return g.cart.ramRead(a)
		case a < 0xd000:
			return int(g.wram[a&0xfff])
		case a < 0xe000:
			return int(g.wram[g.wbank*0x1000+(a&0xfff)])
		case a < 0xfe00:
			a -= 0x2000
			continue
		case a < 0xfea0:
			if g.oamLock {
				return 0xff
			}
			return int(g.oam[a-0xfe00])
		case a < 0xff00:
			return 0xff
		case a < 0xff80:
			return g.ioRead(a)
		case a < 0xffff:
			return int(g.hram[a-0xff80])
		}
		return g.ie
	}
}

func (g *GB) write(a, v int) {
	for {
		switch {
		case a < 0x8000:
			g.cart.romWrite(a, v)
		case a < 0xa000:
			if !g.vramWLock {
				g.vram[g.vbank*0x2000+(a&0x1fff)] = byte(v)
			}
		case a < 0xc000:
			g.cart.ramWrite(a, v)
		case a < 0xd000:
			g.wram[a&0xfff] = byte(v)
		case a < 0xe000:
			g.wram[g.wbank*0x1000+(a&0xfff)] = byte(v)
		case a < 0xfe00:
			a -= 0x2000
			continue
		case a < 0xfea0:
			if !g.oamWLock {
				g.oam[a-0xfe00] = byte(v)
			}
		case a < 0xff00:
		case a < 0xff80:
			g.ioWrite(a, v)
		case a < 0xffff:
			g.hram[a-0xff80] = byte(v)
		default:
			g.ie = v
		}
		return
	}
}

func (g *GB) lcdOn() bool { return g.io[0x40]&0x80 != 0 }

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func (g *GB) ioRead(a int) int {
	r := a & 0x7f
	switch r {
	case 0x00:
		v := 0xcf | (g.joyp & 0x30)
		if g.joyp&0x10 == 0 {
			v &^= (g.keys >> 4) & 15 // directions
		}
		if g.joyp&0x20 == 0 {
			v &^= g.keys & 15 // buttons
		}
		return v
	case 0x01:
		return int(g.io[1])
	case 0x02:
		if g.cgb {
			return int(g.io[2]) | 0x7c
		}
		return int(g.io[2]) | 0x7e
	case 0x04:
		return g.div >> 8
	case 0x05:
		return g.tima
	case 0x06:
		return g.tma
	case 0x07:
		return g.tac | 0xf8
	case 0x0f:
		return g.ifl | 0xe0
	case 0x41:
		v := 0x80 | int(g.io[0x41]&0x78)
		if g.lycFlag {
			v |= 4
		}
		if g.lcdOn() {
			v |= g.mode
		}
		return v
	case 0x44:
		return g.lyReg
	case 0x4d:
		if g.cgb {
			return b2i(g.double)<<7 | (g.key1 & 1) | 0x7e
		}
		return 0xff
	case 0x4f:
		if g.cgb {
			return 0xfe | g.vbank
		}
		return 0xff
	case 0x55:
		if g.cgb && g.hdmaActive {
			return (g.hdmaLen - 1) & 0x7f
		}
		return 0xff
	case 0x68:
		if g.cgb {
			return g.bcps | 0x40
		}
		return 0xff
	case 0x69:
		if g.cgb {
			return int(g.bgPal[g.bcps&0x3f])
		}
		return 0xff
	case 0x6a:
		if g.cgb {
			return g.ocps | 0x40
		}
		return 0xff
	case 0x6b:
		if g.cgb {
			return int(g.obPal[g.ocps&0x3f])
		}
		return 0xff
	case 0x70:
		if g.cgb {
			return 0xf8 | g.wbank
		}
		return 0xff
	}
	if r >= 0x10 && r < 0x40 {
		return g.apu.read(a)
	}
	if r >= 0x40 && r <= 0x4b {
		return int(g.io[r])
	}
	return 0xff
}

func (g *GB) ioWrite(a, v int) {
	r := a & 0x7f
	switch r {
	case 0x00:
		g.joyp = v & 0x30
		return
	case 0x01:
		g.io[1] = byte(v)
		return
	case 0x02:
		g.io[2] = byte(v)
		if v&0x81 == 0x81 {
			g.serialBits = 8
		} else {
			g.serialBits = 0
		}
		return
	case 0x04:
		old := g.div
		g.div = 0
		g.timerEdge(old, 0)
		g.apuEdge(old, 0)
		return
	case 0x05:
		if !g.timaJustLoaded { // a write in the 0 cycle cancels the reload
			g.tima = v
			g.timaReload = 0
		}
		return
	case 0x06:
		g.tma = v
		if g.timaJustLoaded {
			g.tima = v
		}
		return
	case 0x07:
		// changing the timer setting can itself produce a falling edge
		oldBit := g.tac&4 != 0 && (g.div>>g.timerBit())&1 != 0
		g.tac = v & 7
		newBit := g.tac&4 != 0 && (g.div>>g.timerBit())&1 != 0
		if oldBit && !newBit {
			g.incTima()
		}
		return
	case 0x0f:
		g.ifl = v & 0x1f
		return
	case 0x40:
		was := g.lcdOn()
		g.io[0x40] = byte(v)
		if was && v&0x80 == 0 {
			g.ly, g.lyReg, g.lyCmp, g.lineDot, g.mode = 0, 0, 0, 0, 0
			g.oamLock, g.vramLock, g.oamWLock, g.vramWLock = false, false, false, false
			fill := uint32(0xffffff)
			if !g.cgb {
				fill = g.dmgPal[0]
			}
			for i := range g.screen {
				g.screen[i] = fill
			}
		}
		// Switched on: line 0 starts in mode 0 (no OAM scan) and runs a little late.
		if !was && v&0x80 != 0 {
			g.ly, g.lyReg, g.lyCmp, g.lineDot, g.mode, g.winLine, g.firstLine = 0, 0, 0, 4, 0, 0, true
			g.checkStat()
		}
		return
	case 0x41:
		g.io[0x41] = byte(v & 0x78)
		g.checkStat()
		return
	case 0x44:
		return
	case 0x45:
		g.io[0x45] = byte(v)
		g.checkStat()
		return
	case 0x46: // OAM DMA: the first byte is copied two M-cycles later
		g.dmaReq = v << 8
		g.dmaDelay = 2
		g.io[0x46] = byte(v)
		return
	case 0x4d:
		if g.cgb {
			g.key1 = v & 1
		}
		return
	case 0x4f:
		if g.cgb {
			g.vbank = v & 1
		}
		return
	case 0x51:
		g.hdmaSrc = (g.hdmaSrc & 0xff) | (v << 8)
		return
	case 0x52:
		g.hdmaSrc = (g.hdmaSrc & 0xff00) | (v & 0xf0)
		return
	case 0x53:
		g.hdmaDst = (g.hdmaDst & 0xff) | ((v & 0x1f) << 8)
		return
	case 0x54:
		g.hdmaDst = (g.hdmaDst & 0xff00) | (v & 0xf0)
		return
	case 0x55:
		if !g.cgb {
			return
		}
		if g.hdmaActive && v&0x80 == 0 {
			g.hdmaActive = false
			return
		}
		g.hdmaLen = (v & 0x7f) + 1
		if v&0x80 != 0 {
			g.hdmaActive = true
		} else {
			for g.hdmaLen > 0 { // general-purpose DMA: all at once
				g.hdmaBlock()
			}
		}
		return
	case 0x68:
		g.bcps = v & 0xbf
		return
	case 0x69:
		g.bgPal[g.bcps&0x3f] = byte(v)
		if g.bcps&0x80 != 0 {
			g.bcps = 0x80 | ((g.bcps + 1) & 0x3f)
		}
		return
	case 0x6a:
		g.ocps = v & 0xbf
		return
	case 0x6b:
		g.obPal[g.ocps&0x3f] = byte(v)
		if g.ocps&0x80 != 0 {
			g.ocps = 0x80 | ((g.ocps + 1) & 0x3f)
		}
		return
	case 0x70:
		if g.cgb {
			g.wbank = v & 7
			if g.wbank == 0 {
				g.wbank = 1
			}
		}
		return
	}
	if r >= 0x10 && r < 0x40 {
		g.apu.write(a, v)
		return
	}
	if r >= 0x40 && r <= 0x4b {
		g.io[r] = byte(v)
	}
}

func (g *GB) hdmaBlock() {
	for i := 0; i < 16; i++ {
		g.vram[g.vbank*0x2000+((g.hdmaDst+i)&0x1fff)] = byte(g.read(g.hdmaSrc + i))
	}
	g.hdmaSrc = (g.hdmaSrc + 16) & 0xffff
	g.hdmaDst = (g.hdmaDst + 16) & 0x1fff
	g.hdmaLen--
	if g.hdmaLen <= 0 {
		g.hdmaActive = false
		g.hdmaLen = 0
	}
	// the CPU is paused while a block is copied
	n := 8
	if g.double {
		n = 16
	}
	for i := 0; i < n; i++ {
		g.tick()
	}
}

// ---- core.Core ----

func (g *GB) Info() core.Info {
	b := Buttons
	if g.cb.kind == "MBC7" {
		b = append(append([]string{}, Buttons...), tiltButtons...)
	}
	return core.Info{System: "gb", Buttons: b, Players: 1, Stereo: true, Aspect: 10.0 / 9}
}
func (g *GB) FPS() float64     { return fps }
func (g *GB) Width() int       { return 160 }
func (g *GB) Height() int      { return 144 }
func (g *GB) Screen() []uint32 { return g.screen }

// SetInput: bits in the order of Buttons (A, B, Select, Start, Up, Down,
// Left, Right), then Tilt Up, Down, Left, Right.
func (g *GB) SetInput(player int, bits uint32) {
	if player != 0 {
		return
	}
	g.tiltKeys = int(bits >> 8)
	// GB order: A B Select Start (low nibble), Right Left Up Down (high nibble)
	k := int(bits & 15)
	if bits&0x80 != 0 {
		k |= 0x10
	}
	if bits&0x40 != 0 {
		k |= 0x20
	}
	if bits&0x10 != 0 {
		k |= 0x40
	}
	if bits&0x20 != 0 {
		k |= 0x80
	}
	if k&^g.keys != 0 {
		g.ifl |= 0x10 // joypad interrupt on a new press
	}
	g.keys = k
}

// SetAnalog: tilt cartridges (MBC7) read the right stick (axes 2 and 3).
func (g *GB) SetAnalog(player, axis int, v float32) {
	if player != 0 {
		return
	}
	switch axis {
	case 2:
		g.stickX = float64(v)
	case 3:
		g.stickY = float64(v)
	}
}

func clamp1(x float64) float64 { return math.Max(-1, math.Min(1, x)) }

func (g *GB) RunFrame() {
	g.frameDone = false
	k := g.tiltKeys
	g.tiltX = clamp1(g.stickX + float64(b2i(k&8 != 0)) - float64(b2i(k&4 != 0)))
	g.tiltY = clamp1(g.stickY + float64(b2i(k&2 != 0)) - float64(b2i(k&1 != 0)))
	for guard := 0; !g.frameDone && guard < 300000; guard++ {
		if g.locked {
			g.idle()
			continue
		}
		g.step()
	}
	g.cart.frame()
}

// Rumble: how hard the cartridge's rumble motor ran this frame (0..1).
func (g *GB) Rumble() float32 {
	if !g.cb.hasRumble {
		return 0
	}
	return float32(g.cb.rumbleLevel)
}

// UsesRightStick: tilt cartridges read the right stick themselves, so the
// app leaves it alone (no save-state or recording shortcuts on it).
func (g *GB) UsesRightStick() bool { return g.cb.kind == "MBC7" }

// ControlsNote is shown in the Controls window for games with extra controls.
func (g *GB) ControlsNote() string {
	if g.UsesRightStick() {
		return "Tilt the game with the right stick, or with the tilt keys (I J K L to start with). Clicking the sticks (L3/R3) is used for save states."
	}
	return ""
}

func (g *GB) TakeAudio() []float32 {
	a := append([]float32(nil), g.apu.out...)
	g.apu.out = g.apu.out[:0]
	return a
}

func (g *GB) SRAM() []byte { return cartSram(g.cart) }

var stateMagic = []byte("RPGB\x01")

func (g *GB) state(s *sio) {
	s.Ints(&g.a, &g.f, &g.b, &g.c, &g.d, &g.e, &g.h, &g.l, &g.sp, &g.pc)
	s.Bool(&g.ime)
	s.Int(&g.eiDelay)
	s.Bools(&g.halted, &g.haltBug, &g.stopped, &g.locked)
	s.Ints(&g.div, &g.tima, &g.tma, &g.tac, &g.timaReload)
	s.Bool(&g.timaJustLoaded)
	s.Ints(&g.ly, &g.lyReg, &g.lyCmp, &g.m3end)
	s.Bools(&g.firstLine, &g.oamLock, &g.vramLock, &g.oamWLock, &g.vramWLock, &g.lycFlag)
	s.Ints(&g.serialBits, &g.lineDot, &g.mode)
	s.Bool(&g.statLine)
	s.Int(&g.winLine)
	s.Bool(&g.double)
	s.Ints(&g.key1, &g.hdmaSrc, &g.hdmaDst, &g.hdmaLen)
	s.Bools(&g.hdmaActive, &g.hdmaPending)
	s.Ints(&g.vbank, &g.wbank, &g.bcps, &g.ocps, &g.ie, &g.ifl, &g.joyp)
	s.I64(&g.cycles)
	s.I64(&g.clock)
	s.Ints(&g.lcdOffDots, &g.ppuNewIf)
	s.Bool(&g.dmaActive)
	s.Ints(&g.dmaDelay, &g.dmaIdx, &g.dmaBase, &g.dmaReq, &g.dmaByte)
	s.Ints(&g.keys, &g.tiltKeys)
	s.F64(&g.tiltX)
	s.F64(&g.tiltY)
	s.Bytes(g.vram[:])
	s.Bytes(g.wram[:])
	s.Bytes(g.oam[:])
	s.Bytes(g.hram[:])
	s.Bytes(g.io[:])
	s.Bytes(g.bgPal[:])
	s.Bytes(g.obPal[:])
	g.cart.state(s)
	g.apu.state(s)
	// the picture too: a loaded game shows at once, and lines the PPU doesn't redraw stay right
	s.U32s(g.screen)
}

func (g *GB) SaveState() ([]byte, error) {
	s := &sio{save: true, buf: append([]byte(nil), stateMagic...)}
	crc := int(g.romCRC)
	s.Int(&crc)
	g.state(s)
	return s.buf, s.err
}

func (g *GB) LoadState(b []byte) error {
	if !bytes.HasPrefix(b, stateMagic) {
		return errors.New("gb: not a Game Boy saved state")
	}
	s := &sio{buf: b, pos: len(stateMagic)}
	crc := 0
	s.Int(&crc)
	if s.err != nil || uint32(crc) != g.romCRC {
		return errors.New("gb: this saved state is from a different game")
	}
	// Restore into a copy first, so a damaged state leaves the game untouched.
	backup, err := g.SaveState()
	if err != nil {
		return err
	}
	g.state(s)
	if s.err == nil && s.pos != len(b) {
		s.err = errState
	}
	if s.err != nil {
		r := &sio{buf: backup, pos: len(stateMagic) + 8}
		g.state(r)
		return s.err
	}
	return nil
}
