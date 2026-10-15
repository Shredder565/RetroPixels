package snes

import "math"

// Cx4 (web/emu/snes_cx4.js): the chip's functions computed directly
// (high-level emulation) when the game starts one.

var cx4Sin, cx4Cos [512]int

func init() {
	for i := range 512 {
		cx4Sin[i] = int(int16(jsRound(32767 * jsSin(2*math.Pi*float64(i)/512))))
		cx4Cos[i] = int(int16(jsRound(32767 * jsCos(2*math.Pi*float64(i)/512))))
	}
}

var cx4Constants = [16]int{0x000000, 0xffffff, 0x00ff00, 0xff0000, 0x00ffff, 0xffff00, 0x800000, 0x7fffff,
	0x008000, 0x007fff, 0xff7fff, 0xffff7f, 0x010000, 0xfeffff, 0x000100, 0x00feff}

const cx4ProgramSum = 0x054336

var cx4Rad = 2 * math.Pi / 128

func i32(x int) int { return int(int32(x)) }

// s16 of a JavaScript number
func s16n(x float64) int { return int(int16(jsToInt32(x))) }

type cx4 struct {
	s   *SNES
	c   *cart
	ram [0x2000]uint8
	w   struct{ x, y, z, x2, y2, dist, scale int }
}

func newCx4(s *SNES) coproc {
	c := &cx4{s: s, c: s.cart}
	s.claim(0x00, 0x3f, 0x6000, 0x7fff, true)
	return c
}

func (c *cx4) reset() { c.ram = [0x2000]uint8{} }

func (c *cx4) w16(a int) int   { return int(c.ram[a]) | int(c.ram[a+1])<<8 }
func (c *cx4) s16at(a int) int { return s16(c.w16(a)) }
func (c *cx4) w24(a int) int   { return int(c.ram[a]) | int(c.ram[a+1])<<8 | int(c.ram[a+2])<<16 }
func (c *cx4) put16(a, v int) {
	c.ram[a] = uint8(v)
	c.ram[a+1] = uint8(v >> 8)
}
func (c *cx4) put24(a, v int) {
	c.put16(a, v)
	c.ram[a+2] = uint8(v >> 16)
}
func (c *cx4) reg24s(a int) int { return s8(int(c.ram[a+2]))*65536 + c.w16(a) }

func (c *cx4) bus(addr int) int {
	bank, off := (addr>>16)&0xff, addr&0xffff
	if bank&0x40 == 0 && off >= 0x6000 && off < 0x8000 {
		return int(c.ram[off-0x6000])
	}
	if bank == 0x7e || bank == 0x7f {
		return int(c.s.wram[(bank&1)<<16|off])
	}
	v := c.c.read(bank, off)
	if v < 0 {
		return 0
	}
	return v
}

func (c *cx4) busBE16(addr int) int { return c.bus(addr)<<8 | c.bus(addr+1) }

func (c *cx4) read(bank, off int) int {
	if off < 0x6000 || off >= 0x8000 {
		return -1
	}
	if off == 0x7f5e {
		return 0
	}
	return int(c.ram[off-0x6000])
}

func (c *cx4) write(bank, off int, v uint8) bool {
	if off < 0x6000 || off >= 0x8000 {
		return false
	}
	c.ram[off-0x6000] = v
	if off == 0x7f47 {
		c.copy()
	} else if off == 0x7f4f {
		c.run(int(v))
	}
	return true
}

func (c *cx4) copy() {
	src, n, dst := c.w24(0x1f40), c.w16(0x1f43), c.w16(0x1f45)
	for i := 0; i < n; i++ {
		d := (dst + i) & 0xffff
		if d >= 0x6000 && d < 0x8000 {
			c.ram[d-0x6000] = uint8(c.bus((src + i) & 0xffffff))
		}
	}
}

