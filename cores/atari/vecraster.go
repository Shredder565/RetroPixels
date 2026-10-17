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
	pix    []uint32
	tone   [1024]uint32 // the picture's colour for an amount of light
}

func newVecRaster(w, h int, dvgX0, dvgY0, dvgW, dvgH float32) *vecRaster {
	r := &vecRaster{w: w, h: h, sx: float32(w) / dvgW, sy: float32(h) / dvgH, x0: dvgX0, y0: dvgY0}
	r.acc = make([]float32, w*h)
	r.glow = make([]float32, w*h)
	r.tmp = make([]float32, w*h)
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
			r.splat(x0, y0, z*3)
			r.splat(x0+0.6, y0, z*0.8)
			r.splat(x0, y0+0.6, z*0.8)
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
	r.blur()
	for i, a := range r.acc {
		v := a + r.glow[i]*0.7
		k := int(v * 256)
		if k > 1023 {
			k = 1023
		}
		r.pix[i] = r.tone[k]
	}
}

// blur makes glow from acc: a box blur 5 pixels across, twice (near a
// Gaussian), one direction at a time.
func (r *vecRaster) blur() {
	w, h := r.w, r.h
	pass := func(src, dst []float32, step, n, lines, stride int) {
		for l := 0; l < lines; l++ {
			base := l * stride
			var sum float32
			for k := 0; k < 2 && k < n; k++ {
				sum += src[base+k*step]
			}
			for i := 0; i < n; i++ {
				if i+2 < n {
					sum += src[base+(i+2)*step]
				}
				if i-3 >= 0 {
					sum -= src[base+(i-3)*step]
				}
				dst[base+i*step] = sum * 0.2
			}
		}
	}
	pass(r.acc, r.tmp, 1, w, h, w)
	pass(r.tmp, r.glow, w, h, w, 1)
	pass(r.glow, r.tmp, 1, w, h, w)
	pass(r.tmp, r.glow, w, h, w, 1)
}
