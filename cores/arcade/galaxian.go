package arcade

// Namco Galaxian hardware: a port of web/emu/arcade_galaxian.js.
//
// One Z80 at 3.072 MHz with up to 16 KB of program ROM, 1 KB of work RAM, a
// 32x32 tile screen where each tile column has its own scroll and colour, 8
// hardware sprites, 8 bullets drawn by the hardware itself, a twinkling
// starfield, and colours from one 32-byte PROM. The interrupt is the NMI at
// the start of vertical blank. The monitor is on its side: the board draws
// 256x224 and the framework turns the picture upright. The sound is
// RetroPixels' own models of the board's analog circuits, their rates, shapes
// and levels measured from how the board sounds.

import (
	"math"

	"retropixels/cores/z80"
)

const (
	gxCPUHz   = 3072000
	gxLineCyc = 192
	gxLines   = 264
	gxFirst   = 16
	gxLast    = 240
	gxW       = 256
	gxH       = gxLast - gxFirst
)

var gxFPS = float64(gxCPUHz) / (gxLineCyc * gxLines)

// The starfield: a 17-bit shift register stepping twice a pixel, 512 steps a
// line, without stopping from frame to frame (so the field drifts one half
// pixel a frame). A star shows where its output has been 0 for 8 steps and
// then 1; the 6 bits before those give the star's colour. Stars on every
// other 8-pixel stretch show, alternating line by line. (Worked out from how
// the board's stars look.)
const (
	gxStarPeriod = 1<<17 - 1
	gxStarT0     = 79139 // where the register is at the first line of the first frame
)

// gxStarAt: each step's star colour (0 = none).
var gxStarAt = func() []uint8 {
	bits := make([]uint8, gxStarPeriod)
	r := 1
	for t := range bits {
		bits[t] = uint8(r & 1)
		r = r>>1 | ((r^r>>12)&1)<<16
	}
	at := make([]uint8, gxStarPeriod)
	for t := range at {
		ok := bits[(t+8)%gxStarPeriod] == 1
		for j := 0; j < 8 && ok; j++ {
			ok = bits[(t+j)%gxStarPeriod] == 0
		}
		if !ok {
			continue
		}
		c := 0
		for k := 0; k < 6; k++ {
			c |= int(bits[(t-1-k+gxStarPeriod)%gxStarPeriod]) << k
		}
		at[t] = uint8(c)
	}
	return at
}()

var gxStarLevels = [4]int{0x00, 0xc2, 0xd6, 0xff}

// ---- sound: RetroPixels' own models of the board's sound circuits, with
// their rates, shapes and levels measured from how they sound ----

// The tune: the pitch register sets the rate of a 16-step counter, and the two
// volume bits mix its outputs into one of four waveforms.
var gxToneTable = [4][16]float64{
	{8.8, 41.3, 8.0, 40.5, -41.7, -8.4, -40.9, -7.6, 8.8, 41.3, 8.0, 40.5, -41.7, -8.4, -40.9, -7.6},
	{-125.1, -81.1, -121.7, -77.8, 82.9, 123.4, 79.4, 120.1, -125.1, -81.1, -121.7, -77.8, 82.9, 123.4, 79.4, 120.1},
	{-4.9, 41.3, -5.5, 40.6, 63.0, 108.1, 60.2, 105.3, -113.0, -65.0, -110.0, -62.1, -38.1, 8.6, -37.6, 9.1},
	{-147.8, -109.8, -143.6, -105.6, 31.2, 66.3, 29.6, 64.7, -64.4, -27.8, -62.9, -26.2, 109.3, 143.0, 105.1, 138.9},
}

const gxToneClock = 1536000.0 // counter steps a second times (256 - pitch)

// The three background voices: oscillators whose low part lasts a fixed time
// and whose high part a slow sweep (shared by the three, its speed set by
// 6004-6007) stretches, so each falls in pitch to about half and snaps back.
var (
	gxFSTop   = [3]float64{238.6, 334.2, 483.6}        // Hz at the sweep's start
	gxFSDepth = [3]float64{0.482, 0.487, 0.518}        // how far it falls
	gxFSLow   = [3]float64{3.22e-3, 2.27e-3, 1.515e-3} // s
	gxLFOBits = [4]float64{34.8, 73.5, 160, 351.5}
)

