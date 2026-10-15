// Package gba is the native Game Boy Advance emulator: a Go port of
// web/emu/gba.js and gba_gpio.js that behaves the same way, frame for frame
// and sample for sample.
//
// ARM7TDMI CPU (cores/arm7), memory bus with wait states, video (modes 0-5,
// sprites, windows, blending, mosaic), sound (four PSG channels + two DMA
// sound FIFOs), DMA, timers, interrupts, keypad, cartridge saves (SRAM, Flash,
// EEPROM), real-time clock and the sensor carts (gpio.go). No Nintendo BIOS is
// used: RetroPixels' own small BIOS handles interrupts and the BIOS calls
// (bios.go).
package gba

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"math"
	"time"

	"retropixels/core"
	"retropixels/cores/arm7"
)

const (
	frameLines = 228
	lineCycles = 1232
	hdraw      = 960
	cpuHz      = 16777216
	frameCyc   = lineCycles * frameLines
	fps        = float64(cpuHz) / frameCyc // 59.7275
	never      = math.MaxInt64             // no timer due
)

// RetroPixels BIOS (ARM code)
var biosWords = [16]uint32{
	0xeafffffe, // 0x00 reset (unused: the game starts directly)
	0xe1b0f00e, // 0x04 undefined instruction: movs pc, lr
	0xe1b0f00e, // 0x08 software interrupt (handled by RetroPixels): movs pc, lr
	0xe25ef004, // 0x0C prefetch abort: subs pc, lr, #4
	0xe25ef008, // 0x10 data abort: subs pc, lr, #8
	0xeafffffe, // 0x14 reserved
	0xea000000, // 0x18 IRQ: b 0x20
	0xe25ef004, // 0x1C FIQ: subs pc, lr, #4
	0xe92d500f, // 0x20 stmfd sp!, {r0-r3, r12, lr}
	0xe3a00301, // 0x24 mov r0, #0x04000000
	0xe28fe000, // 0x28 add lr, pc, #0
	0xe510f004, // 0x2C ldr pc, [r0, #-4]     (the game's handler at 0x03FFFFFC)
	0xe8bd500f, // 0x30 ldmfd sp!, {r0-r3, r12, lr}
	0xe25ef004, // 0x34 subs pc, lr, #4
	0xe55ec002, // 0x38 (not executed: value the real chip's open bus shows after an interrupt)
	0xe55ec002, // 0x3C
}

var prescale = [4]int64{1, 64, 256, 1024}
var wsN = [4]int{4, 3, 2, 8}

// Buttons in input bit order (the GBA's own KEYINPUT order). Sensor carts
// add their keys after these (Info().Buttons).
var Buttons = []string{"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"}

type timer struct {
	reload, ctrl, count int
	start               int64
}

type dmaCh struct {
	src, dst uint32
	count    int
	pending  bool
	latch    uint32
}

// GBA is one running game.
type GBA struct {
	cpu  *arm7.CPU
	now  func() time.Time // the computer's clock, for the cartridge clock
	rom  []byte
	save *saveMem
	gpio *gpioPort
	apu  *apu
	ppu  ppu

	romSize    int
	romCRC     uint32
	eepromBase uint32
	sampleRate int

	bios  [0x4000]byte
	ewram [0x40000]byte
	iwram [0x8000]byte
	pal   [0x400]byte
	vram  [0x18000]byte
	oam   [0x400]byte
	io    [0x400]byte

	screen []uint32
	keys   int

	// timing tables (per 16 MB region)
	n16, s16, n32, s32, f16, f32 [16]int64

	timers [4]timer
	dma    [4]dmaCh

	time, lineStart      int64
	line                 int
	nextTimer            int64
	waiting, stopped     bool
	lastFetch, biosLatch uint32
}

// New opens a game (.gba data).
func New(rom []byte, opts core.Options) (core.Core, error) { return newGBA(rom, opts, time.Now) }

func newGBA(romIn []byte, opts core.Options, now func() time.Time) (g *GBA, err error) {
	if len(romIn) == 0 {
		return nil, errors.New("This isn't a Game Boy Advance game file")
	}
	if len(romIn) > 64<<20 {
		return nil, errors.New("This file is too big to be a Game Boy Advance game")
	}
	defer func() {
		if r := recover(); r != nil {
			g, err = nil, fmt.Errorf("This Game Boy Advance game couldn't be started (%v).", r)
		}
	}()
	size := 4
	for size < len(romIn) {
		size <<= 1
	}
	g = &GBA{now: now, romSize: len(romIn), romCRC: crc32.ChecksumIEEE(romIn)}
	g.rom = make([]byte, max(size, 0x100))
	copy(g.rom, romIn)
	for i, w := range biosWords {
		binary.LittleEndian.PutUint32(g.bios[i*4:], w)
	}
	g.save = newSave(g.rom)
	if len(opts.SRAM) > 0 {
		g.save.load(opts.SRAM)
	}
	// extra chips on the cartridge's GPIO pins (clock, sensors, rumble): see gpio.go
	g.gpio = newGPIO(g, g.rom, g.save.rtc)
	g.eepromBase = 0x0d000000
	if g.romSize > 0x1000000 {
		g.eepromBase = 0x0dffff00
	}
	g.sampleRate = opts.SampleRate
	if g.sampleRate <= 0 {
		g.sampleRate = 48000
	}
	g.apu = newAPU(g, g.sampleRate)
	g.ppu.g = g
	g.cpu = arm7.New(bus{g})
	g.screen = make([]uint32, 240*160)
	g.Reset()
	return g, nil
}

// bus is the processor's view of the machine (kept apart so the core's own
// methods don't show up as the arm7 interfaces).
type bus struct{ g *GBA }

