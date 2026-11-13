package ctr

import "math"

// The PICA200's shader processor, run once for each vertex: 16 inputs,
// 16 temporaries, 16 outputs, 96 float uniforms, 16 bool and 4 integer
// uniforms, two address registers and the loop counter, with calls,
// conditional blocks and loops. Arithmetic is float32 here (the chip uses
// 24-bit floats) with the chip's rule that 0 × ∞ is 0.

type vec4 [4]float32

// f24 turns a 24-bit PICA float (1 sign, 7 exponent, 16 mantissa bits)
// into a float32.
func f24(raw uint32) float32 {
	raw &= 0xffffff
	sign := raw >> 23
	exp := raw >> 16 & 0x7f
	if exp == 0 {
		return math.Float32frombits(sign << 31)
	}
	return math.Float32frombits(sign<<31 | (exp+64)<<23 | (raw&0xffff)<<7)
}

// f31 is the 31-bit float of some rasterizer registers (1.7.23).
func f31(raw uint32) float32 {
	raw &= 0x7fffffff
	sign := raw >> 30
	exp := raw >> 23 & 0x7f
	if exp == 0 {
		return math.Float32frombits(sign << 31)
	}
	return math.Float32frombits(sign<<31 | (exp+64)<<23 | raw&0x7fffff)
}

// unpack4f24 reads the three-word form of four 24-bit floats (w, z, y, x
// from the first word on), as uniforms and fixed attributes are written.
func unpack4f24(w [4]uint32) vec4 {
	return vec4{
		f24(w[2]),
		f24(w[1]<<8 | w[2]>>24),
		f24(w[0]<<16 | w[1]>>16),
		f24(w[0] >> 8),
	}
}

// mul is the chip's multiply: 0 × ∞ gives 0, not NaN.
func mul(a, b float32) float32 {
	r := a * b
	if r != r && a == a && b == b {
		return 0
	}
	return r
}

type shaderSetup struct {
	code  [4096]uint32
	swz   [512]uint32
	f     [96]vec4
	b     uint16
	i     [4][3]uint8 // count, start, step
	entry uint32
	// writing
	codeIdx, swzIdx uint32
	fIdx            uint32
	fF32            bool
	fBuf            [4]uint32
	fN              int
}

// writeUniform takes one word of a float uniform upload.
func (s *shaderSetup) writeUniform(v uint32) {
	s.fBuf[s.fN] = v
	s.fN++
	need := 3
	if s.fF32 {
		need = 4
	}
	if s.fN < need {
		return
	}
	s.fN = 0
	var u vec4
	if s.fF32 {
		for i := 0; i < 4; i++ {
			u[3-i] = math.Float32frombits(s.fBuf[i])
		}
	} else {
		u = unpack4f24(s.fBuf)
	}
	if s.fIdx < 96 {
		s.f[s.fIdx] = u
	}
	s.fIdx++
}

type shaderUnit struct {
	in, tmp, out [16]vec4
	a0           [2]int32
	aL           int32
	cx, cy       bool
	// geometry shader output
	emit        func(u *shaderUnit, winding, prim bool, id int)
	emitID      int
	emitWinding bool
	emitPrim    bool
}

type flowFrame struct {
	final, ret, loop uint32
	repeat           int32
	inc              int32
	isLoop           bool
}

func (u *shaderUnit) offset(idx uint32) int32 {
	switch idx {
	case 1:
		return u.a0[0]
	case 2:
		return u.a0[1]
	case 3:
		return u.aL
	}
	return 0
}

func (u *shaderUnit) read(s *shaderSetup, reg uint32, off int32, sel uint32, neg bool) vec4 {
	var v vec4
	switch {
	case reg < 0x10:
		v = u.in[reg]
	case reg < 0x20:
		v = u.tmp[reg-0x10]
	default:
		i := int32(reg-0x20) + off
		if i >= 0 && i < 96 {
			v = s.f[i]
		} else {
			v = vec4{1, 1, 1, 1}
		}
	}
	r := vec4{v[sel>>6&3], v[sel>>4&3], v[sel>>2&3], v[sel&3]}
	if neg {
		for i := range r {
			r[i] = -r[i]
		}
	}
	return r
}

