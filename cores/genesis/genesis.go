// Package genesis is the native Sega Genesis / Mega Drive core: a Go port of
// web/emu/genesis.js and genesis_cart.js that plays exactly like them (same
// picture and sound, frame for frame).
//
// Machine: the Motorola 68000 (cores/m68k), the Zilog Z80 sound processor
// (cores/z80), the Yamaha YM2612 FM chip (cores/ym2612), the SN76489 PSG
// (cores/sn76489) and the Sega VDP (planes A/B, window, sprites,
// shadow/highlight, DMA, interlace). The cartridge hardware (battery saves,
// EEPROMs, bank mappers, J-Cart) is in cart.go. Pads are 6-button, or
// 3-button for games that need it (or when Mode is held at power-on).
package genesis

import (
	"errors"
	"strconv"
	"strings"

	"retropixels/core"
	"retropixels/cores/m68k"
	"retropixels/cores/sn76489"
	"retropixels/cores/ym2612"
	"retropixels/cores/z80"
)

const (
	lineClk = 3420    // master clocks per scan line
	active  = 2560    // where the visible part of a line ends (render + H-interrupt point)
	slice   = 1140    // CPUs are interleaved in slices this long
	ymClks  = 7 * 144 // master clocks per YM2612 sample
	psgClks = 15 * 16 // master clocks per PSG step
)

// Buttons: input bit i is Buttons[i] (as in genesis.js).
var Buttons = []string{"A", "B", "C", "Start", "X", "Y", "Z", "Mode", "Up", "Down", "Left", "Right"}

const (
	bA = 1 << iota
	bB
	bC
	bStart
	bX
	bY
	bZ
	bMode
	bUp
	bDown
	bLeft
	bRight
)

// Genesis is one running console.
type Genesis struct {
	rom      []uint8
	romSize  int
	region   string
	pal      bool
	overseas bool

	sampleRate    int
	mclkRate      float64
	linesPerFrame int
	fps           float64
	nativeRate    float64

	ram   [0x10000]uint8
	zram  [0x2000]uint8
	input [4]uint32
	cart  *cart
	three [2]bool // pads acting as 3-button pads

	vdp vdp
	ym  *ym2612.YM2612
	psg *sn76489.PSG
	cpu *m68k.CPU
	z80 *z80.CPU

	width, height int
	screen        []uint32

	ioData, ioCtrl [3]uint8
	thCount        [2]uint8
	thTime         [2]int64

	busReq, zReset int
	zBank          int
	mclk, zc       int64
	ymNext         int64
	psgAcc         int
	line           int
	lineStart      int64
	vblank         bool
	hvLatch        int
	frame          int
	bootFrames     int

	audioL, audioR                []float32 // native-rate samples not yet taken
	rsPos, prevL, prevR, lpL, lpR float64
	out                           []float32
}

// prepareRom: undo the .smd format (512-byte header, 16 KB blocks with odd /
// even bytes split) and round the size up to a power of two, mirroring an
// odd-sized tail.
func prepareRom(in []byte) (rom []uint8, realSize int) {
	hasSega := len(in) > 0x104 && string(in[0x100:0x104]) == "SEGA"
	if !hasSega && len(in)%16384 == 512 && in[8] == 0xaa && in[9] == 0xbb {
		body := in[512:]
		out := make([]uint8, len(body))
		for b := 0; b < len(body); b += 16384 {
			for i := 0; i < 8192; i++ {
				out[b+i*2+1] = body[b+i]
				out[b+i*2] = body[b+8192+i]
			}
		}
		in = out
	}
	size := 1
	for size < len(in) {
		size <<= 1
	}
	rom = make([]uint8, max(size, 0x10000))
	copy(rom, in)
	for i := len(in); i < size; i++ {
		rom[i] = rom[i-size>>1]
	}
	return rom, len(in)
}

// jsSpace: the characters JS's \s and trim() treat as white space, among
// those a header byte can be.
func jsSpace(r rune) bool {
	return r == 9 || r == 10 || r == 11 || r == 12 || r == 13 || r == 32 || r == 0xa0 ||
		r == 0x1680 || (r >= 0x2000 && r <= 0x200a) || r == 0x2028 || r == 0x2029 || r == 0x202f || r == 0x205f || r == 0x3000 || r == 0xfeff
}

func jsTrim(s []rune) []rune {
	for len(s) > 0 && jsSpace(s[0]) {
		s = s[1:]
	}
	for len(s) > 0 && jsSpace(s[len(s)-1]) {
		s = s[:len(s)-1]
	}
	return s
}

