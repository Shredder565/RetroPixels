// Package psikyo is Psikyo's PS5V2 arcade board (Dragon Blaze): a Hitachi
// SH-2 at 28.6 MHz, a Yamaha YMF278B (OPL4) for FM and sampled sound, a
// sprite-based video chip whose background layers are tilemaps kept in
// sprite RAM, and a 93C56 serial EEPROM for settings and high scores.
//
// SH-2 memory:
//
//	00000000-000fffff program ROM (mirrored at 05000000)
//	03000000          players 1 and 2 and the coin/service bits, active low
//	03000004          read: region jumper (bits 0-1), EEPROM data out (bit 4)
//	                  write: EEPROM chip select (bit 7), clock (6), data (5)
//	03100000-07       YMF278B
//	04000000-0400ffff sprite RAM (sprites, sprite list, tilemaps)
//	04040000-04044fff palette RAM
//	04050000-040501ff zoom table
//	0405ffdc          writing acknowledges the vblank interrupt
//	0405ffe0-0405ffff video registers
//	06000000-060fffff work RAM
//
// The vblank interrupt is IRL 4 (autovector 66).
package psikyo

import (
	"encoding/binary"

	"retropixels/cores/arcade"
	"retropixels/cores/sh2"
	"retropixels/cores/ymf278b"
)

// Timing: a 7.86 MHz pixel clock, 512 pixels a line, 256 lines (224 shown),
// 60 frames a second.
const (
	cpuHz     = 28636350
	lines     = 256
	visLines  = 224
	frameRate = 60.0
	frameCyc  = cpuHz / 60           // CPU cycles a frame
	ymfRate   = float64(cpuHz) / 768 // YMF278B samples a second
)

type board struct {
	game *arcade.Game
	dips map[string]int
	pads []arcade.Pad

	rom  []byte // program, 1 MB
	ram  []byte // work RAM, 1 MB
	vid  *video
	ee   eeprom
	ee0  [256]uint8 // the EEPROM's contents at power on
	cpu  *sh2.CPU
	ymf  *ymf278b.Chip
	out  *arcade.Resampler
	irq  bool  // vblank interrupt pending
	ymfT int64 // CPU cycle of the next YMF278B sample
	base int64 // CPU cycles run before this frame
}

type bus struct{ b *board }

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{game: g, dips: opts.Dips}
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
	b.ram = make([]byte, 0x100000)
	b.vid = newVideo(loadGfx(g, files))
	b.ymf = ymf278b.New(arcade.Region(g, files, "pcm", 0x200000))
	b.out = arcade.NewResampler(ymfRate, float64(rate))
	b.ee0 = defaultEEPROM(b.rom)
	if d := eepromFile(g, files); d != nil {
		// MAME's dumps keep the 16-bit words byte-swapped
		for i := 0; i+1 < len(d) && i+1 < 256; i += 2 {
			b.ee0[i], b.ee0[i+1] = d[i+1], d[i]
		}
	}
	if len(opts.SRAM) == 256 {
		copy(b.ee0[:], opts.SRAM)
	}
	b.cpu = sh2.New(bus{b})
	b.cpu.Map(0x00000000, b.rom, false)
	b.cpu.Map(0x05000000, b.rom, false)
	b.cpu.Map(0x06000000, b.ram, true)
	b.cpu.Map(0x04000000, b.vid.spr[:], true)
	b.cpu.Map(0x04040000, b.vid.pal[:], true)
	b.ee.mem = b.ee0
	b.Reset()
	return b, nil
}

func eepromFile(g *arcade.Game, files arcade.Files) []byte {
	for i := range g.Files {
		if g.Files[i].Role == "eeprom" {
			return arcade.FindFile(&g.Files[i], files)
		}
	}
	return nil
}

