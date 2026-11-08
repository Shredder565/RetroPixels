package boards

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/konami"
	"retropixels/cores/m68k"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// ---- X-Men (Konami, 1992): 2 and 4 player cabinets ----
//
// A 68000 at 16 MHz with a K052109 / K051962 tilemap pair, K053246 / K053247
// sprites and a K053251 priority mixer; a Z80 at 8 MHz with a YM2151 and a
// K054539 PCM chip, talking to the 68000 through a K054321. The serial
// EEPROM keeps the settings.

const (
	xmLineCyc = 1024 // 68000 cycles a line (384 pixels at 6 MHz)
	xmLines   = 264
	xmW       = 304
	xmH       = 224
	xmTop     = 16
	xmLeft    = 104 // the K052109's own x of the first pixel shown
)

var (
	xmCPUHz       = float64(16000000)
	xmZ80Hz       = float64(8000000)
	xmYMHz        = float64(4000000)
	xmPCMHz       = float64(18432000) / 384
	xmFPS         = xmCPUHz / (float64(xmLineCyc) * float64(xmLines))
	xmSndCyc      = float64(xmLineCyc) * xmZ80Hz / xmCPUHz // Z80 cycles a line (512)
	xmGain        = 1.4
	xmSetupFrames = 60
)

type xmen struct {
	game       *arcade.Game
	sampleRate int
	players    int
	rom        []byte
	sndRom     []byte
	spriteRom  []byte
	tileRom    []byte
	ram1       [0x1000]uint8 // 101000-101fff
	ram2       [0x4000]uint8 // 110000-113fff
	palRam     [0x1000]uint8
	sndRam     [0x2000]uint8
	palette    [2048]uint32
	screen     []uint32

	k052109 *konami.K052109
	k053247 *konami.K053245
	k053251 *konami.K053251
	k054539 *konami.K054539
	ym      *ym2151.YM2151
	eeprom  *byteEeprom
	cpu     *m68k.CPU
	z80     *z80.CPU

	layerBase  [3]int
	layerPri   [3]int // sorted, back to front
	spriteBase int
	scroll     [2][0x400]uint8 // the K052109's scroll registers as at the end of vblank
	scrollCtl  uint8

	layerPens  [3][]uint16
	sprPenData []uint8 // the sprite ROM, a byte a pixel, 256 a tile
	sprList    []int
	mixPens    []uint16
	mixPri     []uint8
	mixShade   []bool
	inputs     []arcade.Pad
	resampler  *arcade.Resampler

	ctrl                 int // 108000
	irq3, irq5, sndIRQ   bool
	latch                [3]uint8 // K054321: 0, 1 to the Z80, 2 back
	volume, active       int
	sndBank              int
	zCyc, pcmFrac, ymAcc float64
	cyc                  int
	setupFrames          int // a blank EEPROM: hold the test switch while it boots, so the game sets it up
}

type xmMain struct{ b *xmen }
type xmSound struct{ b *xmen }

func newXmen(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &xmen{game: g, sampleRate: opts.SampleRate, players: g.Players}
	if b.sampleRate == 0 {
		b.sampleRate = 48000
	}
	if b.players == 0 {
		b.players = 4
	}
	b.rom = make([]byte, 0x100000)
	copy(b.rom, interleaveRole(g, files, "cpu", 1, 2))
	b.sndRom = arcade.Region(g, files, "sound", 0x20000)
	b.tileRom = interleaveRole(g, files, "tiles", 2, 4)
	b.spriteRom = interleaveRole(g, files, "sprites", 2, 8)
	pcm := arcade.Region(g, files, "pcm", 0x200000)
	b.screen = make([]uint32, xmW*xmH)

	b.k052109 = konami.NewK052109(konami.K052109Opts{
		ROM: b.tileRom, ExtraCode: true,
		TileCallback: func(t *konami.Tile) {
			c := t.Color
			if t.Layer == 0 {
				t.Color = b.layerBase[0] + (c&0xf0)>>4
			} else {
				t.Color = b.layerBase[t.Layer] + (c&0x7c)>>2
			}
			t.FlipX = false
		},
		// the ROM test: X-Men's tile codes are 16 bits, bank register 1e00 the top byte
		RomRead: func(off int, k *konami.K052109) int {
			a := (int(k.Ram[0x1e00])<<8|(off&0x1fff)>>5)<<5 | off&0x1f
			return int(b.tileRom[a%len(b.tileRom)])
		},
	})
	b.k052109.AddrMap = true
	b.k052109.DX = [3]int{0, -6, -6}
	b.k053247 = konami.NewK053245(konami.K053245Opts{Type: "053247"}) // its RAM and registers; drawSprites draws
	b.k053251 = &konami.K053251{}
	b.k054539 = konami.NewK054539(pcm)
	b.ym = ym2151.New(xmYMHz)
	var nv []byte
	if len(opts.SRAM) >= 128 {
		nv = opts.SRAM
	}
	if nv == nil {
		if d := roleFileIfPresent(g, files, "eeprom"); len(d) >= 128 {
			nv = d
		}
	}
	b.eeprom = newByteEeprom(nv)
	b.cpu = m68k.New(xmMain{b})
	b.z80 = z80.New(xmSound{b})
	n := xmW * xmH
	for i := range b.layerPens {
		b.layerPens[i] = make([]uint16, n)
	}
	b.mixPens, b.mixPri, b.mixShade = make([]uint16, n), make([]uint8, n), make([]bool, n)
	b.sprPenData = konami.DecodeK053245(b.spriteRom, "packed")
	b.sprList = make([]int, 0, 256)
	b.resampler = arcade.NewResampler(xmPCMHz, float64(b.sampleRate))
	b.Reset()
	return b, nil
}

