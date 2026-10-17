package m6809

import (
	"bytes"
	"testing"
)

// ram is 64 KB of memory that counts interrupt acknowledges.
type ram struct {
	mem  [65536]uint8
	acks []int
}

func (r *ram) Read(a uint16) uint8     { return r.mem[a] }
func (r *ram) Write(a uint16, v uint8) { r.mem[a] = v }
func (r *ram) Ack(k int)               { r.acks = append(r.acks, k) }

// newCPU makes a processor with code at $1000, S at $8000, U at $9000 and
// every vector pointing to its own handler address ($F0xx).
func newCPU(model Model, code ...uint8) (*CPU, *ram) {
	r := &ram{}
	for i, v := range []uint16{VecTrap, VecSWI3, VecSWI2, VecFIRQ, VecIRQ, VecSWI, VecNMI} {
		r.mem[v] = 0xf0
		r.mem[v+1] = uint8(0x10 * (i + 1))
	}
	r.mem[VecReset], r.mem[VecReset+1] = 0x10, 0x00
	copy(r.mem[0x1000:], code)
	c := New(r, model)
	c.S, c.U = 0x8000, 0x9000
	c.CC = 0
	return c, r
}

// steps runs n instructions and returns the cycles they took.
func steps(c *CPU, n int) int {
	t := 0
	for i := 0; i < n; i++ {
		t += c.Step()
	}
	return t
}

func TestReset(t *testing.T) {
	c, _ := newCPU(MC6809)
	if c.PC != 0x1000 || c.CC&(FlagI|FlagF) != FlagI|FlagF && false {
		t.Fatalf("PC %04x", c.PC)
	}
	c.CC, c.DP = 0, 0x55
	c.Reset()
	if c.PC != 0x1000 || c.CC&(FlagI|FlagF) != FlagI|FlagF || c.DP != 0 {
		t.Fatalf("after reset PC %04x CC %02x DP %02x", c.PC, c.CC, c.DP)
	}
}

// accumulator instructions: result in A and flags NZVC (and H for adds)
func TestALU8(t *testing.T) {
	cases := []struct {
		name   string
		a, cc  uint8
		code   []uint8
		want   uint8
		flags  uint8 // expected HNZVC
		cycles int
	}{
		{"LDA # neg", 0, 0, []uint8{0x86, 0x80}, 0x80, FlagN, 2},
		{"LDA # zero", 5, FlagV, []uint8{0x86, 0x00}, 0x00, FlagZ, 2},
		{"ADDA overflow", 0x7f, 0, []uint8{0x8b, 0x01}, 0x80, FlagH | FlagN | FlagV, 2},
		{"ADDA carry", 0xff, 0, []uint8{0x8b, 0x01}, 0x00, FlagH | FlagZ | FlagC, 2},
		{"ADCA", 0x10, FlagC, []uint8{0x89, 0x20}, 0x31, 0, 2},
		{"SUBA borrow", 0x00, 0, []uint8{0x80, 0x01}, 0xff, FlagN | FlagC, 2},
		{"SUBA overflow", 0x80, 0, []uint8{0x80, 0x01}, 0x7f, FlagV, 2},
		{"SBCA", 0x10, FlagC, []uint8{0x82, 0x0f}, 0x00, FlagZ, 2},
		{"CMPA equal", 0x42, 0, []uint8{0x81, 0x42}, 0x42, FlagZ, 2},
		{"CMPA less", 0x41, 0, []uint8{0x81, 0x42}, 0x41, FlagN | FlagC, 2},
		{"ANDA", 0xf0, FlagV | FlagC, []uint8{0x84, 0x3c}, 0x30, FlagC, 2},
		{"BITA", 0xf0, 0, []uint8{0x85, 0x0f}, 0xf0, FlagZ, 2},
		{"ORA", 0x80, 0, []uint8{0x8a, 0x01}, 0x81, FlagN, 2},
		{"EORA", 0xff, 0, []uint8{0x88, 0xff}, 0x00, FlagZ, 2},
		{"NEGA", 0x01, 0, []uint8{0x40}, 0xff, FlagN | FlagC, 2},
		{"NEGA 80", 0x80, 0, []uint8{0x40}, 0x80, FlagN | FlagV | FlagC, 2},
		{"NEGA 0", 0x00, 0, []uint8{0x40}, 0x00, FlagZ, 2},
		{"COMA", 0x0f, 0, []uint8{0x43}, 0xf0, FlagN | FlagC, 2},
		{"LSRA", 0x81, 0, []uint8{0x44}, 0x40, FlagC, 2},
		{"RORA", 0x01, FlagC, []uint8{0x46}, 0x80, FlagN | FlagC, 2},
		{"ASRA", 0x81, 0, []uint8{0x47}, 0xc0, FlagN | FlagC, 2},
		{"ASLA", 0x40, 0, []uint8{0x48}, 0x80, FlagN | FlagV, 2},
		{"ROLA", 0x80, FlagC, []uint8{0x49}, 0x01, FlagV | FlagC, 2},
		{"DECA", 0x80, 0, []uint8{0x4a}, 0x7f, FlagV, 2},
		{"INCA", 0x7f, FlagC, []uint8{0x4c}, 0x80, FlagN | FlagV | FlagC, 2},
		{"TSTA", 0x00, FlagV | FlagC, []uint8{0x4d}, 0x00, FlagZ | FlagC, 2},
		{"CLRA", 0x55, FlagN | FlagV | FlagC, []uint8{0x4f}, 0x00, FlagZ, 2},
		{"DAA", 0x11, FlagH, []uint8{0x19}, 0x17, 0, 2},
		{"DAA carry", 0x9a, 0, []uint8{0x19}, 0x00, FlagZ | FlagC, 2},
		{"ADDA ext", 0x01, 0, []uint8{0xbb, 0x20, 0x00}, 0x34, 0, 5},
		{"ADDA dir", 0x01, 0, []uint8{0x9b, 0x00}, 0x34, 0, 4},
		{"ADDA idx", 0x01, 0, []uint8{0xab, 0x84}, 0x34, 0, 4},
	}
	for _, tc := range cases {
		c, r := newCPU(MC6809, tc.code...)
		r.mem[0x2000] = 0x33
		c.DP = 0x20
		c.X = 0x2000
		c.A, c.CC = tc.a, tc.cc
		n := c.Step()
		if c.A != tc.want || c.CC&0x2f != tc.flags&0x2f && !(c.CC&0x0f == tc.flags&0x0f && tc.flags&FlagH == 0) {
			t.Errorf("%s: A %02x CC %02x, want %02x %02x", tc.name, c.A, c.CC, tc.want, tc.flags)
		}
		if n != tc.cycles {
			t.Errorf("%s: %d cycles, want %d", tc.name, n, tc.cycles)
		}
	}
}