// loadProgram: the "cpu" files are 16 bits wide with their bytes swapped;
// Lane 0 is the high half of each 32-bit word, Lane 1 the low half.
func loadProgram(g *arcade.Game, files arcade.Files) []byte {
	rom := make([]byte, 0x100000)
	for i := range rom {
		rom[i] = 0xff
	}
	for i := range g.Files {
		f := &g.Files[i]
		if f.Role != "cpu" {
			continue
		}
		d := arcade.FindFile(f, files)
		for k := 0; k+1 < len(d) && k*2+3 < len(rom); k += 2 {
			at := k*2 + f.Lane*2
			rom[at], rom[at+1] = d[k+1], d[k]
		}
	}
	return rom
}

// loadGfx: each "gfx" file pair fills 4 MB from its Offset, Lane 0 the
// first two bytes of every 4, Lane 1 the other two. The result starts at the
// video chip's address gfxBase.
func loadGfx(g *arcade.Game, files arcade.Files) []byte {
	size := 0
	for _, f := range g.Files {
		if f.Role == "gfx" {
			size = max(size, f.Offset+f.Size*2-gfxBase)
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
			at := f.Offset - gfxBase + k*2 + f.Lane*2
			out[at], out[at+1] = d[k], d[k+1]
		}
	}
	return out
}

// defaultEEPROM builds the EEPROM a board without one starts with: blank,
// but for the check word the program looks for at 0xec, the low 16 bits of
// the sum of the program's 32-bit words (up to the end of the code the
// program header gives). The game sets up its settings itself.
func defaultEEPROM(rom []byte) [256]uint8 {
	var e [256]uint8
	for i := range e {
		e[i] = 0xff
	}
	end := int(binary.BigEndian.Uint32(rom[0x2e154:]) + binary.BigEndian.Uint32(rom[0x2e15c:]))
	if end > len(rom) {
		end = len(rom)
	}
	var sum uint32
	for i := 0; i < end && i+4 <= len(rom); i += 4 {
		sum += binary.BigEndian.Uint32(rom[i:])
	}
	e[0xec], e[0xed] = uint8(sum>>8), uint8(sum)
	return e
}

func (b *board) Reset() {
	clear(b.ram)
	b.vid.reset()
	b.ee.reset()
	b.ymf.Reset()
	b.irq = false
	b.ymfT = 0
	b.base = 0
	b.cpu.SetIRL(0)
	b.cpu.Cycles = 0
	b.cpu.Reset()
	b.out.Clear()
}

func (b *board) SetInput(pads []arcade.Pad) { b.pads = pads }

func (b *board) pad(n int) arcade.Pad {
	if n < len(b.pads) {
		return b.pads[n]
	}
	return nil
}

// player: one player's byte, active low (bit 7 up, 6 down, 5 right, 4 left,
// 3-1 buttons 1-3, 0 start).
func (b *board) player(n int) uint32 {
	p := b.pad(n)
	v := uint32(0)
	for i, k := range []string{"Start (1P)", "Button 3", "Button 2", "Button 1", "Left", "Right", "Down", "Up"} {
		if p[k] {
			v |= 1 << i
		}
	}
	if n == 1 && b.pad(0)["Start (2P)"] {
		v |= 1
	}
	return ^v & 0xff
}

// inputs: the long at 03000000.
func (b *board) inputs() uint32 {
	p1, p2 := b.pad(0), b.pad(1)
	v := uint32(0)
	if p1["Coin"] {
		v |= 0x01
	}
	if p1["Coin 2"] || p2["Coin"] {
		v |= 0x02
	}
	if p1["Service"] {
		v |= 0x10
	}
	if b.dips["test"] != 0 {
		v |= 0x20
	}
	if b.dips["debug"] != 0 {
		v |= 0x40
	}
	return b.player(0)<<24 | b.player(1)<<16 | 0xff00 | (^v & 0xff)
}

// jp4: the long at 03000004.
func (b *board) jp4() uint32 {
	v := uint32(b.dips["region"]&3) << 24
	if b.ee.dataOut(b.now()) {
		v |= 0x10 << 24
	}
	return v
}

