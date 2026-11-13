package nds

import (
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

// The 8×8 direct-colour checkerboard of TestGPU3DTexture, on a quad over
// the whole screen.
func drawTexturedQuad(g g3) {
	m := g.m
	m.bus9.Write8(0x04000240, 0x80)
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			c := uint32(rgb15(31, 31, 0))
			if (x^y)&4 != 0 {
				c = rgb15(0, 0, 31)
			}
			m.bus9.Write16(0x06800000+uint32(y*8+x)*2, c|0x8000)
		}
	}
	m.bus9.Write8(0x04000240, 0x83)
	g.setup()
	g.cmd(0x29, 3<<6|31<<16|1<<24)
	g.cmd(0x2a, 0|7<<26)
	g.cmd(0x40, 1)
	g.cmd(0x20, rgb15(31, 31, 31))
	pts := [4][4]float64{{-1, -1, 0, 8}, {1, -1, 8, 8}, {1, 1, 8, 0}, {-1, 1, 0, 0}}
	for _, p := range pts {
		g.cmd(0x22, uint32(uint16(int16(p[2]*16)))|uint32(uint16(int16(p[3]*16)))<<16)
		a, b := vtx(p[0], p[1], 0)
		g.cmd(0x23, a, b)
	}
	g.frame()
}

func TestTexturePackReplacesAndDumps(t *testing.T) {
	g := g3{new3DMachine(t)}
	m := g.m
	dump := t.TempDir()
	if err := m.DumpTextures(dump); err != nil {
		t.Fatal(err)
	}
	drawTexturedQuad(g)
	if m.TexturesDumped() != 1 {
		t.Fatalf("%d textures dumped, want 1", m.TexturesDumped())
	}
	h := TextureHash(m.gpu3d.readTex(7<<26, 0))
	if _, err := os.Stat(filepath.Join(dump, HashName(h)+".png")); err != nil {
		t.Fatal("the dumped picture isn't named by the texture's hash:", err)
	}
	m.DumpTextures("")

	// a 32×32 picture for it: red on the left half, green on the right
	pack := t.TempDir()
	img := image.NewNRGBA(image.Rect(0, 0, 32, 32))
	for y := 0; y < 32; y++ {
		for x := 0; x < 32; x++ {
			c := color.NRGBA{255, 0, 0, 255}
			if x >= 16 {
				c = color.NRGBA{0, 255, 0, 255}
			}
			img.SetNRGBA(x, y, c)
		}
	}
	f, _ := os.Create(filepath.Join(pack, HashName(h)+"_board.png"))
	png.Encode(f, img)
	f.Close()
	if err := m.SetTexturePack(pack); err != nil || m.TexturePackSize() != 1 {
		t.Fatalf("pack: %v, %d pictures", err, m.TexturePackSize())
	}
	drawTexturedQuad(g)
	if c, _ := g.px(5, 5); c != rgb15(31, 0, 0) {
		t.Errorf("left: %04x, want the pack's red", c)
	}
	if c, _ := g.px(200, 100); c != rgb15(0, 31, 0) {
		t.Errorf("right: %04x, want the pack's green", c)
	}
	m.EnableTexturePack(false)
	drawTexturedQuad(g)
	if c, _ := g.px(5, 5); c != rgb15(31, 31, 0) {
		t.Errorf("pack off: %04x, want the game's yellow", c)
	}
}

func TestRenderScale(t *testing.T) {
	g := g3{new3DMachine(t)}
	g.setup()
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-0.9, -0.9}, {0.9, -0.9}, {0, 0.9}})
	g.frame()
	want := g.m.gpu3d.out
	g.m.SetRenderScale(3)
	g.setup()
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-0.9, -0.9}, {0.9, -0.9}, {0, 0.9}})
	g.frame()
	diff := 0
	for i := range want {
		if want[i] != g.m.gpu3d.out[i] {
			diff++
		}
	}
	if diff > 200 { // only along the edges
		t.Errorf("%d pixels differ at scale 3", diff)
	}
	if len(g.m.gpu3d.hi) != 768*576 || len(g.m.FrameHi()) != 768*1152 {
		t.Errorf("big picture sizes %d, %d", len(g.m.gpu3d.hi), len(g.m.FrameHi()))
	}
	if c := g.m.gpu3d.hi[300*768+384] & 0x7fff; c != rgb15(31, 0, 0) {
		t.Errorf("big picture middle: %04x, want red", c)
	}
}

// Games send GXFIFO words with STM, to 0x4000400, 0x4000404, ... (all the
// same FIFO).
func TestGXFIFOMirrors(t *testing.T) {
	g := g3{new3DMachine(t)}
	g.setup()
	g.cmd(0x10, 1)
	g.m.bus9.Write32(0x04000400, 0x1c) // MTX_TRANS, packed
	g.m.bus9.Write32(0x04000404, fx(1))
	g.m.bus9.Write32(0x04000408, fx(2))
	g.m.bus9.Write32(0x0400040c, fx(3))
	if v := g.m.bus9.Read32(0x04000640 + 13*4); v != fx(2) {
		t.Errorf("clip matrix y translation %x, want %x", v, fx(2))
	}
}

// VTX_DIFF moves by 1/4096 units per step (±0.125 in all).
func TestVertexDiff(t *testing.T) {
	g := g3{new3DMachine(t)}
	g.setup()
	g.cmd(0x40, 0)
	a, b := vtx(0.5, 0.5, 0)
	g.cmd(0x23, a, b)
	g.cmd(0x28, 511|uint32(0x3ff&-512)<<10|10<<20)
	if v := g.m.gpu3d.s.LastV; v != [3]int64{2048 + 511, 2048 - 512, 10} {
		t.Errorf("vertex %v after VTX_DIFF", v)
	}
}

// An opaque 3D pixel over a second target keeps its colour even when BLDCNT
// blends BG0 with EVA = EVB = 16 (the 3D layer blends by its own alpha).
func TestOpaque3DOverSecondTarget(t *testing.T) {
	g := g3{new3DMachine(t)}
	m := g.m
	g.setup()
	g.tri(rgb15(31, 0, 0), 31, 1, 0.5, [3][2]float64{{-1, -1}, {1, -1}, {-1, 1}})
	g.frame()
	m.bus9.Write16(0x05000000, rgb15(0, 31, 31)) // backdrop
	e := m.gpuA
	e.dispcnt = 1<<16 | 8 | 1<<8 // normal display, 3D, BG0
	e.bldcnt = 1 | 1<<6 | 0x20<<8
	e.bldalpha = 16 | 16<<8
	e.renderLine(180)
	if c := e.raw[10]; c != rgb15(31, 0, 0) {
		t.Errorf("3D over the backdrop: %04x, want red", c)
	}
}
