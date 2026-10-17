package sega16

import "retropixels/cores/arcade"

// System 16B video: a text layer and two scrolling tile layers (315-5197),
// sprites (315-5196) and 2048 colours of palette RAM with shadow and
// hilight.
//
// Tiles are 8x8, 3 bits per pixel, one bitplane per tile ROM. Tile RAM holds
// 16 pages of 64x32 tiles; each tile layer shows a 2x2 arrangement of pages
// (1024x512 pixels) chosen by its page register and scrolled over it. A tile
// word is p------- -------- (priority), the low 13 bits the tile number and
// bits 6-12 the colour (they overlap: the art is laid out for it). Bit 12 of
// the number picks one of the two tile bank registers, which give the
// number's upper bits. Text RAM is 64x28 tiles, of which columns 24-63 show;
// a text word is p------c ccnnnnnn nnn (priority, colour 0-7, tile 0-511).
// Text RAM also holds the layers' registers:
//
//	0xe80/0xe82  page select, foreground / background (4 bits per quadrant)
//	0xe90/0xe92  vertical scroll (bit 15: per-column scroll from 0xf00)
//	0xe98/0xe9a  horizontal scroll (bit 15: per-row scroll from 0xf80)
//
// Sprites are drawn into a line of their own first (a later entry covers an
// earlier one), then mixed with the tile layers by priority. From back to
// front: sprite priority 0, background (low), sprite priority 1, background
// (high), foreground (low), sprite priority 2, foreground (high), text (low),
// sprite priority 3, text (high). Sprite colour 0x3f is a shadow: it darkens
// what is under it, or brightens it when that colour's bit 15 is set.

const (
	scrW = 320
	scrH = 224
)

// Mixing levels (see above). Tile levels are per priority bit. Level 0 is
// the backdrop: the background layer's pen 0, which shows its colour but is
// behind everything.
var (
	bgLevel   = [2]uint8{2, 4}
	fgLevel   = [2]uint8{5, 7}
	textLevel = [2]uint8{8, 10}
	sprLevel  = [4]uint8{1, 3, 6, 9}
)

// The DAC levels for a 5-bit colour value: normal, shadowed and hilighted
// (measured from the board's resistor network as MAME presents it).
var (
	dacNormal = [32]uint8{0, 8, 16, 25, 33, 41, 49, 58, 66, 74, 82, 91, 99, 107, 115, 123,
		132, 140, 148, 156, 164, 173, 181, 189, 197, 206, 214, 222, 230, 239, 247, 255}
	dacShadow = [32]uint8{0, 7, 13, 20, 26, 32, 39, 45, 52, 58, 64, 71, 77, 84, 90, 97,
		103, 110, 116, 123, 129, 136, 142, 149, 155, 161, 168, 174, 181, 187, 193, 200}
	dacHilight = [32]uint8{55, 62, 68, 74, 81, 87, 94, 100, 106, 113, 119, 126, 132, 139, 145, 152,
		158, 165, 171, 178, 184, 191, 197, 203, 210, 216, 223, 229, 235, 242, 248, 255}
)

type video struct {
	tileRAM [0x8000]uint16
	textRAM [0x800]uint16
	sprRAM  [0x400]uint16
	palRAM  [0x800]uint16
	bank    [2]int // tile bank registers
	enable  bool   // display enable (I/O output bit 5)

	tiles   []uint8  // decoded tiles: 64 pens each
	ntiles  int      // power of two
	sprites []uint16 // sprite ROM words
	sprBank [16]int  // sprite bank -> word offset (or -1: no ROM)

	normal, shadow, hilight [0x800]uint32

	sprPix []uint16 // a frame of sprite pixels: colour<<4|pen, 0 = none
	sprPri []uint8  // and their priority (bit 7: shadow)
	screen []uint32
}

func newVideo(tileROM [3][]byte, sprWords []uint16, sprBanks [16]int) *video {
	v := &video{sprites: sprWords, sprBank: sprBanks}
	n := len(tileROM[0]) / 8
	v.ntiles = 1
	for v.ntiles < n {
		v.ntiles <<= 1
	}
	v.tiles = make([]uint8, v.ntiles*64)
	for t := 0; t < n; t++ {
		for y := 0; y < 8; y++ {
			var b [3]uint8
			for p := range b {
				if a := t*8 + y; a < len(tileROM[p]) {
					b[p] = tileROM[p][a]
				}
			}
			for x := 0; x < 8; x++ {
				s := uint(7 - x)
				v.tiles[t*64+y*8+x] = (b[0]>>s)&1 | ((b[1]>>s)&1)<<1 | ((b[2]>>s)&1)<<2
			}
		}
	}
	v.sprPix = make([]uint16, scrW*scrH)
	v.sprPri = make([]uint8, scrW*scrH)
	v.screen = make([]uint32, scrW*scrH)
	return v
}

