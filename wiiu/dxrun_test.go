package wiiu

import (
	"encoding/binary"
	"math"
	"math/rand"
	"testing"
)

// dxVSRun is one vertex shader test run (the Direct3D side is in
// latte_dxbc_windows_test.go).
type dxVSRun struct {
	mem   map[uint32]uint32 // program memory (GPU words)
	vs    uint32
	fs    uint32
	nGPR  int        // R1..RnGPR come from in
	in    [][]uint32 // per vertex, 4*nGPR words
	nOut  int        // outputs: position, then parameters 0..nOut-2
	sys   [dxSysSize * 4]uint32
	cfile [256 * 4]uint32
}

func (r *dxVSRun) read32(a uint32) uint32 { return r.mem[a] }

func (r *dxVSRun) load(addr uint32, code []byte) {
	for i := 0; i+3 < len(code); i += 4 {
		r.mem[addr+uint32(i)] = binary.LittleEndian.Uint32(code[i:])
	}
}

func newVSRun() *dxVSRun { return &dxVSRun{mem: map[uint32]uint32{}, vs: tVS, fs: tVS + 0x10000} }

func dxSimpleRunAny(vs *shaderAsm, nGPR, nOut int) *dxVSRun {
	r := newVSRun()
	r.load(r.vs, vs.Bytes())
	ret := &shaderAsm{}
	ret.CF(cfRETURN, 0, 0, 0, 0)
	r.load(r.fs, ret.Bytes())
	r.nGPR, r.nOut = nGPR, nOut
	return r
}

// ---- random ALU programs ----

// dxRandOps are the ALU instructions the random programs use, with whether
// their results are only close (not bit-exact) on other hardware.
var dxRandOp2 = []uint32{
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x19, 0x1a,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
	0x70, 0x71, 0x72,
}

var dxRandTrans = []uint32{0x6b, 0x6c, 0x6d, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x66, 0x69, 0x6a, 0x61, 0x63, 0x6e, 0x6f, 0x64, 0x65, 0x67, 0x68, 0x62}

var dxRandOp3 = []uint32{0x0c, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e}

var dxRandVals = []uint32{
	0, 0x80000000, f2u(1), f2u(-1), f2u(0.5), f2u(2), f2u(3.75), f2u(-7.25), f2u(100), f2u(1e-3), f2u(1e5),
	f2u(-0.25), f2u(0.125), f2u(1.5), f2u(-2.5), 1, 7, 0xffffffff, 12345, 31, 33, 0x7f800000, 0xff800000,
	f2u(0.3), f2u(-1.75), f2u(65536), f2u(-3),
}

// dxConstGPRs, when set, makes dxRandALU read literals and constants from
// registers R9..R15 and gives their values.
var dxConstGPRs *[]uint32

// dxRandALU builds a vertex shader of random ALU groups over R1..R4, and
// exports R5..R8.
func dxRandALU(rnd func(int) int, groups int) *shaderAsm {
	vs := &shaderAsm{}
	var gs []aluGroupSpec
	pick := func(l []uint32) uint32 { return l[rnd(len(l))] }
	var pvSet [4]bool
	psSet := false
	for g := 0; g < groups; g++ {
		var insts []aluInst
		var lits []uint32
		litHi := false
		src := func() aluSrcSpec {
			var s aluSrcSpec
			switch k := rnd(10); {
			case k < 6:
				s = gprSrc(uint32(1+rnd(8)), uint32(rnd(4)))
			case k == 6:
				s = specialSrc([]uint32{srcZero, srcOne, srcIntOne, srcIntMOne, srcHalf}[rnd(5)])
			case k == 7:
				s = litSrc(uint32(rnd(4)))
				if s.ch >= 2 {
					litHi = true
				}
			case k == 8 && g > 0:
				// PV and PS only where the last group set them (the rest
				// is undefined)
				s = aluSrcSpec{sel: srcPV, ch: uint32(rnd(4))}
				if !pvSet[s.ch] {
					s = gprSrc(1+s.ch, s.ch)
				}
			case k == 9 && g > 0:
				s = aluSrcSpec{sel: srcPS}
				if !psSet {
					s = gprSrc(2, 1)
				}
			default:
				s = gprSrc(uint32(1+rnd(4)), uint32(rnd(4)))
			}
			if rnd(6) == 0 {
				s.neg = true
			}
			return s
		}
		if rnd(8) == 0 {
			// a four-wide DOT4 / MAX4
			op := []uint32{0x50, 0x51, 0x53}[rnd(3)]
			for c := uint32(0); c < 4; c++ {
				insts = append(insts, op2(op, uint32(5+rnd(4)), c, src(), src()))
			}
		} else {
			for c := uint32(0); c < 4; c++ {
				if rnd(3) == 0 {
					continue
				}
				var in aluInst
				if rnd(4) == 0 {
					in = op3i(pick(dxRandOp3), uint32(5+rnd(4)), c, src(), src(), src())
				} else {
					in = op2(pick(dxRandOp2), uint32(5+rnd(4)), c, src(), src())
					in.src[0].abs = rnd(8) == 0
					in.omod = uint32(rnd(8)) & 3
					if rnd(3) != 0 {
						in.omod = 0
					}
				}
				in.clamp = rnd(6) == 0
				insts = append(insts, in)
			}
			if rnd(2) == 0 {
				// a destination no vector slot writes
				gpr, ch := uint32(5+rnd(4)), uint32(rnd(4))
				free := true
				for _, v := range insts {
					if v.dst == gpr && v.dstChan == ch {
						free = false
					}
				}
				if free {
					insts = append(insts, op2(pick(dxRandTrans), gpr, ch, src(), src()))
				}
			}
			if len(insts) == 0 {
				insts = append(insts, op2(0x19, 5, 0, src(), src()))
			}
		}
		for _, in := range insts {
			_ = in
		}
		lits = []uint32{pick(dxRandVals), pick(dxRandVals)}
		if litHi {
			lits = append(lits, pick(dxRandVals), pick(dxRandVals))
		}
		used := false
		for _, in := range insts {
			for k := 0; k < 3; k++ {
				if in.src[k].sel == srcLiteral && (k < 2 || in.op3) {
					used = true
				}
			}
		}
		if !used {
			lits = nil
		}
		gs = append(gs, aluGroupSpec{insts: insts, lits: lits})
		pvSet, psSet = [4]bool{}, false
		for _, in := range insts {
			if transOnly(in.inst) && !in.op3 {
				psSet = true
			} else {
				pvSet[in.dstChan] = true
			}
		}
	}
	if dxConstGPRs != nil {
		// the same program reading its constants from R9..R15
		*dxConstGPRs = make([]uint32, 28)
		c := *dxConstGPRs
		copy(c[20:], []uint32{0, f2u(1), 1, 0xffffffff, f2u(0.5)})
		for g := range gs {
			copy(c[4*g:], gs[g].lits)
			for k := range gs[g].insts {
				for j := range gs[g].insts[k].src {
					sp := &gs[g].insts[k].src[j]
					switch {
					case sp.sel == srcLiteral:
						sp.sel = uint32(9 + g)
					case sp.sel >= srcZero && sp.sel <= srcHalf:
						n := 20 + sp.sel - srcZero
						sp.sel, sp.ch = 9+n/4, n%4
					}
				}
			}
			gs[g].lits = nil
		}
	}
	vs.ALU(8, [2]kcacheSpec{}, gs...)
	vs.Export(false, 1, 60, 5, 1, [4]uint32{0, 1, 2, 3})
	vs.Export(true, 2, 0, 6, 3, [4]uint32{0, 1, 2, 3})
	vs.End()
	return vs
}

