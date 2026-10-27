package ps3

import (
	"encoding/binary"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
)

// testTexel is the texture every unit holds in these tests.
func testTexel(unit int, u, v float32) [4]float32 {
	return [4]float32{
		float32(math.Sin(float64(u*3 + float32(unit)))),
		float32(math.Cos(float64(v * 2))),
		u * v,
		0.5 + 0.1*float32(unit),
	}
}

func fpFromBytes(b []byte) *fpProg {
	return decodeFPFrom(func(a uint32) uint32 {
		if int(a)+4 > len(b) {
			return 0
		}
		return binary.BigEndian.Uint32(b[a:])
	}, 0)
}

func close32(a, b float32) bool {
	if a != a || b != b {
		return a != a && b != b
	}
	if math.IsInf(float64(a), 0) || math.IsInf(float64(b), 0) {
		return a == b || math.Abs(float64(a)) > 1e30 && math.Abs(float64(b)) > 1e30 && (a > 0) == (b > 0)
	}
	d := math.Abs(float64(a - b))
	return d <= 2e-3*math.Max(1, math.Max(math.Abs(float64(a)), math.Abs(float64(b))))
}

// checkFP runs program p on random inputs both ways and reports
// differences.
func checkFP(t *testing.T, name string, p *fpProg, rng *rand.Rand) bool {
	ok := true
	for _, out32 := range []bool{false, true} {
		code, err := translateFP(p, fpKey{shape: p.shape, out32: out32})
		if err != nil {
			t.Errorf("%s: %v", name, err)
			return false
		}
		prog, err := dxParse(code)
		if err != nil {
			t.Errorf("%s: parse: %v", name, err)
			return false
		}
		var tex [16]gpuTexParams
		for u := range tex {
			tex[u].scale = [4]float32{1, 1, 1, 1}
			tex[u].rows, tex[u].add = remapParams([4]int{3, 0, 1, 2}, [4]float32{}, 0xAAE4)
		}
		cb := fpConstBlock(p, 0, &tex)
		for trial := 0; trial < 20; trial++ {
			var fin [16]vec4
			for i := range fin {
				for j := range fin[i] {
					fin[i][j] = rng.Float32()*4 - 2
				}
			}
			fin[0] = vec4{rng.Float32() * 1280, rng.Float32() * 720, rng.Float32(), 0.1 + rng.Float32()}
			fin[14] = vec4{1, 1, 1, 1}
			for in := 1; in < 16; in++ {
				if fpInputToVP[in] < 0 && in != 14 {
					fin[in] = vec4{}
				}
			}
			// interpreter
			var x fpRun
			x.in = &fin
			p.run(&x, func(unit uint8, c vec4, bias, lod float32, useLod bool) vec4 {
				v := testTexel(int(unit), c[0], c[1])
				return vec4{v[0], v[1], v[2], v[3]}
			})
			want := x.h[0]
			if out32 {
				want = x.r[0]
			}
			// translation
			vm := &dxVM{prog: prog, r: make([][4]uint32, dxTemps)}
			for i := 0; i < len(cb); i += 4 {
				vm.cb = append(vm.cb, [4]uint32{fb(cb[i]), fb(cb[i+1]), fb(cb[i+2]), fb(cb[i+3])})
			}
			for in := 1; in < 16; in++ {
				if k := fpInputToVP[in]; k > 0 {
					for j := 0; j < 4; j++ {
						vm.in[k][j] = fb(fin[in][j])
					}
				}
			}
			vm.in[0] = [4]uint32{fb(fin[0][0]), fb(fin[0][1]), 0, 0}
			vm.in[dxPosReg] = [4]uint32{fb(fin[0][2]), 0, 0, fb(fin[0][3])}
			vm.sample = func(unit int, u, v float32) [4]float32 { return testTexel(unit, u, v) }
			func() {
				defer func() {
					if r := recover(); r != nil {
						t.Errorf("%s: vm: %v", name, r)
						ok = false
					}
				}()
				vm.run()
			}()
			if !ok {
				return false
			}
			got := vec4{f(vm.out[0][0]), f(vm.out[0][1]), f(vm.out[0][2]), f(vm.out[0][3])}
			if vm.killed != x.killed {
				t.Errorf("%s (out32 %v): killed %v, want %v", name, out32, vm.killed, x.killed)
				return false
			}
			if x.killed {
				continue
			}
			for i := range got {
				if !close32(got[i], want[i]) {
					t.Errorf("%s (out32 %v): got %v, want %v", name, out32, got, want)
					return false
				}
			}
		}
	}
	return ok
}

// TestFPTranslateCaptured checks every program captured from the game
// (PS3_FPCAPTURE) in PS3_FPDIR.
func TestFPTranslateCaptured(t *testing.T) {
	dir := os.Getenv("PS3_FPDIR")
	if dir == "" {
		t.Skip("PS3_FPDIR not set")
	}
	files, _ := filepath.Glob(filepath.Join(dir, "fp_*.bin"))
	rng := rand.New(rand.NewSource(1))
	bad := 0
	for _, fn := range files {
		b, err := os.ReadFile(fn)
		if err != nil {
			t.Fatal(err)
		}
		p := fpFromBytes(b)
		if !checkFP(t, filepath.Base(fn), p, rng) {
			bad++
			if os.Getenv("PS3_FPVERBOSE") != "" {
				t.Log(fpDisasm(p))
			}
		}
	}
	t.Logf("%d programs, %d differ", len(files), bad)
}

