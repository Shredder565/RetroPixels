package snes

import "math"

// DSP-1 and DSP-2 (web/emu/snes_dsp.js): each documented command computed
// directly (high-level emulation), or, when the player supplies the chip's
// program, the real processor (necdsp.go).

const dspTurn = 2 * math.Pi / 65536

// jsToInt32 is JavaScript's ToInt32 (what x|0 and x<<16>>16 start with).
func jsToInt32(x float64) int {
	if math.IsNaN(x) || math.IsInf(x, 0) {
		return 0
	}
	x = math.Trunc(x)
	if x >= -2147483648 && x <= 2147483647 {
		return int(int32(int64(x)))
	}
	m := math.Mod(x, 4294967296)
	if m < 0 {
		m += 4294967296
	}
	return int(int32(uint32(int64(m))))
}

func s16f(x float64) float64 { return float64(int16(jsToInt32(x))) }

func dspSat(v float64) float64 {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return v
}

func dspFix(v float64) float64  { return dspSat(math.Trunc(v)) }
func dspFrac(v float64) float64 { return v / 32768 }

func dsin(a int) float64 { return dspSat(jsRound(32768 * jsSin(float64(s16(a))*dspTurn))) }
func dcos(a int) float64 { return dspSat(jsRound(32768 * jsCos(float64(s16(a))*dspTurn))) }

// pow2 is 2 ** x.
func pow2(x float64) float64 {
	if x == math.Trunc(x) && math.Abs(x) < 1e6 {
		return math.Ldexp(1, int(x))
	}
	return math.Pow(2, x)
}

func dspInv(c, e int) []float64 {
	if c == 0 {
		return []float64{0x7fff, 0x2f}
	}
	r := 1 / (dspFrac(float64(c)) * pow2(float64(e)))
	ex := math.Ceil(jsLog2(math.Abs(r)))
	m := r / pow2(ex)
	if m < 0 && m >= -0.5 {
		m *= 2
		ex--
	}
	if m > 0 && m <= 0.5 {
		m *= 2
		ex--
	}
	co := jsRound(m * 32768)
	if co > 32767 {
		co = 32767
	}
	if co < -32768 {
		co = -32768
	}
	return []float64{co, s16f(ex)}
}

// dspRes: a command's answer, or a request for more parameters.
type dspRes struct {
	vals []float64
	more int
}

type dspHandler func(p []int) dspRes

type dspCore interface {
	// command returns the parameter count and handler, ok false for unknown commands.
	command(cmd int) (need int, h dspHandler, raster, ok bool)
	reset()
	state(s *sio)
}

// ---------- DSP-1 ----------

type dsp1Params struct {
	sinA, cosA, sinZ, cosZ float64
	nx, ny, nz             float64
	gx, gy, gz             float64
	cx, cy, cz             float64
	les, vOff, secZ        float64
}

type dsp1Core struct {
	mat [3][9]int16
	p   dsp1Params
}

func (d *dsp1Core) reset() {
	d.mat = [3][9]int16{}
	d.p = dsp1Params{secZ: 1}
}

func (d *dsp1Core) attitude(M *[9]int16, m, az, ay, ax int) {
	sz, cz := jsSin(float64(az)*dspTurn), jsCos(float64(az)*dspTurn)
	sy, cy := jsSin(float64(ay)*dspTurn), jsCos(float64(ay)*dspTurn)
	sx, cx := jsSin(float64(ax)*dspTurn), jsCos(float64(ax)*dspTurn)
	h := float64(m) / 2
	rows := [9]float64{
		cz * cy, -sz * cy, sy,
		sz*cx + cz*sx*sy, cz*cx - sz*sx*sy, -sx * cy,
		sz*sx - cz*cx*sy, cz*sx + sz*cx*sy, cx * cy,
	}
	for k := range rows {
		M[k] = int16(jsToInt32(dspFix(h * rows[k])))
	}
}

func dspMulVec(M *[9]int16, v [3]int, cols bool) []float64 {
	o := make([]float64, 0, 3)
	for i := 0; i < 3; i++ {
		sum := 0
		for k := 0; k < 3; k++ {
			if cols {
				sum += v[k] * int(M[k*3+i])
			} else {
				sum += v[k] * int(M[i*3+k])
			}
		}
		o = append(o, float64(s16(floorDiv(sum, 32768))))
	}
	return o
}