func (b bus) Read8(a uint32) uint32 {
	g := b.g
	g.cpu.Cycles += g.n16[a>>24&15]
	return g.read8raw(a)
}
func (b bus) Read16(a uint32) uint32 {
	g := b.g
	g.cpu.Cycles += g.n16[a>>24&15]
	return g.read16raw(a)
}
func (b bus) Read32(a uint32) uint32 {
	g := b.g
	g.cpu.Cycles += g.n32[a>>24&15]
	return g.read32raw(a)
}
func (b bus) Write8(a, v uint32) {
	g := b.g
	g.cpu.Cycles += g.n16[a>>24&15]
	g.write8raw(a, v)
}
func (b bus) Write16(a, v uint32) {
	g := b.g
	g.cpu.Cycles += g.n16[a>>24&15]
	g.write16raw(a, v)
}
func (b bus) Write32(a, v uint32) {
	g := b.g
	g.cpu.Cycles += g.n32[a>>24&15]
	g.write32raw(a, v)
}
func (b bus) Fetch16(a uint32) uint32       { return b.g.fetch16(a) }
func (b bus) Fetch32(a uint32) uint32       { return b.g.fetch32(a) }
func (b bus) Branched(a uint32, thumb bool) { b.g.branched(a, thumb) }
func (b bus) SWI(n uint32) bool             { return b.g.swi(n) }

// ---- little-endian views of the memories ----

func le16(b []byte) uint32 { return uint32(binary.LittleEndian.Uint16(b)) }
func le32(b []byte) uint32 { return binary.LittleEndian.Uint32(b) }

func (g *GBA) io16(o int) uint16    { return binary.LittleEndian.Uint16(g.io[o&0x3fe:]) }
func (g *GBA) setIO16(o int, v int) { binary.LittleEndian.PutUint16(g.io[o&0x3fe:], uint16(v)) }
func (g *GBA) io32(o int) uint32    { return binary.LittleEndian.Uint32(g.io[o&0x3fc:]) }
func (g *GBA) pal16(i int) uint16   { return binary.LittleEndian.Uint16(g.pal[(i*2)&0x3fe:]) }
func (g *GBA) oam16(i int) int      { return int(binary.LittleEndian.Uint16(g.oam[(i*2)&0x3fe:])) }

func (g *GBA) Reset() {
	g.ewram = [0x40000]byte{}
	g.iwram = [0x8000]byte{}
	g.pal = [0x400]byte{}
	g.vram = [0x18000]byte{}
	g.oam = [0x400]byte{}
	g.io = [0x400]byte{}
	g.timers = [4]timer{}
	g.dma = [4]dmaCh{}
	g.apu.reset()
	g.ppu.affX, g.ppu.affY = [2]int32{}, [2]int32{}
	g.time, g.line, g.lineStart, g.nextTimer, g.waiting, g.stopped = 0, 0, 0, never, false, false
	c := g.cpu
	c.Reset()
	// state the real start-up code leaves behind
	c.SwitchMode(0x13)
	c.R[13], c.R[14] = 0x03007fe0, 0
	c.SwitchMode(0x12)
	c.R[13], c.R[14] = 0x03007fa0, 0
	c.SwitchMode(0x1f)
	c.R[13] = 0x03007f00
	c.I, c.F, c.T = 0, 0, 0
	c.Cycles = 0
	c.Branch(0x08000000)
	g.setIO16(0, 0x80)
	g.setIO16(0x20, 0x100)
	g.setIO16(0x26, 0x100)
	g.setIO16(0x30, 0x100)
	g.setIO16(0x36, 0x100)
	g.setIO16(0x88, 0x200)
	g.setIO16(0x134, 0x8000) // link port idle (general-purpose mode)
	g.io[0x300] = 1
	g.updateWait(0)
	g.lastFetch, g.biosLatch = 0, 0xe129f000
}

// ---------- timing ----------

func (g *GBA) updateWait(w int) {
	g.setIO16(0x204, w)
	sram := int64(1 + wsN[w&3])
	ws := [3][2]int64{
		{int64(1 + wsN[w>>2&3]), int64(1 + [2]int{2, 1}[w>>4&1])},
		{int64(1 + wsN[w>>5&3]), int64(1 + [2]int{4, 1}[w>>7&1])},
		{int64(1 + wsN[w>>8&3]), int64(1 + [2]int{8, 1}[w>>10&1])},
	}
	pf := w&0x4000 != 0
	set := func(r int, n16, s16, n32, s32 int64) {
		g.n16[r], g.s16[r], g.n32[r], g.s32[r], g.f16[r], g.f32[r] = n16, s16, n32, s32, s16, s32
	}
	for r := 0; r < 16; r++ {
		set(r, 1, 1, 1, 1)
	}
	set(2, 3, 3, 6, 6)
	set(5, 1, 1, 2, 2)
	set(6, 1, 1, 2, 2)
	for k := 0; k < 3; k++ {
		for _, r := range [2]int{8 + k*2, 9 + k*2} {
			n, s := ws[k][0], ws[k][1]
			set(r, n, s, n+s, s*2)
			if pf {
				g.f16[r], g.f32[r] = 1, 2
			}
		}
	}
	set(14, sram, sram, sram, sram)
	set(15, sram, sram, sram, sram)
}

func (g *GBA) branched(a uint32, thumb bool) {
	r := a >> 24 & 15
	if thumb {
		g.cpu.Cycles += g.n16[r] - g.f16[r]
	} else {
		g.cpu.Cycles += g.n32[r] - g.f32[r]
	}
}

// ---------- instruction fetch ----------
// (The JS processor passes fetch addresses as signed 32-bit numbers, so the
// BIOS check sees addresses from 0x80000000 up as below 0x4000.)

