package n64

// RDP benchmark: a few hundred shaded, perspective-textured (bilinear),
// z-buffered triangles in 1-cycle mode on a 320x240 16-bit frame buffer.

import (
	"math/rand"
	"sort"
	"testing"
)

type benchVtx struct {
	x, y       float64 // pixels (y whole)
	r, g, b, a float64 // 0-255
	s, t, w, z float64 // texels, 1/w in (0,1], 0-1
}

// triCmd builds a shaded, textured, z-buffered triangle command (op 0x0f)
// from three vertices.
func triCmd(v [3]benchVtx) []uint64 {
	sort.Slice(v[:], func(i, j int) bool { return v[i].y < v[j].y })
	v0, v1, v2 := v[0], v[1], v[2]
	fx := func(f float64) int32 { return int32(f * 65536) }
	slope := func(a, b benchVtx) float64 {
		if b.y == a.y {
			return 0
		}
		return (b.x - a.x) / (b.y - a.y)
	}
	dxh, dxm, dxl := slope(v0, v2), slope(v0, v1), slope(v1, v2)
	lft := uint64(0)
	if v1.x > v0.x+dxh*(v1.y-v0.y) {
		lft = 1
	}
	yh, ym, yl := uint64(v0.y*4), uint64(v1.y*4), uint64(v2.y*4)
	c := []uint64{
		0x0f<<56 | lft<<55 | (yl&0x3fff)<<32 | (ym&0x3fff)<<16 | yh&0x3fff,
		uint64(uint32(fx(v1.x)))<<32 | uint64(uint32(fx(dxl))),
		uint64(uint32(fx(v0.x)))<<32 | uint64(uint32(fx(dxh))),
		uint64(uint32(fx(v0.x)))<<32 | uint64(uint32(fx(dxm))),
	}
	// plane gradients of each attribute
	det := (v1.x-v0.x)*(v2.y-v0.y) - (v2.x-v0.x)*(v1.y-v0.y)
	grad := func(a0, a1, a2 float64) (float64, float64) {
		ddx := ((a1-a0)*(v2.y-v0.y) - (a2-a0)*(v1.y-v0.y)) / det
		ddy := ((a2-a0)*(v1.x-v0.x) - (a1-a0)*(v2.x-v0.x)) / det
		return ddx, ddy
	}
	var a0, dx, de, dy [8]int32
	set := func(i int, f0, f1, f2, scale float64) {
		gx, gy := grad(f0, f1, f2)
		a0[i], dx[i] = fx(f0*scale), fx(gx*scale)
		de[i], dy[i] = fx((gy+gx*dxh)*scale), fx(gy*scale)
	}
	set(0, v0.r, v1.r, v2.r, 1)
	set(1, v0.g, v1.g, v2.g, 1)
	set(2, v0.b, v1.b, v2.b, 1)
	set(3, v0.a, v1.a, v2.a, 1)
	// S and T are 10.5 texels times W; W is scaled so that s*32768/w is S
	set(4, v0.s*32*v0.w, v1.s*32*v1.w, v2.s*32*v2.w, 1)
	set(5, v0.t*32*v0.w, v1.t*32*v1.w, v2.t*32*v2.w, 1)
	set(6, v0.w, v1.w, v2.w, 32768)
	for blk := 0; blk < 2; blk++ {
		var w [8]uint64
		for i := 0; i < 4; i++ {
			k := blk*4 + i
			sh := uint(48 - 16*i)
			w[0] |= uint64(uint32(a0[k])>>16) << sh
			w[1] |= uint64(uint32(dx[k])>>16) << sh
			w[2] |= uint64(uint32(a0[k])&0xffff) << sh
			w[3] |= uint64(uint32(dx[k])&0xffff) << sh
			w[4] |= uint64(uint32(de[k])>>16) << sh
			w[5] |= uint64(uint32(dy[k])>>16) << sh
			w[6] |= uint64(uint32(de[k])&0xffff) << sh
			w[7] |= uint64(uint32(dy[k])&0xffff) << sh
		}
		c = append(c, w[:]...)
	}
	zx, zy := grad(v0.z, v1.z, v2.z)
	const zs = 1 << 30 // z is 16.16 of an 18-bit range scaled to 0-1
	pz := func(f float64) uint64 { return uint64(uint32(int32(f * zs))) }
	c = append(c, pz(v0.z)<<32|pz(zx), pz(zy+zx*dxh)<<32|pz(zy))
	return c
}

func BenchmarkRDPTriangles(b *testing.B) {
	m := &Machine{rdram: make([]byte, rdramSize)}
	d := &m.RDP
	d.m = m
	d.reset()
	const fb, zb, tex = 0x100000, 0x180000, 0x200000

	// a 32x32 RGBA16 texture, wrapped with mask 5
	for i := 0; i < 32*32; i++ {
		put16(m.rdram, tex+uint32(i*2), uint16(i*37)|1)
	}
	d.command([]uint64{0x3d<<56 | 2<<51 | 31<<32 | tex})
	d.command([]uint64{0x35<<56 | 2<<51 | 8<<41 | 7<<24})
	d.command([]uint64{0x34<<56 | 7<<24 | 31<<14 | 31<<2})
	d.command([]uint64{0x35<<56 | 2<<51 | 8<<41 | 5<<14 | 5<<4})
	d.command([]uint64{0x32<<56 | 31<<14 | 31<<2})

	d.command([]uint64{0x3f<<56 | 2<<51 | 319<<32 | fb})
	d.command([]uint64{0x3e<<56 | zb})
	d.command([]uint64{0x2d<<56 | 320<<14 | 240<<2})
	// 1-cycle, perspective, bilinear, z compare and update, no dithering
	d.command([]uint64{0x2f<<56 | 1<<51 | 1<<45 | 1<<43 | 1<<42 | 3<<38 | 3<<36 | 1<<5 | 1<<4})
	// colour = TEX0 * SHADE, alpha = TEX0 * SHADE alpha
	d.command([]uint64{0x3c<<56 | 1<<52 | 4<<47 | 1<<44 | 4<<41 | 1<<37 | 4<<32 |
		15<<28 | 15<<24 | 1<<21 | 4<<18 | 7<<15 | 7<<12 | 7<<9 | 7<<6 | 7<<3 | 7})

	rng := rand.New(rand.NewSource(1))
	var tris [][]uint64
	for len(tris) < 300 {
		cx, cy := 20+rng.Float64()*280, 20+float64(rng.Intn(200))
		var v [3]benchVtx
		for i := range v {
			v[i] = benchVtx{
				x: cx + rng.Float64()*50 - 25, y: cy + float64(rng.Intn(40)-20),
				r: rng.Float64() * 255, g: rng.Float64() * 255, b: rng.Float64() * 255, a: 255,
				s: rng.Float64() * 64, t: rng.Float64() * 64, w: 0.5 + rng.Float64()*0.5,
				z: 0.1 + rng.Float64()*0.8,
			}
		}
		if v[0].y == v[1].y && v[1].y == v[2].y {
			continue
		}
		tris = append(tris, triCmd(v))
	}

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		b.StopTimer()
		z := m.rdram[zb : zb+320*240*2]
		for j := 0; j < len(z); j += 2 {
			z[j], z[j+1] = 0xff, 0xfc
		}
		b.StartTimer()
		for _, c := range tris {
			d.command(c)
		}
	}
	b.ReportMetric(float64(b.Elapsed().Nanoseconds())/float64(b.N*len(tris)), "ns/tri")
}