func dspTurnPt(a, x, y int) (int, int) {
	s, c := int(dsin(a)), int(dcos(a))
	return s16(floorDiv(y*s+x*c, 32768)), s16(floorDiv(y*c-x*s, 32768))
}

func (d *dsp1Core) gyrate(zr, xr, yr, u, f, l int) []float64 {
	sy, cy := jsSin(float64(yr)*dspTurn), jsCos(float64(yr)*dspTurn)
	cx, tx := jsCos(float64(xr)*dspTurn), jsTan(float64(xr)*dspTurn)
	w := float64(u)*cy - float64(f)*sy
	var dz float64
	switch {
	case cx != 0:
		dz = dspFix(w / cx)
	case w > 0:
		dz = 32767
	case w < 0:
		dz = -32767
	}
	dx := math.Trunc(float64(u)*sy + float64(f)*cy)
	dy := dspFix(w * tx)
	return []float64{s16f(float64(zr) + dz), s16f(float64(xr) + dx), s16f(float64(yr) + float64(l) - dy)}
}

func (d *dsp1Core) parameter(fx, fy, fz, lfe, les, aas, azs int) []float64 {
	p := &d.p
	p.sinA, p.cosA, p.sinZ, p.cosZ = dsin(aas), dcos(aas), dsin(azs), dcos(azs)
	sA, cA := dspFrac(p.sinA), dspFrac(p.cosA)
	p.nx = -dspFrac(p.sinZ) * sA
	p.ny = dspFrac(p.sinZ) * cA
	p.nz = dspFrac(p.cosZ)
	ex := s16f(math.Trunc(float64(fx) + float64(lfe)*p.nx))
	ey := s16f(math.Trunc(float64(fy) + float64(lfe)*p.ny))
	ez := s16f(math.Trunc(float64(fz) + float64(lfe)*p.nz))
	p.gx = s16f(math.Trunc(ex - float64(les)*p.nx))
	p.gy = s16f(math.Trunc(ey - float64(les)*p.ny))
	p.gz = s16f(math.Trunc(ez - float64(les)*p.nz))
	p.cz, p.les = ez, float64(les)
	const limit = 0x38c0
	az := max(-limit, min(limit, s16(azs)))
	over := float64(s16(azs)-az) * dspTurn
	sZ := jsSin(float64(az) * dspTurn)
	cZ := jsCos(float64(az)*dspTurn) * jsCos(over)
	vof := 0.0
	if over != 0 {
		vof = dspFix(float64(les) * jsSin(over))
	}
	t := 0.0
	if cZ != 0 {
		t = ez / jsCos(float64(az)*dspTurn)
	}
	p.cx = s16f(math.Trunc(ex + t*sZ*sA))
	p.cy = s16f(math.Trunc(ey - t*sZ*cA))
	p.vOff = dspFix(float64(les) * cZ)
	if tiny := 1e-6; math.Abs(cZ) < tiny {
		p.secZ = 1 / tiny // (at run time, as JavaScript divides)
	} else {
		p.secZ = 1 / cZ
	}
	vva := -32767.0
	if sZ != 0 {
		vva = dspFix(-float64(les) * cZ / sZ)
	}
	return []float64{vof, vva, p.cx, p.cy}
}

func (d *dsp1Core) lineScale(vs int) float64 {
	p := &d.p
	den := p.vOff + float64(int32(jsToInt32(float64(vs)*p.sinZ))>>15)
	if den == 0 || math.IsNaN(den) {
		den = 0.5
	}
	return p.cz / den
}

func (d *dsp1Core) raster(vs int) []float64 {
	p := &d.p
	s := 256 * d.lineScale(vs)
	sA, cA := dspFrac(p.sinA), dspFrac(p.cosA)
	return []float64{dspFix(s * cA), dspFix(-s * sA * p.secZ), dspFix(s * sA), dspFix(s * cA * p.secZ)}
}