func (g *GBA) fetch16(a uint32) uint32 {
	r := a >> 24 & 15
	g.cpu.Cycles += g.f16[r]
	var v uint32
	switch r {
	case 0:
		if int32(a) < 0x4000 {
			v = le16(g.bios[a&0x3ffe:])
			g.biosLatch = le32(g.bios[a&0x3ffc:])
		} else {
			v = g.lastFetch & 0xffff
		}
	case 2:
		v = le16(g.ewram[a&0x3fffe:])
	case 3:
		v = le16(g.iwram[a&0x7ffe:])
	case 8, 9, 10, 11, 12, 13:
		v = g.romRead16(a)
	default:
		v = g.read16raw(a)
	}
	g.lastFetch = v | v<<16
	return v
}

func (g *GBA) fetch32(a uint32) uint32 {
	r := a >> 24 & 15
	g.cpu.Cycles += g.f32[r]
	var v uint32
	switch r {
	case 0:
		if int32(a) < 0x4000 {
			v = le32(g.bios[a&0x3ffc:])
			g.biosLatch = v
		} else {
			v = g.lastFetch
		}
	case 2:
		v = le32(g.ewram[a&0x3fffc:])
	case 3:
		v = le32(g.iwram[a&0x7ffc:])
	case 8, 9, 10, 11, 12, 13:
		v = g.romRead32(a)
	default:
		v = g.read32raw(a)
	}
	g.lastFetch = v
	return v
}

func (g *GBA) romRead16(a uint32) uint32 {
	o := a & 0x1fffffe
	if g.gpio != nil && g.gpio.onPins && o >= 0xc4 && o <= 0xc8 && g.gpio.ctrl&1 != 0 {
		return g.gpio.read16(o)
	}
	if int(o) >= len(g.rom) {
		return o >> 1 & 0xffff
	}
	return le16(g.rom[o:])
}

func (g *GBA) romRead32(a uint32) uint32 {
	o := a & 0x1fffffc
	if int(o) >= len(g.rom) || (g.gpio != nil && o >= 0xc4 && o <= 0xc8) {
		return g.romRead16(o) | g.romRead16(o+2)<<16
	}
	return le32(g.rom[o:])
}

// ---------- data access ----------

func (g *GBA) openBus() uint32 { return g.lastFetch }

func (g *GBA) biosRead32(a uint32) uint32 {
	if g.cpu.PC < 0x4000 {
		return le32(g.bios[a&0x3ffc:])
	}
	return g.biosLatch
}

func (g *GBA) isEEPROM(a uint32) bool { return g.save.typ == saveEEPROM && a >= g.eepromBase }

func (g *GBA) read8raw(a uint32) uint32 {
	switch a >> 24 {
	case 0:
		if a < 0x4000 {
			return g.biosRead32(a) >> ((a & 3) * 8) & 0xff
		}
		return g.openBus() >> ((a & 3) * 8) & 0xff
	case 2:
		return uint32(g.ewram[a&0x3ffff])
	case 3:
		return uint32(g.iwram[a&0x7fff])
	case 4:
		if a&0xffffff < 0x400 {
			return g.ioRead16(int(a&0x3fe)) >> ((a & 1) * 8) & 0xff
		}
		return 0
	case 5:
		return uint32(g.pal[a&0x3ff])
	case 6:
		if g.vramHole(a) {
			return 0
		}
		return uint32(g.vram[vaddr(a)])
	case 7:
		return uint32(g.oam[a&0x3ff])
	case 8, 9, 10, 11, 12:
		return g.romRead16(a) >> ((a & 1) * 8) & 0xff
	case 13:
		if g.isEEPROM(a) {
			return g.save.eepromRead()
		}
		return g.romRead16(a) >> ((a & 1) * 8) & 0xff
	case 14, 15:
		return g.sramRead8(a)
	}
	return g.openBus() >> ((a & 3) * 8) & 0xff
}

func (g *GBA) read16raw(a uint32) uint32 {
	switch a >> 24 {
	case 0:
		if a < 0x4000 {
			return g.biosRead32(a) >> ((a & 2) * 8) & 0xffff
		}
		return g.openBus() >> ((a & 2) * 8) & 0xffff
	case 2:
		return le16(g.ewram[a&0x3fffe:])
	case 3:
		return le16(g.iwram[a&0x7ffe:])
	case 4:
		if a&0xffffff < 0x400 {
			return g.ioRead16(int(a & 0x3fe))
		}
		return 0
	case 5:
		return le16(g.pal[a&0x3fe:])
	case 6:
		if g.vramHole(a) {
			return 0
		}
		return le16(g.vram[vaddr(a)&^1:])
	case 7:
		return le16(g.oam[a&0x3fe:])
	case 8, 9, 10, 11, 12:
		return g.romRead16(a)
	case 13:
		if g.isEEPROM(a) {
			return g.save.eepromRead()
		}
		return g.romRead16(a)
	case 14, 15:
		return g.sramRead8(a) * 0x101
	}
	return g.openBus() >> ((a & 2) * 8) & 0xffff
}

