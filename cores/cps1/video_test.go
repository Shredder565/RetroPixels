package cps1

import "testing"

// Synthetic graphics tests: a made-up graphics ROM where every 16-pixel ROM
// row g has pen (g + x) % 15 at pixel x, so any tile's pixels are known
// (rows 0-63, tile 0 of every size, are transparent).

var testCfg = &Config{
	CPSB:   cpsB05,
	Mapper: []GfxRange{{KindSprites | KindScroll1 | KindScroll2 | KindScroll3, 0, 0x7fff, 0}},
}

const testRows = 0x8000 // ROM rows of 16 pixels (256 KB)

func testPen(row, x int) uint8 {
	if row < 64 {
		return 15
	}
	return uint8((row + x) % 15)
}

// encodeGfx makes the ROM bytes for pens (the inverse of decodeGfx).
func encodeGfx(pen func(row, x int) uint8, rows int) []byte {
	rom := make([]byte, rows*8)
	for g := 0; g < rows; g++ {
		for x := 0; x < 16; x++ {
			p := pen(g, x)
			for plane := 0; plane < 4; plane++ {
				if p>>plane&1 != 0 {
					rom[g*8+(x/8)*4+plane] |= 0x80 >> (x % 8)
				}
			}
		}
	}
	return rom
}

func newTestVideo() *video {
	v := newVideo(testCfg, encodeGfx(testPen, testRows))
	// palette at 0x914000: colour n = (n & 0xfff) with full brightness
	for n := 0; n < 0xc00; n++ {
		w := 0xf000 | n&0xfff
		v.gfxram[0x14000+n*2], v.gfxram[0x14000+n*2+1] = uint8(w>>8), uint8(w)
	}
	v.cpsa[regPalBase] = 0x9140
	v.cpsb[cpsB05.PalCtrl/2] = 0x3f
	v.copyPalette()
	v.cpsa[regObjBase] = 0x9000
	v.cpsa[regS1Base], v.cpsa[regS2Base], v.cpsa[regS3Base] = 0x9080, 0x90c0, 0x9100
	// end the sprite list at once
	v.gfxram[6] = 0xff
	return v
}

func (v *video) put16(a, w int) { v.gfxram[a], v.gfxram[a+1] = uint8(w>>8), uint8(w) }

// layers sets the layer order (bottom to top) and turns scroll 1-3 on.
func (v *video) layers(l0, l1, l2, l3 int) {
	v.cpsb[cpsB05.LayerCtrl/2] = uint16(l0<<6|l1<<8|l2<<10|l3<<12) | 0x02 | 0x08 | 0x20
}

func TestColour(t *testing.T) {
	for _, c := range []struct {
		in   uint16
		want uint32
	}{{0xffff, 0xffffff}, {0x0f00, 0x550000}, {0xf00f, 0x0000ff}, {0x8888, 0x5d5d5d}, {0x0000, 0}} {
		if got := colour(c.in); got != c.want {
			t.Errorf("colour(%#04x) = %#06x, want %#06x", c.in, got, c.want)
		}
	}
}

func TestDecodeGfx(t *testing.T) {
	pens := decodeGfx(encodeGfx(testPen, 4))
	for g := 0; g < 4; g++ {
		for x := 0; x < 16; x++ {
			if pens[g*16+x] != testPen(g, x) {
				t.Fatalf("row %d pixel %d = %d", g, x, pens[g*16+x])
			}
		}
	}
	// pixel 0 is bit 7 of the left half's bytes; pen bit n is byte n
	p := decodeGfx([]byte{0x80, 0, 0, 0x80, 0, 0, 0x01, 0})
	if p[0] != 9 || p[15] != 4 {
		t.Errorf("pens %v", p[:16])
	}
}

func TestScroll2Tile(t *testing.T) {
	v := newTestVideo()
	v.layers(2, 0, 1, 3)
	v.cpsb[cpsB05.LayerCtrl/2] &^= 0x02 | 0x20 // only scroll 2
	// tile 5 (ROM rows 80-95) at column 4, row 1, palette 2, flipped in x
	idx := 1 | 4<<4
	v.put16(0x0c000+idx*4, 5)
	v.put16(0x0c000+idx*4+2, 2|0x20)
	v.render()
	for y := 0; y < 16; y++ {
		for x := 0; x < 16; x++ {
			pen := int(testPen(5*16+y, 15-x))
			want := v.pal[0x400+2*16+pen]
			if got := v.screen[y*ScreenW+x]; got != want {
				t.Fatalf("pixel %d,%d = %#06x, want %#06x", x, y, got, want)
			}
		}
	}
	// scrolled 3 pixels right and 2 down
	v.cpsa[regS2X], v.cpsa[regS2Y] = 0xfffd, 0xfffe
	v.render()
	if got, want := v.screen[2*ScreenW+3], v.pal[0x400+2*16+int(testPen(5*16, 15))]; got != want {
		t.Errorf("scrolled tile at %#06x, want %#06x", got, want)
	}
	// row scroll: line 16 (the first shown) moved by its own entry
	v.cpsa[regS2X], v.cpsa[regS2Y] = 0, 0
	v.cpsa[regRowBase] = 0x9200 // 0x20000 in graphics RAM
	v.cpsa[regRowOffs] = 0
	v.cpsa[regVideoCtl] = 1
	v.put16(0x20000+16*2, 0xfffb) // -5
	v.render()
	if got, want := v.screen[5], v.pal[0x400+2*16+int(testPen(5*16, 15))]; got != want {
		t.Errorf("row-scrolled line at %#06x, want %#06x", got, want)
	}
	if got, want := v.screen[ScreenW+5], v.pal[0x400+2*16+int(testPen(5*16+1, 10))]; got != want {
		t.Errorf("the next line moved too")
	}
}

