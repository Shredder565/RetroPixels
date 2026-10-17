package psikyo

import "encoding/binary"

// The PS5V2's video: everything is drawn from 16x16 tiles (4 or 8 bits a
// pixel) in the graphics ROMs. Sprite RAM (64 KB) holds:
//
//	0x0000-0x37ff  sprites, 16 bytes each:
//	               long 0: y (bits 25-16), x (bits 9-0), 10-bit signed
//	               long 1: byte 0 flip y (bit 7), height-1 in tiles (bits 3-0)
//	                       byte 1 y zoom, byte 2 flip x (bit 7), priority
//	                       class (bits 5-4), width-1 in tiles (bits 3-0),
//	                       byte 3 x zoom
//	               long 2: colour (bits 31-24), 8 bits a pixel (bit 23),
//	                       first tile (bits 18-0; the others follow row by row)
//	0x3800-0x3fff  the sprite list: 16-bit sprite numbers, bit 14 ends it;
//	               later sprites are drawn over earlier ones whatever their
//	               priorities
//	0x4000-0xffff  0x800-byte banks: tilemaps (32x16 tiles of 32 bits:
//	               colour in the top byte, tile below) and register banks
//	               (per layer: scroll at 0x3f0 + 4*layer; tilemap bank,
//	               priority and blending at 0x7f0 + 4*layer)
//
// The registers at 0x0405ffe0:
//
//	reg 2  the priorities of the four sprite classes (a nibble each from
//	       the top), the shading's priority (bits 3-0)
//	reg 6  each layer's register bank (a byte each, layer 0 in the top byte)
//	reg 7  the shading bank (top byte), the layers' enables (a nibble each
//	       from bit 15 down)
//
// The "shading" is a per-line colour (RRGGBB) and amount (0-0x80) in the
// second half of its bank: everything at or below its priority is blended
// towards that colour.
//
// Priorities go from 0 (back) to 15; sprites go under layers of the same
// priority, and the shading over both.
//
// Palette RAM: 0x1400 entries of 32 bits, RRGGBBxx; a pixel's entry is
// colour*16 + pen (both depths).
type video struct {
	spr [0x10000]byte
	// the sprites and sprite list as they were at the last vblank: the
	// chip draws a frame's sprites during the next frame
	sprBuf [0x4000]byte
	pal    [0x10000]byte // the first 0x5000 bytes are palette RAM
	zoom   [0x200]byte
	regs   [0x20]byte

	gfx []byte // the graphics ROMs as the video chip addresses them, from gfxBase

	screen []uint32
	colors [0x1400]uint32

	// sprite pixels of the frame: colour and priority+1 (0 = none)
	sprCol []uint32
	sprPri []uint8
	sprA   []uint16 // opacity, 0-256
	layers []layerInfo
	line   [ScreenW]uint32
	lpix   [ScreenW]int32 // a layer's line: colour, or -1
}

// Screen size before rotation.
const (
	ScreenW = 320
	ScreenH = 224
)

const (
	gfxBase  = 0x400000  // the first graphics ROM pair's address
	gfxSpace = 0x2c00000 // tile addresses wrap at this size
)

type layerInfo struct {
	pri    int
	info   uint32
	scroll uint32
	deep   bool // 8 bits a pixel
}

func newVideo(gfx []byte) *video {
	return &video{
		gfx:    gfx,
		screen: make([]uint32, ScreenW*ScreenH),
		sprCol: make([]uint32, ScreenW*ScreenH),
		sprPri: make([]uint8, ScreenW*ScreenH),
		sprA:   make([]uint16, ScreenW*ScreenH),
	}
}

func (v *video) reset() {
	v.spr = [0x10000]byte{}
	v.sprBuf = [0x4000]byte{}
	v.pal = [0x10000]byte{}
	v.zoom = [0x200]byte{}
	v.regs = [0x20]byte{}
}

func (v *video) reg(n int) uint32 { return binary.BigEndian.Uint32(v.regs[n*4:]) }

func (v *video) sprLong(a int) uint32 { return binary.BigEndian.Uint32(v.spr[a&0xfffc:]) }

