// Package irem is Irem's M62 board as Kung-Fu Master has it (1984): a Z80 at
// 3.072 MHz, a 512x256 scrolling playfield of 8x8 three-bit tiles (the top
// six rows hold still for the score), 32 sprites of 16x16 pixels stacked up
// to four high, and Irem's sound board: an M6803 at 3.58 MHz driving two
// AY-3-8910s and two MSM5205 ADPCM voices, whose sample clock is the
// 6803's NMI.
package irem

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/ay8910"
	"retropixels/cores/m6801"
	"retropixels/cores/msm5205"
	"retropixels/cores/z80"
)

const (
	scrW, scrH  = 256, 256
	lines       = 284
	vblank      = 256
	lineCycles  = 192 // 18.432 MHz / 6 per 384-pixel line at 18.432 MHz / 3
	frameCycles = lines * lineCycles
	FPS         = 18432000.0 / 3 / 384 / lines
	sndHz       = 3579545.0 / 4 // the 6803's E clock, also the AY clock
	sndPerLine  = sndHz / FPS / lines
	msmClock    = 384000.0
	hOffset     = 128 // the first pixel shown
)

type board struct {
	rom     []byte
	sndROM  []byte
	tiles   [1024 * 64]uint8
	sprites [1024 * 256]uint8
	height  []byte
	chrPens [256]uint32
	sprPens [256]uint32

	ram       [0x1000]uint8
	tileRAM   [0x1000]uint8
	spriteRAM [0x100]uint8
	hscroll   uint16
	flip      bool

	main *z80.CPU
	snd  *m6801.CPU
	ay   [2]*ay8910.PSG
	msm  [2]*msm5205.Chip

	latch        uint8
	port1, port2 uint8
	ayAddr       [2]uint8
	lastEnable   int // the 45M's last port enable bits (-1 before the first write)
	msmMode      uint8
	vck          bool
	sndT, vckT   float64 // sound cycles into the frame, the next VCK edge

	out      *arcade.Resampler
	screen   []uint32
	p1, p2   arcade.Pad
	dips     map[string]int
	cycles   int
	ayLP, dc float64
	msmF     [4]float64 // the ADPCM filter's last inputs and outputs
}

type mainBus struct{ b *board }

func (x mainBus) Read(a uint16) uint8     { return x.b.read(a) }
func (x mainBus) Write(a uint16, v uint8) { x.b.write(a, v) }
func (x mainBus) In(p uint16) uint8       { return x.b.in(uint8(p)) }
func (x mainBus) Out(p uint16, v uint8)   { x.b.out8(uint8(p), v) }
func (x mainBus) Ack()                    { x.b.main.IRQ = false }

type sndBus struct{ b *board }

func (x sndBus) Read(a uint16) uint8 {
	if a >= 0x4000 {
		return x.b.sndROM[a]
	}
	return 0
}

func (x sndBus) Write(a uint16, v uint8) {
	if a&0x0800 == 0 {
		return
	}
	b := x.b
	switch a & 3 {
	case 0:
		if b.latch&0x80 != 0 {
			b.snd.IRQ = false
		}
	case 1, 2:
		// as MAME does, 3-bit codes go to the 4-bit decoder shifted left
		if b.msmMode&4 == 0 {
			v = (v & 7) << 1
		}
		b.msm[a&3-1].Write(v)
	}
}

func (x sndBus) PortRead(port int) uint8 {
	b := x.b
	if port == 1 {
		switch {
		case b.port2&0x08 != 0:
			return b.ay[0].ReadData()
		case b.port2&0x10 != 0:
			return b.ay[1].ReadData()
		}
		return 0xff
	}
	return 0
}

func (x sndBus) PortWrite(port int, v, ddr uint8) {
	if ddr == 0 {
		return
	}
	v = v&ddr | ^ddr
	b := x.b
	switch port {
	case 1:
		b.port1 = v
	case 2:
		// the PSGs latch on a falling edge of bit 0
		if b.port2&1 != 0 && v&1 == 0 {
			for i, sel := range []uint8{0x08, 0x10} {
				if b.port2&sel == 0 {
					continue
				}
				if b.port2&4 != 0 {
					b.ayAddr[i] = b.port1
					b.ay[i].WriteAddr(b.port1)
				} else {
					b.ayWrite(i, b.port1)
				}
			}
		}
		b.port2 = v
	}
}

