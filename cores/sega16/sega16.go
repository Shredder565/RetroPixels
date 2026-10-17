// Package sega16 runs Sega System 16B boards: a 68000 behind a 315-5195
// memory mapper, the 315-5197 tile layers, the 315-5196 sprites, and a Z80
// sound board with a YM2151 and a uPD7759. Each game's ROM layout, inputs
// and DIP switches are data (games.go), so other System 16B games can be
// added next to Altered Beast.
//
// Everything here was worked out from the hardware's behaviour: the games'
// own code (disassembled) and MAME run as a black box (its pictures, and
// the RAM the game wrote, rendered here and compared pixel by pixel).
package sega16

import (
	"retropixels/cores/arcade"
	"retropixels/cores/m68k"
)

const (
	cpuHz    = 10000000
	pixHz    = 6293700 // 25.1748 MHz / 4
	lineDots = 400
	lines    = 262
	vblank   = 224 // first line of the vertical blank (IRQ 4)
)

var (
	fps         = float64(pixHz) / (lineDots * lines)
	cpuPerLine  = float64(cpuHz) * lineDots / pixHz
	z80PerLine  = float64(z80Hz) * lineDots / pixHz
	cpuPerFrame = int64(cpuPerLine * lines)
)

type board struct {
	game  *gameDef
	dips  map[string]int
	pads  []arcade.Pad
	cpu   *m68k.CPU
	m     mapper
	v     *video
	snd   *sound
	rom   []byte // 68000 program
	ram   [0x4000]uint8
	misc  uint8 // I/O output port (C40001)
	frame int
}

type mainBus struct{ b *board }

func newBoard(gd *gameDef, g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{game: gd, dips: opts.Dips}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range gd.dips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	rate := opts.SampleRate
	if rate == 0 {
		rate = 48000
	}
	hi := arcade.Region(g, files, "cpu_hi", 0)
	lo := arcade.Region(g, files, "cpu_lo", 0)
	b.rom = make([]byte, len(hi)*2)
	for i := range hi {
		b.rom[i*2], b.rom[i*2+1] = hi[i], lo[i]
	}
	var tiles [3][]byte
	for p := range tiles {
		tiles[p] = arcade.Region(g, files, "tiles"+string(rune('0'+p)), 0)
	}
	shi := arcade.Region(g, files, "spr_hi", 0)
	slo := arcade.Region(g, files, "spr_lo", 0)
	words := make([]uint16, len(shi)+0x10000)
	for i := range shi {
		words[i] = uint16(shi[i])<<8 | uint16(slo[i])
	}
	for i := len(shi); i < len(words); i++ {
		words[i] = 0xffff
	}
	var banks [16]int
	for i := range banks {
		banks[i] = -1
		if n := gd.spriteBank(i); n >= 0 && n*0x10000 < len(shi) {
			banks[i] = n * 0x10000
		}
	}
	b.v = newVideo(tiles, words, banks)
	b.snd = newSound(arcade.Region(g, files, "sound", 0x8000), arcade.Region(g, files, "samples", 0), gd.soundBank, rate)
	b.cpu = m68k.New(mainBus{b})
	b.Reset()
	return b, nil
}

func (b *board) Reset() {
	b.ram = [0x4000]uint8{}
	b.m.reset()
	b.v.reset()
	b.snd.reset()
	b.misc = 0
	b.frame = 0
	b.cpu.SetIRQ(0)
	b.cpu.Cycles = 0
	b.cpu.Reset()
	b.cpu.Cycles = 0
}

func (b *board) SetInput(pads []arcade.Pad) { b.pads = pads }

func (b *board) pad(p int) arcade.Pad {
	if p < len(b.pads) {
		return b.pads[p]
	}
	return nil
}

// ---- 68000 side ----

func (m mainBus) Read16(a uint32) uint16 {
	b := m.b
	r, off := b.m.region(a)
	switch r {
	case 0, 1:
		if r == 1 {
			off += 0x80000
		}
		if int(off)+1 < len(b.rom) {
			return uint16(b.rom[off])<<8 | uint16(b.rom[off+1])
		}
		return 0xffff
	case 3:
		off &= 0x3fff
		return uint16(b.ram[off])<<8 | uint16(b.ram[off+1])
	case 4:
		return b.v.sprRAM[off>>1&0x3ff]
	case 5:
		if off < 0x10000 {
			return b.v.tileRAM[off>>1]
		}
		return b.v.textRAM[off>>1&0x7ff]
	case 6:
		return b.v.palRAM[off>>1&0x7ff]
	case 7:
		return 0xff00 | uint16(b.ioRead(off))
	case -1:
		return 0xff00 | uint16(b.m.read(a))
	}
	return 0xffff
}

