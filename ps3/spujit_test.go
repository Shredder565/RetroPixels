package ps3

import (
	"encoding/binary"
	"fmt"
	"math"
	"math/rand"
	"testing"
)

const spuTestCode = 0x1000

// TestSPUJITMatchesInterpreter runs random blocks of the instructions the
// SPU recompiler translates both ways and compares the registers and local
// store after.
func TestSPUJITMatchesInterpreter(t *testing.T) {
	if !spuJitOK {
		t.Skip("no SPU recompiler on this system")
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
		fused := false
		for i := 0; i < n; i++ {
			w := randSPUInst(rng)
			if w>>28 >= 0xd {
				fused = true
			}
			code = append(code, w)
		}
		for k := 0; k < 3; k++ {
			at := rng.Intn(len(code))
			code[at] = randSPUBranch(rng, at, len(code))
		}
		code = append(code, 0) // stop: the end

		ls := make([]byte, lsSize)
		rng.Read(ls)
		for i, w := range code {
			binary.BigEndian.PutUint32(ls[spuTestCode+4*i:], w)
		}
		var regs [128]Vec
		for r := range regs {
			for k := range regs[r] {
				regs[r][k] = randSPUWord(rng)
			}
		}
		for r := 100; r < 128; r++ { // addresses in the data
			regs[r][0] = 0x8000 + uint32(rng.Intn(0x30000))
		}
		regs[99][0] = spuTestCode + 4*uint32(len(code)-1) // the end, for bi
		want, wls, _ := spuRunTest(false, ls, regs)
		got, gls, j := spuRunTest(true, ls, regs)
		translated += j.Translated
		interp += j.Interp
		bad := ""
		for r := range want {
			for k := range want[r] {
				x, y := want[r][k], got[r][k]
				nan := func(v uint32) bool { return v&0x7f800000 == 0x7f800000 && v&0x7fffff != 0 }
				if x != y && !(fused && (x-y+1 <= 2 || nan(x) && nan(y))) {
					bad += fmt.Sprintf("r%d[%d] = %08X, want %08X\n", r, k, y, x)
				}
			}
		}
		for a := 0; a < lsSize; a += 4 {
			if binary.BigEndian.Uint32(wls[a:]) != binary.BigEndian.Uint32(gls[a:]) {
				bad += fmt.Sprintf("LS %05x = %08X, want %08X\n", a, binary.BigEndian.Uint32(gls[a:]), binary.BigEndian.Uint32(wls[a:]))
				break
			}
		}
		if bad != "" {
			s := ""
			for i, w := range code {
				s += fmt.Sprintf("%05x: %08x  %s\n", spuTestCode+4*i, w, SPUDisasm(uint32(spuTestCode+4*i), w))
			}
			t.Fatalf("trial %d:\n%s\ncode:\n%s", trial, bad, s)
		}
	}
	t.Logf("%d instructions translated, %d interpreted", translated, interp)
}

var testSPUJIT *spuJIT

// spuTestFused: the random blocks have only fused multiply-adds for their
// three-register instructions (which can differ in the last bit: the
// interpreter rounds twice), compared with that allowance and only for
// their own result.
var spuTestFused bool

func TestSPUJITFused(t *testing.T) {
	if !spuJitFMA {
		t.Skip("no FMA")
	}
	rng := rand.New(rand.NewSource(5))
	spuTestFused = true
	defer func() { spuTestFused = false }()
	for trial := 0; trial < 3000; trial++ {
		w := uint32(0xd+rng.Intn(3))<<28 | 3<<21 | uint32(rng.Intn(128))<<14 | uint32(rng.Intn(128))<<7 | uint32(rng.Intn(128))
		ls := make([]byte, lsSize)
		binary.BigEndian.PutUint32(ls[spuTestCode:], w)
		var regs [128]Vec
		for r := range regs {
			for k := range regs[r] {
				regs[r][k] = randSPUWord(rng)
			}
		}
		want, _, _ := spuRunTest(false, ls, regs)
		got, _, _ := spuRunTest(true, ls, regs)
		for k := 0; k < 4; k++ {
			x, y := want[3][k], got[3][k]
			nan := func(v uint32) bool { return v&0x7f800000 == 0x7f800000 && v&0x7fffff != 0 }
			if x != y && x-y+1 > 2 && !(nan(x) && nan(y)) {
				t.Fatalf("%08x %s: lane %d = %08X, want %08X (a %08X b %08X c %08X)", w, SPUDisasm(0, w), k, y, x,
					regs[w>>7&0x7f][k], regs[w>>14&0x7f][k], regs[w&0x7f][k])
			}
		}
	}
}

