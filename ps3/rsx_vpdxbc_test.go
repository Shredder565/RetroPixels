package ps3

import (
	"math"
	"math/rand"
	"testing"
)

// randVPProgram makes a straight-line vertex program of n instructions.
func randVPProgram(rng *rand.Rand, n int) []vpInst {
	vecOps := []uint8{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16}
	scaOps := []uint8{0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 0x0d, 0x0e, 0x0f, 0x10}
	src := func() vpSrc {
		s := vpSrc{typ: uint8(1 + rng.Intn(3)), tmp: uint8(rng.Intn(6))}
		for i := range s.swz {
			s.swz[i] = uint8(rng.Intn(4))
		}
		if rng.Intn(3) == 0 {
			s.swz = [4]uint8{0, 1, 2, 3}
		}
		s.neg = rng.Intn(4) == 0
		s.abs = rng.Intn(5) == 0
		s.noop = s.swz == [4]uint8{0, 1, 2, 3} && !s.neg && !s.abs
		return s
	}
	mask := func() [4]bool {
		var m [4]bool
		for i := range m {
			m[i] = rng.Intn(3) != 0
		}
		return m
	}
	var p []vpInst
	for i := 0; i < n; i++ {
		in := vpInst{vecOp: vecOps[rng.Intn(len(vecOps))], scaOp: scaOps[rng.Intn(len(scaOps))]}
		for k := range in.src {
			in.src[k] = src()
		}
		in.input = uint8(rng.Intn(16))
		in.constant = uint16(rng.Intn(470))
		if rng.Intn(5) == 0 {
			in.indexConst = true
			in.constant = uint16(rng.Intn(40))
		}
		in.addrSwz = uint8(rng.Intn(4))
		in.addrSel = uint8(rng.Intn(2))
		in.cond = 7
		if rng.Intn(4) == 0 {
			in.condTest = true
			in.cond = uint8(rng.Intn(8))
		}
		for k := range in.condSwz {
			in.condSwz[k] = uint8(rng.Intn(4))
		}
		in.condSel = uint8(rng.Intn(2))
		in.dstTmp = uint8(rng.Intn(6))
		if rng.Intn(4) == 0 {
			in.dstTmp = 0x3f
		}
		in.scaDstTmp = uint8(rng.Intn(6))
		if rng.Intn(3) == 0 {
			in.scaDstTmp = 0x3f
		}
		in.vecResult = rng.Intn(3) == 0
		in.dst = uint8(rng.Intn(16))
		in.sat = rng.Intn(6) == 0
		in.vecMask, in.scaMask = mask(), mask()
		p = append(p, in)
	}
	// the position from inputs, so it's sane
	last := vpInst{vecOp: 1, scaOp: 0, dstTmp: 0x3f, scaDstTmp: 0x3f, vecResult: true, dst: 0, cond: 7, end: true}
	last.src[0] = vpSrc{typ: 2, swz: [4]uint8{0, 1, 2, 3}, noop: true}
	last.input = 0
	last.vecMask = [4]bool{true, true, true, true}
	return append(p, last)
}

func vpClose(a, b float32) bool {
	if a != a || b != b {
		return true // NaNs come out differently (min/max)
	}
	if math.IsInf(float64(a), 0) || math.IsInf(float64(b), 0) || math.Abs(float64(a)) > 1e15 || math.Abs(float64(b)) > 1e15 {
		return true
	}
	d := math.Abs(float64(a - b))
	return d <= 2e-3*math.Max(1, math.Max(math.Abs(float64(a)), math.Abs(float64(b))))
}

// TestVPTranslate runs random vertex programs both ways.
func TestVPTranslate(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	bad := 0
	for trial := 0; trial < 400 && bad < 5; trial++ {
		prog := randVPProgram(rng, 1+rng.Intn(12))
		pcs, ok := vpTrace(prog, 0, 0)
		if !ok {
			t.Fatalf("trace failed")
		}
		slots := uint32(rng.Intn(1<<16)) | 1
		code, err := translateVP(prog, pcs, slots)
		if err != nil {
			t.Fatal(err)
		}
		ops, err := dxParse(code)
		if err != nil {
			t.Fatal(err)
		}
		d := &rsxDraw{}
		for i := range d.vc {
			for j := 0; j < 4; j++ {
				d.vc[i][j] = rng.Float32()*4 - 2
			}
		}
		cb := make([][4]uint32, vxConstSize)
		for i := 0; i < 512; i++ {
			if i < 468 {
				for j := 0; j < 4; j++ {
					cb[i][j] = fb(d.vc[i][j])
				}
			}
		}
		sx, sy, sz, ox, oy, oz, W, H := float32(640), float32(-360), float32(0.5), float32(640), float32(360), float32(0.5), float32(1280), float32(720)
		cb[vxConstVP] = [4]uint32{fb(sx), fb(sy), fb(sz), 0}
		cb[vxConstVP+1] = [4]uint32{fb(ox), fb(oy), fb(oz), 0}
		cb[vxConstVP+2] = [4]uint32{fb(W), fb(H), 0, 0}
		for v := 0; v < 5; v++ {
			var attrs [16]vec4
			var packed [][4]uint32
			for a := 0; a < 16; a++ {
				if slots>>a&1 == 0 {
					continue
				}
				for j := 0; j < 4; j++ {
					attrs[a][j] = rng.Float32()*6 - 3
				}
				packed = append(packed, [4]uint32{fb(attrs[a][0]), fb(attrs[a][1]), fb(attrs[a][2]), fb(attrs[a][3])})
			}
			attrs[0][3] = 0.5 + rng.Float32()
			packed[0][3] = fb(attrs[0][3])
			var x vpRun
			d.runVP(prog, 0, &attrs, &x)
			vm := &dxVM{prog: ops, r: make([][4]uint32, vxTemps), cb: cb}
			vm.in[0] = [4]uint32{2, 0, 0, 0}
			n := len(packed)
			vm.ld = func(unit int, i uint32) [4]uint32 { return packed[int(i)-2*n] }
			vm.run()
			for k := 1; k < 16; k++ {
				for j := 0; j < 4; j++ {
					if !vpClose(x.out[k][j], f(vm.out[k][j])) {
						t.Errorf("trial %d vertex %d: out %d.%d = %v, want %v", trial, v, k, j, f(vm.out[k][j]), x.out[k][j])
						bad++
					}
				}
			}
			o := x.out[0]
			wc := o[3]
			want := [4]float32{2*(o[0]*sx+ox*wc)/W - wc, wc - 2*(o[1]*sy+oy*wc)/H, o[2]*sz + oz*wc, wc}
			for j := 0; j < 4; j++ {
				if !vpClose(want[j], f(vm.out[0][j])) {
					t.Errorf("trial %d: position.%d = %v, want %v", trial, j, f(vm.out[0][j]), want[j])
					bad++
				}
			}
			if bad >= 5 {
				for _, in := range prog {
					t.Logf("%+v", in)
				}
				break
			}
		}
	}
}
