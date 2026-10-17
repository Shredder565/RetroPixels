package technos

// Double Dragon (1987).
//
// Main HD6309 at 3 MHz: 0000-0fff RAM, 1000-13ff palette (red/green, then
// blue), 1800-1fff characters, 2000-21ff RAM shared with the HD63701,
// 2800-2fff sprites, 3000-37ff background, 3800-3804 inputs and switches,
// 3808 control (bit 0 x scroll bit 8, 1 y scroll bit 8, 2 picture not
// flipped, 3 HD63701 out of reset, 4 HD63701 halted, 5-7 ROM bank), 3809 /
// 380a scroll, 380b-380d acknowledge NMI / FIRQ / IRQ, 380e sound command,
// 380f NMI to the HD63701; 4000-7fff a 16 KB bank of 6, 8000-ffff ROM. NMI
// at the start of the vertical blank (line 240), FIRQ every 16 lines, IRQ
// when the HD63701 has finished.
//
// HD63701 (1.5 MHz) with its own 16 KB ROM: 8000-81ff the shared RAM. The
// main processor halts it while it reads the shared RAM and wakes it with an
// NMI; it answers with a pulse on port 6 bit 0, which interrupts the main
// processor.
//
// Sound 6809 at 1.5 MHz: 0000-0fff RAM, 1000 the command (IRQ), 1800 the
// ADPCM voices' idle bits, 2800-2801 YM2151 (its IRQ is the FIRQ),
// 3800-3807 ADPCM: start, end address, start address and stop, for voice 0
// at even and voice 1 at odd addresses. Each voice plays nibbles from its
// 64 KB ROM, between addresses in 512-byte units, through an MSM5205 at
// 375 kHz / 48.

import (
	"retropixels/cores/arcade"
	"retropixels/cores/m6801"
	"retropixels/cores/m6809"
	"retropixels/cores/msm5205"
	"retropixels/cores/ym2151"
)

const (
	ddSubLine   = 96 // HD63701 clocks a line (1.5 MHz)
	ddSoundLine = 96 // sound 6809 clocks a line (1.5 MHz)
	ymClock     = 3579545
	adpcmClock  = 375000
)

type ddragon struct {
	game *arcade.Game
	dips map[string]int

	main, snd *m6809.CPU
	sub       *m6801.CPU
	ym        *ym2151.YM2151
	adpcm     [2]*msm5205.Chip
	vid       *video

	prog, banks, subROM, sndROM []byte
	adpcmROM                    [2][]byte

	ram      [0x1000]uint8
	palRAM   [0x400]uint8
	fgRAM    [0x800]uint8
	shared   [0x200]uint8
	sprRAM   [0x800]uint8
	bgRAM    [0x800]uint8
	sndRAM   [0x1000]uint8
	ctrl     uint8 // 3808
	scrollX  uint8
	scrollY  uint8
	firq     bool
	irq      bool
	latch    uint8
	sndIRQ   bool
	subPort6 uint8
	subReset bool // held in reset (3808 bit 3 low)
	subHalt  bool

	adpcmPos, adpcmEnd   [2]int // nibble addresses
	adpcmStart, adpcmTop [2]uint8
	adpcmIdle            [2]bool

	mainOver, subOver, sndOver int
	ymFrac, adpcmFrac          float64
	adpcmOut                   [2]int
	lp                         float64 // output low-pass filter
	out                        *arcade.Resampler
	pads                       []arcade.Pad
	line                       int
	frame                      int64
}

type ddMainBus struct{ b *ddragon }

func (x ddMainBus) Read(a uint16) uint8     { return x.b.read(a) }
func (x ddMainBus) Write(a uint16, v uint8) { x.b.write(a, v) }

type ddSubBus struct{ b *ddragon }

func (x ddSubBus) Read(a uint16) uint8 {
	switch {
	case a >= 0xc000:
		return x.b.subROM[a-0xc000]
	case a >= 0x8000 && a < 0x8200:
		return x.b.shared[a-0x8000]
	}
	return 0xff
}

func (x ddSubBus) Write(a uint16, v uint8) {
	if a >= 0x8000 && a < 0x8200 {
		x.b.shared[a-0x8000] = v
	}
}

func (x ddSubBus) PortRead(int) uint8 { return 0xff }

// PortWrite: port 6 bit 0 going low interrupts the main processor.
func (x ddSubBus) PortWrite(port int, v, ddr uint8) {
	if port != 6 {
		return
	}
	out := v & ddr
	if x.b.subPort6&1 != 0 && out&1 == 0 {
		x.b.irq = true
	}
	x.b.subPort6 = out
}

type ddSndBus struct{ b *ddragon }

