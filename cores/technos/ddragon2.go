package technos

// Double Dragon II: The Revenge (1988).
//
// Main HD6309 at 3 MHz, as on Double Dragon but with 6 KB of RAM at
// 0000-17ff and the palette at 3c00-3fff; 380f interrupts the sub processor.
//
// Sub Z80 at 4 MHz: 0000-bfff ROM, c000-c3ff RAM (the main processor sees
// its first 512 bytes at 2000-21ff), d000 acknowledges its NMI, e000
// interrupts the main processor. Halted and reset by 3808 as on Double
// Dragon.
//
// Sound Z80 at 3.58 MHz: 0000-7fff ROM, 8000-87ff RAM, 8800-8801 YM2151 (its
// IRQ), 9800 MSM6295 (1.056 MHz, 8 kHz samples), a000 the command (an NMI
// when written).

import (
	"retropixels/cores/arcade"
	"retropixels/cores/m6809"
	"retropixels/cores/msm6295"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

const (
	dd2SubLine = 256 // sub Z80 clocks a line (4 MHz)
	dd2SndHz   = 3579545
	okiRate    = 1056000.0 / 132
)

type ddragon2 struct {
	game *arcade.Game
	dips map[string]int

	main *m6809.CPU
	sub  *z80.CPU
	snd  *z80.CPU
	ym   *ym2151.YM2151
	oki  *msm6295.Chip
	vid  *video

	prog, banks, subROM, sndROM []byte

	ram      [0x1800]uint8
	palRAM   [0x400]uint8
	fgRAM    [0x800]uint8
	shared   [0x400]uint8
	sprRAM   [0x800]uint8
	bgRAM    [0x800]uint8
	sndRAM   [0x800]uint8
	ctrl     uint8
	scrollX  uint8
	scrollY  uint8
	firq     bool
	irq      bool
	latch    uint8
	subReset bool
	subHalt  bool

	mainOver, subOver, sndOver int
	sndFrac, ymFrac, okiFrac   float64
	okiOut                     int
	lp                         float64
	out                        *arcade.Resampler
	pads                       []arcade.Pad
	line                       int
	frame                      int64
	trace                      func(ev string, v uint8) // tests: board events
}

type dd2MainBus struct{ b *ddragon2 }

func (x dd2MainBus) Read(a uint16) uint8     { return x.b.read(a) }
func (x dd2MainBus) Write(a uint16, v uint8) { x.b.write(a, v) }

type dd2SubBus struct{ b *ddragon2 }

func (x dd2SubBus) Read(a uint16) uint8 {
	switch {
	case a < 0xc000:
		return x.b.subROM[a]
	case a < 0xc400:
		return x.b.shared[a-0xc000]
	}
	return 0xff
}

func (x dd2SubBus) Write(a uint16, v uint8) {
	switch {
	case a >= 0xc000 && a < 0xc400:
		x.b.shared[a-0xc000] = v
	case a == 0xe000:
		x.b.irq = true
		if x.b.trace != nil {
			x.b.trace("subE000", v)
		}
	}
}

func (x dd2SubBus) In(uint16) uint8   { return 0xff }
func (x dd2SubBus) Out(uint16, uint8) {}

type dd2SndBus struct{ b *ddragon2 }

func (x dd2SndBus) Read(a uint16) uint8 {
	b := x.b
	switch {
	case a < 0x8000:
		return b.sndROM[a]
	case a < 0x8800:
		return b.sndRAM[a-0x8000]
	case a == 0x8801:
		return b.ym.Read()
	case a == 0x9800:
		return b.oki.Read()
	case a == 0xa000:
		return b.latch
	}
	return 0xff
}

func (x dd2SndBus) Write(a uint16, v uint8) {
	b := x.b
	switch {
	case a >= 0x8000 && a < 0x8800:
		b.sndRAM[a-0x8000] = v
	case a == 0x8800 || a == 0x8801:
		b.ym.Write(int(a&1), v)
	case a == 0x9800:
		b.oki.Write(v)
	}
}

func (x dd2SndBus) In(uint16) uint8   { return 0xff }
func (x dd2SndBus) Out(uint16, uint8) {}

func newDDragon2(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &ddragon2{game: g, dips: opts.Dips}
	b.prog = arcade.Region(g, files, "cpu", 0x8000)
	b.banks = arcade.Region(g, files, "banks", 0x18000)
	b.subROM = arcade.Region(g, files, "sub", 0x10000)
	b.sndROM = arcade.Region(g, files, "sound", 0x8000)
	b.vid = newVideo(arcade.Region(g, files, "chars", 0x10000), arcade.Region(g, files, "tiles", 0x40000),
		arcade.Region(g, files, "sprites", 0xc0000))
	b.vid.dd2Sprites = true
	b.vid.fg, b.vid.bg, b.vid.spr = b.fgRAM[:], b.bgRAM[:], b.sprRAM[:]
	b.vid.palLo, b.vid.palHi = b.palRAM[:0x200], b.palRAM[0x200:]
	b.main = m6809.New(dd2MainBus{b}, m6809.HD6309)
	b.sub = z80.New(dd2SubBus{b})
	b.snd = z80.New(dd2SndBus{b})
	b.ym = ym2151.New(ymClock)
	b.ym.OnIRQ = func(on bool) { b.snd.IRQ = on }
	b.oki = msm6295.New(arcade.Region(g, files, "oki", 0x40000))
	b.out = arcade.NewResampler(b.ym.Rate(), float64(opts.SampleRate))
	b.Reset()
	return b, nil
}

func (b *ddragon2) Reset() {
	b.ram, b.palRAM, b.fgRAM, b.shared = [0x1800]uint8{}, [0x400]uint8{}, [0x800]uint8{}, [0x400]uint8{}
	b.sprRAM, b.bgRAM, b.sndRAM = [0x800]uint8{}, [0x800]uint8{}, [0x800]uint8{}
	b.ctrl, b.scrollX, b.scrollY = 0, 0, 0
	b.firq, b.irq, b.latch = false, false, 0
	b.subReset, b.subHalt = true, false
	b.main.IRQ, b.main.FIRQ = false, false
	b.main.Reset()
	b.sub.Reset()
	b.sub.IRQ = false
	b.snd.Reset()
	b.snd.IRQ = false
	b.ym.Reset()
	b.oki.Reset()
	b.mainOver, b.subOver, b.sndOver = 0, 0, 0
	b.sndFrac, b.ymFrac, b.okiFrac, b.okiOut, b.lp = 0, 0, 0, 0, 0
	b.line, b.frame = 0, 0
	b.applyCtrl()
}

func (b *ddragon2) read(a uint16) uint8 {
	switch {
	case a < 0x1800:
		return b.ram[a]
	case a < 0x2000:
		return b.fgRAM[a-0x1800]
	case a < 0x2200:
		return b.shared[a-0x2000]
	case a >= 0x2800 && a < 0x3000:
		return b.sprRAM[a-0x2800]
	case a >= 0x3000 && a < 0x3800:
		return b.bgRAM[a-0x3000]
	case a >= 0x3800 && a < 0x3805:
		return b.input(a - 0x3800)
	case a >= 0x3c00 && a < 0x4000:
		return b.palRAM[a-0x3c00]
	case a >= 0x4000 && a < 0x8000:
		bank := int(b.ctrl>>5) % 6
		return b.banks[bank*0x4000+int(a-0x4000)]
	case a >= 0x8000:
		return b.prog[a-0x8000]
	}
	return 0xff
}

func (b *ddragon2) write(a uint16, v uint8) {
	switch {
	case a < 0x1800:
		b.ram[a] = v
	case a < 0x2000:
		b.fgRAM[a-0x1800] = v
	case a < 0x2200:
		b.shared[a-0x2000] = v
	case a >= 0x2800 && a < 0x3000:
		b.sprRAM[a-0x2800] = v
	case a >= 0x3000 && a < 0x3800:
		b.bgRAM[a-0x3000] = v
	case a >= 0x3c00 && a < 0x4000:
		b.palRAM[a-0x3c00] = v
	case a == 0x3808:
		b.ctrl = v
		b.applyCtrl()
	case a == 0x3809:
		b.scrollX = v
	case a == 0x380a:
		b.scrollY = v
	case a == 0x380c:
		b.firq = false
	case a == 0x380d:
		b.irq = false
	case a == 0x380e:
		b.latch = v
		b.snd.NMI()
	case a == 0x380f:
		b.sub.NMI()
		if b.trace != nil {
			b.trace("380f", v)
		}
	}
}

func (b *ddragon2) applyCtrl() {
	reset := b.ctrl&0x08 == 0
	if reset != b.subReset {
		b.sub.Reset()
	}
	b.subReset = reset
	b.subHalt = b.ctrl&0x10 != 0
}

func (b *ddragon2) input(r uint16) uint8 {
	p1, p2 := pad(b.pads, 0), pad(b.pads, 1)
	d := b.dips
	switch r {
	case 0:
		v := padBits(p1, "Right", "Left", "Up", "Down", "Button 1", "Button 2", "Start (1P)", "Start (2P)")
		if p2["Start (1P)"] {
			v &^= 0x80
		}
		return v
	case 1:
		v := padBits(p2, "Right", "Left", "Up", "Down", "Button 1", "Button 2", "Coin", "Coin 2")
		if p1["Coin"] {
			v &^= 0x40
		}
		if p1["Coin 2"] {
			v &^= 0x80
		}
		return v
	case 2:
		v := uint8(0x07)
		if p1["Service"] {
			v &^= 1
		}
		if p1["Button 3"] {
			v &^= 2
		}
		if p2["Button 3"] {
			v &^= 4
		}
		if b.line >= vblankLine {
			v |= 0x08
		}
		if !b.subHalt && !b.subReset {
			v |= 0x10
		}
		return v
	case 3:
		return uint8(d["coina"]&7 | (d["coinb"]&7)<<3 | (d["cabinet"]&1)<<6 | (d["flip"]&1)<<7)
	case 4:
		return uint8(d["difficulty"]&3 | (d["demosound"]&1)<<2 | (d["kick"]&1)<<3 | (d["timer"]&3)<<4 | (d["lives"]&3)<<6)
	}
	return 0xff
}

func (b *ddragon2) SetInput(p []arcade.Pad) { b.pads = p }
func (b *ddragon2) Screen() []uint32        { return b.vid.screen }
func (b *ddragon2) TakeAudio() []float32    { return b.out.Take() }

func (b *ddragon2) RunFrame() {
	for {
		b.runLine()
		if b.line == vblankLine {
			break
		}
	}
	b.vid.scrollX = int(b.scrollX) | int(b.ctrl&1)<<8
	b.vid.scrollY = int(b.scrollY) | int(b.ctrl&2)<<7
	b.vid.flip = b.ctrl&4 == 0
	b.vid.render()
	b.frame++
}

func (b *ddragon2) runLine() {
	const lineHz = float64(pixelHz) / lineClocks
	line := b.line
	if line == vblankLine {
		b.main.NMI()
	}
	if line&15 == 0 && (line < 256 || line == 264) {
		b.firq = true
	}
	b.sndFrac += dd2SndHz / lineHz
	sndLine := int(b.sndFrac)
	b.sndFrac -= float64(sndLine)
	for s := 0; s < 4; s++ {
		n := b.mainOver
		for n < mainLine/4 {
			b.main.FIRQ, b.main.IRQ = b.firq, b.irq
			n += b.main.Step()
		}
		b.mainOver = n - mainLine/4
		if b.subReset || b.subHalt {
			b.subOver = 0
		} else {
			n = b.subOver
			for n < dd2SubLine/4 {
				n += b.sub.Step()
			}
			b.subOver = n - dd2SubLine/4
		}
		slice := sndLine / 4
		if s == 3 {
			slice = sndLine - 3*(sndLine/4)
		}
		n = b.sndOver
		for n < slice {
			n += b.snd.Step()
		}
		b.sndOver = n - slice
	}
	b.okiFrac += okiRate / lineHz
	for b.okiFrac >= 1 {
		b.okiFrac--
		b.okiOut = b.oki.Sample()
	}
	b.ymFrac += b.ym.Rate() / lineHz
	for b.ymFrac >= 1 {
		b.ymFrac--
		b.ym.Tick()
		fm := float64(b.ym.Out[0]+b.ym.Out[1]) / 65536
		pcm := float64(b.okiOut) / 4096
		b.lp += lpCoef(b.ym.Rate()) * (fm*dd2FMGain + pcm*dd2PCMGain - b.lp)
		b.out.Add(b.lp)
	}
	b.line = (line + 1) % lines
}

var dd2FMGain, dd2PCMGain = 1.2, 0.5

func (b *ddragon2) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.AppendState(w.B)
	w.B = b.sub.AppendState(w.B)
	w.B = b.snd.AppendState(w.B)
	w.B = b.ym.AppendState(w.B)
	w.B = b.oki.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.palRAM[:])
	w.Bytes(b.fgRAM[:])
	w.Bytes(b.shared[:])
	w.Bytes(b.sprRAM[:])
	w.Bytes(b.bgRAM[:])
	w.Bytes(b.sndRAM[:])
	w.U8(b.ctrl)
	w.U8(b.scrollX)
	w.U8(b.scrollY)
	w.Bool(b.firq)
	w.Bool(b.irq)
	w.U8(b.latch)
	w.Bool(b.subReset)
	w.Bool(b.subHalt)
	w.Int(b.mainOver)
	w.Int(b.subOver)
	w.Int(b.sndOver)
	w.F64(b.sndFrac)
	w.F64(b.ymFrac)
	w.F64(b.okiFrac)
	w.Int(b.okiOut)
	w.F64(b.lp)
	w.Int(b.line)
	w.Int(int(b.frame))
	b.out.AppendState(w)
	return w.B, nil
}

