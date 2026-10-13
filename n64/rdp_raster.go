package n64

// RDP edge walking: triangles, texture rectangles and fill rectangles are
// turned into spans (one per scan line) with their coverage and the
// starting values of the interpolated attributes.
//
// Every scan line is split into 4 sub-scan-lines. Each sub-scan-line has a
// left and right edge position; a pixel has 8 coverage samples, two per
// sub-scan-line (at x+0 and x+0.5 on even ones, x+0.25 and x+0.75 on odd
// ones). Attributes are given at the major (H) edge on the first scan line
// and step by their DxDe along that edge and by DxDx across the span.

// attribute slots
const (
	atR = iota
	atG
	atB
	atA
	atS
	atT
	atW
	atZ
	atN
)

type rdpSpans struct {
	a0, dx, de, dy [atN]int32 // start value, per pixel, per line on the edge, per line
	shade, tex, z  bool
	tile, levels   uint32
	rect           bool
	lq, rq         [4]int32 // sub-scan-line edges in quarter pixels: covered if lq <= q < rq
}

// sext14 sign-extends a 14-bit 11.2 coordinate.
func sext14(v uint64) int32 { return int32(uint32(v)<<18) >> 18 }

// attrs reads one 8-word block of 4 interleaved int.frac attributes.
func attrs(c []uint64, a0, dx, de, dy []int32) {
	for i := 0; i < 4; i++ {
		sh := uint(48 - 16*i)
		hi := func(k int) int32 { return int32(uint32(c[k]>>sh&0xffff) << 16) }
		lo := func(k int) int32 { return int32(uint32(c[k] >> sh & 0xffff)) }
		a0[i] = hi(0) | lo(2)
		dx[i] = hi(1) | lo(3)
		de[i] = hi(4) | lo(6)
		dy[i] = hi(5) | lo(7)
	}
}

func (d *RDP) triangle(c []uint64, shade, tex, z bool) {
	w := c[0]
	sp := &d.sp
	*sp = rdpSpans{shade: shade, tex: tex, z: z}
	sp.tile = uint32(w>>48) & 7
	sp.levels = uint32(w>>51) & 7
	lft := w>>55&1 != 0
	yl, ym, yh := sext14(w>>32), sext14(w>>16), sext14(w)
	xl, dxldy := int32(c[1]>>32), int32(c[1])
	xh, dxhdy := int32(c[2]>>32), int32(c[2])
	xm, dxmdy := int32(c[3]>>32), int32(c[3])
	k := 4
	if shade {
		attrs(c[k:k+8], sp.a0[atR:], sp.dx[atR:], sp.de[atR:], sp.dy[atR:])
		k += 8
	}
	if tex {
		var a0, dx, de, dy [4]int32
		attrs(c[k:k+8], a0[:], dx[:], de[:], dy[:])
		copy(sp.a0[atS:atZ], a0[:3])
		copy(sp.dx[atS:atZ], dx[:3])
		copy(sp.de[atS:atZ], de[:3])
		copy(sp.dy[atS:atZ], dy[:3])
		k += 8
	}
	if z {
		sp.a0[atZ], sp.dx[atZ] = int32(c[k]>>32), int32(c[k])
		sp.de[atZ], sp.dy[atZ] = int32(c[k+1]>>32), int32(c[k+1])
	}
	d.rasterize(lft, yh, ym, yl, xh, dxhdy, xm, dxmdy, xl, dxldy)
}

// texRect draws Texture Rectangle (flip swaps the roles of S and T).
func (d *RDP) texRect(w, w2 uint64, flip bool) {
	sp := &d.sp
	*sp = rdpSpans{tex: true, rect: true}
	sp.tile = uint32(w>>24) & 7
	xl, yl := int32(w>>44)&0xfff, int32(w>>32)&0xfff
	xh, yh := int32(w>>12)&0xfff, int32(w)&0xfff
	s, t := int32(int16(w2>>48)), int32(int16(w2>>32))
	dsdx, dtdy := int32(int16(w2>>16)), int32(int16(w2))
	sp.a0[atS], sp.a0[atT] = s<<16, t<<16
	if flip {
		sp.de[atS], sp.dy[atS] = dsdx<<11, dsdx<<11
		sp.dx[atT] = dtdy << 11
	} else {
		sp.dx[atS] = dsdx << 11
		sp.de[atT], sp.dy[atT] = dtdy<<11, dtdy<<11
	}
	d.rect(xh, yh, xl, yl)
}

func (d *RDP) fillRect(w uint64) {
	d.sp = rdpSpans{rect: true}
	xl, yl := int32(w>>44)&0xfff, int32(w>>32)&0xfff
	xh, yh := int32(w>>12)&0xfff, int32(w)&0xfff
	d.rect(xh, yh, xl, yl)
}

