package dec8

// The Real Ghostbusters board.
//
// Main HD6309 at 3 MHz: 0000-17ff RAM, 1800-1fff characters, 2000-27ff the
// playfield map, 2800-2bff RAM, 2c00-2fff the playfield's row scroll,
// 3000-37ff sprites; reads 3800 / 3801 the players' controls, 3802 nothing,
// 3803 switches 1 (bit 3: the vertical blank), 3820 switches 2, 3830-383f
// the scroll registers, 3840 / 3860 the MCU's answer (high, low); writes
// 3800 the sound command, 3820-3827 the playfield's control, 3830-383f its
// scroll, 3840 the bank and control (bit 0 the MCU's interrupt on, 1 the
// vertical-blank NMI on, 2 copies the sprites on going up, 3 flip, 4-7 the
// ROM bank at 4000-7fff), 3860 / 3861 the question for the MCU (high, low).
// 4000-7fff is one of 16 KB banks, 8000-ffff fixed ROM.
//
// Sound DECO 222 at 1.5 MHz: 0000-05ff RAM, 2000-2001 YM2203 (1.5 MHz),
// 4000-4001 YM3812 (3 MHz), 6000 the command (its write makes an NMI),
// 8000-ffff ROM. The YM3812's timer interrupts it.
//
// Picture: 6 MHz pixel clock, 384 pixels a line, 272 lines; lines 8-247
// are shown and the vertical blank is lines 248-7. The NMI comes at the
// start of the blank; the game sets the scroll and copies the sprites in it,
// and the frame is drawn from what is there when the blank ends.

import (
	"retropixels/cores/arcade"
	"retropixels/cores/cpu6502"
	"retropixels/cores/m6809"
	"retropixels/cores/ym2203"
	"retropixels/cores/ym3812"
)

const (
	pixelHz     = 6000000
	lineClocks  = 384
	lines       = 272
	vblankStart = 248
	vblankEnd   = 8
	fps         = float64(pixelHz) / (lineClocks * lines)
	mainLine    = 192 // main CPU clocks a line (3 MHz)
	soundLine   = 96  // sound CPU and YM2203 clocks a line (1.5 MHz)
	oplLine     = 192 // YM3812 clocks a line (3 MHz)
	soundSteps  = 4   // sound slices (and samples) a line
)

type ghostb struct {
	game *arcade.Game
	dips map[string]int

	rom, snd []byte
	vid      *video

	main  *m6809.CPU
	sound *cpu6502.CPU
	ym    *ym2203.Chip
	opl   *ym3812.Chip

	ram     [0x3800]uint8 // 0000-37ff
	sndRAM  [0x600]uint8
	sprBuf  [0x800]uint8 // the sprite chip's copy of the list
	ctrl    [8]uint8     // 3820-3827
	scroll  [16]uint8    // 3830-383f
	b3840   uint8
	latch   uint8
	mcu     mcu
	vblank  bool
	line    int // the line the next RunFrame starts at
	mainOv  int
	soundOv int
	frame   int64

	pads     []arcade.Pad
	out      *arcade.Resampler
	fmGain   float64
	ssgGain  float64
	oplGain  float64
	onRender func()                     // tests: called when a frame is drawn
	onSample func(fm, ssg, opl float64) // tests: each chip's output
}

type mainBus struct{ b *ghostb }

func (x mainBus) Read(a uint16) uint8     { return x.b.read(a) }
func (x mainBus) Write(a uint16, v uint8) { x.b.write(a, v) }

type soundBus struct{ b *ghostb }

func (x soundBus) Read(a uint16) uint8     { return x.b.sndRead(a) }
func (x soundBus) Write(a uint16, v uint8) { x.b.sndWrite(a, v) }

// ReadOp is an opcode fetch: the DECO 222 has bits 5 and 6 of its opcodes
// swapped.
func (x soundBus) ReadOp(a uint16) uint8 {
	v := x.b.sndRead(a)
	return v&0x9f | v>>1&0x20 | v<<1&0x40
}