func (b *ddragon2) LoadState(s []byte) error {
	rest, err := b.main.LoadState(s)
	if err == nil {
		rest, err = b.sub.LoadState(rest)
	}
	if err == nil {
		rest, err = b.snd.LoadState(rest)
	}
	if err == nil {
		rest, err = b.ym.LoadState(rest)
	}
	if err == nil {
		rest, err = b.oki.LoadState(rest)
	}
	if err != nil {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.palRAM[:])
	r.Bytes(b.fgRAM[:])
	r.Bytes(b.shared[:])
	r.Bytes(b.sprRAM[:])
	r.Bytes(b.bgRAM[:])
	r.Bytes(b.sndRAM[:])
	b.ctrl = r.U8()
	b.scrollX = r.U8()
	b.scrollY = r.U8()
	b.firq = r.Bool()
	b.irq = r.Bool()
	b.latch = r.U8()
	b.subReset = r.Bool()
	b.subHalt = r.Bool()
	b.mainOver = max(0, min(r.Int(), 64))
	b.subOver = max(0, min(r.Int(), 64))
	b.sndOver = max(0, min(r.Int(), 64))
	b.sndFrac = r.F64()
	b.ymFrac = r.F64()
	b.okiFrac = r.F64()
	b.okiOut = r.Int()
	b.lp = r.F64()
	b.line = max(0, min(r.Int(), lines-1))
	b.frame = int64(r.Int())
	b.out.LoadState(r)
	b.out.Clear()
	return r.Done()
}
