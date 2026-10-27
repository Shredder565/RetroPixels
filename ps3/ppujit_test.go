package ps3

import (
	"fmt"
	"math"
	"math/rand"
	"testing"
)

const (
	jitTestCode = 0x00010000
	jitTestData = 0x30000000
)

// TestJITMatchesInterpreter runs random blocks of the instructions the
// recompiler translates, in the recompiler and in the interpreter, and
// compares the registers and memory after.
func TestJITMatchesInterpreter(t *testing.T) {
	if !jitSupported {
		t.Skip("no recompiler on this system")
	}
	rng := rand.New(rand.NewSource(1))
	trials := 20000
	if testing.Short() {
		trials = 1000
	}
	translated, interp := 0, 0
	for trial := 0; trial < trials; trial++ {
		n := 1 + rng.Intn(24)
		var code []uint32
		for i := 0; i < n; i++ {
			code = append(code, randPPUInst(rng))
		}
		// forward branches
		lk := rng.Intn(2) == 0 // links, or returns (not both: returns could loop)
		for k := 0; k < 3; k++ {
			at := rng.Intn(len(code))
			code[at] = randPPUBranch(rng, at, len(code), lk)
		}
		code = append(code, 0x44000002) // sc: the end

		data := make([]byte, 0x10000)
		rng.Read(data)
		var st PPUState
		for i := range st.GPR {
			st.GPR[i] = rng.Uint64()
			switch rng.Intn(4) {
			case 0:
				st.GPR[i] = uint64(rng.Intn(80)) - 40
			case 1:
				st.GPR[i] = uint64(int64(int32(rng.Uint32())))
			}
		}
		for i := 27; i < 32; i++ {
			st.GPR[i] = uint64(rng.Uint32())<<32 | jitTestData + 0x100 + uint64(rng.Intn(0xfe00))
			if rng.Intn(8) == 0 { // near the end of the page
				st.GPR[i] = jitTestData + 0xffe0 + uint64(rng.Intn(0x20))
			}
		}
		st.GPR[26] = uint64(rng.Intn(256))
		for i := range st.FPR {
			st.FPR[i] = randF64(rng)
			st.VR[i] = Vec{rng.Uint32(), rng.Uint32(), rng.Uint32(), rng.Uint32()}
		}
		st.CR = rng.Uint32()
		st.LR = jitTestCode + uint64(len(code)-1)*4
		st.CTR = uint64(rng.Intn(4))
		if rng.Intn(4) == 0 {
			st.CTR = rng.Uint64()
		}
		st.XER = uint64(rng.Uint32() & 0xe0000000)
		st.PC = jitTestCode

		want, wm, _ := jitRunPPU(false, code, st, data)
		got, gm, j := jitRunPPU(true, code, st, data)
		translated += j.Translated
		interp += j.Interp
		if bad := ppuDiff(want, got, wm, gm); bad != "" {
			s := ""
			for i, w := range code {
				s += fmt.Sprintf("%08X: %08X\n", jitTestCode+4*i, w)
			}
			t.Fatalf("trial %d:\n%s\ncode:\n%sstart: %+v", trial, bad, s, st)
		}
	}
	t.Logf("%d instructions translated, %d interpreted", translated, interp)
}

var testJIT *JIT

func jitRunPPU(useJIT bool, code []uint32, st PPUState, data []byte) (*PPUState, *Memory, *JIT) {
	mem := NewMemory()
	mem.Map(jitTestCode, 0x10000)
	mem.Map(jitTestData, 0x10000)
	for i, w := range code {
		mem.Write32(jitTestCode+uint32(i)*4, w)
	}
	mem.WriteBytes(jitTestData, data)
	c := &PPU{Mem: mem, PPUState: new(PPUState)}
	*c.PPUState = st
	c.Syscall = func(c *PPU) { c.Stop() }
	c.TB = func() uint64 { return 12345 }
	var j *JIT
	if useJIT {
		if testJIT == nil {
			testJIT = newJIT(mem)
		}
		j = testJIT
		j.mem = mem
		mem.codeWrite = j.codeWritten
		j.reset()
		j.Translated, j.Interp = 0, 0
		c.jit = j
	}
	c.Run(100000)
	return c.PPUState, mem, j
}

