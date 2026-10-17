package dec8

// The DEC8 picture: a 512x512 scrolling playfield of 16x16 tiles (Data
// East's BAC06 tile chip), 16x16 sprites (the sprite chip of Karnov's
// board) and a 256x256 layer of 8x8 characters on top. 1024 colours from two
// PROMs: characters use 0x000-0x0ff (eight colours a set), sprites
// 0x100-0x1ff and the playfield 0x200-0x2ff (sixteen a set).

import "retropixels/cores/arcade"

const (
	scrW      = 256
	scrH      = 240
	firstLine = 8 // the first of the 256 lines shown
)

// decodeGfx turns count tiles of size x size pixels into one byte a pixel.
// Each bit plane is a separate area of rom (planes: their byte offsets, the
// highest bit of the pixel first); a tile is size bytes a plane for 8-pixel
// ones, and for 16-pixel ones 32: the left 8 pixels of its 16 lines, then
// the right 8 (the other way round with swap). lsb: the leftmost pixel is
// in each byte's bit 0 (else bit 7).
func decodeGfx(rom []byte, count, size int, planes []int, lsb, swap bool) []uint8 {
	per := size // bytes a plane per tile
	if size == 16 {
		per = 32
	}
	out := make([]uint8, count*size*size)
	for t := 0; t < count; t++ {
		d := out[t*size*size:]
		for y := 0; y < size; y++ {
			for x := 0; x < size; x++ {
				o := t*per + y
				if size == 16 && (x >= 8) != swap {
					o += 16
				}
				var v uint8
				for _, p := range planes {
					b := uint8(0)
					if p+o < len(rom) {
						sh := 7 - x&7
						if lsb {
							sh = x & 7
						}
						b = rom[p+o] >> sh & 1
					}
					v = v<<1 | b
				}
				d[y*size+x] = v
			}
		}
	}
	return out
}

// promPalette makes the 1024 colours: red in the low nibble of the first
// PROM, green in its high nibble, blue in the second; each bit drives a
// resistor (weights about 1, 2, 4, 8 to full scale 255).
func promPalette(rg, b []byte) [1024]uint32 {
	w := [4]int{14, 31, 67, 143}
	c := func(v uint8) int {
		s := 0
		for i := 0; i < 4; i++ {
			if v>>i&1 != 0 {
				s += w[i]
			}
		}
		return s
	}
	var p [1024]uint32
	for i := range p {
		p[i] = arcade.RGB(c(rg[i]&15), c(rg[i]>>4), c(b[i]&15))
	}
	return p
}

// video is the picture hardware's state that the renderer reads.
type video struct {
	chars, tiles, sprites []uint8
	pal                   [1024]uint32
	screen                []uint32
	raw                   []uint16 // palette indexes, 256x256
}

func newVideo(charROM, tileROM, spriteROM, rg, b []byte) *video {
	v := &video{
		chars:   decodeGfx(charROM, 1024, 8, []int{0x6000, 0x4000, 0x2000}, false, false),
		tiles:   decodeGfx(tileROM, 2048, 16, []int{0x20000, 0, 0x30000, 0x10000}, true, false),
		sprites: decodeGfx(spriteROM, 4096, 16, []int{0x60000, 0x40000, 0x20000, 0}, false, true),
		pal:     promPalette(rg, b),
		screen:  make([]uint32, scrW*scrH),
		raw:     make([]uint16, 256*256),
	}
	return v
}

// frameRegs are what the renderer needs from the board for one frame.
type frameRegs struct {
	ctrl   [4]uint8  // BAC06 control (3820, 3822, 3824, 3826)
	scroll [16]uint8 // BAC06 scroll registers (3830-383f)
	flip   bool
}

// render draws one frame: vram is the characters (0x800 bytes), pf the
// playfield (0x800), rows the row scroll table (0x400), spr the sprite list
// (0x800).
func (v *video) render(vram, pf, rows, spr []uint8, r *frameRegs) {
	v.drawPlayfield(pf, rows, r)
	v.drawSprites(spr)
	v.drawChars(vram)
	v.toScreen(r.flip)
}

// pfIndex is the playfield entry for a pixel of the 512x512 map: four
// pages of 16x16 tiles, the two of the left half first.
func pfIndex(px, py int) int {
	page := (px>>8&1)<<1 | py>>8&1
	return page<<8 | (py>>4&15)<<4 | px>>4&15
}

