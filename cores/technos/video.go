// Package technos has Technos Japan's Double Dragon boards: Double Dragon
// (1987) and Double Dragon II: The Revenge (1988).
//
// Both run the game on a Hitachi HD6309 at 3 MHz (in its 6809 mode) with
// banked program ROM, and share the picture hardware: a 512x512 background
// of 16x16 tiles that scrolls both ways, a fixed 256x256 layer of 8x8
// characters on top, and 16x16 sprites (which can be 32 wide or tall)
// between them, in 384 colours of 12 bits from palette RAM. A second
// processor builds the game's object list (an HD63701 microcontroller on
// Double Dragon, a Z80 on Double Dragon II), and a third runs the sound: a
// 6809 with a YM2151 and two MSM5205 ADPCM voices on Double Dragon, a Z80
// with a YM2151 and an MSM6295 on Double Dragon II.
//
// The screen is 256x240 of a 384x272 frame (6 MHz pixel clock, 57.44 Hz).
package technos

import (
	"math"

	"retropixels/cores/arcade"
)

const (
	pixelHz    = 6000000
	lineClocks = 384
	lines      = 272
	scrW       = 256
	scrH       = 240
	vblankLine = 240 // the vertical blank starts here: NMI, and the frame is drawn
	fps        = float64(pixelHz) / (lineClocks * lines)
	mainLine   = 192 // HD6309 clocks a line (3 MHz)
	firstLine  = 8   // the screen's top line is line 8 of the layers
)

// gfxLayout says where each bit of a tile is in its ROM, in bits from the
// tile's start (bit 0 is the top bit of the first byte); planes go from the
// highest bit of the pixel value to the lowest.
type gfxLayout struct {
	w, h   int
	planes []int
	xs, ys []int
	step   int // bits from one tile to the next
}

// decode turns count tiles into one byte per pixel (w*h per tile).
func (l *gfxLayout) decode(rom []byte, count int) []uint8 {
	out := make([]uint8, count*l.w*l.h)
	bit := func(o int) uint8 {
		if o>>3 >= len(rom) {
			return 0
		}
		return rom[o>>3] >> (7 - o&7) & 1
	}
	for t := 0; t < count; t++ {
		base := t * l.step
		d := out[t*l.w*l.h:]
		for y := 0; y < l.h; y++ {
			for x := 0; x < l.w; x++ {
				var v uint8
				for _, p := range l.planes {
					v = v<<1 | bit(base+p+l.ys[y]+l.xs[x])
				}
				d[y*l.w+x] = v
			}
		}
	}
	return out
}

// seq is n numbers from a, step apart.
func seq(a, step, n int) []int {
	s := make([]int, n)
	for i := range s {
		s[i] = a + i*step
	}
	return s
}

// The 8x8 characters: 32 bytes each, a byte per pixel pair and line: the
// four bits of the left pixel in the byte's odd bits, the right pixel's in
// its even bits; columns 0-1 in bytes 0-7, 2-3 in 8-15 and so on.
func charLayout() *gfxLayout {
	var xs []int
	for c := 0; c < 4; c++ {
		xs = append(xs, c*64+1, c*64+0)
	}
	return &gfxLayout{w: 8, h: 8, planes: []int{0, 2, 4, 6}, xs: xs, ys: seq(0, 8, 8), step: 256}
}

// The 16x16 tiles and sprites: 64 bytes each in each half of the ROMs;
// a byte holds four pixels of two planes (high nibble the higher plane),
// the other two planes in the other half; columns 0-3 are in bytes 0-15
// (one a line), 4-7 in 16-31, 8-11 in 32-47, 12-15 in 48-63.
func tileLayout(romBytes int) *gfxLayout {
	half := romBytes * 8
	var xs []int
	for c := 0; c < 4; c++ {
		xs = append(xs, c*128+3, c*128+2, c*128+1, c*128+0)
	}
	return &gfxLayout{w: 16, h: 16, planes: []int{half, half + 4, 0, 4}, xs: xs, ys: seq(0, 8, 16), step: 512}
}