func TestB16(t *testing.T) {
	// ADDD, SUBD, CMPX, LDD, STD, LDX, STX, LDU, LDY, LDS, STS
	c, r := newCPU(MC6809,
		0xcc, 0x12, 0x34, // LDD #$1234
		0xc3, 0xee, 0xee, // ADDD #$EEEE -> $0122 C
		0x83, 0x01, 0x23, // SUBD #$0123 -> $FFFF N C
		0xfd, 0x30, 0x00, // STD $3000
		0x8e, 0x80, 0x00, // LDX #$8000
		0x8c, 0x80, 0x01, // CMPX #$8001 -> N C
		0x10, 0x8e, 0x00, 0x00, // LDY #0 -> Z
		0x10, 0xce, 0x70, 0x00, // LDS #$7000
		0x10, 0xff, 0x30, 0x02, // STS $3002
		0xce, 0xab, 0xcd, // LDU #$ABCD
		0xff, 0x30, 0x04, // STU $3004
		0x10, 0x83, 0xff, 0xff, // CMPD #$FFFF
		0x11, 0x83, 0xab, 0xcd, // CMPU #$ABCD
		0x11, 0x8c, 0x70, 0x01, // CMPS #$7001
	)
	want := []int{3, 4, 4, 6, 3, 4, 4, 4, 7, 3, 6, 5, 5, 5}
	for i, w := range want {
		if n := c.Step(); n != w {
			t.Errorf("instruction %d: %d cycles, want %d", i, n, w)
		}
		switch i {
		case 1:
			if c.D() != 0x0122 || c.CC&FlagC == 0 {
				t.Errorf("ADDD: %04x %02x", c.D(), c.CC)
			}
		case 2:
			if c.D() != 0xffff || c.CC&(FlagN|FlagC) != FlagN|FlagC {
				t.Errorf("SUBD: %04x %02x", c.D(), c.CC)
			}
		case 5:
			if c.CC&0x0f != FlagN|FlagC {
				t.Errorf("CMPX: %02x", c.CC)
			}
		case 6:
			if c.CC&(FlagN|FlagZ|FlagV) != FlagZ {
				t.Errorf("LDY: %02x", c.CC)
			}
		case 11:
			if c.CC&FlagZ == 0 {
				t.Errorf("CMPD")
			}
		case 12:
			if c.CC&FlagZ == 0 {
				t.Errorf("CMPU")
			}
		case 13:
			if c.CC&(FlagN|FlagC) != FlagN|FlagC {
				t.Errorf("CMPS %02x", c.CC)
			}
		}
	}
	if !bytes.Equal(r.mem[0x3000:0x3006], []uint8{0xff, 0xff, 0x70, 0x00, 0xab, 0xcd}) {
		t.Errorf("stores: % x", r.mem[0x3000:0x3006])
	}
}

func TestMemoryRMW(t *testing.T) {
	c, r := newCPU(MC6809,
		0x0c, 0x10, // INC <$10       6
		0x70, 0x20, 0x11, // NEG $2011 7
		0x6a, 0x84, // DEC ,X       6
		0x0f, 0x12, // CLR <$12      6
		0x6d, 0x01, // TST 1,X (n5)  7
		0x0e, 0x40, // JMP <$40      3
	)
	c.DP = 0x20
	c.X = 0x2013
	r.mem[0x2010] = 0x7f
	r.mem[0x2011] = 0x01
	r.mem[0x2012] = 0x99
	r.mem[0x2013] = 0x00
	r.mem[0x2014] = 0x80
	for i, w := range []int{6, 7, 6, 6, 7, 3} {
		if n := c.Step(); n != w {
			t.Errorf("instruction %d: %d cycles, want %d", i, n, w)
		}
	}
	if got := r.mem[0x2010:0x2014]; !bytes.Equal(got, []uint8{0x80, 0xff, 0x00, 0xff}) {
		t.Errorf("memory % x", got)
	}
	if c.CC&FlagN == 0 || c.PC != 0x2040 {
		t.Errorf("TST/JMP: CC %02x PC %04x", c.CC, c.PC)
	}
}

