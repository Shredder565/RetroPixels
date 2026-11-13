package ctr

import (
	"encoding/binary"
	"math"
	"testing"
)

// toF24 makes a 24-bit PICA float.
func toF24(f float32) uint32 {
	b := math.Float32bits(f)
	if f == 0 {
		return b >> 8 & 0x800000
	}
	exp := b>>23&0xff - 64
	return b>>31<<23 | exp<<16 | b>>7&0xffff
}

type gpuRig struct {
	t      *testing.T
	m      *Machine
	g      *gpu
	fb, vb uint32 // physical addresses
	w, h   int
}

func newRig(t *testing.T, w, h int) *gpuRig {
	m := newMachine(Options{})
	r := &gpuRig{t: t, m: m, g: m.gpu, w: w, h: h}
	pa, err := m.mem.allocLinear(0x100000)
	if err != nil {
		t.Fatal(err)
	}
	r.fb = pa
	r.vb = r.fb + 0x80000
	g := r.g
	set := func(id, v uint32) { g.writeReg(id, v, 0xffffffff) }
	// framebuffer: RGBA8, w×h
	set(0x117, 0)
	set(0x11d, r.fb>>3)
	set(0x11e, uint32(w)|uint32(h-1)<<12|1<<24)
	set(0x112, 0xf)
	set(0x113, 0xf)
	set(0x107, 0xf<<8)
	set(0x102, 3) // copy
	set(0x041, toF24(float32(w)/2))
	set(0x043, toF24(float32(h)/2))
	// TEV: stage 0 passes the vertex colour, the rest the previous stage
	set(0xc0, 0)
	set(0xc1, 0)
	set(0xc2, 0)
	for _, b := range []uint32{0xc8, 0xd0, 0xd8, 0xf0, 0xf8} {
		set(b, 0x000f000f)
		set(b+1, 0)
		set(b+2, 0)
	}
	// shader: o0 = v0, o1 = v1, o2 = v2
	set(0x2cb, 0)
	for i := uint32(0); i < 3; i++ {
		set(0x2cc, 0x13<<26|i<<21|i<<12)
	}
	set(0x2cc, 0x22<<26)
	set(0x2d5, 0)
	set(0x2d6, 0xf|0x1b<<5|0x1b<<14)
	set(0x2ba, 0)
	set(0x2bd, 7)
	set(0x04f, 3)
	set(0x050, 0x03020100) // position
	set(0x051, 0x0b0a0908) // colour
	set(0x052, 0x1f1f0d0c) // texture coordinates 0
	// three float attributes of 4 components
	set(0x201, 0xfff)
	set(0x202, 2<<28)
	set(0x2b9, 2)
	set(0x2bb, 0x210)
	set(0x200, r.vb>>3)
	set(0x203, 0)
	set(0x204, 0x210)
	set(0x205, 3<<28|48<<16)
	return r
}

func (r *gpuRig) set(id, v uint32) { r.g.writeReg(id, v, 0xffffffff) }

// vertices writes vertices (12 floats each: position, colour, uv).
func (r *gpuRig) vertices(vs ...float32) {
	b := r.m.mem.physSlice(r.vb, uint32(len(vs)*4))
	for i, f := range vs {
		binary.LittleEndian.PutUint32(b[i*4:], math.Float32bits(f))
	}
}

func (r *gpuRig) draw(mode, n uint32) {
	r.set(0x25e, mode<<8)
	r.set(0x228, n)
	r.set(0x22a, 0)
	r.set(0x22e, 1)
}

// px reads the colour buffer at (x, y), y counted from the bottom.
func (r *gpuRig) px(x, y int) uint32 {
	b := r.m.mem.physSlice(r.fb, uint32(r.w*r.h*4))
	off := int(tiledOffset(uint32(x), uint32(r.h-1-y), uint32(r.w))) * 4
	c := readColor(b[off:], 0)
	return uint32(c[0])<<24 | uint32(c[1])<<16 | uint32(c[2])<<8 | uint32(c[3])
}