func (m mainBus) Read8(a uint32) uint8 {
	w := m.Read16(a &^ 1)
	if a&1 != 0 {
		return uint8(w)
	}
	return uint8(w >> 8)
}

func (m mainBus) Write16(a uint32, v uint16) {
	b := m.b
	r, off := b.m.region(a)
	switch r {
	case 3:
		off &= 0x3fff
		b.ram[off], b.ram[off+1] = uint8(v>>8), uint8(v)
	case 4:
		b.v.sprRAM[off>>1&0x3ff] = v
	case 5:
		if off < 0x10000 {
			b.v.tileRAM[off>>1] = v
		} else {
			b.v.textRAM[off>>1&0x7ff] = v
		}
	case 6:
		i := int(off >> 1 & 0x7ff)
		b.v.palRAM[i] = v
		b.v.setColor(i)
	default:
		m.Write8(a|1, uint8(v))
	}
}

func (m mainBus) Write8(a uint32, v uint8) {
	b := m.b
	r, off := b.m.region(a)
	switch r {
	case 2:
		// ROM board latch: the tile banks
		if a&1 != 0 {
			b.v.bank[off>>1&1] = int(v & 7)
		}
	case 3:
		b.ram[off&0x3fff] = v
	case 4, 5, 6:
		// byte writes to video RAM change one half of the word
		w := mainBus{b}.Read16(a &^ 1)
		if a&1 != 0 {
			w = w&0xff00 | uint16(v)
		} else {
			w = w&0x00ff | uint16(v)<<8
		}
		m.Write16(a&^1, w)
	case 7:
		if a&1 != 0 {
			b.ioWrite(off, v)
		}
	case -1:
		if a&1 != 0 && b.m.write(a, v) {
			b.syncSound()
			b.snd.command(v)
		}
	}
}

func (m mainBus) Ack(level int) int {
	m.b.cpu.SetIRQ(0)
	return -1
}

// ioRead: the I/O chip. 0x1000: inputs (service, player 1, -, player 2);
// 0x2000: DIP switches (B at +1, A at +3).
func (b *board) ioRead(off uint32) uint8 {
	switch off & 0x3000 {
	case 0x0000:
		return b.misc
	case 0x1000:
		return b.game.input(b, int(off>>1&3))
	case 0x2000:
		if off&2 != 0 {
			return b.game.dsw(b.dips, 0)
		}
		return b.game.dsw(b.dips, 1)
	}
	return 0xff
}

// ioWrite: 0x0000 is the output port: bit 5 shows the picture.
func (b *board) ioWrite(off uint32, v uint8) {
	if off&0x3000 == 0 {
		b.misc = v
		b.v.enable = v&0x20 != 0
	}
}

// ---- timing ----

func (b *board) syncSound() {
	c := min(b.cpu.Cycles, cpuPerFrame)
	b.snd.runTo(float64(c) / cpuPerLine * z80PerLine)
}

func (b *board) RunFrame() {
	for line := 0; line < lines; line++ {
		if line == vblank {
			b.v.render()
			b.cpu.SetIRQ(4)
		}
		b.cpu.Run(int64(float64(line+1) * cpuPerLine))
		b.snd.runTo(float64(line+1) * z80PerLine)
	}
	b.cpu.Cycles -= cpuPerFrame
	b.snd.zcyc -= float64(lines) * z80PerLine
	b.frame++
}

func (b *board) Screen() []uint32     { return b.v.screen }
func (b *board) TakeAudio() []float32 { return b.snd.out.Take() }

// ---- save states ----

const stateMagic = "S16B1"

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(stateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.m.regs[:])
	w.U8(b.misc)
	w.Int(b.frame)
	b.v.appendState(w)
	b.snd.appendState(w)
	return w.B, nil
}

func (b *board) LoadState(data []byte) error {
	if len(data) < len(stateMagic) || string(data[:len(stateMagic)]) != stateMagic {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: data[len(stateMagic):]}
	var err error
	if r.B, err = b.cpu.LoadState(r.B); err != nil {
		return err
	}
	r.Bytes(b.ram[:])
	r.Bytes(b.m.regs[:])
	b.misc = r.U8()
	b.frame = r.Int()
	b.v.loadState(r)
	if err := b.snd.loadState(r); err != nil {
		return err
	}
	if err := r.Done(); err != nil {
		return err
	}
	b.m.rebuild()
	b.v.enable = b.misc&0x20 != 0
	b.snd.out.Clear()
	b.v.render()
	return nil
}