func (d *dsp1Core) project(x, y, z int) []float64 {
	p := &d.p
	sA, cA, sZ, cZ := dspFrac(p.sinA), dspFrac(p.cosA), dspFrac(p.sinZ), dspFrac(p.cosZ)
	dx, dy, dz := float64(x)-p.gx, float64(y)-p.gy, float64(z)-p.gz
	depth := p.les - (dx*p.nx + dy*p.ny + dz*p.nz)
	if depth == 0 || math.IsNaN(depth) {
		depth = 1e-9
	}
	k := p.les / depth
	h := (dx*cA + dy*sA) * k
	v := (-dx*cZ*sA + dy*cZ*cA - dz*sZ) * k
	return []float64{dspFix(h), dspFix(v), dspFix(256 * k)}
}

func (d *dsp1Core) target(h, v int) []float64 {
	p := &d.p
	s := d.lineScale(v)
	sA, cA := dspFrac(p.sinA), dspFrac(p.cosA)
	across := dspFix(s * float64(h))
	along := dspFix(s * float64(v) * p.secZ)
	return []float64{
		s16f(p.cx + math.Trunc(across*cA) - math.Trunc(along*sA)),
		s16f(p.cy + math.Trunc(across*sA) + math.Trunc(along*cA)),
	}
}

func ints3(p []int) [3]int { return [3]int{p[0], p[1], p[2]} }

func vals(v ...int) []float64 {
	o := make([]float64, len(v))
	for i, x := range v {
		o[i] = float64(x)
	}
	return o
}

func res(v []float64) dspRes { return dspRes{vals: v} }

func (d *dsp1Core) command(cmd int) (int, dspHandler, bool, bool) {
	A, B, C := &d.mat[0], &d.mat[1], &d.mat[2]
	att := func(M *[9]int16) dspHandler {
		return func(p []int) dspRes { d.attitude(M, p[0], p[1], p[2], p[3]); return res(nil) }
	}
	obj := func(M *[9]int16) dspHandler {
		return func(p []int) dspRes { return res(dspMulVec(M, ints3(p), false)) }
	}
	subj := func(M *[9]int16) dspHandler {
		return func(p []int) dspRes { return res(dspMulVec(M, ints3(p), true)) }
	}
	scal := func(M *[9]int16) dspHandler {
		return func(p []int) dspRes { return res(dspMulVec(M, ints3(p), false)[:1]) }
	}
	mul := func(plus int) dspHandler {
		return func(p []int) dspRes { return res(vals(s16((p[0]*p[1])>>15 + plus))) }
	}
	rng := func(plus int) dspHandler {
		return func(p []int) dspRes {
			x, y, z, r := int64(p[0]), int64(p[1]), int64(p[2]), int64(p[3])
			return res(vals(s16(int(int32(x*x+y*y+z*z-r*r))>>15 + plus)))
		}
	}
	switch cmd {
	case 0x00:
		return 2, mul(0), false, true
	case 0x20:
		return 2, mul(1), false, true
	case 0x10, 0x30:
		return 2, func(p []int) dspRes { return res(dspInv(p[0], p[1])) }, false, true
	case 0x04, 0x24:
		return 2, func(p []int) dspRes {
			a, r := p[0], p[1]
			return res(vals(s16(floorDiv(int(dsin(a))*r, 32768)), s16(floorDiv(int(dcos(a))*r, 32768))))
		}, false, true
	case 0x08:
		return 3, func(p []int) dspRes {
			x, y, z := int64(p[0]), int64(p[1]), int64(p[2])
			v := int(int32((x*x + y*y + z*z) * 2))
			return res(vals(v&0xffff, int(uint32(v)>>16)&0xffff))
		}, false, true
	case 0x18:
		return 4, rng(0), false, true
	case 0x38:
		return 4, rng(1), false, true
	case 0x28:
		return 3, func(p []int) dspRes {
			x, y, z := float64(p[0]), float64(p[1]), float64(p[2])
			return res([]float64{dspFix(math.Sqrt(x*x + y*y + z*z))})
		}, false, true
	case 0x0c, 0x2c:
		return 3, func(p []int) dspRes { x, y := dspTurnPt(p[0], p[1], p[2]); return res(vals(x, y)) }, false, true
	case 0x1c, 0x3c:
		return 6, func(p []int) dspRes {
			x, y, z := p[3], p[4], p[5]
			x, y = dspTurnPt(p[0], x, y)
			z, x = dspTurnPt(p[1], z, x)
			y, z = dspTurnPt(p[2], y, z)
			return res(vals(x, y, z))
		}, false, true
	case 0x01, 0x05, 0x31, 0x35:
		return 4, att(A), false, true
	case 0x11, 0x15:
		return 4, att(B), false, true
	case 0x21, 0x25:
		return 4, att(C), false, true
	case 0x0d, 0x09, 0x39, 0x3d:
		return 3, obj(A), false, true
	case 0x19, 0x1d:
		return 3, obj(B), false, true
	case 0x29, 0x2d:
		return 3, obj(C), false, true
	case 0x03, 0x33:
		return 3, subj(A), false, true
	case 0x13:
		return 3, subj(B), false, true
	case 0x23:
		return 3, subj(C), false, true
	case 0x0b, 0x3b:
		return 3, scal(A), false, true
	case 0x1b:
		return 3, scal(B), false, true
	case 0x2b:
		return 3, scal(C), false, true
	case 0x14, 0x34:
		return 6, func(p []int) dspRes { return res(d.gyrate(p[0], p[1], p[2], p[3], p[4], p[5])) }, false, true
	case 0x02, 0x12, 0x22, 0x32:
		return 7, func(p []int) dspRes { return res(d.parameter(p[0], p[1], p[2], p[3], p[4], p[5], p[6])) }, false, true
	case 0x0a, 0x1a, 0x2a, 0x3a:
		return 1, func(p []int) dspRes { return res(d.raster(p[0])) }, true, true
	case 0x06, 0x16, 0x26, 0x36:
		return 3, func(p []int) dspRes { return res(d.project(p[0], p[1], p[2])) }, false, true
	case 0x0e, 0x1e, 0x2e, 0x3e:
		return 2, func(p []int) dspRes { return res(d.target(p[0], p[1])) }, false, true
	case 0x07, 0x0f, 0x17, 0x27, 0x37:
		return 1, func(p []int) dspRes { return res([]float64{0}) }, false, true
	case 0x2f:
		return 1, func(p []int) dspRes { return res([]float64{0x0100}) }, false, true
	case 0x1f, 0x3f:
		return 1, func(p []int) dspRes { return res(make([]float64, 1024)) }, false, true
	}
	return 0, nil, false, false
}