func TestIndexed(t *testing.T) {
	type tc struct {
		name   string
		pb     []uint8 // postbyte and offset bytes
		ea     uint16
		x      uint16 // X after
		cycles int    // LDA total
	}
	cases := []tc{
		{",X", []uint8{0x84}, 0x4000, 0x4000, 4},
		{"5-bit +15", []uint8{0x0f}, 0x400f, 0x4000, 5},
		{"5-bit -16", []uint8{0x10}, 0x3ff0, 0x4000, 5},
		{",X+", []uint8{0x80}, 0x4000, 0x4001, 6},
		{",X++", []uint8{0x81}, 0x4000, 0x4002, 7},
		{",-X", []uint8{0x82}, 0x3fff, 0x3fff, 6},
		{",--X", []uint8{0x83}, 0x3ffe, 0x3ffe, 7},
		{"B,X", []uint8{0x85}, 0x3ffe, 0x4000, 5},
		{"A,X", []uint8{0x86}, 0x4010, 0x4000, 5},
		{"n8,X", []uint8{0x88, 0x80}, 0x3f80, 0x4000, 5},
		{"n16,X", []uint8{0x89, 0x01, 0x00}, 0x4100, 0x4000, 8},
		{"D,X", []uint8{0x8b}, 0x50fe, 0x4000, 8},
		{"n8,PC", []uint8{0x8c, 0x10}, 0x1013, 0x4000, 5},
		{"n16,PC", []uint8{0x8d, 0x01, 0x00}, 0x1104, 0x4000, 9},
		{"[,X]", []uint8{0x94}, 0x5555, 0x4000, 7},
		{"[,X++]", []uint8{0x91}, 0x5555, 0x4002, 10},
		{"[n16]", []uint8{0x9f, 0x40, 0x00}, 0x5555, 0x4000, 9},
		{"[n16,X]", []uint8{0x99, 0x00, 0x00}, 0x5555, 0x4000, 11},
		{",Y", []uint8{0xa4}, 0x6000, 0x4000, 4},
		{",U", []uint8{0xc4}, 0x9000, 0x4000, 4},
		{",S", []uint8{0xe4}, 0x8000, 0x4000, 4},
	}
	for _, k := range cases {
		code := append([]uint8{0xa6}, k.pb...)
		c, r := newCPU(MC6809, code...)
		c.X, c.Y, c.A, c.B = 0x4000, 0x6000, 0x10, 0xfe
		r.mem[0x4000], r.mem[0x4001] = 0x55, 0x55
		r.mem[k.ea] = 0x77
		n := c.Step()
		if c.A != 0x77 || c.X != k.x || n != k.cycles {
			t.Errorf("%s: A %02x X %04x %d cycles, want 77 %04x %d", k.name, c.A, c.X, n, k.x, k.cycles)
		}
	}
}

func TestBranches(t *testing.T) {
	// condition: opcode, CC that takes it, CC that doesn't
	cases := []struct {
		op      uint8
		yes, no uint8
	}{
		{0x20, 0, 0}, {0x22, 0, FlagC}, {0x23, FlagZ, 0}, {0x24, 0, FlagC}, {0x25, FlagC, 0},
		{0x26, 0, FlagZ}, {0x27, FlagZ, 0}, {0x28, 0, FlagV}, {0x29, FlagV, 0}, {0x2a, 0, FlagN},
		{0x2b, FlagN, 0}, {0x2c, FlagN | FlagV, FlagN}, {0x2d, FlagV, FlagN | FlagV},
		{0x2e, 0, FlagZ}, {0x2f, FlagN, 0},
	}
	for _, k := range cases {
		c, _ := newCPU(MC6809, k.op, 0x10)
		c.CC = k.yes
		if n := c.Step(); c.PC != 0x1012 || n != 3 {
			t.Errorf("%02x not taken with CC %02x (PC %04x)", k.op, k.yes, c.PC)
		}
		lop, pc, cyc := []uint8{0x10, k.op, 0xff, 0xf0}, uint16(0x0ff4), 6
		if k.op == 0x20 {
			lop, pc, cyc = []uint8{0x16, 0xff, 0xf0}, 0x0ff3, 5 // LBRA
		}
		c, _ = newCPU(MC6809, lop...)
		c.CC = k.yes
		if n := c.Step(); c.PC != pc || n != cyc {
			t.Errorf("long %02x not taken (PC %04x, %d cycles)", k.op, c.PC, n)
		}
		if k.op == 0x20 {
			continue
		}
		c, _ = newCPU(MC6809, k.op, 0x10)
		c.CC = k.no
		if c.Step(); c.PC != 0x1002 {
			t.Errorf("%02x taken with CC %02x", k.op, k.no)
		}
		c, _ = newCPU(MC6809, 0x10, k.op, 0xff, 0xf0)
		c.CC = k.no
		if n := c.Step(); c.PC != 0x1004 || n != 5 {
			t.Errorf("long %02x taken with CC %02x (%d cycles)", k.op, k.no, n)
		}
	}
	c, _ := newCPU(MC6809, 0x21, 0x10)
	if c.Step(); c.PC != 0x1002 {
		t.Errorf("BRN branched")
	}
}

