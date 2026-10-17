package arcade

// The Donkey Kong board (Nintendo, 1981) and Donkey Kong Jr. (1982): a port
// of web/emu/arcade_dkong.js. A Z80 at 3.072 MHz with the program ROMs, work
// RAM, a 32x32 tile map and a sprite list that an 8257 DMA chip copies into
// the sprite memory once a frame; colours from the set's three PROMs; an 8035
// sound processor (mcs48) running the set's own sound program through an
// 8-bit DAC and the board's filter; and the board's analogue sound effects
// (Donkey Kong: walking, jumping, the stomp; Junior: walking/climbing,
// jumping, falling), made by RetroPixels' own code to sound like the
// circuits. The monitor is on its side: the board draws 256x224, turned a
// quarter turn clockwise. Junior has 24 KB of program (its three ROMs hold
// their 2 KB parts out of order), twice the tiles, a 4 KB sound program and
// more sound effects played by the 8035.

import (
	"math"

	"retropixels/cores/mcs48"
	"retropixels/cores/z80"
)

const (
	dkCPUHz   = 3072000
	dkLineCyc = 192
	dkLines   = 264
	dkSndHz   = 6000000 / 15
	dkW, dkH  = 256, 224
	dkTop     = 16
)

var dkFPS = float64(dkCPUHz) / (dkLineCyc * dkLines)

type dkong struct {
	jr         bool // Donkey Kong Jr.
	dips       map[string]int
	sampleRate float64
	rom        []byte
	sndRom     []byte
	tiles      []uint8
	sprites    []uint8
	palette    [256]uint32
	tileColour []byte

	ram       [0x1000]uint8
	spriteRam [0x400]uint8
	vram      [0x400]uint8
	screen    []uint32
	raster    []uint8
	p1, p2    Pad
	audio     []float32
	cpu       *z80.CPU
	snd       *mcs48.CPU

	latch259, misc, soundLatch int
	gfxBank                    int
	sndIrq                     bool
	sndP2, dac                 int
	dma                        [8]uint16
	dmaFlip, dmaMode           int
	cyc                        int
	sndCyc                     float64
	sndAcc                     float64
	sndAccN                    int
	samplePos                  float64
	env                        float64
	filt                       [5]dkBiquad // the DAC's filter
	fx                         [12]float64 // the analogue effects (see effects)
	noise                      int
}

type dkBus struct{ b *dkong }

func (x dkBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x dkBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }
func (x dkBus) In(uint16) uint8         { return 0xff }
func (x dkBus) Out(uint16, uint8)       {}

type dkSndBus struct{ b *dkong }

func (x dkSndBus) ROM(a int) uint8 {
	if a < len(x.b.sndRom) && (x.b.jr || a < 0x800) {
		return x.b.sndRom[a]
	}
	return 0xff
}

// Donkey Kong: P2 bit 6 selects the tune latch; otherwise P2 bits 0-2 page
// the tune data ROM. Junior: always the latch.
func (x dkSndBus) ReadX(a uint8) uint8 {
	b := x.b
	if b.jr || b.sndP2&0x40 != 0 {
		return uint8(b.soundLatch & 0x0f)
	}
	return b.sndRom[0x1000+(b.sndP2&7)*256+int(a)]
}
func (x dkSndBus) WriteX(a, v uint8) {}
func (x dkSndBus) OutP(p int, v uint8) {
	if p == 1 {
		x.b.dac = int(v)
	} else if p == 2 {
		x.b.sndP2 = int(v)
	}
}

// Port 2 reads back its own latch, except bit 5: sound latch output 3
// (inverted), and on Junior bit 4: output 6 (inverted).
func (x dkSndBus) InP(p int) uint8 {
	b := x.b
	switch p {
	case 1:
		return uint8(b.snd.Port(1))
	case 2:
		v := b.snd.Port(2)&^0x20 | bit(b.latch259&0x08 == 0, 0x20)
		if b.jr {
			v = v&^0x10 | bit(b.latch259&0x40 == 0, 0x10)
		}
		return uint8(v)
	}
	return 0xff
}

