package ps3

// The RSX vertex program: 128-bit instructions, each with a vector and a
// scalar operation, run here by an interpreter over decoded instructions.

import "math"

type vec4 [4]float32

type vpSrc struct {
	typ  uint8 // 1 temp, 2 input, 3 constant
	tmp  uint8
	swz  [4]uint8
	neg  bool
	abs  bool
	noop bool // identity swizzle, no neg/abs
}

type vpInst struct {
	vecOp, scaOp uint8
	src          [3]vpSrc
	input        uint8
	constant     uint16
	indexConst   bool
	addrSwz      uint8
	addrSel      uint8
	cond         uint8
	condTest     bool
	condSwz      [4]uint8
	condSel      uint8
	dstTmp       uint8
	scaDstTmp    uint8
	vecResult    bool
	dst          uint8
	sat          bool
	vecMask      [4]bool
	scaMask      [4]bool
	end          bool
	target       int // branch target
	brbIndex     uint8
	brbTrue      bool
	texNum       uint8
}

func bitsOf(v uint32, pos, n uint) uint32 { return v >> pos & (1<<n - 1) }

func decodeVPSrc(s uint32, abs bool) vpSrc {
	r := vpSrc{typ: uint8(s & 3), tmp: uint8(s >> 2 & 0x3f), neg: s>>16&1 != 0, abs: abs}
	r.swz = [4]uint8{uint8(s >> 14 & 3), uint8(s >> 12 & 3), uint8(s >> 10 & 3), uint8(s >> 8 & 3)}
	r.noop = r.swz == [4]uint8{0, 1, 2, 3} && !r.neg && !abs
	return r
}

func decodeVP(w [4]uint32) vpInst {
	d0, d1, d2, d3 := w[0], w[1], w[2], w[3]
	in := vpInst{
		vecOp:      uint8(bitsOf(d1, 22, 5)),
		scaOp:      uint8(bitsOf(d1, 27, 5)),
		input:      uint8(bitsOf(d1, 8, 4)),
		constant:   uint16(bitsOf(d1, 12, 10)),
		indexConst: d3>>1&1 != 0,
		addrSwz:    uint8(d0 & 3),
		addrSel:    uint8(d0 >> 24 & 1),
		cond:       uint8(bitsOf(d0, 10, 3)),
		condTest:   d0>>13&1 != 0,
		condSel:    uint8(d0 >> 25 & 1),
		dstTmp:     uint8(bitsOf(d0, 15, 6)),
		scaDstTmp:  uint8(bitsOf(d3, 7, 6)),
		vecResult:  d0>>30&1 != 0,
		dst:        uint8(bitsOf(d3, 2, 5)),
		sat:        d0>>26&1 != 0,
		end:        d3&1 != 0,
		texNum:     uint8(bitsOf(d2, 8, 2)),
		brbIndex:   uint8(bitsOf(d3, 23, 5)),
		brbTrue:    d3>>28&1 != 0,
	}
	in.condSwz = [4]uint8{uint8(d0 >> 8 & 3), uint8(d0 >> 6 & 3), uint8(d0 >> 4 & 3), uint8(d0 >> 2 & 3)}
	in.vecMask = [4]bool{d3>>16&1 != 0, d3>>15&1 != 0, d3>>14&1 != 0, d3>>13&1 != 0}
	in.scaMask = [4]bool{d3>>20&1 != 0, d3>>19&1 != 0, d3>>18&1 != 0, d3>>17&1 != 0}
	src0 := bitsOf(d1, 0, 8)<<9 | bitsOf(d2, 23, 9)
	src1 := bitsOf(d2, 6, 17)
	src2 := bitsOf(d2, 0, 6)<<11 | bitsOf(d3, 21, 11)
	in.src[0] = decodeVPSrc(src0, d0>>21&1 != 0)
	in.src[1] = decodeVPSrc(src1, d0>>22&1 != 0)
	in.src[2] = decodeVPSrc(src2, d0>>23&1 != 0)
	in.target = int(d0>>23&1)<<9 | int(bitsOf(d2, 0, 6))<<3 | int(bitsOf(d3, 29, 3))
	return in
}

type vpRun struct {
	tmp [64]vec4
	a   [2][4]int32
	cc  [2]vec4
	out [16]vec4
}

func swizzle(v vec4, s [4]uint8) vec4 { return vec4{v[s[0]], v[s[1]], v[s[2]], v[s[3]]} }

func (r *rsxDraw) vpRead(x *vpRun, in *vpInst, n int, attrs *[16]vec4) vec4 {
	s := &in.src[n]
	var v vec4
	switch s.typ {
	case 1:
		v = x.tmp[s.tmp]
	case 2:
		v = attrs[in.input]
	case 3:
		if in.indexConst {
			v = r.vc[(int32(in.constant)+x.a[in.addrSel][in.addrSwz])&511]
		} else if in.constant < 468 {
			v = r.vc[in.constant]
		}
	}
	if s.noop {
		return v
	}
	v = swizzle(v, s.swz)
	if s.abs {
		for i := range v {
			v[i] = float32(math.Abs(float64(v[i])))
		}
	}
	if s.neg {
		for i := range v {
			v[i] = -v[i]
		}
	}
	return v
}