func (b *xmen) Reset() {
	b.ram1, b.ram2, b.palRam, b.sndRam = [0x1000]uint8{}, [0x4000]uint8{}, [0x1000]uint8{}, [0x2000]uint8{}
	b.palette = [2048]uint32{}
	b.scroll, b.scrollCtl = [2][0x400]uint8{}, 0
	b.k052109.Reset()
	b.k053247.Reset()
	b.k053251.Reset()
	b.k054539.Reset()
	b.ym.Reset()
	b.layerBase, b.layerPri, b.spriteBase = [3]int{}, [3]int{}, 0
	b.irq3, b.irq5, b.sndIRQ = false, false, false
	b.latch, b.volume, b.active, b.sndBank = [3]uint8{}, 0, 0, 0
	b.control(0, 0xffff)
	b.updateIRQ()
	b.cpu.Reset()
	b.z80.Reset()
	b.z80.IRQ = false
	b.zCyc, b.pcmFrac, b.ymAcc, b.cyc = 0, 0, 0, 0
	b.setupFrames = 0
	if allFF(b.eeprom.bytes[:]) {
		b.setupFrames = xmSetupFrames
	}
	b.resampler.Clear()
}

func (b *xmen) SetInput(pads []arcade.Pad) { b.inputs = pads }

func (b *xmen) pad(p int) arcade.Pad {
	if p < len(b.inputs) && p < b.players {
		return b.inputs[p]
	}
	return nil
}

// playerByte: up, down, left, right, attack, jump, (button 3), coin; low = pressed.
func (b *xmen) playerByte(p int) int {
	i := b.pad(p)
	v := 0
	for k, n := range []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3"} {
		if i[n] {
			v |= 1 << k
		}
	}
	if i["Coin"] || (p == 1 && b.pad(0)["Coin 2"]) {
		v |= 0x80
	}
	return ^v & 0xff
}

// systemWord (10a004): services 1-4, EEPROM data and ready, starts 1-4, test.
func (b *xmen) systemWord() uint16 {
	v := 0
	for p := 0; p < 4; p++ {
		i := b.pad(p)
		if i["Service"] {
			v |= 1 << p
		}
		if i["Start (1P)"] || (p == 1 && b.pad(0)["Start (2P)"]) {
			v |= 0x100 << p
		}
	}
	if p0 := b.pad(0); (p0["Service"] && p0["Start (1P)"]) || b.setupFrames > 0 {
		v |= 0x4000 // hold Service + Start for the test menu
	}
	w := ^v & 0xffff
	w &^= 0x40
	w |= b.eeprom.read() << 6
	return uint16(w | 0x80)
}

func (b *xmen) updateIRQ() {
	switch {
	case b.irq5:
		b.cpu.SetIRQ(5)
	case b.irq3:
		b.cpu.SetIRQ(3)
	default:
		b.cpu.SetIRQ(0)
	}
}

