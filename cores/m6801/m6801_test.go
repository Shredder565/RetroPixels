package m6801

import (
	"bytes"
	"testing"
)

// mem is 64 KB outside the chip plus port pins.
type mem struct {
	m      [65536]uint8
	pins   [8]uint8
	writes [][3]int // port, value, ddr
}

func (r *mem) Read(a uint16) uint8     { return r.m[a] }
func (r *mem) Write(a uint16, v uint8) { r.m[a] = v }
func (r *mem) PortRead(p int) uint8    { return r.pins[p] }
func (r *mem) PortWrite(p int, v, d uint8) {
	r.writes = append(r.writes, [3]int{p, int(v), int(d)})
}

// newCPU makes a processor with code at $1000 and S at $01FF; vector n
// points to $F0n0.
func newCPU(model Model, code ...uint8) (*CPU, *mem) {
	r := &mem{}
	for i := 0; i < 11; i++ {
		v := 0xffea + 2*i
		r.m[v] = 0xf0
		r.m[v+1] = uint8(i << 4)
	}
	r.m[0xfffe], r.m[0xffff] = 0x10, 0x00
	copy(r.m[0x1000:], code)
	c := New(r, model)
	c.S = 0x01ff
	c.CC = 0xc0
	return c, r
}

func TestALU(t *testing.T) {
	cases := []struct {
		name  string
		a, cc uint8
		code  []uint8
		want  uint8
		flags uint8
		c01   int // 6801 cycles
		c63   int // 6301 cycles
	}{
		{"LDAA #", 0, 0, []uint8{0x86, 0x80}, 0x80, FlagN, 2, 2},
		{"ADDA H V", 0x7f, 0, []uint8{0x8b, 0x01}, 0x80, FlagH | FlagN | FlagV, 2, 2},
		{"ADDA C", 0xff, 0, []uint8{0x8b, 0x01}, 0x00, FlagH | FlagZ | FlagC, 2, 2},
		{"SUBA", 0x00, 0, []uint8{0x80, 0x01}, 0xff, FlagN | FlagC, 2, 2},
		{"SBCA", 0x10, FlagC, []uint8{0x82, 0x0f}, 0x00, FlagZ, 2, 2},
		{"CMPA", 0x42, 0, []uint8{0x81, 0x42}, 0x42, FlagZ, 2, 2},
		{"ANDA", 0xf0, FlagV, []uint8{0x84, 0x3c}, 0x30, 0, 2, 2},
		{"NEGA", 0x01, 0, []uint8{0x40}, 0xff, FlagN | FlagC, 2, 1},
		{"COMA", 0x0f, 0, []uint8{0x43}, 0xf0, FlagN | FlagC, 2, 1},
		{"LSRA", 0x01, 0, []uint8{0x44}, 0x00, FlagZ | FlagC | FlagV, 2, 1},
		{"ASLA", 0x80, 0, []uint8{0x48}, 0x00, FlagZ | FlagC | FlagV, 2, 1},
		{"RORA", 0x00, FlagC, []uint8{0x46}, 0x80, FlagN | FlagV, 2, 1},
		{"DECA", 0x80, 0, []uint8{0x4a}, 0x7f, FlagV, 2, 1},
		{"INCA", 0x7f, 0, []uint8{0x4c}, 0x80, FlagN | FlagV, 2, 1},
		{"TSTA", 0x00, FlagC | FlagV, []uint8{0x4d}, 0x00, FlagZ, 2, 1},
		{"CLRA", 0x12, FlagN | FlagC, []uint8{0x4f}, 0x00, FlagZ, 2, 1},
		{"DAA", 0x11, FlagH, []uint8{0x19}, 0x17, FlagH, 2, 2},
		{"ADDA dir", 0x01, 0, []uint8{0x9b, 0x90}, 0x34, 0, 3, 3},
		{"ADDA idx", 0x01, 0, []uint8{0xab, 0x10}, 0x34, 0, 4, 4},
		{"ADDA ext", 0x01, 0, []uint8{0xbb, 0x20, 0x00}, 0x34, 0, 4, 4},
	}
	for _, model := range []Model{MC6801, HD6301V} {
		for _, tc := range cases {
			c, r := newCPU(model, tc.code...)
			c.ram[0x10] = 0x33 // $90
			c.X = 0x1ff0
			r.m[0x2000] = 0x33
			c.A, c.CC = tc.a, tc.cc|0xc0
			n := c.Step()
			want := tc.c01
			if model != MC6801 {
				want = tc.c63
			}
			if c.A != tc.want || c.CC&0x2f != tc.flags {
				t.Errorf("%v %s: A %02x CC %02x, want %02x %02x", model, tc.name, c.A, c.CC, tc.want, tc.flags)
			}
			if n != want {
				t.Errorf("%v %s: %d cycles, want %d", model, tc.name, n, want)
			}
		}
	}
}