func (g *GBA) read32raw(a uint32) uint32 {
	switch a >> 24 {
	case 0:
		if a < 0x4000 {
			return g.biosRead32(a)
		}
		return g.openBus()
	case 2:
		return le32(g.ewram[a&0x3fffc:])
	case 3:
		return le32(g.iwram[a&0x7ffc:])
	case 4:
		if a&0xffffff < 0x400 {
			o := int(a & 0x3fc)
			return g.ioRead16(o) | g.ioRead16(o+2)<<16
		}
		return 0
	case 5:
		return le32(g.pal[a&0x3fc:])
	case 6:
		if g.vramHole(a) {
			return 0
		}
		return le32(g.vram[vaddr(a)&^3:])
	case 7:
		return le32(g.oam[a&0x3fc:])
	case 8, 9, 10, 11, 12:
		return g.romRead32(a)
	case 13:
		if g.isEEPROM(a) {
			return g.save.eepromRead()
		}
		return g.romRead32(a)
	case 14, 15:
		return g.sramRead8(a) * 0x1010101
	}
	return g.openBus()
}

func vaddr(a uint32) uint32 {
	a &= 0x1ffff
	if a >= 0x18000 {
		return a - 0x8000
	}
	return a
}

// vramHole: in the bitmap modes the mirror at 0x18000-0x1BFFF goes nowhere
// (its bank belongs to the backgrounds).
func (g *GBA) vramHole(a uint32) bool {
	a &= 0x1ffff
	return a >= 0x18000 && a < 0x1c000 && g.io16(0)&7 >= 3
}

// save memory area (the tilt sensor of a few carts answers here too)
func (g *GBA) sramRead8(a uint32) uint32 {
	if g.gpio != nil && g.gpio.tilt != nil {
		if v, ok := g.gpio.busRead8(a); ok {
			return v
		}
	}
	return g.save.read8(a)
}

func (g *GBA) sramWrite8(a, v uint32) {
	if g.gpio != nil && g.gpio.tilt != nil && g.gpio.busWrite8(a, v) {
		return
	}
	g.save.write8(a, v)
}

func (g *GBA) gpioAddr(o uint32) bool { return g.gpio != nil && o >= 0xc4 && o <= 0xc8 }

func (g *GBA) write8raw(a, v uint32) {
	v &= 0xff
	switch a >> 24 {
	case 2:
		g.ewram[a&0x3ffff] = byte(v)
	case 3:
		g.iwram[a&0x7fff] = byte(v)
	case 4:
		if a&0xffffff < 0x400 {
			g.ioWrite8(int(a&0x3ff), int(v))
		}
	case 5:
		binary.LittleEndian.PutUint16(g.pal[a&0x3fe:], uint16(v*0x101))
	case 6:
		o := vaddr(a)
		lim := uint32(0x10000)
		if g.io16(0)&7 >= 3 {
			lim = 0x14000
		}
		if o < lim && !g.vramHole(a) {
			binary.LittleEndian.PutUint16(g.vram[o&^1:], uint16(v*0x101))
		}
	case 8:
		if a&1 == 0 && g.gpioAddr(a&0x1ffffff) {
			g.gpio.write16(a&0x1fffffe, v)
		}
	case 14, 15:
		g.sramWrite8(a, v)
	}
}

func (g *GBA) write16raw(a, v uint32) {
	v &= 0xffff
	switch a >> 24 {
	case 2:
		binary.LittleEndian.PutUint16(g.ewram[a&0x3fffe:], uint16(v))
	case 3:
		binary.LittleEndian.PutUint16(g.iwram[a&0x7ffe:], uint16(v))
	case 4:
		if a&0xffffff < 0x400 {
			g.ioWrite16(int(a&0x3fe), int(v))
		}
	case 5:
		binary.LittleEndian.PutUint16(g.pal[a&0x3fe:], uint16(v))
	case 6:
		if !g.vramHole(a) {
			binary.LittleEndian.PutUint16(g.vram[vaddr(a)&^1:], uint16(v))
		}
	case 7:
		binary.LittleEndian.PutUint16(g.oam[a&0x3fe:], uint16(v))
	case 8:
		if g.gpioAddr(a & 0x1fffffe) {
			g.gpio.write16(a&0x1fffffe, v)
		}
	case 13:
		if g.isEEPROM(a) {
			g.save.eepromWrite(v & 1)
		}
	case 14, 15:
		g.sramWrite8(a, v>>((a&1)*8)&0xff)
	}
}

func (g *GBA) write32raw(a, v uint32) {
	switch a >> 24 {
	case 2:
		binary.LittleEndian.PutUint32(g.ewram[a&0x3fffc:], v)
	case 3:
		binary.LittleEndian.PutUint32(g.iwram[a&0x7ffc:], v)
	case 4:
		if a&0xffffff < 0x400 {
			o := int(a & 0x3fc)
			if o == 0xa0 || o == 0xa4 {
				k := 0
				if o == 0xa4 {
					k = 1
				}
				g.apu.fifoWrite(k, v, 4)
				return
			}
			g.ioWrite16(o, int(v&0xffff))
			g.ioWrite16(o+2, int(v>>16))
		}
	case 5:
		binary.LittleEndian.PutUint32(g.pal[a&0x3fc:], v)
	case 6:
		if !g.vramHole(a) {
			binary.LittleEndian.PutUint32(g.vram[vaddr(a)&^3:], v)
		}
	case 7:
		binary.LittleEndian.PutUint32(g.oam[a&0x3fc:], v)
	case 8:
		if g.gpio != nil {
			g.write16raw(a&^3, v&0xffff)
			g.write16raw(a&^3+2, v>>16)
		}
	case 13:
		if g.isEEPROM(a) {
			g.save.eepromWrite(v & 1)
		}
	case 14, 15:
		g.sramWrite8(a, v>>((a&3)*8)&0xff)
	}
}

// ---------- I/O registers ----------