// control: the write to 108000 (mask: the bytes written).
func (b *xmen) control(v, mask int) {
	b.ctrl = b.ctrl&^mask | v&mask
	if mask&0xff != 0 {
		b.eeprom.write(v&0x10 != 0, v&0x08 != 0, v&0x04 != 0)
		if v&0x20 == 0 && b.irq5 {
			b.irq5 = false
			b.updateIRQ()
		}
	}
	if mask&0xff00 != 0 {
		b.k052109.Rmrd = v&0x200 != 0
		snd := v&0x400 != 0
		if snd && !b.sndIRQ {
			b.z80.IRQ = true
		}
		b.sndIRQ = snd
	}
}

func (b *xmen) k052109Write(o int, v uint8) {
	if o == 0x1d00 && b.irq3 && b.k052109.Ram[0x1d00]&^v&4 != 0 {
		b.irq3 = false
		b.updateIRQ()
	}
	b.k052109.Write(o, v)
}

func (m xmMain) Read16(a uint32) uint16 {
	b := m.b
	a &= 0xffffff
	switch {
	case a < 0x100000:
		return uint16(b.rom[a])<<8 | uint16(b.rom[a+1])
	case a >= 0x100000 && a < 0x101000:
		o := a - 0x100000
		return uint16(b.k053247.Ram[o])<<8 | uint16(b.k053247.Ram[o+1])
	case a >= 0x110000 && a < 0x114000:
		o := a - 0x110000
		return uint16(b.ram2[o])<<8 | uint16(b.ram2[o+1])
	case a >= 0x104000 && a < 0x105000:
		o := a - 0x104000
		return uint16(b.palRam[o])<<8 | uint16(b.palRam[o+1])
	case a == 0x10a004:
		return b.systemWord()
	}
	return uint16(m.Read8(a))<<8 | uint16(m.Read8(a+1))
}

func (m xmMain) Read8(a uint32) uint8 {
	b := m.b
	a &= 0xffffff
	switch {
	case a < 0x100000:
		return b.rom[a]
	case a >= 0x100000 && a < 0x101000:
		return b.k053247.Ram[a-0x100000]
	case a >= 0x101000 && a < 0x102000:
		return b.ram1[a-0x101000]
	case a >= 0x104000 && a < 0x105000:
		return b.palRam[a-0x104000]
	case a >= 0x110000 && a < 0x114000:
		return b.ram2[a-0x110000]
	case a >= 0x18c000 && a < 0x198000:
		if a&1 == 0 {
			return 0
		}
		return b.k052109.Read(int(a-0x18c000) >> 1)
	case a >= 0x108040 && a < 0x108060:
		if a&1 != 0 && (a-0x108040)>>1 == 0xa {
			return b.latch[2]
		}
		return 0
	case a == 0x10a000:
		return uint8(b.playerByte(3))
	case a == 0x10a001:
		return uint8(b.playerByte(1))
	case a == 0x10a002:
		return uint8(b.playerByte(2))
	case a == 0x10a003:
		return uint8(b.playerByte(0))
	case a == 0x10a004:
		return uint8(b.systemWord() >> 8)
	case a == 0x10a005:
		return uint8(b.systemWord())
	case a == 0x10a00c || a == 0x10a00d:
		if b.ctrl&0x100 == 0 {
			return 0
		}
		r := &b.k053247.Regs
		addr := int(r[6])<<17 | int(r[7])<<9 | int(r[4])<<1 | int(a&1) ^ 1
		return b.spriteRom[addr&(len(b.spriteRom)-1)]
	}
	return 0xff
}

func (m xmMain) Write16(a uint32, v uint16) {
	b := m.b
	a &= 0xffffff
	switch {
	case a >= 0x100000 && a < 0x101000:
		o := a - 0x100000
		b.k053247.Ram[o], b.k053247.Ram[o+1] = uint8(v>>8), uint8(v)
		return
	case a >= 0x110000 && a < 0x114000:
		o := a - 0x110000
		b.ram2[o], b.ram2[o+1] = uint8(v>>8), uint8(v)
		return
	case a >= 0x104000 && a < 0x105000:
		o := a - 0x104000
		b.palRam[o], b.palRam[o+1] = uint8(v>>8), uint8(v)
		b.updateColour(int(o >> 1))
		return
	case a == 0x108000:
		b.control(int(v), 0xffff)
		return
	}
	m.Write8(a, uint8(v>>8))
	m.Write8(a+1, uint8(v))
}

