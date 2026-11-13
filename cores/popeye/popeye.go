// Package popeye is Nintendo's Popeye board (TPP2, 1982): one Z80 at 4 MHz
// with an encrypted program, an AY-3-8910 for sound (its ports read the
// coin switches and pick which of the other switches the game reads), a
// 512x512 picture (448 lines shown) made of a low-resolution bitmap
// background, 16x16 sprites copied out of work RAM at each vertical blank,
// and a layer of 8x8 characters drawn twice as big.
//
// The game turns its vertical-blank NMI on and off through the Z80's I
// register (the board reads it off the address bus during refresh); bit 1
// arms a watchdog. A protection chip at E000 shifts the last two values
// written.
package popeye

import (
	"retropixels/cores/arcade"
	"retropixels/cores/ay8910"
	"retropixels/cores/z80"
)

const (
	cpuHz   = 4000000
	FPS     = 59.94
	lines   = 512
	vblank  = 480 // the first line after the picture
	top     = 32  // the first line shown
	scrW    = 512
	scrH    = 448
	dmaSize = 0x280
)

var frameCycles = 66734 // 4 MHz / 59.94 Hz

type board struct {
	rom               []byte
	chars             [256 * 64]uint8 // 8x8, 1 bit
	sprites           [512 * 256]uint8
	bgProm, charProm  []byte
	sprProm           [256]uint8
	pens              [80]uint32
	ram               [0x800]uint8 // 8800-8fff (8c00-8e7f is the sprite / scroll list)
	vram, cram        [0x400]uint8
	bgram             [0x1000]uint8
	sprRAM            [dmaSize]uint8
	scroll            [3]uint8
	palBank           uint8
	prot0, prot1      uint8
	protShift, dswBit uint8
	flip              bool
	nmiOn, nmiLine    bool
	wdOn, held        bool
	wdCount           uint8
	field             uint8
	cpu               *z80.CPU
	ay                *ay8910.PSG
	out               *arcade.Resampler
	screen            []uint32
	lineBuf           [512]uint16
	p1, p2            arcade.Pad
	dips              map[string]int
	cycles, ayFrac    int
	frame             int
}

type bus struct{ b *board }

func (x bus) Read(a uint16) uint8     { return x.b.read(a) }
func (x bus) Write(a uint16, v uint8) { x.b.write(a, v) }
func (x bus) In(p uint16) uint8       { return x.b.in(uint8(p)) }
func (x bus) Out(p uint16, v uint8) {
	switch p & 0xff {
	case 0:
		x.b.ay.WriteAddr(v)
	case 1:
		x.b.ay.WriteData(v)
	}
}

func bitswap(v int, bits ...int) int {
	out, n := 0, len(bits)
	for i, s := range bits {
		out |= (v >> s & 1) << (n - 1 - i)
	}
	return out
}

func newBoard(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &board{dips: opts.Dips}
	enc := arcade.Region(g, files, "cpu", 0x8000)
	b.rom = make([]byte, 0x8000)
	for i := range b.rom {
		b.rom[i] = byte(bitswap(int(enc[bitswap(i, 15, 14, 13, 12, 11, 10, 8, 7, 6, 3, 9, 5, 4, 2, 1, 0)^0x3f]), 3, 4, 2, 5, 1, 6, 0, 7))
	}
	// the character ROM's first half is empty: the second half is used
	cr := arcade.Region(g, files, "chars", 0x1000)[0x800:]
	for c := 0; c < 256; c++ {
		for y := 0; y < 8; y++ {
			v := cr[c*8+y]
			for x := 0; x < 8; x++ {
				b.chars[c*64+y*8+x] = v >> x & 1
			}
		}
	}
	sr := arcade.Region(g, files, "sprites", 0x8000)
	for s := 0; s < 512; s++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				o := s*16 + 15 - y
				if x < 8 {
					o += 0x2000
				}
				bit := uint(x & 7)
				p := (sr[o]>>bit&1)<<1 | sr[o+0x4000]>>bit&1
				b.sprites[s*256+y*16+x] = p
			}
		}
	}
	b.bgProm = arcade.Region(g, files, "bgpal", 32)
	b.charProm = append([]byte(nil), arcade.Region(g, files, "charpal", 32)...)
	// two of the character PROM's address pins are tied together
	fixed := make([]byte, 32)
	for i := range fixed {
		fixed[i] = b.charProm[(i&0xf)|(i&8)<<1]
	}
	b.charProm = fixed
	lo, hi := arcade.Region(g, files, "sprpal-lo", 256), arcade.Region(g, files, "sprpal-hi", 256)
	for i := range b.sprProm {
		b.sprProm[i] = lo[i]&15 | hi[i]<<4
	}
	b.screen = make([]uint32, scrW*scrH)
	b.ay = ay8910.New(false)
	b.ay.PortIn = func(int) uint8 { return b.dsw0() }
	b.ay.PortOut = func(port int, v uint8) {
		if port == 1 {
			b.flip = v&1 != 0
			b.dswBit = (v & 0x0e) >> 1
		}
	}
	b.out = arcade.NewResampler(FPS*lines, float64(opts.SampleRate))
	b.cpu = z80.New(bus{b})
	b.Reset()
	return b, nil
}