func (g *GBA) ioRead16(o int) uint32 {
	switch o {
	case 0x004:
		pos := g.cpu.Cycles - g.lineStart
		stat := int(g.io16(4))
		vb, hb, vc := 0, 0, 0
		if g.line >= 160 && g.line < 227 {
			vb = 1
		}
		if pos >= hdraw {
			hb = 2
		}
		if g.line == stat>>8 {
			vc = 4
		}
		return uint32(stat&0xff38 | vb | hb | vc)
	case 0x006:
		return uint32(g.line)
	case 0x084:
		v := uint32(g.io16(0x84) & 0x80)
		for i := range g.apu.ch {
			if g.apu.ch[i].on {
				v |= 1 << i
			}
		}
		return v
	case 0x100, 0x104, 0x108, 0x10c:
		return uint32(g.timerCount((o - 0x100) >> 2))
	case 0x130:
		return uint32(^g.keys & 0x3ff)
	case 0x202:
		return uint32(g.io16(0x202))
	case 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e:
		return 0
	}
	if o >= 0x90 && o < 0xa0 {
		i := o - 0x90
		return uint32(g.apu.waveRead(i) | g.apu.waveRead(i+1)<<8)
	}
	if o >= 0xa0 && o < 0xe0 {
		// DMA registers: only the control halves read back
		if o == 0xba || o == 0xc6 || o == 0xd2 || o == 0xde {
			return uint32(g.io16(o))
		}
		return 0
	}
	return uint32(g.io16(o))
}

func (g *GBA) ioWrite8(o, v int) {
	if o == 0x301 { // HALTCNT: bit 7 = Stop (deep sleep)
		g.cpu.Halted = true
		g.stopped = v&0x80 != 0
		return
	}
	if o == 0x202 || o == 0x203 {
		g.setIO16(0x202, int(g.io16(0x202))&^(v<<((o&1)*8)))
		g.updateIrq()
		return
	}
	if o >= 0xa0 && o < 0xa8 {
		k := 1
		if o < 0xa4 {
			k = 0
		}
		g.apu.fifoWrite(k, uint32(v), 1)
		return
	}
	cur := int(g.io16(o &^ 1))
	var nv int
	if o&1 != 0 {
		nv = cur&0xff | v<<8
	} else {
		nv = cur&0xff00 | v
	}
	g.ioWrite16(o&^1, nv)
}

func (g *GBA) ioWrite16(o, v int) {
	if o >= 0x60 && o <= 0x84 {
		g.apu.advance(g.cpu.Cycles)
		if o == 0x84 {
			old := int(g.io16(0x84))
			g.apu.write16(0x84, v)
			g.setIO16(0x84, old&^0x80|v&0x80)
			return
		}
		g.setIO16(o, v)
		g.apu.write16(o, v)
		return
	}
	if o >= 0x90 && o < 0xa0 {
		g.apu.advance(g.cpu.Cycles)
		i := o - 0x90
		g.apu.waveWrite(i, v&0xff)
		g.apu.waveWrite(i+1, v>>8)
		return
	}
	if o >= 0xa0 && o < 0xa8 {
		k := 1
		if o < 0xa4 {
			k = 0
		}
		g.apu.fifoWrite(k, uint32(v), 2)
		return
	}
	switch o {
	case 0x004:
		g.setIO16(4, int(g.io16(4))&7|v&0xff38)
		return
	case 0x006:
		return
	case 0x028, 0x02a:
		g.setIO16(o, v)
		g.ppu.writeRef(0, false)
		return
	case 0x02c, 0x02e:
		g.setIO16(o, v)
		g.ppu.writeRef(0, true)
		return
	case 0x038, 0x03a:
		g.setIO16(o, v)
		g.ppu.writeRef(1, false)
		return
	case 0x03c, 0x03e:
		g.setIO16(o, v)
		g.ppu.writeRef(1, true)
		return
	case 0x0ba, 0x0c6, 0x0d2, 0x0de:
		g.dmaControl((o-0xba)/12, v)
		return
	case 0x100, 0x104, 0x108, 0x10c:
		g.syncTimers()
		g.timers[(o-0x100)>>2].reload = v
		return
	case 0x102, 0x106, 0x10a, 0x10e:
		g.timerControl((o-0x102)>>2, v)
		return
	case 0x128: // SIOCNT: with no link cable attached, transfers finish on their own
		g.setIO16(0x128, v)
		mode := v >> 12 & 3
		multi := mode == 2 && g.io16(0x134)&0x8000 == 0
		if v&0x80 != 0 && (multi || v&1 != 0) {
			if multi {
				g.setIO16(0x120, int(g.io16(0x12a)))
				g.setIO16(0x122, 0xffff)
				g.setIO16(0x124, 0xffff)
				g.setIO16(0x126, 0xffff)
			} else {
				g.setIO16(0x120, 0xffff)
				g.setIO16(0x122, 0xffff)
			}
			g.setIO16(0x128, v&^0x80)
			if v&0x4000 != 0 {
				g.raise(0x80)
			}
		}
		if mode == 2 { // nothing plugged in: this GBA is the parent and no one else is ready
			g.setIO16(0x128, int(g.io16(0x128))&^0x0c)
		}
		return
	case 0x130:
		return
	case 0x132:
		g.setIO16(0x132, v)
		g.checkKeyIrq()
		return
	case 0x200:
		g.setIO16(0x200, v)
		g.updateIrq()
		return
	case 0x202:
		g.setIO16(0x202, int(g.io16(0x202))&^v)
		g.updateIrq()
		return
	case 0x204:
		g.updateWait(v & 0x5fff)
		return
	case 0x208:
		g.setIO16(0x208, v&1)
		g.updateIrq()
		return
	case 0x300:
		g.setIO16(0x300, int(g.io16(0x300))&0xff00|v&1)
		if v&0xff00 != 0 {
			g.cpu.Halted = true
			g.stopped = v&0x8000 != 0
		}
		return
	}
	g.setIO16(o, v)
}

