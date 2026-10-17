//go:build windows

package wiiu

import (
	"os"
	"path/filepath"
	"testing"
)

// TestD3DFrames runs test programs with the software renderer and with
// the Direct3D backend and compares the pictures (the programs come from
// TestEmitRPX: set WIIU_RPX to its directory).
func TestD3DFrames(t *testing.T) {
	dir := os.Getenv("WIIU_RPX")
	if dir == "" {
		t.Skip("WIIU_RPX not set")
	}
	dxDevice(t)
	for _, name := range []string{"gx2test", "demo"} {
		t.Run(name, func(t *testing.T) {
			rpx, err := os.ReadFile(filepath.Join(dir, name+".rpx"))
			if err != nil {
				t.Skip(err)
			}
			frames := 30
			soft, _ := runRPX(t, rpx, frames)
			m, err := New(rpx, "test.rpx", nil)
			if err != nil {
				t.Fatal(err)
			}
			hw, err := newD3DBackend(m.gpu, false)
			if err != nil {
				t.Fatal(err)
			}
			defer hw.close()
			m.gpu.hw = hw
			dxDumpShaders = os.Getenv("WIIU_D3D_DUMP") != ""
			for i := 0; i < frames && !m.Exited && m.Err == nil; i++ {
				m.RunFrame()
			}
			if m.Err != nil {
				t.Fatal(m.Err)
			}
			t.Logf("%d draws; last fallback: %q", m.gpu.Draws, hw.err)
			a, b := soft.screen.TV, m.screen.TV
			bad, worst := 0, 0
			for i := range a {
				d := 0
				for s := 0; s < 32; s += 8 {
					x, y := int(a[i]>>s&255), int(b[i]>>s&255)
					d = max(d, x-y, y-x)
				}
				if d > 8 {
					bad++
				}
				worst = max(worst, d)
			}
			t.Logf("%d of %d pixels differ by more than 8 (worst %d)", bad, len(a), worst)
			if bad > len(a)/1000 {
				t.Errorf("pictures differ")
				if out := os.Getenv("WIIU_DUMP"); out != "" {
					writePPM(filepath.Join(out, name+"_soft.ppm"), a, TVWidth, TVHeight)
					writePPM(filepath.Join(out, name+"_d3d.ppm"), b, TVWidth, TVHeight)
				}
			}
		})
	}
}

// TestUseHostGPU: a machine picks up the Direct3D backend, and the
// software switch keeps it off.
func TestUseHostGPU(t *testing.T) {
	m := newMachine(nil)
	if !m.UseHostGPU() {
		t.Skip("no Direct3D device here")
	}
	m.Close()
	if m.gpu.hw != nil {
		t.Fatal("Close kept the backend")
	}
	t.Setenv("RETROPIXELS_WIIU_GPU", "software")
	m2 := newMachine(nil)
	if m2.UseHostGPU() {
		t.Fatal("RETROPIXELS_WIIU_GPU=software still used the host GPU")
	}
}

// TestD3DPrims draws each primitive type with the software renderer and
// with Direct3D and compares the pictures.
func TestD3DPrims(t *testing.T) {
	verts := []float32{
		-0.9, 0.9, 0, 1, 1, 0, 0, 1,
		0.8, 0.7, 0, 1, 0, 1, 0, 1,
		-0.7, -0.8, 0, 1, 0, 0, 1, 1,
		0.6, -0.5, 0, 1, 1, 1, 0, 1,
		0.1, 0.2, 0, 1, 0, 1, 1, 1,
		-0.3, -0.1, 0, 1, 1, 0, 1, 1,
	}
	run := func(prim uint32, hw bool) []uint32 {
		g := newTestGPU()
		if hw {
			b, err := newD3DBackend(g, false)
			if err != nil {
				t.Skip(err)
			}
			defer b.close()
			g.hw = b
		}
		b := &pm4{}
		setupTarget(b, tile2DThin1)
		passShaders(g, b)
		putFloatsBE(g, tVB, verts...)
		b.packet(itHLE_CLEAR_COLOR, tCB>>8, (64/8-1)|(64*64/64-1)<<10, fmt8_8_8_8<<2|tile2DThin1<<8, 0, f2u(0), f2u(0), f2u(0), f2u(1))
		b.set(regVGT_PRIMITIVE_TYPE, prim)
		b.packet(itNUM_INSTANCES, 1)
		b.packet(itDRAW_INDEX_AUTO, 6, 2)
		g.exec(b.w)
		if g.hw != nil {
			if e := g.hw.(*d3dBackend).err; e != "" {
				t.Fatalf("prim %#x fell back: %s", prim, e)
			}
			g.hw.flushAll()
		}
		out := make([]uint32, 64*64)
		for y := uint32(0); y < 64; y++ {
			for x := uint32(0); x < 64; x++ {
				out[y*64+x] = readPix(g, tile2DThin1, x, y)
			}
		}
		return out
	}
	for _, prim := range []uint32{1, 2, 3, 4, 5, 6, 0x11, 0x13, 0x14} {
		soft, hard := run(prim, false), run(prim, true)
		bad, drawn := 0, 0
		for i := range soft {
			if soft[i] != 0xff {
				drawn++
			}
			for c := 0; c < 32; c += 8 {
				d := int(soft[i]>>c&0xff) - int(hard[i]>>c&0xff)
				if d > 2 || d < -2 {
					bad++
					break
				}
			}
		}
		t.Logf("prim %#x: %d pixels drawn, %d differ", prim, drawn, bad)
		// edges may land differently by a pixel
		if bad > 64 {
			t.Errorf("prim %#x: %d of %d pixels differ", prim, bad, drawn)
		}
	}
}