func (x ddSndBus) Read(a uint16) uint8     { return x.b.sndRead(a) }
func (x ddSndBus) Write(a uint16, v uint8) { x.b.sndWrite(a, v) }

func newDDragon(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &ddragon{game: g, dips: opts.Dips}
	b.prog = arcade.Region(g, files, "cpu", 0x8000)
	b.banks = arcade.Region(g, files, "banks", 0x18000)
	b.subROM = arcade.Region(g, files, "mcu", 0x4000)
	b.sndROM = arcade.Region(g, files, "sound", 0x8000)
	b.adpcmROM[0] = arcade.Region(g, files, "adpcm0", 0x10000)
	b.adpcmROM[1] = arcade.Region(g, files, "adpcm1", 0x10000)
	b.vid = newVideo(arcade.Region(g, files, "chars", 0x8000), arcade.Region(g, files, "tiles", 0x40000),
		arcade.Region(g, files, "sprites", 0x80000))
	b.vid.fg, b.vid.bg, b.vid.spr = b.fgRAM[:], b.bgRAM[:], b.sprRAM[:]
	b.vid.palLo, b.vid.palHi = b.palRAM[:0x200], b.palRAM[0x200:]
	b.main = m6809.New(ddMainBus{b}, m6809.HD6309)
	b.sub = m6801.New(ddSubBus{b}, m6801.HD63701Y)
	b.snd = m6809.New(ddSndBus{b}, m6809.MC6809)
	b.ym = ym2151.New(ymClock)
	b.ym.OnIRQ = func(on bool) { b.snd.FIRQ = on }
	for i := range b.adpcm {
		b.adpcm[i] = msm5205.New(adpcmClock, 48, 4)
	}
	b.out = arcade.NewResampler(b.ym.Rate(), float64(opts.SampleRate))
	b.Reset()
	return b, nil
}

func (b *ddragon) Reset() {
	b.ram, b.palRAM, b.fgRAM, b.shared = [0x1000]uint8{}, [0x400]uint8{}, [0x800]uint8{}, [0x200]uint8{}
	b.sprRAM, b.bgRAM, b.sndRAM = [0x800]uint8{}, [0x800]uint8{}, [0x1000]uint8{}
	b.ctrl, b.scrollX, b.scrollY = 0, 0, 0
	b.firq, b.irq, b.sndIRQ, b.latch = false, false, false, 0
	b.subPort6, b.subReset, b.subHalt = 0, true, false
	b.main.IRQ, b.main.FIRQ = false, false
	b.main.Reset()
	b.snd.IRQ, b.snd.FIRQ = false, false
	b.snd.Reset()
	b.sub.IRQ = false
	b.sub.Reset()
	b.ym.Reset()
	for i := range b.adpcm {
		b.adpcm[i].Reset()
		b.adpcm[i].SetReset(true)
		b.adpcmIdle[i] = true
		b.adpcmPos[i], b.adpcmEnd[i], b.adpcmStart[i], b.adpcmTop[i] = 0, 0, 0, 0
		b.adpcmOut[i] = 0
	}
	b.mainOver, b.subOver, b.sndOver = 0, 0, 0
	b.ymFrac, b.adpcmFrac, b.lp = 0, 0, 0
	b.line, b.frame = 0, 0
	b.applyCtrl()
}

// ---------------------------------------------------------------- main CPU

func (b *ddragon) read(a uint16) uint8 {
	switch {
	case a < 0x1000:
		return b.ram[a]
	case a < 0x1400:
		return b.palRAM[a-0x1000]
	case a >= 0x1800 && a < 0x2000:
		return b.fgRAM[a-0x1800]
	case a >= 0x2000 && a < 0x2200:
		return b.shared[a-0x2000]
	case a >= 0x2800 && a < 0x3000:
		return b.sprRAM[a-0x2800]
	case a >= 0x3000 && a < 0x3800:
		return b.bgRAM[a-0x3000]
	case a >= 0x3800 && a < 0x3805:
		return b.input(a - 0x3800)
	case a >= 0x4000 && a < 0x8000:
		bank := int(b.ctrl>>5) % 6
		return b.banks[bank*0x4000+int(a-0x4000)]
	case a >= 0x8000:
		return b.prog[a-0x8000]
	}
	return 0xff
}

