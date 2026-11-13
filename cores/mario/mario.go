// Package mario is Nintendo's Mario Bros. board (1983): the Donkey Kong 3
// main board's design (a Z80 at 4 MHz, a Z80 DMA chip copying the sprite list
// once a frame, a 32x32 tile map, 16x16 sprites in 8 colours) with an
// 8039-family sound processor (M58715) that plays the music and most effects
// through an 8-bit DAC, and three analogue circuits for Mario's and Luigi's
// footsteps and the skid, made here by RetroPixels' own code to sound like
// them.
//
// The M58715's own 2 KB ROM was never read out; like MAME, the board starts
// it with a three-byte stand-in that jumps to the external program.
package mario

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/mcs48"
	"retropixels/cores/z80"
)

const (
	cpuHz   = 4000000
	lineCyc = 256 // 64 us lines
	lines   = 264
	top     = 16
	scrW    = 256
	scrH    = 224
	vblank  = top + scrH
	sndHz   = 11000000 / 15 // the sound processor's machine cycles a second
	lineHz  = 6000000.0 / 384
	FPS     = lineHz / lines
)

type board struct {
	rom          []byte // 0000-5fff, f000-ffff at 6000
	sndROM       []byte // 4 KB
	boot         [0x800]uint8
	tiles        [512 * 64]uint8
	sprites      [256 * 256]uint8
	pens         [256]uint32
	ram          [0x1000]uint8
	spriteRAM    [0x400]uint8
	vram         [0x400]uint8
	ctrl         [8]uint8 // 7e80-7e87
	scroll       uint8
	latch        uint8 // tune number
	p1Latch      uint8 // the sound processor's port 1 (also set by the game)
	p2Latch      uint8
	tLatch       uint8 // T0 / T1
	sndIRQ       bool
	dma          dma
	stall        int
	nmiLine      bool
	cpu          *z80.CPU
	snd          *mcs48.CPU
	dac          int
	sndCyc       float64
	lp, hp, hpIn float64
	walk         [2]float64 // footstep envelopes
	walkPh       [2]float64
	skid         bool
	skidEnv      float64
	skidPh       float64
	noise        uint32
	acc          float64
	accN         int
	out          *arcade.Resampler
	screen       []uint32
	raster       []uint8
	p1, p2       arcade.Pad
	dips         map[string]int
	cyc          int
	frame        int
}

type bus struct{ b *board }

func (x bus) Read(a uint16) uint8     { return x.b.read(a) }
func (x bus) Write(a uint16, v uint8) { x.b.write(a, v) }
func (x bus) In(uint16) uint8         { return 0xff }
func (x bus) Out(p uint16, v uint8) {
	if p&0xff == 0 {
		x.b.dma.write(int(v))
	}
}

type sndBus struct{ b *board }

// ROM: 000-7ff is the processor's own ROM (the stand-in) until port 2 bit 5
// goes low, then the external ROM's first half; 800-fff its second half.
func (x sndBus) ROM(a int) uint8 {
	b := x.b
	a &= 0xfff
	if a >= 0x800 || b.p2Latch&0x20 == 0 {
		return b.sndROM[a]
	}
	return b.boot[a]
}