func (c *cx4) run(fn int) {
	r := &c.ram
	page := r[0x1f4d]
	if page == 0x0e && fn < 0x40 && fn&3 == 0 {
		r[0x1f80] = uint8(fn >> 2)
		return
	}
	if fn == 0 {
		switch page {
		case 0x00:
			c.buildOAM()
		case 0x03:
			c.scaleRotate(0)
		case 0x05:
			c.transformLines()
		case 0x07:
			c.scaleRotate(64)
		case 0x08:
			c.wireframe()
		case 0x0b:
			c.disintegrate()
		case 0x0c:
			c.wave()
		}
		return
	}
	switch fn {
	case 0x01:
		for i := 0x300; i < 0xc00; i++ {
			r[i] = 0
		}
		c.wireframe()
	case 0x05:
		dist := c.w16(0x1f83)
		if dist != 0 {
			c.put16(0x1f80, i32(65536/dist*c.w16(0x1f81))>>8)
		} else {
			c.put16(0x1f80, 65536)
		}
	case 0x0d:
		x, y, want := c.s16at(0x1f80), c.s16at(0x1f83), c.s16at(0x1f86)
		l := jsHypot(float64(x), float64(y))
		k := 0.0
		if l != 0 {
			k = float64(want) / l
		}
		c.put16(0x1f89, int(math.Trunc(float64(x)*k)))
		c.put16(0x1f8c, int(math.Trunc(float64(y)*k)))
	case 0x10, 0x13:
		ang, radius := c.w16(0x1f80)&0x1ff, c.s16at(0x1f83)
		if fn == 0x13 {
			c.put24(0x1f86, i32(2*radius*cx4Cos[ang])>>8)
			c.put24(0x1f89, i32(2*radius*cx4Sin[ang])>>8)
		} else {
			y := i32(2*radius*cx4Sin[ang]) >> 16
			c.put24(0x1f86, i32(2*radius*cx4Cos[ang])>>16)
			c.put24(0x1f89, y-(y>>6))
		}
	case 0x15:
		x, y := c.s16at(0x1f80), c.s16at(0x1f83)
		c.put16(0x1f80, int(math.Trunc(jsHypot(float64(x), float64(y)))))
	case 0x1f:
		x, y := c.s16at(0x1f80), c.s16at(0x1f83)
		var a int
		if x == 0 {
			if y > 0 {
				a = 128
			} else {
				a = 384
			}
		} else {
			a = int(math.Trunc(jsAtan(float64(y)/float64(x)) * 256 / math.Pi))
			if x < 0 {
				a += 256
			}
			a &= 0x1ff
		}
		c.put16(0x1f86, a)
	case 0x22:
		c.trapezoid()
	case 0x25, 0x54:
		a := int64(c.reg24s(0x1f80))
		b := a
		at := 0x1f83
		if fn != 0x54 {
			b = int64(c.reg24s(0x1f83))
			at = 0x1f80
		}
		p := uint64(a*b) & (1<<48 - 1)
		c.put24(at, int(p&0xffffff))
		c.put24(at+3, int(p>>24))
	case 0x2d:
		x, y := c.turn3D(c.s16at(0x1f81), c.s16at(0x1f84), c.s16at(0x1f87), int(r[0x1f89]), int(r[0x1f8a]), int(r[0x1f8b]), c.w16(0x1f90), false)
		c.put16(0x1f80, x)
		c.put16(0x1f83, y)
	case 0x40:
		sum := 0
		for i := range 0x800 {
			sum += int(r[i])
		}
		c.put16(0x1f80, sum)
	case 0x5c:
		for i, v := range cx4Constants {
			c.put24(i*3, v)
		}
	case 0x89:
		c.put24(0x1f80, cx4ProgramSum)
	}
}