// drawPlayfield draws the BAC06 layer: 9-bit x and y scroll (3830-3833),
// and with bit 2 of control register 0 a signed x offset for each map line
// from the row scroll table (2c00, big-endian words). Map entries are
// big-endian words: colour in the top four bits, tile below. (Ghostbusters
// keeps the chip's other settings fixed: a 512x512 map of 16x16 tiles.)
func (v *video) drawPlayfield(pf, rows []uint8, r *frameRegs) {
	sx := int(r.scroll[0]&1)<<8 | int(r.scroll[1])
	sy := int(r.scroll[2]&1)<<8 | int(r.scroll[3])
	rowOn := r.ctrl[0]&4 != 0
	for y := 0; y < 256; y++ {
		py := (y + sy) & 511
		x0 := sx
		if rowOn {
			x0 += int(int16(uint16(rows[py*2])<<8 | uint16(rows[py*2+1])))
		}
		for x := 0; x < 256; x++ {
			px := (x + x0) & 511
			o := pfIndex(px, py) * 2
			w := int(pf[o])<<8 | int(pf[o+1])
			code := w & 0x7ff
			pen := v.tiles[code<<8|(py&15)<<4|px&15]
			v.raw[y<<8|x] = uint16(0x200 | (w>>12)<<4 | int(pen))
		}
	}
}

// drawSprites draws the sprite list: 256 sprites of 8 bytes (big-endian
// words): y (bit 15: shown), flags (bit 0 shown, 1 flip y, 2 flip x, 4 two
// tiles high), x, and colour (top four bits) and code. Positions count from
// the bottom right of a 512x512 space. Later sprites go over earlier ones.
func (v *video) drawSprites(spr []uint8) {
	for o := 0; o < 0x800; o += 8 {
		w0 := int(spr[o])<<8 | int(spr[o+1])
		w1 := int(spr[o+2])<<8 | int(spr[o+3])
		if w0&0x8000 == 0 || w1&1 == 0 {
			continue
		}
		w2 := int(spr[o+4])<<8 | int(spr[o+5])
		w3 := int(spr[o+6])<<8 | int(spr[o+7])
		x := 256 - (w2+16)&0x1ff
		y := 256 - (w0+16)&0x1ff
		code := w3 & 0xfff
		col := w3 >> 12
		fx, fy := w1&4 != 0, w1&2 != 0
		if w1&0x10 == 0 {
			v.sprite(code, col, x, y, fx, fy)
			continue
		}
		// two tiles high: the even code on top (the odd one when flipped)
		code &^= 1
		top, bot := code, code+1
		if fy {
			top, bot = bot, top
		}
		v.sprite(top, col, x, y-16, fx, fy)
		v.sprite(bot, col, x, y, fx, fy)
	}
}

// sprite draws one 16x16 sprite at x, y (256x256 space; wraps at 512).
func (v *video) sprite(code, col, x, y int, fx, fy bool) {
	t := v.sprites[code<<8:]
	for yy := 0; yy < 16; yy++ {
		sy := (y + yy) & 511
		if sy > 255 {
			continue
		}
		ty := yy
		if fy {
			ty = 15 - yy
		}
		for xx := 0; xx < 16; xx++ {
			sx := (x + xx) & 511
			if sx > 255 {
				continue
			}
			tx := xx
			if fx {
				tx = 15 - xx
			}
			if p := t[ty<<4|tx]; p != 0 {
				v.raw[sy<<8|sx] = uint16(0x100 | col<<4 | int(p))
			}
		}
	}
}

// drawChars draws the 32x32 characters (two bytes each: colour in the top
// six bits and the code's top two bits, then the code's low byte).
func (v *video) drawChars(vram []uint8) {
	for ty := 0; ty < 32; ty++ {
		for tx := 0; tx < 32; tx++ {
			o := (ty<<5 | tx) * 2
			hi, lo := int(vram[o]), int(vram[o+1])
			code := (hi&3)<<8 | lo
			col := hi >> 2
			t := v.chars[code<<6:]
			for y := 0; y < 8; y++ {
				d := (ty*8+y)<<8 | tx*8
				for x := 0; x < 8; x++ {
					if p := t[y<<3|x]; p != 0 {
						v.raw[d+x] = uint16(col<<3 | int(p))
					}
				}
			}
		}
	}
}

func (v *video) toScreen(flip bool) {
	for y := 0; y < scrH; y++ {
		for x := 0; x < scrW; x++ {
			sx, sy := x, y+firstLine
			if flip {
				sx, sy = 255-x, 255-sy
			}
			v.screen[y*scrW+x] = v.pal[v.raw[sy<<8|sx]&0x3ff]
		}
	}
}