// ReadX: with port 2 bit 7 high the tune latch, else tune data from the
// external ROM, paged by port 2 bits 0-3.
func (x sndBus) ReadX(a uint8) uint8 {
	b := x.b
	if b.p2Latch&0x80 != 0 {
		return b.latch
	}
	return b.sndROM[(int(b.p2Latch&0x0f)<<8|int(a))&0x7ff]
}
func (x sndBus) WriteX(_, v uint8) { x.b.dac = int(v) }
func (x sndBus) OutP(p int, v uint8) {
	switch p {
	case 1:
		x.b.p1Latch = v
	case 2:
		x.b.p2Latch = v
	}
}
func (x sndBus) InP(p int) uint8 {
	switch p {
	case 1:
		return x.b.p1Latch
	case 2:
		return x.b.p2Latch & 0xef // bit 4 is wired to ground
	}
	return 0xff
}
func (x sndBus) T0() bool      { return x.b.tLatch&1 != 0 }
func (x sndBus) T1() bool      { return x.b.tLatch&2 != 0 }
func (x sndBus) IntLine() bool { return x.b.sndIRQ }

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips}
	b.rom = arcade.Region(g, files, "cpu", 0x7000)
	b.sndROM = arcade.Region(g, files, "sound", 0x1000)
	b.boot[0], b.boot[1], b.boot[2] = 0xf5, 0x04, 0x00 // SEL MB1; JMP 000 (= 800)
	t := arcade.Region(g, files, "tiles", 0x2000)
	for c := 0; c < 512; c++ {
		for y := 0; y < 8; y++ {
			lo, hi := t[c*8+y], t[0x1000+c*8+y]
			for x := 0; x < 8; x++ {
				b.tiles[c*64+y*8+x] = (hi>>(7-x)&1)<<1 | lo>>(7-x)&1
			}
		}
	}
	s := arcade.Region(g, files, "sprites", 0x6000)
	for c := 0; c < 256; c++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				o := c*16 + y
				if x >= 8 {
					o += 0x1000
				}
				sh := uint(7 - x&7)
				b.sprites[c*256+y*16+x] = (s[o+0x4000]>>sh&1)<<2 | (s[o+0x2000]>>sh&1)<<1 | s[o]>>sh&1
			}
		}
	}
	b.palette(arcade.Region(g, files, "palette", 0x200))
	b.screen = make([]uint32, scrW*scrH)
	b.raster = make([]uint8, scrW*scrH)
	b.out = arcade.NewResampler(lineHz, float64(opts.SampleRate))
	b.cpu = z80.New(bus{b})
	b.snd = mcs48.New(sndBus{b}, 128)
	b.Reset()
	return b, nil
}

// ---------------------------------------------------------------- colours

// resNet is MAME's compute_res_net for these PROM outputs (MB7052, TTL):
// three resistors to red and green and two to blue, a 470 ohm (680 for blue)
// bias, Darlington (red, green) and emitter-follower (blue) amplifiers and
// Nintendo's inverting monitor input.
func resNet(inputs int, r []float64, rBias, minout, cut float64) float64 {
	const vcc, vOL, vOH, ttlHRes = 5.0, 0.05, 4.0, 50.0
	rTotal, v := 0.0, 0.0
	for i, x := range r {
		if inputs>>i&1 == 0 {
			rTotal += 1 / x
			v += vOL / x
		}
	}
	rTotal += 1 / rBias
	v += vcc / rBias
	open := v/rTotal > vOH
	for i, x := range r {
		if inputs>>i&1 != 0 && !open {
			rTotal += 1 / (x + ttlHRes)
			v += vOH / (x + ttlHRes)
		}
	}
	v /= rTotal
	v = math.Max(minout, v-cut)
	v = vcc - v
	v = math.Max(0, v-0.7)
	v = math.Min(v, vcc-1.4)
	v = v / (vcc - 1.4) * vcc
	return float64(int(v*255/vcc + 0.4))
}

// palette: the PROM's first half (for Nintendo's monitor), then MAME's
// normalize_range over all 256 colours.
func (b *board) palette(prom []byte) {
	rg := []float64{1000, 470, 220}
	bl := []float64{470, 220}
	var rgb [256][3]float64
	ymin, ymax := 1000*255, 0
	for i := 0; i < 256; i++ {
		c := int(prom[i])
		rgb[i] = [3]float64{resNet(c>>5&7, rg, 470, 0.7, 0), resNet(c>>2&7, rg, 470, 0.7, 0), resNet(c&3, bl, 680, 0, 0.7)}
		y := 299*int(rgb[i][0]) + 587*int(rgb[i][1]) + 114*int(rgb[i][2])
		ymin, ymax = min(ymin, y), max(ymax, y)
	}
	clamp := func(v int) int { return max(0, min(255, v)) }
	for i := 0; i < 256; i++ {
		r, g, bb := int(rgb[i][0]), int(rgb[i][1]), int(rgb[i][2])
		y := 299*r + 587*g + 114*bb
		u := (bb - y/1000) * 492 / 1000
		v := (r - y/1000) * 877 / 1000
		t := 0
		if ymax > ymin {
			t = (y - ymin) * 256 / (ymax - ymin)
		}
		b.pens[i] = arcade.RGB(clamp(t+1140*v/1000), clamp(t-395*u/1000-581*v/1000), clamp(t+2032*u/1000))
	}
}

// ---------------------------------------------------------------- memory