// TestD3DTextures samples 1D, 2D, 3D and array textures (coordinates from
// the vertex colours) with both renderers and compares.
func TestD3DTextures(t *testing.T) {
	verts := []float32{
		-1, 1, 0, 1, 0.02, 0.03, 0.1, 1,
		1, 1, 0, 1, 0.97, 0.1, 0.4, 1,
		-1, -1, 0, 1, 0.05, 0.95, 0.6, 1,
		1, -1, 0, 1, 0.9, 0.9, 0.9, 1,
	}
	run := func(dim, h, d uint32, hw bool) []uint32 {
		g := newTestGPU()
		if hw {
			b, err := newD3DBackend(g, false)
			if err != nil {
				t.Skip(err)
			}
			defer b.close()
			g.hw = b
		}
		b := &pm4{}
		setupTarget(b, tile2DThin1)
		passShaders(g, b)
		ps := &shaderAsm{}
		ps.TEX(texSpec{inst: 0x10, srcGPR: 0, srcSel: [4]uint32{0, 1, 2, 4}, dstGPR: 1, dstSel: [4]uint32{0, 1, 2, 3},
			normalized: [4]bool{true, true, true, true}})
		ps.Export(true, 0, 0, 1, 1, [4]uint32{0, 1, 2, 3})
		ps.End()
		putShader(g, tPS, ps)
		putFloatsBE(g, tVB, verts...)
		seed := uint32(12345)
		for a := uint32(tTex); a < tTex+0x10000; a += 4 {
			seed = seed*1103515245 + 12345
			g.mem.Write32(a, seed)
		}
		const w = 64
		b.set(regSQ_FETCH_RESOURCE+0*28,
			dim|tileLinearAligned<<3|(w/8-1)<<8|(w-1)<<19,
			(h-1)|(d-1)<<14|fmt8_8_8_8<<26,
			tTex>>8, 0,
			0<<16|1<<19|2<<22|3<<25,
			0, 2<<30)
		b.set(regSQ_TEX_SAMPLER_WORD0_0, 0, 0, 0)
		b.packet(itHLE_CLEAR_COLOR, tCB>>8, (64/8-1)|(64*64/64-1)<<10, fmt8_8_8_8<<2|tile2DThin1<<8, 0, f2u(0), f2u(0), f2u(0), f2u(1))
		b.set(regVGT_PRIMITIVE_TYPE, 5)
		b.packet(itNUM_INSTANCES, 1)
		b.packet(itDRAW_INDEX_AUTO, 4, 2)
		g.exec(b.w)
		if g.ShaderErr != "" {
			t.Fatal(g.ShaderErr)
		}
		if g.hw != nil {
			if e := g.hw.(*d3dBackend).err; e != "" {
				t.Fatalf("dim %d fell back: %s", dim, e)
			}
			g.hw.flushAll()
		}
		out := make([]uint32, 64*64)
		for y := uint32(0); y < 64; y++ {
			for x := uint32(0); x < 64; x++ {
				out[y*64+x] = readPix(g, tile2DThin1, x, y)
			}
		}
		return out
	}
	for _, c := range []struct{ dim, h, d uint32 }{{0, 1, 1}, {1, 8, 1}, {2, 8, 4}, {4, 1, 4}, {5, 8, 4}} {
		soft, hard := run(c.dim, c.h, c.d, false), run(c.dim, c.h, c.d, true)
		bad := 0
		for i := range soft {
			if soft[i] != hard[i] {
				bad++
				if bad <= 3 {
					t.Logf("dim %d pixel %d,%d: software %08x, Direct3D %08x", c.dim, i%64, i/64, soft[i], hard[i])
				}
			}
		}
		t.Logf("dim %d: %d of 4096 pixels differ", c.dim, bad)
		// texel edges can land a pixel apart
		if bad > 200 {
			t.Errorf("dim %d: %d pixels differ", c.dim, bad)
		}
	}
}
