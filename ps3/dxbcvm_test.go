package ps3

// A small interpreter for the shader bytecode the translator emits, so
// tests can run translated shaders without a GPU and compare them with the
// fragment program interpreter.

import (
	"encoding/binary"
	"fmt"
	"math"
)

type dxOp struct {
	tok  uint32
	op   uint32
	ext  []uint32
	args []dxArg
}

type dxIdx struct {
	imm uint32
	rel *dxArg
}

type dxArg struct {
	typ  uint32
	nc   uint32 // 0, 1 or 4
	mode uint32
	comp uint32
	idx  []dxIdx
	mod  uint32
	imm  []uint32
}

// dxVM runs one pixel (or vertex).
type dxVM struct {
	prog   []dxOp
	r      [][4]uint32
	in     [32][4]uint32
	out    [24][4]uint32
	cb     [][4]uint32
	sample func(unit int, u, v float32) [4]float32
	ld     func(unit int, i uint32) [4]uint32
	killed bool
}

func dxParse(blob []byte) ([]dxOp, error) {
	if len(blob) < 32 || string(blob[:4]) != "DXBC" {
		return nil, fmt.Errorf("not DXBC")
	}
	n := binary.LittleEndian.Uint32(blob[28:])
	var w []uint32
	for i := uint32(0); i < n; i++ {
		o := binary.LittleEndian.Uint32(blob[32+4*i:])
		if string(blob[o:o+4]) == "SHEX" {
			size := binary.LittleEndian.Uint32(blob[o+4:])
			for j := uint32(0); j < size/4; j++ {
				w = append(w, binary.LittleEndian.Uint32(blob[o+8+4*j:]))
			}
		}
	}
	if len(w) < 2 || int(w[1]) != len(w) {
		return nil, fmt.Errorf("bad SHEX length")
	}
	var prog []dxOp
	for p := 2; p < len(w); {
		tok := w[p]
		op := tok & 0x7ff
		l := int(tok >> 24 & 0x7f)
		if op == 104 || op == 105 || op == 94 || op == 92 || op == 93 { // dcl_temps etc: fixed lengths
			if l == 0 {
				return nil, fmt.Errorf("zero length")
			}
		}
		if l == 0 || p+l > len(w) {
			return nil, fmt.Errorf("bad instruction length at %d", p)
		}
		d := dxOp{tok: tok, op: op}
		q := p + 1
		if tok>>31 != 0 {
			for {
				d.ext = append(d.ext, w[q])
				q++
				if w[q-1]>>31 == 0 {
					break
				}
			}
		}
		if op < 88 || op >= 108 {
			for q < p+l {
				a, nq, err := dxArgAt(w, q)
				if err != nil {
					return nil, fmt.Errorf("op %d: %v", op, err)
				}
				d.args = append(d.args, a)
				q = nq
			}
			if q != p+l {
				return nil, fmt.Errorf("op %d: operands overrun", op)
			}
		}
		prog = append(prog, d)
		p += l
	}
	return prog, nil
}

func dxArgAt(w []uint32, q int) (dxArg, int, error) {
	t := w[q]
	q++
	a := dxArg{}
	switch t & 3 {
	case 0:
		a.nc = 0
	case 1:
		a.nc = 1
	case 2:
		a.nc = 4
	default:
		return a, q, fmt.Errorf("bad component count")
	}
	a.mode = t >> 2 & 3
	a.comp = t >> 4 & 0xff
	a.typ = t >> 12 & 0xff
	dims := int(t >> 20 & 3)
	if t>>31 != 0 {
		a.mod = w[q] >> 6 & 0xff
		q++
	}
	if a.typ == 4 { // immediate
		n := 1
		if a.nc == 4 {
			n = 4
		}
		a.imm = w[q : q+n]
		return a, q + n, nil
	}
	for k := 0; k < dims; k++ {
		rep := t >> (22 + 3*k) & 7
		var ix dxIdx
		switch rep {
		case 0:
			ix.imm = w[q]
			q++
		case 2, 3:
			if rep == 3 {
				ix.imm = w[q]
				q++
			}
			r, nq, err := dxArgAt(w, q)
			if err != nil {
				return a, q, err
			}
			ix.rel = &r
			q = nq
		default:
			return a, q, fmt.Errorf("index representation %d", rep)
		}
		a.idx = append(a.idx, ix)
	}
	return a, q, nil
}