// headerText: header bytes as JS's String.fromCharCode makes them (one
// character per byte).
func headerText(rom []uint8, from, to int) []rune {
	out := make([]rune, 0, to-from)
	for _, b := range rom[from:to] {
		out = append(out, rune(b))
	}
	return out
}

// jsUpper: JS's toUpperCase for the characters a header byte can be.
func jsUpper(s []rune) []rune {
	out := make([]rune, 0, len(s))
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z':
			out = append(out, r-32)
		case r >= 0xe0 && r <= 0xfe && r != 0xf7:
			out = append(out, r-32)
		case r == 0xdf:
			out = append(out, 'S', 'S')
		case r == 0xff:
			out = append(out, 0x178)
		case r == 0xb5:
			out = append(out, 0x39c)
		default:
			out = append(out, r)
		}
	}
	return out
}

func headerRegion(rom []uint8) string {
	s := jsUpper(headerText(rom, 0x1f0, 0x1f3))
	str := string(s)
	t := jsTrim(s)
	if len(t) == 1 && strings.ContainsRune("0123456789ABCDEF", t[0]) && !strings.ContainsAny(str, "JUE") {
		n, _ := strconv.ParseInt(string(t), 16, 32)
		switch {
		case n&4 != 0:
			return "us"
		case n&1 != 0:
			return "jp"
		case n&8 != 0:
			return "eu"
		}
		return "us"
	}
	switch {
	case strings.ContainsRune(str, 'U'):
		return "us"
	case strings.ContainsRune(str, 'J'):
		return "jp"
	case strings.ContainsRune(str, 'E'):
		return "eu"
	}
	return "us"
}

// bus: what the 68000 sees.
type bus struct{ g *Genesis }

func (b bus) Read8(a uint32) uint8       { return uint8(b.g.read8(int(a))) }
func (b bus) Read16(a uint32) uint16     { return uint16(b.g.read16(int(a))) }
func (b bus) Write8(a uint32, v uint8)   { b.g.write8(int(a), int(v)) }
func (b bus) Write16(a uint32, v uint16) { b.g.write16(int(a), int(v)) }
func (b bus) Ack(level int) int          { return b.g.ack(level) }

// zbus: what the Z80 sees.
type zbus struct{ g *Genesis }

func (b zbus) Read(a uint16) uint8     { return uint8(b.g.zread(int(a))) }
func (b zbus) Write(a uint16, v uint8) { b.g.zwrite(int(a), int(v)) }
func (b zbus) In(uint16) uint8         { return 0xff }
func (b zbus) Out(uint16, uint8)       {}
func (b zbus) Vector() uint8           { return 0xff }

// New starts a Genesis game.
func New(romIn []byte, opts core.Options) (core.Core, error) {
	if len(romIn) == 0 {
		return nil, errors.New("this Genesis game file is empty")
	}
	rom, realSize := prepareRom(romIn)
	g := &Genesis{rom: rom, romSize: realSize}
	g.region = headerRegion(rom)
	g.pal = g.region == "eu"
	g.overseas = g.region != "jp"
	g.sampleRate = opts.SampleRate
	if g.sampleRate <= 0 {
		g.sampleRate = 48000
	}
	g.mclkRate = 53693175
	g.linesPerFrame = 262
	if g.pal {
		g.mclkRate, g.linesPerFrame = 53203424, 313
	}
	g.fps = g.mclkRate / float64(lineClk*g.linesPerFrame)
	g.nativeRate = g.mclkRate / ymClks

	c, err := newCart(rom, realSize, opts.SRAM, func(p int) uint32 { return g.input[p] })
	if err != nil {
		return nil, err
	}
	g.cart = c
	g.three = [2]bool{c.threeButton, c.threeButton}
	g.vdp.md = g
	g.ym = ym2612.New()
	g.psg = sn76489.New()
	g.cpu = m68k.New(bus{g})
	g.cpu.TASWrites = false
	g.z80 = z80.New(zbus{g})

	g.width, g.height = 320, 224
	g.screen = make([]uint32, 320*480)
	g.audioL = make([]float32, 0, 2048)
	g.audioR = make([]float32, 0, 2048)
	g.reset(true)
	return g, nil
}

// Reset is the console's reset button (memory is kept, as in the web player).
func (g *Genesis) Reset() { g.reset(false) }

