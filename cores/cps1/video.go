package cps1

// The CPS-1 video: the CPS-A and CPS-B chips drawing from the 192 KB of
// graphics RAM at 0x900000.
//
// CPS-A registers (write only, words at 0x800100):
//
//	00 sprite list base    02 scroll 1 base    04 scroll 2 base
//	06 scroll 3 base       08 row scroll base  0a palette base
//	0c/0e scroll 1 x/y     10/12 scroll 2 x/y  14/16 scroll 3 x/y
//	18-1e star fields      20 row scroll offset
//	22 video control: bit 0 row scroll on scroll 2, bit 15 flip screen
//
// A base register holds an address in graphics RAM divided by 256. Writing
// the palette base has the palette copied from there into the palette chips
// at the next vblank, before the frame is drawn (up to 6 pages of 32 palettes of 16 colours: sprites, scroll 1-3, two star
// pages), the pages the CPS-B's palette control picks.
//
// Scroll layers are 64x64 tiles of 8x8 (scroll 1), 16x16 (scroll 2) and
// 32x32 (scroll 3) pixels, two words per tile: the tile number, then the
// attributes (bits 0-4 palette, 5 flip x, 6 flip y, 7-8 tile group). Pen 15
// is transparent. The CPS-B's layer control picks the order of the four
// layers (two bits each at bits 6, 8, 10, 12, bottom to top: 0 sprites,
// 1-3 scroll 1-3) and turns layers on; its four priority masks say which
// pens of each tile group of the layer under the sprites still show in front
// of the sprites.
//
// The sprite list is copied at each vblank and drawn the next frame: four
// words an entry (x, y, tile, attributes: bits 0-4 palette, 5 flip x, 6 flip
// y, 8-11 width-1 and 12-15 height-1 in 16x16 tiles), ended by an entry
// whose attributes' high byte is 0xff. Earlier entries are in front.
//
// The picture is 384x224: hardware pixels 64-447 of lines 16-239.

const (
	ScreenW = 384
	ScreenH = 224
	hx0     = 64 // first visible hardware pixel
	hy0     = 16 // first visible line
	clear16 = 0xffff
)

// cps-a register word numbers
const (
	regObjBase  = 0x00
	regS1Base   = 0x01
	regS2Base   = 0x02
	regS3Base   = 0x03
	regRowBase  = 0x04
	regPalBase  = 0x05
	regS1X      = 0x06
	regS1Y      = 0x07
	regS2X      = 0x08
	regS2Y      = 0x09
	regS3X      = 0x0a
	regS3Y      = 0x0b
	regRowOffs  = 0x10
	regVideoCtl = 0x11
)

type video struct {
	cfg  *Config
	pens []uint8 // the graphics ROMs: 16 pixels for every 8 bytes

	gfxram [0x30000]uint8
	cpsa   [0x20]uint16
	cpsb   [0x20]uint16
	palRaw [0xc00]uint16
	pal    [0xc00]uint32
	obj    [0x800]uint8 // the sprite list as copied at the last vblank
	// palPending: the palette base was written; the copy is made at the
	// next vblank, from graphics RAM as it is then
	palPending bool

	screen []uint32
	spr    []uint16 // sprite pixels of the frame (colour index or clear16)
	line   [4][ScreenW]uint16
	high   [ScreenW]bool
}

func newVideo(cfg *Config, gfx []byte) *video {
	v := &video{cfg: cfg}
	v.pens = decodeGfx(gfx)
	v.screen = make([]uint32, ScreenW*ScreenH)
	v.spr = make([]uint16, ScreenW*ScreenH)
	return v
}

// decodeGfx turns the graphics ROMs (four 16-bit-wide ROMs side by side,
// 8 bytes a row of 16 pixels: bytes 0-3 are bit planes 0-3 of the left 8
// pixels, bytes 4-7 of the right 8, leftmost pixel in bit 7) into pens.
func decodeGfx(rom []byte) []uint8 {
	n := len(rom) / 8
	pens := make([]uint8, n*16)
	for g := 0; g < n; g++ {
		b := rom[g*8 : g*8+8]
		for half := 0; half < 2; half++ {
			p0, p1, p2, p3 := b[half*4], b[half*4+1], b[half*4+2], b[half*4+3]
			for x := 0; x < 8; x++ {
				s := uint(7 - x)
				pens[g*16+half*8+x] = (p0>>s)&1 | (p1>>s)&1<<1 | (p2>>s)&1<<2 | (p3>>s)&1<<3
			}
		}
	}
	return pens
}