// the test pins read the main board's sound latch outputs 5 and 4 (inverted)
func (x dkSndBus) T0() bool      { return x.b.latch259&0x20 == 0 }
func (x dkSndBus) T1() bool      { return x.b.latch259&0x10 == 0 }
func (x dkSndBus) IntLine() bool { return x.b.sndIrq }

// dkJrROM: where each 2 KB part of Junior's three program ROMs goes.
var dkJrROM = [12]int{0x0000, 0x0800, 0x3000, 0x3800, 0x2000, 0x4800, 0x1000, 0x5800, 0x4000, 0x2800, 0x5000, 0x1800}

func newDkong(g *Game, files Files, opts Opts) (Board, error) {
	b := &dkong{sampleRate: float64(opts.SampleRate), dips: opts.Dips, jr: g.ID == "dkongjr"}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	dips := g.Dips
	if dips == nil {
		dips = dkDips
	}
	for _, d := range dips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	nTiles := 256
	if b.jr {
		parts := Region(g, files, "cpu", 0x6000)
		b.rom = make([]byte, 0x6000)
		for i, at := range dkJrROM {
			copy(b.rom[at:at+0x800], parts[i*0x800:])
		}
		b.sndRom = Region(g, files, "sound", 0x1000)
		nTiles = 512
	} else {
		b.rom = Region(g, files, "cpu", 0x4000)
		b.sndRom = Region(g, files, "sound", 0x1800)
	}
	tileRom, spriteRom := Region(g, files, "tiles", nTiles*16), Region(g, files, "sprites", 0x2000)
	proms := Region(g, files, "prom", 0x300)
	b.decodeGraphics(tileRom, spriteRom, nTiles)
	b.decodePalette(proms)
	b.tileColour = append([]byte(nil), proms[0x200:0x300]...)
	b.screen = make([]uint32, dkW*dkH)
	b.raster = make([]uint8, dkW*256)
	b.p1, b.p2 = Pad{}, Pad{}
	b.audio = make([]float32, 0, 4096)
	b.cpu = z80.New(dkBus{b})
	b.snd = mcs48.New(dkSndBus{b}, 64)
	b.Reset()
	return b, nil
}

func (b *dkong) decodeGraphics(tileRom, spriteRom []byte, nTiles int) {
	b.tiles = make([]uint8, nTiles*64)
	half := nTiles * 8
	for n := 0; n < nTiles; n++ {
		for y := 0; y < 8; y++ {
			lo, hi := tileRom[n*8+y], tileRom[half+n*8+y]
			for x := 0; x < 8; x++ {
				s := 7 - x
				b.tiles[n*64+y*8+x] = (lo>>s)&1 | ((hi>>s)&1)<<1
			}
		}
	}
	b.sprites = make([]uint8, 128*256)
	for n := 0; n < 128; n++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				half := 0
				if x >= 8 {
					half = 0x800
				}
				s, o := 7-(x&7), half+n*16+y
				b.sprites[n*256+y*16+x] = (spriteRom[o]>>s)&1 | ((spriteRom[o+0x1000]>>s)&1)<<1
			}
		}
	}
}

func jsRound(v float64) int { return int(math.Floor(v + 0.5)) }

// The colour levels each PROM pattern gives on the monitor, measured: the
// resistor ladders aren't linear, and each gun picks up a little of the
// other two.
var (
	dkLevR = [8]float64{0, 37.1, 72, 105.2, 142.4, 171.8, 201.4, 231.3}
	dkLevG = [8]float64{0, 38.3, 75.2, 107.9, 148.1, 177.5, 210.2, 239.6}
	dkLevB = [4]float64{0, 138.2, 217.9, 255}
)

// dkColour: one colour from its 3 red, 3 green and 2 blue bits.
func dkColour(r, g, b int) uint32 {
	lr, lg, lb := dkLevR[r], dkLevG[g], dkLevB[b]
	c := func(v float64) int { return min(255, jsRound(v)) }
	return RGB(c(lr+0.0709*lg+0.0157*lb), c(lg+0.0366*lr+0.0151*lb), c(lb+0.0387*lr+0.0724*lg))
}