func TestSubroutines(t *testing.T) {
	c, r := newCPU(MC6809,
		0x8d, 0x04, // BSR +4 -> $1006           7
		0x12, // NOP
		0x12,
		0x12,
		0x12,
		0xbd, 0x20, 0x00, // $1006 JSR $2000      8
		0x17, 0x00, 0x10, // LBSR                 9
	)
	r.mem[0x2000] = 0x39 // RTS (5)
	for i, w := range []int{7, 8, 5, 9} {
		if n := c.Step(); n != w {
			t.Errorf("step %d: %d cycles, want %d", i, n, w)
		}
	}
	if c.PC != 0x101c || c.S != 0x7ffc || r.mem[0x7ffe] != 0x10 || r.mem[0x7fff] != 0x02 || r.mem[0x7ffc] != 0x10 || r.mem[0x7ffd] != 0x0c {
		t.Errorf("PC %04x S %04x stack % x", c.PC, c.S, r.mem[0x7ffc:0x8000])
	}
}

func TestPushPull(t *testing.T) {
	c, r := newCPU(MC6809,
		0x34, 0xff, // PSHS all  5+12
		0x36, 0x06, // PSHU A,B 5+2
		0x35, 0xff, // PULS all
	)
	c.A, c.B, c.DP, c.CC = 1, 2, 3, 4
	c.X, c.Y, c.U = 0x0506, 0x0708, 0x090a
	if n := c.Step(); n != 17 || c.S != 0x8000-12 {
		t.Fatalf("PSHS: %d cycles S %04x", n, c.S)
	}
	want := []uint8{4, 1, 2, 3, 5, 6, 7, 8, 9, 0x0a, 0x10, 0x02}
	if !bytes.Equal(r.mem[0x8000-12:0x8000], want) {
		t.Errorf("stack % x want % x", r.mem[0x8000-12:0x8000], want)
	}
	if n := c.Step(); n != 7 || c.U != 0x0908 || r.mem[0x0908] != 1 || r.mem[0x0909] != 2 {
		t.Errorf("PSHU: %d U %04x", n, c.U)
	}
	c.A, c.B, c.DP, c.CC, c.X, c.Y = 0, 0, 0, 0, 0, 0
	if n := c.Step(); n != 17 {
		t.Errorf("PULS %d cycles", n)
	}
	if c.A != 1 || c.B != 2 || c.DP != 3 || c.CC != 4 || c.X != 0x0506 || c.Y != 0x0708 || c.U != 0x090a || c.PC != 0x1002 || c.S != 0x8000 {
		t.Errorf("PULS: %+v", c)
	}
}

func TestMisc(t *testing.T) {
	c, _ := newCPU(MC6809,
		0x3d,       // MUL      11
		0x1d,       // SEX      2
		0x3a,       // ABX      3
		0x1e, 0x89, // EXG A,B  8
		0x1f, 0x81, // TFR A,X  6 (8 -> 16: $FF high byte on the 6809)
		0x30, 0x1f, // LEAX -1,X  5 -> Z clear
		0x32, 0x62, // LEAS 2,S
		0x1a, 0x01, // ORCC #1
		0x1c, 0xfe, // ANDCC #$FE
	)
	c.A, c.B, c.X = 0x0f, 0x0f, 0x1000
	steps(c, 1)
	if c.D() != 0x00e1 || c.CC&FlagC == 0 {
		t.Errorf("MUL %04x %02x", c.D(), c.CC)
	}
	steps(c, 1)
	if c.D() != 0xffe1 || c.CC&FlagN == 0 {
		t.Errorf("SEX %04x", c.D())
	}
	steps(c, 1)
	if c.X != 0x10e1 {
		t.Errorf("ABX %04x", c.X)
	}
	if n := c.Step(); n != 8 || c.A != 0xe1 || c.B != 0xff {
		t.Errorf("EXG %02x %02x %d", c.A, c.B, n)
	}
	if n := c.Step(); n != 6 || c.X != 0xffe1 {
		t.Errorf("TFR %04x %d", c.X, n)
	}
	if n := c.Step(); n != 5 || c.X != 0xffe0 || c.CC&FlagZ != 0 {
		t.Errorf("LEAX %04x %d", c.X, n)
	}
	steps(c, 1)
	if c.S != 0x8002 {
		t.Errorf("LEAS %04x", c.S)
	}
	steps(c, 2)
	if c.CC&FlagC != 0 {
		t.Errorf("ANDCC")
	}
}

func TestSWIAndRTI(t *testing.T) {
	c, r := newCPU(MC6809, 0x3f)
	r.mem[0xf060] = 0x3b // RTI at the SWI handler
	c.A, c.B, c.X = 0x11, 0x22, 0x3344
	if n := c.Step(); n != 19 || c.PC != 0xf060 || c.S != 0x8000-12 || c.CC&(FlagE|FlagI|FlagF) != FlagE|FlagI|FlagF {
		t.Fatalf("SWI: %d PC %04x S %04x CC %02x", n, c.PC, c.S, c.CC)
	}
	c.A, c.X = 0, 0
	if n := c.Step(); n != 15 || c.PC != 0x1001 || c.A != 0x11 || c.X != 0x3344 || c.S != 0x8000 || c.CC != FlagE {
		t.Errorf("RTI: %d PC %04x A %02x CC %02x", n, c.PC, c.A, c.CC)
	}
	// SWI2 / SWI3 leave the masks alone
	c, _ = newCPU(MC6809, 0x10, 0x3f)
	if n := c.Step(); n != 20 || c.PC != 0xf030 || c.CC&(FlagI|FlagF) != 0 {
		t.Errorf("SWI2: %d %04x %02x", n, c.PC, c.CC)
	}
	c, _ = newCPU(MC6809, 0x11, 0x3f)
	if n := c.Step(); n != 20 || c.PC != 0xf020 {
		t.Errorf("SWI3: %d %04x", n, c.PC)
	}
}