const (
	gxFSLevel = 0.138
	gxFSSnap  = 0.018 // s the sweep takes to snap back
	gxFSHold  = 0.008 // s of it the voices stay low
	gxHitClk  = 9000.0
	gxFireClk = 7920.0 // the shot's pulse clock
	gxFireSq  = 7700.0 // its unsteady square's mean edge rate
	gxFireOn  = 0.036  // s the shot takes to start
	gxFireOff = 0.078  // s it keeps going after 6805 goes off
)

type gxSound struct {
	tone          float64    // tone counter position (steps, 0-16)
	lfo           float64    // sweep: 0-1 falling, 1-2 snapping back
	fsPh          [3]float64 // time into each voice's cycle
	fsLP, fsHP    float64
	fsHPin        float64
	hitEnv, hitPh float64
	hitBit        float64
	bp            [4]float64 // band-pass state
	fireT         float64    // time since the shot started or stopped
	fireSince     float64    // time since 6805 changed
	fireOn        float64    // 1 while the shot sounds
	fireLvl       float64    // how loud its fading part starts: charges while 6805 is on
	firePh        float64    // pulse clock position
	fireSqPh      float64    // time to the square's next edge
	fireBit       float64
	firePulse     float64 // time left of the current pulse
	fireX, fireY  float64
	due           float64
	noise, fnoise int // the noise registers
}

// floats: the sound's state, for save states.
func (s *gxSound) floats() []*float64 {
	return []*float64{&s.tone, &s.lfo, &s.fsPh[0], &s.fsPh[1], &s.fsPh[2], &s.fsLP, &s.fsHP, &s.fsHPin, &s.hitEnv, &s.hitPh,
		&s.hitBit, &s.bp[0], &s.bp[1], &s.bp[2], &s.bp[3], &s.fireT, &s.fireSince, &s.fireOn, &s.fireLvl, &s.firePh, &s.fireSqPh,
		&s.fireBit, &s.firePulse, &s.fireX, &s.fireY, &s.due}
}

// gxNoise steps a 17-bit noise register and returns its new low bit.
func gxNoise(r *int) int {
	n := *r
	bit := (n ^ n>>5) & 1
	*r = n>>1 | bit<<16
	return *r & 1
}

type galaxian struct {
	dips       map[string]int
	sampleRate float64
	rom        []byte
	tiles      []uint8
	sprites    []uint8
	pens       [32]uint32
	starPens   [64]uint32
	ram, vram  [0x400]uint8
	obj        [0x100]uint8
	l60, l68   [8]uint8
	l70        [8]uint8
	screen     []uint32
	line       [gxW]bool
	p1, p2     Pad
	audio      []float32
	cpu        *z80.CPU
	pitch      int
	cycles     int
	frame      int
	snd        gxSound
}

type gxBus struct{ b *galaxian }

func (x gxBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x gxBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }
func (x gxBus) In(uint16) uint8         { return 0xff }
func (x gxBus) Out(uint16, uint8)       {}

func newGalaxian(g *Game, files Files, opts Opts) (Board, error) {
	b := &galaxian{dips: opts.Dips, sampleRate: float64(opts.SampleRate)}
	b.rom = Region(g, files, "cpu", 0x4000)
	gfx := Region(g, files, "gfx", 0x1000)
	prom := Region(g, files, "colour", 32)
	b.decodeGraphics(gfx)
	for i := range b.pens {
		b.pens[i] = gxColour(prom[i])
	}
	for i := range b.starPens {
		b.starPens[i] = RGB(gxStarLevels[i&3], gxStarLevels[(i>>2)&3], gxStarLevels[i>>4])
	}
	b.screen = make([]uint32, gxW*gxH)
	b.p1, b.p2 = Pad{}, Pad{}
	b.audio = make([]float32, 0, 2048)
	b.cpu = z80.New(gxBus{b})
	b.Reset()
	return b, nil
}

func (b *galaxian) Reset() {
	b.cpu.Reset()
	b.ram, b.vram, b.obj = [0x400]uint8{}, [0x400]uint8{}, [0x100]uint8{}
	b.l60, b.l68, b.l70 = [8]uint8{}, [8]uint8{}, [8]uint8{}
	b.pitch, b.cycles, b.frame = 0xff, 0, 0
	b.snd = gxSound{noise: 1, fnoise: 0x1234, fireT: 10}
}

// gxColour: one PROM colour, 3 bits each of red and green and 2 of blue,
// through the board's resistors (the levels measured).
func gxColour(c uint8) uint32 {
	bit := func(n uint) int { return int(c>>n) & 1 }
	return RGB(0x1d*bit(0)+0x3e*bit(1)+0x85*bit(2), 0x1d*bit(3)+0x3e*bit(4)+0x85*bit(5), 0x45*bit(6)+0x94*bit(7))
}