func (b *ddragon) write(a uint16, v uint8) {
	switch {
	case a < 0x1000:
		b.ram[a] = v
	case a < 0x1400:
		b.palRAM[a-0x1000] = v
	case a >= 0x1800 && a < 0x2000:
		b.fgRAM[a-0x1800] = v
	case a >= 0x2000 && a < 0x2200:
		b.shared[a-0x2000] = v
	case a >= 0x2800 && a < 0x3000:
		b.sprRAM[a-0x2800] = v
	case a >= 0x3000 && a < 0x3800:
		b.bgRAM[a-0x3000] = v
	case a == 0x3808:
		b.ctrl = v
		b.applyCtrl()
	case a == 0x3809:
		b.scrollX = v
	case a == 0x380a:
		b.scrollY = v
	case a == 0x380b: // NMI acknowledge (the NMI is an edge)
	case a == 0x380c:
		b.firq = false
	case a == 0x380d:
		b.irq = false
	case a == 0x380e:
		b.latch = v
		b.sndIRQ = true
	case a == 0x380f:
		b.sub.NMI()
	}
}

// applyCtrl follows 3808: the HD63701's reset and halt lines.
func (b *ddragon) applyCtrl() {
	reset := b.ctrl&0x08 == 0
	if reset && !b.subReset {
		b.sub.Reset()
	}
	if !reset && b.subReset {
		b.sub.Reset()
	}
	b.subReset = reset
	b.subHalt = b.ctrl&0x10 != 0
}

func (b *ddragon) input(r uint16) uint8 {
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
		return uint8(d["difficulty"]&3 | (d["demosound"]&1)<<2 | 0x08 | (d["bonus"]&3)<<4 | (d["lives"]&3)<<6)
	}
	return 0xff
}

// ---------------------------------------------------------------- sound CPU

func (b *ddragon) sndRead(a uint16) uint8 {
	switch {
	case a < 0x1000:
		return b.sndRAM[a]
	case a == 0x1000:
		b.sndIRQ = false
		return b.latch
	case a == 0x1800:
		var v uint8
		for i := range b.adpcmIdle {
			if b.adpcmIdle[i] {
				v |= 1 << i
			}
		}
		return v
	case a == 0x2801:
		return b.ym.Read()
	case a >= 0x8000:
		return b.sndROM[a-0x8000]
	}
	return 0xff
}

func (b *ddragon) sndWrite(a uint16, v uint8) {
	switch {
	case a < 0x1000:
		b.sndRAM[a] = v
	case a == 0x2800 || a == 0x2801:
		b.ym.Write(int(a&1), v)
	case a >= 0x3800 && a < 0x3808:
		ch := int(a & 1)
		switch a & 6 {
		case 0: // start
			b.adpcmPos[ch] = int(b.adpcmStart[ch]) << 10
			b.adpcmEnd[ch] = int(b.adpcmTop[ch]) << 10
			b.adpcmIdle[ch] = false
			b.adpcm[ch].SetReset(false)
		case 2:
			b.adpcmTop[ch] = v
		case 4:
			b.adpcmStart[ch] = v
		case 6: // stop
			b.adpcmIdle[ch] = true
			b.adpcm[ch].SetReset(true)
		}
	}
}

// adpcmTick is one VCK of both voices: each plays its next nibble.
func (b *ddragon) adpcmTick() {
	for ch := range b.adpcm {
		if b.adpcmIdle[ch] {
			b.adpcmOut[ch] = b.adpcm[ch].Clock()
			continue
		}
		if b.adpcmPos[ch] >= b.adpcmEnd[ch] || b.adpcmPos[ch] >= 0x20000 {
			b.adpcmIdle[ch] = true
			b.adpcm[ch].SetReset(true)
			b.adpcmOut[ch] = b.adpcm[ch].Clock()
			continue
		}
		d := b.adpcmROM[ch][b.adpcmPos[ch]>>1]
		if b.adpcmPos[ch]&1 == 0 {
			d >>= 4
		}
		b.adpcm[ch].Write(d & 15)
		b.adpcmOut[ch] = b.adpcm[ch].Clock()
		b.adpcmPos[ch]++
	}
}

// ---------------------------------------------------------------- frame

func (b *ddragon) SetInput(p []arcade.Pad) { b.pads = p }
func (b *ddragon) Screen() []uint32        { return b.vid.screen }
func (b *ddragon) TakeAudio() []float32    { return b.out.Take() }

// RunFrame runs from the start of the vertical blank (where the last frame
// was drawn) to the next one, and draws the frame.
func (b *ddragon) syncVid() {
	b.vid.scrollX = int(b.scrollX) | int(b.ctrl&1)<<8
	b.vid.scrollY = int(b.scrollY) | int(b.ctrl&2)<<7
	b.vid.flip = b.ctrl&4 == 0
}

// RunFrame runs from the start of the vertical blank to the next, drawing
// each row as its line ends (the game changes the video RAM while the
// picture is being drawn).
func (b *ddragon) RunFrame() {
	for {
		row := b.line
		b.runLine()
		if row < scrH {
			b.syncVid()
			b.vid.renderRows(row, row+1)
		}
		if b.line == vblankLine {
			break
		}
	}
	b.frame++
}