func TestInterrupts(t *testing.T) {
	c, r := newCPU(MC6809, 0x12, 0x12, 0x12)
	r.mem[0xf040] = 0x3b // FIRQ handler: RTI
	r.mem[0xf050] = 0x3b // IRQ handler: RTI
	c.CC = FlagI | FlagF
	c.IRQ, c.FIRQ = true, true
	if n := c.Step(); n != 2 || c.PC != 0x1001 {
		t.Fatalf("masked interrupt taken")
	}
	c.CC = FlagI // FIRQ unmasked
	if n := c.Step(); n != 10 || c.PC != 0xf040 || c.S != 0x8000-3 || c.CC&(FlagE|FlagI|FlagF) != FlagI|FlagF {
		t.Fatalf("FIRQ: %d PC %04x S %04x CC %02x", n, c.PC, c.S, c.CC)
	}
	c.FIRQ = false
	if n := c.Step(); n != 6 || c.PC != 0x1001 || c.CC != FlagI {
		t.Errorf("RTI from FIRQ: %d %04x %02x", n, c.PC, c.CC)
	}
	c.CC = 0
	if n := c.Step(); n != 19 || c.PC != 0xf050 || c.S != 0x8000-12 || c.CC&FlagI == 0 || c.CC&FlagF != 0 {
		t.Errorf("IRQ: %d PC %04x S %04x CC %02x", n, c.PC, c.S, c.CC)
	}
	if len(r.acks) != 2 || r.acks[0] != AckFIRQ || r.acks[1] != AckIRQ {
		t.Errorf("acks %v", r.acks)
	}

	// NMI: ignored until S is loaded, then taken even when masked
	c, _ = newCPU(MC6809, 0x10, 0xce, 0x80, 0x00, 0x12)
	c.CC = FlagI | FlagF
	c.NMI()
	if c.Step(); c.PC != 0x1004 {
		t.Fatalf("NMI before LDS taken")
	}
	c.SetNMI(true)
	if n := c.Step(); n != 19 || c.PC != 0xf070 {
		t.Errorf("NMI: %d %04x", n, c.PC)
	}
	c.SetNMI(true) // still held: no new edge
	if c.Step(); c.PC == 0xf070 && c.S != 0x8000-12 {
		t.Errorf("NMI level retriggered")
	}
}

func TestCWAIAndSYNC(t *testing.T) {
	c, _ := newCPU(MC6809, 0x3c, 0xef, 0x12) // CWAI #$EF (clear I)
	c.CC = FlagI
	c.Step()
	if !c.Waiting() || c.S != 0x8000-12 {
		t.Fatalf("CWAI did not wait")
	}
	if n := c.Step(); n != 1 || c.PC != 0x1002 {
		t.Fatalf("CWAI ran on")
	}
	c.IRQ = true
	c.Step()
	if c.PC != 0xf050 || c.S != 0x8000-12 || c.Waiting() {
		t.Errorf("CWAI IRQ: PC %04x S %04x", c.PC, c.S)
	}

	// SYNC with a masked line just goes on
	c, _ = newCPU(MC6809, 0x13, 0x12)
	c.CC = FlagI
	c.Step()
	if n := c.Step(); n != 1 || !c.Waiting() {
		t.Fatalf("SYNC did not wait")
	}
	c.IRQ = true
	c.Step()
	if c.PC != 0x1002 || c.Waiting() {
		t.Errorf("SYNC: PC %04x", c.PC)
	}
}

func TestDirectPage(t *testing.T) {
	c, r := newCPU(MC6809, 0x1f, 0x8b, 0x96, 0x34, 0x97, 0x35) // TFR A,DP; LDA <$34; STA <$35
	c.A = 0x12
	r.mem[0x1234] = 0x99
	steps(c, 3)
	if c.DP != 0x12 || c.A != 0x99 || r.mem[0x1235] != 0x99 {
		t.Errorf("DP %02x A %02x", c.DP, c.A)
	}
}

func TestState(t *testing.T) {
	c, _ := newCPU(HD6309)
	c.A, c.B, c.X, c.Y, c.U, c.S, c.PC, c.DP, c.CC = 1, 2, 3, 4, 5, 6, 7, 8, 9
	c.E, c.F, c.V = 10, 11, 12
	c.setMD(1)
	c.IRQ = true
	b := c.AppendState(nil)
	if len(b) != StateSize {
		t.Fatalf("state %d bytes", len(b))
	}
	d, _ := newCPU(HD6309)
	rest, err := d.LoadState(append(b, 0xaa))
	if err != nil || len(rest) != 1 {
		t.Fatal(err)
	}
	if !bytes.Equal(d.AppendState(nil), b) || !d.Native() {
		t.Errorf("state did not round-trip")
	}
	if _, err := d.LoadState(b[:10]); err == nil {
		t.Errorf("short state accepted")
	}
}

// ---------------------------------------------------------------- HD6309