// tileData returns a tile's bytes (128 for 4 bits a pixel, 256 for 8), or
// nil where there is no ROM.
func (v *video) tileData(tile uint32, deep bool) []byte {
	size := 128
	if deep {
		size = 256
	}
	a := int(uint64(tile)*uint64(size)%gfxSpace) - gfxBase
	if a < 0 || a+size > len(v.gfx) {
		return nil
	}
	return v.gfx[a : a+size]
}

// pen reads pixel (x, y) of a tile; 0 is transparent.
func pen(d []byte, x, y int, deep bool) int {
	if deep {
		return int(d[y*16+x])
	}
	b := d[y*8+x>>1]
	if x&1 == 0 {
		return int(b >> 4)
	}
	return int(b & 15)
}

// latch takes the sprites for the next frame (at the end of vblank's
// render).
func (v *video) latch() { copy(v.sprBuf[:], v.spr[:0x4000]) }

// render draws the frame.
func (v *video) render() {
	for i := range v.colors {
		v.colors[i] = binary.BigEndian.Uint32(v.pal[i*4:]) >> 8
	}
	clear(v.sprPri)
	v.drawSprites()

	r2, r6, r7 := v.reg(2), v.reg(6), v.reg(7)
	v.layers = v.layers[:0]
	for l := 0; l < 4; l++ {
		bits := r7 >> (12 - 4*l)
		if bits&8 == 0 || devMask&(1<<l) == 0 {
			continue
		}
		bank := int(r6>>(24-8*l)) & 0xff
		info := v.sprLong(bank*0x800 + 0x7f0 + 4*l)
		v.layers = append(v.layers, layerInfo{
			pri:    int(info>>24) & 15,
			info:   info,
			scroll: v.sprLong(bank*0x800 + 0x3f0 + 4*l),
			deep:   bits&4 != 0,
		})
	}
	shadeBank := int(r7>>24) * 0x800
	shadePri := int(r2 & 15)

	for y := 0; y < ScreenH; y++ {
		line := v.line[:]
		clear(line)
		row := y * ScreenW
		sc := v.sprCol[row : row+ScreenW]
		sp := v.sprPri[row : row+ScreenW]
		sa := v.sprA[row : row+ScreenW]
		for pri := 0; pri < 16; pri++ {
			for x, p := range sp {
				if int(p) == pri+1 {
					if a := int(sa[x]); a < 256 {
						line[x] = blend(line[x], sc[x], a)
					} else {
						line[x] = sc[x]
					}
				}
			}
			for i := range v.layers {
				if v.layers[i].pri == pri {
					v.drawLayerLine(&v.layers[i], y, line)
				}
			}
			if pri == shadePri {
				s := v.sprLong(shadeBank + 0x400 + y*4)
				if a := int(s & 0xff); a != 0 {
					shade(line, s>>8, min(a, 0x80))
				}
			}
		}
		copy(v.screen[row:], line)
	}
}

// shade blends a line towards colour c by a/0x80.
func shade(line []uint32, c uint32, a int) {
	cr, cg, cb := int(c>>16&0xff)*a, int(c>>8&0xff)*a, int(c&0xff)*a
	k := 0x80 - a
	for x, p := range line {
		r := (int(p>>16&0xff)*k + cr) >> 7
		g := (int(p>>8&0xff)*k + cg) >> 7
		b := (int(p&0xff)*k + cb) >> 7
		line[x] = uint32(r<<16 | g<<8 | b)
	}
}

// blend mixes colour c over p with weight a/256.
func blend(p, c uint32, a int) uint32 {
	k := 0x100 - a
	r := (int(p>>16&0xff)*k + int(c>>16&0xff)*a) >> 8
	g := (int(p>>8&0xff)*k + int(c>>8&0xff)*a) >> 8
	b := (int(p&0xff)*k + int(c&0xff)*a) >> 8
	return uint32(r<<16 | g<<8 | b)
}

// alpha6 widens a 6-bit opacity to 8 bits.
func alpha6(a int) int { return a<<2 | a>>4 }

// penAlpha is a pixel's opacity (0-256) under blending mode m: bit 7 set
// fades the top 64 pens of 8-bit graphics out (and lets the others through
// at 255/256); otherwise bits 5-0 are a transparency, 0 for none.
func penAlpha(m uint8, p int, deep bool) int {
	if m&0x80 != 0 {
		if deep && p >= 0xc0 {
			return alpha6(0xff - p)
		}
		return alpha6(0x3f)
	}
	if t := int(m & 0x3f); t != 0 {
		return alpha6(0x3f - t)
	}
	return 256
}