func (g *Genesis) reset(hard bool) {
	if hard {
		g.ram = [0x10000]uint8{}
		g.zram = [0x2000]uint8{}
	}
	g.vdp.reset()
	g.ym.Reset()
	g.psg.Reset()
	g.z80.Reset()
	g.cart.reset()
	g.ioData = [3]uint8{0x7f, 0x7f, 0x7f}
	g.ioCtrl = [3]uint8{}
	g.thCount = [2]uint8{}
	g.thTime = [2]int64{}
	g.busReq, g.zReset, g.zBank = 0, 1, 0
	g.mclk, g.zc, g.ymNext, g.psgAcc = 0, 0, ymClks, 0
	g.line, g.lineStart, g.vblank, g.hvLatch = 0, 0, false, -1
	g.frame, g.bootFrames = 0, 3
	g.rsPos, g.prevL, g.prevR, g.lpL, g.lpR = 0, 0, 0, 0, 0
	g.cpu.Cycles = 0
	g.cpu.Reset()
	g.cpu.Cycles = 0
}

// ---- timing helpers ----

func (g *Genesis) now68k() int64 { return g.cpu.Cycles * 7 }

func (g *Genesis) dmaStall(n int, internal, vramTarget bool) {
	if internal { // fills / copies don't hold the 68000
		return
	}
	blank := g.vblank || !g.vdp.display()
	var perLine float64
	if g.vdp.h40() {
		perLine = 18
		if blank {
			perLine = 205
		}
	} else {
		perLine = 16
		if blank {
			perLine = 167
		}
	}
	words := perLine
	if vramTarget {
		words = perLine / 2
	}
	g.cpu.Cycles += dmaCycles(n, words)
}

func (g *Genesis) dmaRead(a int) int {
	a &= 0xfffffe
	if a < 0x400000 {
		return g.cart.read16(a)
	}
	if a >= 0xe00000 {
		a &= 0xffff
		return int(g.ram[a])<<8 | int(g.ram[a+1])
	}
	return 0
}

func (g *Genesis) updateIRQ() {
	v := &g.vdp
	lv := 0
	if v.vintPending && v.reg[1]&0x20 != 0 {
		lv = 6
	} else if v.hintPending && v.reg[0]&0x10 != 0 {
		lv = 4
	}
	if lv != g.cpu.IRQ() {
		g.cpu.SetIRQ(lv)
	}
}

func (g *Genesis) ack(lv int) int {
	if lv == 6 {
		g.vdp.vintPending = false
	} else if lv == 4 {
		g.vdp.hintPending = false
	}
	g.updateIRQ()
	return -1
}

// ---- audio ----

func (g *Genesis) syncAudio(t int64) {
	ym, psg := g.ym, g.psg
	for g.ymNext <= t {
		ym.Tick()
		g.psgAcc += ymClks
		p, n := 0.0, 0
		for g.psgAcc >= psgClks {
			g.psgAcc -= psgClks
			p += psg.Step()
			n++
		}
		pv := 0.0
		if n != 0 {
			pv = float64(p/float64(n)) * 0.13
		}
		g.audioL = append(g.audioL, float32(float64(ym.Out[0])/1400+pv))
		g.audioR = append(g.audioR, float32(float64(ym.Out[1])/1400+pv))
		g.ymNext += ymClks
	}
}

func (g *Genesis) ymWrite(port, v int, t int64) { g.syncAudio(t); g.ym.Write(port, uint8(v)) }
func (g *Genesis) ymRead(t int64) int           { g.syncAudio(t); return int(g.ym.ReadStatus()) }
func (g *Genesis) psgWrite(v int, t int64)      { g.syncAudio(t); g.psg.Write(uint8(v)) }

// ---- I/O (pads) ----

func (g *Genesis) padRead(p int) int {
	ctrl, data := int(g.ioCtrl[p]), int(g.ioData[p])
	v := 0x7f
	if p < 2 {
		b := g.input[p]
		th := 0x40
		if ctrl&0x40 != 0 {
			th = data & 0x40
		}
		g.thTimeout(p)
		c := int(g.thCount[p])
		if g.three[p] {
			c = 0
		}
		on := func(bit uint32) int {
			if b&bit != 0 {
				return 0
			}
			return 1
		}
		if th != 0 {
			if c == 3 {
				v = 0x40 | on(bC)<<5 | on(bB)<<4 | on(bMode)<<3 | on(bX)<<2 | on(bY)<<1 | on(bZ)
			} else {
				v = 0x40 | on(bC)<<5 | on(bB)<<4 | on(bRight)<<3 | on(bLeft)<<2 | on(bDown)<<1 | on(bUp)
			}
		} else {
			switch c {
			case 3:
				v = on(bStart)<<5 | on(bA)<<4
			case 4:
				v = on(bStart)<<5 | on(bA)<<4 | 0xf
			default:
				v = on(bStart)<<5 | on(bA)<<4 | on(bDown)<<1 | on(bUp)
			}
		}
	}
	return data&0x80 | (data&ctrl)&0x7f | v&^ctrl&0x7f
}