func TestSixteenBit(t *testing.T) {
	c, r := newCPU(HD6301V,
		0xcc, 0x12, 0x34, // LDD #$1234
		0xc3, 0xee, 0xee, // ADDD #$EEEE -> $0122 C
		0x83, 0x01, 0x23, // SUBD #$0123 -> $FFFF
		0xfd, 0x30, 0x00, // STD $3000
		0xce, 0x80, 0x00, // LDX #$8000
		0x8c, 0x80, 0x01, // CPX #$8001
		0x18,       // XGDX
		0x3d,       // MUL
		0x04,       // LSRD
		0x05,       // ASLD
		0x3a,       // ABX
		0xdf, 0x90, // STX <$90 (internal RAM)
	)
	steps := func(n int) {
		for i := 0; i < n; i++ {
			c.Step()
		}
	}
	steps(2)
	if c.D() != 0x0122 || c.CC&FlagC == 0 {
		t.Errorf("ADDD %04x", c.D())
	}
	steps(2)
	if c.D() != 0xffff || r.m[0x3000] != 0xff {
		t.Errorf("SUBD/STD %04x", c.D())
	}
	steps(2)
	if c.CC&(FlagN|FlagC) != FlagN|FlagC {
		t.Errorf("CPX %02x", c.CC)
	}
	steps(1)
	if c.X != 0xffff || c.D() != 0x8000 {
		t.Errorf("XGDX X %04x D %04x", c.X, c.D())
	}
	steps(1)
	if c.D() != 0 {
		t.Errorf("MUL %04x", c.D())
	}
	c.SetD(0x8001)
	steps(1)
	if c.D() != 0x4000 || c.CC&FlagC == 0 {
		t.Errorf("LSRD %04x", c.D())
	}
	steps(1)
	if c.D() != 0x8000 || c.CC&FlagN == 0 {
		t.Errorf("ASLD %04x", c.D())
	}
	steps(2)
	if c.X != 0xffff || c.ram[0x10] != 0xff || c.ram[0x11] != 0xff {
		t.Errorf("ABX/STX %04x % x", c.X, c.ram[0x10:0x12])
	}
}

func TestStackAndSubroutines(t *testing.T) {
	c, r := newCPU(HD6301V,
		0x36,       // PSHA
		0x3c,       // PSHX
		0x8d, 0x02, // BSR +2
		0x01, 0x01,
		0x33, // $1006: PULB (BSR's return high byte)
		0x32, // PULA
		0x38, // PULX (the X pushed)
		0x30, // TSX
	)
	r.m[0x0100] = 0
	c.A, c.X = 0x5a, 0x1234
	for i := 0; i < 3; i++ {
		c.Step()
	}
	if c.PC != 0x1006 || c.S != 0x01fa {
		t.Fatalf("BSR PC %04x S %04x", c.PC, c.S)
	}
	for i := 0; i < 3; i++ {
		c.Step()
	}
	if c.B != 0x10 || c.A != 0x04 || c.X != 0x1234 {
		t.Errorf("pulls: B %02x A %02x X %04x", c.B, c.A, c.X)
	}
	c.Step()
	if c.X != 0x01ff {
		t.Errorf("TSX %04x", c.X)
	}
}

func TestInterrupts(t *testing.T) {
	c, r := newCPU(HD63701Y, 0x0e, 0x01, 0x3e)
	irqVec := uint16(r.m[0xfff8])<<8 | uint16(r.m[0xfff9])
	nmiVec := uint16(r.m[0xfffc])<<8 | uint16(r.m[0xfffd])
	c.CC |= FlagI
	c.IRQ = true
	c.Step() // CLI
	if n := c.Step(); n != 12 || c.PC != irqVec || c.S != 0x01ff-7 || c.CC&FlagI == 0 {
		t.Fatalf("IRQ: %d PC %04x S %04x", n, c.PC, c.S)
	}
	c.IRQ = false
	c.PC = 0x1001
	c.CC &^= FlagI
	c.S = 0x01ff
	c.Step() // NOP
	c.Step() // WAI
	if !c.Waiting() || c.S != 0x01ff-7 {
		t.Fatal("WAI")
	}
	if n := c.Step(); n != 1 {
		t.Fatal("WAI ran on")
	}
	c.NMI()
	c.Step()
	if c.PC != nmiVec || c.S != 0x01ff-7 || c.Waiting() {
		t.Errorf("NMI after WAI: PC %04x S %04x", c.PC, c.S)
	}
	if r.m[0x1ff] != 0x03 || r.m[0x1fe] != 0x10 {
		t.Errorf("stacked PC % x", r.m[0x1fe:0x200])
	}
}

func TestPorts(t *testing.T) {
	c, r := newCPU(HD63701Y,
		0x86, 0xff, 0x97, 0x16, // LDAA #$FF; STAA $16 (P6DDR)
		0x86, 0x05, 0x97, 0x17, // STAA $17 (P6)
		0x86, 0x0f, 0x97, 0x00, // P1DDR = $0F
		0x96, 0x02, // LDAA $02 (P1)
	)
	r.pins[1] = 0xa0
	for i := 0; i < 6; i++ {
		c.Step()
	}
	if last := r.writes[1]; last != [3]int{6, 5, 0xff} {
		t.Errorf("port 6 write %v", r.writes)
	}
	c.wr(0x02, 0x03)
	c.Step()
	c.Step()
	if c.A != 0xa3 {
		t.Errorf("port 1 read %02x", c.A)
	}
}