// Each colour: red 3 bits and green's top bit from the second PROM, green's
// low 2 bits and blue's 2 bits from the first; the outputs are inverted.
// Colour 0 of each group of 4 is the black background.
func (b *dkong) decodePalette(proms []byte) {
	for i := 0; i < 256; i++ {
		lo, hi := int(^proms[i]&0xf), int(^proms[0x100+i]&0xf)
		if i&3 == 0 {
			continue
		}
		b.palette[i] = dkColour((hi>>1)&7, ((hi&1)<<2)|((lo>>2)&3), lo&3)
	}
}

func (b *dkong) Reset() {
	b.cpu.Reset()
	b.snd.Reset()
	b.ram, b.spriteRam, b.vram = [0x1000]uint8{}, [0x400]uint8{}, [0x400]uint8{}
	b.latch259, b.misc, b.soundLatch, b.gfxBank = 0, 0, 0x0f, 0
	b.sndIrq, b.sndP2, b.dac = false, 0xff, 0
	b.dma, b.dmaFlip, b.dmaMode = [8]uint16{}, 0, 0
	b.cyc, b.sndCyc, b.sndAcc, b.sndAccN, b.samplePos = 0, 0, 0, 0, 0
	b.env = 1
	b.filt = dkFilter(b.sampleRate)
	// effect times off (-1), phases 0
	b.fx = [12]float64{-1, -1, 0, -1, 0, -1, 0, 125, 0, 0, 0, 0}
	b.noise = 1
	for i := range b.screen {
		b.screen[i] = 0
	}
}

