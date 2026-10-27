package ps3

// The RSX fragment program: 128-bit instructions (with their halfwords
// swapped in memory), optionally followed by a 128-bit constant. Decoded
// once per program and run per pixel.

import (
	"math"
	"math/bits"
)

type fpSrc struct {
	typ  uint8 // 0 temp, 1 input, 2 constant
	tmp  uint8
	half bool
	swz  [4]uint8
	neg  bool
	abs  bool
	prec uint8 // precision modifier: 2 clamp to ±2, 3 to ±1, 4 saturate
	noop bool
}

type fpInst struct {
	op       uint8
	flow     bool
	end      bool
	dst      uint8
	dstHalf  bool
	setCond  bool
	mask     [4]bool
	input    uint8
	tex      uint8
	bx2      bool
	noDest   bool
	sat      bool
	prec     uint8 // destination precision: 1 half, 2 fixed ±2, 3 fixed ±1
	scale    float32
	cond     uint8 // exec_if bits (7 = always)
	condSwz  [4]uint8
	condReg  uint8 // which CC is tested
	condSet  uint8 // which CC set_cond writes
	src      [3]fpSrc
	constant vec4
	cidx     int // which of the program's constants (-1 none)
	// flow control
	elseAt, endAt int
	loopStart     int
	loopEnd       int
	loopInc       int
}

type fpProg struct {
	inst      []fpInst
	consts    int    // how many inline constants
	shape     uint64 // hash of the instructions without their constants
	size      uint32 // bytes, constants included
	inputs    uint32 // bit per input register used
	usesTex   uint32
	usesDeriv bool
	kills     bool
}

var fpScale = [8]float32{1, 2, 4, 8, 1, 0.5, 0.25, 0.125}

func fpSwap(w uint32) uint32 { return bits.RotateLeft32(w, 16) }

func decodeFPSrc(w uint32, abs bool, prec uint32) fpSrc {
	s := fpSrc{typ: uint8(w & 3), tmp: uint8(w >> 2 & 0x3f), half: w>>8&1 != 0, neg: w>>17&1 != 0, abs: abs}
	if prec >= 2 && prec <= 4 {
		s.prec = uint8(prec)
	}
	s.swz = [4]uint8{uint8(w >> 9 & 3), uint8(w >> 11 & 3), uint8(w >> 13 & 3), uint8(w >> 15 & 3)}
	s.noop = s.swz == [4]uint8{0, 1, 2, 3} && !s.neg && !s.abs && s.prec == 0
	return s
}

// decodeFP reads a fragment program at a (stops at the end bit).
func (m *Machine) decodeFP(a uint32) *fpProg { return decodeFPFrom(m.Mem.Read32, a) }