func TestTimer(t *testing.T) {
	// output compare at $0010 with its interrupt enabled
	c, r := newCPU(HD6301V, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01)
	ocVec := uint16(r.m[0xfff4])<<8 | uint16(r.m[0xfff5])
	c.wr(0x0b, 0x00)
	c.wr(0x0c, 0x10)
	c.wr(0x08, timerEOCI)
	c.frc = 0
	c.CC &^= FlagI
	for i := 0; i < 15; i++ {
		c.Step()
	}
	if c.PC == ocVec {
		t.Fatal("compare too early")
	}
	c.Step()
	c.Step()
	if c.PC != ocVec {
		t.Fatalf("no compare interrupt (frc %04x PC %04x)", c.frc, c.PC)
	}
	// reading TCSR then writing OCR clears the flag
	if c.rd(0x08)&timerOCF == 0 {
		t.Fatal("OCF not set")
	}
	c.wr(0x0c, 0x80)
	if c.rd(0x08)&timerOCF != 0 {
		t.Error("OCF not cleared")
	}
}

func TestHitachiOps(t *testing.T) {
	c, r := newCPU(HD6301V,
		0x71, 0x0f, 0x90, // AIM #$0F,<$90
		0x72, 0x80, 0x90, // OIM #$80,<$90
		0x75, 0xff, 0x90, // EIM #$FF,<$90
		0x7b, 0x01, 0x90, // TIM #$01,<$90
		0x61, 0xf0, 0x00, // AIM #$F0,0,X
		0x00, // illegal: trap
	)
	c.ram[0x10] = 0x3c
	c.X = 0x2000
	r.m[0x2000] = 0xff
	if n := c.Step(); n != 6 || c.ram[0x10] != 0x0c {
		t.Errorf("AIM %02x %d", c.ram[0x10], n)
	}
	c.Step()
	if c.ram[0x10] != 0x8c {
		t.Errorf("OIM %02x", c.ram[0x10])
	}
	c.Step()
	if c.ram[0x10] != 0x73 {
		t.Errorf("EIM %02x", c.ram[0x10])
	}
	if n := c.Step(); n != 4 || c.CC&FlagZ != 0 {
		t.Errorf("TIM %d %02x", n, c.CC)
	}
	if n := c.Step(); n != 7 || r.m[0x2000] != 0xf0 {
		t.Errorf("AIM idx %02x %d", r.m[0x2000], n)
	}
	c.Step()
	trap := uint16(r.m[0xffee])<<8 | uint16(r.m[0xffef])
	if c.PC != trap {
		t.Errorf("trap: PC %04x want %04x", c.PC, trap)
	}
	// the 6801 doesn't know them
	c, _ = newCPU(MC6801, 0x18)
	c.X = 0x1234
	c.Step()
	if c.D() == 0x1234 {
		t.Error("6801 ran XGDX")
	}
}

func TestBranches(t *testing.T) {
	cases := []struct{ op, yes, no uint8 }{
		{0x22, 0, FlagC}, {0x23, FlagZ, 0}, {0x24, 0, FlagC}, {0x25, FlagC, 0}, {0x26, 0, FlagZ}, {0x27, FlagZ, 0},
		{0x28, 0, FlagV}, {0x29, FlagV, 0}, {0x2a, 0, FlagN}, {0x2b, FlagN, 0}, {0x2c, FlagN | FlagV, FlagN},
		{0x2d, FlagN, 0}, {0x2e, 0, FlagZ}, {0x2f, FlagZ, 0},
	}
	for _, k := range cases {
		c, _ := newCPU(HD6301V, k.op, 0x10)
		c.CC = 0xc0 | k.yes
		if n := c.Step(); c.PC != 0x1012 || n != 3 {
			t.Errorf("%02x not taken", k.op)
		}
		c, _ = newCPU(HD6301V, k.op, 0x10)
		c.CC = 0xc0 | k.no
		if c.Step(); c.PC != 0x1002 {
			t.Errorf("%02x taken", k.op)
		}
	}
}

func TestState(t *testing.T) {
	c, _ := newCPU(HD63701Y, 0x01)
	c.A, c.B, c.X, c.S = 1, 2, 3, 4
	c.ram[5] = 9
	c.wr(0x17, 0x33)
	b := c.AppendState(nil)
	if len(b) != c.StateSize() {
		t.Fatalf("size %d", len(b))
	}
	d, _ := newCPU(HD63701Y)
	if _, err := d.LoadState(b); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(d.AppendState(nil), b) {
		t.Error("state did not round-trip")
	}
}
