package wiiu

import (
	"fmt"
	"math/rand"
	"testing"
)

// TestJITMatchesInterpreter runs random blocks of the instructions the
// recompiler translates, in the recompiler and in the plain interpreter,
// and compares the registers and memory after.
func TestJITMatchesInterpreter(t *testing.T) {
	if !jitSupported {
		t.Skip("no recompiler on this system")
	}
	rng := rand.New(rand.NewSource(1))
	const codeAt = CodeBase + 0x10000
	const dataAt = DataBase + 0x20000
	jit := newJIT(NewMemory())
	trials := 4000
	if testing.Short() {
		trials = 500
	}
	for trial := 0; trial < trials; trial++ {
		n := 1 + rng.Intn(24)
		var code []uint32
		for i := 0; i < n; i++ {
			code = append(code, randInst(rng))
		}
		branch := rng.Intn(2) == 0
		if branch {
			code = append(code, randBranch(rng))
		}
		code = append(code, 0x44000002) // sc: the end

		var data [0x10000]byte
		rng.Read(data[:])
		var ctx Context
		for i := range ctx.GPR {
			ctx.GPR[i] = rng.Uint32()
			if rng.Intn(3) == 0 {
				ctx.GPR[i] = uint32(rng.Intn(64)) - 32
			}
		}
		for i := 28; i < 32; i++ {
			ctx.GPR[i] = dataAt + 0x100 + uint32(rng.Intn(0xfe00))
			if rng.Intn(8) == 0 { // near the end of the page
				ctx.GPR[i] = dataAt + 0xffe0 + uint32(rng.Intn(0x20))
			}
		}
		ctx.GPR[27] = uint32(rng.Intn(256))
		ctx.CR, ctx.LR, ctx.CTR = rng.Uint32(), codeAt+uint32(rng.Intn(64))*4, uint32(rng.Intn(4))
		ctx.XER = rng.Uint32() & 0xe0000000
		ctx.PC = codeAt

		// mode 0: Step (the plain interpreter), 1: Run with the decoded
		// cache, 2: Run with the recompiler
		run := func(mode int) (*Context, *Memory) {
			mem := NewMemory()
			if mode == 2 {
				mem.jit = jit
				jit.mem = mem
				jit.reset()
				for i := range jit.hostMn {
					jit.hostMn[i] = 0
				}
			}
			for i, w := range code {
				mem.Write32(codeAt+uint32(i)*4, w)
			}
			mem.WriteBytes(dataAt, data[:])
			c := &CPU{Mem: mem, Context: new(Context)}
			*c.Context = ctx
			steps := n
			if branch {
				steps++
			}
			if mode > 0 {
				c.Run(steps)
			} else {
				for i := 0; i < steps; i++ {
					c.Step()
				}
			}
			return c.Context, mem
		}
		want, wm := run(0)
		for mode := 1; mode <= 2; mode++ {
			got, gm := run(mode)
			bad := ""
			for i := range want.GPR {
				if want.GPR[i] != got.GPR[i] {
					bad += fmt.Sprintf("r%d = %08X, want %08X\n", i, got.GPR[i], want.GPR[i])
				}
			}
			for _, r := range []struct {
				name      string
				got, want uint32
			}{{"CR", got.CR, want.CR}, {"LR", got.LR, want.LR}, {"CTR", got.CTR, want.CTR},
				{"XER", got.XER, want.XER}, {"PC", got.PC, want.PC}} {
				if r.got != r.want {
					bad += fmt.Sprintf("%s = %08X, want %08X\n", r.name, r.got, r.want)
				}
			}
			for a := uint32(dataAt); a < dataAt+0x10000; a += 4 {
				if wm.Read32(a) != gm.Read32(a) {
					bad += fmt.Sprintf("memory %08X = %08X, want %08X\n", a, gm.Read32(a), wm.Read32(a))
					break
				}
			}
			if bad != "" {
				var list string
				for i, w := range code {
					list += fmt.Sprintf("  %08X: %08X\n", codeAt+uint32(i)*4, w)
				}
				t.Fatalf("trial %d, mode %d:\n%sstart: %+v\ncode:\n%s", trial, mode, bad, ctx, list)
			}
		}
	}
}

