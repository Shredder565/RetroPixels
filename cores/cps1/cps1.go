// Package cps1 is Capcom's CP System (CPS-1) arcade board: a 68000 at 10 MHz
// with the CPS-A and CPS-B video chips (three scroll layers, sprites, a
// palette with brightness), and a Z80 sound CPU at 3.58 MHz with a YM2151 FM
// chip and an OKI MSM6295 for samples. The games differ in their CPS-B chip
// and graphics ROM mapping, which are data (config.go), so more games can be
// added with table entries.
//
// 68000 memory:
//
//	000000-3fffff program ROM
//	800000 players 1 (low byte) and 2 (high byte), active low
//	800018/1a/1c/1e system inputs, DIP switches A, B, C
//	800030 coin counters        800100-13f CPS-A    800140-17f CPS-B
//	800180 sound command        800188 sound fade command
//	900000-92ffff graphics RAM  ff0000-ffffff work RAM
//
// The 68000 gets interrupt 2 at the start of each vblank.
package cps1

import (
	"fmt"

	"retropixels/cores/arcade"
	"retropixels/cores/m68k"
	"retropixels/cores/msm6295"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// Timing: an 8 MHz pixel clock, 512 pixels a line, 262 lines.
const (
	lines     = 262 // 240 drawn (16 of them blank borders), 22 of vblank
	cpuHz     = 10000000
	pixelHz   = 8000000
	lineCyc   = cpuHz * 512 / pixelHz // 68000 cycles a line (640)
	z80Hz     = 3579545
	okiHz     = 1000000
	okiDiv    = 132 // pin 7 high
	frameRate = float64(pixelHz) / (512 * lines)
)

type board struct {
	game *arcade.Game
	cfg  *Config
	dips map[string]int
	pads []arcade.Pad

	rom  []byte // 68000 program
	zrom []byte // Z80 program (64 KB: 32 KB fixed, two 16 KB banks)
	wram [0x10000]uint8
	zram [0x800]uint8
	vid  *video

	cpu *m68k.CPU
	z80 *z80.CPU
	ym  *ym2151.YM2151
	oki *msm6295.Chip
	out *arcade.Resampler

	latch, fade uint8
	zbank       int
	zcyc        float64 // Z80 cycles run this frame
	ymClk       int     // Z80 cycles towards the next YM2151 sample
	okiAcc      int     // time towards the next MSM6295 sample, in 1/(okiHz*z80Hz) s
	okiOut      float64 // the MSM6295's last sample
	frame       int64
}

type mainBus struct{ b *board }
type soundBus struct{ b *board }

// z80Line: Z80 cycles per line.
var z80Line = float64(z80Hz) / (frameRate * lines)

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	cfg := configs[g.ID]
	if cfg == nil {
		return nil, fmt.Errorf("cps1: no board setup for %s", g.ID)
	}
	b := &board{game: g, cfg: cfg, dips: opts.Dips}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range g.Dips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	rate := opts.SampleRate
	if rate == 0 {
		rate = 48000
	}
	b.rom = loadProgram(g, files)
	b.zrom = arcade.Region(g, files, "audio", 0x10000)
	gfx := loadGfx(g, files)
	b.vid = newVideo(cfg, gfx)
	b.oki = msm6295.New(arcade.Region(g, files, "oki", 0x40000))
	b.ym = ym2151.New(z80Hz)
	b.ym.OnIRQ = func(on bool) { b.z80.IRQ = on }
	b.out = arcade.NewResampler(float64(z80Hz)/64, float64(rate))
	b.cpu = m68k.New(mainBus{b})
	b.z80 = z80.New(soundBus{b})
	b.Reset()
	return b, nil
}