func (b *board) Reset() {
	b.cpu.Reset()
	b.cpu.IRQ = false
	b.ay.Reset()
	b.ram, b.vram, b.cram, b.bgram, b.sprRAM = [0x800]uint8{}, [0x400]uint8{}, [0x400]uint8{}, [0x1000]uint8{}, [dmaSize]uint8{}
	b.scroll, b.palBank = [3]uint8{}, 0
	b.prot0, b.prot1, b.protShift, b.dswBit, b.flip = 0, 0, 0, 0, false
	b.nmiOn, b.nmiLine, b.wdOn, b.held, b.wdCount, b.field = false, false, false, false, 0, 0
	b.cycles, b.ayFrac, b.frame = 0, 0, 0
	b.updatePalette()
}

func (b *board) read(a uint16) uint8 {
	switch {
	case a < 0x8000:
		return b.rom[a]
	case a >= 0x8800 && a < 0x9000:
		return b.ram[a-0x8800]
	case a == 0xe000:
		return uint8((int(b.prot1)<<b.protShift | int(b.prot0)>>(8-b.protShift)) & 0xff)
	case a == 0xe001:
		return 0
	}
	return 0xff
}

func (b *board) write(a uint16, v uint8) {
	switch {
	case a >= 0x8800 && a < 0x9000:
		b.ram[a-0x8800] = v
	case a >= 0xa000 && a < 0xa400:
		b.vram[a-0xa000] = v
	case a >= 0xa400 && a < 0xa800:
		b.cram[a-0xa400] = v
	case a >= 0xc000 && a < 0xe000:
		o := int(a-0xc000) & 0xfff
		shift := 0
		if a&0x1000 != 0 {
			shift = 4
		}
		b.bgram[o] = b.bgram[o]&^(0x0f<<shift) | (v&15)<<shift
	case a == 0xe000:
		b.protShift = v & 7
	case a == 0xe001:
		b.prot0, b.prot1 = b.prot1, v
	}
}

// ---------------------------------------------------------------- inputs

func bit(on bool, v uint8) uint8 {
	if on {
		return v
	}
	return 0
}

func (b *board) in(p uint8) uint8 {
	switch p {
	case 0:
		return pad(b.p1)
	case 1:
		return pad(b.p2)
	case 2:
		q, r := b.p1, b.p2
		return bit(q["Start (1P)"] || r["Start (1P)"], 0x04) | bit(q["Start (2P)"] || r["Start (2P)"], 0x08) |
			(b.field^1)<<4 | bit(q["Coin 2"], 0x20) | bit(q["Service"], 0x40) | bit(q["Coin"], 0x80)
	case 3:
		return b.ay.ReadData()
	}
	return 0xff
}

func pad(p arcade.Pad) uint8 {
	return bit(p["Right"], 1) | bit(p["Left"], 2) | bit(p["Up"], 4) | bit(p["Down"], 8) | bit(p["Punch"], 0x10)
}

func (b *board) dsw1() int {
	d := b.dips
	return d["lives"]&3 | (d["difficulty"]&3)<<2 | (d["bonus"]&3)<<4 | (d["demosound"]&1)<<6 | (d["cabinet"]&1)<<7
}

// dsw0: the coin switches, the copyright text jumpers, and one of the other
// switches (picked through the sound chip's port B).
func (b *board) dsw0() uint8 {
	d := b.dips
	return uint8(d["coins"]&15) | 0x10 | uint8(d["copyright"]&3)<<5 | uint8(b.dsw1()>>b.dswBit&1)<<7
}

func (b *board) SetInput(p []arcade.Pad) {
	b.p1 = p[0]
	b.p2 = arcade.Pad{}
	if len(p) > 1 && p[1] != nil {
		b.p2 = p[1]
	}
}

// ---------------------------------------------------------------- video