// two ROMs: the first gives each pixel's high bit, the second its low bit
func (b *galaxian) decodeGraphics(gfx []byte) {
	half := len(gfx) >> 1
	px := func(o, bit int) uint8 { return ((gfx[o]>>bit)&1)<<1 | (gfx[o+half]>>bit)&1 }
	b.tiles = make([]uint8, 256*64)
	for t := 0; t < 256; t++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				b.tiles[t*64+y*8+x] = px(t*8+y, 7-x)
			}
		}
	}
	b.sprites = make([]uint8, 64*256)
	for s := 0; s < 64; s++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				b.sprites[s*256+y*16+x] = px(s*32+(y&7)+(x&8)+((y&8)<<1), 7-(x&7))
			}
		}
	}
}

var (
	gxWhite  = RGB(0xff, 0xff, 0xff)
	gxYellow = RGB(0xff, 0xff, 0x00)
)

func (b *galaxian) drawLine(y int) {
	scr, row, obj, used := b.screen, (y-gxFirst)*gxW, &b.obj, &b.line
	*used = [gxW]bool{}
	for col := 0; col < 32; col++ {
		ty := (y + int(obj[col*2])) & 0xff
		pal := int(obj[col*2+1]&7) << 2
		t := int(b.vram[((ty>>3)<<5)|col])*64 + (ty&7)*8
		for x := 0; x < 8; x++ {
			p := b.tiles[t+x]
			o := col*8 + x
			scr[row+o] = b.pens[pal|int(p)]
			used[o] = p != 0
		}
	}
	for n := 7; n >= 0; n-- {
		base0 := 0x40 + n*4
		adj := 0
		if n < 3 {
			adj = 1
		}
		sy := (240 - int(obj[base0]) + adj) & 0xff
		r := (y - sy) & 0xff
		if r >= 16 {
			continue
		}
		attr := int(obj[base0+1])
		pal := int(obj[base0+2]&7) << 2
		sx := int(obj[base0+3]) + 1
		yy, fx := r, 0
		if attr&0x80 != 0 {
			yy = 15 - r
		}
		if attr&0x40 != 0 {
			fx = 15
		}
		base := (attr&0x3f)*256 + yy*16
		for x := 0; x < 16; x++ {
			o := sx + x
			if o >= gxW {
				break
			}
			if p := b.sprites[base+(x^fx)]; p != 0 {
				scr[row+o] = b.pens[pal|int(p)]
				used[o] = true
			}
		}
	}
	shell, missile := -1, -1
	for n := 0; n < 8; n++ {
		match := y
		if n < 3 {
			match = y - 1
		}
		match &= 0xff
		if (int(obj[0x60+n*4+1])+match)&0xff != 0xff {
			continue
		}
		if n == 7 {
			missile = n
		} else {
			shell = n
		}
	}
	for _, n := range [2]int{shell, missile} {
		if n < 0 {
			continue
		}
		x0 := 255 - int(obj[0x60+n*4+3]) - 4
		c := gxWhite
		if n == 7 {
			c = gxYellow
		}
		for x := x0; x < x0+4; x++ {
			if x >= 0 && x < gxW {
				scr[row+x] = c
				used[x] = true
			}
		}
	}
	if b.l70[4] != 0 {
		line := y - gxFirst
		t := ((gxStarT0-b.frame+line*512)%gxStarPeriod + gxStarPeriod) % gxStarPeriod
		for h := 0; h < 512; h++ {
			if c := gxStarAt[t]; c != 0 && (line^(h>>4))&1 != 0 && !used[h>>1] {
				scr[row+(h>>1)] = b.starPens[c]
			}
			if t++; t == gxStarPeriod {
				t = 0
			}
		}
	}
}

func (b *galaxian) flipPicture() {
	s := b.screen
	if b.l70[6] != 0 {
		for y := 0; y < gxH; y++ {
			r := s[y*gxW : y*gxW+gxW]
			for i, j := 0, gxW-1; i < j; i, j = i+1, j-1 {
				r[i], r[j] = r[j], r[i]
			}
		}
	}
	if b.l70[7] != 0 {
		tmp := make([]uint32, gxW)
		for y := 0; y < gxH>>1; y++ {
			copy(tmp, s[y*gxW:y*gxW+gxW])
			copy(s[y*gxW:], s[(gxH-1-y)*gxW:(gxH-y)*gxW])
			copy(s[(gxH-1-y)*gxW:], tmp)
		}
	}
}

