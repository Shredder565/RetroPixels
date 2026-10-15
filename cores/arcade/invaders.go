package arcade

// The Space Invaders board (Taito, 1978; Midway in America): a port of
// web/emu/arcade_invaders.js. An Intel 8080 at 1.9968 MHz, 8 KB of program
// ROM and 8 KB of RAM (7 KB of it the 1-bit picture, 256x224 on a sideways
// monitor), the external shift register, two interrupts a frame, the
// cabinet's coloured strips, and RetroPixels' own version of each sound.

import (
	"math"

	"retropixels/cores/i8080"
)

const (
	invCPUHz    = 1996800
	invFPS      = 60
	invLines    = 262
	invFrameCyc = invCPUHz / invFPS // 33280
	invW, invH  = 256, 224
	invNV       = 10
)

var (
	invWhite = RGB(0xff, 0xff, 0xff)
	invRed   = RGB(0xff, 0x40, 0x40)
	invGreen = RGB(0x40, 0xff, 0x40)
)

var invMarchHz = [4]float64{98, 87, 78, 69}

// overlayColour: the coloured strips, in the player's view (224 wide, 256 tall).
func invOverlay(dx, dy int) uint32 {
	switch {
	case dy >= 32 && dy < 64:
		return invRed
	case dy >= 184 && dy < 240:
		return invGreen
	case dy >= 240 && dx >= 16 && dx < 134:
		return invGreen
	}
	return invWhite
}

type invaders struct {
	dips       map[string]int
	sampleRate float64
	rom, ram   [0x2000]uint8
	screen     []uint32
	tint       []uint32
	tintOn     int
	p1, p2     Pad
	cpu        *i8080.CPU
	audio      []float32
	voices     [invNV * 4]float64

	shiftData, shiftAmount int
	port3, port5           int
	pendingIrq             int
	noise                  int
	lp, hp, hpPrev         float64
	sampleFrac, ufoLfo     float64
	cyc                    int
}

type invBus struct{ b *invaders }

func (x invBus) Read(a uint16) uint8 {
	a &= 0x3fff // the upper address lines aren't decoded
	if a < 0x2000 {
		return x.b.rom[a]
	}
	return x.b.ram[a-0x2000]
}
func (x invBus) Write(a uint16, v uint8) {
	a &= 0x3fff
	if a >= 0x2000 {
		x.b.ram[a-0x2000] = v
	}
}
func (x invBus) In(p uint8) uint8 { return x.b.portIn(int(p & 7)) }
func (x invBus) Out(p, v uint8)   { x.b.portOut(int(p&7), int(v)) }

var invDips = []Dip{
	{ID: "lives", Label: "Cannons", Default: 0, Options: []DipOption{{"3", 0}, {"4", 1}, {"5", 2}, {"6", 3}}},
	{ID: "bonus", Label: "Extra cannon at", Default: 0, Options: []DipOption{{"1500 points", 0}, {"1000 points", 1}}},
	{ID: "coinInfo", Label: "Coin info on the title screen", Default: 1, Options: []DipOption{{"Shown", 1}, {"Hidden", 0}}},
	{ID: "overlay", Label: "Coloured screen strips", Default: 1, Options: []DipOption{{"On", 1}, {"Off (black and white)", 0}}},
}

func newInvaders(g *Game, files Files, opts Opts) (Board, error) {
	b := &invaders{sampleRate: float64(opts.SampleRate), dips: opts.Dips}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range invDips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	for _, f := range g.Files {
		if d := files[f.Name]; d != nil && f.Role == "cpu" {
			copy(b.rom[max(f.Offset, 0):], d[:min(len(d), f.Size)])
		}
	}
	b.screen = make([]uint32, invW*invH)
	b.tint = make([]uint32, invW*invH)
	b.p1, b.p2 = Pad{}, Pad{}
	b.makeTint()
	b.cpu = i8080.New(invBus{b})
	b.audio = make([]float32, 0, 4096)
	b.Reset()
	return b, nil
}

// the colour of each pixel when lit, in the board's own (unrotated) layout
func (b *invaders) makeTint() {
	b.tintOn = b.dips["overlay"]
	for y := 0; y < invH; y++ {
		for x := 0; x < invW; x++ {
			c := invWhite
			if b.tintOn != 0 {
				c = invOverlay(y, 255-x)
			}
			b.tint[y*invW+x] = c
		}
	}
}

func (b *invaders) Reset() {
	b.cpu.Reset()
	b.ram = [0x2000]uint8{}
	b.shiftData, b.shiftAmount = 0, 0
	b.port3, b.port5 = 0, 0
	b.pendingIrq = 0
	b.voices = [invNV * 4]float64{}
	b.noise, b.lp, b.hp, b.hpPrev, b.sampleFrac = 1, 0, 0, 0, 0
	b.ufoLfo = 0
	b.cyc = 0
	for i := range b.screen {
		b.screen[i] = 0
	}
}

