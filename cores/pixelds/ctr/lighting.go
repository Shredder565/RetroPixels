package ctr

import "math"

// Fragment lighting: up to eight lights, each with ambient, diffuse and
// two specular colours, scaled by lookup tables (distribution, Fresnel,
// reflection, spotlight and distance attenuation) read at dot products of
// the normal, view, light and half vectors. The normal is the
// interpolated quaternion turning (0, 0, 1), optionally bent by a normal
// map. It gives the combiners their "fragment primary" (diffuse) and
// "fragment secondary" (specular) colours.

// The lookup tables as the registers number them.
const (
	lutD0 = 0
	lutD1 = 1
	lutFR = 3
	lutRB = 4
	lutRG = 5
	lutRR = 6
	lutSP = 8  // + light
	lutDA = 16 // + light
)

// lutInput: where each table's input selector, abs flag and scale are
// in their registers.
const (
	inD0 = 0
	inD1 = 1
	inSP = 2
	inFR = 3
	inRB = 4
	inRG = 5
	inRR = 6
)

// Which tables each lighting configuration has.
var lutSupport = [9]uint32{
	1<<lutD0 | 1<<lutRR | 1<<inSP<<16 | 1<<31,
	1<<lutFR | 1<<lutRR | 1<<inSP<<16 | 1<<31,
	1<<lutD0 | 1<<lutD1 | 1<<lutRR | 1<<31,
	1<<lutD0 | 1<<lutD1 | 1<<lutFR | 1<<31,
	1<<lutD0 | 1<<lutD1 | 1<<lutRR | 1<<lutRG | 1<<lutRB | 1<<inSP<<16 | 1<<31,
	1<<lutD0 | 1<<lutFR | 1<<lutRR | 1<<lutRG | 1<<lutRB | 1<<inSP<<16 | 1<<31,
	1<<lutD0 | 1<<lutD1 | 1<<lutFR | 1<<lutRR | 1<<inSP<<16 | 1<<31,
	0xffffffff,
	0xffffffff,
}

type light struct {
	spec0, spec1, diffuse, ambient [3]float32
	pos                            [3]float32
	spot                           [3]float32
	directional, twoSided          bool
	geo0, geo1                     bool
	daBias, daScale                float32
	id                             int
	spotOn, daOn                   bool
}

type lightState struct {
	on         bool
	lights     []light
	ambient    [3]float32
	luts       *[24][256]uint32
	cfg0, cfg1 uint32
	abs, sel   uint32
	scale      uint32
	support    uint32
	bumpMode   uint32
	bumpTex    int
	renorm     bool
	clampHigh  bool
	fresnel    uint32
}

func lightColor(v uint32) [3]float32 {
	return [3]float32{float32(v>>20&0xff) / 255, float32(v>>10&0xff) / 255, float32(v&0xff) / 255}
}

// f16 is a half float.
func f16(v uint32) float32 {
	v &= 0xffff
	sign, exp, man := v>>15, v>>10&31, v&0x3ff
	if exp == 0 {
		return math.Float32frombits(sign << 31)
	}
	return math.Float32frombits(sign<<31 | (exp+112)<<23 | man<<13)
}

// f20 is the 20-bit float of the distance attenuation registers (1.7.12).
func f20(v uint32) float32 {
	v &= 0xfffff
	sign, exp, man := v>>19, v>>12&0x7f, v&0xfff
	if exp == 0 {
		return math.Float32frombits(sign << 31)
	}
	return math.Float32frombits(sign<<31 | (exp+64)<<23 | man<<11)
}