// gxOverlap: how much of [a, b) lies in [0, h).
func gxOverlap(a, b, h float64) float64 {
	return math.Max(0, math.Min(b, h)-math.Max(a, 0))
}

func (b *galaxian) makeSound(n int) {
	st, l68 := &b.snd, &b.l68
	rate := b.sampleRate
	dt := 1 / rate
	// the tune
	steps := 0.0
	if b.pitch != 0xff {
		steps = gxToneClock / float64(256-b.pitch) * dt // counter steps a sample
	}
	table := &gxToneTable[int(l68[6])|int(l68[7])<<1]
	// the sweep's speed
	sweep := 709.6
	for i := 0; i < 4; i++ {
		sweep -= gxLFOBits[i] * float64(b.l60[4+i])
	}
	sweep /= 115
	lpK := 1 - math.Exp(-2*math.Pi*500*dt)
	hpK := math.Exp(-dt / 0.013)
	// the hit's band-pass (165 Hz, Q 2.75)
	k := math.Tan(math.Pi * 165 * dt)
	nrm := 1 / (1 + k/2.75 + k*k)
	b0, a1, a2 := k/2.75*nrm, 2*(k*k-1)*nrm, (1-k/2.75+k*k)*nrm
	hitUp, hitDown := 1-math.Exp(-dt/0.05), math.Exp(-dt/0.85)
	fireHP := math.Exp(-2 * math.Pi * 8870 * dt)
	fireUp := 1 - math.Exp(-dt/0.08)
	for i := 0; i < n; i++ {
		v := 0.0
		// the tune: the table averaged over the sample
		if steps > 0 {
			if steps >= 16 {
				st.tone = math.Mod(st.tone+steps, 16)
			} else {
				acc, left, p := 0.0, steps, st.tone
				for left > 0 {
					idx := int(p)
					take := math.Min(left, float64(idx+1)-p)
					acc += table[idx&15] * take
					left -= take
					p += take
					if p >= 16 {
						p -= 16
					}
				}
				st.tone = p
				v += acc / steps / 1000
			}
		}
		// the background voices
		u := st.lfo
		if u < 1 {
			st.lfo += sweep * dt
		} else {
			u = 2 - u
			st.lfo += dt / gxFSSnap
			if st.lfo >= 2 {
				st.lfo -= 2
			}
		}
		x := 0.0
		hold := st.lfo >= 1 && (st.lfo-1)*gxFSSnap < gxFSHold // the voices stay low as the sweep snaps back
		for j := 0; j < 3; j++ {
			if l68[j] == 0 || hold {
				st.fsPh[j] = 0
				continue
			}
			T := 1 / (gxFSTop[j] * (1 - gxFSDepth[j]*u))
			h := math.Max(0, T-gxFSLow[j])
			p := st.fsPh[j]
			hi := gxOverlap(p, p+dt, h)
			if p += dt; p >= T {
				p -= T
				hi += gxOverlap(0, p, h)
			}
			st.fsPh[j] = p
			x += hi / dt
		}
		st.fsLP += (x*gxFSLevel - st.fsLP) * lpK
		st.fsHP = hpK * (st.fsHP + st.fsLP - st.fsHPin)
		st.fsHPin = st.fsLP
		v += st.fsHP
		// the hit: noise through a band-pass, its level charging while 6803 is on
		if l68[3] != 0 {
			st.hitEnv += (1 - st.hitEnv) * hitUp
		} else {
			st.hitEnv *= hitDown
		}
		{
			p := st.hitPh + gxHitClk*dt
			nx := 0.0
			if p >= 1 {
				frac := (p - math.Floor(p)) / (gxHitClk * dt)
				nx = st.hitBit * (1 - frac)
				for p >= 1 {
					p--
					st.hitBit = float64(gxNoise(&st.noise)*2 - 1)
				}
				nx += st.hitBit * frac
			} else {
				nx = st.hitBit
			}
			st.hitPh = p
			bp := &st.bp
			y := b0*nx - b0*bp[1] - a1*bp[2] - a2*bp[3]
			bp[1], bp[0], bp[3], bp[2] = bp[0], nx, bp[2], y
			lvl := st.hitEnv
			if l68[3] != 0 {
				lvl = 1
			}
			v += y * lvl * 1.4
		}
		// the shot: a short while after 6805 goes on, an unsteady square, then
		// after a quarter second short pulses on a ~7.9 kHz clock; a while
		// after 6805 goes off, the square again, fading; all heard
		// through a steep high-pass
		{
			st.fireSince += dt
			st.fireT += dt
			if l68[5] != 0 {
				st.fireLvl += (1 - st.fireLvl) * fireUp
			}
			if want := float64(l68[5]); want != st.fireOn {
				delay := gxFireOff - 0.075*math.Max(0, 1-st.fireT) // shorter after a short shot
				if want != 0 {
					delay = gxFireOn
				}
				if st.fireSince >= delay {
					st.fireOn, st.fireT = want, 0
				}
			}
			pulses := st.fireOn != 0 && st.fireT >= 0.25
			p := st.firePh + gxFireClk*dt
			for p >= 1 {
				p--
				r := 0
				for q := 0; q < 7; q++ {
					r = r<<1 | gxNoise(&st.fnoise)
				}
				if pulses && r < 51 {
					st.firePulse = 1.25 / 48000
				}
			}
			st.firePh = p
			for st.fireSqPh -= dt; st.fireSqPh <= 0; {
				r := 0
				for q := 0; q < 7; q++ {
					r = r<<1 | gxNoise(&st.fnoise)
				}
				st.fireBit = 1 - st.fireBit
				st.fireSqPh += (0.3 + 1.4*float64(r)/128) / gxFireSq
			}
			fx := 0.0
			switch {
			case pulses:
				fx = math.Min(st.firePulse, dt) / dt
				st.firePulse = math.Max(0, st.firePulse-dt)
			case st.fireOn != 0:
				fx = st.fireBit
			case st.fireT < 3:
				fx = st.fireBit * st.fireLvl * math.Exp(-math.Max(0, st.fireT-0.05)/0.17)
			}
			st.fireY = fireHP * (st.fireY + fx - st.fireX)
			st.fireX = fx
			v += st.fireY
		}
		b.audio = append(b.audio, float32(math.Max(-1, math.Min(1, v))))
	}
}