func (m xmMain) Write8(a uint32, v uint8) {
	b := m.b
	a &= 0xffffff
	switch {
	case a >= 0x100000 && a < 0x101000:
		b.k053247.Ram[a-0x100000] = v
	case a >= 0x101000 && a < 0x102000:
		b.ram1[a-0x101000] = v
	case a >= 0x104000 && a < 0x105000:
		b.palRam[a-0x104000] = v
		b.updateColour(int(a-0x104000) >> 1)
	case a >= 0x110000 && a < 0x114000:
		b.ram2[a-0x110000] = v
	case a >= 0x18c000 && a < 0x198000:
		if a&1 != 0 {
			b.k052109Write(int(a-0x18c000)>>1, v)
		}
	case a == 0x108000:
		b.control(int(v)<<8, 0xff00)
	case a == 0x108001:
		b.control(int(v), 0x00ff)
	case a >= 0x108020 && a < 0x108028:
		b.k053247.Regs[a-0x108020] = v
	case a >= 0x108040 && a < 0x108060:
		if a&1 == 0 {
			return
		}
		switch (a - 0x108040) >> 1 {
		case 0:
			b.active = int(v)
		case 2:
			b.volume = 0
		case 3:
			if v != 0 && b.volume < 64 {
				b.volume++
			}
		case 6:
			b.latch[0] = v
		case 7:
			b.latch[1] = v
		}
	case a >= 0x108060 && a < 0x108080:
		if a&1 != 0 {
			b.k053251.Write(int(a-0x108060)>>1, v)
		}
	}
}

// Ack: the board's interrupts are autovectored and stay on until cleared.
func (m xmMain) Ack(level int) int { return -1 }

func (b *xmen) updateColour(i int) {
	v := int(b.palRam[i*2])<<8 | int(b.palRam[i*2+1])
	c5 := func(x int) int { return x<<3 | x>>2 }
	b.palette[i] = arcade.RGB(c5(v&31), c5((v>>5)&31), c5((v>>10)&31))
}

func (s xmSound) Read(addr uint16) uint8 {
	b, a := s.b, int(addr)
	switch {
	case a < 0x8000:
		return b.sndRom[a]
	case a < 0xc000:
		return b.sndRom[(b.sndBank*0x4000+(a-0x8000))%len(b.sndRom)]
	case a < 0xe000:
		return b.sndRam[a-0xc000]
	case a < 0xe230:
		return b.k054539.Read(a - 0xe000)
	case a&^0x400 == 0xe800 || a&^0x400 == 0xe801:
		return b.ym.Read()
	case a == 0xf002:
		return b.latch[0]
	case a == 0xf003:
		return b.latch[1]
	}
	return 0xff
}

func (s xmSound) Write(addr uint16, v uint8) {
	b, a := s.b, int(addr)
	switch {
	case a >= 0xc000 && a < 0xe000:
		b.sndRam[a-0xc000] = v
	case a >= 0xe000 && a < 0xe230:
		b.k054539.Write(a-0xe000, v)
	case a&^0x400 == 0xe800 || a&^0x400 == 0xe801:
		b.ym.Write(a&1, v)
	case a == 0xf000:
		b.latch[2] = v
	case a == 0xf800:
		b.sndBank = int(v & 7)
	}
}

func (s xmSound) In(uint16) uint8   { return 0xff }
func (s xmSound) Out(uint16, uint8) {}
func (s xmSound) Ack()              { s.b.z80.IRQ = false }

// runSound runs the Z80 for a line and makes the line's sound.
func (b *xmen) runSound(cycles float64) {
	end := b.zCyc + cycles
	for b.zCyc < end {
		b.zCyc += float64(b.z80.Step())
	}
	b.pcmFrac += cycles * xmPCMHz / xmZ80Hz
	n := int(math.Floor(b.pcmFrac))
	b.pcmFrac -= float64(n)
	ymRate := b.ym.Rate()
	gain := 0.0
	if b.ctrl&0x800 != 0 { // bit 11 low mutes the amplifier
		vol := math.Pow(2, float64(b.volume-40)/10)
		gain = vol * float64(b.active&1+(b.active>>1)&1) / 2 * xmGain
	}
	for i := 0; i < n; i++ {
		l, r := b.k054539.Sample()
		b.ymAcc += ymRate / xmPCMHz
		ym, k := 0.0, 0
		for b.ymAcc >= 1 {
			b.ymAcc--
			b.ym.Tick()
			ym += float64(b.ym.Out[0] + b.ym.Out[1])
			k++
		}
		if k > 0 {
			ym /= float64(k)
		}
		b.resampler.Add(gain * (ym/32768*0.20 + l + r) / 2)
	}
}

