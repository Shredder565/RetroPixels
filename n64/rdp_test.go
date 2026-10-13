package n64

// RDP unit tests: texture loading and decoding, z encoding, edge walking,
// copy-mode rectangles, Full Sync and save states. Each test builds a bare
// machine and feeds commands straight to the decoder.

import (
	"bytes"
	"testing"
)

// newTestRDP returns an RDP with 8 MB of RDRAM, a 64x64 16-bit colour
// image at 0x100000 and a scissor box covering all of it.
func newTestRDP() *RDP {
	m := &Machine{rdram: make([]byte, rdramSize)}
	d := &m.RDP
	d.m = m
	d.reset()
	d.command([]uint64{0x3f<<56 | 2<<51 | 63<<32 | 0x100000})
	d.command([]uint64{0x2d<<56 | 0<<44 | 0<<32 | 256<<12 | 256})
	return d
}

func put16(b []byte, a uint32, v uint16) { b[a], b[a+1] = byte(v>>8), byte(v) }
func get16(b []byte, a uint32) uint16    { return uint16(b[a])<<8 | uint16(b[a+1]) }

// pix reads pixel x, y of the 64-wide 16-bit test image.
func (d *RDP) pix(x, y int) uint16 { return get16(d.m.rdram, 0x100000+uint32(y*64+x)*2) }

// setTile sends Set Tile for tile 0 (no clamp, mask or shift).
func (d *RDP) setTile(fmt, size, line, tmem uint32) {
	d.command([]uint64{0x35<<56 | uint64(fmt)<<53 | uint64(size)<<51 | uint64(line)<<41 | uint64(tmem)<<32})
}

// loadTex copies an image of w x h texels into RDRAM at 0x200000 and loads
// it into TMEM through tile 7 with Load Tile, then sets up tile 0 to read it.
func (d *RDP) loadTex(fmt, size uint32, w, h uint32, img []byte) {
	copy(d.m.rdram[0x200000:], img)
	d.command([]uint64{0x3d<<56 | uint64(fmt)<<53 | uint64(size)<<51 | uint64(w-1)<<32 | 0x200000})
	bits := uint32(4) << size
	line := (w*bits/8 + 7) / 8
	if size == 3 {
		line /= 2 // RGBA32 is split across both halves of TMEM
	}
	d.command([]uint64{0x35<<56 | uint64(fmt)<<53 | uint64(size)<<51 | uint64(line)<<41 | 7<<24})
	d.command([]uint64{0x34<<56 | 7<<24 | uint64(w-1)<<14 | uint64(h-1)<<2})
	d.setTile(fmt, size, line, 0)
	d.command([]uint64{0x32<<56 | uint64(w-1)<<14 | uint64(h-1)<<2})
}

func (d *RDP) texelOf(s, t int32) [4]int32 {
	var x rdpTileX
	d.Tiles[0].prepare(&x, false)
	var out [4]int32
	d.texelAt(&x, s, t, false, &out)
	return out
}