// video is the picture hardware both boards share.
type video struct {
	chars, tiles, sprites []uint8 // decoded, a byte a pixel
	nChars, nTiles, nSpr  int

	fg, bg, spr  []uint8 // the board's character, background and sprite RAM
	palLo, palHi []uint8 // palette RAM: red/green bytes and blue bytes
	scrollX      int     // 9 bits
	scrollY      int
	flip         bool
	sprBase      int  // where the sprite list starts in spr
	sprCount     int  // how many five-byte entries the list has
	dd2Sprites   bool // Double Dragon II's sprite code / colour bits
	raw          [256 * 256]uint16
	rowLo, rowHi int // the raw rows being drawn
	screen       []uint32
	pal          [384]uint32
}

func newVideo(chars, tiles, sprites []byte) *video {
	v := &video{}
	v.nChars = len(chars) / 32
	v.chars = charLayout().decode(chars, v.nChars)
	v.nTiles = len(tiles) / 128
	v.tiles = tileLayout(len(tiles)/2).decode(tiles, v.nTiles)
	v.nSpr = len(sprites) / 128
	v.sprites = tileLayout(len(sprites)/2).decode(sprites, v.nSpr)
	v.screen = make([]uint32, scrW*scrH)
	v.sprCount = 64
	return v
}

// colour turns palette entry i (xxxxGGGG RRRR, xxxxBBBB) into a pixel.
func (v *video) colour(i int) uint32 {
	lo, hi := int(v.palLo[i]), int(v.palHi[i])
	return arcade.RGB((lo&15)*17, (lo>>4)*17, (hi&15)*17)
}

// render draws the whole frame.
func (v *video) render() { v.renderRows(0, scrH) }

// renderRows draws screen rows y0..y1-1 from the video RAM as it is now
// (the boards draw each row as the beam reaches it, so changes made while
// the picture is being drawn show from the next row on): background,
// sprites, characters.
func (v *video) renderRows(y0, y1 int) {
	if y0 >= y1 {
		return
	}
	for i := range v.pal {
		v.pal[i] = v.colour(i)
	}
	lo, hi := y0, y1
	if v.flip {
		lo, hi = scrH-y1, scrH-y0
	}
	v.rowLo, v.rowHi = lo, hi
	raw := &v.raw
	// background: 32x32 tiles in four 16x16 quarters; two bytes a tile:
	// attribute (bits 0-2 code bits 8-10, 3-5 colour, 6 flip x, 7 flip y),
	// then code
	for y := lo; y < hi; y++ {
		by := (y + firstLine + v.scrollY) & 511
		row := by >> 4
		for x := 0; x < scrW; x++ {
			bx := (x + v.scrollX) & 511
			col := bx >> 4
			o := 2 * (col&15 | (row&15)<<4 | (col&16)<<4 | (row&16)<<5)
			attr := int(v.bg[o])
			code := (int(v.bg[o+1]) | (attr&7)<<8) % v.nTiles
			tx, ty := bx&15, by&15
			if attr&0x40 != 0 {
				tx ^= 15
			}
			if attr&0x80 != 0 {
				ty ^= 15
			}
			raw[y<<8|x] = uint16(256 + (attr>>3&7)<<4 | int(v.tiles[code<<8|ty<<4|tx]))
		}
	}
	v.drawSprites()
	// characters: 32x32; attribute (bits 0-2 code bits 8-10, 5-7 colour), code
	for ty := 0; ty < 32; ty++ {
		if ty*8+8-firstLine <= lo || ty*8-firstLine >= hi {
			continue
		}
		for tx := 0; tx < 32; tx++ {
			o := 2 * (ty<<5 | tx)
			attr := int(v.fg[o])
			code := (int(v.fg[o+1]) | (attr&7)<<8) % v.nChars
			c := (attr >> 5) << 4
			t := v.chars[code<<6:]
			for y := 0; y < 8; y++ {
				sy := ty*8 + y - firstLine
				if sy < lo || sy >= hi {
					continue
				}
				d := sy<<8 | tx*8
				for x := 0; x < 8; x++ {
					if p := t[y<<3|x]; p != 0 {
						raw[d+x] = uint16(c | int(p))
					}
				}
			}
		}
	}
	for y := y0; y < y1; y++ {
		for x := 0; x < scrW; x++ {
			sx, sy := x, y
			if v.flip {
				sx, sy = 255-x, scrH-1-y
			}
			v.screen[y*scrW+x] = v.pal[raw[sy<<8|sx]]
		}
	}
}