func (d *dsp1Core) state(s *sio) {
	for i := range d.mat {
		for k := range d.mat[i] {
			v := int(d.mat[i][k])
			s.Int(&v)
			d.mat[i][k] = int16(v)
		}
	}
	p := &d.p
	for _, f := range []*float64{&p.sinA, &p.cosA, &p.sinZ, &p.cosZ, &p.nx, &p.ny, &p.nz, &p.gx, &p.gy, &p.gz,
		&p.cx, &p.cy, &p.cz, &p.les, &p.vOff, &p.secZ} {
		s.F64(f)
	}
	if !s.save && math.IsNaN(p.secZ) {
		p.secZ = 1
	}
}

// ---------- DSP-2 ----------

type dsp2Core struct{ transparent int }

func (d *dsp2Core) reset() { d.transparent = 0 }

func (d *dsp2Core) command(cmd int) (int, dspHandler, bool, bool) {
	switch cmd {
	case 0x01:
		return 32, func(px []int) dspRes {
			t := make([]int, 32)
			for y := 0; y < 8; y++ {
				for x := 0; x < 8; x++ {
					b := px[y*4+x>>1]
					c := b >> 4
					if x&1 != 0 {
						c = b & 15
					}
					bit := 0x80 >> x
					if c&1 != 0 {
						t[y*2] |= bit
					}
					if c&2 != 0 {
						t[y*2+1] |= bit
					}
					if c&4 != 0 {
						t[16+y*2] |= bit
					}
					if c&8 != 0 {
						t[17+y*2] |= bit
					}
				}
			}
			return res(vals(t...))
		}, false, true
	case 0x03:
		return 1, func(p []int) dspRes { d.transparent = p[0] & 15; return res(nil) }, false, true
	case 0x05:
		return 1, func(p []int) dspRes {
			n, rest := p[0], p[1:]
			if len(rest) < 2*n {
				return dspRes{more: 1 + 2*n}
			}
			o := make([]int, 0, n)
			t := d.transparent
			for i := 0; i < n; i++ {
				a, b := rest[i], rest[n+i]
				hi, lo := b&0xf0, b&15
				if b>>4 == t {
					hi = a & 0xf0
				}
				if b&15 == t {
					lo = a & 15
				}
				o = append(o, hi|lo)
			}
			return res(vals(o...))
		}, false, true
	case 0x06:
		return 1, func(p []int) dspRes {
			n, rest := p[0], p[1:]
			if len(rest) < n {
				return dspRes{more: 1 + n}
			}
			o := make([]int, n)
			for i := 0; i < n; i++ {
				b := rest[i]
				o[n-1-i] = (b<<4 | b>>4) & 0xff
			}
			return res(vals(o...))
		}, false, true
	case 0x07:
		return 4, func(p []int) dspRes {
			r := ((p[0] | p[1]<<8) + (p[2] | p[3]<<8)) & 0xffff
			return res(vals(r&0xff, r>>8))
		}, false, true
	case 0x08:
		return 4, func(p []int) dspRes {
			r := ((p[0] | p[1]<<8) - (p[2] | p[3]<<8)) & 0xffff
			return res(vals(r&0xff, r>>8))
		}, false, true
	case 0x09:
		return 4, func(p []int) dspRes {
			r := uint32((p[0] | p[1]<<8) * (p[2] | p[3]<<8))
			return res(vals(int(r&0xff), int(r>>8&0xff), int(r>>16&0xff), int(r>>24)))
		}, false, true
	case 0x0d:
		return 2, func(p []int) dspRes {
			outW, inW, rest := p[0], p[1], p[2:]
			nIn := (inW + 1) >> 1
			if len(rest) < nIn {
				return dspRes{more: 2 + nIn}
			}
			o := make([]int, (outW+1)>>1)
			for i := 0; i < outW; i++ {
				s := i * inW / outW
				b := 0
				if s>>1 < len(rest) {
					b = rest[s>>1]
				}
				c := b >> 4
				if s&1 != 0 {
					c = b & 15
				}
				if i&1 != 0 {
					o[i>>1] |= c
				} else {
					o[i>>1] |= c << 4
				}
			}
			return res(vals(o...))
		}, false, true
	}
	return 0, func(p []int) dspRes { return res(nil) }, false, true
}