func (d *RDP) rect(xh, yh, xl, yl int32) {
	if d.cycleType() >= 2 { // fill and copy include the bottom and right edges
		xl |= 3
		xl++
		yl |= 3
		yl++
	}
	d.rasterize(true, yh, yl, yl, xh<<14, 0, xl<<14, 0, xl<<14, 0)
}

func (d *RDP) cycleType() uint32 { return uint32(d.Modes>>52) & 3 }

// rasterize walks the edges and draws every span. y values are 11.2,
// x values 16.16, slopes 16.16 per scan line.
func (d *RDP) rasterize(lft bool, yh, ym, yl, xh, dxhdy, xm, dxmdy, xl, dxldy int32) {
	if d.Color.Addr == 0 && d.Color.Width == 0 {
		return
	}
	d.setupPipe()
	sp := &d.sp
	ys0, ys1 := yh, yl
	if sc := int32(d.ScYH); ys0 < sc {
		ys0 = sc
	}
	if sc := int32(d.ScYL); ys1 > sc {
		ys1 = sc
	}
	if ys0 >= ys1 {
		return
	}
	scL, scR := int32(d.ScXH), int32(d.ScXL)
	ybase := yh &^ 3
	for y := ys0 >> 2; y <= (ys1-1)>>2; y++ {
		if d.ScField != 0 && uint32(y&1) != d.ScOdd {
			continue
		}
		xmin, xmax := int32(1<<30), int32(-1<<30)
		for k := int32(0); k < 4; k++ {
			ys := y*4 + k
			if ys < ys0 || ys >= ys1 {
				sp.lq[k], sp.rq[k] = 0, 0
				continue
			}
			dy := int64(ys - ybase)
			eh := int64(xh) + (int64(dxhdy)*dy)>>2
			var eo int64
			if ys < ym {
				eo = int64(xm) + (int64(dxmdy)*dy)>>2
			} else {
				eo = int64(xl) + (int64(dxldy)*int64(ys-ym))>>2
			}
			l, r := eh, eo
			if !lft {
				l, r = eo, eh
			}
			lq := int32(clampQ((l + 0x3fff) >> 14))
			rq := int32(clampQ((r + 0x3fff) >> 14))
			if lq < scL {
				lq = scL
			}
			if rq > scR {
				rq = scR
			}
			if lq >= rq {
				sp.lq[k], sp.rq[k] = 0, 0
				continue
			}
			sp.lq[k], sp.rq[k] = lq, rq
			if lq>>2 < xmin {
				xmin = lq >> 2
			}
			if (rq-1)>>2 > xmax {
				xmax = (rq - 1) >> 2
			}
		}
		if xmin > xmax {
			continue
		}
		// attribute values at pixel xmin of this line
		dyl := int64(y) - int64(ybase>>2)
		xe := int64(xh) + (int64(dxhdy)*(int64(y)*4-int64(ybase)))>>2
		off := int64(xmin)<<16 - xe
		var at [atN]int32
		for i := range at {
			at[i] = sp.a0[i] + int32(int64(sp.de[i])*dyl) + int32((int64(sp.dx[i])*off)>>16)
		}
		d.drawSpan(y, xmin, xmax, &at)
	}
}

func clampQ(v int64) int64 {
	if v < -(1 << 20) {
		return -(1 << 20)
	}
	if v > 1<<20 {
		return 1 << 20
	}
	return v
}

// coverage counts the covered samples (0-8) of pixel x on the current line.
func (sp *rdpSpans) coverage(x int32) int32 {
	n := int32(0)
	q := x * 4
	for k := int32(0); k < 4; k++ {
		l, r := sp.lq[k], sp.rq[k]
		p := q + k&1
		if p >= l && p < r {
			n++
		}
		if p+2 >= l && p+2 < r {
			n++
		}
	}
	return n
}

// fullRange returns the pixels of the current line with all 8 samples covered.
func (sp *rdpSpans) fullRange() (int32, int32) {
	lo, hi := int32(-1<<30), int32(1<<30)
	for k := int32(0); k < 4; k++ {
		l, r := sp.lq[k], sp.rq[k]
		if l >= r {
			return 1, 0
		}
		// first pixel whose first sample (x*4 + k&1) >= l
		a := (l - k&1 + 3) >> 2
		// last pixel whose last sample (x*4 + k&1 + 2) < r
		b := (r - 1 - k&1 - 2) >> 2
		if a > lo {
			lo = a
		}
		if b < hi {
			hi = b
		}
	}
	return lo, hi
}