func (b *xmen) drawFrame() {
	k51, n := b.k053251, xmW*xmH
	// the K052109's layers are on the K053251's inputs 3, 0 and 2
	ci := [3]int{3, 0, 2}
	b.spriteBase = k51.PaletteBase(1)
	order := [3]int{0, 1, 2}
	var pri [3]int
	for i := 0; i < 3; i++ {
		b.layerBase[i] = k51.PaletteBase(ci[i])
		pri[i] = k51.Priority(ci[i])
	}
	sw := func(a, c int) {
		if pri[a] < pri[c] {
			pri[a], pri[c] = pri[c], pri[a]
			order[a], order[c] = order[c], order[a]
		}
	}
	sw(0, 1)
	sw(0, 2)
	sw(1, 2)
	b.layerPri = pri
	// the scroll registers as they were when the picture started
	ram := &b.k052109.Ram
	var now [2][0x400]uint8
	copy(now[0][:], ram[0x1800:0x1c00])
	copy(now[1][:], ram[0x3800:0x3c00])
	ctl := ram[0x1c80]
	copy(ram[0x1800:0x1c00], b.scroll[0][:])
	copy(ram[0x3800:0x3c00], b.scroll[1][:])
	ram[0x1c80] = b.scrollCtl
	for l := 0; l < 3; l++ {
		clear(b.layerPens[l])
		b.k052109.Render(l, b.layerPens[l], xmW, xmH, konami.RenderOpts{X0: xmLeft, Y0: xmTop})
	}
	copy(ram[0x1800:0x1c00], now[0][:])
	copy(ram[0x3800:0x3c00], now[1][:])
	ram[0x1c80] = ctl

	// the layers, back to front, with MAME's priority bits (1, 2, 4) where each is solid
	bg := uint16(16*k51.PaletteBase(4) + 1)
	l0, l1, l2 := b.layerPens[order[0]], b.layerPens[order[1]], b.layerPens[order[2]]
	pens, mpri, shade := b.mixPens, b.mixPri, b.mixShade
	for i := 0; i < n; i++ {
		pen, p := bg, uint8(0)
		if v := l0[i]; v&15 != 0 {
			pen, p = v, 1
		}
		if v := l1[i]; v&15 != 0 {
			pen, p = v, p|2
		}
		if v := l2[i]; v&15 != 0 {
			pen, p = v, p|4
		}
		pens[i], mpri[i], shade[i] = pen, p, false
	}
	b.drawSprites()
	for i := 0; i < n; i++ {
		c := b.palette[pens[i]&2047]
		if shade[i] {
			c = uint32(simShade[(c>>16)&255])<<16 | uint32(simShade[(c>>8)&255])<<8 | uint32(simShade[c&255])
		}
		b.screen[i] = c
	}
}

var (
	xmXOff = [8]int{0, 1, 4, 5, 16, 17, 20, 21}
	xmYOff = [8]int{0, 2, 8, 10, 32, 34, 40, 42}
)