// dxClose: the same bits, both NaN, or (for ordinary floats) close.
func dxClose(a, b uint32, tol float64) bool {
	if a == b {
		return true
	}
	fa, fb := float64(u2f(a)), float64(u2f(b))
	if math.IsNaN(fa) && math.IsNaN(fb) {
		return true
	}
	normal := func(v uint32) bool { e := v >> 23 & 0xff; return e != 0 && e != 255 }
	if !normal(a) || !normal(b) {
		// a denormal and a zero are the same (some hardware doesn't
		// flush them); otherwise these are integers
		tiny := func(v uint32) bool { return v>>23&0xff == 0 }
		if tiny(a) && tiny(b) && (a&0x7fffffff == 0 || b&0x7fffffff == 0) {
			return true
		}
		return (a|b)&0x7fffffff == 0
	}
	return math.Abs(fa-fb) <= tol*math.Max(1, math.Abs(fb))
}

// TestDXTranslateRandom checks random ALU programs translate (the Windows
// tests also run them).
func TestDXTranslateRandom(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	for i := 0; i < 200; i++ {
		vs := dxRandALU(rng.Intn, 1+rng.Intn(6))
		r := dxSimpleRunAny(vs, 4, 4)
		cfg := &dxConfig{read32: r.read32, fsAddr: r.fs, nParams: 4, testPoints: 4, testGPRs: 4,
			texInfo: func(uint32) dxTexKey { return dxTexKey{} }, vbufInfo: func(uint32) dxFetchKey { return dxFetchKey{} }}
		if _, err := translateDX(cfg, r.vs); err != nil {
			t.Fatal(err)
		}
	}
}

// interp runs the vertex shader in the interpreter.
func (r *dxVSRun) interp(t *testing.T) [][]uint32 {
	g := newTestGPU()
	for a, w := range r.mem {
		g.mem.Write32(a, bswap32(w))
	}
	e := &stageEnv{g: g, stage: stageVS, fsAddr: r.fs}
	out := make([][]uint32, len(r.in))
	for base := 0; base < len(r.in); base += nl {
		s := &shaderRun{env: e}
		var outs [nl]vtxOut
		for l := 0; l < nl && base+l < len(r.in); l++ {
			e.vtx[l] = &outs[l]
			s.valid[l] = true
			s.gpr[0][0][l] = uint32(base + l)
			for g := 0; g < r.nGPR; g++ {
				for c := 0; c < 4; c++ {
					s.gpr[1+g][c][l] = r.in[base+l][4*g+c]
				}
			}
		}
		s.run(r.vs)
		if s.Err != "" {
			t.Fatal(s.Err)
		}
		for l := 0; l < nl && base+l < len(r.in); l++ {
			o := make([]uint32, 4*r.nOut)
			for c := 0; c < 4; c++ {
				o[c] = f2u(outs[l].pos[c])
			}
			for p := 1; p < r.nOut; p++ {
				copy(o[4*p:4*p+4], outs[l].param[p-1][:])
			}
			out[base+l] = o
		}
	}
	return out
}
