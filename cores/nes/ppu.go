package nes

import (
	"math"
	"sync"
)

// PPU (graphics, emulated dot by dot), from web/emu/nes.js.

// palette is computed from the NES's composite video levels (NTSC model);
// entries are 0x00RRGGBB, 64 colours × 8 emphasis settings. It is built when
// the first game opens.
var (
	palette     [512]uint32
	paletteOnce sync.Once
)

func initPalette() { paletteOnce.Do(func() { palette = buildPalette() }) }

func buildPalette() [512]uint32 {
	// Signal levels per luma row, low and high halves of the waveform (black = 0, white = 1).
	lo := [4]float64{-0.117, 0.000, 0.308, 0.715}
	hi := [4]float64{0.397, 0.681, 1.000, 1.000}
	var cos, sin [12]float64
	for p := range cos {
		ang := math.Pi * float64(p+3) / 6 // colour-burst phase reference
		cos[p], sin[p] = jsCos(ang), jsSin(ang)
	}
	to8 := func(v float64) uint32 {
		v = math.Max(0, v)
		x := math.Pow(v, 1/1.1) * 255
		// math.Pow can be an ulp away from JavaScript's (correctly rounded)
		// Math.pow; that only matters when the result is right at a rounding
		// boundary, so use the exact version there.
		if f := x - math.Floor(x); math.Abs(f-0.5) < 1e-9 || f < 1e-9 || f > 1-1e-9 {
			x = jsPow(v, 1/1.1) * 255
		}
		return uint32(math.Max(0, math.Min(255, jsRound(x))))
	}
	var pal [512]uint32
	for emph := 0; emph < 8; emph++ {
		for c := 0; c < 64; c++ {
			hue, lum := c&15, (c>>4)&3
			y, i, q := 0.0, 0.0, 0.0
			for p := 0; p < 12; p++ {
				// 12 phases per colour cycle; the colour's square wave is "high" for 6 of them.
				var level float64
				switch {
				case hue == 0:
					level = hi[lum]
				case hue >= 13:
					if hue == 13 {
						level = lo[lum]
					} else {
						level = lo[1]
					}
				case (p+hue+12-1)%12 < 6:
					level = hi[lum]
				default:
					level = lo[lum]
				}
				// Emphasis bits darken the phases tied to red, green and blue.
				if hue < 14 && emph != 0 {
					r := emph&1 != 0 && (p+12-0)%12 < 6
					g := emph&2 != 0 && (p+12-4)%12 < 6
					b := emph&4 != 0 && (p+12-8)%12 < 6
					if r || g || b {
						level = level*0.746 - 0.0912*0.746*0
					}
				}
				y += level
				i += level * cos[p]
				q += level * sin[p]
			}
			y /= 12
			i /= 12
			q /= 12
			const sat = 1.6
			r := y + sat*(0.956*i+0.621*q)
			g := y + sat*(-0.272*i-0.647*q)
			b := y + sat*(-1.106*i+1.703*q)
			pal[emph*64+c] = to8(r)<<16 | to8(g)<<8 | to8(b)
		}
	}
	return pal
}

var rev [256]uint8

func init() {
	for i := 0; i < 256; i++ {
		r := 0
		for b := 0; b < 8; b++ {
			if i&(1<<b) != 0 {
				r |= 0x80 >> b
			}
		}
		rev[i] = uint8(r)
	}
}

type ppu struct {
	nes    *NES
	vram   []byte
	palRam [32]byte
	oam    [256]byte
	screen []uint32
	pal    *[512]uint32 // the colours (the console's palette unless an arcade board has its own)
	noSkip bool         // never skip the odd frames' dot (the arcade RGB PPUs)

	ctrl, mask, status, oamAddr int
	v, t, x, w, buffer, openBus int
	busFrame                    int
	line, dot                   int
	odd                         bool
	frame                       int
	nmiOut                      bool
	ticks, nmiRise              int64
	suppressNmi, renderAtSkip   bool
	fetch                       int // what the PPU is reading: 0 = CPU access, 1 = background, 2 = sprites

	// background pipeline
	ntByte, atByte, ptLo, ptHi                   int
	bgLo, bgHi, atLo, atHi, atLatchLo, atLatchHi int

	// sprites for the current line
	spCount                 int
	spX, spAttr, spLo, spHi [8]int
	spZero                  bool
	nextSp                  [32]byte
	nextCount               int
	nextZero                bool
	spLoN, spHiN            [8]int
	lineSp                  [256]uint16 // the current line's sprite pixels (see buildLineSprites)
	ovfDot                  int
	frameDone               bool
}