func (g *gpu) lightState() *lightState {
	r := &g.reg
	if r[0x08f]&1 == 0 {
		return nil
	}
	ls := &lightState{on: true, luts: &g.luts}
	ls.cfg0, ls.cfg1 = r[0x1c3], r[0x1c4]
	ls.abs, ls.sel, ls.scale = r[0x1d0], r[0x1d1], r[0x1d2]
	ls.ambient = lightColor(r[0x1c0])
	cfg := ls.cfg0 >> 4 & 0xf
	if cfg > 8 {
		cfg = 8
	}
	ls.support = lutSupport[cfg]
	ls.bumpMode = ls.cfg0 >> 28 & 3
	ls.bumpTex = int(ls.cfg0 >> 22 & 3)
	ls.renorm = ls.cfg0>>30&1 == 0
	ls.clampHigh = ls.cfg0>>27&1 != 0
	ls.fresnel = ls.cfg0 >> 2 & 3
	n := int(r[0x1c2]&7) + 1
	perm := r[0x1d9]
	for i := 0; i < n; i++ {
		id := int(perm >> (4 * i) & 7)
		b := 0x140 + uint32(id)*0x10
		l := light{id: id}
		l.spec0, l.spec1 = lightColor(r[b]), lightColor(r[b+1])
		l.diffuse, l.ambient = lightColor(r[b+2]), lightColor(r[b+3])
		l.pos = [3]float32{f16(r[b+4]), f16(r[b+4] >> 16), f16(r[b+5])}
		s13 := func(v uint32) float32 { return float32(int32(v<<19)>>19) / 2047 }
		l.spot = [3]float32{s13(r[b+6]), s13(r[b+6] >> 16), s13(r[b+7])}
		c := r[b+9]
		l.directional = c&1 != 0
		l.twoSided = c>>1&1 != 0
		l.geo0, l.geo1 = c>>2&1 != 0, c>>3&1 != 0
		l.daBias, l.daScale = f20(r[b+0xa]), f20(r[b+0xb])
		l.spotOn = ls.cfg1>>(8+id)&1 == 0 && ls.support&(1<<inSP<<16) != 0
		l.daOn = ls.cfg1>>(24+id)&1 == 0
		ls.lights = append(ls.lights, l)
	}
	return ls
}

// has: the table is in this configuration and not switched off.
func (ls *lightState) has(lut int) bool {
	if ls.support>>lut&1 == 0 {
		return false
	}
	switch lut {
	case lutD0:
		return ls.cfg1>>16&1 == 0
	case lutD1:
		return ls.cfg1>>17&1 == 0
	case lutFR:
		return ls.cfg1>>19&1 == 0
	case lutRB:
		return ls.cfg1>>20&1 == 0
	case lutRG:
		return ls.cfg1>>21&1 == 0
	case lutRR:
		return ls.cfg1>>22&1 == 0
	}
	return true
}

func (ls *lightState) entry(lut int, idx int, delta float32) float32 {
	e := ls.luts[lut][idx&0xff]
	v := float32(e&0xfff) / 4095
	d := float32(int32(e<<8)>>20) / 4095
	return v + d*delta
}

// lookup reads table lut at input slot in (its selector, abs flag and
// scale) with the dot products of this fragment.
func (ls *lightState) lookup(lut, in int, dots *[6]float32) float32 {
	x := dots[ls.sel>>(4*in)&7%6]
	var v float32
	if ls.abs>>(4*in+1)&1 == 0 { // absolute value
		x = float32(math.Abs(float64(x)))
		i := max(0, min(255, int(math.Floor(float64(x*256)))))
		v = ls.entry(lut, i, x*256-float32(i))
	} else {
		i := max(-128, min(127, int(math.Floor(float64(x*128)))))
		v = ls.entry(lut, i&0xff, x*128-float32(i))
	}
	switch ls.scale >> (4 * in) & 7 {
	case 1:
		v *= 2
	case 2:
		v *= 4
	case 3:
		v *= 8
	case 6:
		v /= 4
	case 7:
		v /= 2
	}
	return v
}

type v3 [3]float32