func (b *dkong) SetInput(p []Pad) {
	b.p1, b.p2 = Pad{}, Pad{}
	if p[0] != nil {
		b.p1 = p[0]
	}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

func dkJoy(i Pad) uint8 {
	return uint8(bit(i["Right"], 1) | bit(i["Left"], 2) | bit(i["Up"], 4) | bit(i["Down"], 8) | bit(i["Jump"], 0x10))
}

func (b *dkong) read(a int) uint8 {
	switch {
	case a < len(b.rom):
		return b.rom[a]
	case a >= 0x6000 && a < 0x7000:
		return b.ram[a-0x6000]
	case a >= 0x7000 && a < 0x7400:
		return b.spriteRam[a-0x7000]
	case a >= 0x7400 && a < 0x7800:
		return b.vram[a-0x7400]
	}
	switch a & 0xff80 {
	case 0x7c00:
		return dkJoy(b.p1)
	case 0x7c80:
		return dkJoy(b.p2)
	case 0x7d00:
		i := b.p1
		return uint8(bit(i["Service"], 1) | bit(i["Start (1P)"], 4) | bit(i["Start (2P)"], 8) | bit(i["Coin"], 0x80) |
			bit(b.snd.Port(2)&0x10 == 0, 0x40)) // the sound processor's "busy" line
	case 0x7d80:
		d := b.dips
		return uint8(d["lives"]&3 | (d["bonus"]&3)<<2 | (d["coins"]&7)<<4 | bit(d["cabinet"] == 0, 0x80))
	}
	return 0xff
}

func (b *dkong) write(a int, v uint8) {
	switch {
	case a >= 0x6000 && a < 0x7000:
		b.ram[a-0x6000] = v
	case a >= 0x7000 && a < 0x7400:
		b.spriteRam[a-0x7000] = v
	case a >= 0x7400 && a < 0x7800:
		b.vram[a-0x7400] = v
	case a >= 0x7800 && a < 0x7810:
		b.dmaWrite(a&0xf, int(v))
	case a == 0x7c00:
		b.soundLatch = int(^v & 0x0f)
	case a == 0x7c80:
		if b.jr {
			b.gfxBank = int(v & 1)
		}
	case a >= 0x7d00 && a < 0x7d08:
		bt, old := 1<<(a&7), b.latch259
		if v&1 != 0 {
			b.latch259 = old | bt
		} else {
			b.latch259 = old &^ bt
		}
		b.trigger(old, b.latch259)
	case a >= 0x7d80 && a < 0x7d88:
		bt, old := 1<<(a&7), b.misc
		if v&1 != 0 {
			b.misc = old | bt
		} else {
			b.misc = old &^ bt
		}
		if bt == 1 {
			b.sndIrq = v&1 != 0
		}
		if bt == 0x20 && v&1 != 0 && old&0x20 == 0 {
			b.runDma()
		}
	}
}

// The 8257 DMA controller: two writes (low, high) per address/count register,
// then a request copies channel 0's memory to channel 1's.
func (b *dkong) dmaWrite(r, v int) {
	if r == 8 {
		b.dmaFlip, b.dmaMode = 0, v
		return
	}
	if r > 7 {
		return
	}
	cur := int(b.dma[r])
	if b.dmaFlip != 0 {
		b.dma[r] = uint16(cur&0xff | v<<8)
	} else {
		b.dma[r] = uint16(cur&0xff00 | v)
	}
	b.dmaFlip ^= 1
}

func (b *dkong) runDma() {
	src, dst, n := int(b.dma[0]), int(b.dma[2]), int(b.dma[1]&0x3fff)+1
	for i := 0; i < n; i++ {
		b.write((dst+i)&0xffff, b.read((src+i)&0xffff))
	}
}

func (b *dkong) drawFrame() {
	raster, vram, bank := b.raster, &b.vram, (b.misc>>6)&3
	tbank := b.gfxBank * 256
	for row := 2; row < 30; row++ {
		for col := 0; col < 32; col++ {
			code := int(vram[row*32+col]) + tbank
			colour := (int(b.tileColour[col+32*(row>>2)]&0x0f) + 16*bank) * 4
			g := code * 64
			for y := 0; y < 8; y++ {
				o, t := (row*8+y)*dkW+col*8, g+y*8
				for x := 0; x < 8; x++ {
					if p := b.tiles[t+x]; p != 0 {
						raster[o+x] = uint8(colour + int(p))
					} else {
						raster[o+x] = 0
					}
				}
			}
		}
	}
	sr := &b.spriteRam
	base := 0
	if b.misc&0x08 != 0 {
		base = 0x200
	}
	for o := base; o < base+0x180; o += 4 {
		yb := int(sr[o])
		if yb == 0 {
			continue
		}
		code, flipY, flipX := int(sr[o+1]&0x7f), sr[o+1]&0x80 != 0, sr[o+2]&0x80 != 0
		colour := (int(sr[o+2]&0x0f) + 16*bank) * 4
		sx, sy, g := int(sr[o+3])-8, 247-yb, code*256
		for y := 0; y < 16; y++ {
			ry := sy + y
			if ry < dkTop || ry >= dkTop+dkH {
				continue
			}
			srcY := y
			if flipY {
				srcY = 15 - y
			}
			for x := 0; x < 16; x++ {
				rx := (sx + x) & 0xff
				xx := x
				if flipX {
					xx = 15 - x
				}
				if p := b.sprites[g+srcY*16+xx]; p != 0 {
					raster[ry*dkW+rx] = uint8(colour + int(p))
				}
			}
		}
	}
	pal, scr, flip := &b.palette, b.screen, b.misc&0x04 == 0
	for y := 0; y < dkH; y++ {
		src := (y + dkTop) * dkW
		for x := 0; x < dkW; x++ {
			c := raster[src+x]
			px := uint32(0)
			if c != 0 {
				px = pal[c]
			}
			if flip {
				scr[(dkH-1-y)*dkW+(dkW-1-x)] = px
			} else {
				scr[y*dkW+x] = px
			}
		}
	}
}

// ---- sound ----

// dkBiquad is one filter section, made from its analogue form by the
// bilinear transform.
type dkBiquad struct{ b0, b1, b2, a1, a2, x1, x2, y1, y2 float64 }

func (f *dkBiquad) run(x float64) float64 {
	y := f.b0*x + f.b1*f.x1 + f.b2*f.x2 - f.a1*f.y1 - f.a2*f.y2
	f.x2, f.x1, f.y2, f.y1 = f.x1, x, f.y1, y
	return y
}

// dkPole1: a first-order low-pass (or high-pass) at fc Hz.
func dkPole1(fc, rate float64, high bool) dkBiquad {
	k := math.Tan(math.Pi * fc / rate)
	n := 1 / (1 + k)
	if high {
		return dkBiquad{b0: n, b1: -n, a1: (k - 1) * n}
	}
	return dkBiquad{b0: k * n, b1: k * n, a1: (k - 1) * n}
}

// dkPole2: a second-order low-pass at fc Hz with the given Q.
func dkPole2(fc, q, rate float64) dkBiquad {
	k := math.Tan(math.Pi * fc / rate)
	k2 := k * k
	n := 1 / (1 + k/q + k2)
	return dkBiquad{b0: k2 * n, b1: 2 * k2 * n, b2: k2 * n, a1: 2 * (k2 - 1) * n, a2: (1 - k/q + k2) * n}
}

// dkFilter: the DAC's output stage, measured: a band-pass with its peak
// around 150 Hz that falls away steeply above 1 kHz.
func dkFilter(rate float64) [5]dkBiquad {
	return [5]dkBiquad{dkPole1(88, rate, true), dkPole1(272, rate, false), dkPole1(831, rate, false), dkPole2(1837, 0.81, rate), {b0: -2.96}}
}

// trigger starts the effects whose latch outputs changed: on Donkey Kong a
// rising edge starts walk, jump and stomp (and the walk's falling edge its
// second half); on Junior a falling edge starts each, and output 7 picks the
// climbing sound instead of the walking one.
func (b *dkong) trigger(old, now int) {
	F := &b.fx
	up, down := now&^old, old&^now
	if !b.jr {
		if up&1 != 0 {
			F[0], F[1] = 0, -1
		}
		if down&1 != 0 && F[0] >= 0 {
			F[1] = 0
		}
		if up&2 != 0 {
			F[3] = 0
		}
		if up&4 != 0 {
			F[5] = 0
		}
		return
	}
	if down&1 != 0 {
		F[0], F[1], F[2] = 0, 0, float64((now>>7)&1)
	}
	if down&2 != 0 {
		F[3], F[4] = 0, 0
	}
	if down&4 != 0 {
		F[5], F[6], F[8] = 0, 0, 0
	}
}

// rand: the next value of a 16-bit noise register, 0 or 1.
func (b *dkong) rand() float64 {
	n := b.noise
	bt := (n ^ (n >> 2) ^ (n >> 3) ^ (n >> 5)) & 1
	b.noise = (n >> 1) | (bt << 15)
	return float64(n & 1)
}

// frac: the fraction part (x - floor x).
func frac(x float64) float64 { return x - math.Floor(x) }

const tau = 2 * math.Pi

// effects: one sample of the analogue sound effects. fx holds, for each
// effect, its time since it started (-1 = off) and its phases.
func (b *dkong) effects(dt float64) float64 {
	if b.jr {
		return b.jrEffects(dt)
	}
	F := &b.fx
	s := 0.0
	// walk: a low "boop" that falls in pitch, wobbling, and a quieter higher
	// one when the trigger lets go
	if t := F[0]; t >= 0 {
		f, a := 0.0, 0.0
		if r := F[1]; r < 0 {
			rise := math.Min(1, math.Max(0, (t-0.035)/0.01))
			f, a = 272+290*math.Exp(-t/0.005)+90*rise*rise, 0.26*math.Exp(-math.Max(0, t-0.004)/0.024)
		} else {
			f, a = 585*(1+0.19*math.Sin(tau*(t-0.06)/0.08)), 0.035
			if r > 0.03 {
				a *= math.Exp(-(r - 0.03) / 0.023)
			}
			F[1] = r + dt
			if r > 0.3 {
				F[0] = -1
			}
		}
		F[2] = frac(F[2] + f*dt)
		s += a * math.Sin(tau*F[2])
		F[0] = t + dt
	}
	// jump: a tone wobbling between about 330 and 475 Hz for half a second
	if t := F[3]; t >= 0 {
		u := frac((t - 0.068) / 0.112)
		tri := 2 * u
		if u >= 0.5 {
			tri = 2 - 2*u
		}
		a := 0.09 * (1 - math.Exp(-t/0.008))
		if t > 0.27 {
			a *= math.Exp(-(t - 0.27) / 0.12)
		}
		F[4] = frac(F[4] + (333+142*tri)*dt)
		s += a * math.Sin(tau*F[4])
		F[3] = t + dt
		if t > 0.51 {
			F[3] = -1
		}
	}
	// stomp: a deep rumble around 125 Hz whose pitch jumps about every cycle
	if t := F[5]; t >= 0 {
		a := 0.4 * math.Exp(-1.95*t-3.17*t*t) * (1 - math.Exp(-t/0.008))
		if t > 0.62 {
			a *= math.Exp(-(t - 0.62) / 0.03)
		}
		ph := F[6] + F[7]*dt
		if ph >= 1 {
			ph--
			F[7] = 125 * (0.8 + 0.4*(b.rand()*0.5+b.rand()*0.25+b.rand()*0.125+b.rand()*0.125))
		}
		F[6] = ph
		s += a * math.Sin(tau*ph)
		F[5] = t + dt
		if t > 0.7 {
			F[5] = -1
		}
	}
	return s
}

// jrEffects: Junior's walk/climb, jump and fall.
func (b *dkong) jrEffects(dt float64) float64 {
	F := &b.fx
	s := 0.0
	// walk (a short buzz) or climb (a short falling tone): 55 ms
	if t := F[0]; t >= 0 {
		a := 1.0
		if t > 0.055 {
			a = math.Exp(-(t - 0.055) / 0.004)
		}
		if F[2] != 0 {
			f := 320 * math.Exp(-t/0.064)
			F[1] = frac(F[1] + f*dt)
			s += 0.1 * a * (math.Sin(tau*F[1]) + 0.25*math.Sin(2*tau*F[1]))
		} else {
			F[1] += (b.rand()*2 - 1 - F[1]) * 0.5
			s += 0.1 * a * F[1]
		}
		F[0] = t + dt
		if t > 0.08 {
			F[0] = -1
		}
	}
	// jump: a wobbling tone around 600 Hz that dies away, then a quiet tail
	if t := F[3]; t >= 0 {
		a := 0.085 * math.Exp(-t/0.175)
		if t > 0.265 {
			a = 0.025 * math.Exp(-(t-0.265)/0.3)
		}
		f := 600 * (1 + 0.1*math.Sin(tau*t/0.045))
		F[4] = frac(F[4] + f*dt)
		x := math.Sin(tau*F[4]) + 0.2*math.Sin(3*tau*F[4])
		s += a * x
		F[3] = t + dt
		if t > 1.3 {
			F[3] = -1
		}
	}
	// fall: a low noisy crash
	if t := F[5]; t >= 0 {
		a := 0.7 * math.Exp(-t/0.1)
		if t > 0.22 {
			a += 0.1 * math.Exp(-(t-0.22)/0.12)
		}
		F[6] += (b.rand()*2 - 1 - F[6]) * 0.06
		F[8] += (F[6] - F[8]) * 0.06
		s += a * F[8] * 6
		F[5] = t + dt
		if t > 0.8 {
			F[5] = -1
		}
	}
	return s
}

// runSound runs the sound processor up to the main processor's time.
func (b *dkong) runSound(untilCyc int) {
	target := float64(untilCyc) * dkSndHz / dkCPUHz
	perSample, dt := dkSndHz/b.sampleRate, 1/b.sampleRate
	for b.sndCyc < target {
		c := b.snd.Step()
		b.sndCyc += float64(c)
		// the DAC's volume: full while P2 bit 7 is high, fading away while it's low
		on, tau := 0.0, 0.1
		if b.sndP2&0x80 != 0 {
			on, tau = 1, 0.002
		}
		b.env += (on - b.env) * (1 - math.Exp(-float64(c)/dkSndHz/tau))
		b.sndAcc += float64(b.dac) / 255 * b.env * float64(c)
		b.sndAccN += c
		b.samplePos += float64(c)
		if b.samplePos >= perSample {
			b.samplePos -= perSample
			v := b.sndAcc / float64(b.sndAccN)
			b.sndAcc, b.sndAccN = 0, 0
			for i := range b.filt {
				v = b.filt[i].run(v)
			}
			v += b.effects(dt)
			b.audio = append(b.audio, float32(math.Max(-1, math.Min(1, v))))
		}
	}
}

func (b *dkong) Screen() []uint32 { return b.screen }

func (b *dkong) RunFrame() {
	cpu := b.cpu
	done := 0
	for line := 0; line < dkLines; line++ {
		if line == dkTop+dkH {
			b.drawFrame()
			if b.misc&0x10 != 0 {
				cpu.NMI()
			}
		}
		end := (line + 1) * dkLineCyc
		for b.cyc < end {
			b.cyc += cpu.Step()
		}
		b.runSound(b.cyc)
		done = end
	}
	b.cyc -= done
	b.sndCyc -= float64(done) * dkSndHz / dkCPUHz
}

func (b *dkong) TakeAudio() []float32 {
	out := append([]float32(nil), b.audio...)
	b.audio = b.audio[:0]
	return out
}

func (b *dkong) SaveState() ([]byte, error) {
	w := &StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.B = b.snd.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.spriteRam[:])
	w.Bytes(b.vram[:])
	w.U16s(b.dma[:])
	w.F64s(b.fx[:])
	w.Ints([]int{b.latch259, b.misc, b.soundLatch, b.sndP2, b.dac, b.dmaFlip, b.dmaMode, b.cyc, b.sndAccN, b.noise, b.gfxBank})
	w.Bool(b.sndIrq)
	w.F64s([]float64{b.sndCyc, b.sndAcc, b.samplePos, b.env})
	for i := range b.filt {
		f := &b.filt[i]
		w.F64s([]float64{f.x1, f.x2, f.y1, f.y2})
	}
	return w.B, nil
}