func ppuDiff(want, got *PPUState, wm, gm *Memory) string {
	bad := ""
	want.syncFPRF()
	got.syncFPRF()
	for i := range want.GPR {
		if want.GPR[i] != got.GPR[i] {
			bad += fmt.Sprintf("r%d = %016X, want %016X\n", i, got.GPR[i], want.GPR[i])
		}
		nan := func(v uint64) bool { return v&0x7ff0000000000000 == 0x7ff0000000000000 && v&0xfffffffffffff != 0 }
		if want.FPR[i] != got.FPR[i] && !(nan(want.FPR[i]) && nan(got.FPR[i])) {
			bad += fmt.Sprintf("f%d = %016X, want %016X\n", i, got.FPR[i], want.FPR[i])
		}
	}
	for i := range want.VR {
		if want.VR[i] != got.VR[i] {
			bad += fmt.Sprintf("v%d = %08X, want %08X\n", i, got.VR[i], want.VR[i])
		}
	}
	for _, r := range []struct {
		name      string
		got, want uint64
	}{{"CR", uint64(got.CR), uint64(want.CR)}, {"LR", got.LR, want.LR}, {"CTR", got.CTR, want.CTR},
		{"XER", got.XER, want.XER}, {"PC", uint64(got.PC), uint64(want.PC)}, {"FPSCR", uint64(got.FPSCR), uint64(want.FPSCR)}} {
		if r.got != r.want {
			bad += fmt.Sprintf("%s = %X, want %X\n", r.name, r.got, r.want)
		}
	}
	for a := uint32(jitTestData); a < jitTestData+0x10000; a += 4 {
		if wm.Read32(a) != gm.Read32(a) {
			bad += fmt.Sprintf("memory %08X = %08X, want %08X\n", a, gm.Read32(a), wm.Read32(a))
			break
		}
	}
	return bad
}

func randF64(rng *rand.Rand) uint64 {
	switch rng.Intn(8) {
	case 0:
		return rng.Uint64()
	case 1:
		return math.Float64bits(math.Inf(1 - 2*rng.Intn(2)))
	case 2:
		return math.Float64bits(float64(float32(rng.NormFloat64() * 1e-40)))
	default:
		return math.Float64bits(float64(float32(rng.NormFloat64() * 100)))
	}
}

// randPPUBranch makes a branch at index at that only goes forward (to at most
// the final sc at index n).
func randPPUBranch(rng *rand.Rand, at, n int, link bool) uint32 {
	off := uint32(4 * (1 + rng.Intn(n-at)))
	bo := []uint32{20, 12, 4, 16, 18, 0, 2, 8, 10}[rng.Intn(9)]
	bi := uint32(rng.Intn(32))
	lk := uint32(0)
	kinds := 3
	if link {
		lk = uint32(rng.Intn(2))
	} else {
		kinds = 4
	}
	switch rng.Intn(kinds) {
	case 0:
		return 18<<26 | off | lk
	case 1, 2:
		return 16<<26 | bo<<21 | bi<<16 | off&0xfffc | lk
	default: // bclr to the sc (LR is set there): no lk, which would change LR
		return 19<<26 | bo<<21 | bi<<16 | 16<<1
	}
}

