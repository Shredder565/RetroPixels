package nds

import (
	"os"
	"testing"
)

// These tests drive the 3D engine through its I/O ports, the way a game
// does, and look at the picture it draws.

func new3DMachine(t *testing.T) *Machine {
	rom, err := os.ReadFile("testdata/selftest.nds")
	if err != nil {
		t.Skip(err)
	}
	m, err := New(rom, Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return m
}

type g3 struct{ m *Machine }

func (g g3) cmd(c uint32, p ...uint32) {
	if len(p) == 0 {
		g.m.bus9.Write32(0x04000400+c*4, 0)
	}
	for _, v := range p {
		g.m.bus9.Write32(0x04000400+c*4, v)
	}
}

func fx(f float64) uint32 { return uint32(int32(f * 4096)) }

func vtx(x, y, z float64) (uint32, uint32) {
	return uint32(uint16(int16(x*4096))) | uint32(uint16(int16(y*4096)))<<16, uint32(uint16(int16(z * 4096)))
}

func rgb15(r, g, b uint32) uint32 { return r | g<<5 | b<<10 }

func (g g3) frame() {
	g.cmd(0x50, 0) // swap buffers
	g.m.gpu3d.vblank()
	g.m.gpu3d.renderFrame()
}

func (g g3) px(x, y int) (uint32, uint32) {
	v := g.m.gpu3d.out[y*256+x]
	return v & 0x7fff, v >> 16 & 31
}

func (g g3) setup() {
	g.m.bus9.Write32(0x04000060, 1|8)                   // textures, blending
	g.m.bus9.Write32(0x04000350, rgb15(0, 0, 8)|31<<16) // clear: dark blue, opaque
	g.m.bus9.Write32(0x04000354, 0x7fff)
	g.cmd(0x60, 0|0<<8|255<<16|191<<24) // viewport
	g.cmd(0x10, 0)
	g.cmd(0x15)
	g.cmd(0x10, 2)
	g.cmd(0x15)
}

func (g g3) tri(col uint32, alpha uint32, id uint32, z float64, pts [3][2]float64) {
	g.cmd(0x29, 3<<6|alpha<<16|id<<24) // both sides
	g.cmd(0x2a, 0)
	g.cmd(0x40, 0)
	g.cmd(0x20, col)
	for _, p := range pts {
		a, b := vtx(p[0], p[1], z)
		g.cmd(0x23, a, b)
	}
	g.cmd(0x41)
}

func TestGPU3DTriangles(t *testing.T) {
	g := g3{new3DMachine(t)}
	g.setup()
	// a red triangle behind, a green one in front on the right half
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-0.9, -0.9}, {0.9, -0.9}, {0, 0.9}})
	g.tri(rgb15(0, 31, 0), 31, 2, 0.1, [3][2]float64{{0, -0.9}, {0.9, -0.9}, {0.9, 0.9}})
	g.frame()
	if c, a := g.px(2, 2); c != rgb15(0, 0, 8) || a != 31 {
		t.Errorf("corner: colour %04x alpha %d, want the clear colour", c, a)
	}
	if c, _ := g.px(100, 150); c != rgb15(31, 0, 0) {
		t.Errorf("left middle: %04x, want red", c)
	}
	if c, _ := g.px(200, 150); c != rgb15(0, 31, 0) {
		t.Errorf("right: %04x, want green (in front)", c)
	}
	// translucent blue over everything at alpha 16
	g.setup()
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-1, -1}, {1, -1}, {-1, 1}})
	g.tri(rgb15(0, 0, 31), 16, 3, 0, [3][2]float64{{-1, -1}, {1, -1}, {-1, 1}})
	g.frame()
	c, a := g.px(40, 150)
	if c&31 == 0 || c>>10&31 == 0 || a != 31 {
		t.Errorf("translucent: %04x alpha %d, want red and blue mixed", c, a)
	}
	// the clip matrix reads back (identity projection × scale 2)
	g.cmd(0x10, 1)
	g.cmd(0x1b, fx(2), fx(2), fx(2))
	if v := g.m.bus9.Read32(0x04000640); v != fx(2) {
		t.Errorf("clip matrix [0] = %x, want %x", v, fx(2))
	}
	if v := g.m.bus9.Read32(0x04000680); v != fx(1) {
		t.Errorf("vector matrix [0] = %x, want %x (scale leaves it)", v, fx(1))
	}
}

func TestGPU3DTexture(t *testing.T) {
	g := g3{new3DMachine(t)}
	m := g.m
	m.bus9.Write8(0x04000240, 0x80) // VRAM A to the LCD to write the texture
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			c := uint32(rgb15(31, 31, 0))
			if (x^y)&4 != 0 {
				c = rgb15(0, 0, 31)
			}
			m.bus9.Write16(0x06800000+uint32(y*8+x)*2, c|0x8000)
		}
	}
	m.bus9.Write8(0x04000240, 0x83) // VRAM A: texture slot 0
	g.setup()
	g.cmd(0x29, 3<<6|31<<16|1<<24)
	g.cmd(0x2a, 0|7<<26) // 8×8 direct colour at 0
	g.cmd(0x40, 1)       // quad
	g.cmd(0x20, rgb15(31, 31, 31))
	pts := [4][4]float64{{-1, -1, 0, 8}, {1, -1, 8, 8}, {1, 1, 8, 0}, {-1, 1, 0, 0}}
	for _, p := range pts {
		g.cmd(0x22, uint32(uint16(int16(p[2]*16)))|uint32(uint16(int16(p[3]*16)))<<16)
		a, b := vtx(p[0], p[1], 0)
		g.cmd(0x23, a, b)
	}
	g.frame()
	// texel (0,0) is yellow at the top left; (4,0) blue
	if c, _ := g.px(5, 5); c != rgb15(31, 31, 0) {
		t.Errorf("texel 0,0: %04x, want yellow", c)
	}
	if c, _ := g.px(128+5, 5); c != rgb15(0, 0, 31) {
		t.Errorf("texel 4,0: %04x, want blue", c)
	}
}

func TestGPU3DState(t *testing.T) {
	g := g3{new3DMachine(t)}
	g.setup()
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-0.9, -0.9}, {0.9, -0.9}, {0, 0.9}})
	g.frame()
	b := g.m.gpu3d.saveState()
	want := g.m.gpu3d.out
	g.m.gpu3d.out = [256 * 192]uint32{}
	if err := g.m.gpu3d.loadState(b); err != nil {
		t.Fatal(err)
	}
	if g.m.gpu3d.out != want {
		t.Error("the picture differs after loading the state")
	}
}

func TestDisplayCapture(t *testing.T) {
	g := g3{new3DMachine(t)}
	m := g.m
	g.setup()
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-1, -1}, {1, -1}, {-1, 1}})
	g.frame()
	m.bus9.Write8(0x04000241, 0x80)              // VRAM B to the LCD
	m.dispCapCnt = 1<<31 | 1<<16 | 3<<20 | 1<<24 // 3D only, 256×192, into B
	for y := 0; y < 192; y++ {
		m.captureLine(y)
	}
	at := func(x, y int) uint16 {
		return uint16(m.vram.banks[1][(y*256+x)*2]) | uint16(m.vram.banks[1][(y*256+x)*2+1])<<8
	}
	if v := at(10, 180); v != 0x8000|uint16(rgb15(31, 0, 0)) {
		t.Errorf("captured %04x at 10,180, want opaque red", v)
	}
	if v := at(250, 5); v != 0x8000|uint16(rgb15(0, 0, 8)) {
		t.Errorf("captured %04x at 250,5, want the clear colour", v)
	}
}