// drawSprites draws the K053247's list over the layers the way MAME does:
// front sprite first, each pixel drawn once, hidden behind the layers its
// priority puts it behind.
func (b *xmen) drawSprites() {
	ram, regs := b.k053247.Ram, &b.k053247.Regs
	word := func(i int) int { return int(ram[i*2])<<8 | int(ram[i*2+1]) }
	list := b.sprList[:0]
	for offs := 0; offs < 0x800; offs += 8 {
		if word(offs)&0x8000 != 0 {
			list = append(list, offs)
		}
	}
	// descending z (smaller z is closer), as the chip's sort leaves it
	for y := 0; y < len(list)-1; y++ {
		offs := list[y]
		z := word(offs) & 0xff
		for x := y + 1; x < len(list); x++ {
			t := list[x]
			if c := word(t) & 0xff; z <= c {
				z = c
				list[x] = offs
				list[y], offs = t, t
			}
		}
	}
	b.sprList = list
	flipSX, flipSY := regs[5]&1 != 0, regs[5]&2 != 0
	offx := int(int16(uint16(regs[0])<<8 | uint16(regs[1])))
	offy := int(int16(uint16(regs[2])<<8 | uint16(regs[3])))
	for k := len(list) - 1; k >= 0; k-- {
		offs := list[k]
		code := word(offs + 1)
		attr := word(offs + 6)
		// the board's callback
		p := (attr & 0xe0) >> 4
		var primask uint32
		switch {
		case p <= b.layerPri[2]:
		case p <= b.layerPri[1]:
			primask = 0xf0
		case p <= b.layerPri[0]:
			primask = 0xf0 | 0xcc
		default:
			primask = 0xf0 | 0xcc | 0xaa
		}
		color := b.spriteBase + attr&0x1f
		shadow := (attr >> 10) & 3

		xa, ya := 0, 0
		if code&1 != 0 {
			xa++
		}
		if code&2 != 0 {
			ya++
		}
		if code&4 != 0 {
			xa += 2
		}
		if code&8 != 0 {
			ya += 2
		}
		if code&0x10 != 0 {
			xa += 4
		}
		if code&0x20 != 0 {
			ya += 4
		}
		code &^= 0x3f
		w0 := word(offs)
		oy, ox := word(offs+2)&0x3ff, word(offs+3)&0x3ff
		scaley := word(offs+4) & 0x3ff
		zoomy := 0x800000
		if scaley != 0 {
			zoomy = (0x400000 + scaley>>1) / scaley
		}
		scalex, zoomx := scaley, zoomy
		if w0&0x4000 == 0 {
			scalex = word(offs+5) & 0x3ff
			zoomx = 0x800000
			if scalex != 0 {
				zoomx = (0x400000 + scalex>>1) / scalex
			}
		}
		nozoom := scalex == 0x40 && scaley == 0x40
		flipx, flipy := w0&0x1000 != 0, w0&0x2000 != 0
		mirrorx, mirrory := attr&0x4000 != 0, attr&0x8000 != 0
		if mirrorx {
			flipx = false
		}
		if flipSX {
			ox = -ox
			if !mirrorx {
				flipx = !flipx
			}
		}
		if flipSY {
			oy = -oy
			if !mirrory {
				flipy = !flipy
			}
		}
		ox = (ox - offx) & 1023
		oy = (-oy - offy) & 1023
		if ox >= 1024-384 {
			ox -= 1024
		}
		if oy >= 1024-512 {
			oy -= 1024
		}
		width, height := 1<<((w0>>8)&3), 1<<((w0>>10)&3)
		ox += -43 // the board's sprite offsets (MAME's set_config)
		oy -= -2
		ox -= (zoomx * width) >> 13
		oy -= (zoomy * height) >> 13
		// to the picture's own pixels (the visible area starts at 8, 16)
		ox -= 8
		oy -= xmTop
		for y := 0; y < height; y++ {
			sy := oy + ((zoomy*y + 1<<11) >> 12)
			zh := oy + ((zoomy*(y+1) + 1<<11) >> 12) - sy
			for x := 0; x < width; x++ {
				sx := ox + ((zoomx*x + 1<<11) >> 12)
				zw := ox + ((zoomx*(x+1) + 1<<11) >> 12) - sx
				tc := code
				var fx, fy bool
				if mirrorx {
					if !flipx != (x<<1 < width) {
						tc += xmXOff[(width-1-x+xa)&7]
						fx = true
					} else {
						tc += xmXOff[(x+xa)&7]
					}
				} else {
					if flipx {
						tc += xmXOff[(width-1-x+xa)&7]
					} else {
						tc += xmXOff[(x+xa)&7]
					}
					fx = flipx
				}
				if mirrory {
					if !flipy != (y<<1 >= height) {
						tc += xmYOff[(height-1-y+ya)&7]
						fy = true
					} else {
						tc += xmYOff[(y+ya)&7]
					}
				} else {
					if flipy {
						tc += xmYOff[(height-1-y+ya)&7]
					} else {
						tc += xmYOff[(y+ya)&7]
					}
					fy = flipy
				}
				if nozoom {
					zw, zh = 16, 16
				}
				b.drawTile(tc, color, shadow != 0, fx, fy, sx, sy, zw, zh, primask)
				if mirrory && height == 1 {
					b.drawTile(tc, color, shadow != 0, fx, !fy, sx, sy, zw, zh, primask)
				}
			}
		}
	}
}