// randomFP makes a program of random arithmetic instructions.
func randomFP(rng *rand.Rand, n int) *fpProg {
	ops := []uint8{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0x10, 0x11, 0x13, 0x14, 0x17, 0x18, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
		0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2e, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x12}
	p := &fpProg{}
	for i := 0; i < n; i++ {
		in := fpInst{op: ops[rng.Intn(len(ops))], cidx: -1, scale: 1, cond: 7}
		in.dst = uint8(rng.Intn(6))
		in.dstHalf = rng.Intn(3) == 0
		for k := range in.mask {
			in.mask[k] = rng.Intn(4) != 0
		}
		in.input = uint8(1 + rng.Intn(13))
		in.tex = uint8(rng.Intn(4))
		in.setCond = rng.Intn(4) == 0
		in.condSet = uint8(rng.Intn(2))
		if rng.Intn(4) == 0 {
			in.cond = uint8(1 + rng.Intn(6))
			in.condReg = uint8(rng.Intn(2))
			for k := range in.condSwz {
				in.condSwz[k] = uint8(rng.Intn(4))
			}
		}
		in.sat = rng.Intn(5) == 0
		if !in.sat && rng.Intn(5) == 0 {
			in.prec = uint8(1 + rng.Intn(3))
		}
		in.scale = fpScale[rng.Intn(8)]
		in.noDest = rng.Intn(10) == 0
		in.bx2 = rng.Intn(5) == 0
		hasConst := false
		for k := range in.src {
			s := &in.src[k]
			s.typ = uint8(rng.Intn(3))
			s.tmp = uint8(rng.Intn(6))
			s.half = rng.Intn(3) == 0
			for c := range s.swz {
				s.swz[c] = uint8(rng.Intn(4))
			}
			s.neg = rng.Intn(4) == 0
			s.abs = rng.Intn(5) == 0
			if rng.Intn(5) == 0 {
				s.prec = uint8(2 + rng.Intn(3))
			}
			s.noop = s.swz == [4]uint8{0, 1, 2, 3} && !s.neg && !s.abs && s.prec == 0
			if s.typ == 2 && fpReads(in.op) > k {
				hasConst = true
			}
			if s.typ == 1 && fpReads(in.op) > k {
				p.inputs |= 1 << in.input
			}
		}
		if hasConst {
			in.cidx = p.consts
			p.consts++
			for c := range in.constant {
				in.constant[c] = rng.Float32()*4 - 2
			}
		}
		switch in.op {
		case 0x17, 0x18:
			p.usesTex |= 1 << in.tex
		case 0x12:
			p.kills = true
		}
		p.inst = append(p.inst, in)
	}
	p.inst[len(p.inst)-1].end = true
	return p
}

func TestFPTranslateRandom(t *testing.T) {
	rng := rand.New(rand.NewSource(2))
	bad := 0
	for i := 0; i < 3000; i++ {
		p := randomFP(rng, 1+rng.Intn(12))
		if !checkFP(t, "random", p, rng) {
			bad++
			t.Log(fpDisasm(p))
			if bad > 5 {
				t.FailNow()
			}
		}
	}
}

// randomFlowFP makes programs with ifs, loops, breaks, returns and kills.
func randomFlowFP(rng *rand.Rand) *fpProg {
	base := randomFP(rng, 40)
	pool := base.inst
	pool[len(pool)-1].end = false
	p := &fpProg{inputs: base.inputs, usesTex: base.usesTex, consts: base.consts}
	next := 0
	arith := func() {
		in := pool[next%len(pool)]
		next++
		if in.op == 0x12 && rng.Intn(2) == 0 {
			in.op = 0x01
		}
		p.inst = append(p.inst, in)
	}
	flow := func(op uint8) *fpInst {
		in := fpInst{op: op, flow: true, cidx: -1, scale: 1, cond: 7}
		if rng.Intn(3) != 0 {
			in.cond = uint8(1 + rng.Intn(6))
			in.condReg = uint8(rng.Intn(2))
			for k := range in.condSwz {
				in.condSwz[k] = uint8(rng.Intn(4))
			}
		}
		p.inst = append(p.inst, in)
		return &p.inst[len(p.inst)-1]
	}
	var block func(depth, loops int)
	block = func(depth, loops int) {
		n := 1 + rng.Intn(4)
		for i := 0; i < n; i++ {
			switch r := rng.Intn(10); {
			case r < 5 || depth > 2:
				arith()
			case r < 7: // if / else
				at := len(p.inst)
				flow(0x42)
				block(depth+1, loops)
				elseAt := len(p.inst)
				if rng.Intn(2) == 0 {
					block(depth+1, loops)
				}
				p.inst[at].elseAt, p.inst[at].endAt = elseAt, len(p.inst)
			case r < 8: // loop
				at := len(p.inst)
				in := flow(0x43 + uint8(rng.Intn(2)))
				in.loopStart = rng.Intn(3)
				in.loopEnd = in.loopStart + rng.Intn(5)
				in.loopInc = 1 + rng.Intn(2)
				block(depth+1, loops+1)
				p.inst[at].endAt = len(p.inst)
			case r < 9 && loops > 0:
				flow(0x40)
			default:
				if rng.Intn(3) == 0 {
					flow(0x45)
				} else {
					arith()
				}
			}
		}
	}
	block(0, 0)
	arith()
	p.inst[len(p.inst)-1].end = true
	return p
}

func TestFPTranslateFlow(t *testing.T) {
	rng := rand.New(rand.NewSource(3))
	bad := 0
	for i := 0; i < 2000; i++ {
		p := randomFlowFP(rng)
		if !checkFP(t, "flow", p, rng) {
			bad++
			t.Log(fpDisasm(p))
			if bad > 3 {
				t.FailNow()
			}
		}
	}
}