// sprite placement: x and y count down from these; a wide sprite is the next
// even code to its right, a tall one the next code below it.
const sprX, sprY, sprWideBit, sprTallBit, sprStepX, sprStepY = 240, 232, 0x20, 0x10, 2, 1

// drawSprites draws the 64 sprites of five bytes: y, attribute (bit 7 shown,
// bit 4 tall, bit 5 wide, bit 3 flip x, bit 2 flip y, bit 1 x bit 8, bit 0
// y bit 8), code high bits and colour, code, x.
func (v *video) drawSprites() {
	s := v.spr[v.sprBase:]
	for i := 0; i < v.sprCount; i++ {
		e := s[i*5 : i*5+5]
		attr := int(e[1])
		if attr&0x80 == 0 {
			continue
		}
		var code, colr int
		if v.dd2Sprites {
			code = int(e[3]) | int(e[2]&0x1f)<<8
			colr = int(e[2]>>5) << 4
		} else {
			code = int(e[3]) | int(e[2]&0x0f)<<8
			colr = int(e[2]>>4&7) << 4
		}
		sx := sprX - int(e[4]) + (attr&2)<<7
		sy := sprY - int(e[0]) + (attr&1)<<8
		wide, tall := attr&sprWideBit != 0, attr&sprTallBit != 0
		fx, fy := attr&0x08 != 0, attr&0x04 != 0
		if wide {
			sx -= 16
		}
		if tall {
			sy -= 16
		}
		w, h := 1, 1
		if wide {
			w = 2
			code &^= sprStepX
		}
		if tall {
			h = 2
			code &^= sprStepY
		}
		for dy := 0; dy < h; dy++ {
			for dx := 0; dx < w; dx++ {
				cx, cy := dx, dy
				if fx {
					cx = w - 1 - dx
				}
				if fy {
					cy = h - 1 - dy
				}
				v.sprite(code+cx*sprStepX+cy*sprStepY, colr, sx+dx*16, sy+dy*16, fx, fy)
			}
		}
	}
}

func (v *video) sprite(code, colr, sx, sy int, fx, fy bool) {
	t := v.sprites[(code%v.nSpr)<<8:]
	for y := 0; y < 16; y++ {
		yy := (sy + y) & 511
		if yy < v.rowLo || yy >= v.rowHi {
			continue
		}
		ty := y
		if fy {
			ty = 15 - y
		}
		for x := 0; x < 16; x++ {
			xx := (sx + x) & 511
			if xx > 255 {
				continue
			}
			tx := x
			if fx {
				tx = 15 - x
			}
			if p := t[ty<<4|tx]; p != 0 {
				v.raw[yy<<8|xx] = uint16(128 + colr | int(p))
			}
		}
	}
}

// padBits: active-low bits for the buttons held (bit -> button name).
func padBits(p arcade.Pad, names ...string) uint8 {
	v := uint8(0xff)
	for i, n := range names {
		if n != "" && p[n] {
			v &^= 1 << i
		}
	}
	return v
}

func pad(p []arcade.Pad, i int) arcade.Pad {
	if i < len(p) && p[i] != nil {
		return p[i]
	}
	return arcade.Pad{}
}

// lpCoef is the coefficient of the one-pole low-pass filter (about 15 kHz)
// both boards put on their sound, for a top end like MAME's at rate Hz.
func lpCoef(rate float64) float64 { return 1 - math.Exp(-2*math.Pi*15000/rate) }