// loadProgram builds the 68000's ROM: "cpu_even"/"cpu_odd" files are byte
// wide (the high and low bytes of each word) and fill the space from their
// Offset (a byte address) together; "cpu_word" files are 16 bits wide with
// their bytes swapped.
func loadProgram(g *arcade.Game, files arcade.Files) []byte {
	size := 0
	for _, f := range g.Files {
		switch f.Role {
		case "cpu_even", "cpu_odd":
			size = max(size, f.Offset+f.Size*2)
		case "cpu_word":
			size = max(size, f.Offset+f.Size)
		}
	}
	rom := make([]byte, size)
	for i := range rom {
		rom[i] = 0xff
	}
	for i := range g.Files {
		f := &g.Files[i]
		d := arcade.FindFile(f, files)
		if d == nil {
			continue
		}
		switch f.Role {
		case "cpu_even", "cpu_odd":
			o := f.Offset
			if f.Role == "cpu_odd" {
				o++
			}
			for k, v := range d {
				rom[o+k*2] = v
			}
		case "cpu_word":
			for k := 0; k+1 < len(d); k += 2 {
				rom[f.Offset+k], rom[f.Offset+k+1] = d[k+1], d[k]
			}
		}
	}
	return rom
}

// loadGfx builds the graphics ROM: "gfx" files are 16 bits wide, four side
// by side make 64-bit rows; Lane is the file's place (0-3) in the row and
// Offset the row group's start.
func loadGfx(g *arcade.Game, files arcade.Files) []byte {
	size := 0
	for _, f := range g.Files {
		if f.Role == "gfx" {
			size = max(size, f.Offset+f.Size*4)
		}
	}
	out := make([]byte, size)
	for i := range g.Files {
		f := &g.Files[i]
		if f.Role != "gfx" {
			continue
		}
		d := arcade.FindFile(f, files)
		for k := 0; k+1 < len(d); k += 2 {
			at := f.Offset + k*4 + f.Lane*2
			out[at], out[at+1] = d[k], d[k+1]
		}
	}
	return out
}

func (b *board) Reset() {
	b.wram = [0x10000]uint8{}
	b.zram = [0x800]uint8{}
	b.vid.reset()
	b.latch, b.fade = 0, 0
	b.zbank = 0
	b.zcyc, b.ymClk, b.okiAcc, b.okiOut = 0, 0, 0, 0
	b.frame = 0
	b.ym.Reset()
	b.oki.Reset()
	b.cpu.SetIRQ(0)
	b.cpu.Cycles = 0
	b.cpu.Reset()
	b.cpu.Cycles = 0
	b.z80.Reset()
	b.z80.IRQ = false
	b.out.Clear()
}

func (b *board) SetInput(pads []arcade.Pad) { b.pads = pads }

func (b *board) pad(n int) arcade.Pad {
	if n < len(b.pads) {
		return b.pads[n]
	}
	return nil
}

// player: one player's controls, active low (bit 0 right, 1 left, 2 down,
// 3 up, 4-6 buttons 1-3).
func (b *board) player(n int) int {
	p := b.pad(n)
	v := 0
	for i, k := range []string{"Right", "Left", "Down", "Up", "Button 1", "Button 2", "Button 3"} {
		if p[k] {
			v |= 1 << i
		}
	}
	return ^v & 0xff
}

// system: coins, starts and service (active low; bit 6 is the test switch).
func (b *board) system() int {
	p1, p2 := b.pad(0), b.pad(1)
	v := 0
	if p1["Coin"] {
		v |= 0x01
	}
	if p1["Coin 2"] || p2["Coin"] {
		v |= 0x02
	}
	if p1["Service"] {
		v |= 0x04
	}
	if p1["Start (1P)"] {
		v |= 0x10
	}
	if p1["Start (2P)"] || p2["Start (1P)"] {
		v |= 0x20
	}
	return ^v & 0xff
}

// dsw: DIP switch bank 0-2 from the settings (each setting's value is its
// bits as the switches read them; unused switches read 1).
func (b *board) dsw(bank int) int {
	v := 0xff
	for _, d := range b.game.Dips {
		m, ok := dipMasks[d.ID]
		if !ok || m.bank != bank {
			continue
		}
		v = v&^m.mask | b.dips[d.ID]&m.mask
	}
	return v
}