func spuRunTest(useJIT bool, ls []byte, regs [128]Vec) ([128]Vec, []byte, *spuJIT) {
	c := newSPUCore()
	copy(c.LS, ls)
	c.GPR = regs
	c.PC = spuTestCode
	c.Stop = func(c *SPUCore, code uint32) bool { c.stop = true; return true }
	c.RdCh = func(c *SPUCore, ch uint32) (uint32, bool) { return 7, true }
	c.WrCh = func(c *SPUCore, ch uint32, v uint32) bool { return true }
	c.ChCount = func(c *SPUCore, ch uint32) uint32 { return 1 }
	var j *spuJIT
	if useJIT {
		if testSPUJIT == nil {
			testSPUJIT = newSPUJIT()
		}
		j = testSPUJIT
		j.reset()
		j.Translated, j.Interp = 0, 0
		c.jit = j
	} else {
		c.noJIT = true
	}
	for i := 0; i < 100 && !c.stop; i++ {
		c.Run(100000)
	}
	return c.GPR, c.LS, j
}

func randSPUWord(rng *rand.Rand) uint32 {
	switch rng.Intn(8) {
	case 0:
		return uint32(rng.Intn(64)) - 32
	case 1:
		return math.Float32bits(float32(rng.NormFloat64() * 1e-38)) // tiny and denormal
	case 2:
		return []uint32{0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0x80000000, 0x7f7fffff, 0x4f000000, 0xcf000000}[rng.Intn(8)]
	case 3, 4:
		return math.Float32bits(float32(rng.NormFloat64() * math.Pow(2, float64(rng.Intn(80)-40))))
	default:
		return rng.Uint32()
	}
}

// randSPUBranch: a forward branch at index at (to at most the final stop).
func randSPUBranch(rng *rand.Rand, at, n int) uint32 {
	off := uint32(1 + rng.Intn(n-at)) // in words
	rT := uint32(rng.Intn(99))
	ri16 := func(op uint32) uint32 { return op<<23 | (off&0xffff)<<7 | rT }
	switch rng.Intn(8) {
	case 0:
		return ri16(0x064) // br
	case 1:
		return ri16(0x042)
	case 2:
		return ri16(0x040)
	case 3:
		return ri16(0x046)
	case 4:
		return ri16(0x044)
	case 5: // brsl into a register
		return ri16(0x066)
	case 6: // bi to the end
		return 0x1a8<<21 | 99<<7
	default: // biz / binz / bihz / bihnz to the end
		return (0x128+uint32(rng.Intn(4)))<<21 | 99<<7 | rT
	}
}