func (v *video) reset() {
	v.tileRAM = [0x8000]uint16{}
	v.textRAM = [0x800]uint16{}
	v.sprRAM = [0x400]uint16{}
	v.palRAM = [0x800]uint16{}
	for i := range v.palRAM {
		v.setColor(i)
	}
	v.bank = [2]int{0, 1}
	v.enable = false
}

// setColor works out palette entry i: ---- bgr BBBB GGGG RRRR, the bgr bits
// being the colours' lowest bits; bit 15 makes shadows over it hilights.
func (v *video) setColor(i int) {
	w := int(v.palRAM[i])
	r := (w&15)<<1 | (w>>12)&1
	g := (w>>4&15)<<1 | (w>>13)&1
	b := (w>>8&15)<<1 | (w>>14)&1
	v.normal[i] = arcade.RGB(int(dacNormal[r]), int(dacNormal[g]), int(dacNormal[b]))
	v.shadow[i] = arcade.RGB(int(dacShadow[r]), int(dacShadow[g]), int(dacShadow[b]))
	if w&0x8000 != 0 {
		v.shadow[i] = arcade.RGB(int(dacHilight[r]), int(dacHilight[g]), int(dacHilight[b]))
	}
	v.hilight[i] = arcade.RGB(int(dacHilight[r]), int(dacHilight[g]), int(dacHilight[b]))
}

// tilePen: pen of tile word w at pixel (x, y) of the tile; and its palette entry.
func (v *video) tileCode(w uint16) int {
	code := int(w) & 0xfff
	code |= v.bank[(w>>12)&1] << 12
	return code & (v.ntiles - 1)
}

// layerLine draws one tile layer's line y into pix (palette entry | level<<12,
// 0 = transparent).
func (v *video) layerLine(layer, y int, pix []uint16) {
	t := &v.textRAM
	pages := t[0x740+layer]
	vs := t[0x748+layer]
	hs := t[0x74c+layer]
	if hs&0x8000 != 0 {
		// per-row horizontal scroll: one value per 8 lines
		hs = t[0x7c0+((y>>3)&31)*2+layer]
	}
	colScroll := vs&0x8000 != 0
	levels := &fgLevel
	if layer == 1 {
		levels = &bgLevel
	}
	for x := 0; x < scrW; x++ {
		vsx := vs
		if colScroll {
			vsx = t[0x780+((x>>4)&31)*2+layer]
		}
		vy := (y + int(vsx)) & 511
		vx := (x + 0xc0 - int(hs)) & 1023
		q := (vy >> 8 << 1) | vx>>9
		page := int(pages>>(4*quadNibble[q])) & 15
		w := v.tileRAM[page<<11|(vy>>3&31)<<6|(vx>>3&63)]
		pen := v.tiles[v.tileCode(w)<<6|(vy&7)<<3|vx&7]
		if pen == 0 {
			// transparent; the background's pen 0 is the backdrop
			pix[x] = 0
			if layer == 1 {
				pix[x] = uint16((int(w) >> 6 & 0x7f) << 3)
			}
			continue
		}
		pix[x] = uint16((int(w)>>6&0x7f)<<3|int(pen)) | uint16(levels[w>>15])<<12
	}
}

// quadNibble: which 4 bits of the page register pick each quadrant
// (top left, top right, bottom left, bottom right).
var quadNibble = [4]uint{2, 3, 0, 1}

// textLine draws the text layer's line y.
func (v *video) textLine(y int, pix []uint16) {
	row := (y >> 3) & 31
	for x := 0; x < scrW; x++ {
		vx := x + 192
		w := v.textRAM[row<<6|(vx>>3)&63]
		pen := v.tiles[(int(w)&0x1ff)<<6|(y&7)<<3|vx&7]
		if pen == 0 {
			pix[x] = 0
			continue
		}
		pix[x] = uint16((int(w)>>9&7)<<3|int(pen)) | uint16(textLevel[w>>15])<<12
	}
}