func (b *board) ioRead(a uint32) uint16 {
	switch {
	case a < 0x800008:
		return uint16(b.player(1)<<8 | b.player(0))
	case a >= 0x800018 && a < 0x800020:
		var v int
		switch (a - 0x800018) >> 1 {
		case 0:
			v = b.system()
		default:
			v = b.dsw(int((a-0x800018)>>1) - 1)
		}
		return uint16(v<<8 | v)
	case a >= 0x800140 && a < 0x800180:
		return b.vid.readB(int(a - 0x800140))
	}
	return 0xffff
}

func (m mainBus) Read16(a uint32) uint16 {
	b := m.b
	switch {
	case int(a) < len(b.rom):
		return uint16(b.rom[a])<<8 | uint16(b.rom[a+1])
	case a >= 0xff0000:
		a &= 0xffff
		return uint16(b.wram[a])<<8 | uint16(b.wram[a+1])
	case a >= 0x900000 && a < 0x930000:
		a -= 0x900000
		return uint16(b.vid.gfxram[a])<<8 | uint16(b.vid.gfxram[a+1])
	case a >= 0x800000 && a < 0x800200:
		return b.ioRead(a)
	}
	return 0xffff
}

func (m mainBus) Read8(a uint32) uint8 {
	b := m.b
	switch {
	case int(a) < len(b.rom):
		return b.rom[a]
	case a >= 0xff0000:
		return b.wram[a&0xffff]
	case a >= 0x900000 && a < 0x930000:
		return b.vid.gfxram[a-0x900000]
	}
	v := m.Read16(a &^ 1)
	if a&1 == 0 {
		return uint8(v >> 8)
	}
	return uint8(v)
}

func (m mainBus) Write16(a uint32, v uint16) { m.b.write(a, v, 0xffff) }

func (m mainBus) Write8(a uint32, v uint8) {
	if a&1 == 0 {
		m.b.write(a&^1, uint16(v)<<8, 0xff00)
	} else {
		m.b.write(a&^1, uint16(v), 0x00ff)
	}
}

// write is a word write; mask says which bytes are written.
func (b *board) write(a uint32, v, mask uint16) {
	put := func(mem []uint8, o uint32) {
		if mask&0xff00 != 0 {
			mem[o] = uint8(v >> 8)
		}
		if mask&0xff != 0 {
			mem[o+1] = uint8(v)
		}
	}
	switch {
	case a >= 0xff0000:
		put(b.wram[:], a&0xffff)
	case a >= 0x900000 && a < 0x930000:
		put(b.vid.gfxram[:], a-0x900000)
	case a >= 0x800100 && a < 0x800140:
		b.vid.writeA(int(a-0x800100)>>1, v, mask)
	case a >= 0x800140 && a < 0x800180:
		b.vid.writeB(int(a-0x800140), v, mask)
	case a >= 0x800180 && a < 0x800188:
		if mask&0xff != 0 {
			b.syncZ80()
			b.latch = uint8(v)
		}
	case a >= 0x800188 && a < 0x800190:
		if mask&0xff != 0 {
			b.syncZ80()
			b.fade = uint8(v)
		}
	}
}

// Ack: the vblank interrupt is cleared when the 68000 takes it.
func (m mainBus) Ack(level int) int {
	m.b.cpu.SetIRQ(0)
	return -1
}

// RunFrame runs one frame, which here ends where vblank starts: the
// picture is drawn, the sprite list copied and the 68000 interrupted (the
// first vblank comes a whole frame after power on).
func (b *board) RunFrame() {
	for line := 0; line < lines; line++ {
		b.cpu.Run(int64((line + 1) * lineCyc))
		b.runZ80To(float64(line+1) * z80Line)
	}
	b.cpu.Cycles -= lines * lineCyc
	b.zcyc -= lines * z80Line
	b.vid.vblank()
	b.cpu.SetIRQ(2)
	b.frame++
}

func (b *board) Screen() []uint32     { return b.vid.screen }
func (b *board) TakeAudio() []float32 { return b.out.Take() }

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "cps1", Title: "Capcom CP System", Width: ScreenW, Height: ScreenH, FPS: frameRate,
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Games:   games(), New: newBoard,
	})
}
