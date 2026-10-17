package ddragon3

import "retropixels/cores/arcade"

// The video: two 512x512 layers of 16x16 tiles that scroll, 256 sprites of
// one to eight 16x16 tiles stacked, and 768 colours (xBBBBBGGGGGRRRRR).
//
// Background (082000): 32x32 words, bits 0-11 the tile (bit 12 from video
// register 0c000c bit 0), 12-15 the colour (colours 512-767); it covers
// what is under it. Foreground (080000): 32x32 pairs of words, the first
// the colour (bits 0-3, colours 256-511) and flips (bit 6 x, bit 7 y), the
// second the tile; colour 0 is clear.
//
// Video registers (0c0000, words): foreground x and y scroll, background
// x and y scroll, then 0c000b bit 0 the screen flip and 0c000c bit 0 the
// background tile bank. 100000 bits 5-6 set which layer is in front.
//
// Sprites (180000): 16 bytes each, only the low byte of each word used:
//
//	0 y (bit 8 in byte 1 bit 1)
//	1 bit 0 shown, 1 y bit 8, 2 x bit 8, 3 flip y, 4 flip x,
//	  5-7 tiles in the column less one
//	2, 3 tile (low, high)   4 colour (bits 0-3; colours 0-255)   5 x
//
// The sprite chip shows the list as it was at the last vblank.

// Screen size.
const (
	ScreenW = 320
	ScreenH = 240
)

type video struct {
	tiles   []byte // background tiles, 256 pixels (0-15) each
	sprites []byte // sprite tiles, the same
	nTiles  int
	nSpr    int

	fgram  [0x1000]byte
	bgram  [0x800]byte
	palram [0x600]byte
	spram  [0x1000]byte
	spbuf  [0x1000]byte // the sprite list being shown
	regs   [16]byte     // 0c0000-0c000f
	prio   [2]byte      // 100000

	pal    [0x300]uint32
	screen []uint32
	line   [ScreenW]uint16 // a line's colour numbers being built
	opaque [ScreenW]bool
}

// decode turns four bit planes into 16x16 tiles: each tile is 32 bytes of
// each plane, the left 8 pixels of its 16 rows then the right 8.
func decode(p [4][]byte) ([]byte, int) {
	n := len(p[0]) / 32
	out := make([]byte, n*256)
	for t := 0; t < n; t++ {
		for y := 0; y < 16; y++ {
			for x := 0; x < 16; x++ {
				o := t*32 + (x>>3)*16 + y
				bit := uint(7 - x&7)
				v := 0
				for k := 0; k < 4; k++ {
					v |= int(p[k][o]>>bit&1) << k
				}
				out[t*256+y*16+x] = byte(v)
			}
		}
	}
	return out, n
}

func newVideo(tiles, sprites [4][]byte) *video {
	v := &video{screen: make([]uint32, ScreenW*ScreenH)}
	v.tiles, v.nTiles = decode(tiles)
	v.sprites, v.nSpr = decode(sprites)
	return v
}

func (v *video) reset() {
	v.fgram = [0x1000]byte{}
	v.bgram = [0x800]byte{}
	v.palram = [0x600]byte{}
	v.spram = [0x1000]byte{}
	v.spbuf = [0x1000]byte{}
	v.regs = [16]byte{}
	v.prio = [2]byte{}
	v.pal = [0x300]uint32{}
	clear(v.screen)
}

// updateColour remakes colour i from palette RAM.
func (v *video) updateColour(i int) {
	c := int(v.palram[i*2])<<8 | int(v.palram[i*2+1])
	five := func(x int) int { x &= 31; return x<<3 | x>>2 }
	v.pal[i] = arcade.RGB(five(c), five(c>>5), five(c>>10))
}

func (v *video) reg(n int) int { return int(v.regs[n*2])<<8 | int(v.regs[n*2+1]) }

// vblank latches the sprite list.
func (v *video) vblank() { v.spbuf = v.spram }

// drawBG fills a line with the background.
func (v *video) drawBG(y int) {
	sx, sy := v.reg(2), v.reg(3)
	bank := (v.reg(6) & 1) << 12
	ty := (y + firstLine + sy) & 511
	for x := 0; x < ScreenW; x++ {
		tx := (x + sx) & 511
		e := int(v.bgram[((ty>>4)*32+tx>>4)*2])<<8 | int(v.bgram[((ty>>4)*32+tx>>4)*2+1])
		t := (e&0xfff | bank) % v.nTiles
		p := v.tiles[t*256+(ty&15)*16+tx&15]
		v.line[x] = uint16(512 + (e>>12)*16 + int(p))
		v.opaque[x] = p != 0
	}
}