func TestRDPTexelDecode(t *testing.T) {
	// RGBA16: 4x2, odd rows are word-swapped in TMEM
	d := newTestRDP()
	img := make([]byte, 16)
	for i := 0; i < 8; i++ {
		put16(img, uint32(i*2), uint16(i)<<11|uint16(31-i)<<1|1)
	}
	d.loadTex(0, 2, 4, 2, img)
	for i := int32(0); i < 8; i++ {
		got := d.texelOf(i&3, i>>2)
		r := i<<3 | i>>2
		b := (31-i)<<3 | (31-i)>>2
		if got != [4]int32{r, 0, b, 255} {
			t.Errorf("rgba16 texel %d = %v", i, got)
		}
	}

	// RGBA32: red/green in the low half, blue/alpha in the high half
	d = newTestRDP()
	img = make([]byte, 32)
	for i := range img {
		img[i] = byte(i * 7)
	}
	d.loadTex(0, 3, 4, 2, img)
	for i := int32(0); i < 8; i++ {
		got := d.texelOf(i&3, i>>2)
		want := [4]int32{i * 28, i*28 + 7, i*28 + 14, i*28 + 21}
		if got != want {
			t.Errorf("rgba32 texel %d = %v, want %v", i, got, want)
		}
	}

	// IA8, I4, IA4, IA16
	d = newTestRDP()
	d.loadTex(3, 1, 8, 1, []byte{0x3c, 0xf0, 0, 0, 0, 0, 0, 0})
	if got := d.texelOf(0, 0); got != [4]int32{51, 51, 51, 204} {
		t.Errorf("ia8 = %v", got)
	}
	d = newTestRDP()
	d.loadTex(4, 0, 16, 1, []byte{0x5a, 0, 0, 0, 0, 0, 0, 0})
	if got := d.texelOf(0, 0); got != [4]int32{85, 85, 85, 85} {
		t.Errorf("i4 even = %v", got)
	}
	if got := d.texelOf(1, 0); got != [4]int32{170, 170, 170, 170} {
		t.Errorf("i4 odd = %v", got)
	}
	d = newTestRDP()
	d.loadTex(3, 0, 16, 1, []byte{0xf0, 0, 0, 0, 0, 0, 0, 0})
	if got := d.texelOf(0, 0); got != [4]int32{255, 255, 255, 255} {
		t.Errorf("ia4 = %v", got)
	}
	if got := d.texelOf(1, 0); got != [4]int32{0, 0, 0, 0} {
		t.Errorf("ia4 odd = %v", got)
	}
	d = newTestRDP()
	d.loadTex(3, 2, 4, 1, []byte{0x80, 0x40, 0, 0, 0, 0, 0, 0})
	if got := d.texelOf(0, 0); got != [4]int32{128, 128, 128, 64} {
		t.Errorf("ia16 = %v", got)
	}
}

func TestRDPTLUT(t *testing.T) {
	// a CI4 texture looked up through a 16-entry RGBA16 palette
	d := newTestRDP()
	pal := make([]byte, 32)
	for i := 0; i < 16; i++ {
		put16(pal, uint32(i*2), uint16(i)<<6|1) // green = index
	}
	copy(d.m.rdram[0x300000:], pal)
	d.command([]uint64{0x3d<<56 | 2<<51 | 0x300000})
	d.command([]uint64{0x35<<56 | 256<<32 | 7<<24})
	d.command([]uint64{0x30<<56 | 7<<24 | 15<<14})
	d.loadTex(2, 0, 16, 1, []byte{0x1e, 0, 0, 0, 0, 0, 0, 0})
	var x rdpTileX
	d.Tiles[0].prepare(&x, true)
	var out [4]int32
	d.texelAt(&x, 1, 0, false, &out)
	if want := [4]int32{0, 14<<3 | 14>>2, 0, 255}; out != want {
		t.Errorf("tlut texel = %v, want %v", out, want)
	}
}

func TestRDPTexWrap(t *testing.T) {
	// mask 2 (4 texels) with mirroring: 0 1 2 3 3 2 1 0 0 1 ...
	want := []int32{0, 1, 2, 3, 3, 2, 1, 0, 0, 1}
	for i, w := range want {
		if got := texWrap(int32(i), 3, 1, 2); got != w {
			t.Errorf("mirror wrap %d = %d, want %d", i, got, w)
		}
	}
	if got := texWrap(-1, 3, 0, 2); got != 3 {
		t.Errorf("wrap -1 = %d", got)
	}
	// shift right 1, then left by 1 (shift 15)
	if got := texShift(64, 1, 0); got != 32 {
		t.Errorf("shift 1 = %d", got)
	}
	if got := texShift(64, 15, 0); got != 128 {
		t.Errorf("shift 15 = %d", got)
	}
}

func TestRDPZFormat(t *testing.T) {
	prev := int32(-1)
	for v := uint32(0); v < 0x10000; v += 4 {
		z := zDecode(v)
		if z <= prev {
			t.Fatalf("z decode not increasing at %#x", v)
		}
		prev = z
		if e := zEncode(z); e != v {
			t.Fatalf("z round trip %#x -> %#x -> %#x", v, z, e)
		}
	}
	if zDecode(0xfffc) >= 1<<18 {
		t.Errorf("z max out of 18 bits")
	}
}