// drawLayerLine draws one line of a 512x256 tilemap layer.
func (v *video) drawLayerLine(l *layerInfo, y int, line []uint32) {
	mapBase := int(l.info&0xff) * 0x800
	sx := int(l.scroll & 0x1ff)
	sy := int(l.scroll>>16) & 0xff
	mode := uint8(l.info >> 8)
	deep := l.deep
	my := (y - sy) & 0xff
	ty := my & 15
	for x := 0; x < ScreenW; {
		mx := (x - sx) & 0x1ff
		e := v.sprLong(mapBase + ((my>>4)*32+(mx>>4))*4)
		n := min(16-mx&15, ScreenW-x)
		if d := v.tileData(e&0x7ffff, deep); d != nil {
			col := int(e>>24) * 16
			for i := 0; i < n; i++ {
				p := pen(d, (mx+i)&15, ty, deep)
				if p == 0 {
					continue
				}
				c := v.colors[(col+p)%len(v.colors)]
				if a := penAlpha(mode, p, deep); a < 256 {
					c = blend(line[x+i], c, a)
				}
				line[x+i] = c
			}
		}
		x += n
	}
}

var devMask = -1

// drawSprites draws the listed sprites into the sprite buffer.
func (v *video) drawSprites() {
	r2 := v.reg(2)
	for i := 0; i < 0x400; i++ {
		e := int(binary.BigEndian.Uint16(v.sprBuf[0x3800+i*2:]))
		if n := e & 0x3ff; n*16 < 0x3800 {
			class := int(v.sprBuf[n*16+6]>>4) & 3
			v.drawSprite(n*16, uint8(r2>>(28-4*class))&15+1)
		}
		if e&0x4000 != 0 {
			break
		}
	}
}

// zoomed: the size in pixels of n tiles at zoom z (0x3f = 1:1).
func zoomed(n, z int) int { return n * 16 * (z + 1) >> 6 }

func (v *video) drawSprite(a int, pri uint8) {
	b := v.sprBuf[a : a+12]
	l0, l1, l2 := binary.BigEndian.Uint32(b), binary.BigEndian.Uint32(b[4:]), binary.BigEndian.Uint32(b[8:])
	y := int(int32(l0<<6) >> 22)
	x := int(int32(l0<<22) >> 22)
	h := int(l1>>24)&15 + 1
	w := int(l1>>8)&15 + 1
	zy, zx := int(l1>>16)&0xff, int(l1)&0xff
	flipY, flipX := l1&0x80000000 != 0, l1&0x8000 != 0
	deep := l2&0x800000 != 0
	tile := l2 & 0x7ffff
	sel := int(l2>>20) & 7
	mode := v.regs[sel&3+sel>>2*4]
	col := int(l2>>24) * 16
	dw, dh := zoomed(w, zx), zoomed(h, zy)
	if dw == 0 || dh == 0 {
		return
	}
	// source pixels per screen pixel, 16.16
	stepX := (w * 16 << 16) / dw
	stepY := (h * 16 << 16) / dh
	for dy := 0; dy < dh; dy++ {
		py := y + dy
		if py < 0 || py >= ScreenH {
			continue
		}
		srcY := dy * stepY >> 16
		if flipY {
			srcY = h*16 - 1 - srcY
		}
		row := py * ScreenW
		for dx := 0; dx < dw; dx++ {
			px := x + dx
			if px < 0 || px >= ScreenW {
				continue
			}
			srcX := dx * stepX >> 16
			if flipX {
				srcX = w*16 - 1 - srcX
			}
			d := v.tileData(tile+uint32((srcY>>4)*w+srcX>>4), deep)
			if d == nil {
				continue
			}
			p := pen(d, srcX&15, srcY&15, deep)
			if p == 0 {
				continue
			}
			v.sprCol[row+px] = v.colors[(col+p)%len(v.colors)]
			v.sprPri[row+px] = pri
			v.sprA[row+px] = uint16(penAlpha(mode, p, deep))
		}
	}
}