func (b *ddragon) runLine() {
	line := b.line
	if line == vblankLine {
		b.main.NMI()
	}
	if line&15 == 0 && line > 0 && line <= vblankLine || line == 264 { // lines 16, 32 .. 240 and 264
		b.firq = true
	}
	b.main.FIRQ, b.main.IRQ = b.firq, b.irq
	b.snd.IRQ = b.sndIRQ
	// run the processors in four slices a line
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
			for n < ddSubLine/4 {
				n += b.sub.Step()
			}
			b.subOver = n - ddSubLine/4
		}
		n = b.sndOver
		for n < ddSoundLine/4 {
			b.snd.IRQ = b.sndIRQ
			n += b.snd.Step()
		}
		b.sndOver = n - ddSoundLine/4
	}
	b.sound()
	b.line = (line + 1) % lines
}

var ddFMGain, ddPCMGain = 1.2, 0.53

// sound makes a line's worth of samples (YM2151 rate) with the ADPCM voices
// (at their VCK rate) mixed in.
func (b *ddragon) sound() {
	const lineHz = float64(pixelHz) / lineClocks
	b.adpcmFrac += adpcmClock / 48 / lineHz
	for b.adpcmFrac >= 1 {
		b.adpcmFrac--
		b.adpcmTick()
	}
	b.ymFrac += b.ym.Rate() / lineHz
	for b.ymFrac >= 1 {
		b.ymFrac--
		b.ym.Tick()
		fm := float64(b.ym.Out[0]+b.ym.Out[1]) / 65536
		pcm := float64(b.adpcmOut[0]+b.adpcmOut[1]) / 4096
		b.lp += lpCoef(b.ym.Rate()) * (fm*ddFMGain + pcm*ddPCMGain - b.lp)
		b.out.Add(b.lp)
	}
}

// ---------------------------------------------------------------- state

func (b *ddragon) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.AppendState(w.B)
	w.B = b.snd.AppendState(w.B)
	w.B = b.sub.AppendState(w.B)
	w.B = b.ym.AppendState(w.B)
	for i := range b.adpcm {
		w.B = b.adpcm[i].AppendState(w.B)
	}
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
	w.Bool(b.sndIRQ)
	w.U8(b.subPort6)
	w.Bool(b.subReset)
	w.Bool(b.subHalt)
	for i := 0; i < 2; i++ {
		w.Int(b.adpcmPos[i])
		w.Int(b.adpcmEnd[i])
		w.U8(b.adpcmStart[i])
		w.U8(b.adpcmTop[i])
		w.Bool(b.adpcmIdle[i])
		w.Int(b.adpcmOut[i])
	}
	w.Int(b.mainOver)
	w.Int(b.subOver)
	w.Int(b.sndOver)
	w.F64(b.ymFrac)
	w.F64(b.adpcmFrac)
	w.F64(b.lp)
	w.Int(b.line)
	w.Int(int(b.frame))
	b.out.AppendState(w)
	return w.B, nil
}

func (b *ddragon) LoadState(s []byte) error {
	rest, err := b.main.LoadState(s)
	if err == nil {
		rest, err = b.snd.LoadState(rest)
	}
	if err == nil {
		rest, err = b.sub.LoadState(rest)
	}
	if err == nil {
		rest, err = b.ym.LoadState(rest)
	}
	for i := 0; i < 2 && err == nil; i++ {
		rest, err = b.adpcm[i].LoadState(rest)
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
	b.sndIRQ = r.Bool()
	b.subPort6 = r.U8()
	b.subReset = r.Bool()
	b.subHalt = r.Bool()
	for i := 0; i < 2; i++ {
		b.adpcmPos[i] = max(0, min(r.Int(), 0x20000))
		b.adpcmEnd[i] = max(0, min(r.Int(), 0x20000))
		b.adpcmStart[i] = r.U8()
		b.adpcmTop[i] = r.U8()
		b.adpcmIdle[i] = r.Bool()
		b.adpcmOut[i] = r.Int()
	}
	b.mainOver = max(0, min(r.Int(), 64))
	b.subOver = max(0, min(r.Int(), 64))
	b.sndOver = max(0, min(r.Int(), 64))
	b.ymFrac = r.F64()
	b.adpcmFrac = r.F64()
	b.lp = r.F64()
	b.line = max(0, min(r.Int(), lines-1))
	b.frame = int64(r.Int())
	b.out.LoadState(r)
	b.out.Clear()
	if err := r.Done(); err != nil {
		return err
	}
	return nil
}