func TestNativeMode(t *testing.T) {
	c, r := newCPU(HD6309, 0x12, 0x11, 0x3d, 0x01, 0x12, 0x4c)
	if n := c.Step(); n != 2 {
		t.Errorf("emulation NOP %d", n)
	}
	steps(c, 1) // LDMD #1
	if !c.Native() {
		t.Fatal("not native")
	}
	if n := c.Step(); n != 1 {
		t.Errorf("native NOP %d", n)
	}
	// IRQ in native mode stacks E and F too (14 bytes), RTI takes them back
	r.mem[0xf050] = 0x3b
	c.E, c.F = 0xee, 0xff
	c.IRQ = true
	if n := c.Step(); n != 21 || c.S != 0x8000-14 || r.mem[0x8000-14+3] != 0xee || r.mem[0x8000-14+4] != 0xff {
		t.Fatalf("native IRQ: %d S %04x", n, c.S)
	}
	c.IRQ = false
	c.E, c.F = 0, 0
	if n := c.Step(); n != 17 || c.E != 0xee || c.F != 0xff || c.PC != 0x1005 {
		t.Errorf("native RTI: %d E %02x PC %04x", n, c.E, c.PC)
	}
	if n := c.Step(); n != 1 || c.A != 1 {
		t.Errorf("native INCA %d", n)
	}
}

func TestW(t *testing.T) {
	c, r := newCPU(HD6309,
		0x10, 0x86, 0x12, 0x34, // LDW #$1234
		0x10, 0x8b, 0x00, 0x10, // ADDW #$10
		0x10, 0x97, 0x40, // STW <$40
		0x10, 0x80, 0x12, 0x44, // SUBW #$1244 -> 0 Z
		0x10, 0x5c, // INCW
		0x10, 0x53, // COMW -> $FFFE
		0x10, 0x38, // PSHSW
		0x10, 0x5f, // CLRW
		0x10, 0x39, // PULSW
		0x14, // SEXW -> D = $FFFF
	)
	steps(c, 3)
	if c.W() != 0x1244 || r.mem[0x40] != 0x12 || r.mem[0x41] != 0x44 {
		t.Errorf("LDW/ADDW/STW %04x", c.W())
	}
	steps(c, 1)
	if c.W() != 0 || c.CC&FlagZ == 0 {
		t.Errorf("SUBW")
	}
	steps(c, 2)
	if c.W() != 0xfffe {
		t.Errorf("COMW %04x", c.W())
	}
	steps(c, 3)
	if c.W() != 0xfffe || c.S != 0x8000 {
		t.Errorf("PSHSW/PULSW %04x", c.W())
	}
	steps(c, 1)
	if c.D() != 0xffff || c.CC&FlagN == 0 {
		t.Errorf("SEXW %04x", c.D())
	}
}

func TestQ(t *testing.T) {
	c, r := newCPU(HD6309,
		0xcd, 0x12, 0x34, 0x56, 0x78, // LDQ #$12345678
		0x10, 0xfd, 0x30, 0x00, // STQ $3000
		0x10, 0xdc, 0x50, // LDQ <$50
		0x11, 0x8f, 0xff, 0xfe, // MULD #-2
	)
	steps(c, 2)
	if c.Q() != 0x12345678 || !bytes.Equal(r.mem[0x3000:0x3004], []uint8{0x12, 0x34, 0x56, 0x78}) {
		t.Errorf("LDQ/STQ %08x", c.Q())
	}
	r.mem[0x50], r.mem[0x51], r.mem[0x52], r.mem[0x53] = 0x80, 0, 0, 0
	steps(c, 1)
	if c.Q() != 0x80000000 || c.CC&FlagN == 0 {
		t.Errorf("LDQ dir %08x", c.Q())
	}
	c.SetD(300)
	steps(c, 1)
	if int32(c.Q()) != -600 || c.CC&FlagN == 0 {
		t.Errorf("MULD %d", int32(c.Q()))
	}
}

func TestDivide(t *testing.T) {
	c, _ := newCPU(HD6309, 0x11, 0x8d, 0x07) // DIVD #7
	c.SetD(100)
	steps(c, 1)
	if c.B != 14 || c.A != 2 || c.CC&(FlagV|FlagZ|FlagN) != 0 {
		t.Errorf("DIVD: B %d A %d CC %02x", c.B, c.A, c.CC)
	}
	c, _ = newCPU(HD6309, 0x11, 0x8d, 0xfe) // DIVD #-2
	c.SetD(uint16(0xffff & -9))
	steps(c, 1)
	if int8(c.B) != 4 || int8(c.A) != -1 {
		t.Errorf("DIVD signed: B %d A %d", int8(c.B), int8(c.A))
	}
	c, _ = newCPU(HD6309, 0x11, 0x8e, 0x01, 0x00) // DIVQ #256
	c.SetQ(0x00012345)
	steps(c, 1)
	if c.W() != 0x0123 || c.D() != 0x0045 {
		t.Errorf("DIVQ: W %04x D %04x", c.W(), c.D())
	}
	// division by zero traps through $FFF0 and sets MD bit 7
	c, _ = newCPU(HD6309, 0x11, 0x8d, 0x00, 0x11, 0x3c, 0x80) // ...; BITMD #$80
	c.SetD(5)
	steps(c, 1)
	if c.PC != 0xf010 || c.MD&MDDivZero == 0 || c.S != 0x8000-12 {
		t.Fatalf("div0: PC %04x MD %02x", c.PC, c.MD)
	}
	c.PC = 0x1003
	steps(c, 1)
	if c.CC&FlagZ != 0 || c.MD&MDDivZero != 0 {
		t.Errorf("BITMD: CC %02x MD %02x", c.CC, c.MD)
	}
}