// ayWrite writes a PSG's selected register; the 45M's port B (set as an
// output) drives the ADPCM chips' reset and clock-select pins, and turning
// the port into an input makes its pins read high.
func (b *board) ayWrite(i int, v uint8) {
	b.ay[i].WriteData(v)
	if i != 0 || b.ayAddr[0] != 7 {
		return
	}
	if b.lastEnable < 0 || (b.lastEnable^int(v))&0x80 != 0 {
		pb := uint8(0xff)
		if v&0x80 != 0 {
			pb = b.ay[0].Regs()[15]
		}
		b.portB(pb)
	}
	b.lastEnable = int(v & 0xc0)
}

// portB: bits 0 and 1 reset the two MSM5205s, bits 2-4 pick the first one's
// clock divider and code size (the second is its slave).
func (b *board) portB(v uint8) {
	b.msmMode = v >> 2 & 7
	b.msm[0].SetReset(v&1 != 0)
	b.msm[1].SetReset(v&2 != 0)
}

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips}
	b.rom = arcade.Region(g, files, "cpu", 0x8000)
	b.sndROM = make([]byte, 0x10000)
	snd := arcade.Region(g, files, "sound", 0x6000)
	copy(b.sndROM[0xa000:], snd)
	b.height = arcade.Region(g, files, "height", 0x20)

	tr := arcade.Region(g, files, "tiles", 0x6000)
	for c := 0; c < 1024; c++ {
		for y := 0; y < 8; y++ {
			o := c*8 + y
			for x := 0; x < 8; x++ {
				s := uint(7 - x)
				b.tiles[c*64+y*8+x] = tr[o+0x4000]>>s&1<<2 | tr[o+0x2000]>>s&1<<1 | tr[o]>>s&1
			}
		}
	}
	sr := arcade.Region(g, files, "sprites", 0x18000)
	for c := 0; c < 1024; c++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				o := c*32 + y
				if x >= 8 {
					o += 16
				}
				s := uint(7 - x&7)
				b.sprites[c*256+y*16+x] = sr[o+0x10000]>>s&1<<2 | sr[o+0x8000]>>s&1<<1 | sr[o]>>s&1
			}
		}
	}
	b.makePalettes(arcade.Region(g, files, "chrpal", 0x300), arcade.Region(g, files, "sprpal", 0x300))

	b.screen = make([]uint32, scrW*scrH)
	for i := range b.ay {
		b.ay[i] = ay8910.New(false)
	}
	b.ay[0].PortIn = func(int) uint8 { return b.latch }
	b.ay[0].PortOut = func(port int, v uint8) {
		if port == 1 {
			b.portB(v)
		}
	}
	for i := range b.msm {
		b.msm[i] = msm5205.New(msmClock, 96, 4)
	}
	b.out = arcade.NewResampler(FPS*lines, float64(opts.SampleRate))
	b.main = z80.New(mainBus{b})
	b.snd = m6801.New(sndBus{b}, m6801.MC6801)
	b.Reset()
	return b, nil
}

func (b *board) Reset() {
	b.main.Reset()
	b.main.IRQ = false
	b.snd.Reset()
	b.snd.IRQ = true
	for i := range b.ay {
		b.ay[i].Reset()
	}
	for i := range b.msm {
		b.msm[i].Reset()
		b.msm[i].SetDivider(96, 4)
		b.msm[i].SetReset(false)
	}
	b.msmMode = 4
	b.ram, b.tileRAM, b.spriteRAM = [0x1000]uint8{}, [0x1000]uint8{}, [0x100]uint8{}
	b.hscroll, b.flip = 0, false
	b.latch, b.port1, b.port2, b.lastEnable = 0, 0, 0, -1
	b.ayAddr = [2]uint8{}
	b.vck, b.sndT, b.vckT = false, 0, sndHz*96/msmClock/2
	b.cycles, b.ayLP, b.dc, b.msmF = 0, 0, 0, [4]float64{}
}

// ---------------------------------------------------------------- main CPU

func (b *board) read(a uint16) uint8 {
	switch {
	case a < 0x8000:
		return b.rom[a]
	case a >= 0xd000 && a < 0xe000:
		return b.tileRAM[a-0xd000]
	case a >= 0xe000 && a < 0xf000:
		return b.ram[a-0xe000]
	}
	return 0xff
}