func (b *board) Reset() {
	b.cpu.Reset()
	b.cpu.IRQ = false
	b.snd.Reset()
	b.ram, b.spriteRAM, b.vram, b.ctrl = [0x1000]uint8{}, [0x400]uint8{}, [0x400]uint8{}, [8]uint8{}
	b.scroll, b.latch, b.p1Latch, b.p2Latch, b.tLatch, b.sndIRQ = 0, 0, 0, 0xff, 0, false
	b.dma = dma{}
	b.stall, b.cyc, b.frame, b.sndCyc = 0, 0, 0, 0
	b.dac, b.lp, b.hp, b.hpIn = 0, 0, 0, 0
	b.walk, b.walkPh, b.skid, b.skidEnv, b.skidPh, b.noise = [2]float64{}, [2]float64{}, false, 0, 0, 1
	b.acc, b.accN = 0, 0
}

func bit(on bool, v uint8) uint8 {
	if on {
		return v
	}
	return 0
}

func pad(p arcade.Pad) uint8 {
	return bit(p["Right"], 1) | bit(p["Left"], 2) | bit(p["Jump"], 0x10)
}

func (b *board) read(a uint16) uint8 {
	switch {
	case a < 0x6000:
		return b.rom[a]
	case a < 0x7000:
		return b.ram[a-0x6000]
	case a < 0x7400:
		return b.spriteRAM[a-0x7000]
	case a < 0x7800:
		return b.vram[a-0x7400]
	case a == 0x7c00:
		p, q := b.p1, b.p2
		return pad(p) | bit(p["Start (1P)"] || q["Start (1P)"], 0x20) | bit(p["Start (2P)"] || q["Start (2P)"], 0x40) | bit(p["Service"], 0x80)
	case a == 0x7c80:
		return pad(b.p2) | bit(b.p1["Coin"], 0x20) | bit(b.p1["Coin 2"], 0x40)
	case a == 0x7f80:
		d := b.dips
		return uint8(d["lives"]&3 | (d["coins"]&3)<<2 | (d["bonus"]&3)<<4 | (d["difficulty"]&3)<<6)
	case a >= 0xf000:
		return b.rom[0x6000+int(a-0xf000)]
	}
	return 0xff
}

func (b *board) write(a uint16, v uint8) {
	switch {
	case a >= 0x6000 && a < 0x7000:
		b.ram[a-0x6000] = v
	case a >= 0x7000 && a < 0x7400:
		b.spriteRAM[a-0x7000] = v
	case a >= 0x7400 && a < 0x7800:
		b.vram[a-0x7400] = v
	case a == 0x7c00:
		b.walk[0] = 1 // Mario's footstep (1 + time since it)
	case a == 0x7c80:
		b.walk[1] = 1 // Luigi's
	case a == 0x7d00:
		b.scroll = v + 17
	case a == 0x7e00:
		b.latch = v
	case a >= 0x7e80 && a < 0x7e88:
		r := a & 7
		old := b.ctrl[r]
		b.ctrl[r] = v & 1
		switch r {
		case 4: // NMI on
			if v&1 == 0 {
				b.nmiLine = false
			}
		case 5: // DMA ready
			if old == 0 && v&1 != 0 {
				b.runDMA()
			}
		}
	case a >= 0x7f00 && a < 0x7f08:
		d := v & 1
		switch r := a & 7; r {
		case 0: // death
			b.sndIRQ = d != 0
		case 1, 2: // get coin, ice
			m := uint8(1) << (r - 1)
			b.tLatch = b.tLatch&^m | d<<(r-1)
		case 3, 4, 5, 6: // crab, turtle, fly, coin
			m := uint8(1) << (r - 3)
			b.p1Latch = b.p1Latch&^m | d<<(r-3)
		case 7:
			b.skid = d != 0
		}
	}
}

// ---------------------------------------------------------------- video

