package arcade

// The Donkey Kong board (Nintendo, 1981): a port of web/emu/arcade_dkong.js.
// A Z80 at 3.072 MHz with 16 KB of program ROM, work RAM, a 32x32 tile map and
// a sprite list that an 8257 DMA chip copies into the sprite memory once a
// frame; colours from the set's three PROMs; an 8035 sound processor (mcs48)
// running the set's own sound program through an 8-bit DAC; and RetroPixels'
// own versions of the walk, jump and stomp sounds. The monitor is on its
// side: the board draws 256x224, turned a quarter turn clockwise.

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
	fx                         [12]float64
	noise                      int
	hp, hpPrev, lp             float64
}

type dkBus struct{ b *dkong }

func (x dkBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x dkBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }
func (x dkBus) In(uint16) uint8         { return 0xff }
func (x dkBus) Out(uint16, uint8)       {}

type dkSndBus struct{ b *dkong }

func (x dkSndBus) ROM(a int) uint8 {
	if a < 0x800 {
		return x.b.sndRom[a]
	}
	return 0xff
}

// P2 bit 6 selects the tune latch; otherwise P2 bits 0-2 page the tune data ROM
func (x dkSndBus) ReadX(a uint8) uint8 {
	b := x.b
	if b.sndP2&0x40 != 0 {
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

// port 2 reads back its own latch, except bit 5: sound latch output 3 (inverted)
func (x dkSndBus) InP(p int) uint8 {
	b := x.b
	switch p {
	case 1:
		return uint8(b.snd.Port(1))
	case 2:
		return uint8(b.snd.Port(2)&^0x20 | bit(b.latch259&0x08 == 0, 0x20))
	}
	return 0xff
}

// the test pins read the main board's sound latch outputs 5 and 4 (inverted)
func (x dkSndBus) T0() bool      { return x.b.latch259&0x20 == 0 }
func (x dkSndBus) T1() bool      { return x.b.latch259&0x10 == 0 }
func (x dkSndBus) IntLine() bool { return x.b.sndIrq }

func newDkong(g *Game, files Files, opts Opts) (Board, error) {
	b := &dkong{sampleRate: float64(opts.SampleRate), dips: opts.Dips}
	if b.dips == nil {
		b.dips = map[string]int{}
	}
	for _, d := range dkDips {
		if _, ok := b.dips[d.ID]; !ok {
			b.dips[d.ID] = d.Default
		}
	}
	b.rom = Region(g, files, "cpu", 0x4000)
	b.sndRom = Region(g, files, "sound", 0x1800)
	tileRom, spriteRom := Region(g, files, "tiles", 0x1000), Region(g, files, "sprites", 0x2000)
	proms := Region(g, files, "prom", 0x300)
	b.decodeGraphics(tileRom, spriteRom)
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

func (b *dkong) decodeGraphics(tileRom, spriteRom []byte) {
	b.tiles = make([]uint8, 256*64)
	for n := 0; n < 256; n++ {
		for y := 0; y < 8; y++ {
			lo, hi := tileRom[n*8+y], tileRom[0x800+n*8+y]
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

func (b *dkong) decodePalette(proms []byte) {
	w3 := func(v int) int {
		return jsRound(255 * (float64(v&1)*0.13 + float64((v>>1)&1)*0.28 + float64((v>>2)&1)*0.59))
	}
	w2 := func(v int) int { return jsRound(255 * (float64(v&1)*0.32 + float64((v>>1)&1)*0.68)) }
	for i := 0; i < 256; i++ {
		lo, hi := int(^proms[i]&0xf), int(^proms[0x100+i]&0xf)
		r, g, bl := (hi>>1)&7, ((hi&1)<<2)|((lo>>2)&3), lo&3
		b.palette[i] = RGB(w3(r), w3(g), w2(bl))
	}
}

func (b *dkong) Reset() {
	b.cpu.Reset()
	b.snd.Reset()
	b.ram, b.spriteRam, b.vram = [0x1000]uint8{}, [0x400]uint8{}, [0x400]uint8{}
	b.latch259, b.misc, b.soundLatch = 0, 0, 0x0f
	b.sndIrq, b.sndP2, b.dac = false, 0xff, 0
	b.dma, b.dmaFlip, b.dmaMode = [8]uint16{}, 0, 0
	b.cyc, b.sndCyc, b.sndAcc, b.sndAccN, b.samplePos = 0, 0, 0, 0, 0
	b.env = 1
	b.fx = [12]float64{}
	b.noise, b.hp, b.hpPrev, b.lp = 1, 0, 0, 0
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
	case a < 0x4000:
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
		return uint8(bit(i["Start (1P)"], 4) | bit(i["Start (2P)"], 8) | bit(i["Coin"], 0x80) |
			bit(b.snd.Port(2)&0x10 == 0, 0x40)) // the sound processor's "busy" line
	case 0x7d80:
		d := b.dips
		return uint8(d["lives"]&3 | (d["bonus"]&3)<<2 | 0x80)
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
	case a >= 0x7d00 && a < 0x7d08:
		bt, old := 1<<(a&7), b.latch259
		if v&1 != 0 {
			b.latch259 = old | bt
		} else {
			b.latch259 = old &^ bt
		}
		if (b.latch259&^old)&7 != 0 {
			b.startEffect(b.latch259 &^ old & 7)
		}
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
	for row := 2; row < 30; row++ {
		for col := 0; col < 32; col++ {
			code := int(vram[row*32+col])
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

func (b *dkong) startEffect(bits int) {
	for k := 0; k < 3; k++ {
		if bits&(1<<k) != 0 {
			b.fx[k*4], b.fx[k*4+1], b.fx[k*4+3] = 0, 0, 1
		}
	}
}

func (b *dkong) nextNoise() float64 {
	n := b.noise
	bt := (n ^ (n >> 2)) & 1
	b.noise = (n >> 1) | (bt << 15)
	if n&1 != 0 {
		return 1
	}
	return -1
}

// walking, jumping and the stomp: RetroPixels' own versions
func (b *dkong) effects(dt float64) float64 {
	F := &b.fx
	s := 0.0
	if F[3] != 0 {
		F[0] += dt
		t := F[0]
		if t > 0.07 {
			F[3] = 0
		} else {
			F[1] = math.Mod(F[1]+(260-1600*t)*dt, 1)
			s += sq(F[1], 0.5, 1) * 0.16 * math.Exp(-t*35)
		}
	}
	if F[7] != 0 {
		F[4] += dt
		t := F[4]
		if t > 0.3 {
			F[7] = 0
		} else {
			hz := 1040 - 1800*(t-0.06)
			if t < 0.06 {
				hz = 500 + 9000*t
			}
			F[5] = math.Mod(F[5]+hz*dt, 1)
			s += math.Sin(F[5]*math.Pi*2) * 0.14 * math.Exp(-t*6)
		}
	}
	if F[11] != 0 {
		F[8] += dt
		t := F[8]
		if t > 0.5 {
			F[11] = 0
		} else {
			F[10] += (b.nextNoise() - F[10]) * 0.03
			F[9] = math.Mod(F[9]+(70-60*t)*dt, 1)
			s += (math.Sin(F[9]*math.Pi*2)*0.3 + F[10]*0.35) * math.Exp(-t*9)
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
		on, tau := 0.0, 0.12
		if b.sndP2&0x80 != 0 {
			on, tau = 1, 0.002
		}
		b.env += (on - b.env) * (1 - math.Exp(-float64(c)/dkSndHz/tau))
		b.sndAcc += float64(b.dac) / 255 * b.env * float64(c)
		b.sndAccN += c
		b.samplePos += float64(c)
		if b.samplePos >= perSample {
			b.samplePos -= perSample
			v := b.sndAcc/float64(b.sndAccN)*0.6 + b.effects(dt)
			b.sndAcc, b.sndAccN = 0, 0
			b.hp = 0.998 * (b.hp + v - b.hpPrev)
			b.hpPrev = v
			b.lp += (b.hp - b.lp) * 0.5
			b.audio = append(b.audio, float32(math.Max(-1, math.Min(1, b.lp))))
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
	w.Ints([]int{b.latch259, b.misc, b.soundLatch, b.sndP2, b.dac, b.dmaFlip, b.dmaMode, b.cyc, b.sndAccN, b.noise})
	w.Bool(b.sndIrq)
	w.F64s([]float64{b.sndCyc, b.sndAcc, b.samplePos, b.env, b.hp, b.hpPrev, b.lp})
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
	m := make([]int, 10)
	r.Ints(m)
	b.latch259, b.misc, b.soundLatch, b.sndP2, b.dac, b.dmaFlip, b.dmaMode, b.cyc, b.sndAccN, b.noise = m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9]
	b.sndIrq = r.Bool()
	f := make([]float64, 7)
	r.F64s(f)
	b.sndCyc, b.sndAcc, b.samplePos, b.env, b.hp, b.hpPrev, b.lp = f[0], f[1], f[2], f[3], f[4], f[5], f[6]
	if err := r.Done(); err != nil {
		return err
	}
	b.audio = b.audio[:0]
	b.drawFrame()
	return nil
}

var dkDips = []Dip{
	{ID: "lives", Label: "Lives", Default: 0, Options: []DipOption{{"3", 0}, {"4", 1}, {"5", 2}, {"6", 3}}},
	{ID: "bonus", Label: "Extra life at", Default: 0, Options: []DipOption{{"7,000", 0}, {"10,000", 1}, {"15,000", 2}, {"20,000", 3}}},
}

func init() {
	Register(&BoardDef{
		ID: "dkong", Title: "Nintendo Donkey Kong", Width: dkW, Height: dkH, Rotate: 90, FPS: dkFPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Jump", "Coin", "Start (1P)", "Start (2P)"},
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
		},
		New: newDkong,
	})
}