func (b *board) write(a uint16, v uint8) {
	switch {
	case a == 0xa000:
		b.hscroll = b.hscroll&0xff00 | uint16(v)
	case a == 0xb000:
		b.hscroll = b.hscroll&0xff | uint16(v)<<8
	case a >= 0xc000 && a < 0xc100:
		b.spriteRAM[a-0xc000] = v
	case a >= 0xd000 && a < 0xe000:
		b.tileRAM[a-0xd000] = v
	case a >= 0xe000 && a < 0xf000:
		b.ram[a-0xe000] = v
	}
}

func bit(on bool, v uint8) uint8 {
	if on {
		return v
	}
	return 0
}

func pad(p arcade.Pad) uint8 {
	return bit(p["Right"], 1) | bit(p["Left"], 2) | bit(p["Down"], 4) | bit(p["Up"], 8) |
		bit(p["Button 2"], 0x20) | bit(p["Button 1"], 0x80)
}

func (b *board) dsw1() uint8 {
	d := b.dips
	return uint8(d["difficulty"]&1 | (d["energy"]&1)<<1 | (d["lives"]&3)<<2 | d["coins"]&0xf0)
}

func (b *board) dsw2() uint8 {
	d := b.dips
	return 0x01 | uint8(d["cabinet"]&1)<<1 | 0x04 | uint8(d["slowmo"]&1)<<3 | uint8(d["freeze"]&1)<<4 |
		uint8(d["levelselect"]&1)<<5 | uint8(d["invulnerable"]&1)<<6 | 0x80
}

func (b *board) in(p uint8) uint8 {
	q, r := b.p1, b.p2
	switch p {
	case 0:
		return ^(bit(q["Start (1P)"] || r["Start (1P)"], 1) | bit(q["Start (2P)"] || r["Start (2P)"], 2) |
			bit(q["Service"], 4) | bit(q["Coin"], 8))
	case 1:
		return ^pad(q)
	case 2:
		return ^(pad(r)&^0x10 | bit(q["Coin 2"] || r["Coin"], 0x10))
	case 3:
		return b.dsw1()
	case 4:
		return b.dsw2()
	}
	return 0xff
}

func (b *board) out8(p uint8, v uint8) {
	switch p {
	case 0:
		b.latch = v
		if v&0x80 == 0 {
			b.snd.IRQ = true
		}
	case 1:
		b.flip = (v^^b.dsw2())&1 != 0
	}
}

