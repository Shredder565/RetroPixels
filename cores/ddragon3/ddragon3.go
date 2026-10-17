// Package ddragon3 is Technos' Double Dragon 3: The Rosetta Stone board: a
// 68000 at 10 MHz with two scrolling 16x16 tile layers, 256 sprites and a
// 768-colour palette, and a Z80 sound CPU at 3.58 MHz with a YM2151 FM chip
// and an OKI MSM6295 for samples. Up to three players.
//
// 68000 memory:
//
//	000000-07ffff program ROM    080000-080fff foreground layer
//	082000-0827ff background layer
//	0c0000-0c000f scroll and video registers
//	100000 read: players 1 (low byte) and 2; write: layer priority
//	100002 read: coins and service; write (low byte): sound command
//	100004 read: DIP switches; write: vblank interrupt done
//	100006 read: player 3; write: timer interrupt done
//	140000-1405ff palette        180000-180fff sprites
//	1c0000-1c3fff work RAM
//
// The picture is 320x240 of a 448x272 frame at 7 MHz (57.44 frames a
// second). The 68000 gets interrupt 6 when vblank starts (line 248) and
// interrupt 5 every 16 lines; it says it has taken each by writing to
// 100004 or 100006. A frame here runs from the start of one vblank to the
// next, so the picture is drawn at the end of RunFrame; the board powers on
// at the start of a vblank, as MAME's does (the game's boot then lines up
// with MAME's cycle for cycle).
package ddragon3

import (
	"retropixels/cores/arcade"
	"retropixels/cores/m68k"
	"retropixels/cores/msm6295"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// Timing.
const (
	lines      = 272
	vblankLine = 248 // interrupt 6, and where a frame ends
	firstLine  = 8   // the first line shown
	cpuHz      = 10000000
	pixelHz    = 7000000
	lineCyc    = cpuHz * 448 / pixelHz // 68000 cycles a line (640)
	z80Hz      = 3579545
	okiHz      = 1056000
	okiDiv     = 132 // pin 7 high
	frameRate  = float64(pixelHz) / (448 * lines)
	z80Line    = float64(z80Hz) * 448 / pixelHz // Z80 cycles a line
)

type board struct {
	game *arcade.Game
	dips map[string]int
	pads []arcade.Pad

	rom    []byte        // 68000 program
	zrom   []byte        // Z80 program
	okiRom []byte        // both 256 KB sample banks
	okiWin [0x40000]byte // the bank the MSM6295 sees
	wram   [0x4000]uint8
	zram   [0x800]uint8
	vid    *video

	cpu *m68k.CPU
	z80 *z80.CPU
	ym  *ym2151.YM2151
	oki *msm6295.Chip
	out *arcade.Resampler

	line       int  // the next line to run
	irq5, irq6 bool // interrupts waiting to be acknowledged
	latch      uint8
	okiBank    int
	zcyc       float64 // Z80 cycles run this frame
	ymClk      int     // Z80 cycles towards the next YM2151 sample
	okiAcc     int     // time towards the next MSM6295 sample, in 1/(okiHz*z80Hz) s
	okiOut     float64 // the MSM6295's last sample
	frame      int64
}

type mainBus struct{ b *board }
type soundBus struct{ b *board }

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
	b.zrom = arcade.Region(g, files, "audio", 0x10000)
	b.okiRom = arcade.Region(g, files, "oki", 0x80000)
	b.vid = newVideo(planes(g, files, "tiles"), planes(g, files, "sprites"))
	b.oki = msm6295.New(b.okiWin[:])
	b.ym = ym2151.New(z80Hz)
	b.out = arcade.NewResampler(float64(z80Hz)/64, float64(rate))
	b.cpu = m68k.New(mainBus{b})
	b.z80 = z80.New(soundBus{b})
	b.Reset()
	return b, nil
}