func newPPU(n *NES) *ppu {
	p := &ppu{nes: n, screen: make([]uint32, 256*240), nmiRise: -1, ovfDot: -1, pal: &palette}
	// Palette RAM isn't cleared at power; these are the values a typical console wakes up with.
	p.palRam = [32]byte{0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0d, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2c,
		0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3a, 0x00, 0x02, 0x00, 0x20, 0x2c, 0x08}
	return p
}

func (p *ppu) rendering() bool { return p.mask&0x18 != 0 }

func (p *ppu) read(a int) int {
	n := p.nes
	a &= 0x3fff
	if a < 0x2000 {
		return n.m.ppuRead(a)
	}
	if n.a12 != nil && p.fetch != 0 { // chips watching PPU A12 see nametable fetches too
		n.a12.ppuAddr(a)
	}
	if a < 0x3f00 {
		if n.nt != nil {
			return n.nt.ntRead(a & 0xfff)
		}
		return int(p.vram[n.mb.ntIndex(a)])
	}
	return int(p.palRam[palIndex(a)])
}

func (p *ppu) write(a, v int) {
	n := p.nes
	a &= 0x3fff
	switch {
	case a < 0x2000:
		n.m.ppuWrite(a, v)
	case a < 0x3f00:
		if n.nt != nil {
			n.nt.ntWrite(a&0xfff, v)
		} else {
			p.vram[n.mb.ntIndex(a)] = byte(v)
		}
	default:
		p.palRam[palIndex(a)] = byte(v & 0x3f)
	}
}

func palIndex(a int) int {
	a &= 0x1f
	if a&0x13 == 0x10 {
		a &= 0x0f
	}
	return a
}

// cpuRead reads the CPU-visible registers $2000-$2007.
func (p *ppu) cpuRead(r int) int {
	// The PPU's data bus holds the last value for a while, then fades to 0.
	if p.frame-p.busFrame > 36 {
		p.openBus = 0
		p.busFrame = p.frame
	}
	switch r {
	case 2:
		v := p.status&0xe0 | p.openBus&0x1f
		if p.line == 241 {
			// One dot before vblank starts: reads clear and the flag/NMI never happen this frame.
			if p.dot == 1 {
				v &= 0x7f
				p.suppressNmi = true
			} else if p.dot == 2 || p.dot == 3 {
				// On the same dot or just after: reads set, but the NMI is cancelled.
				p.nes.cpu.NMIPending = false
			}
		}
		p.status &= 0x7f
		p.w = 0
		p.updateNmi()
		p.openBus = v
		return v
	case 4:
		p.openBus = int(p.oam[p.oamAddr])
		return p.openBus
	case 7:
		var v int
		if p.v&0x3fff >= 0x3f00 {
			v = p.read(p.v)&0x3f | p.openBus&0xc0
			p.buffer = p.read(p.v - 0x1000)
		} else {
			v = p.buffer
			p.buffer = p.read(p.v)
		}
		p.incV()
		p.openBus = v
		return v
	}
	return p.openBus
}

