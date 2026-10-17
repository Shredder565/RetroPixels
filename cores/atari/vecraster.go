package atari

import "math"

// vecRaster draws the DVG's lines into a raster picture: each line is laid
// down as a row of soft dots (so it is anti-aliased), the picture gets a
// faint glow around everything bright, and the light adds up and is then
// compressed so crossings and dots look like a vector monitor's.
type vecRaster struct {
	w, h   int
	sx, sy float32 // DVG units to pixels
	x0, y0 float32 // the DVG position shown at the bottom left
	acc    []float32
	glow   []float32
	tmp    []float32
	col    []float32 // running sums down the glow's columns
	pix    []uint32
	tone   [1024]uint32 // the picture's colour for an amount of light
}

func newVecRaster(w, h int, dvgX0, dvgY0, dvgW, dvgH float32) *vecRaster {
	r := &vecRaster{w: w, h: h, sx: float32(w) / dvgW, sy: float32(h) / dvgH, x0: dvgX0, y0: dvgY0}
	r.acc = make([]float32, w*h)
	r.glow = make([]float32, w*h)
	r.tmp = make([]float32, w*h)
	r.col = make([]float32, w/2)
	r.pix = make([]uint32, w*h)
	for i := range r.tone {
		v := float64(i) / 256 // light 0..4
		l := 1 - math.Exp(-1.6*v)
		// a slightly blue white, going white when saturated
		rr := l * (0.86 + 0.14*l)
		gg := l * (0.92 + 0.08*l)
		bb := l
		r.tone[i] = uint32(rr*255)<<16 | uint32(gg*255)<<8 | uint32(bb*255)
	}
	return r
}

// splat adds light v at (x, y) in pixels, shared bilinearly over 4 pixels.
func (r *vecRaster) splat(x, y, v float32) {
	x -= 0.5
	y -= 0.5
	fx, fy := float32(math.Floor(float64(x))), float32(math.Floor(float64(y)))
	ix, iy := int(fx), int(fy)
	ax, ay := x-fx, y-fy
	if ix < -1 || iy < -1 || ix >= r.w || iy >= r.h {
		return
	}
	add := func(px, py int, f float32) {
		if px >= 0 && py >= 0 && px < r.w && py < r.h {
			r.acc[py*r.w+px] += f
		}
	}
	add(ix, iy, v*(1-ax)*(1-ay))
	add(ix+1, iy, v*ax*(1-ay))
	add(ix, iy+1, v*(1-ax)*ay)
	add(ix+1, iy+1, v*ax*ay)
}

// draw renders the lines into pix.
func (r *vecRaster) draw(lines []dvgLine) {
	clear(r.acc)
	for _, l := range lines {
		x0, y0 := (l.x0-r.x0)*r.sx, float32(r.h)-(l.y0-r.y0)*r.sy
		x1, y1 := (l.x1-r.x0)*r.sx, float32(r.h)-(l.y1-r.y0)*r.sy
		z := float32(l.z) / 15
		dx, dy := x1-x0, y1-y0
		n := int(math.Sqrt(float64(dx*dx+dy*dy))*2) + 1
		if n == 1 { // a dot: the beam stays, so it's brighter
			r.splat(x0, y0, z*3.5)
			for _, d := range [4][2]float32{{-0.7, 0}, {0.7, 0}, {0, -0.7}, {0, 0.7}} {
				r.splat(x0+d[0], y0+d[1], z*0.6)
			}
			continue
		}
		if n > 4096 {
			n = 4096
		}
		v := z * 0.8
		for i := 0; i <= n; i++ {
			t := float32(i) / float32(n)
			r.splat(x0+dx*t, y0+dy*t, v)
		}
	}
	r.makeGlow()
	w2 := r.w / 2
	for y := 0; y < r.h; y++ {
		// the glow is at half size: take it between its pixels
		gy := float32(y)*0.5 - 0.25
		y0 := max(int(gy), 0)
		y1 := min(y0+1, r.h/2-1)
		fy := max(gy-float32(y0), 0)
		row := r.acc[y*r.w : (y+1)*r.w]
		out := r.pix[y*r.w : (y+1)*r.w]
		g0, g1 := r.glow[y0*w2:(y0+1)*w2], r.glow[y1*w2:(y1+1)*w2]
		for x, a := range row {
			gx := x >> 1
			gx1 := min(gx+1, w2-1)
			if a+(g0[gx]+g0[gx1]+g1[gx]+g1[gx1])*0.6 < 1.0/256 { // dark: most of the screen
				out[x] = r.tone[0]
				continue
			}
			fx := float32(x&1) * 0.5
			g := (g0[gx]*(1-fx)+g0[gx1]*fx)*(1-fy) + (g1[gx]*(1-fx)+g1[gx1]*fx)*fy
			k := int((a + g*0.6) * 256)
			if k > 1023 {
				k = 1023
			}
			out[x] = r.tone[k]
		}
	}
}

// makeGlow: acc at half size, blurred (a box 5 pixels across, twice each
// way: near a Gaussian), into glow.
func (r *vecRaster) makeGlow() {
	w2, h2 := r.w/2, r.h/2
	g, t := r.glow[:w2*h2], r.tmp[:w2*h2]
	for y := 0; y < h2; y++ {
		a, b := r.acc[2*y*r.w:], r.acc[(2*y+1)*r.w:]
		for x := 0; x < w2; x++ {
			g[y*w2+x] = (a[2*x] + a[2*x+1] + b[2*x] + b[2*x+1]) * 0.25
		}
	}
	across := func(src, dst []float32) {
		for y := 0; y < h2; y++ {
			s, d := src[y*w2:(y+1)*w2], dst[y*w2:(y+1)*w2]
			sum := s[0] + s[1]
			for x := range d {
				if x+2 < w2 {
					sum += s[x+2]
				}
				if x >= 3 {
					sum -= s[x-3]
				}
				d[x] = sum * 0.2
			}
		}
	}
	// down the columns, a row at a time (kind to the cache)
	down := func(src, dst []float32) {
		sum := r.col
		copy(sum, src[:w2])
		for x := range sum {
			sum[x] += src[w2+x]
		}
		for y := 0; y < h2; y++ {
			if y+2 < h2 {
				add := src[(y+2)*w2 : (y+3)*w2]
				for x := range sum {
					sum[x] += add[x]
				}
			}
			if y >= 3 {
				sub := src[(y-3)*w2 : (y-2)*w2]
				for x := range sum {
					sum[x] -= sub[x]
				}
			}
			d := dst[y*w2 : (y+1)*w2]
			for x := range d {
				d[x] = sum[x] * 0.2
			}
		}
	}
	across(g, t)
	across(t, g)
	down(g, t)
	down(t, g)
}