func randSPUInst(rng *rand.Rand) uint32 {
	r := func(n int) uint32 { return uint32(rng.Intn(n)) }
	dst := func() uint32 { return r(99) } // r99..r127 hold addresses
	src := func() uint32 { return r(128) }
	addr := func() uint32 { return 100 + r(28) }
	rr := func(op uint32) uint32 { return op<<21 | src()<<14 | src()<<7 | dst() }
	ri7 := func(op uint32) uint32 { return op<<21 | r(128)<<14 | src()<<7 | dst() }
	ri10 := func(op uint32) uint32 { return op<<24 | r(1024)<<14 | src()<<7 | dst() }
	ri16 := func(op uint32) uint32 { return op<<23 | r(0x10000)<<7 | dst() }
	switch r(12) {
	case 0: // RRR
		ops := []uint32{0x8, 0xb, 0xc, 0xb, 0x8}
		if spuTestFused {
			ops = []uint32{0xd, 0xe, 0xf}
		}
		return ops[r(len(ops))]<<28 | dst()<<21 | src()<<14 | src()<<7 | src()
	case 1: // RI10 arithmetic
		ops := []uint32{0x1c, 0x1d, 0x0c, 0x0d, 0x14, 0x15, 0x16, 0x04, 0x05, 0x06, 0x44, 0x45, 0x46, 0x74, 0x75}
		return ri10(ops[r(len(ops))])
	case 2: // RI10 compares
		ops := []uint32{0x7c, 0x4c, 0x5c, 0x7d, 0x4d, 0x5d, 0x7e, 0x4e, 0x5e}
		return ri10(ops[r(len(ops))])
	case 3: // loads and stores
		switch r(5) {
		case 0:
			return 0x34<<24 | r(1024)<<14 | addr()<<7 | dst()
		case 1:
			return 0x24<<24 | r(1024)<<14 | addr()<<7 | src()
		case 2:
			return 0x1c4<<21 | addr()<<14 | addr()<<7 | dst()
		case 3:
			return 0x144<<21 | addr()<<14 | addr()<<7 | src()
		default:
			ops := []uint32{0x061, 0x067, 0x041, 0x047}
			op := ops[r(4)]
			im := 0x2000 + r(0x8000) // (away from the code)
			if op == 0x067 || op == 0x047 {
				im = 0x1000 + r(0x4000)
			}
			return op<<23 | im<<7 | dst()
		}
	case 4: // immediates
		ops := []uint32{0x083, 0x082, 0x081, 0x0c1, 0x065}
		if r(6) == 0 {
			return 0x21<<25 | r(1<<18)<<7 | dst()
		}
		return ri16(ops[r(len(ops))])
	case 5: // RR arithmetic and logic
		ops := []uint32{0x0c0, 0x0c8, 0x040, 0x048, 0x3c4, 0x3c7, 0x3cc, 0x3c5, 0x3c6, 0x3ce, 0x2b6, 0x2ae,
			0x0c1, 0x2c1, 0x041, 0x2c9, 0x241, 0x0c9, 0x049, 0x249, 0x1f0, 0x1b4}
		return rr(ops[r(len(ops))])
	case 6: // shifts by immediates
		ops := []uint32{0x07f, 0x07b, 0x07c, 0x078, 0x07d, 0x079, 0x07e, 0x07a, 0x1ff, 0x1fc, 0x1fd}
		return ri7(ops[r(len(ops))])
	case 7: // shifts by registers
		ops := []uint32{0x1df, 0x1dc, 0x1dd, 0x1cf, 0x1cc, 0x1cd, 0x05b, 0x058, 0x059, 0x05a}
		return rr(ops[r(len(ops))])
	case 8: // compares
		ops := []uint32{0x3c0, 0x240, 0x2c0, 0x3c8, 0x248, 0x2c8, 0x3d0, 0x250, 0x2d0}
		return rr(ops[r(len(ops))])
	case 9: // floats
		ops := []uint32{0x2c4, 0x2c5, 0x2c6, 0x1b8, 0x3d4, 0x3c2, 0x2c2, 0x3ca, 0x2ca}
		return rr(ops[r(len(ops))])
	case 10: // conversions and insertion controls
		if r(2) == 0 {
			return []uint32{0x1d8, 0x1da}[r(2)]<<22 | r(256)<<14 | src()<<7 | dst()
		}
		ops := []uint32{0x1f4, 0x1f5, 0x1f6, 0x1f7, 0x1d4, 0x1d5, 0x1d6, 0x1d7}
		return rr(ops[r(len(ops))])
	default: // nops and the like
		ops := []uint32{0x001, 0x201, 0x002, 0x003, 0x1ac, 0x00c}
		return rr(ops[r(len(ops))])
	}
}