func (b *galaxian) TakeAudio() []float32 {
	out := append([]float32(nil), b.audio...)
	b.audio = b.audio[:0]
	return out
}

func (b *galaxian) read(a int) uint8 {
	a &= 0x7fff
	switch {
	case a < 0x4000:
		return b.rom[a]
	case a < 0x4800:
		return b.ram[a&0x3ff]
	case a < 0x5000:
		return 0xff
	case a < 0x5800:
		return b.vram[a&0x3ff]
	case a < 0x6000:
		return b.obj[a&0xff]
	case a < 0x6800:
		return b.readIn0()
	case a < 0x7000:
		return b.readIn1()
	case a < 0x7800:
		d := b.dips
		return uint8(d["bonus"]&3 | (d["lives"]&1)<<2)
	}
	return 0xff
}

func (b *galaxian) write(a int, v uint8) {
	a &= 0x7fff
	switch {
	case a < 0x4000:
	case a < 0x4800:
		b.ram[a&0x3ff] = v
	case a < 0x5000:
	case a < 0x5800:
		b.vram[a&0x3ff] = v
	case a < 0x6000:
		b.obj[a&0xff] = v
	case a < 0x6800:
		b.l60[a&7] = v & 1
	case a < 0x7000:
		n := a & 7
		was := b.l68[n]
		b.l68[n] = v & 1
		if n == 5 && v&1 != was {
			b.snd.fireSince = 0
			if v&1 != 0 {
				b.snd.fireLvl = 0
			}
		}
	case a < 0x7800:
		b.l70[a&7] = v & 1
	default:
		b.pitch = int(v)
	}
}

func bit(on bool, v int) int {
	if on {
		return v
	}
	return 0
}

func (b *galaxian) readIn0() uint8 {
	p, d := b.p1, b.dips
	return uint8(bit(p["Coin"], 1) | bit(p["Coin 2"], 2) | bit(p["Left"], 4) | bit(p["Right"], 8) | bit(p["Fire"], 0x10) |
		bit(d["cabinet"] != 0, 0x20) | bit(d["test"] != 0, 0x40) | bit(p["Service"], 0x80))
}

func (b *galaxian) readIn1() uint8 {
	p, q, d := b.p1, b.p2, b.dips
	return uint8(bit(p["Start (1P)"] || q["Start (1P)"], 1) | bit(p["Start (2P)"] || q["Start (2P)"], 2) |
		bit(q["Left"], 4) | bit(q["Right"], 8) | bit(q["Fire"], 0x10) | (d["coins"]&3)<<6)
}