// ---------- interrupts ----------

func (g *GBA) raise(bit int) {
	g.setIO16(0x202, int(g.io16(0x202))|bit)
	g.updateIrq()
}

func (g *GBA) updateIrq() {
	pend := g.io16(0x200) & g.io16(0x202) & 0x3fff
	g.cpu.IRQLine = pend != 0 && g.io16(0x208)&1 != 0
	// Stop mode only wakes for the keypad, the cartridge or the link port
	if pend != 0 && (!g.stopped || pend&0x3080 != 0) {
		g.cpu.Halted = false
		g.stopped = false
	}
}

func (g *GBA) checkKeyIrq() {
	cnt := int(g.io16(0x132))
	if cnt&0x4000 == 0 {
		return
	}
	sel := cnt & 0x3ff
	pressed := g.keys & sel
	if (cnt&0x8000 != 0 && pressed == sel) || (cnt&0x8000 == 0 && pressed != 0) {
		g.raise(0x1000)
	}
}

// ---------- timers ----------

func (g *GBA) timerCount(i int) int {
	t := &g.timers[i]
	if t.ctrl&0x80 != 0 && t.ctrl&4 == 0 {
		v := int64(t.count) + max(0, g.cpu.Cycles-t.start)/prescale[t.ctrl&3]
		// an overflow that is due but not handled yet (we're mid-instruction): count on from the reload value
		if v > 0xffff {
			v = int64(t.reload) + (v-0x10000)%int64(0x10000-t.reload)
		}
		return int(v & 0xffff)
	}
	return t.count & 0xffff
}

func (g *GBA) syncTimers() { g.runTimers(g.cpu.Cycles) }

func (g *GBA) timerControl(i, v int) {
	g.syncTimers()
	t := &g.timers[i]
	was := t.ctrl & 0x80
	if t.ctrl&0x80 != 0 && t.ctrl&4 == 0 {
		t.count = g.timerCount(i)
		t.start = max(t.start, g.cpu.Cycles)
	}
	t.ctrl = v & 0xc7
	g.setIO16(0x102+i*4, t.ctrl)
	if was == 0 && v&0x80 != 0 {
		t.count = t.reload
		t.start = g.cpu.Cycles
	}
	g.scheduleTimers()
}

func (g *GBA) overflowAt(i int) (int64, bool) {
	t := &g.timers[i]
	if t.ctrl&0x80 == 0 || (t.ctrl&4 != 0 && i > 0) {
		return 0, false
	}
	return t.start + int64(0x10000-t.count)*prescale[t.ctrl&3], true
}

func (g *GBA) scheduleTimers() {
	next := int64(never)
	for i := 0; i < 4; i++ {
		if ov, ok := g.overflowAt(i); ok && ov < next {
			next = ov
		}
	}
	g.nextTimer = next
}

func (g *GBA) timerOverflow(i int, when int64) {
	t := &g.timers[i]
	if t.ctrl&0x40 != 0 {
		g.raise(8 << i)
	}
	if i < 2 {
		g.apu.advance(when)
		g.apu.timerOverflow(i)
	}
	if i < 3 {
		n := &g.timers[i+1]
		if n.ctrl&0x84 == 0x84 {
			n.count++
			if n.count > 0xffff {
				n.count = n.reload
				g.timerOverflow(i+1, when)
			}
		}
	}
}

func (g *GBA) runTimers(now int64) {
	for guard := 0; guard < 100000 && g.nextTimer <= now; guard++ {
		// handle the earliest overflow
		best, bestT := -1, int64(never)
		for i := 0; i < 4; i++ {
			if ov, ok := g.overflowAt(i); ok && ov < bestT {
				bestT, best = ov, i
			}
		}
		if best < 0 || bestT > now {
			break
		}
		t := &g.timers[best]
		t.count = t.reload
		t.start = bestT
		g.timerOverflow(best, bestT)
		g.scheduleTimers()
	}
	g.scheduleTimers()
}

// ---------- DMA ----------

func dmaBase(i int) int { return 0xb0 + i*12 }

func (g *GBA) dmaCount(i int) int {
	base := dmaBase(i)
	n := int(g.io16(base + 8))
	if n == 0 {
		n = 0x4000
		if i == 3 {
			n = 0x10000
		}
	}
	if i < 3 {
		n &= 0x3fff
		if n == 0 {
			n = 0x4000
		}
	}
	return n
}

func (g *GBA) dmaDst(i int) uint32 {
	base := dmaBase(i)
	m := uint32(0x07ffffff)
	if i == 3 {
		m = 0x0fffffff
	}
	return g.io32(base+4) & m
}

func (g *GBA) dmaControl(i, v int) {
	base := dmaBase(i)
	d := &g.dma[i]
	old := int(g.io16(base + 10))
	g.setIO16(base+10, v)
	if old&0x8000 == 0 && v&0x8000 != 0 {
		m := uint32(0x0fffffff)
		if i == 0 {
			m = 0x07ffffff
		}
		d.src = g.io32(base) & m
		d.dst = g.dmaDst(i)
		d.count = g.dmaCount(i)
		if v>>12&3 == 0 {
			g.runDma(i)
		}
	}
}

func (g *GBA) triggerDma(timing int) {
	for i := 0; i < 4; i++ {
		cnt := int(g.io16(dmaBase(i) + 10))
		if cnt&0x8000 != 0 && cnt>>12&3 == timing {
			g.runDma(i)
		}
	}
}

func (g *GBA) fifoRequest(k int) {
	i := k + 1
	cnt := int(g.io16(dmaBase(i) + 10))
	if cnt&0x8000 != 0 && cnt>>12&3 == 3 {
		g.runDma(i)
	}
}