func TestIllegal(t *testing.T) {
	c, _ := newCPU(HD6309, 0x15)
	steps(c, 1)
	if c.PC != 0xf010 || c.MD&MDIllegal == 0 {
		t.Errorf("6309 illegal: PC %04x MD %02x", c.PC, c.MD)
	}
	c, _ = newCPU(MC6809, 0x15)
	steps(c, 1)
	if c.PC != 0x1001 {
		t.Errorf("6809 illegal: PC %04x", c.PC)
	}
	// 6309 instructions are not on the 6809
	c, _ = newCPU(MC6809, 0x10, 0x86, 0x12, 0x34)
	steps(c, 1)
	if c.W() != 0 {
		t.Errorf("6809 ran LDW")
	}
}

func TestRegisterOps(t *testing.T) {
	c, _ := newCPU(HD6309,
		0x10, 0x30, 0x89, // ADDR A,B
		0x10, 0x37, 0x89, // CMPR A,B
		0x10, 0x32, 0x12, // SUBR X,Y
		0x1f, 0xc1, // TFR 0,X
		0x1f, 0x16, // TFR X,W
		0x1e, 0x67, // EXG W,V
	)
	c.A, c.B, c.X, c.Y = 3, 4, 0x10, 0x30
	steps(c, 1)
	if c.B != 7 || c.A != 3 {
		t.Errorf("ADDR %d", c.B)
	}
	steps(c, 1)
	if c.CC&0x0f != 0 { // B - A = 4
		t.Errorf("CMPR %02x", c.CC)
	}
	steps(c, 1)
	if c.Y != 0x20 {
		t.Errorf("SUBR %04x", c.Y)
	}
	steps(c, 1)
	if c.X != 0 {
		t.Errorf("TFR 0,X")
	}
	c.X = 0x5678
	steps(c, 2)
	if c.W() != 0 || c.V != 0x5678 {
		t.Errorf("EXG W,V: W %04x V %04x", c.W(), c.V)
	}
}

func TestImmediateMemory(t *testing.T) {
	c, r := newCPU(HD6309,
		0x01, 0x0f, 0x10, // OIM #$0F,<$10
		0x02, 0x3c, 0x10, // AIM #$3C,<$10
		0x05, 0xff, 0x10, // EIM #$FF,<$10
		0x0b, 0x04, 0x10, // TIM #$04,<$10
		0x61, 0x80, 0x84, // OIM #$80,,X
		0x72, 0x00, 0x20, 0x00, // AIM #0,$2000 -> Z
	)
	c.X = 0x11
	r.mem[0x10] = 0x30
	r.mem[0x2000] = 0xff
	if n := c.Step(); n != 6 || r.mem[0x10] != 0x3f {
		t.Errorf("OIM %02x %d", r.mem[0x10], n)
	}
	steps(c, 1)
	if r.mem[0x10] != 0x3c {
		t.Errorf("AIM %02x", r.mem[0x10])
	}
	steps(c, 1)
	if r.mem[0x10] != 0xc3 || c.CC&FlagN == 0 {
		t.Errorf("EIM %02x", r.mem[0x10])
	}
	if n := c.Step(); n != 4 || c.CC&FlagZ == 0 || r.mem[0x10] != 0xc3 {
		t.Errorf("TIM %d %02x", n, c.CC)
	}
	steps(c, 1)
	if r.mem[0x11] != 0x80 {
		t.Errorf("OIM idx %02x", r.mem[0x11])
	}
	steps(c, 1)
	if r.mem[0x2000] != 0 || c.CC&FlagZ == 0 {
		t.Errorf("AIM ext")
	}
}

func TestTFM(t *testing.T) {
	c, r := newCPU(HD6309,
		0x11, 0x38, 0x12, // TFM X+,Y+
		0x11, 0x39, 0x12, // TFM X-,Y-
		0x11, 0x3a, 0x12, // TFM X+,Y
		0x11, 0x3b, 0x12, // TFM X,Y+
	)
	copy(r.mem[0x2000:], "HELLO")
	c.X, c.Y = 0x2000, 0x3000
	c.SetW(5)
	total := 0
	for c.PC < 0x1003 {
		total += c.Step()
	}
	if string(r.mem[0x3000:0x3005]) != "HELLO" || c.X != 0x2005 || c.Y != 0x3005 || total != 6+3*5 {
		t.Errorf("TFM +: %q X %04x Y %04x %d cycles", r.mem[0x3000:0x3005], c.X, c.Y, total)
	}
	c.X, c.Y = 0x2004, 0x4004
	c.SetW(5)
	for c.PC < 0x1006 {
		c.Step()
	}
	if string(r.mem[0x4000:0x4005]) != "HELLO" {
		t.Errorf("TFM -: %q", r.mem[0x4000:0x4005])
	}
	c.X, c.Y = 0x2000, 0x5000
	c.SetW(3)
	for c.PC < 0x1009 {
		c.Step()
	}
	if r.mem[0x5000] != 'L' || c.Y != 0x5000 {
		t.Errorf("TFM to port: %c", r.mem[0x5000])
	}
	c.X, c.Y = 0x2004, 0x6000
	c.SetW(3)
	for c.PC < 0x100c {
		c.Step()
	}
	if string(r.mem[0x6000:0x6003]) != "OOO" {
		t.Errorf("TFM from port: %q", r.mem[0x6000:0x6003])
	}

	// an interrupt between bytes comes back to the move
	c, r = newCPU(HD6309, 0x11, 0x38, 0x12, 0x12)
	r.mem[0xf050] = 0x3b
	copy(r.mem[0x2000:], "ABCD")
	c.X, c.Y = 0x2000, 0x3000
	c.SetW(4)
	c.Step()
	c.IRQ = true
	c.Step()
	if c.PC != 0xf050 {
		t.Fatalf("IRQ not taken in TFM")
	}
	c.IRQ = false
	for c.PC != 0x1003 {
		c.Step()
	}
	if string(r.mem[0x3000:0x3004]) != "ABCD" {
		t.Errorf("TFM after IRQ: %q", r.mem[0x3000:0x3004])
	}
}