func randPPUInst(rng *rand.Rand) uint32 {
	r := func(n int) uint32 { return uint32(rng.Intn(n)) }
	dst := func() uint32 { return r(26) } // r0..r25: r26..r31 hold addresses
	base := func() uint32 { return 27 + r(5) }
	disp := func() uint32 { return uint32(int32(rng.Intn(0x200)-0x100)) & 0xffff }
	d := func(op, rd, ra, imm uint32) uint32 { return op<<26 | rd<<21 | ra<<16 | imm&0xffff }
	x := func(rd, ra, rb, xo uint32, rc uint32) uint32 { return 31<<26 | rd<<21 | ra<<16 | rb<<11 | xo<<1 | rc }
	src := func() uint32 { return r(32) }
	rc := func() uint32 { return r(2) }
	switch r(18) {
	case 16, 17: // floating point arithmetic
		f := func() uint32 { return r(32) }
		if r(3) == 0 {
			xos := []uint32{72, 40, 264, 136, 12, 15, 815, 846, 0, 32}
			xo := xos[r(len(xos))]
			if xo == 0 || xo == 32 {
				return 63<<26 | r(8)<<23 | f()<<16 | f()<<11 | xo<<1
			}
			return 63<<26 | f()<<21 | f()<<11 | xo<<1
		}
		op := []uint32{59, 63}[r(2)]
		xos := []uint32{18, 20, 21, 22, 23, 24, 25, 26, 28, 29, 30, 31}
		return op<<26 | f()<<21 | f()<<16 | f()<<11 | f()<<6 | xos[r(len(xos))]<<1
	case 0: // immediate arithmetic
		ops := []uint32{7, 8, 12, 13, 14, 15}
		return d(ops[r(len(ops))], dst(), src(), r(0x10000))
	case 1: // compares
		ops := []uint32{10, 11}
		return d(ops[r(2)], r(8)<<2|r(2), src(), r(0x10000))
	case 2: // immediate logic
		ops := []uint32{24, 25, 26, 27, 28, 29}
		return d(ops[r(len(ops))], src(), dst(), r(0x10000))
	case 3: // 32-bit rotates
		ops := []uint32{20, 21, 23}
		return ops[r(3)]<<26 | src()<<21 | dst()<<16 | src()<<11 | r(32)<<6 | r(32)<<1 | rc()
	case 4: // 64-bit rotates
		if r(4) == 0 {
			return 30<<26 | src()<<21 | dst()<<16 | src()<<11 | r(64)<<5 | (8+r(2))<<1 | rc()
		}
		return 30<<26 | src()<<21 | dst()<<16 | r(32)<<11 | r(64)<<5 | r(4)<<2 | r(2)<<1 | rc()
	case 5, 6: // D-form loads and stores
		ops := []uint32{32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55}
		op := ops[r(len(ops))]
		rd := dst()
		if op >= 48 {
			rd = r(32)
		}
		return d(op, rd, base(), disp())
	case 7: // DS-form
		if r(2) == 0 {
			return d(58, dst(), base(), disp()&^3|r(3))
		}
		return d(62, src(), base(), disp()&^3|r(2))
	case 8, 9: // XO arithmetic
		xos := []uint32{266, 10, 138, 202, 234, 40, 8, 136, 200, 232, 104, 235, 233, 75, 11, 9, 73, 459, 457, 491, 489}
		oe := uint32(0)
		if r(10) == 0 {
			oe = 1
		}
		return x(dst(), src(), src(), oe<<9|xos[r(len(xos))], rc())
	case 10: // compares and logic
		xos := []uint32{0, 32, 28, 60, 444, 412, 316, 124, 476, 284, 444, 444}
		xo := xos[r(len(xos))]
		if xo == 0 || xo == 32 {
			return x(r(8)<<2|r(2), src(), src(), xo, 0)
		}
		return x(src(), dst(), src(), xo, rc())
	case 11: // shifts and the like
		xos := []uint32{24, 536, 27, 539, 824, 826, 827, 26, 58, 922, 954, 986, 792, 794}
		xo := xos[r(len(xos))]
		return x(src(), dst(), src(), xo, rc())
	case 12: // indexed loads
		xos := []uint32{23, 55, 87, 119, 279, 311, 343, 375, 21, 53, 341, 373, 534, 532, 535, 567, 599, 631, 103, 359, 790}
		xo := xos[r(len(xos))]
		if r(2) == 0 {
			return x(dst(), base(), 26, xo, 0)
		}
		return x(dst(), 0, base(), xo, 0)
	case 13: // indexed stores
		xos := []uint32{151, 183, 215, 247, 407, 439, 149, 181, 662, 660, 663, 695, 727, 759, 231, 487, 918}
		xo := xos[r(len(xos))]
		if r(2) == 0 {
			return x(src(), base(), 26, xo, 0)
		}
		return x(src(), 0, base(), xo, 0)
	case 14: // special registers
		switch r(5) {
		case 0:
			spr := []uint32{1, 8, 9}[r(3)]
			return x(dst(), spr&31, spr>>5, 339, 0)
		case 1:
			spr := []uint32{1, 9}[r(2)] // (not LR: the test needs it)
			return x(src(), spr&31, spr>>5, 467, 0)
		case 2:
			return x(dst(), 0, 0, 19, 0)
		case 3:
			return 31<<26 | src()<<21 | r(256)<<12 | 144<<1
		default:
			return x(0, 0, 0, []uint32{598, 278, 854}[r(3)], 0)
		}
	default: // condition register
		if r(4) == 0 {
			return 19<<26 | r(8)<<23 | r(8)<<18
		}
		xos := []uint32{33, 129, 193, 225, 257, 289, 417, 449}
		return 19<<26 | r(32)<<21 | r(32)<<16 | r(32)<<11 | xos[r(len(xos))]<<1
	}
}