// resNet is MAME's model of the colour output (compute_res_net): TTL PROM
// outputs through resistors, a 470 ohm (680 for blue) bias to 5 V, a
// Darlington amplifier and Nintendo's inverting monitor input.
func resNet(inputs int, r []float64, rBias float64) int {
	const vcc, vOL, vOH, ttlHRes, minout = 5.0, 0.05, 4.0, 50.0, 0.7
	rTotal, v := 0.0, 0.0
	for i, x := range r {
		if x != 0 && inputs>>i&1 == 0 {
			rTotal += 1 / x
			v += vOL / x
		}
	}
	rTotal += 1 / rBias
	v += vcc / rBias
	open := v/rTotal > vOH
	for i, x := range r {
		if x != 0 && inputs>>i&1 != 0 && !open {
			rTotal += 1 / (x + ttlHRes)
			v += vOH / (x + ttlHRes)
		}
	}
	v /= rTotal
	v = max(minout, v)
	v = vcc - v
	v = max(0, v-0.7)
	v = min(v, vcc-1.4)
	v = v / (vcc - 1.4) * vcc
	return int(v*255/vcc + 0.4)
}

var (
	txtR = []float64{1000, 470, 220}
	txtB = []float64{470, 220}
	bakR = []float64{1200, 680, 470}
	bakB = []float64{680, 470}
)

func colour(c uint8, rg, bl []float64) uint32 {
	return arcade.RGB(resNet(int(c&7), rg, 470), resNet(int(c>>3&7), rg, 470), resNet(int(c>>6&3), bl, 680))
}

// updatePalette: pens 0-15 background, 16-47 characters (pairs: black, the
// colour), 48-79 sprites; bit 3 of the palette bank picks the background and
// character PROM halves, bits 0-2 the sprite block.
func (b *board) updatePalette() {
	h := int(b.palBank&8) << 1
	for i := 0; i < 16; i++ {
		b.pens[i] = colour(b.bgProm[h+i], bakR, bakB)
		b.pens[16+2*i] = 0
		b.pens[17+2*i] = colour(b.charProm[h+i], txtR, txtB)
	}
	s := int(b.palBank&7) * 32
	for i := 0; i < 32; i++ {
		b.pens[48+i] = colour(b.sprProm[s+i], txtR, txtB)
	}
}

func (b *board) render() {
	b.updatePalette()
	scr := b.screen
	for y := top; y < top+scrH; y++ {
		row := scr[(y-top)*scrW : (y-top+1)*scrW]
		// the background: 4-bit pixels, 2x2 screen pixels each
		yy := y / 2
		if b.flip {
			yy ^= 0xff
		}
		rovi := yy + int(b.scroll[1])
		shift := 0
		if rovi&0x80 != 0 {
			shift = 4
		}
		for x := 0; x < scrW; x++ {
			roh := (0x38 + x/2 + int(b.scroll[0])) & 0xff
			o := 0
			if rovi&0x100 != 0 {
				o = (rovi>>1&0x3f)<<6 | roh>>2&0x3f
			}
			row[x] = b.pens[b.bgram[o]>>shift&15]
		}
		b.spriteLine(y, row)
		b.charLine(y, row)
	}
}

// spriteLine draws the sprites on line y: one per 4-pixel column slot (a
// later entry takes an earlier one's slot), and a sprite's see-through
// pixels also wipe what earlier sprites drew there.
func (b *board) spriteLine(y int, row []uint32) {
	type attr struct {
		row, sx      int
		colour       uint8
		code         int
		flipx, flipy int
	}
	var at [64]attr
	s := &b.sprRAM
	for o := 4; o < dmaSize; o += 4 {
		sy := 0x200 - int(s[o+1])*2
		r := y - sy
		if b.flip {
			sy ^= 0x1ff
			r = sy - y
		}
		if r < 0 || r >= 16 {
			continue
		}
		a := &at[s[o]>>2]
		a.sx = int(s[o]) * 2
		a.row = r
		a.code = (int(s[o+2]&0x7f) + int(s[o+3]&0x10)<<3 + int(s[o+3]&0x04)<<6) ^ 0x1ff
		a.colour = s[o+3] & 7
		a.flipx, a.flipy = 0, 0
		if s[o+2]&0x80 != 0 {
			a.flipx = 15
		}
		if s[o+3]&0x08 != 0 {
			a.flipy = 15
		}
	}
	buf := &b.lineBuf
	*buf = [512]uint16{}
	for i := range at {
		a := &at[i]
		if a.colour == 0 {
			continue
		}
		src := b.sprites[(a.code&0x1ff)*256+(a.row^a.flipy)*16:]
		for x := 0; x < 16; x++ {
			px := a.sx + x - 6
			if px < 0 || px >= 512 {
				continue
			}
			if b.flip {
				px ^= 0x1ff
			}
			p := uint16(src[x^a.flipx])
			if p != 0 {
				p = 48 + uint16(a.colour)*4 + p
			}
			buf[px] = p
		}
	}
	for x := 0; x < scrW; x++ {
		if p := buf[x]; p != 0 {
			row[x] = b.pens[p]
		}
	}
}