// drawTile is MAME's prio_zoom_transtable for one 16x16 sprite tile scaled
// to zw x zh at (sx, sy): pen 0 is clear, pen 15 of a shadow sprite darkens.
func (b *xmen) drawTile(code, color int, shadow, flipx, flipy bool, sx, sy, zw, zh int, primask uint32) {
	if zw < 1 || zh < 1 {
		return
	}
	tiles := len(b.sprPenData) >> 8
	src := b.sprPenData[(code%tiles)<<8:]
	dx, dy := (16<<16)/zw, (16<<16)/zh
	x0, x1, y0, y1 := sx, sx+zw-1, sy, sy+zh-1
	if x0 >= xmW || x1 < 0 || y0 >= xmH || y1 < 0 {
		return
	}
	srcx, srcy := 0, 0
	if x0 < 0 {
		srcx, x0 = -x0*dx, 0
	}
	x1 = min(x1, xmW-1)
	if y0 < 0 {
		srcy, y0 = -y0*dy, 0
	}
	y1 = min(y1, xmH-1)
	if flipx {
		srcx, dx = (zw-1)*dx-srcx, -dx
	}
	if flipy {
		srcy, dy = (zh-1)*dy-srcy, -dy
	}
	pmask := primask | 1<<31
	base := uint16(color%128) << 4
	for y := y0; y <= y1; y++ {
		row := src[(srcy>>16)<<4:]
		srcy += dy
		cx := srcx
		at := y * xmW
		for x := x0; x <= x1; x, cx = x+1, cx+dx {
			pen := row[cx>>16]
			if pen == 0 {
				continue
			}
			i := at + x
			p := b.mixPri[i]
			if pen == 15 && shadow {
				if p&0x80 == 0 && (1<<(p&0x1f))&pmask == 0 {
					b.mixShade[i] = true
					b.mixPri[i] = p | 0x80
				}
				continue
			}
			if (1<<(p&0x1f))&pmask == 0 {
				b.mixPens[i] = base + uint16(pen)
				b.mixShade[i] = false
			}
			b.mixPri[i] = 31
		}
	}
}

func (b *xmen) RunFrame() {
	for line := 0; line < xmLines; line++ {
		switch line {
		case 0:
			if b.ctrl&0x20 != 0 && b.k053247.DMAEnabled() {
				b.irq5 = true
				b.updateIRQ()
			}
		case xmTop: // end of vblank: the K052109 takes its scroll registers
			copy(b.scroll[0][:], b.k052109.Ram[0x1800:0x1c00])
			copy(b.scroll[1][:], b.k052109.Ram[0x3800:0x3c00])
			b.scrollCtl = b.k052109.Ram[0x1c80]
		case xmTop + xmH:
			b.drawFrame()
			if b.k052109.IRQEnabled() {
				b.irq3 = true
				b.updateIRQ()
			}
		}
		end := (line + 1) * xmLineCyc
		for b.cyc < end {
			b.cyc += b.cpu.Step()
		}
		b.runSound(xmSndCyc)
	}
	b.cyc -= xmLines * xmLineCyc
	if b.setupFrames > 0 {
		b.setupFrames--
	}
}

func (b *xmen) TakeAudio() []float32 { return b.resampler.Take() }
func (b *xmen) Screen() []uint32     { return b.screen }
func (b *xmen) SRAM() []byte         { return append([]byte(nil), b.eeprom.bytes[:]...) }

const xmStateMagic = "XMEN\x01"

func (b *xmen) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(xmStateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.B = b.z80.AppendState(w.B)
	w.Bytes(b.ram1[:])
	w.Bytes(b.ram2[:])
	w.Bytes(b.palRam[:])
	w.Bytes(b.sndRam[:])
	w.Bytes(b.scroll[0][:])
	w.Bytes(b.scroll[1][:])
	w.U8(b.scrollCtl)
	b.k052109.AppendState(w)
	b.k053247.AppendState(w)
	b.k053251.AppendState(w)
	b.k054539.AppendState(w)
	w.B = b.ym.AppendState(w.B)
	b.eeprom.appendState(w)
	w.Bytes(b.latch[:])
	w.Ints([]int{b.ctrl, bit(b.irq3), bit(b.irq5), bit(b.sndIRQ), b.volume, b.active, b.sndBank, b.cyc, bit(b.z80.IRQ)})
	w.F64s([]float64{b.zCyc, b.pcmFrac, b.ymAcc})
	b.resampler.AppendState(w)
	return w.B, nil
}