func (b *invaders) SetInput(p []Pad) {
	b.p1, b.p2 = Pad{}, Pad{}
	if p[0] != nil {
		b.p1 = p[0]
	}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

func (b *invaders) portIn(p int) uint8 {
	i, j, d := b.p1, b.p2, b.dips
	switch p {
	case 0:
		return 0x0e
	case 1:
		return uint8(bit(i["Coin"], 1) | bit(i["Start (2P)"], 2) | bit(i["Start (1P)"], 4) | 8 |
			bit(i["Fire"], 0x10) | bit(i["Left"], 0x20) | bit(i["Right"], 0x40))
	case 2:
		return uint8(d["lives"]&3 | bit(d["bonus"] != 0, 8) |
			bit(i["Fire"] || j["Fire"], 0x10) | bit(i["Left"] || j["Left"], 0x20) | bit(i["Right"] || j["Right"], 0x40) |
			bit(d["coinInfo"] == 0, 0x80))
	case 3:
		return uint8(b.shiftData >> (8 - b.shiftAmount))
	}
	return 0
}

func (b *invaders) portOut(p, v int) {
	switch p {
	case 2:
		b.shiftAmount = v & 7
	case 4:
		b.shiftData = (b.shiftData >> 8) | (v << 8)
	case 3:
		b.soundBits(0, b.port3, v)
		b.port3 = v
	case 5:
		b.soundBits(1, b.port5, v)
		b.port5 = v
	}
}

// soundBits starts effects on a 0 -> 1 change of their bit.
func (b *invaders) soundBits(bank, old, v int) {
	V := &b.voices
	rise := v &^ old
	start := func(n int) { V[n*4], V[n*4+1], V[n*4+2], V[n*4+3] = 0, 0, 0, 1 }
	if bank == 0 {
		if rise&0x02 != 0 {
			start(1)
		}
		if rise&0x04 != 0 {
			start(2)
		}
		if rise&0x08 != 0 {
			start(3)
		}
		if rise&0x10 != 0 {
			start(4)
		}
	} else {
		for k := 0; k < 4; k++ {
			if rise&(1<<k) != 0 {
				start(5 + k)
			}
		}
	}
}

func (b *invaders) nextNoise() float64 {
	n := b.noise
	bit := (n ^ (n >> 3)) & 1
	b.noise = (n >> 1) | (bit << 16)
	if n&1 != 0 {
		return 1
	}
	return -1
}

func sq(ph, lim, v float64) float64 {
	if ph < lim {
		return v
	}
	return -v
}

// mix: one output sample of all the effects.
func (b *invaders) mix() float64 {
	V := &b.voices
	dt := 1 / b.sampleRate
	const tau = math.Pi * 2
	if b.port3&0x20 == 0 { // the amplifier is off
		for n := 1; n < invNV; n++ {
			V[n*4] += dt
		}
		return 0
	}
	s := 0.0
	nz := b.nextNoise()
	b.ufoLfo = math.Mod(b.ufoLfo+6.5*dt, 1)
	if b.port3&0x01 != 0 {
		tri := 3 - b.ufoLfo*4
		if b.ufoLfo < 0.5 {
			tri = b.ufoLfo*4 - 1
		}
		hz := 560 + 220*tri
		V[1] = math.Mod(V[1]+hz*dt, 1)
		s += sq(V[1], 0.5, 0.09)*0.7 + math.Sin(V[1]*tau)*0.05
	}
	if V[7] != 0 { // shot
		V[4] += dt
		t := V[4]
		if t > 0.32 {
			V[7] = 0
		} else {
			hz := 1500*math.Exp(-t*7) + 200
			V[5] = math.Mod(V[5]+hz*dt, 1)
			env := math.Exp(-t * 9)
			s += env * (sq(V[5], 0.5, 0.08) + nz*0.05)
		}
	}
	if V[11] != 0 { // cannon hit
		V[8] += dt
		t := V[8]
		if t > 1.2 {
			V[11] = 0
		} else {
			V[9] += (nz - V[9]) * 0.06
			V[10] = math.Mod(V[10]+(55+40*math.Exp(-t*3))*dt, 1)
			env := math.Exp(-t * 2.6)
			s += env * (V[9]*0.55 + sq(V[10], 0.5, 0.06))
		}
	}
	if V[15] != 0 { // invader hit
		V[12] += dt
		t := V[12]
		if t > 0.3 {
			V[15] = 0
		} else {
			V[13] += (nz - V[13]) * 0.25
			V[14] = math.Mod(V[14]+(900*math.Exp(-t*12)+120)*dt, 1)
			env := math.Exp(-t * 11)
			s += env * (V[13]*0.22 + sq(V[14], 0.5, 0.08))
		}
	}
	if V[19] != 0 { // extra cannon
		V[16] += dt
		t := V[16]
		if t > 0.9 {
			V[19] = 0
		} else {
			V[17] = math.Mod(V[17]+(1250+120*math.Sin(t*tau*12))*dt, 1)
			s += math.Exp(-t*3.5) * math.Sin(V[17]*tau) * 0.12
		}
	}
	for k := 0; k < 4; k++ { // the march
		o := (5 + k) * 4
		if V[o+3] == 0 {
			continue
		}
		V[o] += dt
		t := V[o]
		if t > 0.14 {
			V[o+3] = 0
			continue
		}
		V[o+1] = math.Mod(V[o+1]+invMarchHz[k]*dt, 1)
		ph := V[o+1]
		wave := -0.7
		if ph < 0.3 {
			wave = 1
		} else if ph < 0.5 {
			wave = 0.3
		}
		s += wave * math.Exp(-t*26) * 0.3
	}
	if b.port5&0x10 != 0 { // UFO hit
		o := 36
		V[o] += dt
		sweep := math.Mod(V[o]*9, 1)
		V[o+1] = math.Mod(V[o+1]+(1800-1400*sweep)*dt, 1)
		s += sq(V[o+1], 0.5, 0.08)
	} else {
		V[36] = 0
	}
	return s
}

func (b *invaders) makeAudio(cycles int) {
	b.sampleFrac += float64(cycles) * b.sampleRate / invCPUHz
	n := math.Floor(b.sampleFrac)
	b.sampleFrac -= n
	for ; n > 0; n-- {
		s := b.mix()
		b.lp += (s - b.lp) * 0.45
		b.hp = 0.995 * (b.hp + b.lp - b.hpPrev)
		b.hpPrev = b.lp
		b.audio = append(b.audio, float32(math.Max(-1, math.Min(1, b.hp))))
	}
}

func (b *invaders) drawLine(y int) {
	base, row := 0x400+y*32, y*invW
	for bx := 0; bx < 32; bx++ {
		v := b.ram[base+bx]
		o := row + bx*8
		for k := 0; k < 8; k++ {
			if (v>>k)&1 != 0 {
				b.screen[o+k] = b.tint[o+k]
			} else {
				b.screen[o+k] = 0
			}
		}
	}
}

func (b *invaders) Screen() []uint32 { return b.screen }

func (b *invaders) RunFrame() {
	cpu := b.cpu
	if b.tintOn != b.dips["overlay"] {
		b.makeTint()
	}
	b.audio = b.audio[:0]
	done := 0
	for line := 0; line < invLines; line++ {
		if line == 96 {
			b.pendingIrq = 0xcf
		} else if line == 224 {
			b.pendingIrq = 0xd7
		}
		end := int(math.Floor(float64(invFrameCyc*(line+1))/invLines + 0.5))
		start := b.cyc
		for b.cyc < end {
			if b.pendingIrq != 0 {
				if t := cpu.Interrupt(uint8(b.pendingIrq)); t != 0 {
					b.cyc += t
					b.pendingIrq = 0
					continue
				}
			}
			b.cyc += cpu.Step()
		}
		if line < invH {
			b.drawLine(line)
		}
		b.makeAudio(b.cyc - start)
		done = end
	}
	b.cyc -= done
}

func (b *invaders) TakeAudio() []float32 {
	out := append([]float32(nil), b.audio...)
	b.audio = b.audio[:0]
	return out
}

func (b *invaders) SaveState() ([]byte, error) {
	w := &StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.F64s(b.voices[:])
	w.Ints([]int{b.shiftData, b.shiftAmount, b.port3, b.port5, b.pendingIrq, b.cyc, b.noise})
	w.F64s([]float64{b.lp, b.hp, b.hpPrev, b.sampleFrac, b.ufoLfo})
	return w.B, nil
}

func (b *invaders) LoadState(s []byte) error {
	rest, err := b.cpu.LoadState(s)
	if err != nil {
		return err
	}
	r := &StateR{B: rest}
	r.Bytes(b.ram[:])
	r.F64s(b.voices[:])
	m := make([]int, 7)
	r.Ints(m)
	b.shiftData, b.shiftAmount, b.port3, b.port5, b.pendingIrq, b.cyc, b.noise = m[0], m[1], m[2], m[3], m[4], m[5], m[6]
	f := make([]float64, 5)
	r.F64s(f)
	b.lp, b.hp, b.hpPrev, b.sampleFrac, b.ufoLfo = f[0], f[1], f[2], f[3], f[4]
	if err := r.Done(); err != nil {
		return err
	}
	for y := 0; y < invH; y++ {
		b.drawLine(y)
	}
	return nil
}

func init() {
	Register(&BoardDef{
		ID: "invaders", Title: "Space Invaders", Width: invW, Height: invH, Rotate: 270, FPS: invFPS,
		Buttons: []string{"Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)"},
		Dips:    invDips,
		Games: []Game{
			{ID: "invaders", Title: "Space Invaders", Year: 1978, Maker: "Taito / Midway", Files: []File{
				{Name: "invaders.h", Size: 0x800, Role: "cpu", Offset: 0x0000},
				{Name: "invaders.g", Size: 0x800, Role: "cpu", Offset: 0x0800},
				{Name: "invaders.f", Size: 0x800, Role: "cpu", Offset: 0x1000},
				{Name: "invaders.e", Size: 0x800, Role: "cpu", Offset: 0x1800},
			}},
		},
		New: newInvaders,
	})
}
