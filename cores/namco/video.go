package namco

import (
	"fmt"

	"retropixels/cores/arcade"
)

// The picture: 36x28 characters laid out as on Pac-Man (the two columns at
// each end of the screen come from the ends of video RAM), 64 sprites, and
// behind them Galaga's starfield or Dig Dug's playfield.
//
// Sprite RAM is the last 128 bytes of each work RAM: at $8B80 the code and
// colour, at $9380 the position (line, then column), at $9B80 the flips and
// sizes (bit 0 flip x, bit 1 flip y, bit 2 double width, bit 3 double
// height) and the column's top bits and the hide bit in the second byte.

type video struct {
	kind                 kind
	chars                []uint8 // 8x8, one byte per pixel
	sprites              []uint8 // 16x16
	bgTiles              []uint8 // Dig Dug's playfield tiles
	bgMap                []uint8 // Dig Dug's playfield ROM
	palette              [32]uint32
	charPen              [256]uint32 // colour*4+pixel (Dig Dug: colour*2+pixel)
	charClear            [256]bool
	sprPen               [256]uint32
	sprClear             [256]bool
	bgPen                [256]uint32
	starPal              [64]uint32
	stars                starfield
	opaque               []bool // pixels already covered this frame
	charRom, sprRom      []uint8
	lookupChr, lookupSpr []uint8
}

// tileOffset: where character column tc, row tr is in video RAM (the
// Pac-Man layout).
func tileOffset(tc, tr int) int {
	if tc < 2 {
		return 0x3c0 + tc*32 + 2 + tr
	}
	if tc >= 34 {
		return (tc-34)*32 + 2 + tr
	}
	return 0x40 + tr*32 + (tc - 2)
}

// promColour: 3 bits red, 3 bits green, 2 bits blue through weighted
// resistors.
func promColour(c uint8) uint32 {
	bit := func(n uint) int { return int(c>>n) & 1 }
	return arcade.RGB(0x21*bit(0)+0x47*bit(1)+0x97*bit(2), 0x21*bit(3)+0x47*bit(4)+0x97*bit(5), 0x47*bit(6)+0x97*bit(7))
}

// decode2bpp: Namco's 2-bit 8x8 strips: each 8-byte group gives 4 pixels
// of 8 lines, bit n and bit n+4 being one pixel's two bits.
func decode2bpp(rom []uint8, base, x, y int, strips []int) uint8 {
	v := rom[base+strips[x>>2]+y]
	n := x & 3
	return ((v>>(7-n))&1)<<1 | (v>>(3-n))&1
}

func decodeChars(rom []uint8) []uint8 {
	n := len(rom) / 16
	out := make([]uint8, n*64)
	for t := 0; t < n; t++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				out[t*64+y*8+x] = decode2bpp(rom, t*16, x, y, []int{8, 0})
			}
		}
	}
	return out
}

func decodeSprites(rom []uint8) []uint8 {
	n := len(rom) / 64
	out := make([]uint8, n*256)
	for s := 0; s < n; s++ {
		for y := 0; y < 16; y++ {
			half := 0
			if y >= 8 {
				half = 32
			}
			for x := 0; x < 16; x++ {
				out[s*256+y*16+x] = decode2bpp(rom, s*64+half, x, y&7, []int{0, 8, 16, 24})
			}
		}
	}
	return out
}

func newVideo(k kind, g *arcade.Game, files arcade.Files) (*video, error) {
	v := &video{kind: k, opaque: make([]bool, scrW*scrH)}
	pal := arcade.Region(g, files, "palette", 32)
	for i := range v.palette {
		v.palette[i] = promColour(pal[i])
	}
	chr := arcade.Region(g, files, "chrlut", 256)
	spr := arcade.Region(g, files, "sprlut", 256)
	switch k {
	case galaga:
		v.chars = decodeChars(arcade.Region(g, files, "chars", 0x1000))
		v.sprites = decodeSprites(arcade.Region(g, files, "sprites", 0x2000))
		for i := 0; i < 256; i++ {
			// only the low 4 bits count (some dumps have junk on top)
			c := chr[i] & 15
			v.charPen[i] = v.palette[16+c]
			v.charClear[i] = c == 15
			s := spr[i] & 15
			v.sprPen[i] = v.palette[s]
			v.sprClear[i] = s == 15
		}
		// the 05xx's 64 colours: 2 bits each of red, green and blue
		lv := [4]int{0x00, 0x47, 0x97, 0xde}
		for i := range v.starPal {
			v.starPal[i] = arcade.RGB(lv[i&3], lv[i>>2&3], lv[i>>4&3])
		}
	case digdug:
		v.chars = decodeChars1bpp(arcade.Region(g, files, "chars", 0x800))
		v.sprites = decodeSprites(arcade.Region(g, files, "sprites", 0x4000))
		v.bgTiles = decodeChars(arcade.Region(g, files, "bgtiles", 0x1000))
		v.bgMap = arcade.Region(g, files, "bgmap", 0x1000)
		for i := 0; i < 256; i++ {
			s := spr[i] & 15
			v.sprPen[i] = v.palette[16+s]
			v.sprClear[i] = s == 15
			v.bgPen[i] = v.palette[chr[i]&15]
		}
	default:
		return nil, fmt.Errorf("namco: unknown board")
	}
	return v, nil
}