func (c *cx4) buildOAM() {
	r := &c.ram
	slot := int(r[0x626])
	for s := slot; s < 128; s++ {
		r[s*4+1] = 0xe0
	}
	camX, camY := c.w16(0x621), c.w16(0x623)
	emit := func(x, y, tile, attr int, big bool) {
		o, hi, sh := slot*4, 0x200+(slot>>2), (slot&3)*2
		r[o], r[o+1], r[o+2], r[o+3] = uint8(x), uint8(y), uint8(tile), uint8(attr)
		b := (x >> 8) & 1
		if big {
			b |= 2
		}
		r[hi] = uint8(int(r[hi])&^(3<<sh) | b<<sh)
		slot++
	}
	for n := 0; n < int(r[0x620]) && slot < 128; n++ {
		ob := 0x220 + n*16
		ox, oy := s16(c.w16(ob)-camX), s16(c.w16(ob+2)-camY)
		tile, attr := int(r[ob+5]), int(r[ob+4]|r[ob+6])
		list := int(r[ob+7]) | int(r[ob+8])<<8 | int(r[ob+9])<<16
		pieces := c.bus(list)
		if pieces == 0 {
			emit(ox, oy, tile, attr, true)
			continue
		}
		for k := 0; k < pieces && slot < 128; k++ {
			pc := list + 1 + k*4
			flags := c.bus(pc)
			big := flags&0x20 != 0
			size := 8
			if big {
				size = 16
			}
			dx, dy := s8(c.bus(pc+1)), s8(c.bus(pc+2))
			x, y := ox+dx, oy+dy
			if attr&0x40 != 0 {
				x = ox - dx - size
			}
			if attr&0x80 != 0 {
				y = oy - dy - size
			}
			if x < -16 || x > 272 || y < -16 || y > 224 {
				continue
			}
			emit(x, y, tile+c.bus(pc+3), attr^(flags&0xc0), big)
		}
	}
}

func plot4(r *[0x2000]uint8, a int, bit uint8, col int) {
	if col&1 != 0 {
		r[a] |= bit
	}
	if col&2 != 0 {
		r[a+1] |= bit
	}
	if col&4 != 0 {
		r[a+16] |= bit
	}
	if col&8 != 0 {
		r[a+17] |= bit
	}
}

func (c *cx4) scaleRotate(pad int) {
	r := &c.ram
	xs, ys := min(c.w16(0x1f8f), 0x7fff), min(c.w16(0x1f92), 0x7fff)
	ang := c.w16(0x1f80)
	var A, B, C, D int
	switch ang {
	case 0:
		A, B, C, D = xs, 0, 0, ys
	case 128:
		A, B, C, D = 0, -ys, xs, 0
	case 256:
		A, B, C, D = -xs, 0, 0, -ys
	case 384:
		A, B, C, D = 0, ys, -xs, 0
	default:
		a := ang & 0x1ff
		A = s16((cx4Cos[a] * xs) >> 15)
		B = s16(-((cx4Sin[a] * ys) >> 15))
		C = s16((cx4Sin[a] * xs) >> 15)
		D = s16((cx4Cos[a] * ys) >> 15)
	}
	w, h := int(r[0x1f89]&^7), int(r[0x1f8c]&^7)
	rowBytes := w*4 + pad
	for i := range min(0x2000, rowBytes*(h>>3)) {
		r[i] = 0
	}
	cx, cy := c.s16at(0x1f83), c.s16at(0x1f86)
	u0, v0 := cx<<12-cx*(A+B), cy<<12-cy*(C+D)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			out := (y>>3)*rowBytes + (y&7)*2 + (x>>3)*32
			if out+17 >= 0x2000 {
				continue
			}
			sx, sy := int(uint32(u0+x*A+y*B)>>12), int(uint32(v0+x*C+y*D)>>12)
			if sx >= w || sy >= h {
				continue
			}
			i := sy*w + sx
			col := 0
			if j := 0x600 + i>>1; j < 0x2000 {
				col = int(r[j]) >> ((i & 1) * 4)
			}
			plot4(r, out, 0x80>>(x&7), col)
		}
	}
}