func (b *xmen) LoadState(data []byte) (err error) {
	if len(data) < len(xmStateMagic) || string(data[:len(xmStateMagic)]) != xmStateMagic {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: data[len(xmStateMagic):]}
	if r.B, err = b.cpu.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.z80.LoadState(r.B); err != nil {
		return err
	}
	r.Bytes(b.ram1[:])
	r.Bytes(b.ram2[:])
	r.Bytes(b.palRam[:])
	r.Bytes(b.sndRam[:])
	r.Bytes(b.scroll[0][:])
	r.Bytes(b.scroll[1][:])
	b.scrollCtl = r.U8()
	b.k052109.LoadState(r)
	b.k052109.DX = [3]int{0, -6, -6}
	b.k053247.LoadState(r)
	b.k053251.LoadState(r)
	b.k054539.LoadState(r)
	if r.Err == nil {
		if r.B, err = b.ym.LoadState(r.B); err != nil {
			return err
		}
	}
	b.eeprom.loadState(r)
	r.Bytes(b.latch[:])
	m := make([]int, 9)
	r.Ints(m)
	f := make([]float64, 3)
	r.F64s(f)
	b.resampler.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	b.ctrl, b.irq3, b.irq5, b.sndIRQ = m[0], m[1] != 0, m[2] != 0, m[3] != 0
	b.volume, b.active, b.sndBank, b.cyc, b.z80.IRQ = m[4], m[5], m[6], m[7], m[8] != 0
	b.zCyc, b.pcmFrac, b.ymAcc = f[0], f[1], f[2]
	b.k052109.Rmrd = b.ctrl&0x200 != 0
	b.updateIRQ()
	for i := 0; i < 2048; i++ {
		b.updateColour(i)
	}
	b.resampler.Clear()
	b.drawFrame()
	return nil
}

func xmenGames() []arcade.Game {
	shared := []arcade.File{
		{Name: "065-a02.9d", Size: 0x40000, Role: "cpu", Offset: 0x80000, Lane: 0},
		{Name: "065-a03.9f", Size: 0x40000, Role: "cpu", Offset: 0x80000, Lane: 1},
		{Name: "065-a01.6f", Size: 0x20000, Role: "sound", Offset: arcade.Next},
		{Name: "065-a08.15l", Size: 0x100000, Role: "tiles", Lane: 0},
		{Name: "065-a07.16l", Size: 0x100000, Role: "tiles", Lane: 2},
		{Name: "065-a09.2h", Size: 0x100000, Role: "sprites", Lane: 0},
		{Name: "065-a10.2l", Size: 0x100000, Role: "sprites", Lane: 2},
		{Name: "065-a12.1h", Size: 0x100000, Role: "sprites", Lane: 4},
		{Name: "065-a11.1l", Size: 0x100000, Role: "sprites", Lane: 6},
		{Name: "065-a06.1f", Size: 0x200000, Role: "pcm", Offset: arcade.Next},
	}
	version := func(id, title, code string, players int) arcade.Game {
		parent := "xmen"
		if id == "xmen" {
			parent = ""
		}
		files := []arcade.File{
			{Name: "065-" + code + "04.10d", Size: 0x20000, Role: "cpu", Offset: 0, Lane: 0},
			{Name: "065-" + code + "05.10f", Size: 0x20000, Role: "cpu", Offset: 0, Lane: 1},
		}
		files = append(files, shared...)
		files = append(files, arcade.File{Name: "xmen_" + code + ".nv", Size: 0x80, Role: "eeprom", Offset: arcade.Next, Optional: true})
		return arcade.Game{ID: id, Title: title, Year: 1992, Maker: "Konami", Players: players, Loose: true, Parent: parent, Files: files}
	}
	return []arcade.Game{
		version("xmen", "X-Men (4 players, Europe EBA)", "eba", 4),
		version("xmenu", "X-Men (4 players, US UBB)", "ubb", 4),
		version("xmenua", "X-Men (4 players, US UEB)", "ueb", 4),
		version("xmenj", "X-Men (4 players, Japan JBA)", "jba", 4),
		version("xmenja", "X-Men (4 players, Japan JEA)", "jea", 4),
		version("xmena", "X-Men (4 players, Asia AEA)", "aea", 4),
		version("xmenaa", "X-Men (4 players, Asia ADA)", "ada", 4),
		version("xmen2pe", "X-Men (2 players, Europe EAA)", "eaa", 2),
		version("xmen2pu", "X-Men (2 players, US UAB)", "uab", 2),
		version("xmen2pj", "X-Men (2 players, Japan JAA)", "jaa", 2),
		version("xmen2pa", "X-Men (2 players, Asia AAA)", "aaa", 2),
	}
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "xmen", Title: "Konami X-Men", Width: xmW, Height: xmH, Rotate: 0, FPS: xmFPS, Players: 4,
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Coin", "Start (1P)", "Start (2P)", "Service"},
		Dips:    []arcade.Dip{}, Games: xmenGames(), New: newXmen,
	})
}