// decodeChars1bpp: Dig Dug's characters, one bit a pixel, 8 bytes each (a
// byte is a column of 8 pixels... see the MAME comparison in the tests).
func decodeChars1bpp(rom []uint8) []uint8 {
	n := len(rom) / 8
	out := make([]uint8, n*64)
	for t := 0; t < n; t++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				out[t*64+y*8+x] = rom[t*8+y] >> (7 - x) & 1
			}
		}
	}
	return out
}

func (v *video) reset() { v.stars.reset() }

func (v *video) render(b *board) {
	for i := range v.opaque {
		v.opaque[i] = false
	}
	scr := b.screen
	switch v.kind {
	case galaga:
		for i := range scr {
			scr[i] = 0
		}
		v.drawStars(b)
		v.drawSprites(b, 0)
		v.drawCharsGalaga(b)
	case digdug:
		v.drawBG(b)
		v.drawSprites(b, 0)
		v.drawCharsDigdug(b)
	}
	if b.vlat[7] != 0 {
		// flipped (cocktail): the whole picture turned around
		for i, j := 0, len(scr)-1; i < j; i, j = i+1, j-1 {
			scr[i], scr[j] = scr[j], scr[i]
		}
	}
}

func (v *video) endFrame(b *board) {
	if v.kind == galaga {
		v.stars.step(b)
	}
}

func (v *video) drawCharsGalaga(b *board) {
	scr := b.screen
	for tr := 0; tr < 28; tr++ {
		for tc := 0; tc < 36; tc++ {
			o := tileOffset(tc, tr)
			t := int(b.vram[o]) * 64
			col := int(b.vram[0x400+o]&0x3f) << 2
			for y := 0; y < 8; y++ {
				d := (tr*8+y)*scrW + tc*8
				row := v.chars[t+y*8 : t+y*8+8]
				for x := 0; x < 8; x++ {
					p := col | int(row[x])
					if !v.charClear[p] {
						scr[d+x] = v.charPen[p]
					}
				}
			}
		}
	}
}

func (v *video) drawSprites(b *board, _ int) {
	scr := b.screen
	r1, r2, r3 := b.ram[0][0x380:], b.ram[1][0x380:], b.ram[2][0x380:]
	for n := 0; n < 64; n++ {
		o := n * 2
		code := int(r1[o])
		col := int(r1[o+1]&0x3f) << 2
		attr := r3[o]
		if r3[o+1]&2 != 0 {
			continue // hidden
		}
		sx := int(r2[o+1]) - 40 + 0x100*int(r3[o+1]&1)
		sy := 256 - int(r2[o]) + 1
		w, h := 1+int(attr>>2&1), 1+int(attr>>3&1)
		if v.kind == galaga {
			code &= 0x7f
		}
		sy -= 16 * (h - 1)
		sy = (sy & 0xff) - 32
		fx, fy := attr&1 != 0, attr&2 != 0
		for j := 0; j < h; j++ {
			for i := 0; i < w; i++ {
				ci, cj := i, j
				if fx {
					ci = w - 1 - i
				}
				if fy {
					cj = h - 1 - j
				}
				c := (code + ci + cj*2) & 0xff
				if c*256 >= len(v.sprites) {
					continue
				}
				v.drawSprite(scr, c, col, sx+i*16, sy+j*16, fx, fy)
			}
		}
	}
}

func (v *video) drawSprite(scr []uint32, code, col, sx, sy int, fx, fy bool) {
	g := v.sprites[code*256 : code*256+256]
	for y := 0; y < 16; y++ {
		yy := sy + y
		if yy < 0 || yy >= scrH {
			continue
		}
		gy := y
		if fy {
			gy = 15 - y
		}
		for x := 0; x < 16; x++ {
			xx := sx + x
			if xx < 0 || xx >= scrW {
				continue
			}
			gx := x
			if fx {
				gx = 15 - x
			}
			p := col | int(g[gy*16+gx])
			if !v.sprClear[p] {
				scr[yy*scrW+xx] = v.sprPen[p]
			}
		}
	}
}

func (v *video) drawBG(b *board) {
	scr := b.screen
	for i := range scr {
		scr[i] = 0
	}
}

func (v *video) drawCharsDigdug(b *board) {
	scr := b.screen
	for tr := 0; tr < 28; tr++ {
		for tc := 0; tc < 36; tc++ {
			o := tileOffset(tc, tr)
			code := int(b.vram[o])
			t := (code & 0x7f) * 64
			col := code >> 4 & 0x0e
			for y := 0; y < 8; y++ {
				d := (tr*8+y)*scrW + tc*8
				row := v.chars[t+y*8 : t+y*8+8]
				for x := 0; x < 8; x++ {
					if row[x] != 0 {
						scr[d+x] = v.palette[col|1]
					}
				}
			}
		}
	}
}