func (g *GBA) runDma(i int) {
	base := dmaBase(i)
	d := &g.dma[i]
	cnt := int(g.io16(base + 10))
	fifo := (i == 1 || i == 2) && cnt>>12&3 == 3
	word := fifo || cnt&0x400 != 0
	dctl, sctl := cnt>>5&3, cnt>>7&3
	if fifo {
		dctl = 2
	}
	unit := uint32(2)
	if word {
		unit = 4
	}
	step := func(ctl int) uint32 {
		switch ctl {
		case 1:
			return -unit
		case 2:
			return 0
		}
		return unit
	}
	sstep, dstep := step(sctl), step(dctl)
	n := d.count
	if fifo {
		n = 4
	}
	src, dst := d.src, d.dst
	eeprom := g.save.typ == saveEEPROM && (dst>>24 == 13 || src>>24 == 13)
	b := bus{g}
	if word {
		for k := 0; k < n; k++ {
			if src >= 0x02000000 {
				d.latch = b.Read32(src &^ 3)
			} else {
				g.cpu.Cycles++
			}
			b.Write32(dst&^3, d.latch)
			src += sstep
			dst += dstep
		}
	} else {
		// each channel keeps the last value it read; reads below 0x02000000 (the BIOS) don't reach the bus and reuse it
		for k := 0; k < n; k++ {
			if src >= 0x02000000 {
				v := b.Read16(src &^ 1)
				d.latch = v | v<<16
			} else {
				g.cpu.Cycles++
			}
			if dst&2 != 0 {
				b.Write16(dst&^1, d.latch>>16)
			} else {
				b.Write16(dst&^1, d.latch&0xffff)
			}
			src += sstep
			dst += dstep
		}
	}
	g.cpu.Cycles += 2
	d.src = src
	if !fifo {
		d.dst = dst
	}
	if eeprom {
		g.save.eepromDmaDone(n)
	}
	if cnt&0x200 != 0 && cnt>>12&3 != 0 {
		d.count = g.dmaCount(i)
		if dctl == 3 {
			d.dst = g.dmaDst(i)
		}
	} else {
		g.setIO16(base+10, cnt&0x7fff)
	}
	if cnt&0x4000 != 0 {
		g.raise(0x100 << i)
	}
}

// ---------- running ----------

func (g *GBA) runUntil(t int64) {
	cpu := g.cpu
	for cpu.Cycles < t {
		stop := min(t, g.nextTimer)
		if cpu.Halted {
			if cpu.Cycles < stop {
				cpu.Cycles = stop
			}
		} else {
			for cpu.Cycles < stop {
				cpu.Step()
				if cpu.Halted {
					break
				}
			}
		}
		if cpu.Cycles >= g.nextTimer {
			g.runTimers(cpu.Cycles)
		}
	}
}

func (g *GBA) RunFrame() {
	if g.gpio != nil {
		g.gpio.frameStart()
	}
	for line := 0; line < frameLines; line++ {
		g.line = line
		g.lineStart = g.time + int64(line)*lineCycles
		stat := int(g.io16(4))
		if line == stat>>8 && stat&0x20 != 0 {
			g.raise(4)
		}
		if line == 160 {
			if stat&8 != 0 {
				g.raise(1)
			}
			g.triggerDma(1)
		}
		if line == 0 {
			g.ppu.latchAffine()
		}
		g.runUntil(g.lineStart + hdraw)
		if line < 160 {
			g.ppu.renderLine(line, g.screen)
		}
		if g.io16(4)&0x10 != 0 {
			g.raise(2)
		}
		if line < 160 {
			g.triggerDma(2)
		}
		g.runUntil(g.lineStart + lineCycles)
	}
	g.time += frameCyc
	g.apu.advance(g.cpu.Cycles)
	if g.gpio != nil {
		g.gpio.frameEnd(frameCyc)
	}
}

// ---------- core.Core ----------

func (g *GBA) Info() core.Info {
	b := Buttons
	if g.gpio != nil {
		if x := g.gpio.buttons(); x != nil {
			b = append(append([]string{}, Buttons...), x...)
		}
	}
	return core.Info{System: "gba", Buttons: b, Players: 1, Stereo: true, Aspect: 3.0 / 2}
}

func (g *GBA) FPS() float64     { return fps }
func (g *GBA) Width() int       { return 240 }
func (g *GBA) Height() int      { return 160 }
func (g *GBA) Screen() []uint32 { return g.screen }

// SetInput: bits in the order of Buttons, then the sensor cart's keys.
func (g *GBA) SetInput(player int, bits uint32) {
	if player != 0 {
		return
	}
	was := g.keys
	g.keys = int(bits & 0x3ff)
	if g.keys != was {
		g.checkKeyIrq()
	}
	if g.gpio != nil {
		g.gpio.extra = int(bits >> 10) // sensor keys come after the ten GBA buttons
	}
}

// SetAnalog: sensor carts read the right stick (axes 2 and 3).
func (g *GBA) SetAnalog(player, axis int, v float32) {
	if player != 0 || g.gpio == nil {
		return
	}
	switch axis {
	case 2:
		g.gpio.stickX = float64(v)
	case 3:
		g.gpio.stickY = float64(v)
	}
}

// UsesRightStick: sensor cartridges read the right stick themselves, so the
// app leaves it alone (no recording shortcut on it).
func (g *GBA) UsesRightStick() bool { return g.gpio != nil && g.gpio.buttons() != nil }

// ControlsNote is shown in the Controls window for games with extra controls.
func (g *GBA) ControlsNote() string {
	if g.gpio == nil {
		return ""
	}
	return g.gpio.note()
}