func randInst(rng *rand.Rand) uint32 {
	r := func(n int) uint32 { return uint32(rng.Intn(n)) }
	dst := func() uint32 { return r(27) } // r0..r26: r27..r31 hold addresses
	base := func() uint32 { return 28 + r(4) }
	disp := func() uint32 { return uint32(int32(rng.Intn(0x200)-0x100)) & 0xffff }
	d := func(op, rd, ra, imm uint32) uint32 { return op<<26 | rd<<21 | ra<<16 | imm&0xffff }
	x := func(rd, ra, rb, xo uint32, rc uint32) uint32 { return 31<<26 | rd<<21 | ra<<16 | rb<<11 | xo<<1 | rc }
	src := func() uint32 { return r(32) }
	rc := func() uint32 { return r(2) }
	switch r(14) {
	case 0: // immediate arithmetic
		ops := []uint32{7, 8, 12, 13, 14, 15}
		return d(ops[r(len(ops))], dst(), src(), r(0x10000))
	case 1: // compares
		ops := []uint32{10, 11}
		return d(ops[r(2)], r(8)<<2, src(), r(0x10000))
	case 2: // immediate logic
		ops := []uint32{24, 25, 26, 27, 28, 29}
		return d(ops[r(len(ops))], src(), dst(), r(0x10000))
	case 3: // rotates
		ops := []uint32{20, 21, 23}
		return ops[r(3)]<<26 | src()<<21 | dst()<<16 | src()<<11 | r(32)<<6 | r(32)<<1 | rc()
	case 4: // D-form loads and stores
		ops := []uint32{32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45}
		op := ops[r(len(ops))]
		return d(op, dst(), base(), disp())
	case 5: // lmw / stmw
		if r(2) == 0 {
			return d(46, 20+r(7), base(), disp())
		}
		return d(47, 20+r(12), base(), disp())
	case 6, 7: // XO arithmetic (with OE sometimes)
		xos := []uint32{266, 10, 138, 202, 234, 40, 8, 136, 200, 232, 104, 235, 75, 11, 459, 491}
		oe := uint32(0)
		if r(8) == 0 {
			oe = 0x400
		}
		return x(dst(), src(), src(), xos[r(len(xos))], rc()) | oe
	case 8: // compares, logic, shifts
		xos := []uint32{0, 32, 28, 60, 444, 412, 316, 124, 476, 284, 24, 536, 824, 792, 26, 922, 954}
		xo := xos[r(len(xos))]
		if xo == 0 || xo == 32 {
			return x(r(8)<<2, src(), src(), xo, 0)
		}
		return x(src(), dst(), src(), xo, rc())
	case 9: // indexed loads and stores, dcbz
		xos := []uint32{23, 55, 87, 119, 279, 311, 343, 375, 534, 151, 183, 215, 247, 407, 439, 662, 1014}
		xo := xos[r(len(xos))]
		rd := dst()
		if xo == 1014 {
			rd = 0
		}
		return x(rd, base(), 27, xo, 0)
	case 10: // special registers
		sprs := []uint32{1, 8, 9}
		s := sprs[r(3)]
		spr := (s&31)<<5 | s>>5 // the halves are swapped in the encoding
		if r(2) == 0 {
			return 31<<26 | dst()<<21 | spr<<11 | 339<<1
		}
		return 31<<26 | src()<<21 | spr<<11 | 467<<1
	case 11: // CR moves
		if r(2) == 0 {
			return x(dst(), 0, 0, 19, 0)
		}
		return 31<<26 | src()<<21 | r(256)<<12 | 144<<1
	case 12: // CR logic
		xos := []uint32{33, 129, 193, 225, 257, 289, 417, 449, 0, 150}
		xo := xos[r(len(xos))]
		if xo == 0 {
			return 19<<26 | r(8)<<23 | r(8)<<18
		}
		return 19<<26 | r(32)<<21 | r(32)<<16 | r(32)<<11 | xo<<1
	default: // nops
		xos := []uint32{54, 86, 246, 278, 598, 854}
		return x(0, base(), 27, xos[r(len(xos))], 0)
	}
}

func randBranch(rng *rand.Rand) uint32 {
	r := func(n int) uint32 { return uint32(rng.Intn(n)) }
	bos := []uint32{0, 2, 4, 8, 10, 12, 16, 18, 20}
	bo := bos[r(len(bos))] | r(2) // the hint bit
	lk := r(2)
	switch r(4) {
	case 0:
		return 18<<26 | uint32(rng.Intn(64)-32)*4&0x03fffffc | lk
	case 1:
		return 16<<26 | bo<<21 | r(32)<<16 | uint32(rng.Intn(64)-32)*4&0xfffc | lk
	case 2:
		return 19<<26 | bo<<21 | r(32)<<16 | 16<<1 | lk
	default:
		return 19<<26 | (bo|4)<<21 | r(32)<<16 | 528<<1 | lk
	}
}