func testCond(c vec4, mode uint8) [4]bool {
	var r [4]bool
	for i, x := range c {
		switch mode {
		case 7:
			r[i] = true
		case 6: // gt|eq
			r[i] = x >= 0
		case 3: // lt|eq
			r[i] = x <= 0
		case 5: // lt|gt
			r[i] = x != 0
		case 4:
			r[i] = x > 0
		case 1:
			r[i] = x < 0
		case 2:
			r[i] = x == 0
		}
	}
	return r
}

func sat(v vec4) vec4 {
	for i, x := range v {
		if !(x > 0) { // NaN and -0 too
			v[i] = 0
		} else if x > 1 {
			v[i] = 1
		}
	}
	return v
}

func movMask(d *vec4, v vec4, m [4]bool) {
	for i := range v {
		if m[i] {
			d[i] = v[i]
		}
	}
}

func (r *rsxDraw) vpWriteVec(x *vpRun, in *vpInst, v vec4) {
	if in.sat {
		v = sat(v)
	}
	mask := in.vecMask
	if in.condTest {
		t := testCond(swizzle(x.cc[in.condSel], in.condSwz), in.cond)
		for i := range mask {
			mask[i] = mask[i] && t[i]
		}
	}
	if in.dstTmp != 0x3f {
		movMask(&x.tmp[in.dstTmp], v, mask)
		if !in.vecResult {
			return
		}
	}
	if in.vecResult {
		if in.dst < 16 {
			movMask(&x.out[in.dst], v, mask)
		}
		return
	}
	movMask(&x.cc[in.condSel], v, mask)
}

func (r *rsxDraw) vpWriteSca(x *vpRun, in *vpInst, v vec4) {
	if in.sat {
		v = sat(v)
	}
	mask := in.scaMask
	if in.condTest {
		t := testCond(swizzle(x.cc[in.condSel], in.condSwz), in.cond)
		for i := range mask {
			mask[i] = mask[i] && t[i]
		}
	}
	if in.scaDstTmp != 0x3f {
		movMask(&x.tmp[in.scaDstTmp], v, mask)
		return
	}
	if !in.vecResult {
		if in.dst < 16 {
			movMask(&x.out[in.dst], v, mask)
		}
		return
	}
	movMask(&x.cc[in.condSel], v, mask)
}

func dot(a, b vec4, n int) float32 {
	var s float32
	for i := 0; i < n; i++ {
		s += a[i] * b[i]
	}
	return s
}

func b2f(b bool) float32 {
	if b {
		return 1
	}
	return 0
}

func cmpVec(a, b vec4, f func(x, y float32) bool) vec4 {
	return vec4{b2f(f(a[0], b[0])), b2f(f(a[1], b[1])), b2f(f(a[2], b[2])), b2f(f(a[3], b[3]))}
}

func lit(s vec4) vec4 {
	r := vec4{1, 0, 0, 1}
	if s[0] > 0 {
		r[1] = s[0]
		if s[1] > 0 {
			p := s[3]
			if p < -127.9961 {
				p = -127.9961
			} else if p > 127.9961 {
				p = 127.9961
			}
			r[2] = float32(math.Pow(float64(s[1]), float64(p)))
		}
	}
	return r
}