func (b *dkong) LoadState(s []byte) error {
	rest, err := b.cpu.LoadState(s)
	if err != nil {
		return err
	}
	if rest, err = b.snd.LoadState(rest); err != nil {
		return err
	}
	r := &StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.spriteRam[:])
	r.Bytes(b.vram[:])
	r.U16s(b.dma[:])
	r.F64s(b.fx[:])
	m := make([]int, 11)
	r.Ints(m)
	b.latch259, b.misc, b.soundLatch, b.sndP2, b.dac, b.dmaFlip, b.dmaMode, b.cyc, b.sndAccN, b.noise, b.gfxBank = m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10]
	b.sndIrq = r.Bool()
	f := make([]float64, 4)
	r.F64s(f)
	b.sndCyc, b.sndAcc, b.samplePos, b.env = f[0], f[1], f[2], f[3]
	for i := range b.filt {
		r.F64s(f)
		b.filt[i].x1, b.filt[i].x2, b.filt[i].y1, b.filt[i].y2 = f[0], f[1], f[2], f[3]
	}
	if err := r.Done(); err != nil {
		return err
	}
	b.audio = b.audio[:0]
	b.drawFrame()
	return nil
}

var dkCoins = []DipOption{{"1 coin, 1 game", 0}, {"2 coins, 1 game", 1}, {"1 coin, 2 games", 2}, {"3 coins, 1 game", 3},
	{"1 coin, 3 games", 4}, {"4 coins, 1 game", 5}, {"1 coin, 4 games", 6}, {"5 coins, 1 game", 7}}