func decodeFPFrom(read32 func(uint32) uint32, a uint32) *fpProg {
	p := &fpProg{}
	start := a
	slotOf := map[int]int{} // 16-byte slot -> instruction index
	slot := 0
	h := uint64(14695981039346656037)
	for i := 0; i < 4096; i++ {
		var w [4]uint32
		for k := range w {
			w[k] = fpSwap(read32(a + uint32(k)*4))
			h = (h ^ uint64(w[k])) * 1099511628211
		}
		a += 16
		slotOf[slot] = len(p.inst)
		slot++
		in := fpInst{
			op:      uint8(w[0]>>24&0x3f) | uint8(w[2]>>31)<<6,
			flow:    w[2]>>31 != 0,
			end:     w[0]&1 != 0,
			dst:     uint8(w[0] >> 1 & 0x3f),
			dstHalf: w[0]>>7&1 != 0,
			setCond: w[0]>>8&1 != 0,
			mask:    [4]bool{w[0]>>9&1 != 0, w[0]>>10&1 != 0, w[0]>>11&1 != 0, w[0]>>12&1 != 0},
			input:   uint8(w[0] >> 13 & 0xf),
			tex:     uint8(w[0] >> 17 & 0xf),
			bx2:     w[0]>>21&1 != 0,
			noDest:  w[0]>>30&1 != 0,
			sat:     w[0]>>31 != 0,
			prec:    uint8(w[0] >> 22 & 3),
			scale:   fpScale[w[2]>>28&7],
			cond:    uint8(w[1] >> 18 & 7),
			condReg: uint8(w[1] >> 31),
			condSet: uint8(w[1] >> 30 & 1),
		}
		in.condSwz = [4]uint8{uint8(w[1] >> 21 & 3), uint8(w[1] >> 23 & 3), uint8(w[1] >> 25 & 3), uint8(w[1] >> 27 & 3)}
		in.cidx = -1
		if in.flow {
			in.elseAt = int(w[2]&0x7fffffff) >> 2
			in.endAt = int(w[3] >> 2)
			in.loopEnd = int(w[2] >> 2 & 0xff)
			in.loopStart = int(w[2] >> 10 & 0xff)
			in.loopInc = int(w[2] >> 19 & 0xff)
			in.flow = true
			p.inst = append(p.inst, in)
			if in.end {
				break
			}
			continue
		}
		in.src[0] = decodeFPSrc(w[1], w[1]>>29&1 != 0, w[2]>>19&7)
		in.src[1] = decodeFPSrc(w[2], w[2]>>18&1 != 0, w[2]>>22&7)
		in.src[2] = decodeFPSrc(w[3], w[3]>>18&1 != 0, w[2]>>25&7)
		hasConst := false
		for k := range in.src {
			if in.src[k].typ == 2 && fpReads(in.op) > k {
				hasConst = true
			}
			if in.src[k].typ == 1 && fpReads(in.op) > k {
				p.inputs |= 1 << in.input
			}
		}
		if hasConst {
			for k := range in.constant {
				in.constant[k] = math.Float32frombits(fpSwap(read32(a + uint32(k)*4)))
			}
			a += 16
			slot++
			in.cidx = p.consts
			p.consts++
		}
		switch in.op {
		case 0x17, 0x18, 0x19, 0x2f, 0x31:
			p.usesTex |= 1 << in.tex
		case 0x15, 0x16:
			p.usesDeriv = true
		case 0x12:
			p.kills = true
		}
		p.inst = append(p.inst, in)
		if in.end {
			break
		}
	}
	// branch targets are 16-byte slots (constants count): make them
	// instruction indices
	at := func(sl int) int {
		if i, ok := slotOf[sl]; ok {
			return i
		}
		return len(p.inst)
	}
	for i := range p.inst {
		if in := &p.inst[i]; in.flow {
			in.elseAt, in.endAt = at(in.elseAt), at(in.endAt)
		}
	}
	p.shape = h
	p.size = a - start
	return p
}

// fpReads is how many sources an opcode reads.
func fpReads(op uint8) int {
	switch op {
	case 0x00, 0x12, 0x3d, 0x3e:
		return 0
	case 0x01, 0x10, 0x11, 0x15, 0x16, 0x1a, 0x1b, 0x1c, 0x1d, 0x20, 0x21, 0x22, 0x23, 0x39, 0x1e, 0x3c,
		0x17, 0x18, 0x24, 0x13, 0x29, 0x2c, 0x27, 0x25, 0x14, 0x2a, 0x2d, 0x28:
		return 1
	case 0x04, 0x1f, 0x2e:
		return 3
	}
	return 2
}

// fpRun is one pixel's registers.
type fpRun struct {
	r      [48]vec4
	h      [48]vec4
	cc     [2]vec4
	in     *[16]vec4 // interpolated inputs (by FP input number)
	killed bool
}

func (x *fpRun) read(in *fpInst, n int) vec4 {
	s := &in.src[n]
	var v vec4
	switch s.typ {
	case 0:
		if s.half {
			v = x.h[s.tmp]
		} else {
			v = x.r[s.tmp]
		}
	case 1:
		v = x.in[in.input]
	case 2:
		v = in.constant
	}
	if s.noop {
		return v
	}
	v = swizzle(v, s.swz)
	if s.abs {
		for i := range v {
			v[i] = math.Float32frombits(math.Float32bits(v[i]) &^ 0x80000000)
		}
	}
	if s.prec != 0 {
		v = fpClamp(v, s.prec)
	}
	if s.neg {
		for i := range v {
			v[i] = -v[i]
		}
	}
	return v
}

func (x *fpRun) condOK(in *fpInst) bool {
	if in.cond == 7 {
		return true
	}
	t := testCond(swizzle(x.cc[in.condReg], in.condSwz), condMode(in.cond))
	return t[0] || t[1] || t[2] || t[3]
}

// condMode turns FP exec bits (lt 1, eq 2, gt 4) into the VP form.
func condMode(c uint8) uint8 { return c }

