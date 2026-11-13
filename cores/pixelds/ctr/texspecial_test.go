package ctr

import (
	"math"
	"testing"
)

// TestCubeMap: six one-colour faces; the direction in (u, v, w) picks one.
func TestCubeMap(t *testing.T) {
	r := newRig(t, 16, 16)
	colors := [6][4]uint8{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255}}
	base := r.fb + 0x90000
	for f, c := range colors {
		tb := r.m.mem.physSlice(base+uint32(f)*0x100, 8*8*4)
		for i := 0; i < 64; i++ {
			writeColor(tb[i*4:], 0, c)
		}
		if f == 0 {
			r.set(0x085, (base)>>3)
		} else {
			r.set(0x085+uint32(f), (base+uint32(f)*0x100)>>3&0x3fffff)
		}
	}
	r.set(0x052, 0x1f100d0c) // tc0 u, v and w
	r.set(0x080, 1)
	r.set(0x082, 8|8<<16)
	r.set(0x083, 1<<28) // cube
	r.set(0x08e, 0)
	r.set(0xc0, 3|3<<16)
	r.set(0xc2, 0)
	for f, dir := range [6][3]float32{{1, 0.2, 0.1}, {-1, 0.2, 0.1}, {0.1, 1, 0.2}, {0.1, -1, 0.2}, {0.2, 0.1, 1}, {0.2, 0.1, -1}} {
		var vs []float32
		for _, p := range [4][2]float32{{-1, -1}, {1, -1}, {-1, 1}, {1, 1}} {
			vs = append(vs, p[0], p[1], -0.5, 1, 0, 0, 0, 0, dir[0], dir[1], dir[2], 0)
		}
		r.vertices(vs...)
		r.draw(1, 4)
		c := colors[f]
		want := uint32(c[0])<<24 | uint32(c[1])<<16 | uint32(c[2])<<8 | 255
		if got := r.px(8, 8); got != want {
			t.Errorf("face %d: %08x, want %08x", f, got, want)
		}
	}
}

// TestProcTex: a procedural texture whose colour is u through a ramp
// from black to white (the colour map is the identity).
func TestProcTex(t *testing.T) {
	r := newRig(t, 32, 32)
	r.set(0x0af, 2<<8) // colour map: f(x) = x
	for i := 0; i < 128; i++ {
		v := uint32(math.Round(float64(i) / 128 * 4095))
		d := uint32(math.Round(4095.0/128)) & 0xfff
		r.set(0x0b0, v|d<<12)
	}
	r.set(0x0af, 4<<8) // colours: a grey ramp
	for i := 0; i < 256; i++ {
		r.set(0x0b0, uint32(i)*0x010101|0xff000000)
	}
	r.set(0x0af, 5<<8)
	for i := 0; i < 256; i++ {
		r.set(0x0b0, 0)
	}
	r.set(0x0a8, 1|1<<3|0<<6) // clamp to edge, combiner u
	r.set(0x0ac, 1|255<<11)   // linear, 255 colours wide
	r.set(0x0ad, 0)
	r.set(0x080, 1<<10) // unit 3 on, coordinates 0
	r.set(0xc0, 6|6<<16)
	r.set(0xc2, 0)
	r.vertices(
		-1, -1, -0.5, 1, 0, 0, 0, 0, 0, 0, 0, 0,
		1, -1, -0.5, 1, 0, 0, 0, 0, 1, 0, 0, 0,
		-1, 1, -0.5, 1, 0, 0, 0, 0, 0, 1, 0, 0,
		1, 1, -0.5, 1, 0, 0, 0, 0, 1, 1, 0, 0,
	)
	r.draw(1, 4)
	left, right := r.px(2, 16)>>24, r.px(29, 16)>>24
	if left > 30 || right < 220 || r.px(16, 16)>>24 < 100 || r.px(16, 16)>>24 > 160 {
		t.Errorf("ramp: left %d, middle %d, right %d", left, r.px(16, 16)>>24, right)
	}
}