func (b *board) SetInput(p []arcade.Pad) {
	b.p1 = p[0]
	b.p2 = arcade.Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

// ---------------------------------------------------------------- video

// resNet is MAME's compute_res_net for four TTL outputs through 2.2k, 1k,
// 470 and 220 ohms with 470 ohms to ground.
func resNet(in int) float64 {
	const vOL, vOH, ttlHRes = 0.05, 4.0, 50.0
	r := [4]float64{2200, 1000, 470, 220}
	rTotal, v := 1/470.0, 0.0
	for i, x := range r {
		if in>>i&1 != 0 {
			rTotal += 1 / (x + ttlHRes)
			v += vOH / (x + ttlHRes)
		} else {
			rTotal += 1 / x
			v += vOL / x
		}
	}
	return float64(int(v/rTotal*255/5 + 0.4))
}

// makePalettes: the board's colours are dark, so (as MAME does) the
// brightest one is raised to full brightness and the rest scaled with it.
func (b *board) makePalettes(chr, spr []byte) {
	var rgb [2][256][3]float64
	ymax := 1.0
	for k, p := range [][]byte{chr, spr} {
		for i := 0; i < 256; i++ {
			for c := 0; c < 3; c++ {
				rgb[k][i][c] = resNet(int(p[c*256+i] & 15))
			}
			c := rgb[k][i]
			ymax = max(ymax, 299*c[0]+587*c[1]+114*c[2])
		}
	}
	contrast := float32(255000 / ymax)
	adj := func(v float64) int { return min(255, int(float32(v)*contrast)) }
	for i := 0; i < 256; i++ {
		c := rgb[0][i]
		b.chrPens[i] = arcade.RGB(adj(c[0]), adj(c[1]), adj(c[2]))
		c = rgb[1][i]
		b.sprPens[i] = arcade.RGB(adj(c[0]), adj(c[1]), adj(c[2]))
	}
}

// drawTiles draws the playfield tiles of one priority: category 1 (the
// score rows, and colours above 0x19) goes over the sprites.
func (b *board) drawTiles(cat int) {
	for y := 0; y < scrH; y++ {
		row := y >> 3
		scroll := 0
		if row >= 6 {
			scroll = int(b.hscroll)
		}
		line := b.screen[y*scrW : (y+1)*scrW]
		for x := 0; x < scrW; {
			tx := (x + hOffset + scroll) & 511
			idx := row*64 + tx>>3
			code := int(b.tileRAM[idx])
			col := b.tileRAM[idx+0x800]
			c := 0
			if row < 6 || int(col&0x1f)>>1 > 0x0c {
				c = 1
			}
			n := 8 - tx&7
			if c != cat {
				x += n
				continue
			}
			code |= int(col&0xc0) << 2
			pens := b.chrPens[int(col&0x1f)*8:]
			src := b.tiles[code*64+(y&7)*8:]
			for px := tx & 7; px < 8 && x < scrW; px++ {
				sx := px
				if col&0x20 != 0 {
					sx = 7 - px
				}
				line[x] = pens[src[sx]]
				x++
			}
		}
	}
}

func (b *board) drawSprites() {
	sp := b.spriteRAM[:]
	for offs := 0; offs < len(sp); offs += 8 {
		code := int(sp[offs+4]) | int(sp[offs+5]&7)<<8
		col := int(sp[offs] & 0x1f)
		sx := 256*int(sp[offs+7]&1) + int(sp[offs+6]) - hOffset
		sy := 256 + 128 - 15 - (256*int(sp[offs+3]&1) + int(sp[offs+2]))
		flipx, flipy := sp[offs+5]&0x40 != 0, sp[offs+5]&0x80 != 0
		i := int(b.height[(code>>5)&0x1f])
		switch i {
		case 1:
			code &^= 1
			sy -= 16
		case 2:
			i = 3
			code &^= 3
			sy -= 48
		}
		incr := 1
		if flipy {
			incr = -1
			code += i
		}
		for ; i >= 0; i-- {
			b.drawSprite((code+i*incr)&1023, col, flipx, flipy, sx, sy+16*i)
		}
	}
}

func (b *board) drawSprite(code, col int, flipx, flipy bool, sx, sy int) {
	pens := b.sprPens[col*8:]
	src := b.sprites[code*256:]
	for y := 0; y < 16; y++ {
		dy := sy + y
		if dy < 0 || dy >= scrH {
			continue
		}
		yy := y
		if flipy {
			yy = 15 - y
		}
		for x := 0; x < 16; x++ {
			dx := sx + x
			if dx < 0 || dx >= scrW {
				continue
			}
			xx := x
			if flipx {
				xx = 15 - x
			}
			if p := src[yy*16+xx]; p != 0 {
				b.screen[dy*scrW+dx] = pens[p]
			}
		}
	}
}

func (b *board) render() {
	b.drawTiles(0)
	b.drawSprites()
	b.drawTiles(1)
}

func (b *board) Screen() []uint32 { return b.screen }

// ---------------------------------------------------------------- timing

// runSound runs the sound board up to sound cycle t, with the MSM5205's VCK
// edges: the rising edge is the 6803's NMI (the program then puts the next
// code on the data pins), the falling edge decodes.
func (b *board) runSound(t float64) {
	for b.sndT < t {
		slave := b.msmMode&3 == 3
		next := t
		if !slave && b.vckT < next {
			next = b.vckT
		}
		for b.sndT < next {
			n := b.snd.Step()
			b.ay[0].Run(n)
			b.ay[1].Run(n)
			b.sndT += float64(n)
		}
		if slave {
			b.vckT = b.sndT
			continue
		}
		if b.sndT >= b.vckT {
			div := [4]int{96, 48, 64, 96}[b.msmMode&3]
			b.vckT += sndHz * float64(div) / msmClock / 2
			b.vck = !b.vck
			if b.vck {
				b.snd.NMI()
			} else {
				b.msm[0].Clock()
				b.msm[1].Clock()
			}
		}
	}
}

func (b *board) RunFrame() {
	sndBase := 0.0
	for line := 0; line < lines; line++ {
		if line == vblank {
			b.render()
			b.main.IRQ = true
		}
		end := (line + 1) * lineCycles
		for b.cycles < end {
			b.cycles += b.main.Step()
		}
		b.runSound(sndBase + float64(line+1)*sndPerLine)
		b.out.Add(b.mix())
	}
	b.cycles -= frameCycles
	total := sndPerLine * lines
	b.sndT -= total
	b.vckT -= total
}

// The mixer, as MAME's model of the board's analog stage measures: the PSGs
// through a gentle low-pass, the ADPCM voices through a steep one at about
// 850 Hz, then a coupling capacitor.
var msmLP = biquad(850, FPS*lines)

type lowPass struct{ b0, b1, b2, a1, a2 float64 }

func biquad(fc, rate float64) lowPass {
	k := math.Tan(math.Pi * fc / rate)
	n := 1 / (1 + math.Sqrt2*k + k*k)
	b0 := k * k * n
	return lowPass{b0, 2 * b0, b0, 2 * (k*k - 1) * n, (1 - math.Sqrt2*k + k*k) * n}
}

func (b *board) mix() float64 {
	ay := (b.ay[0].Take() + b.ay[1].Take()) / 6 * 0.62
	b.ayLP += (ay - b.ayLP) * (1 - math.Exp(-2*math.Pi*2400/(FPS*lines)))
	x := float64(b.msm[0].Output()+b.msm[1].Output()) / 2048 * 0.85
	f := &b.msmF
	y := msmLP.b0*x + msmLP.b1*f[0] + msmLP.b2*f[1] - msmLP.a1*f[2] - msmLP.a2*f[3]
	f[1], f[0], f[3], f[2] = f[0], x, f[2], y
	s := b.ayLP + y
	b.dc += (s - b.dc) * 0.002
	return s - b.dc
}

func (b *board) TakeAudio() []float32 { return b.out.Take() }

// ---------------------------------------------------------------- state

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.main.AppendState(w.B)
	w.B = b.snd.AppendState(w.B)
	for i := range b.ay {
		w.B = b.ay[i].AppendState(w.B)
	}
	for i := range b.msm {
		w.B = b.msm[i].AppendState(w.B)
	}
	w.Bytes(b.ram[:])
	w.Bytes(b.tileRAM[:])
	w.Bytes(b.spriteRAM[:])
	w.Bytes([]uint8{b.latch, b.port1, b.port2, b.msmMode, uint8(b.hscroll), uint8(b.hscroll >> 8), b.ayAddr[0], b.ayAddr[1]})
	w.Bool(b.flip)
	w.Bool(b.vck)
	w.Bool(b.main.IRQ)
	w.Bool(b.snd.IRQ)
	w.Ints([]int{b.lastEnable, b.cycles, int(b.sndT * 1024), int(b.vckT * 1024)})
	b.out.AppendState(w)
	return w.B, nil
}