func (x *fpRun) write(in *fpInst, v vec4) {
	if in.scale != 1 {
		for i := range v {
			v[i] *= in.scale
		}
	}
	if in.sat {
		v = sat(v)
	} else if in.prec != 0 {
		v = fpClamp(v, in.prec)
	}
	if in.dstHalf {
		v = fpClamp(v, 1)
	}
	mask := in.mask
	if in.cond != 7 {
		t := testCond(swizzle(x.cc[in.condReg], in.condSwz), in.cond)
		for i := range mask {
			mask[i] = mask[i] && t[i]
		}
	}
	if in.setCond {
		movMask(&x.cc[in.condSet], v, mask)
	}
	if in.noDest {
		return
	}
	if in.dstHalf {
		movMask(&x.h[in.dst&47], v, mask)
	} else {
		movMask(&x.r[in.dst&47], v, mask)
	}
}

// fpClamp applies a precision: 1 half range, 2 fixed12 (±2), 3 fixed9
// (±1), 4 saturate.
func fpClamp(v vec4, prec uint8) vec4 {
	lo, hi := float32(-65504), float32(65504)
	switch prec {
	case 2:
		lo, hi = -2, 2
	case 3:
		lo, hi = -1, 1
	case 4:
		lo, hi = 0, 1
	}
	for i := range v {
		if v[i] != v[i] {
			v[i] = 0
		} else {
			v[i] = max(lo, min(hi, v[i]))
		}
	}
	return v
}

func f32(x float64) float32 { return float32(x) }