// TestRDPEdgeWalk draws a triangle with its H edge on the left: the H edge
// is vertical at x=10 and the M edge leaves x=10 with slope 1, from y=10 to 30.
func TestRDPEdgeWalk(t *testing.T) {
	for _, cyc := range []uint64{3, 0} {
		d := newTestRDP()
		d.command([]uint64{0x2f<<56 | cyc<<52 | 3<<38 | 3<<36})
		d.command([]uint64{0x37<<56 | 0xf801f801})
		d.command([]uint64{0x3a<<56 | 0xff0000ff})
		// 1-cycle combiner: colour = PRIM, alpha = PRIM
		d.command([]uint64{0x3c<<56 | 15<<52 | 31<<47 | 7<<44 | 7<<41 | 15<<37 | 31<<32 |
			15<<28 | 15<<24 | 7<<21 | 7<<18 | 3<<15 | 7<<12 | 3<<9 | 3<<6 | 7<<3 | 3})
		yh, yl := uint64(10*4), uint64(30*4)
		d.command([]uint64{
			0x08<<56 | 1<<55 | yl<<32 | yl<<16 | yh,
			10 << 48,
			10 << 48,
			10<<48 | 1<<16,
		})
		for y := 0; y < 40; y++ {
			for x := 0; x < 64; x++ {
				var in bool
				if cyc == 3 {
					// fill mode draws every pixel touched by the edges
					in = y >= 10 && y < 30 && x >= 10 && x <= y
				} else {
					// otherwise a pixel needs its top left sample covered
					in = y >= 10 && y < 30 && x >= 10 && x < y
				}
				if got := d.pix(x, y) == 0xf801; got != in {
					t.Errorf("cycle %d pixel %d,%d drawn=%v", cyc, x, y, got)
				}
			}
		}
	}
}

func TestRDPFillRect(t *testing.T) {
	d := newTestRDP()
	d.command([]uint64{0x2f<<56 | 3<<52})
	d.command([]uint64{0x37<<56 | 0x12345678})
	d.command([]uint64{0x36<<56 | 20<<46 | 12<<34 | 4<<14 | 2<<2})
	for y := 0; y < 16; y++ {
		for x := 0; x < 24; x++ {
			want := uint16(0)
			if x >= 4 && x <= 20 && y >= 2 && y <= 12 {
				want = 0x1234
				if x&1 != 0 {
					want = 0x5678
				}
			}
			if got := d.pix(x, y); got != want {
				t.Fatalf("fill pixel %d,%d = %#x, want %#x", x, y, got, want)
			}
		}
	}
}

func TestRDPCopyRect(t *testing.T) {
	// copy a 4x4 RGBA16 texture to 8,8 (copy mode steps 4 texels per pixel
	// in S, so dsdx is 4.0 in 5.10)
	d := newTestRDP()
	img := make([]byte, 32)
	for i := 0; i < 16; i++ {
		put16(img, uint32(i*2), uint16(i+1)<<1|1)
	}
	d.loadTex(0, 2, 4, 4, img)
	d.command([]uint64{0x2f<<56 | 2<<52})
	d.command([]uint64{0x24<<56 | 11<<46 | 11<<34 | 8<<14 | 8<<2, 4<<26 | 1<<10})
	for y := 0; y < 4; y++ {
		for x := 0; x < 4; x++ {
			if got, want := d.pix(8+x, 8+y), uint16(y*4+x+1)<<1|1; got != want {
				t.Errorf("copy pixel %d,%d = %#x, want %#x", x, y, got, want)
			}
		}
	}
	if d.pix(12, 8) != 0 || d.pix(8, 12) != 0 {
		t.Errorf("copy rect drew outside its box")
	}
}

func TestRDPFullSync(t *testing.T) {
	d := newTestRDP()
	d.command([]uint64{0x29 << 56})
	if d.m.miIntr&intDP == 0 {
		t.Errorf("full sync did not raise the DP interrupt")
	}
}

func TestRDPState(t *testing.T) {
	d := newTestRDP()
	d.loadTex(0, 2, 4, 4, bytes.Repeat([]byte{0xab, 0xcd}, 16))
	d.command([]uint64{0x3a<<56 | 3<<40 | 0x80<<32 | 0x11223344})
	d.Cmd[0], d.CmdN = 0x0f<<56, 3 // a triangle waiting for its words
	b := d.marshalState()

	e := newTestRDP()
	if err := e.unmarshalState(b); err != nil {
		t.Fatal(err)
	}
	if e.rdpRegs != d.rdpRegs {
		t.Errorf("state round trip lost data")
	}
	if !bytes.Equal(e.marshalState(), b) {
		t.Errorf("state not stable")
	}
	if e.unmarshalState(b[1:]) == nil {
		t.Errorf("short state accepted")
	}
}