func (u *shaderUnit) write(reg, mask uint32, v vec4) {
	var d *vec4
	if reg < 0x10 {
		d = &u.out[reg]
	} else {
		d = &u.tmp[reg&0xf]
	}
	for i := 0; i < 4; i++ {
		if mask>>(3-i)&1 != 0 {
			d[i] = v[i]
		}
	}
}

func compare(a, b float32, op uint32) bool {
	switch op {
	case 0:
		return a == b
	case 1:
		return a != b
	case 2:
		return a < b
	case 3:
		return a <= b
	case 4:
		return a > b
	case 5:
		return a >= b
	}
	return true
}

func (u *shaderUnit) cond(instr uint32) bool {
	refx := instr>>25&1 != 0
	refy := instr>>24&1 != 0
	x, y := refx == u.cx, refy == u.cy
	switch instr >> 22 & 3 {
	case 0:
		return x || y
	case 1:
		return x && y
	case 2:
		return x
	}
	return y
}

func dot(a, b vec4, n int) float32 {
	var s float32
	for i := 0; i < n; i++ {
		s += mul(a[i], b[i])
	}
	return s
}

func splat(f float32) vec4 { return vec4{f, f, f, f} }

// run runs the program from its entry point.
func (u *shaderUnit) run(s *shaderSetup) {
	u.a0 = [2]int32{}
	u.aL = 0
	u.cx, u.cy = false, false
	var stack [8]flowFrame
	sp := 0
	call := func(offset, num, ret uint32, repeat, inc int32, loop bool) {
		if sp < len(stack) {
			stack[sp] = flowFrame{final: offset + num, ret: ret, loop: offset, repeat: repeat, inc: inc, isLoop: loop}
			sp++
		}
	}
	pc := s.entry
	for steps := 0; steps < 1<<16; steps++ {
		if sp > 0 && pc == stack[sp-1].final {
			f := &stack[sp-1]
			if f.isLoop {
				u.aL += f.inc
			}
			if f.repeat == 0 {
				pc = f.ret
				sp--
			} else {
				f.repeat--
				pc = f.loop
			}
			continue
		}
		if pc >= 4096 {
			return
		}
		instr := s.code[pc]
		op := instr >> 26
		next := pc + 1
		switch {
		case op < 0x20 || op == 0x2e || op == 0x2f: // arithmetic and CMP
			desc := s.swz[instr&0x7f]
			var src1, src2 uint32
			var o1, o2 int32
			inv := op >= 0x18 && op <= 0x1b
			if inv {
				src1, src2 = instr>>14&0x1f, instr>>7&0x7f
				o2 = u.offset(instr >> 19 & 3)
			} else {
				src1, src2 = instr>>12&0x7f, instr>>7&0x1f
				o1 = u.offset(instr >> 19 & 3)
			}
			a := u.read(s, src1, o1, desc>>5&0xff, desc>>4&1 != 0)
			b := u.read(s, src2, o2, desc>>14&0xff, desc>>13&1 != 0)
			dest, mask := instr>>21&0x1f, desc&0xf
			var r vec4
			switch op {
			case 0x00: // ADD
				for i := range r {
					r[i] = a[i] + b[i]
				}
			case 0x01: // DP3
				r = splat(dot(a, b, 3))
			case 0x02: // DP4
				r = splat(dot(a, b, 4))
			case 0x03, 0x18: // DPH
				r = splat(dot(a, b, 3) + b[3])
			case 0x04, 0x19: // DST
				r = vec4{1, mul(a[1], b[1]), a[2], b[3]}
			case 0x05: // EX2
				r = splat(float32(math.Exp2(float64(a[0]))))
			case 0x06: // LG2
				r = splat(float32(math.Log2(float64(a[0]))))
			case 0x07: // LITP
				r = vec4{max(a[0], 0), max(-127.9961, min(a[1], 127.9961)), a[2], max(a[3], 0)}
				u.cx, u.cy = a[0] > 0, a[3] > 0
			case 0x08: // MUL
				for i := range r {
					r[i] = mul(a[i], b[i])
				}
			case 0x09, 0x1a: // SGE
				for i := range r {
					if a[i] >= b[i] {
						r[i] = 1
					}
				}
			case 0x0a, 0x1b: // SLT
				for i := range r {
					if a[i] < b[i] {
						r[i] = 1
					}
				}
			case 0x0b: // FLR
				for i := range r {
					r[i] = float32(math.Floor(float64(a[i])))
				}
			case 0x0c: // MAX
				for i := range r {
					if a[i] > b[i] {
						r[i] = a[i]
					} else {
						r[i] = b[i]
					}
				}
			case 0x0d: // MIN
				for i := range r {
					if a[i] < b[i] {
						r[i] = a[i]
					} else {
						r[i] = b[i]
					}
				}
			case 0x0e: // RCP
				r = splat(1 / a[0])
			case 0x0f: // RSQ
				r = splat(float32(1 / math.Sqrt(float64(a[0]))))
			case 0x12: // MOVA
				if mask&8 != 0 {
					u.a0[0] = int32(a[0])
				}
				if mask&4 != 0 {
					u.a0[1] = int32(a[1])
				}
				mask = 0
			case 0x13: // MOV
				r = a
			case 0x2e, 0x2f: // CMP
				u.cx = compare(a[0], b[0], instr>>24&7)
				u.cy = compare(a[1], b[1], instr>>21&7)
				mask = 0
			default:
				mask = 0
			}
			if mask != 0 {
				u.write(dest, mask, r)
			}
		case op >= 0x30: // MAD, MADI
			desc := s.swz[instr&0x1f]
			src1 := instr >> 17 & 0x1f
			off := u.offset(instr >> 22 & 3)
			var src2, src3 uint32
			var o2, o3 int32
			if op >= 0x38 {
				src2, src3 = instr>>10&0x7f, instr>>5&0x1f
				o2 = off
			} else {
				src2, src3 = instr>>10&0x1f, instr>>5&0x7f
				o3 = off
			}
			a := u.read(s, src1, 0, desc>>5&0xff, desc>>4&1 != 0)
			b := u.read(s, src2, o2, desc>>14&0xff, desc>>13&1 != 0)
			c := u.read(s, src3, o3, desc>>23&0xff, desc>>22&1 != 0)
			var r vec4
			for i := range r {
				r[i] = mul(a[i], b[i]) + c[i]
			}
			u.write(instr>>24&0x1f, desc&0xf, r)
		default:
			dst, num := instr>>10&0xfff, instr&0xff
			switch op {
			case 0x20: // BREAK
				for sp > 0 {
					sp--
					if stack[sp].isLoop {
						next = stack[sp].ret
						break
					}
				}
			case 0x23: // BREAKC
				if u.cond(instr) {
					for sp > 0 {
						sp--
						if stack[sp].isLoop {
							next = stack[sp].ret
							break
						}
					}
				}
			case 0x21: // NOP
			case 0x22: // END
				return
			case 0x24: // CALL
				call(dst, num, pc+1, 0, 0, false)
				next = dst
			case 0x25: // CALLC
				if u.cond(instr) {
					call(dst, num, pc+1, 0, 0, false)
					next = dst
				}
			case 0x26: // CALLU
				if s.b>>(instr>>22&0xf)&1 != 0 {
					call(dst, num, pc+1, 0, 0, false)
					next = dst
				}
			case 0x27, 0x28: // IFU, IFC
				var c bool
				if op == 0x27 {
					c = s.b>>(instr>>22&0xf)&1 != 0
				} else {
					c = u.cond(instr)
				}
				if c {
					call(pc+1, dst-pc-1, dst+num, 0, 0, false)
				} else {
					call(dst, num, dst+num, 0, 0, false)
					next = dst
				}
			case 0x29: // LOOP
				iu := s.i[instr>>22&3]
				u.aL = int32(iu[1])
				call(pc+1, dst-pc, dst+1, int32(iu[0]), int32(iu[2]), true)
			case 0x2a: // EMIT
				if u.emit != nil {
					u.emit(u, u.emitWinding, u.emitPrim, u.emitID)
				}
			case 0x2b: // SETEMIT
				u.emitID = int(instr >> 24 & 3)
				u.emitWinding = instr>>22&1 != 0
				u.emitPrim = instr>>23&1 != 0
			case 0x2c: // JMPC
				if u.cond(instr) {
					next = dst
				}
			case 0x2d: // JMPU
				if (s.b>>(instr>>22&0xf)&1 != 0) == (num&1 == 0) {
					next = dst
				}
			}
		}
		pc = next
	}
}