// runFP runs the program for one pixel; tex samples texture unit t.
func (p *fpProg) run(x *fpRun, tex func(unit uint8, c vec4, bias float32, lod float32, useLod bool) vec4) {
	// open blocks: ifs (and their elses) and loops, innermost last
	type block struct {
		loop          bool
		inIf          bool // running the if part (skip the else part)
		elseAt, endAt int
		start, count  int
	}
	var st []block
	ip := 0
	for steps := 0; ip >= 0 && ip < len(p.inst) && steps < 65536; steps++ {
		if n := len(st); n > 0 {
			b := &st[n-1]
			if !b.loop && b.inIf && ip == b.elseAt && b.elseAt < b.endAt {
				ip = b.endAt
				st = st[:n-1]
				continue
			}
			if ip == b.endAt {
				if b.loop && b.count > 0 {
					b.count--
					ip = b.start
				} else {
					st = st[:n-1]
					continue
				}
			}
		}
		in := &p.inst[ip]
		ip++
		if in.flow {
			switch in.op {
			case 0x45: // RET
				if x.condOK(in) {
					return
				}
			case 0x42: // IFE
				end := in.endAt
				if end < ip {
					end = len(p.inst)
				}
				if x.condOK(in) {
					st = append(st, block{inIf: true, elseAt: in.elseAt, endAt: end})
				} else if in.elseAt < end {
					ip = in.elseAt
					st = append(st, block{elseAt: in.elseAt, endAt: end})
				} else {
					ip = end
				}
			case 0x43, 0x44: // LOOP, REP
				end := in.endAt
				if end < ip {
					end = len(p.inst)
				}
				if x.condOK(in) && in.loopEnd > in.loopStart {
					st = append(st, block{loop: true, endAt: end, start: ip,
						count: (in.loopEnd - in.loopStart - 1) / max(in.loopInc, 1)})
				} else {
					ip = end
				}
			case 0x40: // BRK
				if x.condOK(in) {
					for k := len(st) - 1; k >= 0; k-- {
						if st[k].loop {
							ip = st[k].endAt
							st = st[:k]
							break
						}
					}
				}
			}
			if in.end {
				return
			}
			continue
		}
		var v vec4
		switch in.op {
		case 0x00, 0x3d, 0x3e:
			if in.end {
				return
			}
			continue
		case 0x12: // KIL
			if x.condOK(in) {
				x.killed = true
				return
			}
			if in.end {
				return
			}
			continue
		}
		s0 := x.read(in, 0)
		switch in.op {
		case 0x01:
			v = s0
		case 0x10:
			for i := range v {
				v[i] = s0[i] - f32(math.Floor(float64(s0[i])))
			}
		case 0x11:
			for i := range v {
				v[i] = f32(math.Floor(float64(s0[i])))
			}
		case 0x15, 0x16: // DDX, DDY
			v = vec4{}
		case 0x1a:
			s := 1 / s0[0]
			v = vec4{s, s, s, s}
		case 0x1b:
			s := f32(1 / math.Sqrt(math.Abs(float64(s0[0]))))
			v = vec4{s, s, s, s}
		case 0x1c:
			s := f32(math.Exp2(float64(s0[0])))
			v = vec4{s, s, s, s}
		case 0x1d:
			s := f32(math.Log2(float64(s0[0])))
			v = vec4{s, s, s, s}
		case 0x20:
			v = vec4{1, 1, 1, 1}
		case 0x21:
			v = vec4{}
		case 0x22:
			s := f32(math.Cos(float64(s0[0])))
			v = vec4{s, s, s, s}
		case 0x23:
			s := f32(math.Sin(float64(s0[0])))
			v = vec4{s, s, s, s}
		case 0x39: // NRM
			l := f32(math.Sqrt(float64(dot(s0, s0, 3))))
			if l != 0 {
				v = vec4{s0[0] / l, s0[1] / l, s0[2] / l, s0[2] / l}
			}
		case 0x1e:
			v = lit(s0)
		case 0x3c: // LIF
			v = vec4{1, s0[1], 0, 1}
			if s0[1] > 0 {
				v[2] = f32(math.Exp2(float64(s0[3])))
			}
		case 0x17: // TEX
			v = tex(in.tex, s0, 0, 0, false)
			if in.bx2 {
				for i := range v {
					v[i] = v[i]*2 - 1
				}
			}
		case 0x18: // TXP
			if s0[3] != 0 {
				s0[0] /= s0[3]
				s0[1] /= s0[3]
				s0[2] /= s0[3]
			}
			v = tex(in.tex, s0, 0, 0, false)
			if in.bx2 {
				for i := range v {
					v[i] = v[i]*2 - 1
				}
			}
		case 0x24: // PK2
			u := uint32(f32ToHalf(s0[0])) | uint32(f32ToHalf(s0[1]))<<16
			f := math.Float32frombits(u)
			v = vec4{f, f, f, f}
		case 0x25: // UP2
			u := math.Float32bits(s0[0])
			v = vec4{halfToF32(uint16(u)), halfToF32(uint16(u >> 16)), halfToF32(uint16(u)), halfToF32(uint16(u >> 16))}
		case 0x13: // PK4
			var u uint32
			for i := 0; i < 4; i++ {
				c := s0[i]
				c = max(-1, min(1, c))
				u |= uint32(uint8(int8(math.Round(float64(c*127))))) << (8 * i)
			}
			f := math.Float32frombits(u)
			v = vec4{f, f, f, f}
		case 0x14: // UP4
			u := math.Float32bits(s0[0])
			for i := range v {
				v[i] = max(-1, float32(int8(u>>(8*i)))/127)
			}
		case 0x27, 0x2c: // PKB, PKG
			var u uint32
			for i := 0; i < 4; i++ {
				c := max(0, min(1, s0[i]))
				u |= uint32(math.Round(float64(c*255))) << (8 * i)
			}
			f := math.Float32frombits(u)
			v = vec4{f, f, f, f}
		case 0x28, 0x2d: // UPB, UPG
			u := math.Float32bits(s0[0])
			for i := range v {
				v[i] = float32(u>>(8*i)&0xff) / 255
			}
		case 0x29: // PK16
			u := uint32(math.Round(float64(max(0, min(1, s0[0]))*65535))) | uint32(math.Round(float64(max(0, min(1, s0[1]))*65535)))<<16
			f := math.Float32frombits(u)
			v = vec4{f, f, f, f}
		case 0x2a: // UP16
			u := math.Float32bits(s0[0])
			a, b := float32(u&0xffff)/65535, float32(u>>16)/65535
			v = vec4{a, b, a, b}
		default:
			s1 := x.read(in, 1)
			switch in.op {
			case 0x02:
				v = vec4{s0[0] * s1[0], s0[1] * s1[1], s0[2] * s1[2], s0[3] * s1[3]}
			case 0x03:
				v = vec4{s0[0] + s1[0], s0[1] + s1[1], s0[2] + s1[2], s0[3] + s1[3]}
			case 0x38:
				d := dot(s0, s1, 2)
				v = vec4{d, d, d, d}
			case 0x05:
				d := dot(s0, s1, 3)
				v = vec4{d, d, d, d}
			case 0x06:
				d := dot(s0, s1, 4)
				v = vec4{d, d, d, d}
			case 0x07:
				v = vec4{1, s0[1] * s1[1], s0[2], s1[3]}
			case 0x08:
				for i := range v {
					v[i] = min(s0[i], s1[i])
				}
			case 0x09:
				for i := range v {
					v[i] = max(s0[i], s1[i])
				}
			case 0x0a:
				v = cmpVec(s0, s1, func(a, b float32) bool { return a < b })
			case 0x0b:
				v = cmpVec(s0, s1, func(a, b float32) bool { return a >= b })
			case 0x0c:
				v = cmpVec(s0, s1, func(a, b float32) bool { return a <= b })
			case 0x0d:
				v = cmpVec(s0, s1, func(a, b float32) bool { return a > b })
			case 0x0e:
				v = cmpVec(s0, s1, func(a, b float32) bool { return a != b })
			case 0x0f:
				v = cmpVec(s0, s1, func(a, b float32) bool { return a == b })
			case 0x26: // POW, as the hardware does it: 2^(y*log2(x))
				s := f32(math.Exp2(float64(float32(float64(s1[0]) * float64(f32(math.Log2(float64(s0[0]))))))))
				v = vec4{s, s, s, s}
			case 0x3a: // DIV
				for i := range v {
					v[i] = s0[i] / s1[0]
				}
			case 0x3b: // DIVSQ
				q := f32(1 / math.Sqrt(math.Abs(float64(s1[0]))))
				for i := range v {
					v[i] = s0[i] * q
				}
			case 0x36: // REFL
				d := 2 * dot(s0, s1, 4)
				for i := range v {
					v[i] = s0[i] - d*s1[i]
				}
			case 0x2f: // TXL
				v = tex(in.tex, s0, 0, s1[0], true)
			case 0x31: // TXB
				v = tex(in.tex, s0, s1[0], 0, false)
			case 0x19: // TXD
				v = tex(in.tex, s0, 0, 0, false)
			default:
				s2 := x.read(in, 2)
				switch in.op {
				case 0x04:
					v = vec4{s0[0]*s1[0] + s2[0], s0[1]*s1[1] + s2[1], s0[2]*s1[2] + s2[2], s0[3]*s1[3] + s2[3]}
				case 0x1f: // LRP
					for i := range v {
						v[i] = s0[i]*s1[i] + (1-s0[i])*s2[i]
					}
				case 0x2e: // DP2A
					d := dot(s0, s1, 2) + s2[0]
					v = vec4{d, d, d, d}
				}
			}
		}
		x.write(in, v)
		if in.end {
			return
		}
	}
}