func (p *ppu) cpuWrite(r, v int) {
	p.openBus = v
	p.busFrame = p.frame
	switch r {
	case 0:
		p.ctrl = v
		p.t = p.t&0xf3ff | (v&3)<<10
		p.updateNmi()
	case 1:
		p.mask = v
	case 3:
		p.oamAddr = v
	case 4:
		if p.oamAddr&3 == 2 {
			p.oam[p.oamAddr] = byte(v & 0xe3)
		} else {
			p.oam[p.oamAddr] = byte(v)
		}
		p.oamAddr = (p.oamAddr + 1) & 0xff
	case 5:
		if p.w == 0 {
			p.t = p.t&0xffe0 | v>>3
			p.x = v & 7
		} else {
			p.t = p.t&0x8c1f | (v&7)<<12 | (v&0xf8)<<2
		}
		p.w ^= 1
	case 6:
		if p.w == 0 {
			p.t = p.t&0x00ff | (v&0x3f)<<8
		} else {
			p.t = p.t&0xff00 | v
			p.v = p.t
			if p.nes.a12 != nil {
				p.nes.a12.ppuAddr(p.v)
			}
		}
		p.w ^= 1
	case 7:
		p.write(p.v, v)
		p.incV()
	}
}

func (p *ppu) incV() {
	if p.ctrl&4 != 0 {
		p.v = (p.v + 32) & 0x7fff
	} else {
		p.v = (p.v + 1) & 0x7fff
	}
	if p.nes.a12 != nil {
		p.nes.a12.ppuAddr(p.v)
	}
}

func (p *ppu) updateNmi() {
	out := p.ctrl&0x80 != 0 && p.status&0x80 != 0
	cpu := p.nes.cpu
	if out && !p.nmiOut {
		cpu.NMIPending = true
		p.nmiRise = p.ticks
	}
	// The CPU only sees the NMI line when it samples it: a pulse that ends before then is missed.
	if !out && p.nmiOut && p.nmiRise > p.nes.lastPoll {
		cpu.NMIPending = false
	}
	// Turning NMIs off right as vblank starts cancels that NMI.
	if !out && p.nmiOut && p.line == 241 && p.dot <= 3 {
		cpu.NMIPending = false
	}
	p.nmiOut = out
}

// scroll helpers (the "loopy" v/t registers)
func (p *ppu) incX() {
	if p.v&0x1f == 31 {
		p.v &^= 0x1f
		p.v ^= 0x400
	} else {
		p.v++
	}
}

func (p *ppu) incY() {
	if p.v&0x7000 != 0x7000 {
		p.v += 0x1000
		return
	}
	p.v &^= 0x7000
	y := (p.v & 0x3e0) >> 5
	switch y {
	case 29:
		y = 0
		p.v ^= 0x800
	case 31:
		y = 0
	default:
		y++
	}
	p.v = p.v&^0x3e0 | y<<5
}

func (p *ppu) bgFetch() {
	p.fetch = 1
	switch p.dot & 7 {
	case 1:
		p.loadShifters()
		p.ntByte = p.read(0x2000 | p.v&0xfff)
	case 3:
		v := p.v
		at := p.read(0x23c0 | v&0xc00 | (v>>4)&0x38 | (v>>2)&7)
		p.atByte = (at >> ((v>>4)&4 | v&2)) & 3
	case 4:
		p.ptLo = p.read((p.ctrl&0x10)<<8 + p.ntByte*16 + (p.v>>12)&7)
	case 7:
		p.ptHi = p.read((p.ctrl&0x10)<<8 + p.ntByte*16 + (p.v>>12)&7 + 8)
	case 0:
		p.incX()
	}
}

func (p *ppu) loadShifters() {
	p.bgLo = p.bgLo&0xff00 | p.ptLo
	p.bgHi = p.bgHi&0xff00 | p.ptHi
	p.atLatchLo = p.atByte & 1
	p.atLatchHi = (p.atByte >> 1) & 1
}

func (p *ppu) shift() {
	p.bgLo = (p.bgLo << 1) & 0xffff
	p.bgHi = (p.bgHi << 1) & 0xffff
	p.atLo = (p.atLo<<1 | p.atLatchLo) & 0xff
	p.atHi = (p.atHi<<1 | p.atLatchHi) & 0xff
}

