package wiiu

import (
	"math"
	"testing"
)

func newTestGPU() *Latte {
	m := &Machine{Mem: NewMemory()}
	m.gx2 = newGX2()
	m.screen = newOSScreen()
	g := newLatte(m)
	m.Log = func(s string) { println(s) }
	return g
}

func putShader(g *Latte, addr uint32, p *shaderAsm) {
	g.mem.WriteBytes(addr, p.Bytes())
}

func putFloatsBE(g *Latte, addr uint32, f ...float32) {
	for i, v := range f {
		g.mem.Write32(addr+uint32(i)*4, math.Float32bits(v))
	}
}

const (
	tCB  = 0x10000000
	tVB  = 0x10100000
	tVS  = 0x10200000
	tPS  = 0x10201000
	tTex = 0x10300000
	tDB  = 0x10400000
)

// setupTarget: a 64x64 RGBA8 colour buffer with the given tile mode and a
// viewport covering it.
func setupTarget(b *pm4, mode uint32) {
	b.set(regCB_COLOR0_BASE, tCB>>8)
	b.set(regCB_COLOR0_SIZE, (64/8-1)|(64*64/64-1)<<10)
	b.set(regCB_COLOR0_INFO, fmt8_8_8_8<<2|mode<<8)
	b.set(regCB_COLOR0_VIEW, 0)
	b.set(regCB_TARGET_MASK, 0xf)
	b.set(regCB_SHADER_MASK, 0xf)
	b.set(regCB_COLOR_CONTROL, 0xcc<<16)
	b.set(regPA_CL_VTE_CNTL, 0x43f)
	b.set(regPA_CL_VPORT_XSCALE_0, f2u(32), f2u(32), f2u(-32), f2u(32), f2u(0.5), f2u(0.5))
	b.set(regPA_SC_VPORT_ZMIN_0, f2u(0), f2u(1))
	b.set(regPA_SC_GENERIC_SCISSOR_TL, 0, 64|64<<16)
}

// colour-passing shaders: the VS fetches a position and a colour, the PS
// writes the colour
func passShaders(g *Latte, b *pm4) {
	vs := &shaderAsm{}
	vs.VTX(
		vtxSpec{buffer: 160, srcGPR: 0, dstGPR: 1, dstSel: [4]uint32{0, 1, 2, 3}, format: fmt32x4F, endian: 2},
		vtxSpec{buffer: 160, srcGPR: 0, dstGPR: 2, dstSel: [4]uint32{0, 1, 2, 3}, format: fmt32x4F, endian: 2, offset: 16},
	)
	vs.Export(true, 1, 60, 1, 1, [4]uint32{0, 1, 2, 3})
	vs.Export(true, 2, 0, 2, 1, [4]uint32{0, 1, 2, 3})
	vs.End()
	putShader(g, tVS, vs)
	ps := &shaderAsm{}
	ps.Export(true, 0, 0, 0, 1, [4]uint32{0, 1, 2, 3})
	ps.End()
	putShader(g, tPS, ps)
	b.set(regSQ_PGM_START_VS, tVS>>8)
	b.set(regSQ_PGM_START_PS, tPS>>8)
	b.set(regSPI_VS_OUT_CONFIG, 0)
	b.set(regSPI_VS_OUT_ID_0, 0xffffff00)
	b.set(regSPI_PS_IN_CONTROL_0, 1)
	b.set(regSPI_PS_INPUT_CNTL_0, 0)
	// the vertex buffer: resource 320 (VS base 160 + 160)
	b.set(regSQ_FETCH_RESOURCE+320*28, tVB, 32*16-1, 32<<8, 0, 0, 0, 3<<30)
}

func readPix(g *Latte, mode uint32, x, y uint32) uint32 {
	lay := surfLayout{Base: tCB, Pitch: 64, Height: 64, Slices: 1, BPP: 32, Mode: mode, Samples: 1}
	return g.mem.Read32(lay.addr(x, y, 0))
}

func TestLatteTriangle(t *testing.T) {
	for _, mode := range []uint32{tileLinearAligned, tile1DThin1, tile2DThin1} {
		g := newTestGPU()
		b := &pm4{}
		setupTarget(b, mode)
		passShaders(g, b)
		// a red/green/blue triangle covering the top-left half
		putFloatsBE(g, tVB,
			-1, 1, 0, 1, 1, 0, 0, 1,
			1, 1, 0, 1, 0, 1, 0, 1,
			-1, -1, 0, 1, 0, 0, 1, 1)
		b.packet(itHLE_CLEAR_COLOR, tCB>>8, (64/8-1)|(64*64/64-1)<<10, fmt8_8_8_8<<2|mode<<8, 0, f2u(0), f2u(0), f2u(0), f2u(1))
		b.set(regVGT_PRIMITIVE_TYPE, 4)
		b.packet(itNUM_INSTANCES, 1)
		b.packet(itDRAW_INDEX_AUTO, 3, 2)
		g.exec(b.w)
		if g.ShaderErr != "" {
			t.Fatal(g.ShaderErr)
		}
		if p := readPix(g, mode, 1, 1); p>>24 < 0xe0 || p&0xff != 0xff {
			t.Fatalf("mode %d: top-left %08x", mode, p)
		}
		if p := readPix(g, mode, 60, 1); p>>16&0xff < 0xe0 {
			t.Fatalf("mode %d: top-right %08x", mode, p)
		}
		if p := readPix(g, mode, 1, 60); p>>8&0xff < 0xe0 {
			t.Fatalf("mode %d: bottom-left %08x", mode, p)
		}
		if p := readPix(g, mode, 62, 62); p != 0x000000ff {
			t.Fatalf("mode %d: bottom-right (outside) %08x", mode, p)
		}
		// the diagonal is shared: pixels right on it belong to one side only
		n := 0
		for y := uint32(0); y < 64; y++ {
			for x := uint32(0); x < 64; x++ {
				if readPix(g, mode, x, y) != 0xff {
					n++
				}
			}
		}
		if n != 64*64/2+32 && n != 64*64/2-32 && n != 64*64/2 {
			t.Fatalf("mode %d: %d pixels drawn", mode, n)
		}
	}
}