// Rumble: how much of the frame the cartridge's rumble motor ran (0..1).
func (g *GBA) Rumble() float32 {
	if g.gpio == nil || !g.gpio.hasRumble {
		return 0
	}
	return float32(g.gpio.rumbleLevel)
}

// HasRumble: the cartridge has a rumble motor.
func (g *GBA) HasRumble() bool { return g.gpio != nil && g.gpio.hasRumble }

func (g *GBA) TakeAudio() []float32 {
	a := append([]float32(nil), g.apu.out...)
	g.apu.out = g.apu.out[:0]
	return a
}

func (g *GBA) SRAM() []byte {
	if g.save.typ == saveNone && !g.save.dirty {
		return nil
	}
	return g.save.bytes()
}

// ---------- save states ----------

var stateMagic = []byte("RPGBA\x01")

func (g *GBA) state(s *sio) {
	cs := g.cpu.SaveState()
	s.U32s(cs.R[:])
	s.U32s(cs.Bank13[:])
	s.U32s(cs.Bank14[:])
	s.U32s(cs.SPSR[:])
	s.U32s(cs.FIQ[:])
	s.U32s(cs.USR[:])
	s.U32s(cs.Pipe[:])
	pc := []uint32{cs.CPSR, cs.PC}
	s.U32s(pc)
	cs.CPSR, cs.PC = pc[0], pc[1]
	s.I64(&cs.Cycles)
	s.Bools(&cs.Halted, &cs.IRQLine)
	if !s.save {
		g.cpu.LoadState(cs)
	}
	s.Bytes(g.ewram[:])
	s.Bytes(g.iwram[:])
	s.Bytes(g.pal[:])
	s.Bytes(g.vram[:])
	s.Bytes(g.oam[:])
	s.Bytes(g.io[:])
	for i := range g.timers {
		t := &g.timers[i]
		s.Ints(&t.reload, &t.ctrl, &t.count)
		s.I64(&t.start)
	}
	for i := range g.dma {
		d := &g.dma[i]
		u := []uint32{d.src, d.dst, d.latch}
		s.U32s(u)
		d.src, d.dst, d.latch = u[0], u[1], u[2]
		s.Int(&d.count)
		s.Bool(&d.pending)
	}
	g.apu.state(s)
	s.I32s(g.ppu.affX[:])
	s.I32s(g.ppu.affY[:])
	g.save.state(s)
	if g.gpio != nil {
		if g.gpio.rtc != nil {
			g.gpio.rtc.stateIO(s)
		}
		g.gpio.state(s)
	}
	s.I64(&g.time)
	s.I64(&g.lineStart)
	s.Ints(&g.line, &g.keys)
	lf := []uint32{g.lastFetch, g.biosLatch}
	s.U32s(lf)
	g.lastFetch, g.biosLatch = lf[0], lf[1]
	s.Bools(&g.waiting, &g.stopped)
	// the picture too, so a loaded game shows at once
	s.U32s(g.screen)
}

// stateOK: the values a loaded state uses as indexes are in range, so a
// damaged state can't crash the emulator later.
func (g *GBA) stateOK() bool {
	in := func(v, lo, hi int) bool { return v >= lo && v <= hi }
	a, sv := g.apu, g.save
	for k := 0; k < 2; k++ {
		if !in(a.fifoR[k], 0, 31) || !in(a.fifoW[k], 0, 31) || !in(a.fifoN[k], 0, 32) {
			return false
		}
	}
	for i := range a.ch {
		if !in(a.ch[i].pos, 0, 63) || !in(a.ch[i].duty, 0, 3) {
			return false
		}
	}
	if !in(sv.typ, saveNone, saveSRAM) || !in(sv.fBank, 0, 1) || sv.fBank*0x10000 >= len(sv.data) ||
		!(sv.eepromSize == 0 || sv.eepromSize == 512 || sv.eepromSize == 8192) || sv.eepromSize > len(sv.data) ||
		!in(sv.eReadPos, 0, len(sv.eRead)) || len(sv.eBits) > 2+14+64+1 || len(sv.eRead) > 68 {
		return false
	}
	for _, x := range sv.eBits {
		if x > 1 {
			return false
		}
	}
	for _, x := range sv.eRead {
		if x > 1 {
			return false
		}
	}
	if g.gpio != nil && g.gpio.rtc != nil {
		for _, x := range g.gpio.rtc.out {
			if x > 1 {
				return false
			}
		}
	}
	return in(g.line, 0, frameLines-1)
}

func (g *GBA) SaveState() ([]byte, error) {
	s := &sio{save: true, buf: append([]byte(nil), stateMagic...)}
	crc := int(g.romCRC)
	s.Int(&crc)
	g.state(s)
	return s.buf, s.err
}

func (g *GBA) LoadState(b []byte) error {
	if !bytes.HasPrefix(b, stateMagic) {
		return errors.New("gba: not a Game Boy Advance saved state")
	}
	s := &sio{buf: b, pos: len(stateMagic)}
	crc := 0
	s.Int(&crc)
	if s.err != nil || uint32(crc) != g.romCRC {
		return errors.New("gba: this saved state is from a different game")
	}
	// Restore into the machine only if the whole state reads back.
	backup, err := g.SaveState()
	if err != nil {
		return err
	}
	g.state(s)
	if s.err == nil && (s.pos != len(b) || !g.stateOK()) {
		s.err = errState
	}
	if s.err != nil {
		r := &sio{buf: backup, pos: len(stateMagic)}
		r.Int(&crc)
		g.state(r)
		return s.err
	}
	g.apu.out = g.apu.out[:0]
	g.updateWait(int(g.io16(0x204)))
	g.scheduleTimers()
	return nil
}