// evalSprites finds sprites for the next line. Done in one go when evaluation
// starts (dot 65), but the overflow flag is raised at the dot the hardware
// would find it, including the chip's bug: after 8 sprites it steps through
// OAM diagonally, so overflow detection is unreliable.
func (p *ppu) evalSprites() {
	h := 8
	if p.ctrl&0x20 != 0 {
		h = 16
	}
	next := p.line + 1
	oam := &p.oam
	n, found, dot, m := 0, 0, 65, 0
	p.nextZero = false
	p.ovfDot = -1
	if p.line < 240 {
		for n < 64 && dot < 256 {
			if found < 8 {
				row := next - int(oam[n*4]) - 1
				dot += 2
				if row >= 0 && row < h {
					copy(p.nextSp[found*4:found*4+4], oam[n*4:n*4+4])
					if n == 0 {
						p.nextZero = true
					}
					found++
					dot += 6
				}
				n++
			} else {
				row := next - int(oam[n*4+m]) - 1
				if row >= 0 && row < h {
					p.ovfDot = dot + 1
					break
				}
				dot += 2
				n++
				m = (m + 1) & 3
			}
		}
	}
	p.nextCount = found
}

// spriteFetch does the pattern fetches for the next line's sprites (dots
// 257-320; done per slot so cartridge chips watching the PPU address bus see
// the right sequence).
func (p *ppu) spriteFetch(slot int) {
	p.fetch = 2
	h := 8
	if p.ctrl&0x20 != 0 {
		h = 16
	}
	next := p.line + 1
	if p.line == 261 {
		next = 0
	}
	if slot < p.nextCount {
		y, tile, attr := int(p.nextSp[slot*4]), int(p.nextSp[slot*4+1]), int(p.nextSp[slot*4+2])
		row := next - y - 1
		if attr&0x80 != 0 {
			row = h - 1 - row
		}
		var addr int
		if h == 16 {
			addr = (tile&1)<<12 | (tile&0xfe)<<4 | row&7
			if row&8 != 0 {
				addr |= 16
			}
		} else {
			addr = (p.ctrl&8)<<9 | tile<<4 | row
		}
		lo, hi := p.read(addr), p.read(addr+8)
		if attr&0x40 != 0 {
			lo, hi = int(rev[lo&0xff]), int(rev[hi&0xff])
		}
		p.spLoN[slot] = lo & 0xff
		p.spHiN[slot] = hi & 0xff
	} else {
		addr := (p.ctrl&8)<<9 | 0xff0
		if h == 16 {
			addr = 0x1ff0
		}
		p.read(addr)
		p.read(addr + 8)
	}
}

func (p *ppu) tick() {
	line, dot, render := p.line, p.dot, p.rendering()
	if line < 240 || line == 261 {
		if render {
			if (dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337) {
				p.shift()
			}
			if (dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336) {
				p.bgFetch()
			}
			if dot == 65 {
				p.evalSprites()
			} else if dot == p.ovfDot && line < 240 {
				p.status |= 0x20
			}
			if dot == 256 {
				p.incY()
			} else if dot == 257 {
				p.v = p.v&^0x41f | p.t&0x41f
			}
			if dot >= 257 && dot <= 320 && (dot-257)&7 == 3 {
				p.spriteFetch((dot - 257) >> 3)
			}
			if line == 261 && dot >= 280 && dot <= 304 {
				p.v = p.v&^0x7be0 | p.t&0x7be0
			}
			if dot == 337 || dot == 339 {
				p.read(0x2000 | p.v&0xfff) // garbage nametable fetches
			}
			if dot == 320 {
				p.spCount, p.spZero = p.nextCount, p.nextZero
				for i := 0; i < 8; i++ {
					p.spX[i] = int(p.nextSp[i*4+3])
					p.spAttr[i] = int(p.nextSp[i*4+2])
					p.spLo[i] = p.spLoN[i]
					p.spHi[i] = p.spHiN[i]
				}
				p.buildLineSprites()
			}
		}
		p.fetch = 0
		if render && dot == 4 {
			p.nes.m.scanline(line)
		}
		if line < 240 && dot >= 1 && dot <= 256 {
			p.pixel(dot-1, line)
		}
		if line == 261 && dot == 1 {
			p.status &= 0x1f
			p.updateNmi()
		}
	} else if line == 241 && dot == 1 {
		if p.nes.vbl != nil {
			p.nes.vbl.vblank()
		}
		if !p.suppressNmi {
			p.status |= 0x80
		}
		p.suppressNmi = false
		p.updateNmi()
		p.frameDone = true
	}
	// advance
	if line == 261 && dot == 338 {
		p.renderAtSkip = render
	}
	p.dot++
	p.ticks++
	if line == 261 && p.dot == 340 && p.odd && p.renderAtSkip && !p.noSkip {
		p.dot = 341 // skipped dot on odd frames
	}
	if p.dot > 340 {
		p.dot = 0
		p.line++
		if p.line > 261 {
			p.line = 0
			p.odd = !p.odd
			p.frame++
		}
	}
}