func (m bus) Read32(a uint32) uint32 {
	b := m.b
	switch {
	case a >= 0x03000000 && a < 0x03000004:
		return b.inputs()
	case a >= 0x03000004 && a < 0x03000008:
		return b.jp4()
	case a >= 0x03100000 && a < 0x03100008:
		return uint32(m.Read16(a))<<16 | uint32(m.Read16(a+2))
	case a >= 0x04050000 && a < 0x04050200:
		return binary.BigEndian.Uint32(b.vid.zoom[a&0x1ff:])
	case a >= 0x0405ffe0 && a < 0x04060000:
		return binary.BigEndian.Uint32(b.vid.regs[a&0x1f:])
	}
	return 0
}

func (m bus) Read16(a uint32) uint16 {
	return uint16(m.Read8(a))<<8 | uint16(m.Read8(a+1))
}

func (m bus) Read8(a uint32) uint8 {
	b := m.b
	switch {
	case a >= 0x03100000 && a < 0x03100008:
		return b.ymf.Read(int(a & 7))
	case a >= 0x04050000 && a < 0x04050200:
		return b.vid.zoom[a&0x1ff]
	}
	v := m.Read32(a &^ 3)
	return uint8(v >> (24 - 8*(a&3)))
}

func (m bus) Write32(a uint32, v uint32) {
	for i := uint32(0); i < 4; i++ {
		m.Write8(a+i, uint8(v>>(24-8*i)))
	}
}

func (m bus) Write16(a uint32, v uint16) {
	m.Write8(a, uint8(v>>8))
	m.Write8(a+1, uint8(v))
}

func (m bus) Write8(a uint32, v uint8) {
	b := m.b
	switch {
	case a == 0x03000004:
		b.ee.write(v&0x80 != 0, v&0x40 != 0, v&0x20 != 0, b.now())
	case a >= 0x03100000 && a < 0x03100008:
		b.ymf.Write(int(a&7), v)
	case a >= 0x04050000 && a < 0x04050200:
		b.vid.zoom[a&0x1ff] = v
	case a >= 0x0405ffdc && a < 0x0405ffe0:
		b.irq = false
		b.cpu.SetIRL(0)
	case a >= 0x0405ffe0 && a < 0x04060000:
		b.vid.regs[a&0x1f] = v
	}
}

// RunFrame runs one frame: 224 shown lines, then vblank, when the picture
// is drawn and the interrupt raised.
func (b *board) RunFrame() {
	for line := 0; line < lines; line++ {
		if line == visLines {
			b.vid.render()
			b.vid.latch()
			b.irq = true
			b.cpu.SetIRL(4)
		}
		t := int64(frameCyc * (line + 1) / lines)
		b.cpu.Run(t)
		b.runSound(t)
	}
	b.cpu.Cycles -= frameCyc
	b.ymfT -= frameCyc
	b.base += frameCyc
}

// now is the time in CPU cycles since power on.
func (b *board) now() int64 { return b.base + b.cpu.Cycles }

// runSound makes the YMF278B's samples up to CPU cycle t (the chip runs
// off the CPU's clock, a sample every 768 cycles).
func (b *board) runSound(t int64) {
	for b.ymfT <= t {
		b.ymfT += 768
		l, r := b.ymf.Sample()
		b.out.Add(float64(l+r) * mixLevel)
	}
}

// mixLevel scales the chip's left+right sum to the output (±1): a voice
// at full volume plays at about half scale.
const mixLevel = 0.4965 / 65536

func (b *board) Screen() []uint32     { return b.vid.screen }
func (b *board) TakeAudio() []float32 { return b.out.Take() }

// SRAM is the EEPROM.
func (b *board) SRAM() []byte { return append([]byte(nil), b.ee.mem[:]...) }

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "psikyo", Title: "Psikyo PS5V2", Width: ScreenW, Height: ScreenH, FPS: frameRate, Rotate: 270,
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Games:   games(), New: newBoard,
	})
}