func (b *board) drawFrame() {
	flip := b.ctrl[2] != 0
	tbank := int(b.ctrl[0]) * 256
	pbank := int(b.ctrl[3]) * 16
	r := b.raster
	for y := 0; y < scrH; y++ {
		sy := y + top
		if flip {
			sy = 255 - sy
		}
		ty := (sy + int(b.scroll)) & 0xff
		for x := 0; x < scrW; x++ {
			sx := x
			if flip {
				sx = 255 - x
			}
			v := int(b.vram[(ty>>3)*32+sx>>3])
			code := v + tbank
			colour := 8 + v>>5 + pbank
			r[y*scrW+x] = uint8(colour*8 + int(b.tiles[code*64+(ty&7)*8+sx&7]))
		}
	}
	s := &b.spriteRAM
	for o := 0; o < 0x400; o += 4 {
		if s[o] == 0 {
			continue
		}
		var y, x int
		if flip {
			y = (int(s[o]) + 0xf7 + 1) & 0xff
		} else {
			y = (int(s[o]) + 0xf9 + 1) & 0xff
		}
		x = int(s[o+3])
		y = 240 - y
		if flip {
			y ^= 0xff
			x ^= 0xff
			y -= 14
			x -= 7
		} else {
			y++
			x -= 8
		}
		code := int(s[o+2])
		colour := (int(s[o+1]&15) + pbank) * 8
		fx, fy := s[o+1]&0x80 != 0, s[o+1]&0x40 != 0
		if flip {
			fx, fy = !fx, !fy
		}
		for py := 0; py < 16; py++ {
			yy := y + py - top
			if yy < 0 || yy >= scrH {
				continue
			}
			ry := py
			if fy {
				ry = 15 - py
			}
			for px := 0; px < 16; px++ {
				xx := x + px
				if xx < 0 || xx >= scrW {
					continue
				}
				rx := px
				if fx {
					rx = 15 - px
				}
				if p := b.sprites[code*256+ry*16+rx]; p != 0 {
					r[yy*scrW+xx] = uint8(colour + int(p))
				}
			}
		}
	}
	for i, p := range r {
		b.screen[i] = b.pens[p]
	}
}

func (b *board) Screen() []uint32 { return b.screen }

func (b *board) SetInput(p []arcade.Pad) {
	b.p1 = p[0]
	b.p2 = arcade.Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

// ---------------------------------------------------------------- sound

// sound runs the sound processor for one line and makes the line's sample:
// the DAC through the board's filters, plus the analogue effects.
func (b *board) sound() {
	b.sndCyc += float64(sndHz) / lineHz
	for b.sndCyc > 0 {
		b.sndCyc -= float64(b.snd.Step())
	}
	const dt = 1 / lineHz
	// DAC: AC-coupled, then a low-pass around 3 kHz
	x := float64(b.dac) / 255
	b.hp = 0.995 * (b.hp + x - b.hpIn)
	b.hpIn = x
	b.lp += (b.hp - b.lp) * 0.7
	v := b.lp * 0.9
	// footsteps: the circuit's output, as measured from MAME's model of it:
	// a buzzing step up for 22 ms, then a sharp drop that dies away
	for i := range b.walk {
		t := b.walk[i]
		if t <= 0 {
			continue
		}
		t -= 1 // walk holds 1 + seconds since the step
		switch {
		case t < 0.022:
			b.walkPh[i] += 1200 * dt
			r := 0.04
			if math.Mod(b.walkPh[i], 1) < 0.5 {
				r = -0.04
			}
			v += 0.34 - 2.5*t + r
		case t < 0.25:
			v += -0.82*math.Exp(-(t-0.022)/0.013) + 0.09*(1-math.Exp(-(t-0.022)/0.02))*math.Exp(-(t-0.022)/0.06)
		default:
			b.walk[i] = 0
			continue
		}
		b.walk[i] += dt
	}
	// skid: noisy squeal while the line is held
	target := 0.0
	if b.skid {
		target = 1
	}
	b.skidEnv += (target - b.skidEnv) * 0.01
	if b.skidEnv > 0.001 {
		b.noise ^= b.noise << 13
		b.noise ^= b.noise >> 17
		b.noise ^= b.noise << 5
		b.skidPh += (1800 + float64(b.noise&0xff)*2) * dt
		sq := 1.0
		if math.Mod(b.skidPh, 1) < 0.5 {
			sq = -1
		}
		v += sq * b.skidEnv * 0.06
	}
	b.out.Add(math.Max(-1, math.Min(1, v)))
}

func (b *board) TakeAudio() []float32 { return b.out.Take() }

// ---------------------------------------------------------------- timing

func (b *board) RunFrame() {
	for line := 0; line < lines; line++ {
		if line == vblank {
			b.drawFrame()
			if b.ctrl[4] != 0 && !b.nmiLine {
				b.nmiLine = true
				b.cpu.NMI()
			}
		}
		end := (line + 1) * lineCyc
		for b.cyc < end {
			if b.stall > 0 {
				n := min(b.stall, end-b.cyc)
				b.stall -= n
				b.cyc += n
				continue
			}
			b.cyc += b.cpu.Step()
		}
		b.sound()
	}
	b.cyc -= lines * lineCyc
	b.frame++
}
