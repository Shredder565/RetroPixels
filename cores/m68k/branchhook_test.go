package m68k

import "testing"

// TestBranchHooks: with BranchHooks on, an A-line / F-line opcode holding a
// backward short branch runs exactly like that Bcc.b; anything else still traps.
func TestBranchHooks(t *testing.T) {
	run := func(op uint16, hooks bool, ccr uint32) (pc, sr uint32, cyc int64) {
		b := &testBus{seed: 7, mem: map[uint32]uint8{}}
		c := New(b)
		c.BranchHooks = hooks
		c.s, c.ipl = 1, 7
		c.A[7] = 0x8000
		c.PC = 0x1000
		c.setCCR(ccr)
		b.mem[0x1000], b.mem[0x1001] = uint8(op>>8), uint8(op)
		for v := uint32(0x28); v < 0x30; v += 4 { // A-line / F-line vectors -> 0x200
			b.mem[v], b.mem[v+1], b.mem[v+2], b.mem[v+3] = 0, 0, 2, 0
		}
		c.Step()
		return c.PC, c.SR(), c.Cycles
	}
	for _, op := range []uint16{0xa7f4, 0xa6f6, 0xf6e8, 0xf4f4, 0xf6da, 0xa6fa, 0xf6e4, 0xa0f0} {
		for _, ccr := range []uint32{0, 4, 1, 8, 2, 0x1f} {
			bcc := 0x6000 | op&0x0fff
			p1, s1, c1 := run(op, true, ccr)
			p2, s2, c2 := run(bcc, false, ccr)
			if p1 != p2 || s1 != s2 || c1 != c2 {
				t.Errorf("%04x ccr %x: hooked pc %x sr %x cyc %d; Bcc pc %x sr %x cyc %d", op, ccr, p1, s1, c1, p2, s2, c2)
			}
		}
	}
	// still traps: hooks off, a forward branch, BSR, a word displacement, -1
	for _, tc := range []struct {
		op    uint16
		hooks bool
	}{{0xa7f4, false}, {0xa704, true}, {0xa1f4, true}, {0xa700, true}, {0xa7ff, true}, {0xf600, true}} {
		if pc, _, _ := run(tc.op, tc.hooks, 0); pc != 0x200 {
			t.Errorf("%04x hooks %v: pc %x, want the trap at 200", tc.op, tc.hooks, pc)
		}
	}
}