func (a v3) add(b v3) v3        { return v3{a[0] + b[0], a[1] + b[1], a[2] + b[2]} }
func (a v3) scale(s float32) v3 { return v3{a[0] * s, a[1] * s, a[2] * s} }
func (a v3) dot(b v3) float32   { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }
func (a v3) cross(b v3) v3 {
	return v3{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}
func (a v3) norm() v3 {
	l := float32(math.Sqrt(float64(a.dot(a))))
	if l == 0 {
		return a
	}
	return a.scale(1 / l)
}

// rotate turns v by the quaternion q (x, y, z, w).
func rotate(q [4]float32, v v3) v3 {
	u := v3{q[0], q[1], q[2]}
	return v.add(u.cross(u.cross(v).add(v.scale(q[3]))).scale(2))
}

// shade returns the fragment's primary and secondary colours.
func (ls *lightState) shade(quat [4]float32, view v3, tex *[3][4]uint8) (prim, sec [4]uint8) {
	ql := float32(math.Sqrt(float64(quat[0]*quat[0] + quat[1]*quat[1] + quat[2]*quat[2] + quat[3]*quat[3])))
	if ql != 0 {
		for i := range quat {
			quat[i] /= ql
		}
	}
	surface := v3{0, 0, 1}
	if ls.bumpMode == 1 { // normal map
		t := tex[ls.bumpTex]
		surface = v3{float32(t[0])/127.5 - 1, float32(t[1])/127.5 - 1, float32(t[2])/127.5 - 1}
		if ls.renorm {
			surface[2] = float32(math.Sqrt(float64(max(0, 1-surface[0]*surface[0]-surface[1]*surface[1]))))
		}
	}
	normal := rotate(quat, surface)
	nview := view.norm()
	var diff, spec v3
	diffA, specA := float32(1), float32(1)
	for i := range ls.lights {
		l := &ls.lights[i]
		pos := v3(l.pos)
		var lv v3
		if l.directional {
			lv = pos.norm()
		} else {
			lv = pos.add(view).norm()
		}
		half := nview.add(lv)
		hn := half.norm()
		ln := lv.dot(normal)
		dots := [6]float32{normal.dot(hn), nview.dot(hn), normal.dot(nview), ln, lv.dot(v3(l.spot)), 0}
		dist := float32(1)
		if l.daOn {
			d := pos.add(view)
			x := max(0, min(1, l.daScale*float32(math.Sqrt(float64(d.dot(d))))+l.daBias))
			i := max(0, min(255, int(math.Floor(float64(x*256)))))
			dist = ls.entry(lutDA+l.id, i, x*256-float32(i))
		}
		spot := float32(1)
		if l.spotOn {
			spot = ls.lookup(lutSP+l.id, inSP, &dots)
		}
		dp := max(ln, 0)
		if l.twoSided {
			dp = float32(math.Abs(float64(ln)))
		}
		geo := float32(1)
		if l.geo0 || l.geo1 {
			if hh := half.dot(half); hh != 0 {
				geo = max(0, min(1, ln/hh))
			} else {
				geo = 0
			}
		}
		d0 := float32(1)
		if ls.has(lutD0) {
			d0 = ls.lookup(lutD0, inD0, &dots)
		}
		d1 := float32(1)
		if ls.has(lutD1) {
			d1 = ls.lookup(lutD1, inD1, &dots)
		}
		refl := v3{1, 1, 1}
		if ls.has(lutRR) {
			refl[0] = ls.lookup(lutRR, inRR, &dots)
		}
		refl[1], refl[2] = refl[0], refl[0]
		if ls.has(lutRG) {
			refl[1] = ls.lookup(lutRG, inRG, &dots)
		}
		if ls.has(lutRB) {
			refl[2] = ls.lookup(lutRB, inRB, &dots)
		}
		s0 := v3(l.spec0).scale(d0)
		if l.geo0 {
			s0 = s0.scale(geo)
		}
		s1 := v3{l.spec1[0] * refl[0], l.spec1[1] * refl[1], l.spec1[2] * refl[2]}.scale(d1)
		if l.geo1 {
			s1 = s1.scale(geo)
		}
		if ls.has(lutFR) {
			fr := ls.lookup(lutFR, inFR, &dots)
			if ls.fresnel&1 != 0 {
				diffA = fr
			}
			if ls.fresnel&2 != 0 {
				specA = fr
			}
		}
		hl := float32(1)
		if ls.clampHigh && ln <= 0 {
			hl = 0
		}
		att := dist * spot
		diff = diff.add(v3(l.diffuse).scale(dp).add(v3(l.ambient)).scale(att))
		spec = spec.add(s0.add(s1).scale(hl * att))
	}
	diff = diff.add(v3(ls.ambient))
	to8 := func(f float32) uint8 { return uint8(max(0, min(1, f))*255 + 0.5) }
	prim = [4]uint8{to8(diff[0]), to8(diff[1]), to8(diff[2]), to8(diffA)}
	sec = [4]uint8{to8(spec[0]), to8(spec[1]), to8(spec[2]), to8(specA)}
	return
}