func TestGPUTriangle(t *testing.T) {
	r := newRig(t, 64, 64)
	r.vertices(
		-1, -1, -0.5, 1, 1, 0, 0, 1, 0, 0, 0, 0,
		1, -1, -0.5, 1, 1, 0, 0, 1, 0, 0, 0, 0,
		-1, 1, -0.5, 1, 1, 0, 0, 1, 0, 0, 0, 0,
	)
	r.draw(0, 3)
	if got := r.px(5, 5); got != 0xff0000ff {
		t.Errorf("inside the triangle: %08x, want ff0000ff", got)
	}
	if got := r.px(60, 60); got != 0 {
		t.Errorf("outside the triangle: %08x, want 0", got)
	}
	// the diagonal: each pixel belongs to exactly one of two triangles
	r.vertices(
		1, -1, -0.5, 1, 0, 1, 0, 1, 0, 0, 0, 0,
		1, 1, -0.5, 1, 0, 1, 0, 1, 0, 0, 0, 0,
		-1, 1, -0.5, 1, 0, 1, 0, 1, 0, 0, 0, 0,
	)
	r.draw(0, 3)
	for y := 0; y < 64; y++ {
		for x := 0; x < 64; x++ {
			want := uint32(0xff0000ff)
			if x+y >= 64 {
				want = 0x00ff00ff
			}
			if x+y == 63 { // pixel centres on the diagonal go to one side
				continue
			}
			if got := r.px(x, y); got != want {
				t.Fatalf("pixel %d,%d: %08x, want %08x", x, y, got, want)
			}
		}
	}
	// a triangle partly behind the camera is clipped, not dropped
	r.vertices(
		-1, -1, -0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
		3, -1, -0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
		-1, 3, 0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
	)
	r.draw(0, 3)
	if got := r.px(2, 2); got != 0x0000ffff {
		t.Errorf("clipped triangle: %08x, want 0000ffff", got)
	}
}

func TestGPUTexture(t *testing.T) {
	r := newRig(t, 32, 32)
	// an 8×8 RGBA8 texture: left half white, right half black, top row red
	tex := r.fb + 0x90000
	tb := r.m.mem.physSlice(tex, 8*8*4)
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			c := [4]uint8{0, 0, 0, 255}
			if x < 4 {
				c = [4]uint8{255, 255, 255, 255}
			}
			if y == 7 { // the top row (t = 1)
				c = [4]uint8{255, 0, 0, 255}
			}
			writeColor(tb[int(tiledOffset(uint32(x), uint32(7-y), 8))*4:], 0, c)
		}
	}
	r.set(0x080, 1)
	r.set(0x082, 8|8<<16)
	r.set(0x083, 0)
	r.set(0x085, tex>>3)
	r.set(0x08e, 0)
	r.set(0xc0, 3|3<<16) // texture 0
	r.set(0xc2, 0)       // replace
	r.vertices(          // a strip covering the screen, uv 0-1
		-1, -1, -0.5, 1, 0, 0, 0, 0, 0, 0, 0, 0,
		1, -1, -0.5, 1, 0, 0, 0, 0, 1, 0, 0, 0,
		-1, 1, -0.5, 1, 0, 0, 0, 0, 0, 1, 0, 0,
		1, 1, -0.5, 1, 0, 0, 0, 0, 1, 1, 0, 0,
	)
	r.draw(1, 4)
	for _, c := range []struct {
		x, y int
		want uint32
	}{{2, 2, 0xffffffff}, {28, 2, 0x000000ff}, {2, 30, 0xff0000ff}, {28, 30, 0xff0000ff}, {14, 14, 0xffffffff}, {17, 14, 0x000000ff}} {
		if got := r.px(c.x, c.y); got != c.want {
			t.Errorf("pixel %d,%d: %08x, want %08x", c.x, c.y, got, c.want)
		}
	}
	// modulated by the vertex colour (blue) in stage 1
	r.set(0xc8, 0xf|0<<4|0xf<<16)
	r.set(0xca, 1|1<<16)
	r.vertices(
		-1, -1, -0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
		1, -1, -0.5, 1, 0, 0, 1, 1, 1, 0, 0, 0,
		-1, 1, -0.5, 1, 0, 0, 1, 1, 0, 1, 0, 0,
		1, 1, -0.5, 1, 0, 0, 1, 1, 1, 1, 0, 0,
	)
	r.draw(1, 4)
	if got := r.px(2, 2); got != 0x0000ffff {
		t.Errorf("modulated: %08x, want 0000ffff", got)
	}
}

func TestETC1(t *testing.T) {
	// one block, individual mode, both halves base colour 0x88 grey,
	// table 0, every pixel index 0 (+2)
	var blk [8]byte
	hi := uint32(8<<28 | 8<<24 | 8<<20 | 8<<16 | 8<<12 | 8<<8)
	binary.LittleEndian.PutUint64(blk[:], uint64(hi)<<32)
	d := make([]byte, 32)
	copy(d, blk[:])
	if c := etc1Texel(d, 1, 1, 8, false); c != [4]uint8{0x8a, 0x8a, 0x8a, 255} {
		t.Errorf("ETC1 texel: %v", c)
	}
}