func (g *Genesis) thTimeout(p int) {
	if g.now68k()-g.thTime[p] > lineClk*24 {
		g.thCount[p] = 0
	}
}

func (g *Genesis) ioWrite(r, v int) {
	if r >= 1 && r <= 3 {
		p := r - 1
		old := g.thLevel(p)
		g.ioData[p] = uint8(v)
		g.thEdge(p, old)
	} else if r >= 4 && r <= 6 {
		p := r - 4
		old := g.thLevel(p)
		g.ioCtrl[p] = uint8(v)
		g.thEdge(p, old)
	}
}

func (g *Genesis) thLevel(p int) int {
	if g.ioCtrl[p]&0x40 != 0 {
		return int(g.ioData[p] & 0x40)
	}
	return 0x40
}

func (g *Genesis) thEdge(p, old int) {
	if p > 1 {
		return
	}
	now := g.thLevel(p)
	if old != 0 && now == 0 {
		g.thTimeout(p)
		if g.thCount[p] < 8 {
			g.thCount[p]++
		}
		g.thTime[p] = g.now68k()
	} else if old == 0 && now != 0 {
		g.thTime[p] = g.now68k()
	}
}

func (g *Genesis) ioRead(r int) int {
	switch r {
	case 0:
		v := 0x20
		if g.overseas {
			v |= 0x80
		}
		if g.pal {
			v |= 0x40
		}
		return v
	case 1, 2, 3:
		return g.padRead(r - 1)
	case 4, 5, 6:
		return int(g.ioCtrl[r-4])
	}
	return 0
}

// ---- Z80 side ----

func (g *Genesis) z80Busy() bool { return g.busReq != 0 || g.zReset != 0 }

func (g *Genesis) zread(a int) int {
	switch {
	case a < 0x4000:
		return int(g.zram[a&0x1fff])
	case a < 0x6000:
		return g.ymRead(g.zc * 15)
	case a < 0x8000:
		return 0xff
	}
	ad := g.zBank<<15 | a&0x7fff
	if ad >= 0xe00000 {
		return 0xff
	}
	return g.read8(ad)
}

func (g *Genesis) zwrite(a, v int) {
	switch {
	case a < 0x4000:
		g.zram[a&0x1fff] = uint8(v)
	case a < 0x6000:
		g.ymWrite(a&3, v, g.zc*15)
	case a < 0x6100:
		g.zBank = (g.zBank>>1 | (v&1)<<8) & 0x1ff
	case a >= 0x7f00 && a < 0x7f20:
		if a&0x19 == 0x11 {
			g.psgWrite(v, g.zc*15)
		}
	case a >= 0x8000:
		ad := g.zBank<<15 | a&0x7fff
		if ad < 0xe00000 {
			g.write8(ad, v)
		}
	}
}

// ---- 68000 side ----

func (g *Genesis) busAck() int {
	if g.busReq != 0 && g.zReset == 0 {
		return 0
	}
	return 1
}

func (g *Genesis) read8(a int) int {
	a &= 0xffffff
	switch {
	case a < 0x400000:
		return g.cart.read8(a)
	case a >= 0xe00000:
		return int(g.ram[a&0xffff])
	case a >= 0xc00000:
		if a&0xe700e0 != 0xc00000 {
			return 0xff
		}
		w := g.vdpRead(a & 0x1e)
		if a&1 != 0 {
			return w & 0xff
		}
		return w >> 8
	case a >= 0xa00000 && a < 0xa10000:
		if g.busReq == 0 {
			return 0xff
		}
		z := a & 0x7fff
		if z < 0x4000 {
			return int(g.zram[z&0x1fff])
		}
		if z < 0x6000 {
			return g.ymRead(g.now68k())
		}
		return 0xff
	case a >= 0xa10000 && a < 0xa10020:
		return g.ioRead((a >> 1) & 0xf)
	case a&0xfffffe == 0xa11100:
		if a&1 != 0 {
			return 0
		}
		return g.busAck() | 0x80
	}
	return 0xff
}