func (v *video) reset() {
	v.gfxram = [0x30000]uint8{}
	v.cpsa = [0x20]uint16{}
	v.cpsb = [0x20]uint16{}
	v.palRaw = [0xc00]uint16{}
	v.pal = [0xc00]uint32{}
	v.obj = [0x800]uint8{}
	v.palPending = false
}

// vblank: the palette copy (if asked for), then the frame, then the sprite
// list copy for the next frame.
func (v *video) vblank() {
	if v.palPending {
		v.palPending = false
		v.copyPalette()
	}
	v.render()
	v.latchSprites()
}

// word reads graphics RAM at a byte offset (outside it reads 0).
func (v *video) word(a int) int {
	if a < 0 || a+1 >= len(v.gfxram) {
		return 0
	}
	return int(v.gfxram[a])<<8 | int(v.gfxram[a+1])
}

// base: where a base register points in graphics RAM, aligned as the chip does.
func (v *video) base(reg int, align int) int {
	return (int(v.cpsa[reg]) << 8) & 0x3ffff &^ (align - 1)
}

// mapTile: a tile's place in the ROM in 64-byte units, or -1.
func (v *video) mapTile(kind, units int) int {
	for _, r := range v.cfg.Mapper {
		if r.Kinds&kind != 0 && units >= r.Start && units <= r.End {
			return units + r.Offset
		}
	}
	return -1
}

// writeA is a CPS-A register write.
func (v *video) writeA(reg int, val uint16, mask uint16) {
	reg &= 0x1f
	v.cpsa[reg] = v.cpsa[reg]&^mask | val&mask
	if reg == regPalBase {
		v.palPending = true
	}
}

// readB is a CPS-B register read.
func (v *video) readB(off int) uint16 {
	b := &v.cfg.CPSB
	switch off {
	case b.IDOffset:
		return uint16(b.IDValue)
	case b.MulLo:
		return uint16(int(v.cpsb[b.MulA>>1]) * int(v.cpsb[b.MulB>>1]))
	case b.MulHi:
		return uint16((int(v.cpsb[b.MulA>>1]) * int(v.cpsb[b.MulB>>1])) >> 16)
	}
	return 0xffff
}

func (v *video) writeB(off int, val uint16, mask uint16) {
	r := (off >> 1) & 0x1f
	v.cpsb[r] = v.cpsb[r]&^mask | val&mask
}

func (v *video) regB(off int) uint16 {
	if off < 0 {
		return 0
	}
	return v.cpsb[(off>>1)&0x1f]
}

// copyPalette copies the palette pages the palette control picks from the
// palette base (each picked page takes the next 0x400 bytes).
func (v *video) copyPalette() {
	ctrl := 0x3f
	if v.cfg.CPSB.PalCtrl >= 0 {
		ctrl = int(v.regB(v.cfg.CPSB.PalCtrl))
	}
	src := v.base(regPalBase, 0x400)
	for page := 0; page < 6; page++ {
		if ctrl&(1<<page) == 0 {
			continue
		}
		for i := 0; i < 0x200; i++ {
			c := uint16(v.word(src + i*2))
			n := page*0x200 + i
			v.palRaw[n] = c
			v.pal[n] = colour(c)
		}
		src += 0x400
	}
}

// colour: a palette word (brightness, red, green, blue: 4 bits each) as RGB.
func colour(c uint16) uint32 {
	br := 0x0f + int(c>>12)*2
	r := int(c>>8&15) * 0x11 * br / 0x2d
	g := int(c>>4&15) * 0x11 * br / 0x2d
	b := int(c&15) * 0x11 * br / 0x2d
	return uint32(r)<<16 | uint32(g)<<8 | uint32(b)
}