func (c *cx4) turn3D(xi, yi, zi, ax, ay, az, scale int, persp bool) (int, int) {
	const cam = 0x95
	x, y, z := float64(xi), float64(yi), float64(zi)
	if persp {
		z -= cam
	}
	rot := func(a int, p, q float64) (float64, float64) {
		t := float64(-a) * cx4Rad
		co, si := jsCos(t), jsSin(t)
		return p*co - q*si, p*si + q*co
	}
	y, z = rot(ax, y, z)
	z, x = rot(ay, z, x)
	x, y = rot(az, x, y)
	var k float64
	if persp {
		k = float64(scale) / (0x90 * (z + cam)) * cam
	} else {
		k = float64(scale) / 0x100
	}
	return s16n(math.Trunc(x * k)), s16n(math.Trunc(y * k))
}

func sign(v int) int {
	switch {
	case v > 0:
		return 1
	case v < 0:
		return -1
	}
	return 0
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

// cx4LineSteps: step x, step y and the number of points of a line.
func cx4LineSteps(x1, y1, x2, y2 int) (int, int, int) {
	dx, dy := s16(x2-x1), s16(y2-y1)
	ax, ay := abs(dx), abs(dy)
	if ax > ay {
		return sign(dx) * 256, s16(256 * dy / ax), ax + 1
	}
	if dy == 0 {
		return dx, dy, 0
	}
	return s16(256 * dx / ay), sign(dy) * 256, ay + 1
}

func (c *cx4) transformLines() {
	r := &c.ram
	ax, ay, az, scale := int(r[0x1f83]), int(r[0x1f86]), int(r[0x1f89]), int(r[0x1f8c])
	points, lines := c.w16(0x1f80), c.w16(0xb00)
	for i, p := 0, 0; i < points && p+10 < 0x2000; i, p = i+1, p+0x10 {
		x, y := c.turn3D(c.s16at(p+1), c.s16at(p+5), c.s16at(p+9), ax, ay, az, scale, true)
		c.put16(p+1, x+0x80)
		c.put16(p+5, y+0x50)
	}
	for _, o := range []int{0x600, 0x608} {
		c.put16(o, 23)
		c.put16(o+2, 0x60)
		c.put16(o+5, 0x40)
	}
	for i, p, o := 0, 0xb02, 0x600; i < lines && p < 0x1f00 && o < 0x1e00; i, p, o = i+1, p+2, o+8 {
		a, b := int(r[p])<<4, int(r[p+1])<<4
		sx, sy, n := cx4LineSteps(c.s16at(a+1), c.s16at(a+5), c.s16at(b+1), c.s16at(b+5))
		if n != 0 {
			c.put16(o, n)
		} else {
			c.put16(o, 1)
		}
		c.put16(o+2, sx)
		c.put16(o+5, sy)
		c.w.x, c.w.y, c.w.dist = sx, sy, n
	}
}

func (c *cx4) wireframe() {
	r := &c.ram
	bank := int(r[0x1f82]) << 16
	point := func(a int) [3]int {
		return [3]int{s16(c.busBE16(bank | a)), s16(c.busBE16((bank | a) + 2)), s16(c.busBE16((bank | a) + 4))}
	}
	line := c.w24(0x1f80)
	for n := int(r[0x295]); n > 0; n, line = n-1, line+5 {
		from := c.busBE16(line)
		if from == 0xffff {
			back := line - 5
			for back > 0 && c.busBE16(back+2) == 0xffff {
				back -= 5
			}
			from = c.busBE16(back + 2)
		}
		c.drawLine(point(from), point(c.busBE16(line+2)), c.bus(line+4))
	}
}

func (c *cx4) drawLine(p1, p2 [3]int, color int) {
	r := &c.ram
	place := func(p [3]int) (int, int) {
		x, y := c.turn3D(p[0], p[1], p[2], int(r[0x1f86]), int(r[0x1f87]), int(r[0x1f88]), int(r[0x1f90]), false)
		return (x + 48) << 8, (y + 48) << 8
	}
	x, y := place(p1)
	xe, ye := place(p2)
	sx, sy, n := cx4LineSteps(x>>8, y>>8, xe>>8, ye>>8)
	c.w.x, c.w.y, c.w.dist = sx, sy, n
	for i := max(n, 1); i > 0; i, x, y = i-1, x+sx, y+sy {
		if x < 0x100 || y < 0x100 || x >= 0x6000 || y >= 0x6000 {
			continue
		}
		px, py := x>>8, y>>8
		a := 0x300 + (py>>3)*0xc0 + (px>>3)*16 + (py&7)*2
		bit := uint8(0x80 >> (px & 7))
		if color&1 != 0 {
			r[a] |= bit
		} else {
			r[a] &^= bit
		}
		if color&2 != 0 {
			r[a+1] |= bit
		} else {
			r[a+1] &^= bit
		}
	}
}

func (c *cx4) trapezoid() {
	r := &c.ram
	slope := func(a int) int {
		if cx4Cos[a] == 0 {
			return -0x80000000
		}
		return i32(cx4Sin[a] * 65536 / cx4Cos[a])
	}
	t1, t2 := slope(c.w16(0x1f8c)&0x1ff), slope(c.w16(0x1f8f)&0x1ff)
	shift, width := c.w16(0x1f86)-c.w16(0x1f80), c.w16(0x1f93)
	y := s16(c.w16(0x1f83) - c.w16(0x1f89))
	for j := 0; j < 225; j, y = j+1, s16(y+1) {
		left, right := 1, 0
		if y >= 0 {
			left = s16(floorDiv(t1*y, 65536) + shift)
			right = s16(floorDiv(t2*y, 65536) + shift + width)
			if left < 0 && right < 0 {
				left, right = 1, 0
			} else {
				left, right = max(left, 0), max(right, 0)
			}
			if left > 255 && right > 255 {
				left, right = 255, 254
			} else {
				left, right = min(left, 255), min(right, 255)
			}
		}
		r[0x800+j], r[0x900+j] = uint8(left), uint8(right)
	}
}

func (c *cx4) wave() {
	r := &c.ram
	start := int(r[0x1f83])
	for col := 0; col < 128; col++ {
		tile, mask := col>>2, 0xc0c0>>((col&3)*2)
		idx := start
		if col != 0 {
			idx = (start + col) & 0x7f
		}
		shift := -s8(int(r[0xb00+idx])) - 16
		for row := 0; row < 40; row++ {
			a, src := tile*16+(row>>3)*0x200+(row&7)*2, row+shift
			fill := 0xff00
			if src < 0 {
				fill = 0
			} else if src < 8 {
				fill = c.w16(0xa00 + src*2)
			}
			c.put16(a, c.w16(a)&^mask|fill&mask)
		}
	}
}

func (c *cx4) disintegrate() {
	r := &c.ram
	w, h := int(r[0x1f89]), int(r[0x1f8c])
	cx, cy, kx, ky := c.s16at(0x1f80), c.s16at(0x1f83), c.s16at(0x1f86), c.s16at(0x1f8f)
	for i := range min(0x2000, (w*h)>>1) {
		r[i] = 0
	}
	for i := 0; i < h; i++ {
		ty := i32(cy<<8 + (i-cy)*ky) // the web core's >> wraps it to 32 bits
		py := ty >> 8
		for j := 0; j < w; j++ {
			tx := i32(cx<<8 + (j-cx)*kx)
			px := tx >> 8
			src := 0x600 + i*(w>>1) + (j >> 1)
			if px < 0 || px >= w || py < 0 || py >= h || py*w+px >= 0x2000 || src >= 0x2000 {
				continue
			}
			out := (ty>>11)*w*4 + (tx>>11)*32 + (py&7)*2
			if out+17 < 0x2000 {
				plot4(r, out, uint8(0x80>>(px&7)), int(r[src])>>((j&1)*4))
			}
		}
	}
}

func (c *cx4) state(s *sio) {
	s.Bytes(c.ram[:])
	s.Ints(&c.w.x, &c.w.y, &c.w.z, &c.w.x2, &c.w.y2, &c.w.dist, &c.w.scale)
}