var dkDips = []Dip{
	{ID: "lives", Label: "Lives", Default: 0, Options: []DipOption{{"3", 0}, {"4", 1}, {"5", 2}, {"6", 3}}},
	{ID: "bonus", Label: "Extra life at", Default: 0, Options: []DipOption{{"7,000", 0}, {"10,000", 1}, {"15,000", 2}, {"20,000", 3}}},
	{ID: "coins", Label: "Coins", Default: 0, Options: dkCoins},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []DipOption{{"Upright", 0}, {"Cocktail table", 1}}},
}

var dkJrDips = []Dip{
	{ID: "lives", Label: "Lives", Default: 0, Options: []DipOption{{"3", 0}, {"4", 1}, {"5", 2}, {"6", 3}}},
	{ID: "bonus", Label: "Extra life at", Default: 0, Options: []DipOption{{"10,000", 0}, {"15,000", 1}, {"20,000", 2}, {"25,000", 3}}},
	{ID: "coins", Label: "Coins", Default: 0, Options: dkCoins},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []DipOption{{"Upright", 0}, {"Cocktail table", 1}}},
}

func init() {
	Register(&BoardDef{
		ID: "dkong", Title: "Nintendo Donkey Kong", Width: dkW, Height: dkH, Rotate: 90, FPS: dkFPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Jump", "Coin", "Start (1P)", "Start (2P)", "Service"},
		Dips:    dkDips,
		Games: []Game{
			{ID: "dkong", Title: "Donkey Kong", Year: 1981, Maker: "Nintendo", Files: []File{
				{Name: "c_5et_g.bin", Size: 0x1000, Role: "cpu", Offset: 0x0000},
				{Name: "c_5ct_g.bin", Size: 0x1000, Role: "cpu", Offset: 0x1000},
				{Name: "c_5bt_g.bin", Size: 0x1000, Role: "cpu", Offset: 0x2000},
				{Name: "c_5at_g.bin", Size: 0x1000, Role: "cpu", Offset: 0x3000},
				{Name: "s_3i_b.bin", Size: 0x800, Role: "sound", Offset: 0x0000},
				{Name: "s_3j_b.bin", Size: 0x800, Role: "sound", Offset: 0x1000},
				{Name: "v_5h_b.bin", Size: 0x800, Role: "tiles", Offset: 0x0000},
				{Name: "v_3pt.bin", Size: 0x800, Role: "tiles", Offset: 0x0800},
				{Name: "l_4m_b.bin", Size: 0x800, Role: "sprites", Offset: 0x0000},
				{Name: "l_4n_b.bin", Size: 0x800, Role: "sprites", Offset: 0x0800},
				{Name: "l_4r_b.bin", Size: 0x800, Role: "sprites", Offset: 0x1000},
				{Name: "l_4s_b.bin", Size: 0x800, Role: "sprites", Offset: 0x1800},
				{Name: "c-2k.bpr", Size: 0x100, Role: "prom", Offset: 0x000},
				{Name: "c-2j.bpr", Size: 0x100, Role: "prom", Offset: 0x100},
				{Name: "v-5e.bpr", Size: 0x100, Role: "prom", Offset: 0x200},
			}},
			{ID: "dkongjr", Title: "Donkey Kong Junior", Year: 1982, Maker: "Nintendo", Dips: dkJrDips, Files: []File{
				{Name: "dkj.5b", Alt: []string{"djr1-c_5b_f-2.5b"}, Size: 0x2000, Role: "cpu", Offset: 0x0000},
				{Name: "dkj.5c", Alt: []string{"djr1-c_5c_f-2.5c"}, Size: 0x2000, Role: "cpu", Offset: 0x2000},
				{Name: "dkj.5e", Alt: []string{"djr1-c_5e_f-2.5e"}, Size: 0x2000, Role: "cpu", Offset: 0x4000},
				{Name: "c_3h.bin", Alt: []string{"djr1-c_3h.3h"}, Size: 0x1000, Role: "sound", Offset: 0x0000},
				{Name: "dkj.3n", Alt: []string{"djr1-v.3n"}, Size: 0x1000, Role: "tiles", Offset: 0x0000},
				{Name: "dkj.3p", Alt: []string{"djr1-v.3p"}, Size: 0x1000, Role: "tiles", Offset: 0x1000},
				{Name: "v_7c.bin", Alt: []string{"djr1-v_7c.7c"}, Size: 0x800, Role: "sprites", Offset: 0x0000},
				{Name: "v_7d.bin", Alt: []string{"djr1-v_7d.7d"}, Size: 0x800, Role: "sprites", Offset: 0x0800},
				{Name: "v_7e.bin", Alt: []string{"djr1-v_7e.7e"}, Size: 0x800, Role: "sprites", Offset: 0x1000},
				{Name: "v_7f.bin", Alt: []string{"djr1-v_7f.7f"}, Size: 0x800, Role: "sprites", Offset: 0x1800},
				{Name: "c-2e.bpr", Alt: []string{"djr1-c-2e.2e"}, Size: 0x100, Role: "prom", Offset: 0x000},
				{Name: "c-2f.bpr", Alt: []string{"djr1-c-2f.2f"}, Size: 0x100, Role: "prom", Offset: 0x100},
				{Name: "v-2n.bpr", Alt: []string{"djr1-v-2n.2n"}, Size: 0x100, Role: "prom", Offset: 0x200},
			}},
		},
		New: newDkong,
	})
}