func newGhostb(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &ghostb{game: g, dips: opts.Dips, fmGain: 0.049, ssgGain: 0.11, oplGain: 0.087}
	b.rom = arcade.Region(g, files, "cpu", 0x50000)
	b.snd = arcade.Region(g, files, "sound", 0x8000)
	proms := arcade.Region(g, files, "proms", 0x800)
	b.vid = newVideo(arcade.Region(g, files, "chars", 0x8000), arcade.Region(g, files, "tiles", 0x40000),
		arcade.Region(g, files, "sprites", 0x80000), proms[:0x400], proms[0x400:])
	b.ym = ym2203.New()
	b.opl = ym3812.New()
	b.out = arcade.NewResampler(fps*lines*soundSteps, float64(opts.SampleRate))
	b.main = m6809.New(mainBus{b}, m6809.HD6309)
	b.sound = cpu6502.New(soundBus{b}, cpu6502.Options{Decimal: true})
	b.Reset()
	return b, nil
}

func (b *ghostb) Reset() {
	b.ram, b.sndRAM, b.sprBuf = [0x3800]uint8{}, [0x600]uint8{}, [0x800]uint8{}
	b.ctrl, b.scroll = [8]uint8{}, [16]uint8{}
	b.b3840, b.latch = 0, 0
	b.mcu.reset()
	b.vblank = false
	b.line, b.mainOv, b.soundOv, b.frame = vblankEnd, 0, 0, 0
	b.ym.Reset()
	b.opl.Reset()
	b.main.IRQ, b.main.FIRQ = false, false
	b.main.Reset()
	b.sound.NMIPending, b.sound.IRQLine = false, false
	b.sound.Reset()
}

func (b *ghostb) bank() int { return int(b.b3840 >> 4) }

func (b *ghostb) read(a uint16) uint8 {
	switch {
	case a < 0x3800:
		return b.ram[a]
	case a >= 0x8000:
		return b.rom[a]
	case a >= 0x4000:
		return b.rom[0x10000+b.bank()*0x4000+int(a-0x4000)]
	}
	switch {
	case a == 0x3800:
		return b.controls(0)
	case a == 0x3801:
		return b.controls(1)
	case a == 0x3803:
		d := b.dips
		v := uint8(0x97) | uint8(d["demosound"]&1)<<5 | uint8(d["flip"]&1)<<6
		if b.vblank {
			v |= 8
		}
		return v
	case a == 0x3820:
		d := b.dips
		return uint8(d["lives"]&3 | (d["difficulty"]&3)<<2 | (d["scenetime"]&3)<<4 | (d["continue"]&1)<<6 | (d["energy"]&1)<<7)
	case a >= 0x3830 && a < 0x3840:
		return b.scroll[a-0x3830]
	case a == 0x3840:
		return b.mcu.hi
	case a == 0x3860:
		return b.mcu.lo
	}
	return 0xff
}

func (b *ghostb) write(a uint16, v uint8) {
	switch {
	case a < 0x3800:
		b.ram[a] = v
	case a == 0x3800:
		b.latch = v
		b.sound.NMIPending = true
	case a >= 0x3820 && a < 0x3828:
		b.ctrl[a-0x3820] = v
	case a >= 0x3830 && a < 0x3840:
		b.scroll[a-0x3830] = v
	case a == 0x3840:
		if v&4 != 0 && b.b3840&4 == 0 {
			copy(b.sprBuf[:], b.ram[0x3000:0x3800])
		}
		b.b3840 = v
		b.mcu.enable(v&1 != 0)
	case a == 0x3860:
		b.mcu.ask(v, true)
	case a == 0x3861:
		b.mcu.ask(v, false)
	}
}

// controls: active-low joystick (up, down, left, right) and two buttons.
func (b *ghostb) controls(p int) uint8 {
	pd := pad(b.pads, p)
	v := uint8(0xff)
	for i, n := range []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2"} {
		if pd[n] {
			v &^= 1 << i
		}
	}
	return v
}

func pad(p []arcade.Pad, i int) arcade.Pad {
	if i < len(p) && p[i] != nil {
		return p[i]
	}
	return arcade.Pad{}
}

func (b *ghostb) sndRead(a uint16) uint8 {
	switch {
	case a >= 0x8000:
		return b.snd[a-0x8000]
	case a < 0x600:
		return b.sndRAM[a]
	case a >= 0x6000 && a < 0x8000:
		return b.latch
	}
	return 0xff
}