func (b *galaxian) SetInput(p []Pad) {
	b.p1 = p[0]
	b.p2 = Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

func (b *galaxian) Screen() []uint32 { return b.screen }

func (b *galaxian) RunFrame() {
	cpu := b.cpu
	perLine := b.sampleRate / (gxFPS * gxLines)
	for y := 0; y < gxLines; y++ {
		if y == gxLast {
			b.flipPicture()
			if b.l70[1] != 0 {
				cpu.NMI()
			}
		}
		end := b.cycles + gxLineCyc
		for b.cycles < end {
			b.cycles += cpu.Step()
		}
		if y >= gxFirst && y < gxLast {
			b.drawLine(y)
		}
		b.snd.due += perLine
		n := math.Floor(b.snd.due)
		b.snd.due -= n
		b.makeSound(int(n))
	}
	b.cycles -= gxLines * gxLineCyc
	b.frame++
}

func (b *galaxian) SaveState() ([]byte, error) {
	w := &StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.obj[:])
	w.Bytes(b.l60[:])
	w.Bytes(b.l68[:])
	w.Bytes(b.l70[:])
	w.Ints([]int{b.pitch, b.cycles, b.frame, b.snd.noise, b.snd.fnoise})
	for _, f := range b.snd.floats() {
		w.F64s([]float64{*f})
	}
	return w.B, nil
}

func (b *galaxian) LoadState(st []byte) error {
	rest, err := b.cpu.LoadState(st)
	if err != nil {
		return err
	}
	r := &StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.obj[:])
	r.Bytes(b.l60[:])
	r.Bytes(b.l68[:])
	r.Bytes(b.l70[:])
	m := make([]int, 5)
	r.Ints(m)
	b.pitch, b.cycles, b.frame, b.snd.noise, b.snd.fnoise = m[0], m[1], m[2], m[3], m[4]
	f := make([]float64, 1)
	for _, p := range b.snd.floats() {
		r.F64s(f)
		*p = f[0]
	}
	if err := r.Done(); err != nil {
		return err
	}
	b.audio = b.audio[:0]
	for y := gxFirst; y < gxLast; y++ {
		b.drawLine(y)
	}
	b.flipPicture()
	return nil
}

func init() {
	fs := []File{
		{Name: "galmidw.u", Size: 0x800, Role: "cpu", Offset: 0x0000},
		{Name: "galmidw.v", Size: 0x800, Role: "cpu", Offset: 0x0800},
		{Name: "galmidw.w", Size: 0x800, Role: "cpu", Offset: 0x1000},
		{Name: "galmidw.y", Size: 0x800, Role: "cpu", Offset: 0x1800},
		{Name: "7l", Alt: []string{"galmidw.z", "7l.bin"}, Size: 0x800, Role: "cpu", Offset: 0x2000},
		{Name: "1h.bin", Alt: []string{"1h", "galmidw.1j", "galaxian.1j"}, Size: 0x800, Role: "gfx", Offset: 0},
		{Name: "1k.bin", Alt: []string{"1k", "galmidw.1k", "galaxian.1k"}, Size: 0x800, Role: "gfx", Offset: 0x800},
		{Name: "6l.bpr", Alt: []string{"6l", "galaxian.6l"}, Size: 0x20, Role: "colour", Offset: Next},
	}
	Register(&BoardDef{
		ID: "galaxian", Title: "Namco Galaxian", Width: gxW, Height: gxH, Rotate: 90, FPS: gxFPS,
		Buttons: []string{"Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips: []Dip{
			{ID: "coins", Label: "Coins", Default: 0, Options: []DipOption{{"1 coin, 1 game", 0}, {"2 coins, 1 game", 1}, {"1 coin, 2 games", 2}, {"Free play", 3}}},
			{ID: "bonus", Label: "Extra ship at", Default: 0, Options: []DipOption{{"7,000 points", 0}, {"10,000 points", 1}, {"12,000 points", 2}, {"20,000 points", 3}}},
			{ID: "lives", Label: "Ships", Default: 1, Options: []DipOption{{"2", 0}, {"3", 1}}},
			{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []DipOption{{"Upright", 0}, {"Cocktail table", 1}}},
			{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: offOpts},
		},
		Games: []Game{
			{ID: "galaxian", Title: "Galaxian", Year: 1979, Maker: "Namco", Files: fs},
			{ID: "galmidw", Title: "Galaxian (Midway)", Year: 1979, Maker: "Namco (Midway license)", Parent: "galaxian", Files: fs},
		},
		New: newGalaxian,
	})
}