func (g *Genesis) read16(a int) int {
	a &= 0xfffffe
	switch {
	case a < 0x400000:
		return g.cart.read16(a)
	case a >= 0xe00000:
		a &= 0xffff
		return int(g.ram[a])<<8 | int(g.ram[a+1])
	case a >= 0xc00000:
		if a&0xe700e0 != 0xc00000 {
			return 0xffff
		}
		return g.vdpRead(a & 0x1e)
	case a >= 0xa00000 && a < 0xa10000:
		b := g.read8(a)
		return b<<8 | b
	case a >= 0xa10000 && a < 0xa10020:
		b := g.ioRead((a >> 1) & 0xf)
		return b<<8 | b
	case a == 0xa11100:
		return g.busAck()<<8 | 0x8000
	}
	return 0xffff
}

func (g *Genesis) write8(a, v int) {
	a &= 0xffffff
	v &= 0xff
	switch {
	case a >= 0xe00000:
		g.ram[a&0xffff] = uint8(v)
	case a < 0x400000:
		g.cart.write8(a, v)
	case a < 0x800000:
		g.cart.highWrite(a, v)
	case a >= 0xc00000:
		if a&0xe700e0 != 0xc00000 {
			return
		}
		r := a & 0x1f
		if r >= 0x10 && r < 0x18 {
			if r&1 != 0 {
				g.psgWrite(v, g.now68k())
			}
			return
		}
		g.vdpWrite(r&0x1e, v<<8|v)
	case a >= 0xa00000 && a < 0xa10000:
		if g.busReq == 0 {
			return
		}
		z := a & 0x7fff
		switch {
		case z < 0x4000:
			g.zram[z&0x1fff] = uint8(v)
		case z < 0x6000:
			g.ymWrite(z&3, v, g.now68k())
		case z < 0x6100:
			g.zBank = (g.zBank>>1 | (v&1)<<8) & 0x1ff
		case z >= 0x7f00 && z&0x19 == 0x11:
			g.psgWrite(v, g.now68k())
		}
	case a >= 0xa10000 && a < 0xa10020:
		if a&1 != 0 {
			g.ioWrite((a>>1)&0xf, v)
		}
	case a == 0xa11100:
		g.setBusReq(v&1 != 0)
	case a == 0xa11200:
		g.setZReset(v&1 == 0)
	case a >= 0xa13000 && a < 0xa13100:
		g.cart.timeWrite(a, v)
	}
}

func (g *Genesis) write16(a, v int) {
	a &= 0xfffffe
	v &= 0xffff
	switch {
	case a >= 0xe00000:
		a &= 0xffff
		g.ram[a] = uint8(v >> 8)
		g.ram[a+1] = uint8(v)
	case a >= 0xc00000:
		if a&0xe700e0 != 0xc00000 {
			return
		}
		r := a & 0x1f
		if r >= 0x10 && r < 0x18 {
			g.psgWrite(v&0xff, g.now68k())
			return
		}
		g.vdpWrite(r&0x1e, v)
	case a < 0x400000:
		g.cart.write16(a, v)
	case a >= 0xa00000 && a < 0xa10000:
		g.write8(a, v>>8)
	case a >= 0xa10000 && a < 0xa10020:
		g.ioWrite((a>>1)&0xf, v&0xff)
	case a == 0xa11100:
		g.setBusReq((v>>8)&1 != 0)
	case a == 0xa11200:
		g.setZReset((v>>8)&1 == 0)
	default:
		g.write8(a, v>>8)
		g.write8(a+1, v&0xff)
	}
}

func (g *Genesis) setBusReq(on bool) {
	g.busReq = 0
	if on {
		g.busReq = 1
	}
}

func (g *Genesis) setZReset(on bool) {
	if on && g.zReset == 0 {
		g.z80.Reset()
		g.syncAudio(g.now68k())
		g.ym.Reset()
	}
	g.zReset = 0
	if on {
		g.zReset = 1
	}
}

func (g *Genesis) vdpRead(r int) int {
	switch {
	case r < 4:
		return g.vdp.readData()
	case r < 8:
		return g.vdp.status()
	case r < 0x10:
		return g.vdp.hv()
	}
	return 0xffff
}

func (g *Genesis) vdpWrite(r, v int) {
	if r < 4 {
		g.vdp.writeData(v)
	} else if r < 8 {
		g.vdp.writeControl(v)
	}
}

// ---- running ----