func halfToF32(h uint16) float32 {
	s := uint32(h>>15) << 31
	e := int(h >> 10 & 0x1f)
	f := uint32(h & 0x3ff)
	switch {
	case e == 0:
		if f == 0 {
			return math.Float32frombits(s)
		}
		v := float32(f) / 1024 / 16384
		if s != 0 {
			v = -v
		}
		return v
	case e == 31:
		return math.Float32frombits(s | 0x7f800000 | f<<13)
	}
	return math.Float32frombits(s | uint32(e-15+127)<<23 | f<<13)
}

func f32ToHalf(v float32) uint16 {
	b := math.Float32bits(v)
	s := uint16(b>>16) & 0x8000
	e := int(b>>23&0xff) - 127 + 15
	m := b & 0x7fffff
	switch {
	case b&0x7fffffff == 0:
		return s
	case int(b>>23&0xff) == 255:
		if m != 0 {
			return s | 0x7e00
		}
		return s | 0x7c00
	case e >= 31:
		return s | 0x7c00
	case e <= 0:
		if e < -10 {
			return s
		}
		m |= 0x800000
		sh := uint(14 - e)
		h := m >> sh
		rem := m & (1<<sh - 1)
		half := uint32(1) << (sh - 1)
		if rem > half || rem == half && h&1 != 0 {
			h++
		}
		return s | uint16(h)
	}
	h := uint32(e)<<10 | m>>13
	rem := m & 0x1fff
	if rem > 0x1000 || rem == 0x1000 && h&1 != 0 {
		h++
	}
	return s | uint16(h)
}