func (d *dsp2Core) state(s *sio) { s.Int(&d.transparent) }

// ---------- the DR / SR port ----------

type hlePort struct {
	core    dspCore
	words   bool
	cmd     int
	need    int
	params  []int
	partial int
	out     []int
	outPos  int
	handler dspHandler
	raster  bool
	vs      int
}

func (p *hlePort) reset() {
	p.cmd, p.need, p.params, p.partial = -1, 0, nil, -1
	p.out, p.outPos, p.handler, p.raster, p.vs = nil, 0, nil, false, 0
}

func (p *hlePort) writeDR(v int) {
	if p.cmd < 0 || p.outPos < len(p.out) {
		p.startCommand(v)
		return
	}
	if p.words {
		if p.partial < 0 {
			p.partial = v
			return
		}
		p.params = append(p.params, s16(p.partial|v<<8))
		p.partial = -1
	} else {
		p.params = append(p.params, v)
	}
	p.more()
}

func (p *hlePort) startCommand(v int) {
	p.out, p.outPos, p.params, p.partial, p.raster = nil, 0, nil, -1, false
	need, h, raster, ok := p.core.command(v)
	if !ok {
		p.cmd = -1
		return
	}
	p.cmd, p.need, p.handler, p.raster = v, need, h, raster
	p.more()
}

func (p *hlePort) more() {
	if len(p.params) < p.need {
		return
	}
	r := p.handler(p.params)
	if r.more != 0 {
		p.need = r.more
		return
	}
	if p.raster {
		p.vs = p.params[0]
	}
	p.setOut(r.vals)
	if len(p.out) == 0 && !p.raster {
		p.cmd = -1
	}
}