// latchSprites copies the sprite list (done at vblank).
func (v *video) latchSprites() {
	b := v.base(regObjBase, 0x800)
	for i := range v.obj {
		if b+i < len(v.gfxram) {
			v.obj[i] = v.gfxram[b+i]
		} else {
			v.obj[i] = 0
		}
	}
}

// ---------------------------------------------------------------- drawing

// drawSprites draws the latched sprite list into v.spr.
func (v *video) drawSprites() {
	for i := range v.spr {
		v.spr[i] = clear16
	}
	ob := v.obj[:]
	w := func(i int) int { return int(ob[i*2])<<8 | int(ob[i*2+1]) }
	last := 0
	for last < 256 && w(last*4+3)&0xff00 != 0xff00 {
		last++
	}
	for e := last - 1; e >= 0; e-- {
		x, y, code, attr := w(e*4), w(e*4+1), w(e*4+2), w(e*4+3)
		col := (attr & 0x1f) << 4
		fx, fy := attr&0x20 != 0, attr&0x40 != 0
		nx, ny := (attr>>8&15)+1, (attr>>12&15)+1
		for j := 0; j < ny; j++ {
			for i := 0; i < nx; i++ {
				ci, cj := i, j
				if fx {
					ci = nx - 1 - i
				}
				if fy {
					cj = ny - 1 - j
				}
				c := code&^0xf + (code+ci)&0xf + 0x10*cj
				sx := (x + i*16) & 0x1ff
				sy := (y + j*16) & 0x1ff
				v.drawSprite(c&0xffff, col, fx, fy, sx-hx0, sy-hy0)
				// sprites wrap around the 512x512 space
				if sx-hx0+16 > 512 {
					v.drawSprite(c&0xffff, col, fx, fy, sx-hx0-512, sy-hy0)
				}
				if sy-hy0+16 > 512 {
					v.drawSprite(c&0xffff, col, fx, fy, sx-hx0, sy-hy0-512)
				}
			}
		}
	}
}

func (v *video) drawSprite(code, col int, fx, fy bool, px, py int) {
	if px <= -16 || px >= ScreenW || py <= -16 || py >= ScreenH {
		return
	}
	m := v.mapTile(KindSprites, code<<1)
	if m < 0 {
		return
	}
	at := m * 8 * 16 // pens
	if at+256 > len(v.pens) {
		return
	}
	for ty := 0; ty < 16; ty++ {
		yy := py + ty
		if yy < 0 || yy >= ScreenH {
			continue
		}
		ry := ty
		if fy {
			ry = 15 - ty
		}
		row := v.pens[at+ry*16 : at+ry*16+16]
		dst := v.spr[yy*ScreenW:]
		for tx := 0; tx < 16; tx++ {
			xx := px + tx
			if xx < 0 || xx >= ScreenW {
				continue
			}
			rx := tx
			if fx {
				rx = 15 - tx
			}
			if p := row[rx]; p != 15 {
				dst[xx] = uint16(col | int(p))
			}
		}
	}
}