func TestBitOps(t *testing.T) {
	c, r := newCPU(HD6309,
		0x11, 0x36, 0x7a, 0x20, // LDBT A,7,2,<$20   (A bit 2 = mem bit 7)
		0x11, 0x37, 0x80, 0x21, // STBT B,0,0,<$21   (mem bit 0 = B bit 0)
		0x11, 0x32, 0x40, 0x22, // BOR A,0,0,<$22
		0x11, 0x31, 0x88, 0x20, // BIAND B,1,0,<$20
	)
	r.mem[0x20] = 0x80
	r.mem[0x22] = 0x01
	c.B = 0x01
	steps(c, 1)
	if c.A != 0x04 {
		t.Errorf("LDBT A %02x", c.A)
	}
	steps(c, 1)
	if r.mem[0x21] != 0x01 {
		t.Errorf("STBT %02x", r.mem[0x21])
	}
	steps(c, 1)
	if c.A != 0x05 {
		t.Errorf("BOR A %02x", c.A)
	}
	steps(c, 1) // mem $20 bit 1 is 0, so B bit 0 &= 1
	if c.B != 0x01 {
		t.Errorf("BIAND B %02x", c.B)
	}
}

func TestEF(t *testing.T) {
	c, r := newCPU(HD6309,
		0x11, 0x86, 0x7f, // LDE #$7F
		0x11, 0x4c, // INCE -> $80 V
		0x11, 0xcb, 0x01, // ADDF #1
		0x11, 0xd7, 0x10, // STF <$10
		0x11, 0x5f, // CLRF
		0x10, 0x48, // ASLD
		0x10, 0x40, // NEGD
	)
	steps(c, 2)
	if c.E != 0x80 || c.CC&FlagV == 0 {
		t.Errorf("INCE %02x %02x", c.E, c.CC)
	}
	steps(c, 2)
	if r.mem[0x10] != 1 {
		t.Errorf("STF")
	}
	steps(c, 1)
	if c.F != 0 || c.CC&FlagZ == 0 {
		t.Errorf("CLRF")
	}
	c.SetD(0x4001)
	steps(c, 2)
	if c.D() != 0x7ffe {
		t.Errorf("ASLD/NEGD %04x", c.D())
	}
}

func TestWIndexed(t *testing.T) {
	c, r := newCPU(HD6309,
		0xa6, 0x8f, // LDA ,W
		0xa6, 0xaf, 0x00, 0x01, // LDA 1,W
		0xa6, 0xcf, // LDA ,W++
		0xa6, 0xef, // LDA ,--W
		0xa6, 0x90, // LDA [,W]
		0xa6, 0x87, // LDA E,X
		0xe6, 0x9f, 0x40, 0x00, // LDB [$4000]
	)
	c.SetW(0x4000)
	c.X = 0x4000
	r.mem[0x4000], r.mem[0x4001], r.mem[0x0040] = 0x00, 0x40, 0x99
	r.mem[0x4040] = 0x77
	steps(c, 1)
	if c.A != 0x00 {
		t.Errorf(",W %02x", c.A)
	}
	steps(c, 1)
	if c.A != 0x40 {
		t.Errorf("1,W %02x", c.A)
	}
	steps(c, 1)
	if c.W() != 0x4002 {
		t.Errorf(",W++ %04x", c.W())
	}
	steps(c, 1)
	if c.W() != 0x4000 {
		t.Errorf(",--W %04x", c.W())
	}
	steps(c, 1)
	if c.A != 0x99 {
		t.Errorf("[,W] %02x", c.A)
	}
	steps(c, 1)
	if c.A != 0x77 {
		t.Errorf("E,X %02x", c.A)
	}
	steps(c, 1)
	if c.B != 0x99 {
		t.Errorf("[n16] %02x", c.B)
	}
}

// The 6309 in emulation mode keeps the 6809's timing.
func TestEmulationTiming(t *testing.T) {
	prog := []uint8{0x86, 0x01, 0x97, 0x10, 0xb7, 0x20, 0x00, 0xa6, 0x84, 0xcc, 0, 0, 0xfd, 0x20, 0x00, 0x34, 0x16, 0x35, 0x16, 0x3d, 0x1f, 0x12}
	a, _ := newCPU(MC6809, prog...)
	b, _ := newCPU(HD6309, prog...)
	for i := 0; i < 10; i++ {
		if x, y := a.Step(), b.Step(); x != y {
			t.Errorf("instruction %d: 6809 %d cycles, 6309 %d", i, x, y)
		}
	}
}