// TestShaderFlow: a loop, a call and a conditional block in the shader.
func TestShaderFlow(t *testing.T) {
	var s shaderSetup
	s.swz[0] = 0xf | 0x1b<<5 | 0x1b<<14
	s.swz[1] = 0x8 | 0x1b<<5 | 0x1b<<14 // x only
	s.i[0] = [3]uint8{3, 0, 1}          // 4 times
	s.f[0] = vec4{1, 1, 1, 1}
	code := []uint32{
		0x13<<26 | 0x10<<21 | 0x20<<12 | 1,           // 0: mov r0.x, c0 (r0.x = 1)
		0x29<<26 | 0<<22 | 2<<10,                     // 1: loop i0 to 2
		0x00<<26 | 0x10<<21 | 0x10<<12 | 0x10<<7 | 1, // 2: add r0.x, r0, r0
		0x24<<26 | 6<<10 | 1,                         // 3: call 6, 1
		0x13<<26 | 0<<21 | 0x10<<12,                  // 4: mov o0, r0
		0x22 << 26,                                   // 5: end
		0x00<<26 | 0x10<<21 | 0x10<<12 | 0x0<<7 | 1,  // 6: add r0.x, r0, v0
	}
	copy(s.code[:], code)
	var u shaderUnit
	u.in[0] = vec4{0.5, 0, 0, 0}
	u.run(&s)
	if u.out[0][0] != 16.5 {
		t.Errorf("shader result %v, want 16.5", u.out[0][0])
	}
}

// TestLighting: one directional light, no lookup tables: the diffuse
// colour is scaled by N·L and the ambient colours are added.
func TestLighting(t *testing.T) {
	m := newMachine(Options{})
	g := m.gpu
	set := func(id, v uint32) { g.writeReg(id, v, 0xffffffff) }
	set(0x08f, 1)
	set(0x1c2, 0)                         // one light
	set(0x1c3, 0)                         // configuration 0
	set(0x1c4, 0xff<<24|0x7f<<16|0xff<<8) // no tables, no spot, no distance
	set(0x1c0, 0x10<<20|0x10<<10|0x10)    // global ambient
	set(0x142, 0xc8<<20|0x64<<10|0x32)    // diffuse
	set(0x143, 0)
	set(0x144, 0) // position (0, 0, 1): directional
	set(0x145, 0x3c00)
	set(0x149, 1)
	ls := g.lightState()
	var tex [3][4]uint8
	prim, _ := ls.shade([4]float32{0, 0, 0, 1}, v3{0, 0, -1}, &tex)
	if prim != [4]uint8{0xd8, 0x74, 0x42, 255} {
		t.Errorf("light facing the surface: %x", prim)
	}
	// turned 60° away: N·L = 0.5
	s := float32(math.Sin(math.Pi / 6))
	c := float32(math.Cos(math.Pi / 6))
	prim, _ = ls.shade([4]float32{s, 0, 0, c}, v3{0, 0, -1}, &tex)
	want := [4]uint8{116, 66, 41, 255} // half the diffuse colour plus the ambient
	for i := range want {
		if d := int(prim[i]) - int(want[i]); d < -1 || d > 1 {
			t.Errorf("light at 60°: %x, want %x", prim, want)
			break
		}
	}
}

func BenchmarkFullscreenQuad(b *testing.B) {
	r := newRig(&testing.T{}, 240, 400)
	r.set(0x080, 1)
	r.set(0x082, 8|8<<16)
	r.set(0x083, 2)
	r.set(0x085, (r.fb+0x90000)>>3)
	r.set(0x0c0, 3|3<<16)
	r.vertices(
		-1, -1, -0.5, 1, 1, 1, 1, 1, 0, 0, 0, 0,
		1, -1, -0.5, 1, 1, 1, 1, 1, 1, 0, 0, 0,
		-1, 1, -0.5, 1, 1, 1, 1, 1, 0, 1, 0, 0,
		1, 1, -0.5, 1, 1, 1, 1, 1, 1, 1, 0, 0,
	)
	for i := 0; i < b.N; i++ {
		r.draw(1, 4)
	}
}