// drawLine draws one scroll layer's line (n = 1..3) into out; with high,
// it also marks the pixels whose pens the priority masks put in front of
// the sprites.
func (v *video) drawLine(n, hy int, out []uint16, high []bool) {
	var masks [4]uint16
	if high != nil {
		for g := 0; g < 4; g++ {
			masks[g] = v.regB(v.cfg.CPSB.Prio[g])
		}
		for i := range high {
			high[i] = false
		}
	}
	var size, mapW, kind, shift, palBase int
	var sx, sy int
	switch n {
	case 1:
		size, kind, shift, palBase = 8, KindScroll1, 0, 0x200
		sx, sy = int(v.cpsa[regS1X]), int(v.cpsa[regS1Y])
	case 2:
		size, kind, shift, palBase = 16, KindScroll2, 1, 0x400
		sx, sy = int(v.cpsa[regS2X]), int(v.cpsa[regS2Y])
		if v.cpsa[regVideoCtl]&1 != 0 {
			rb := v.base(regRowBase, 0x800)
			sx += v.word(rb + ((int(v.cpsa[regRowOffs])+hy)&0x3ff)*2)
		}
	default:
		size, kind, shift, palBase = 32, KindScroll3, 3, 0x600
		sx, sy = int(v.cpsa[regS3X]), int(v.cpsa[regS3Y])
	}
	mapW = size * 64
	base := v.base(regS1Base+n-1, 0x4000)
	ty := (sy + hy) & (mapW - 1)
	row, yin := ty/size, ty%size
	x := 0
	tx := (sx + hx0) & (mapW - 1)
	for x < ScreenW {
		col, xin := tx/size, tx%size
		var idx int
		switch n {
		case 1:
			idx = row&0x1f | (col&0x3f)<<5 | (row&0x20)<<6
		case 2:
			idx = row&0x0f | (col&0x3f)<<4 | (row&0x30)<<6
		default:
			idx = row&0x07 | (col&0x3f)<<3 | (row&0x38)<<6
		}
		code := v.word(base + idx*4)
		attr := v.word(base + idx*4 + 2)
		cnt := min(size-xin, ScreenW-x)
		m := v.mapTile(kind, code<<shift)
		ry := yin
		if attr&0x40 != 0 {
			ry = size - 1 - yin
		}
		var rowPens []uint8
		if m >= 0 {
			var at int
			switch n {
			case 1:
				// 8x8 tiles use one half of each 16-pixel row: even map
				// columns the left half, odd columns the right
				at = (m*8+ry)*16 + (col&1)*8
			case 2:
				at = (m*8 + ry) * 16
			default:
				// a 32-pixel row is two 16-pixel ROM rows
				at = (m*8 + ry*2) * 16
			}
			if at+size <= len(v.pens) {
				rowPens = v.pens[at : at+size]
			}
		}
		pal := palBase | (attr&0x1f)<<4
		fx := attr&0x20 != 0
		mask := masks[(attr>>7)&3]
		for k := 0; k < cnt; k++ {
			px := xin + k
			if fx {
				px = size - 1 - px
			}
			p := uint8(15)
			if rowPens != nil {
				p = rowPens[px]
			}
			if p == 15 {
				out[x+k] = clear16
				continue
			}
			out[x+k] = uint16(pal | int(p))
			if high != nil && mask&(1<<p) != 0 {
				high[x+k] = true
			}
		}
		x += cnt
		tx = (tx + cnt) & (mapW - 1)
	}
}

// render draws the frame.
func (v *video) render() {
	b := &v.cfg.CPSB
	lc := v.regB(b.LayerCtrl)
	var order [4]int
	for s := 0; s < 4; s++ {
		order[s] = int(lc>>(6+2*s)) & 3
	}
	on := func(l int) bool { return l == 0 || lc&b.LayerEnable[l-1] != 0 }
	v.drawSprites()
	bg := v.pal[0xbff]
	for y := 0; y < ScreenH; y++ {
		hy := y + hy0
		highSlot := -1
		for s := 0; s < 4; s++ {
			l := order[s]
			if l == 0 || !on(l) {
				continue
			}
			var hi []bool
			if s < 3 && order[s+1] == 0 && highSlot < 0 {
				hi = v.high[:]
				highSlot = s
			}
			v.drawLine(l, hy, v.line[s][:], hi)
		}
		dst := v.screen[y*ScreenW : (y+1)*ScreenW]
		for x := 0; x < ScreenW; x++ {
			c := uint16(clear16)
			for s := 0; s < 4; s++ {
				l := order[s]
				if !on(l) {
					continue
				}
				var p uint16
				if l == 0 {
					p = v.spr[y*ScreenW+x]
					if p != clear16 && s > 0 && s-1 == highSlot && v.high[x] {
						continue
					}
				} else {
					p = v.line[s][x]
				}
				if p != clear16 {
					c = p
				}
			}
			if c == clear16 {
				dst[x] = bg
			} else {
				dst[x] = v.pal[c]
			}
		}
	}
	if v.cpsa[regVideoCtl]&0x8000 != 0 {
		// flip screen: the whole picture turned round
		s := v.screen
		for i, j := 0, len(s)-1; i < j; i, j = i+1, j-1 {
			s[i], s[j] = s[j], s[i]
		}
	}
}