func ceilDiv(a, b int64) int64 { return (a + b - 1) / b }

func (g *Genesis) runTo(t int64) {
	for g.mclk < t {
		end := min(t, g.mclk+slice)
		if g.z80Busy() {
			g.zc = max(g.zc, ceilDiv(end, 15))
		} else {
			for g.zc*15 < end {
				g.zc += int64(g.z80.Step())
			}
		}
		g.cpu.Run(ceilDiv(end, 7))
		g.mclk = end
	}
}

// RunFrame runs the console for one frame.
func (g *Genesis) RunFrame() {
	vdp := &g.vdp
	// Holding Mode while switching on makes a 6-button pad act as a 3-button one (for games that misread it).
	if g.bootFrames > 0 {
		g.bootFrames--
		for p := 0; p < 2; p++ {
			if g.input[p]&bMode != 0 {
				g.three[p] = true
			}
		}
	}
	H, total := vdp.lines(), g.linesPerFrame
	if vdp.reg[12]&2 != 0 {
		vdp.odd ^= 1
	} else {
		vdp.odd = 0
	}
	im2 := vdp.im2()
	for line := 0; line < total; line++ {
		g.line = line
		g.lineStart = g.mclk
		if line == 0 {
			g.vblank = false
		}
		if line == H {
			g.vblank = true
			g.runTo(g.lineStart + 80)
			vdp.vintPending = true
			g.updateIRQ()
			g.z80.IRQ = true
		} else if line == H+1 {
			g.z80.IRQ = false
		}
		g.runTo(g.lineStart + active)
		if line < H {
			vdp.renderLine(line, g.screen)
		}
		if line <= H {
			if vdp.hcount == 0 {
				vdp.hcount = int(vdp.reg[10])
				vdp.hintPending = true
				g.updateIRQ()
			} else {
				vdp.hcount--
			}
		} else {
			vdp.hcount = int(vdp.reg[10])
		}
		g.runTo(g.lineStart + lineClk)
	}
	g.syncAudio(g.mclk)
	g.width = vdp.width()
	g.height = H
	if im2 {
		g.height = H * 2
	}
	g.frame++
}

// SetInput sets a pad's buttons (bit i = Buttons[i]); pads 3 and 4 are the J-Cart's.
func (g *Genesis) SetInput(p int, bits uint32) {
	if p >= 0 && p < 4 {
		g.input[p] = bits
	}
}

// TakeAudio resamples the native ~53 kHz output to the player's rate
// (interleaved stereo).
func (g *Genesis) TakeAudio() []float32 {
	n := len(g.audioL)
	ratio := g.nativeRate / float64(g.sampleRate)
	L, R := g.audioL, g.audioR
	out := g.out[:0]
	pos := g.rsPos
	// one-pole low-pass at the native rate to soften aliasing
	const a = 0.55
	for i := 0; i < n; i++ {
		g.lpL += float64((float64(L[i]) - g.lpL) * a)
		L[i] = float32(g.lpL)
		g.lpR += float64((float64(R[i]) - g.lpR) * a)
		R[i] = float32(g.lpR)
	}
	for pos < float64(n) {
		i := int(pos)
		f := pos - float64(i)
		l0, r0 := g.prevL, g.prevR
		if i != 0 {
			l0, r0 = float64(L[i-1]), float64(R[i-1])
		}
		out = append(out, float32(l0+float64((float64(L[i])-l0)*f)), float32(r0+float64((float64(R[i])-r0)*f)))
		pos += ratio
	}
	g.rsPos = pos - float64(n)
	if n > 0 {
		g.prevL, g.prevR = float64(L[n-1]), float64(R[n-1])
	}
	g.audioL, g.audioR = g.audioL[:0], g.audioR[:0]
	g.out = out
	res := make([]float32, len(out))
	copy(res, out)
	return res
}

// ---- core.Core ----

// Info describes the controls and picture.
func (g *Genesis) Info() core.Info {
	return core.Info{System: "segaMD", Buttons: Buttons, Players: 4, Stereo: true, Aspect: 4.0 / 3}
}

func (g *Genesis) FPS() float64     { return g.fps }
func (g *Genesis) Width() int       { return g.width }
func (g *Genesis) Height() int      { return g.height }
func (g *Genesis) Screen() []uint32 { return g.screen[:g.width*g.height] }

// SRAM is the battery save (or EEPROM), or nil when the game has none.
func (g *Genesis) SRAM() []byte { return g.cart.getSRAM() }