func (b *ghostb) sndWrite(a uint16, v uint8) {
	switch {
	case a < 0x600:
		b.sndRAM[a] = v
	case a >= 0x2000 && a < 0x4000:
		b.ym.Write(int(a&1), v)
	case a >= 0x4000 && a < 0x6000:
		b.opl.Write(int(a&1), v)
	}
}

func (b *ghostb) SetInput(p []arcade.Pad) { b.pads = p }
func (b *ghostb) Screen() []uint32        { return b.vid.screen }
func (b *ghostb) TakeAudio() []float32    { return b.out.Take() }

// RunFrame runs from the end of one vertical blank to the end of the next
// and draws the frame.
func (b *ghostb) RunFrame() {
	b.mcu.coins(pad(b.pads, 0), pad(b.pads, 1))
	for {
		switch b.line {
		case vblankStart:
			b.vblank = true
			if b.b3840&2 != 0 {
				b.main.NMI()
			}
		case vblankEnd:
			b.vblank = false
		}
		b.main.IRQ = b.mcu.irq
		n := b.mainOv
		for n < mainLine {
			n += b.main.Step()
		}
		b.mainOv = n - mainLine
		for s := 0; s < soundSteps; s++ {
			b.sound.IRQLine = b.opl.IRQ() || b.ym.IRQ()
			n := b.soundOv
			for n < soundLine/soundSteps {
				n += b.sound.Step()
			}
			b.soundOv = n - soundLine/soundSteps
			b.ym.Run(soundLine / soundSteps)
			b.opl.Run(oplLine / soundSteps)
			fm, ssg := b.ym.Take()
			opl := b.opl.Take()
			b.out.Add(fm*b.fmGain + ssg*b.ssgGain + opl*b.oplGain)
			if b.onSample != nil {
				b.onSample(fm, ssg, opl)
			}
		}
		if b.line = (b.line + 1) % lines; b.line == vblankEnd {
			break
		}
	}
	b.render()
	b.frame++
}

func (b *ghostb) render() {
	r := frameRegs{flip: b.b3840&8 != 0}
	for i := range r.ctrl {
		r.ctrl[i] = b.ctrl[i*2]
	}
	r.scroll = b.scroll
	b.vid.render(b.ram[0x1800:0x2000], b.ram[0x2000:0x2800], b.ram[0x2c00:0x3000], b.sprBuf[:], &r)
	if b.onRender != nil {
		b.onRender()
	}
}

func (b *ghostb) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.AppendState(w.B)
	w.Bytes(b.sound.SaveState())
	w.B = b.ym.AppendState(w.B)
	w.B = b.opl.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.sndRAM[:])
	w.Bytes(b.sprBuf[:])
	w.Bytes(b.ctrl[:])
	w.Bytes(b.scroll[:])
	w.U8(b.b3840)
	w.U8(b.latch)
	b.mcu.save(w)
	w.Bool(b.vblank)
	w.Int(b.line)
	w.Int(b.mainOv)
	w.Int(b.soundOv)
	w.Int(int(b.frame))
	b.out.AppendState(w)
	return w.B, nil
}

func (b *ghostb) LoadState(s []byte) error {
	rest, err := b.main.LoadState(s)
	if err != nil {
		return err
	}
	if len(rest) < cpu6502.StateSize {
		return arcade.ErrBadState
	}
	if err := b.sound.LoadState(rest[:cpu6502.StateSize]); err != nil {
		return arcade.ErrBadState
	}
	rest = rest[cpu6502.StateSize:]
	if rest, err = b.ym.LoadState(rest); err != nil {
		return err
	}
	if rest, err = b.opl.LoadState(rest); err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.sndRAM[:])
	r.Bytes(b.sprBuf[:])
	r.Bytes(b.ctrl[:])
	r.Bytes(b.scroll[:])
	b.b3840 = r.U8()
	b.latch = r.U8()
	b.mcu.load(r)
	b.vblank = r.Bool()
	b.line = max(0, min(r.Int(), lines-1))
	b.mainOv = max(0, min(r.Int(), 64))
	b.soundOv = max(0, min(r.Int(), 64))
	b.frame = int64(r.Int())
	b.out.LoadState(r)
	b.out.Clear()
	if r.Err == nil {
		b.render()
	}
	return r.Done()
}