func TestScroll1And3Tiles(t *testing.T) {
	v := newTestVideo()
	v.layers(3, 0, 2, 1)
	v.cpsb[cpsB05.LayerCtrl/2] &^= 0x08
	// scroll 3: tile 2 (ROM rows 128-191, two a line) at column 2 (x 64 = screen 0), row 0
	v.put16(0x10000+(2<<3)*4, 2)
	v.put16(0x10000+(2<<3)*4+2, 7)
	// scroll 1: tile 9 (rows 72-79) at column 9 (screen x 8), row 2 (line 16 = screen 0)
	// odd columns use the right half of the 16-pixel rows
	i1 := 2 | 9<<5
	v.put16(0x08000+i1*4, 9)
	v.put16(0x08000+i1*4+2, 1)
	v.render()
	for x := 0; x < 32; x++ {
		var want uint32
		if x >= 8 && x < 16 {
			want = v.pal[0x200+16+int(testPen(9*8, 8+x-8))]
		} else {
			// screen line 0 is map line 16: ROM row 2*8 + 16*2, half x/16
			want = v.pal[0x600+7*16+int(testPen(2*64+16*2+x/16, x%16))]
		}
		if p := testPen(9*8, 8+x-8); x >= 8 && x < 16 && p == 15 {
			want = v.pal[0x600+7*16+int(testPen(2*64+16*2+x/16, x%16))]
		}
		if got := v.screen[x]; got != want {
			t.Fatalf("pixel %d = %#06x, want %#06x", x, got, want)
		}
	}
}

func TestSpritesAndPriority(t *testing.T) {
	v := newTestVideo()
	// scroll 3, then sprites, then scroll 1 on top; scroll 2 off
	v.layers(3, 0, 1, 2)
	v.cpsb[cpsB05.LayerCtrl/2] &^= 0x08
	// a scroll 3 tile in group 1 under the sprite (tile 2, palette 3)
	v.put16(0x10000+(2<<3)*4, 2)
	v.put16(0x10000+(2<<3)*4+2, 3|1<<7)
	// sprite: tile 0x10 2 wide, flipped in x, palette 4, at screen 0,0
	v.put16(0, hx0)
	v.put16(2, hy0)
	v.put16(4, 0x10)
	v.put16(6, 4|0x20|1<<8)
	v.put16(8+6, 0xff00)
	v.latchSprites()
	v.render()
	// flipped: the left 16 pixels are tile 0x11 mirrored
	for x := 0; x < 32; x++ {
		code := 0x11
		if x >= 16 {
			code = 0x10
		}
		p := testPen(code*16, 15-x%16)
		want := v.pal[4*16+int(p)]
		if p == 15 {
			want = v.pal[0x600+3*16+int(testPen(2*64+16*2+x/16, x%16))]
		}
		if got := v.screen[x]; got != want {
			t.Fatalf("pixel %d = %#06x, want %#06x", x, got, want)
		}
	}
	// group 1's mask puts scroll 3's pen 5 in front of the sprites
	v.cpsb[cpsB05.Prio[1]/2] = 1 << 5
	v.render()
	n := 0
	for x := 0; x < 32; x++ {
		p3 := testPen(2*64+16*2+x/16, x%16)
		ps := testPen((0x11-x/16)*16, 15-x%16)
		got := v.screen[x]
		switch {
		case p3 == 5:
			n++
			if got != v.pal[0x600+3*16+5] {
				t.Errorf("pixel %d: scroll 3 pen 5 isn't in front", x)
			}
		case ps != 15:
			if got != v.pal[4*16+int(ps)] {
				t.Errorf("pixel %d: sprite hidden", x)
			}
		}
	}
	if n == 0 {
		t.Fatal("no pen 5 in the test row")
	}
	// earlier sprites are in front of later ones
	v.put16(8, hx0)
	v.put16(10, hy0)
	v.put16(12, 0x20)
	v.put16(14, 5)
	v.put16(16+6, 0xff00)
	v.latchSprites()
	v.cpsb[cpsB05.Prio[1]/2] = 0
	v.render()
	if p := testPen(0x11*16, 15); p != 15 && v.screen[0] != v.pal[4*16+int(p)] {
		t.Error("the first sprite isn't in front")
	}
}

func TestFlipScreen(t *testing.T) {
	v := newTestVideo()
	v.layers(2, 0, 1, 3)
	idx := 1 | 4<<4
	v.put16(0x0c000+idx*4, 5)
	v.put16(0x0c000+idx*4+2, 2)
	v.render()
	a := v.screen[0]
	v.cpsa[regVideoCtl] = 0x8000
	v.render()
	if v.screen[ScreenW*ScreenH-1] != a {
		t.Error("flip screen didn't turn the picture round")
	}
}

func TestMapperRanges(t *testing.T) {
	v := newVideo(configs["1941"], make([]byte, 0x200000))
	if v.mapTile(KindSprites, 0x1fff) != 0x1fff || v.mapTile(KindSprites, 0x2000) >= 0 {
		t.Error("sprite range")
	}
	if v.mapTile(KindScroll1, 0x4020) != 0x4020 || v.mapTile(KindScroll2, 0x4020) >= 0 {
		t.Error("scroll 1 range")
	}
	if v.mapTile(KindScroll2, 0x2420<<1) != 0x4840 || v.mapTile(KindScroll3, 0x420<<3) != 0x2100 {
		t.Error("scroll 2/3 ranges")
	}
}