func (b *board) LoadState(s []byte) error {
	rest, err := b.main.LoadState(s)
	if err != nil {
		return err
	}
	if rest, err = b.snd.LoadState(rest); err != nil {
		return err
	}
	for i := range b.ay {
		if rest, err = b.ay[i].LoadState(rest); err != nil {
			return err
		}
	}
	for i := range b.msm {
		if rest, err = b.msm[i].LoadState(rest); err != nil {
			return err
		}
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.tileRAM[:])
	r.Bytes(b.spriteRAM[:])
	v := make([]uint8, 8)
	r.Bytes(v)
	b.latch, b.port1, b.port2, b.msmMode = v[0], v[1], v[2], v[3]
	b.ayAddr = [2]uint8{v[6], v[7]}
	b.hscroll = uint16(v[4]) | uint16(v[5])<<8
	b.flip = r.Bool()
	b.vck = r.Bool()
	b.main.IRQ = r.Bool()
	b.snd.IRQ = r.Bool()
	m := make([]int, 4)
	r.Ints(m)
	b.lastEnable, b.cycles = m[0], m[1]
	b.sndT, b.vckT = float64(m[2])/1024, float64(m[3])/1024
	b.out.LoadState(r)
	b.out.Clear()
	b.render()
	return r.Done()
}