func (p *hlePort) setOut(v []float64) {
	o := make([]int, 0, 2*len(v))
	for _, w := range v {
		x := jsToInt32(w)
		if p.words {
			o = append(o, x&0xff, (x>>8)&0xff)
		} else {
			o = append(o, x&0xff)
		}
	}
	p.out, p.outPos = o, 0
}

func (p *hlePort) readDR() int {
	if p.outPos >= len(p.out) {
		return 0x80
	}
	v := p.out[p.outPos]
	p.outPos++
	if p.outPos >= len(p.out) {
		if p.raster {
			p.vs = s16(p.vs + 1)
			p.setOut(p.handler([]int{p.vs}).vals)
		} else {
			p.cmd = -1
		}
	}
	return v
}

func (p *hlePort) readSR() int { return 0x80 }

func (p *hlePort) state(s *sio) {
	s.Ints(&p.cmd, &p.need)
	s.IntVar(&p.params)
	s.Int(&p.partial)
	s.IntVar(&p.out)
	s.Ints(&p.outPos, &p.vs)
	if !s.save {
		p.handler, p.raster = nil, false
		if p.cmd >= 0 {
			if _, h, raster, ok := p.core.command(p.cmd); ok {
				p.handler, p.raster = h, raster
			}
		}
	}
}

// ---------- cartridge glue ----------

type dspPort interface {
	readDR() int
	readSR() int
	writeDR(v int)
	reset()
	state(s *sio)
}

type dspChip struct {
	core                       dspCore // nil with the real processor
	port                       dspPort
	b0, b1, dr0, dr1, sr0, sr1 int
}

func newDSPChip(s *SNES, kind int, o *chipOpts) (coproc, error) {
	c := &dspChip{}
	name := "DSP-1"
	if kind == 2 {
		name = "DSP-2"
	}
	fw, err := findFirmware(s, o, name)
	if err != nil {
		return nil, err
	}
	if fw != nil {
		c.port = newNECClocked(s, newNECDSP(upd7725, fw), 7600000)
	} else {
		if kind == 2 {
			c.core = &dsp2Core{}
		} else {
			c.core = &dsp1Core{}
		}
		c.core.reset()
		hp := &hlePort{core: c.core, words: kind != 2}
		hp.reset()
		c.port = hp
	}
	cart := s.cart
	switch {
	case kind == 2:
		c.b0, c.b1, c.dr0, c.dr1, c.sr0, c.sr1 = 0x20, 0x3f, 0x8000, 0xbfff, 0xc000, 0xffff
	case cart.mapping != mapLo:
		c.b0, c.b1, c.dr0, c.dr1, c.sr0, c.sr1 = 0x00, 0x1f, 0x6000, 0x6fff, 0x7000, 0x7fff
	case cart.size > 0x100000:
		c.b0, c.b1, c.dr0, c.dr1, c.sr0, c.sr1 = 0x60, 0x6f, 0x0000, 0x3fff, 0x4000, 0x7fff
	default:
		c.b0, c.b1, c.dr0, c.dr1, c.sr0, c.sr1 = 0x20, 0x3f, 0x8000, 0xbfff, 0xc000, 0xffff
	}
	s.claim(c.b0, c.b1, c.dr0, c.sr1, true)
	return c, nil
}

func (c *dspChip) hit(bank, off int) bool {
	b := bank & 0x7f
	return b >= c.b0 && b <= c.b1 && off >= c.dr0 && off <= c.sr1
}

func (c *dspChip) read(bank, off int) int {
	if !c.hit(bank, off) {
		return -1
	}
	if off <= c.dr1 {
		return c.port.readDR()
	}
	return c.port.readSR()
}

func (c *dspChip) write(bank, off int, v uint8) bool {
	if !c.hit(bank, off) {
		return false
	}
	if off <= c.dr1 {
		c.port.writeDR(int(v))
	}
	return true
}

func (c *dspChip) reset() {
	if c.core != nil {
		c.core.reset()
	}
	c.port.reset()
}

func (c *dspChip) state(s *sio) {
	if c.core != nil {
		c.core.state(s)
	}
	c.port.state(s)
}
