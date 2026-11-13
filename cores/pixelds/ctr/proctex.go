package ctr

import "math"

// Procedural textures (texture unit 3): the colour comes from functions
// of the coordinates instead of an image. The coordinates are made
// positive, optionally shaken with noise, shifted every other row,
// clamped or repeated, combined into one number (u, u², u+v, the
// distance from the origin, ...), mapped through a table, and that picks
// a colour from a 256-entry colour table. Registers 0xa8-0xad hold the
// settings; 0xaf-0xb7 load the tables.

type procTexTables struct {
	noise, colorMap, alphaMap [128]uint32 // 12-bit value, 12-bit signed difference
	color, diff               [256]uint32
	idx, sel                  uint32
}

func (p *procTexTables) write(v uint32) {
	i := p.idx & 0xff
	switch p.sel {
	case 0:
		p.noise[i&127] = v
	case 2:
		p.colorMap[i&127] = v
	case 3:
		p.alphaMap[i&127] = v
	case 4:
		p.color[i] = v
	case 5:
		p.diff[i] = v
	}
	p.idx++
}

// procTex is the settings for one triangle.
type procTex struct {
	t              *procTexTables
	cfg            uint32
	ampU, ampV     float32
	phaseU, phaseV float32
	freqU, freqV   float32
	linear         bool
	width, offset  int
	coord          int
}

func (g *gpu) procTexState() *procTex {
	r := &g.reg
	p := &procTex{t: &g.proctex, cfg: r[0x0a8], coord: int(r[0x080] >> 8 & 3)}
	p.ampU = float32(int16(r[0x0a9])) / 4095
	p.ampV = float32(int16(r[0x0aa])) / 4095
	p.phaseU, p.phaseV = f16(r[0x0a9]>>16), f16(r[0x0aa]>>16)
	p.freqU, p.freqV = f16(r[0x0ab]&0xffff), f16(r[0x0ab]>>16)
	lut := r[0x0ac]
	switch lut & 7 {
	case 1, 3, 5:
		p.linear = true
	}
	p.width = int(lut >> 11 & 0xff)
	p.offset = int(r[0x0ad] & 0xff)
	return p
}

func ptValue(e uint32) (float32, float32) {
	d := int32(e<<8) >> 20 // bits 12-23, signed
	return float32(e&0xfff) / 4095, float32(d) / 4095
}

func ptLookup(t *[128]uint32, x float32) float32 {
	x *= 128
	i := min(int(x), 127)
	if i < 0 {
		i = 0
	}
	v, d := ptValue(t[i])
	return v + (x-float32(i))*d
}

func ptRand1(v uint32) uint32 {
	table := [16]uint32{0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11}
	return (v%9+2)*3&0xf ^ table[v/9&0xf]
}

func ptRand2(x, y uint32) float32 {
	table := [16]uint32{10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14}
	u := ptRand1(x)
	v := ptRand1(y)
	if u&3 == 1 {
		v += 4
	}
	v ^= (u & 1) * 6
	v += 10 + u
	v &= 0xf
	v ^= table[u]
	return -1 + float32(v)*2/15
}

func (p *procTex) noise(u, v float32) float32 {
	x := 9 * p.freqU * abs32(u+p.phaseU)
	y := 9 * p.freqV * abs32(v+p.phaseV)
	xi, yi := int(x), int(y)
	xf, yf := x-float32(xi), y-float32(yi)
	g0 := ptRand2(uint32(xi), uint32(yi)) * (xf + yf)
	g1 := ptRand2(uint32(xi+1), uint32(yi)) * (xf + yf - 1)
	g2 := ptRand2(uint32(xi), uint32(yi+1)) * (xf + yf - 1)
	g3 := ptRand2(uint32(xi+1), uint32(yi+1)) * (xf + yf - 2)
	nx := ptLookup(&p.t.noise, xf)
	ny := ptLookup(&p.t.noise, yf)
	top := g0 + (g1-g0)*nx
	bot := g2 + (g3-g2)*nx
	return top + (bot-top)*ny
}

func abs32(f float32) float32 { return float32(math.Abs(float64(f))) }

func ptShift(v float32, mode, clamp uint32) float32 {
	off := float32(0.5)
	if clamp == 3 {
		off = 1
	}
	switch mode {
	case 1:
		return off * float32(int(v)/2%2)
	case 2:
		return off * float32((int(v)+1)/2%2)
	}
	return 0
}

func ptClamp(c float32, mode uint32) float32 {
	switch mode {
	case 0:
		if c > 1 {
			return 0
		}
	case 2:
		return c - float32(math.Floor(float64(c)))
	case 3:
		i := int(c)
		f := c - float32(i)
		if i%2 != 0 {
			return 1 - f
		}
		return f
	case 4:
		if c <= 0.5 {
			return 0
		}
		return 1
	default:
		return min(c, 1)
	}
	return c
}

func ptCombine(u, v float32, op uint32, t *[128]uint32) float32 {
	var f float32
	switch op {
	case 0:
		f = u
	case 1:
		f = u * u
	case 2:
		f = v
	case 3:
		f = v * v
	case 4:
		f = (u + v) / 2
	case 5:
		f = (u*u + v*v) / 2
	case 6:
		f = min(float32(math.Sqrt(float64(u*u+v*v))), 1)
	case 7:
		f = min(u, v)
	case 8:
		f = max(u, v)
	case 9:
		f = min(((u+v)/2+float32(math.Sqrt(float64(u*u+v*v))))/2, 1)
	}
	return ptLookup(t, f)
}

// sample makes the procedural texture's colour at (u, v).
func (p *procTex) sample(u, v float32) [4]uint8 {
	cfg := p.cfg
	uClamp, vClamp := cfg&7, cfg>>3&7
	u, v = abs32(u), abs32(v)
	us := ptShift(v, cfg>>16&3, uClamp)
	vs := ptShift(u, cfg>>18&3, vClamp)
	if cfg>>15&1 != 0 {
		n := p.noise(u, v)
		u, v = abs32(u+n*p.ampU), abs32(v+n*p.ampV)
	}
	u, v = ptClamp(u+us, uClamp), ptClamp(v+vs, vClamp)
	c := ptCombine(u, v, cfg>>6&0xf, &p.t.colorMap)
	idx := float32(p.offset) + c*float32(max(p.width-1, 0))
	var out [4]uint8
	if p.linear {
		i := min(max(int(idx), 0), 255)
		f := idx - float32(i)
		col, d := p.t.color[i], p.t.diff[i]
		for k := 0; k < 4; k++ {
			base := float32(col >> (8 * k) & 0xff)
			diff := float32(int8(d>>(8*k))) * 2
			out[k] = uint8(max(0, min(255, base+f*diff)))
		}
	} else {
		i := min(max(int(idx+0.5), 0), 255)
		col := p.t.color[i]
		out = [4]uint8{uint8(col), uint8(col >> 8), uint8(col >> 16), uint8(col >> 24)}
	}
	if cfg>>14&1 != 0 {
		a := ptCombine(u, v, cfg>>10&0xf, &p.t.alphaMap)
		out[3] = uint8(max(0, min(1, a)) * 255)
	}
	return out
}