func (vm *dxVM) index(ix dxIdx) uint32 {
	v := ix.imm
	if ix.rel != nil {
		v += vm.read(*ix.rel)[0]
	}
	return v
}

// read returns a source operand's four components (swizzled, modified).
func (vm *dxVM) read(a dxArg) [4]uint32 {
	var v [4]uint32
	switch a.typ {
	case 4:
		if len(a.imm) == 1 {
			v = [4]uint32{a.imm[0], a.imm[0], a.imm[0], a.imm[0]}
		} else {
			copy(v[:], a.imm)
		}
	case 0:
		v = vm.r[vm.index(a.idx[0])]
	case 1:
		v = vm.in[vm.index(a.idx[0])]
	case 8:
		i := vm.index(a.idx[1])
		if int(i) < len(vm.cb) {
			v = vm.cb[i]
		}
	default:
		panic(fmt.Sprintf("read type %d", a.typ))
	}
	if a.nc == 4 {
		switch a.mode {
		case 1:
			v = [4]uint32{v[a.comp&3], v[a.comp>>2&3], v[a.comp>>4&3], v[a.comp>>6&3]}
		case 2:
			c := v[a.comp&3]
			v = [4]uint32{c, c, c, c}
		}
	}
	if a.mod&2 != 0 {
		for i := range v {
			v[i] &^= 0x80000000
		}
	}
	if a.mod&1 != 0 {
		for i := range v {
			v[i] ^= 0x80000000
		}
	}
	return v
}

func (a dxArg) mask() uint32 {
	if a.nc == 4 && a.mode == 0 {
		return a.comp
	}
	if a.nc == 4 && a.mode == 2 {
		return 1 << (a.comp & 3)
	}
	return 15
}

func (vm *dxVM) write(a dxArg, v [4]uint32, sat bool) {
	if a.typ == 13 { // null
		return
	}
	m := a.mask()
	if sat {
		for i := range v {
			f := math.Float32frombits(v[i])
			if !(f > 0) {
				f = 0
			} else if f > 1 {
				f = 1
			}
			v[i] = math.Float32bits(f)
		}
	}
	var dst *[4]uint32
	switch a.typ {
	case 0:
		dst = &vm.r[vm.index(a.idx[0])]
	case 2:
		dst = &vm.out[vm.index(a.idx[0])]
	default:
		panic(fmt.Sprintf("write type %d", a.typ))
	}
	for i := 0; i < 4; i++ {
		if m>>i&1 != 0 {
			dst[i] = v[i]
		}
	}
}

func f(u uint32) float32  { return math.Float32frombits(u) }
func fb(x float32) uint32 { return math.Float32bits(x) }
func dxBool(b bool) uint32 {
	if b {
		return 0xffffffff
	}
	return 0
}

func dxMin(a, b float32) float32 {
	if a != a {
		return b
	}
	if b != b {
		return a
	}
	return min(a, b)
}

func dxMax(a, b float32) float32 {
	if a != a {
		return b
	}
	if b != b {
		return a
	}
	return max(a, b)
}