// drawFG draws the foreground over a line (opaque: cover everything).
func (v *video) drawFG(y int, opaque bool) {
	sx, sy := v.reg(0), v.reg(1)
	ty := (y + firstLine + sy) & 511
	for x := 0; x < ScreenW; x++ {
		tx := (x + sx) & 511
		o := ((ty>>4)*32 + tx>>4) * 4
		attr := int(v.fgram[o+1])
		t := (int(v.fgram[o+2])<<8 | int(v.fgram[o+3])) % v.nTiles
		px, py := tx&15, ty&15
		if attr&0x40 != 0 {
			px = 15 - px
		}
		if attr&0x80 != 0 {
			py = 15 - py
		}
		p := v.tiles[t*256+py*16+px]
		if p != 0 || opaque {
			v.line[x] = uint16(256 + (attr&15)*16 + int(p))
		}
	}
}

// render draws the whole picture.
func (v *video) render() {
	mode := int(v.prio[1]) >> 5 & 3
	for y := 0; y < ScreenH; y++ {
		switch mode {
		case 3: // foreground at the back
			v.drawFG(y, true)
			v.drawBGOver(y)
		default:
			v.drawBG(y)
			if mode != 1 {
				v.drawFG(y, false)
			}
		}
		row := v.screen[y*ScreenW : (y+1)*ScreenW]
		for x := range row {
			row[x] = v.pal[v.line[x]]
		}
	}
	v.drawSprites()
	if mode == 1 {
		// foreground in front of the sprites
		for y := 0; y < ScreenH; y++ {
			v.drawFGTop(y)
		}
	}
}

// drawBGOver draws the background's coloured pixels over a line.
func (v *video) drawBGOver(y int) {
	sx, sy := v.reg(2), v.reg(3)
	bank := (v.reg(6) & 1) << 12
	ty := (y + firstLine + sy) & 511
	for x := 0; x < ScreenW; x++ {
		tx := (x + sx) & 511
		e := int(v.bgram[((ty>>4)*32+tx>>4)*2])<<8 | int(v.bgram[((ty>>4)*32+tx>>4)*2+1])
		t := (e&0xfff | bank) % v.nTiles
		p := v.tiles[t*256+(ty&15)*16+tx&15]
		if p != 0 {
			v.line[x] = uint16(512 + (e>>12)*16 + int(p))
		}
	}
}

// drawFGTop draws the foreground's coloured pixels straight onto the screen.
func (v *video) drawFGTop(y int) {
	for x := range v.line {
		v.line[x] = 0xffff
	}
	v.drawFG(y, false)
	row := v.screen[y*ScreenW : (y+1)*ScreenW]
	for x, c := range v.line {
		if c != 0xffff {
			row[x] = v.pal[c]
		}
	}
}

// Sprite placement (found by comparing with MAME).
var (
	sprX0 = 0
	sprY0 = 0xe8
)

// drawSprites draws the latched sprite list, the first sprite at the back.
func (v *video) drawSprites() {
	for i := 0; i < 256; i++ {
		s := v.spbuf[i*16 : i*16+16]
		attr := int(s[3])
		if attr&1 == 0 {
			continue
		}
		sx := int(s[11]) | (attr&4)<<6
		sy := int(s[1]) | (attr&2)<<7
		n := attr>>5&7 + 1
		code := int(s[5]) | int(s[7])<<8
		col := int(s[9]) & 15
		fx, fy := attr&0x10 != 0, attr&0x08 != 0
		x := sx - sprX0
		y := sprY0 - sy
		for k := 0; k < n; k++ {
			c := code + k
			if fy {
				c = code + n - 1 - k
			}
			v.drawTile(c, col, x, y-k*16, fx, fy)
		}
	}
}

func (v *video) drawTile(code, col, x0, y0 int, fx, fy bool) {
	t := v.sprites[(code%v.nSpr)*256:]
	for py := 0; py < 16; py++ {
		y := y0 + py
		if y < 0 || y >= ScreenH {
			continue
		}
		ty := py
		if fy {
			ty = 15 - py
		}
		for px := 0; px < 16; px++ {
			x := (x0 + px) & 511
			if x >= ScreenW {
				continue
			}
			tx := px
			if fx {
				tx = 15 - px
			}
			if p := t[ty*16+tx]; p != 0 {
				v.screen[y*ScreenW+x] = v.pal[col*16+int(p)]
			}
		}
	}
}