// drawSprites renders the sprite list into sprPix/sprPri.
func (v *video) drawSprites() {
	clear(v.sprPix)
	clear(v.sprPri)
	s := &v.sprRAM
	for e := 0; e < 128; e++ {
		d := s[e*8 : e*8+8]
		if d[2]&0x8000 != 0 {
			break
		}
		if d[2]&0x4000 != 0 {
			continue
		}
		top, bottom := int(d[0]&0xff), int(d[0]>>8)
		if top >= bottom {
			continue
		}
		bank := v.sprBank[(d[4]>>8)&15]
		if bank < 0 {
			continue
		}
		x0 := int(d[1]&0x1ff) - 0xb8
		flip := d[2]&0x100 != 0
		pitch := int(int8(d[2]))
		addr := int(d[3])
		color := int(d[4] & 0x3f)
		pri := sprLevel[(d[4]>>6)&3]
		if color == 0x3f {
			pri |= 0x80
		}
		hz, vz := int(d[5]&0x1f), int(d[5]>>5&0x1f)
		vacc := vz
		rom := v.sprites[bank:]
		for y := top; y < bottom; y++ {
			addr = (addr + pitch) & 0xffff
			if y > top {
				if vacc += vz; vacc >= 32 {
					vacc -= 32
					addr = (addr + pitch) & 0xffff
				}
			}
			if y >= scrH {
				continue
			}
			v.spriteLine(rom, y, x0, addr, flip, hz, uint16(color<<4), pri)
		}
	}
}

// spriteLine draws one line of a sprite: 4 pixels per ROM word, ending after
// the word whose last pixel (in reading order) is 15. Pens 0 and 15 are
// transparent.
func (v *video) spriteLine(rom []uint16, y, x, a int, flip bool, hz int, color uint16, pri uint8) {
	line := y * scrW
	hacc := 32
	for n := 0; n < 128 && x < scrW; n++ {
		w := rom[a&0xffff]
		if flip {
			a--
			w = w>>12 | (w>>4)&0xf0 | (w<<4)&0xf00 | w<<12
		} else {
			a++
		}
		for k := 12; k >= 0; k -= 4 {
			p := (w >> uint(k)) & 15
			if hz != 0 {
				if hacc += hz; hacc >= 64 {
					hacc -= 64
					continue
				}
			}
			if p != 0 && p != 15 && x >= 0 && x < scrW {
				v.sprPix[line+x] = color | p
				v.sprPri[line+x] = pri
			}
			x++
		}
		if w&15 == 15 {
			break
		}
	}
}

// drawLine mixes line y of the picture.
func (v *video) drawLine(y int, bg, fg, tx []uint16) {
	out := v.screen[y*scrW : (y+1)*scrW]
	if !v.enable {
		clear(out)
		return
	}
	v.layerLine(1, y, bg)
	v.layerLine(0, y, fg)
	v.textLine(y, tx)
	sp, spri := v.sprPix[y*scrW:], v.sprPri[y*scrW:]
	for x := 0; x < scrW; x++ {
		c, lv := bg[x]&0xfff, uint8(bg[x]>>12)
		for _, p := range [2]uint16{fg[x], tx[x]} {
			if p != 0 && uint8(p>>12) >= lv {
				c, lv = p&0xfff, uint8(p>>12)
			}
		}
		col := v.normal[c]
		if s := sp[x]; s != 0 {
			pr := spri[x]
			if pr&0x7f > lv {
				if pr&0x80 != 0 {
					col = v.shadow[c]
				} else {
					col = v.normal[0x400|s]
				}
			}
		}
		out[x] = col
	}
}

// render draws the whole frame.
func (v *video) render() {
	var bg, fg, tx [scrW]uint16
	v.drawSprites()
	for y := 0; y < scrH; y++ {
		v.drawLine(y, bg[:], fg[:], tx[:])
	}
}

func (v *video) appendState(w *arcade.StateW) {
	w.U16s(v.tileRAM[:])
	w.U16s(v.textRAM[:])
	w.U16s(v.sprRAM[:])
	w.U16s(v.palRAM[:])
	w.Int(v.bank[0])
	w.Int(v.bank[1])
	w.Bool(v.enable)
}

func (v *video) loadState(r *arcade.StateR) {
	r.U16s(v.tileRAM[:])
	r.U16s(v.textRAM[:])
	r.U16s(v.sprRAM[:])
	r.U16s(v.palRAM[:])
	v.bank[0] = r.Int()
	v.bank[1] = r.Int()
	v.enable = r.Bool()
	for i := range v.palRAM {
		v.setColor(i)
	}
}