// run executes the program from the start.
func (vm *dxVM) run() {
	type loopFrame struct{ start int }
	var loops []int
	for pc := 0; pc < len(vm.prog); pc++ {
		d := &vm.prog[pc]
		sat := d.tok&(1<<13) != 0
		nz := d.tok&(1<<18) != 0
		A := func(i int) [4]uint32 { return vm.read(d.args[i]) }
		per := func(fn func(a, b, c uint32) uint32) {
			var x, y, z [4]uint32
			n := len(d.args)
			if n > 1 {
				x = A(1)
			}
			if n > 2 {
				y = A(2)
			}
			if n > 3 {
				z = A(3)
			}
			var r [4]uint32
			for i := 0; i < 4; i++ {
				r[i] = fn(x[i], y[i], z[i])
			}
			vm.write(d.args[0], r, sat)
		}
		ff := func(fn func(a, b, c float32) float32) {
			per(func(a, b, c uint32) uint32 { return fb(fn(f(a), f(b), f(c))) })
		}
		dp := func(n int) {
			x, y := A(1), A(2)
			var s float32
			for i := 0; i < n; i++ {
				s += f(x[i]) * f(y[i])
			}
			vm.write(d.args[0], [4]uint32{fb(s), fb(s), fb(s), fb(s)}, sat)
		}
		switch d.op {
		case 88, 89, 90, 95, 96, 97, 98, 99, 100, 101, 103, 104, 105, 106: // declarations
		case 54: // mov
			vm.write(d.args[0], A(1), sat)
		case 55: // movc
			c, x, y := A(1), A(2), A(3)
			var r [4]uint32
			for i := range r {
				if c[i] != 0 {
					r[i] = x[i]
				} else {
					r[i] = y[i]
				}
			}
			vm.write(d.args[0], r, sat)
		case 0:
			ff(func(a, b, _ float32) float32 { return a + b })
		case 56:
			ff(func(a, b, _ float32) float32 { return a * b })
		case 50:
			ff(func(a, b, c float32) float32 { return a*b + c })
		case 14:
			ff(func(a, b, _ float32) float32 { return a / b })
		case 51:
			ff(func(a, b, _ float32) float32 { return dxMin(a, b) })
		case 52:
			ff(func(a, b, _ float32) float32 { return dxMax(a, b) })
		case 15:
			dp(2)
		case 16:
			dp(3)
		case 17:
			dp(4)
		case 49:
			per(func(a, b, _ uint32) uint32 { return dxBool(f(a) < f(b)) })
		case 29:
			per(func(a, b, _ uint32) uint32 { return dxBool(f(a) >= f(b)) })
		case 24:
			per(func(a, b, _ uint32) uint32 { return dxBool(f(a) == f(b)) })
		case 57:
			per(func(a, b, _ uint32) uint32 { return dxBool(f(a) != f(b)) })
		case 1:
			per(func(a, b, _ uint32) uint32 { return a & b })
		case 60:
			per(func(a, b, _ uint32) uint32 { return a | b })
		case 41:
			per(func(a, b, _ uint32) uint32 { return a << (b & 31) })
		case 85:
			per(func(a, b, _ uint32) uint32 { return a >> (b & 31) })
		case 30:
			per(func(a, b, _ uint32) uint32 { return a + b })
		case 35:
			per(func(a, b, c uint32) uint32 { return uint32(int32(a)*int32(b) + int32(c)) })
		case 26:
			ff(func(a, _, _ float32) float32 { return a - float32(math.Floor(float64(a))) })
		case 65:
			ff(func(a, _, _ float32) float32 { return float32(math.Floor(float64(a))) })
		case 64:
			ff(func(a, _, _ float32) float32 { return float32(math.RoundToEven(float64(a))) })
		case 11, 12, 122, 123, 124, 125:
			vm.write(d.args[0], [4]uint32{}, sat)
		case 68:
			ff(func(a, _, _ float32) float32 { return float32(1 / math.Sqrt(float64(a))) })
		case 25:
			ff(func(a, _, _ float32) float32 { return float32(math.Exp2(float64(a))) })
		case 47:
			ff(func(a, _, _ float32) float32 { return float32(math.Log2(float64(a))) })
		case 77: // sincos
			x := A(2)
			var s, c [4]uint32
			for i := range x {
				s[i] = fb(float32(math.Sin(float64(f(x[i])))))
				c[i] = fb(float32(math.Cos(float64(f(x[i])))))
			}
			vm.write(d.args[0], s, sat)
			vm.write(d.args[1], c, sat)
		case 130: // f32tof16
			per(func(a, _, _ uint32) uint32 { return uint32(f32ToHalf(f(a))) })
		case 131:
			per(func(a, _, _ uint32) uint32 { return fb(halfToF32(uint16(a))) })
		case 43:
			per(func(a, _, _ uint32) uint32 { return fb(float32(int32(a))) })
		case 86:
			per(func(a, _, _ uint32) uint32 { return fb(float32(a)) })
		case 27:
			per(func(a, _, _ uint32) uint32 { return uint32(int32(f(a))) })
		case 28:
			per(func(a, _, _ uint32) uint32 {
				x := f(a)
				if !(x > 0) {
					return 0
				}
				return uint32(x)
			})
		case 139: // ibfe width, offset, value
			per(func(wd, off, v uint32) uint32 {
				wd, off = wd&31, off&31
				if wd == 0 {
					return 0
				}
				return uint32(int32(v<<(32-wd-off)) >> (32 - wd))
			})
		case 138:
			per(func(wd, off, v uint32) uint32 {
				wd, off = wd&31, off&31
				if wd == 0 {
					return 0
				}
				return v << (32 - wd - off) >> (32 - wd)
			})
		case 31: // if
			c := A(0)[0]
			if (c != 0) != nz {
				pc = vm.skip(pc, true)
			}
		case 18: // else: we ran the if part, skip to endif
			pc = vm.skip(pc, false)
		case 21: // endif
		case 48: // loop
			loops = append(loops, pc)
		case 22: // endloop
			pc = loops[len(loops)-1]
		case 2: // break
			pc = vm.loopEnd(pc)
			loops = loops[:len(loops)-1]
		case 3: // breakc
			c := A(0)[0]
			if (c != 0) == nz {
				pc = vm.loopEnd(pc)
				loops = loops[:len(loops)-1]
			}
		case 13: // discard
			c := A(0)[0]
			if (c != 0) == nz {
				vm.killed = true
			}
		case 62: // ret
			return
		case 72: // sample_l dst, addr, tex, samp, lod
			c := A(1)
			unit := int(d.args[2].idx[0].imm)
			v := vm.sample(unit, f(c[0]), f(c[1]))
			r := [4]uint32{fb(v[0]), fb(v[1]), fb(v[2]), fb(v[3])}
			if d.args[2].mode == 1 {
				sw := d.args[2].comp
				r = [4]uint32{r[sw&3], r[sw>>2&3], r[sw>>4&3], r[sw>>6&3]}
			}
			vm.write(d.args[0], r, sat)
		case 45: // ld
			c := A(1)
			vm.write(d.args[0], vm.ld(int(d.args[2].idx[0].imm), c[0]), sat)
		default:
			panic(fmt.Sprintf("dxVM: op %d", d.op))
		}
	}
}

// skip moves from an if (or else) to its else/endif.
func (vm *dxVM) skip(pc int, toElse bool) int {
	depth := 0
	for p := pc + 1; p < len(vm.prog); p++ {
		switch vm.prog[p].op {
		case 31:
			depth++
		case 21:
			if depth == 0 {
				return p
			}
			depth--
		case 18:
			if depth == 0 && toElse {
				return p
			}
		}
	}
	return len(vm.prog)
}

// loopEnd finds the endloop of the loop pc is in.
func (vm *dxVM) loopEnd(pc int) int {
	depth := 0
	for p := pc + 1; p < len(vm.prog); p++ {
		switch vm.prog[p].op {
		case 48:
			depth++
		case 22:
			if depth == 0 {
				return p
			}
			depth--
		}
	}
	return len(vm.prog)
}