// runVP runs the vertex program for one vertex.
func (r *rsxDraw) runVP(prog []vpInst, start int, attrs *[16]vec4, x *vpRun) {
	for i := range x.out {
		x.out[i] = vec4{0, 0, 0, 1}
	}
	x.a = [2][4]int32{}
	x.cc = [2]vec4{}
	var stack [8]int
	sp := 0
	pc := start
	for steps := 0; pc >= 0 && pc < len(prog) && steps < 20000; steps++ {
		in := &prog[pc]
		pc++
		vop, sop := in.vecOp, in.scaOp
		if in.condTest && in.cond == 0 {
			vop, sop = 0, 0
		}
		if vop == 0x0d { // ARL
			v := r.vpRead(x, in, 0, attrs)
			for i := range v {
				x.a[in.dstTmp&1][i] = int32(math.Floor(float64(v[i])))
			}
		} else if vop != 0 {
			v := r.vpRead(x, in, 0, attrs)
			switch vop {
			case 0x01:
			case 0x02:
				b := r.vpRead(x, in, 1, attrs)
				for i := range v {
					v[i] *= b[i]
				}
			case 0x03:
				b := r.vpRead(x, in, 2, attrs)
				for i := range v {
					v[i] += b[i]
				}
			case 0x04:
				b, c := r.vpRead(x, in, 1, attrs), r.vpRead(x, in, 2, attrs)
				for i := range v {
					v[i] = v[i]*b[i] + c[i]
				}
			case 0x05:
				d := dot(v, r.vpRead(x, in, 1, attrs), 3)
				v = vec4{d, d, d, d}
			case 0x06:
				b := r.vpRead(x, in, 1, attrs)
				d := dot(v, b, 3) + b[3]
				v = vec4{d, d, d, d}
			case 0x07:
				d := dot(v, r.vpRead(x, in, 1, attrs), 4)
				v = vec4{d, d, d, d}
			case 0x08:
				b := r.vpRead(x, in, 1, attrs)
				v = vec4{1, v[1] * b[1], v[2], b[3]}
			case 0x09:
				b := r.vpRead(x, in, 1, attrs)
				for i := range v {
					v[i] = min(v[i], b[i])
				}
			case 0x0a:
				b := r.vpRead(x, in, 1, attrs)
				for i := range v {
					v[i] = max(v[i], b[i])
				}
			case 0x0b:
				v = cmpVec(v, r.vpRead(x, in, 1, attrs), func(a, b float32) bool { return a < b })
			case 0x0c:
				v = cmpVec(v, r.vpRead(x, in, 1, attrs), func(a, b float32) bool { return a >= b })
			case 0x0e:
				for i := range v {
					v[i] -= float32(math.Floor(float64(v[i])))
				}
			case 0x0f:
				for i := range v {
					v[i] = float32(math.Floor(float64(v[i])))
				}
			case 0x10:
				v = cmpVec(v, r.vpRead(x, in, 1, attrs), func(a, b float32) bool { return a == b })
			case 0x11:
				v = vec4{}
			case 0x12:
				v = cmpVec(v, r.vpRead(x, in, 1, attrs), func(a, b float32) bool { return a > b })
			case 0x13:
				v = cmpVec(v, r.vpRead(x, in, 1, attrs), func(a, b float32) bool { return a <= b })
			case 0x14:
				v = cmpVec(v, r.vpRead(x, in, 1, attrs), func(a, b float32) bool { return a != b })
			case 0x15:
				v = vec4{1, 1, 1, 1}
			case 0x16:
				for i := range v {
					switch {
					case v[i] > 0:
						v[i] = 1
					case v[i] < 0:
						v[i] = -1
					}
				}
			case 0x19: // TXL: vertex textures aren't used here
				v = vec4{}
			}
			r.vpWriteVec(x, in, v)
		}
		if sop == 0x07 {
			r.vpWriteSca(x, in, lit(r.vpRead(x, in, 2, attrs)))
		} else if sop != 0 {
			s := r.vpRead(x, in, 2, attrs)[0]
			dyn := func() bool {
				if in.cond == 7 {
					return true
				}
				if in.cond == 0 {
					return false
				}
				t := testCond(swizzle(x.cc[in.condSel], in.condSwz), in.cond)
				return t[0] || t[1] || t[2] || t[3]
			}
			static := func() bool {
				return (r.branchBits>>in.brbIndex&1 != 0) == in.brbTrue
			}
			write := true
			switch sop {
			case 0x01:
			case 0x02:
				s = 1 / s
			case 0x03:
				s = 1 / s
				if s < 5.42101e-20 && s > -5.42101e-20 {
					s = float32(math.Copysign(5.42101e-20, float64(s)))
				} else if s > 1.884467e19 {
					s = 1.884467e19
				} else if s < -1.884467e19 {
					s = -1.884467e19
				}
			case 0x04:
				s = float32(1 / math.Sqrt(math.Abs(float64(s))))
			case 0x05:
				s = float32(math.Exp2(float64(s)))
			case 0x06:
				s = float32(math.Log2(math.Abs(float64(s))))
			case 0x0d:
				s = float32(math.Log2(float64(s)))
			case 0x0e:
				s = float32(math.Exp2(float64(s)))
			case 0x0f:
				s = float32(math.Sin(float64(s)))
			case 0x10:
				s = float32(math.Cos(float64(s)))
			case 0x08: // BRA (address register)
				if dyn() {
					pc = int(x.a[in.addrSel][0])
				}
				write = false
			case 0x09: // BRI
				if dyn() {
					pc = in.target
				}
				write = false
			case 0x0a: // CAL
				if dyn() {
					if sp == len(stack) {
						return
					}
					stack[sp] = pc
					sp++
					pc = in.target
				}
				write = false
			case 0x0c: // RET
				if dyn() {
					if sp == 0 {
						return
					}
					sp--
					pc = stack[sp]
				}
				write = false
			case 0x11: // BRB
				if static() {
					pc = in.target
				}
				write = false
			case 0x12: // CLB
				if static() {
					if sp == len(stack) {
						return
					}
					stack[sp] = pc
					sp++
					pc = in.target
				}
				write = false
			default:
				write = false
			}
			if write {
				r.vpWriteSca(x, in, vec4{s, s, s, s})
			}
		}
		if in.end {
			break
		}
	}
}