func (p *ppu) pixel(x, y int) {
	bg, pal := 0, 0
	mask := p.mask
	if mask&8 != 0 && (x >= 8 || mask&2 != 0) {
		bit := uint(15 - p.x)
		bg = (p.bgLo>>bit)&1 | ((p.bgHi>>bit)&1)<<1
		if bg != 0 {
			ab := uint(7 - p.x)
			pal = ((p.atLo>>ab)&1|((p.atHi>>ab)&1)<<1)<<2 | bg
		}
	}
	color := 0
	if bg != 0 {
		color = pal
	}
	if mask&0x10 != 0 && (x >= 8 || mask&4 != 0) {
		if e := p.lineSp[x]; e != 0 {
			sp, attr := int(e&3), int(e>>2)&0xff
			if e&lineSpSlot0 != 0 && p.spZero && bg != 0 && x != 255 {
				p.status |= 0x40
			}
			if bg == 0 || attr&0x20 == 0 {
				color = 0x10 | (attr&3)<<2 | sp
			}
		}
	}
	c := int(p.palRam[palIndex(0x3f00|color)])
	if mask&1 != 0 {
		c &= 0x30 // greyscale
	}
	p.screen[y*256+x] = p.pal[((mask>>5)&7)*64+c]
}

const lineSpSlot0 = 1 << 10

// buildLineSprites works out, for each x of the coming line, the first
// sprite (in OAM order) with a non-transparent pixel there: what pixel()
// would otherwise find by scanning the sprites at every dot.
func (p *ppu) buildLineSprites() {
	p.lineSp = [256]uint16{}
	for i := p.spCount - 1; i >= 0; i-- { // lower slots win, so they go last
		for off := 0; off < 8; off++ {
			x := p.spX[i] + off
			if x > 255 {
				break
			}
			sp := (p.spLo[i]>>(7-off))&1 | ((p.spHi[i]>>(7-off))&1)<<1
			if sp == 0 {
				continue
			}
			e := uint16(sp) | uint16(p.spAttr[i]&0xff)<<2
			if i == 0 {
				e |= lineSpSlot0
			}
			p.lineSp[x] = e
		}
	}
}

func (p *ppu) state(s *sio) {
	s.Ints(&p.ctrl, &p.mask, &p.status, &p.oamAddr, &p.v, &p.t, &p.x, &p.w, &p.buffer, &p.openBus, &p.busFrame,
		&p.line, &p.dot, &p.frame, &p.fetch, &p.ntByte, &p.atByte, &p.ptLo, &p.ptHi, &p.bgLo, &p.bgHi, &p.atLo, &p.atHi,
		&p.atLatchLo, &p.atLatchHi, &p.spCount, &p.nextCount, &p.ovfDot)
	s.Bools(&p.odd, &p.nmiOut, &p.suppressNmi, &p.renderAtSkip, &p.spZero, &p.nextZero, &p.frameDone)
	s.I64(&p.ticks)
	s.I64(&p.nmiRise)
	s.Bytes(p.palRam[:])
	s.Bytes(p.oam[:])
	s.Bytes(p.nextSp[:])
	s.IntSlice(p.spX[:])
	s.IntSlice(p.spAttr[:])
	s.IntSlice(p.spLo[:])
	s.IntSlice(p.spHi[:])
	s.IntSlice(p.spLoN[:])
	s.IntSlice(p.spHiN[:])
	if !s.save {
		p.buildLineSprites()
	}
}
