package ctr

import "testing"

// TestGeometryShader: the vertex shader's outputs for three vertices go
// to a geometry shader (points mode, nine inputs) that emits the
// triangle; then again with the winding flag, and in fixed-primitive
// mode through its uniforms.
func TestGeometryShader(t *testing.T) {
	r := newRig(t, 64, 64)
	mov := func(dst, src uint32) uint32 { return 0x13<<26 | dst<<21 | src<<12 }
	setemit := func(id, prim, winding uint32) uint32 { return 0x2b<<26 | id<<24 | prim<<23 | winding<<22 }
	emit := uint32(0x2a << 26)
	load := func(src [3][2]uint32, winding uint32) {
		code := []uint32{}
		for i, s := range src {
			prim := uint32(0)
			if i == 2 {
				prim = 1
			}
			code = append(code, setemit(uint32(i), prim, winding), mov(0, s[0]), mov(1, s[1]), emit)
		}
		code = append(code, 0x22<<26)
		r.set(0x29b, 0)
		for _, c := range code {
			r.set(0x29c, c)
		}
	}
	r.set(0x2a5, 0)
	r.set(0x2a6, 0xf|0x1b<<5|0x1b<<14)
	r.set(0x28a, 0)
	r.set(0x28d, 3) // o0, o1
	r.set(0x229, 2) // use the geometry shader
	r.set(0x24a, 2) // three vertex shader outputs a vertex
	r.set(0x252, 0) // points
	r.set(0x289, 8) // nine inputs: three vertices
	r.set(0x28b, 0x76543210)
	r.set(0x28c, 0x8)
	load([3][2]uint32{{0, 1}, {3, 4}, {6, 7}}, 0)
	r.vertices(
		-1, -1, -0.5, 1, 1, 0, 0, 1, 0, 0, 0, 0,
		1, -1, -0.5, 1, 1, 0, 0, 1, 0, 0, 0, 0,
		-1, 1, -0.5, 1, 1, 0, 0, 1, 0, 0, 0, 0,
	)
	r.draw(0, 3)
	if got := r.px(5, 5); got != 0xff0000ff {
		t.Errorf("inside the emitted triangle: %08x, want ff0000ff", got)
	}
	if got := r.px(60, 60); got != 0 {
		t.Errorf("outside it: %08x, want 0", got)
	}
	if r.g.gs.b&(1<<15) == 0 {
		t.Errorf("b15 not set after the geometry shader ran")
	}

	// the winding flag swaps the first two vertices: with back faces
	// culled (counter-clockwise kept), the triangle disappears
	r.set(0x040, 2)
	load([3][2]uint32{{0, 1}, {3, 4}, {6, 7}}, 1)
	r.vertices(
		1, -1, -0.5, 1, 0, 1, 0, 1, 0, 0, 0, 0,
		1, 1, -0.5, 1, 0, 1, 0, 1, 0, 0, 0, 0,
		-1, 1, -0.5, 1, 0, 1, 0, 1, 0, 0, 0, 0,
	)
	r.draw(0, 3)
	if got := r.px(60, 60); got != 0 {
		t.Errorf("swapped winding wasn't culled: %08x", got)
	}
	load([3][2]uint32{{0, 1}, {3, 4}, {6, 7}}, 0)
	r.draw(0, 3)
	if got := r.px(60, 60); got != 0x00ff00ff {
		t.Errorf("the same triangle unswapped: %08x, want 00ff00ff", got)
	}
	r.set(0x040, 0)

	// fixed primitives: three vertices into uniforms c8.. (3 each)
	r.set(0x252, 2|2<<8|8<<16)
	load([3][2]uint32{{0x28, 0x29}, {0x2b, 0x2c}, {0x2e, 0x2f}}, 0)
	r.vertices(
		1, -1, -0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
		1, 1, -0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
		-1, 1, -0.5, 1, 0, 0, 1, 1, 0, 0, 0, 0,
	)
	r.draw(0, 3)
	if got := r.px(60, 60); got != 0x0000ffff {
		t.Errorf("fixed-primitive triangle: %08x, want 0000ffff", got)
	}
}