// loadProgram builds the 68000's ROM from its even- and odd-byte chips (the
// even chip is half the size: the top half's even bytes read 0).
func loadProgram(g *arcade.Game, files arcade.Files) []byte {
	rom := make([]byte, 0x80000)
	for i := range g.Files {
		f := &g.Files[i]
		o := 0
		switch f.Role {
		case "cpu_even":
		case "cpu_odd":
			o = 1
		default:
			continue
		}
		for k, v := range arcade.FindFile(f, files) {
			if o+k*2 < len(rom) {
				rom[o+k*2] = v
			}
		}
	}
	return rom
}

// planes gathers a graphics role's chips into four bit planes (a file's
// Lane is its plane, its Offset where it goes in the plane).
func planes(g *arcade.Game, files arcade.Files, role string) [4][]byte {
	var p [4][]byte
	size := 0
	for _, f := range g.Files {
		if f.Role == role {
			size = max(size, f.Offset+f.Size)
		}
	}
	for i := range p {
		p[i] = make([]byte, size)
	}
	for i := range g.Files {
		f := &g.Files[i]
		if f.Role == role {
			copy(p[f.Lane][f.Offset:], arcade.FindFile(f, files))
		}
	}
	return p
}

func (b *board) Reset() {
	b.wram = [0x4000]uint8{}
	b.zram = [0x800]uint8{}
	b.vid.reset()
	b.line = vblankLine
	b.irq5, b.irq6 = false, false
	b.latch = 0
	b.setOkiBank(0)
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

func (b *board) setOkiBank(n int) {
	b.okiBank = n & 1
	copy(b.okiWin[:], b.okiRom[b.okiBank*0x40000:])
}

func (b *board) SetInput(pads []arcade.Pad) { b.pads = pads }

func (b *board) pad(n int) arcade.Pad {
	if n < len(b.pads) {
		return b.pads[n]
	}
	return nil
}

// player: one player's byte, active low: bit 0 right, 1 left, 2 up, 3 down,
// 4-6 buttons 1-3, 7 start. Player 1's "Start (2P)" is player 2's start.
func (b *board) player(n int) int {
	p := b.pad(n)
	v := 0
	for i, k := range []string{"Right", "Left", "Up", "Down", "Button 1", "Button 2", "Button 3", "Start (1P)"} {
		if p[k] {
			v |= 1 << i
		}
	}
	if n == 1 && b.pad(0)["Start (2P)"] {
		v |= 0x80
	}
	return ^v & 0xff
}

// system: coin 1 (bit 0), coin 2 (bit 1, players 2 and 3's coin) and
// service (bit 2), active low.
func (b *board) system() int {
	p1 := b.pad(0)
	v := 0
	if p1["Coin"] {
		v |= 1
	}
	if p1["Coin 2"] || b.pad(1)["Coin"] || b.pad(2)["Coin"] {
		v |= 2
	}
	for n := 0; n < 3; n++ {
		if b.pad(n)["Service"] {
			v |= 4
		}
	}
	return ^v & 0xffff
}

// dsw: the DIP switch word.
func (b *board) dsw() int {
	v := 0xffff
	for _, d := range b.game.Dips {
		if m, ok := dipMasks[d.ID]; ok {
			v = v&^m | b.dips[d.ID]&m
		}
	}
	return v
}

func (b *board) ioRead(a uint32) uint16 {
	switch a & 6 {
	case 0:
		return uint16(b.player(1)<<8 | b.player(0))
	case 2:
		return uint16(b.system())
	case 4:
		return uint16(b.dsw())
	}
	return uint16(0xff00 | b.player(2))
}

func (m mainBus) Read16(a uint32) uint16 {
	b := m.b
	switch {
	case a < 0x80000:
		return uint16(b.rom[a])<<8 | uint16(b.rom[a+1])
	case a >= 0x1c0000 && a < 0x1c4000:
		a -= 0x1c0000
		return uint16(b.wram[a])<<8 | uint16(b.wram[a+1])
	case a >= 0x080000 && a < 0x081000:
		return be16(b.vid.fgram[:], a-0x80000)
	case a >= 0x082000 && a < 0x082800:
		return be16(b.vid.bgram[:], a-0x82000)
	case a >= 0x140000 && a < 0x140600:
		return be16(b.vid.palram[:], a-0x140000)
	case a >= 0x180000 && a < 0x181000:
		return be16(b.vid.spram[:], a-0x180000)
	case a >= 0x100000 && a < 0x100008:
		return b.ioRead(a)
	}
	return 0
}

func be16(m []byte, o uint32) uint16 { return uint16(m[o])<<8 | uint16(m[o+1]) }

func (m mainBus) Read8(a uint32) uint8 {
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
	vd := b.vid
	switch {
	case a >= 0x1c0000 && a < 0x1c4000:
		put(b.wram[:], a-0x1c0000)
	case a >= 0x080000 && a < 0x081000:
		put(vd.fgram[:], a-0x80000)
	case a >= 0x082000 && a < 0x082800:
		put(vd.bgram[:], a-0x82000)
	case a >= 0x140000 && a < 0x140600:
		put(vd.palram[:], a-0x140000)
		vd.updateColour(int(a-0x140000) >> 1)
	case a >= 0x180000 && a < 0x181000:
		put(vd.spram[:], a-0x180000)
	case a >= 0x0c0000 && a < 0x0c0010:
		put(vd.regs[:], a-0xc0000)
	case a >= 0x100000 && a < 0x100008:
		switch a & 6 {
		case 0:
			put(vd.prio[:], 0)
		case 2:
			if mask&0xff != 0 {
				b.syncZ80()
				b.latch = uint8(v)
				b.z80.NMI()
			}
		case 4:
			b.irq6 = false
			b.setIRQ()
		case 6:
			b.irq5 = false
			b.setIRQ()
		}
	}
}

// Ack: the interrupts are autovectored, and the 68000 reads an autovector
// in step with its E clock (a tenth of its own): at least 6 more cycles,
// and the handler starts on a set phase of that clock (both measured
// against MAME, which then runs the game's frames cycle for cycle alike).
// A frame is a whole number of E clocks, so the cycle count from the
// frame's start gives the phase.
func (m mainBus) Ack(level int) int {
	c := m.b.cpu.Cycles + 6
	for ((c+20)%10+10)%10 != 5 { // 20: the cycles until the handler's first instruction
		c++
	}
	m.b.cpu.Cycles = c
	return -1
}

func (b *board) setIRQ() {
	switch {
	case b.irq6:
		b.cpu.SetIRQ(6)
	case b.irq5:
		b.cpu.SetIRQ(5)
	default:
		b.cpu.SetIRQ(0)
	}
}

// RunFrame runs from one vblank start to the next and draws the picture.
func (b *board) RunFrame() {
	n := 0
	for {
		l := b.line
		if l == vblankLine {
			b.irq6 = true
		}
		if l%16 == 0 {
			b.irq5 = true
		}
		b.setIRQ()
		n++
		b.cpu.Run(int64(n * lineCyc))
		b.runZ80To(float64(n) * z80Line)
		b.line = (l + 1) % lines
		if b.line == vblankLine {
			break
		}
	}
	b.cpu.Cycles -= int64(n * lineCyc)
	b.zcyc -= float64(n) * z80Line
	b.vid.render()
	b.vid.vblank()
	b.frame++
}

func (b *board) Screen() []uint32     { return b.vid.screen }
func (b *board) TakeAudio() []float32 { return b.out.Take() }

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "ddragon3", Title: "Technos Double Dragon 3", Width: ScreenW, Height: ScreenH, FPS: frameRate,
		Buttons: dd3Buttons, Players: 3, Characters: []string{"Billy", "Jimmy", "Sonny"},
		Games: games(), New: newBoard,
	})
}