func (b *board) charLine(y int, row []uint32) {
	ty := y
	if b.flip {
		ty = 511 - y
	}
	for tx := 0; tx < 32; tx++ {
		o := (ty>>4)*32 + tx
		code, col := int(b.vram[o]), int(b.cram[o]&15)
		cy := (ty & 15) >> 1
		for x := 0; x < 16; x++ {
			sx := tx*16 + x
			cx := x >> 1
			if b.flip {
				sx = 511 - sx
			}
			if b.chars[code*64+cy*8+cx] != 0 {
				row[sx] = b.pens[17+2*col]
			}
		}
	}
}

func (b *board) Screen() []uint32 { return b.screen }

// ---------------------------------------------------------------- timing

func (b *board) vblankStart() {
	b.render()
	copy(b.sprRAM[:], b.ram[0x400:0x400+dmaSize])
	copy(b.scroll[:], b.ram[0x400:0x403])
	b.palBank = b.ram[0x403]
	b.field ^= 1
	if b.nmiOn && !b.nmiLine {
		b.nmiLine = true
		b.cpu.NMI()
	}
	// the watchdog counts frames while it is armed and the NMI is off
	c := b.wdCount
	if b.nmiOn || !b.wdOn {
		c = 0
	} else {
		c = (c + 1) & 15
	}
	if (c^b.wdCount)&4 != 0 {
		if c&4 != 0 {
			b.cpu.Reset()
			b.ay.Reset()
			b.nmiOn, b.nmiLine, b.wdOn = false, false, false
			b.held = true
		} else {
			b.held = false
		}
	}
	b.wdCount = c
}

func (b *board) RunFrame() {
	cpu := b.cpu
	for line := 0; line < lines; line++ {
		if line == vblank {
			b.vblankStart()
		}
		end := (line + 1) * frameCycles / lines
		for b.cycles < end {
			if b.held {
				b.cycles = end
				break
			}
			b.cycles += cpu.Step()
			// the I register is on the address bus at each refresh
			iv := cpu.IReg()
			on := iv&1 != 0
			if on != b.nmiOn {
				b.nmiOn = on
				if !on {
					b.nmiLine = false
				}
			}
			b.wdOn = iv&2 != 0
		}
		// the sound chip runs at 2 MHz
		t := (line + 1) * frameCycles / 2 / lines
		b.ay.Run(t - b.ayFrac)
		b.ayFrac = t
		b.out.Add(b.ay.Take() / 3 * 0.6)
	}
	b.cycles -= frameCycles
	b.ayFrac = 0
	b.frame++
}

func (b *board) TakeAudio() []float32 { return b.out.Take() }

// ---------------------------------------------------------------- state

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.cram[:])
	w.Bytes(b.bgram[:])
	w.Bytes(b.sprRAM[:])
	w.Bytes(b.scroll[:])
	w.Bytes([]uint8{b.palBank, b.prot0, b.prot1, b.protShift, b.dswBit, b.wdCount, b.field})
	w.Bool(b.flip)
	w.Bool(b.nmiOn)
	w.Bool(b.nmiLine)
	w.Bool(b.wdOn)
	w.Bool(b.held)
	w.Ints([]int{b.cycles, b.ayFrac, b.frame})
	w.B = append(w.B, b.ay.AppendState(nil)...)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *board) LoadState(s []byte) error {
	rest, err := b.cpu.LoadState(s)
	if err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.cram[:])
	r.Bytes(b.bgram[:])
	r.Bytes(b.sprRAM[:])
	r.Bytes(b.scroll[:])
	v := make([]uint8, 7)
	r.Bytes(v)
	b.palBank, b.prot0, b.prot1, b.protShift, b.dswBit, b.wdCount, b.field = v[0], v[1], v[2], v[3], v[4], v[5], v[6]
	b.flip = r.Bool()
	b.nmiOn = r.Bool()
	b.nmiLine = r.Bool()
	b.wdOn = r.Bool()
	b.held = r.Bool()
	m := make([]int, 3)
	r.Ints(m)
	b.cycles, b.ayFrac, b.frame = m[0], m[1], m[2]
	rest2, err := b.ay.LoadState(r.B)
	if err != nil {
		return err
	}
	r.B = rest2
	b.out.LoadState(r)
	b.out.Clear()
	b.render()
	return r.Done()
}
