package sh2

import (
	"encoding/binary"
	"math/rand"
	"strings"
	"testing"
)

// ------------------------------------------------------------ test harness

// The test machine: 64 KB of RAM at 0 mapped straight into the CPU, and a
// bus that keeps everything else in a map (so tests can see bus accesses).
// Programs start at 0x1000 with the stack at 0xf000.

const (
	progAt  = 0x1000
	stackAt = 0xf000
)

type testBus struct {
	mem    map[uint32]uint8
	reads  int
	writes int
	acks   []int
	vector int // for external-vector mode, 0 = not implementing it
}

func (b *testBus) Read8(a uint32) uint8 { b.reads++; return b.mem[a] }
func (b *testBus) Read16(a uint32) uint16 {
	b.reads++
	return uint16(b.mem[a])<<8 | uint16(b.mem[a+1])
}
func (b *testBus) Read32(a uint32) uint32 {
	b.reads++
	return uint32(b.mem[a])<<24 | uint32(b.mem[a+1])<<16 | uint32(b.mem[a+2])<<8 | uint32(b.mem[a+3])
}
func (b *testBus) Write8(a uint32, v uint8) { b.writes++; b.mem[a] = v }
func (b *testBus) Write16(a uint32, v uint16) {
	b.writes++
	b.mem[a], b.mem[a+1] = uint8(v>>8), uint8(v)
}
func (b *testBus) Write32(a uint32, v uint32) {
	b.writes++
	b.mem[a], b.mem[a+1], b.mem[a+2], b.mem[a+3] = uint8(v>>24), uint8(v>>16), uint8(v>>8), uint8(v)
}
func (b *testBus) IRQAck(level int) { b.acks = append(b.acks, level) }

// vecBus also supplies external vectors.
type vecBus struct{ *testBus }

func (b vecBus) IRQVector(level int) int { return b.vector }

type machine struct {
	*CPU
	ram []byte
	bus *testBus
}

// newMachine loads prog at progAt and resets the CPU.
func newMachine(t *testing.T, prog ...uint16) *machine {
	t.Helper()
	bus := &testBus{mem: map[uint32]uint8{}}
	m := &machine{ram: make([]byte, 0x10000), bus: bus}
	m.CPU = New(bus)
	m.Map(0, m.ram, true)
	binary.BigEndian.PutUint32(m.ram[0:], progAt)
	binary.BigEndian.PutUint32(m.ram[4:], stackAt)
	m.load(progAt, prog...)
	m.Reset()
	return m
}

func (m *machine) load(at uint32, prog ...uint16) {
	for i, op := range prog {
		binary.BigEndian.PutUint16(m.ram[at+uint32(i)*2:], op)
	}
}

func (m *machine) setVector(n int, addr uint32) {
	binary.BigEndian.PutUint32(m.ram[m.VBR+uint32(n)*4:], addr)
}

// steps runs n instructions (a delayed branch and its slot count as one).
func (m *machine) steps(n int) {
	for i := 0; i < n; i++ {
		m.Step()
	}
}

// runTo steps until PC is at, failing after a while.
func (m *machine) runTo(t *testing.T, at uint32) {
	t.Helper()
	for i := 0; i < 100000; i++ {
		if m.PC == at {
			return
		}
		m.Step()
	}
	t.Fatalf("never reached %08x (PC %08x)", at, m.PC)
}

func (m *machine) mem32(a uint32) uint32 { return binary.BigEndian.Uint32(m.ram[a:]) }
func (m *machine) mem16(a uint32) uint16 { return binary.BigEndian.Uint16(m.ram[a:]) }

// A assembles one instruction from its bit pattern (see patterns): the
// fields are given in the order n, m, then the immediate or displacement,
// leaving out the ones the pattern does not have.
func A(mn string, f ...int) uint16 {
	pat, ok := patterns[mn]
	if !ok {
		panic("no pattern " + mn)
	}
	var op uint16
	vals := map[byte]int{}
	order := []byte{'n', 'm', 'i'}
	k := 0
	for _, c := range order {
		if strings.IndexByte(pat, c) >= 0 {
			vals[c] = f[k]
			k++
		}
	}
	if k != len(f) {
		panic("wrong number of fields for " + mn)
	}
	// constant bits, then each field from its low bit up
	bit := map[byte]int{}
	for i := 0; i < 16; i++ {
		if pat[15-i] == '1' {
			op |= 1 << i
		}
	}
	for i := 0; i < 16; i++ {
		c := pat[15-i]
		if c == 'n' || c == 'm' || c == 'i' {
			if uint(vals[c])>>bit[c]&1 != 0 {
				op |= 1 << i
			}
			bit[c]++
		}
	}
	return op
}

var patterns = map[string]string{
	"MOVI": "1110nnnniiiiiiii", "MOVWPC": "1001nnnniiiiiiii", "MOVLPC": "1101nnnniiiiiiii",
	"MOV":   "0110nnnnmmmm0011",
	"MOVBS": "0010nnnnmmmm0000", "MOVWS": "0010nnnnmmmm0001", "MOVLS": "0010nnnnmmmm0010",
	"MOVBL": "0110nnnnmmmm0000", "MOVWL": "0110nnnnmmmm0001", "MOVLL": "0110nnnnmmmm0010",
	"MOVBM": "0010nnnnmmmm0100", "MOVWM": "0010nnnnmmmm0101", "MOVLM": "0010nnnnmmmm0110",
	"MOVBP": "0110nnnnmmmm0100", "MOVWP": "0110nnnnmmmm0101", "MOVLP": "0110nnnnmmmm0110",
	"MOVBS4": "10000000nnnniiii", "MOVWS4": "10000001nnnniiii", "MOVLS4": "0001nnnnmmmmiiii",
	"MOVBL4": "10000100mmmmiiii", "MOVWL4": "10000101mmmmiiii", "MOVLL4": "0101nnnnmmmmiiii",
	"MOVBS0": "0000nnnnmmmm0100", "MOVWS0": "0000nnnnmmmm0101", "MOVLS0": "0000nnnnmmmm0110",
	"MOVBL0": "0000nnnnmmmm1100", "MOVWL0": "0000nnnnmmmm1101", "MOVLL0": "0000nnnnmmmm1110",
	"MOVBSG": "11000000iiiiiiii", "MOVWSG": "11000001iiiiiiii", "MOVLSG": "11000010iiiiiiii",
	"MOVBLG": "11000100iiiiiiii", "MOVWLG": "11000101iiiiiiii", "MOVLLG": "11000110iiiiiiii",
	"MOVA": "11000111iiiiiiii", "MOVT": "0000nnnn00101001",
	"SWAPB": "0110nnnnmmmm1000", "SWAPW": "0110nnnnmmmm1001", "XTRCT": "0010nnnnmmmm1101",
	"ADD": "0011nnnnmmmm1100", "ADDI": "0111nnnniiiiiiii", "ADDC": "0011nnnnmmmm1110", "ADDV": "0011nnnnmmmm1111",
	"CMPIM": "10001000iiiiiiii", "CMPEQ": "0011nnnnmmmm0000", "CMPHS": "0011nnnnmmmm0010",
	"CMPGE": "0011nnnnmmmm0011", "CMPHI": "0011nnnnmmmm0110", "CMPGT": "0011nnnnmmmm0111",
	"CMPPZ": "0100nnnn00010001", "CMPPL": "0100nnnn00010101", "CMPSTR": "0010nnnnmmmm1100",
	"DIV1": "0011nnnnmmmm0100", "DIV0S": "0010nnnnmmmm0111", "DIV0U": "0000000000011001",
	"DMULS": "0011nnnnmmmm1101", "DMULU": "0011nnnnmmmm0101", "DT": "0100nnnn00010000",
	"EXTSB": "0110nnnnmmmm1110", "EXTSW": "0110nnnnmmmm1111", "EXTUB": "0110nnnnmmmm1100", "EXTUW": "0110nnnnmmmm1101",
	"MACL": "0000nnnnmmmm1111", "MACW": "0100nnnnmmmm1111",
	"MULL": "0000nnnnmmmm0111", "MULS": "0010nnnnmmmm1111", "MULU": "0010nnnnmmmm1110",
	"NEG": "0110nnnnmmmm1011", "NEGC": "0110nnnnmmmm1010",
	"SUB": "0011nnnnmmmm1000", "SUBC": "0011nnnnmmmm1010", "SUBV": "0011nnnnmmmm1011",
	"AND": "0010nnnnmmmm1001", "ANDI": "11001001iiiiiiii", "ANDM": "11001101iiiiiiii",
	"NOT": "0110nnnnmmmm0111",
	"OR":  "0010nnnnmmmm1011", "ORI": "11001011iiiiiiii", "ORM": "11001111iiiiiiii",
	"TAS": "0100nnnn00011011",
	"TST": "0010nnnnmmmm1000", "TSTI": "11001000iiiiiiii", "TSTM": "11001100iiiiiiii",
	"XOR": "0010nnnnmmmm1010", "XORI": "11001010iiiiiiii", "XORM": "11001110iiiiiiii",
	"ROTL": "0100nnnn00000100", "ROTR": "0100nnnn00000101", "ROTCL": "0100nnnn00100100", "ROTCR": "0100nnnn00100101",
	"SHAL": "0100nnnn00100000", "SHAR": "0100nnnn00100001", "SHLL": "0100nnnn00000000", "SHLR": "0100nnnn00000001",
	"SHLL2": "0100nnnn00001000", "SHLR2": "0100nnnn00001001", "SHLL8": "0100nnnn00011000",
	"SHLR8": "0100nnnn00011001", "SHLL16": "0100nnnn00101000", "SHLR16": "0100nnnn00101001",
	"BF": "10001011iiiiiiii", "BFS": "10001111iiiiiiii", "BT": "10001001iiiiiiii", "BTS": "10001101iiiiiiii",
	"BRA": "1010iiiiiiiiiiii", "BRAF": "0000nnnn00100011", "BSR": "1011iiiiiiiiiiii", "BSRF": "0000nnnn00000011",
	"JMP": "0100nnnn00101011", "JSR": "0100nnnn00001011", "RTS": "0000000000001011",
	"CLRMAC": "0000000000101000", "CLRT": "0000000000001000", "SETT": "0000000000011000",
	"LDCSR": "0100nnnn00001110", "LDCGBR": "0100nnnn00011110", "LDCVBR": "0100nnnn00101110",
	"LDCMSR": "0100nnnn00000111", "LDCMGBR": "0100nnnn00010111", "LDCMVBR": "0100nnnn00100111",
	"LDSMACH": "0100nnnn00001010", "LDSMACL": "0100nnnn00011010", "LDSPR": "0100nnnn00101010",
	"LDSMMACH": "0100nnnn00000110", "LDSMMACL": "0100nnnn00010110", "LDSMPR": "0100nnnn00100110",
	"NOP": "0000000000001001", "RTE": "0000000000101011", "SLEEP": "0000000000011011",
	"STCSR": "0000nnnn00000010", "STCGBR": "0000nnnn00010010", "STCVBR": "0000nnnn00100010",
	"STCMSR": "0100nnnn00000011", "STCMGBR": "0100nnnn00010011", "STCMVBR": "0100nnnn00100011",
	"STSMACH": "0000nnnn00001010", "STSMACL": "0000nnnn00011010", "STSPR": "0000nnnn00101010",
	"STSMMACH": "0100nnnn00000010", "STSMMACL": "0100nnnn00010010", "STSMPR": "0100nnnn00100010",
	"TRAPA": "11000011iiiiiiii",
}

var nop = A("NOP")

func TestAssembler(t *testing.T) {
	// a few encodings checked by hand against the instruction set
	for _, c := range []struct {
		got, want uint16
	}{
		{A("MOV", 3, 1), 0x6313},
		{A("MOVI", 2, -1), 0xe2ff},
		{A("ADDI", 15, -4), 0x7ffc},
		{A("BRA", -2), 0xaffe},
		{A("MOVLS4", 1, 2, 3), 0x1123},
		{A("MOVBL4", 5, 2), 0x8452},
		{A("NOP"), 0x0009},
		{A("RTS"), 0x000b},
		{A("JSR", 4), 0x440b},
	} {
		if c.got != c.want {
			t.Errorf("assembled %04x, want %04x", c.got, c.want)
		}
	}
}

// ------------------------------------------------------------ data moves

func TestResetVectors(t *testing.T) {
	m := newMachine(t, nop)
	if m.PC != progAt || m.R[15] != stackAt {
		t.Fatalf("PC %08x SP %08x", m.PC, m.R[15])
	}
	if m.SR&0xf0 != 0xf0 {
		t.Errorf("SR %03x: interrupts should be masked after reset", m.SR)
	}
	if m.VBR != 0 {
		t.Errorf("VBR %08x", m.VBR)
	}
}

func TestMoveImmediate(t *testing.T) {
	m := newMachine(t, A("MOVI", 1, 0x7f), A("MOVI", 2, -128), A("MOV", 3, 2))
	m.steps(3)
	if m.R[1] != 0x7f || m.R[2] != 0xffffff80 || m.R[3] != 0xffffff80 {
		t.Fatalf("R1 %x R2 %x R3 %x", m.R[1], m.R[2], m.R[3])
	}
}

func TestMovePCRelative(t *testing.T) {
	// 1000 mov.w @(disp,pc) with disp 2: address 1000+4+4 = 1008
	// 1002 mov.l @(disp,pc) with disp 1: (1002+4)&~3 + 4 = 1008
	// 1004 mova @(1,pc),r0 = 1004+4 + 4 = 100c
	m := newMachine(t, A("MOVWPC", 1, 2), A("MOVLPC", 2, 1), A("MOVA", 1), nop, 0x8123, 0x4567)
	m.steps(3)
	if m.R[1] != 0xffff8123 {
		t.Errorf("mov.w @(disp,pc): %08x", m.R[1])
	}
	if m.R[2] != 0x81234567 {
		t.Errorf("mov.l @(disp,pc): %08x", m.R[2])
	}
	if m.R[0] != 0x100c {
		t.Errorf("mova: %08x", m.R[0])
	}
}

func TestLoadsSignExtend(t *testing.T) {
	m := newMachine(t,
		A("MOVBL", 1, 8), A("MOVWL", 2, 8), A("MOVLL", 3, 8),
		A("MOVBP", 4, 9), A("MOVBP", 5, 9), A("MOVWP", 6, 9), A("MOVLP", 7, 9))
	copy(m.ram[0x2000:], []byte{0x80, 0x01, 0x7f, 0xfe, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc})
	m.R[8], m.R[9] = 0x2000, 0x2000
	m.steps(7)
	want := []uint32{1: 0xffffff80, 2: 0xffff8001, 3: 0x80017ffe, 4: 0xffffff80, 5: 1, 6: 0x7ffe, 7: 0x12345678}
	for i := 1; i <= 7; i++ {
		if m.R[i] != want[i] {
			t.Errorf("R%d = %08x, want %08x", i, m.R[i], want[i])
		}
	}
	if m.R[9] != 0x2008 {
		t.Errorf("post-increment left R9 at %08x", m.R[9])
	}
}

func TestPostIncrementSameRegister(t *testing.T) {
	// mov.l @r1+,r1: the loaded value wins over the increment
	m := newMachine(t, A("MOVLP", 1, 1))
	binary.BigEndian.PutUint32(m.ram[0x2000:], 0xcafef00d)
	m.R[1] = 0x2000
	m.steps(1)
	if m.R[1] != 0xcafef00d {
		t.Fatalf("R1 %08x", m.R[1])
	}
}

func TestStores(t *testing.T) {
	m := newMachine(t,
		A("MOVBS", 8, 1), A("MOVWS", 9, 1), A("MOVLS", 10, 1),
		A("MOVBM", 11, 1), A("MOVWM", 11, 1), A("MOVLM", 11, 1))
	m.R[1] = 0x11223344
	m.R[8], m.R[9], m.R[10], m.R[11] = 0x2000, 0x2010, 0x2020, 0x2038
	m.steps(6)
	if m.ram[0x2000] != 0x44 || m.mem16(0x2010) != 0x3344 || m.mem32(0x2020) != 0x11223344 {
		t.Errorf("plain stores: %x %x %x", m.ram[0x2000], m.mem16(0x2010), m.mem32(0x2020))
	}
	// pre-decrement: byte at 2037, word at 2035&~1=2034 (2037-2=2035 is odd, the bus aligns), long at 2030
	if m.R[11] != 0x2038-1-2-4 {
		t.Errorf("R11 %08x", m.R[11])
	}
	if m.ram[0x2037] != 0x44 || m.mem32(0x2030&^3) != 0x11223344 {
		t.Errorf("pre-decrement stores: %x %x", m.ram[0x2037], m.mem32(0x2030))
	}
}

func TestDisplacementAndIndexed(t *testing.T) {
	m := newMachine(t,
		A("MOVBS4", 8, 3),    // r0 -> @(3,r8)
		A("MOVWS4", 8, 3),    // r0 -> @(6,r8)
		A("MOVLS4", 8, 1, 3), // r1 -> @(12,r8)
		A("MOVBL4", 8, 3),    // @(3,r8) -> r0
		A("MOVLL4", 2, 8, 3), // @(12,r8) -> r2
		A("MOVBS0", 8, 1),    // r1 -> @(r0,r8)
		A("MOVLL0", 3, 8),    // @(r0,r8) -> r3
	)
	m.R[0], m.R[1], m.R[8] = 0x0000ff85, 0xdeadbeef, 0x2000
	m.steps(3)
	if m.ram[0x2003] != 0x85 || m.mem16(0x2006) != 0xff85 || m.mem32(0x200c) != 0xdeadbeef {
		t.Fatalf("displacement stores: %x %x %x", m.ram[0x2003], m.mem16(0x2006), m.mem32(0x200c))
	}
	m.steps(2)
	if m.R[0] != 0xffffff85 || m.R[2] != 0xdeadbeef {
		t.Fatalf("displacement loads: %x %x", m.R[0], m.R[2])
	}
	m.R[0] = 0x10
	m.steps(2)
	if m.ram[0x2010] != 0xef || m.R[3] != 0xef000000 {
		t.Fatalf("indexed: %x %x", m.ram[0x2010], m.R[3])
	}
}

func TestGBRRelative(t *testing.T) {
	m := newMachine(t,
		A("LDCGBR", 1),
		A("MOVLSG", 2), // r0 -> @(8,gbr)
		A("MOVWSG", 1), // r0 -> @(2,gbr)
		A("MOVBLG", 9), // @(9,gbr) -> r0
		A("STCGBR", 4),
	)
	m.R[0], m.R[1] = 0x12348765, 0x3000
	m.steps(3)
	if m.mem32(0x3008) != 0x12348765 || m.mem16(0x3002) != 0x8765 {
		t.Fatalf("stores %08x %04x", m.mem32(0x3008), m.mem16(0x3002))
	}
	m.steps(2)
	if m.R[0] != 0x34 || m.R[4] != 0x3000 {
		t.Fatalf("R0 %08x R4 %08x", m.R[0], m.R[4])
	}
}

func TestSwapExtendExtract(t *testing.T) {
	m := newMachine(t, A("SWAPB", 2, 1), A("SWAPW", 3, 1), A("XTRCT", 4, 1),
		A("EXTSB", 5, 1), A("EXTSW", 6, 1), A("EXTUB", 7, 1), A("EXTUW", 8, 1), A("MOVT", 9), A("SETT"), A("MOVT", 10))
	m.R[1] = 0x1234c6a7
	m.R[4] = 0xaabbccdd
	m.steps(10)
	want := map[int]uint32{2: 0x1234a7c6, 3: 0xc6a71234, 4: 0xc6a7aabb, 5: 0xffffffa7, 6: 0xffffc6a7, 7: 0xa7, 8: 0xc6a7, 9: 0, 10: 1}
	for r, w := range want {
		if m.R[r] != w {
			t.Errorf("R%d %08x want %08x", r, m.R[r], w)
		}
	}
}

// ------------------------------------------------------------ arithmetic

func TestAddSubCarryOverflow(t *testing.T) {
	type tc struct {
		op          string
		a, b, t     uint32 // Rn, Rm, T in
		want, wantT uint32
	}
	for _, c := range []tc{
		{"ADD", 5, 7, 0, 12, 0},
		{"ADDC", 0xffffffff, 1, 0, 0, 1},
		{"ADDC", 0xfffffffe, 1, 1, 0, 1},
		{"ADDC", 1, 2, 1, 4, 0},
		{"ADDV", 0x7fffffff, 1, 0, 0x80000000, 1},
		{"ADDV", 0x80000000, 0xffffffff, 0, 0x7fffffff, 1},
		{"ADDV", 5, 0xfffffffe, 0, 3, 0},
		{"SUB", 5, 7, 0, 0xfffffffe, 0},
		{"SUBC", 0, 1, 0, 0xffffffff, 1},
		{"SUBC", 5, 4, 1, 0, 0},
		{"SUBC", 5, 5, 1, 0xffffffff, 1},
		{"SUBV", 0x80000000, 1, 0, 0x7fffffff, 1},
		{"SUBV", 0x7fffffff, 0xffffffff, 0, 0x80000000, 1},
		{"SUBV", 3, 5, 0, 0xfffffffe, 0},
		{"NEGC", 0, 1, 0, 0xffffffff, 1}, // Rn = 0 - Rm - T
		{"NEGC", 0, 0, 0, 0, 0},
		{"NEGC", 0, 0, 1, 0xffffffff, 1},
		{"NEG", 0, 5, 0, 0xfffffffb, 0},
	} {
		m := newMachine(t, A(c.op, 1, 2))
		m.R[1], m.R[2] = c.a, c.b
		m.SR = m.SR&^1 | c.t
		m.steps(1)
		gotT := m.SR & 1
		if c.op == "ADD" || c.op == "SUB" || c.op == "NEG" {
			gotT = c.wantT // these leave T alone
		}
		if m.R[1] != c.want || gotT != c.wantT {
			t.Errorf("%s %08x,%08x T%d: got %08x T%d, want %08x T%d", c.op, c.a, c.b, c.t, m.R[1], m.SR&1, c.want, c.wantT)
		}
	}
}

func TestAddImmediate(t *testing.T) {
	m := newMachine(t, A("ADDI", 1, -1), A("ADDI", 2, 127))
	m.R[1], m.R[2] = 0, 1
	m.steps(2)
	if m.R[1] != 0xffffffff || m.R[2] != 128 {
		t.Fatalf("%08x %08x", m.R[1], m.R[2])
	}
}

func TestCompare(t *testing.T) {
	type tc struct {
		op   string
		a, b uint32
		want bool
	}
	for _, c := range []tc{
		{"CMPEQ", 5, 5, true}, {"CMPEQ", 5, 6, false},
		{"CMPHS", 0xffffffff, 1, true}, {"CMPHS", 1, 1, true}, {"CMPHS", 1, 2, false},
		{"CMPHI", 2, 1, true}, {"CMPHI", 1, 1, false}, {"CMPHI", 1, 0xffffffff, false},
		{"CMPGE", 1, 0xffffffff, true}, {"CMPGE", 0xffffffff, 1, false}, {"CMPGE", 3, 3, true},
		{"CMPGT", 1, 0xffffffff, true}, {"CMPGT", 3, 3, false},
		{"CMPSTR", 0x12345678, 0xaabb56cc, true}, {"CMPSTR", 0x12345678, 0x87654321, false},
		{"CMPSTR", 0x00000000, 0xffffff00, true},
	} {
		m := newMachine(t, A(c.op, 1, 2))
		m.R[1], m.R[2] = c.a, c.b
		m.steps(1)
		if (m.SR&1 != 0) != c.want {
			t.Errorf("%s %08x,%08x: T=%d", c.op, c.a, c.b, m.SR&1)
		}
	}
	for _, c := range []struct {
		op   string
		v    uint32
		want bool
	}{{"CMPPZ", 0, true}, {"CMPPZ", 0x80000000, false}, {"CMPPL", 0, false}, {"CMPPL", 1, true}, {"CMPPL", 0xffffffff, false}} {
		m := newMachine(t, A(c.op, 3))
		m.R[3] = c.v
		m.steps(1)
		if (m.SR&1 != 0) != c.want {
			t.Errorf("%s %08x: T=%d", c.op, c.v, m.SR&1)
		}
	}
	m := newMachine(t, A("CMPIM", -2))
	m.R[0] = 0xfffffffe
	m.steps(1)
	if m.SR&1 == 0 {
		t.Error("cmp/eq #-2,r0 with r0 = -2")
	}
}

func TestDecrementAndTest(t *testing.T) {
	// a dt/bf loop runs exactly R1 times
	m := newMachine(t,
		A("ADDI", 2, 1), // 1000
		A("DT", 1),      // 1002
		A("BF", -4),     // 1004: back to 1000 (1004+4-8)
		nop)             // 1006
	m.R[1] = 10
	m.runTo(t, 0x1006)
	if m.R[2] != 10 || m.R[1] != 0 || m.SR&1 != 1 {
		t.Fatalf("R1 %d R2 %d T %d", m.R[1], m.R[2], m.SR&1)
	}
}

func TestMultiply(t *testing.T) {
	m := newMachine(t, A("MULL", 1, 2), A("STSMACL", 3),
		A("MULS", 4, 5), A("STSMACL", 6),
		A("MULU", 4, 5), A("STSMACL", 7),
		A("DMULS", 8, 9), A("STSMACH", 10), A("STSMACL", 11),
		A("DMULU", 8, 9), A("STSMACH", 12), A("STSMACL", 13))
	m.R[1], m.R[2] = 0x12345678, 0x9abcdef0
	m.R[4], m.R[5] = 0xffff8000, 0x00017fff // low halves: -32768 and 32767
	m.R[8], m.R[9] = 0xfffffffe, 0x7fffffff // -2 and 2^31-1
	m.steps(13)
	if want := uint32(0x12345678 * 0x9abcdef0 & 0xffffffff); m.R[3] != want {
		t.Errorf("mul.l %08x want %08x", m.R[3], want)
	}
	if p := int32(-32768); m.R[6] != uint32(p*32767) {
		t.Errorf("muls.w %08x", m.R[6])
	}
	if m.R[7] != 0x8000*0x7fff {
		t.Errorf("mulu.w %08x", m.R[7])
	}
	s := int64(-2) * 0x7fffffff
	if m.R[10] != uint32(uint64(s)>>32) || m.R[11] != uint32(s) {
		t.Errorf("dmuls.l %08x:%08x want %016x", m.R[10], m.R[11], uint64(s))
	}
	u := uint64(0xfffffffe) * 0x7fffffff
	if m.R[12] != uint32(u>>32) || m.R[13] != uint32(u) {
		t.Errorf("dmulu.l %08x:%08x want %016x", m.R[12], m.R[13], u)
	}
}

func TestMultiplyRandom(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	for i := 0; i < 200; i++ {
		a, b := r.Uint32(), r.Uint32()
		m := newMachine(t, A("DMULS", 1, 2), A("STSMACH", 3), A("STSMACL", 4),
			A("DMULU", 1, 2), A("STSMACH", 5), A("STSMACL", 6))
		m.R[1], m.R[2] = a, b
		m.steps(6)
		s := int64(int32(a)) * int64(int32(b))
		u := uint64(a) * uint64(b)
		if uint64(m.R[3])<<32|uint64(m.R[4]) != uint64(s) || uint64(m.R[5])<<32|uint64(m.R[6]) != u {
			t.Fatalf("%08x*%08x: dmuls %08x%08x dmulu %08x%08x", a, b, m.R[3], m.R[4], m.R[5], m.R[6])
		}
	}
}

func TestMultiplyAccumulate(t *testing.T) {
	// mac.w @r1+,@r2+ twice: 3*-4 + 5*6 = 18, added to MAC
	m := newMachine(t, A("CLRMAC"), A("MACW", 2, 1), A("MACW", 2, 1), A("STSMACH", 3), A("STSMACL", 4))
	binary.BigEndian.PutUint32(m.ram[0x2000:], 0x00030005)
	binary.BigEndian.PutUint32(m.ram[0x2100:], 0xfffc0006)
	m.R[1], m.R[2] = 0x2000, 0x2100
	m.steps(5)
	if m.R[3] != 0 || m.R[4] != 18 || m.R[1] != 0x2004 || m.R[2] != 0x2104 {
		t.Fatalf("mac.w: MAC %08x:%08x R1 %x R2 %x", m.R[3], m.R[4], m.R[1], m.R[2])
	}
	// mac.l: 64-bit accumulate of signed longs
	m = newMachine(t, A("CLRMAC"), A("MACL", 2, 1), A("MACL", 2, 1), A("STSMACH", 3), A("STSMACL", 4))
	binary.BigEndian.PutUint32(m.ram[0x2000:], 0x7fffffff)
	binary.BigEndian.PutUint32(m.ram[0x2004:], 0xffffffff)
	binary.BigEndian.PutUint32(m.ram[0x2100:], 0x7fffffff)
	binary.BigEndian.PutUint32(m.ram[0x2104:], 0x00000003)
	m.R[1], m.R[2] = 0x2000, 0x2100
	m.steps(5)
	want := int64(0x7fffffff)*0x7fffffff - 3
	if got := int64(uint64(m.R[3])<<32 | uint64(m.R[4])); got != want {
		t.Fatalf("mac.l: %016x want %016x", got, want)
	}
}

func TestMacSaturation(t *testing.T) {
	m := newMachine(t, A("MACL", 2, 1), A("STSMACH", 3), A("STSMACL", 4))
	binary.BigEndian.PutUint32(m.ram[0x2000:], 0x7fffffff)
	binary.BigEndian.PutUint32(m.ram[0x2100:], 0x7fffffff)
	m.R[1], m.R[2] = 0x2000, 0x2100
	m.SR |= srS
	m.steps(3)
	if m.R[3] != 0x00007fff || m.R[4] != 0xffffffff {
		t.Errorf("mac.l saturating up: %08x:%08x", m.R[3], m.R[4])
	}
	m = newMachine(t, A("MACL", 2, 1), A("STSMACH", 3), A("STSMACL", 4))
	binary.BigEndian.PutUint32(m.ram[0x2000:], 0x80000000)
	binary.BigEndian.PutUint32(m.ram[0x2100:], 0x7fffffff)
	m.R[1], m.R[2] = 0x2000, 0x2100
	m.SR |= srS
	m.steps(3)
	if m.R[3] != 0xffff8000 || m.R[4] != 0 {
		t.Errorf("mac.l saturating down: %08x:%08x", m.R[3], m.R[4])
	}
	// mac.w with S: a 32-bit saturating add into MACL
	m = newMachine(t, A("MACW", 2, 1), A("STSMACL", 4))
	binary.BigEndian.PutUint16(m.ram[0x2000:], 0x7fff)
	binary.BigEndian.PutUint16(m.ram[0x2100:], 0x7fff)
	m.R[1], m.R[2] = 0x2000, 0x2100
	m.MACL = 0x7fff0000
	m.SR |= srS
	m.steps(2)
	if m.R[4] != 0x7fffffff {
		t.Errorf("mac.w saturating: %08x", m.R[4])
	}
}

// divide32by16 runs the SH-2 manual's unsigned 32/16 division: R1 / R0
// (R0 < 0x10000, R1 < R0<<16) leaves the quotient in R1.
func TestDivideUnsigned32by16(t *testing.T) {
	prog := []uint16{A("SHLL16", 0), A("DIV0U")}
	for i := 0; i < 16; i++ {
		prog = append(prog, A("DIV1", 1, 0))
	}
	prog = append(prog, A("ROTCL", 1), A("EXTUW", 1, 1), nop)
	r := rand.New(rand.NewSource(2))
	for i := 0; i < 300; i++ {
		d := uint32(r.Intn(0xffff) + 1)
		n := r.Uint32() % (d << 16)
		m := newMachine(t, prog...)
		m.R[0], m.R[1] = d, n
		m.runTo(t, progAt+uint32(len(prog)-1)*2)
		if m.R[1] != n/d {
			t.Fatalf("%d / %d = %d, want %d", n, d, m.R[1], n/d)
		}
	}
}

// The manual's signed 16/16 division: R1 / R0, both 16-bit signed.
func TestDivideSigned16(t *testing.T) {
	prog := []uint16{
		A("SHLL16", 0), A("EXTSW", 1, 1), A("XOR", 2, 2), A("MOV", 3, 1), A("ROTCL", 3), A("SUBC", 1, 2),
		A("DIV0S", 1, 0),
	}
	for i := 0; i < 16; i++ {
		prog = append(prog, A("DIV1", 1, 0))
	}
	prog = append(prog, A("EXTSW", 1, 1), A("ROTCL", 1), A("ADDC", 1, 2), A("EXTSW", 1, 1), nop)
	r := rand.New(rand.NewSource(3))
	for i := 0; i < 300; i++ {
		d := int32(r.Intn(0xffff) - 0x7fff)
		if d == 0 {
			d = 7
		}
		n := int32(r.Intn(0xffff) - 0x7fff)
		m := newMachine(t, prog...)
		m.R[0], m.R[1] = uint32(d)&0xffff, uint32(n)
		m.runTo(t, progAt+uint32(len(prog)-1)*2)
		if want := n / d; int32(m.R[1]) != want {
			t.Fatalf("%d / %d = %d, want %d", n, d, int32(m.R[1]), want)
		}
	}
}

// The manual's unsigned 64/32 division: R1:R2 / R3 (R1 < R3), quotient in R2.
func TestDivideUnsigned64by32(t *testing.T) {
	prog := []uint16{A("DIV0U")}
	for i := 0; i < 32; i++ {
		prog = append(prog, A("ROTCL", 2), A("DIV1", 1, 3))
	}
	prog = append(prog, A("ROTCL", 2), nop)
	r := rand.New(rand.NewSource(4))
	for i := 0; i < 200; i++ {
		d := r.Uint32() | 1
		hi := r.Uint32() % d
		lo := r.Uint32()
		m := newMachine(t, prog...)
		m.R[1], m.R[2], m.R[3] = hi, lo, d
		m.runTo(t, progAt+uint32(len(prog)-1)*2)
		want := (uint64(hi)<<32 | uint64(lo)) / uint64(d)
		if uint64(m.R[2]) != want {
			t.Fatalf("%08x%08x / %08x = %08x, want %08x", hi, lo, d, m.R[2], want)
		}
	}
}

// ------------------------------------------------------------ logic and shifts

func TestLogic(t *testing.T) {
	m := newMachine(t, A("AND", 1, 2), A("OR", 3, 2), A("XOR", 4, 2), A("NOT", 5, 2),
		A("TST", 1, 2), A("ANDI", 0x0f), A("ORI", 0x80), A("XORI", 0x01))
	m.R[1], m.R[2], m.R[3], m.R[4] = 0xff00ff00, 0x0ff00ff0, 0x0000000f, 0xffffffff
	m.R[0] = 0x123456
	m.steps(8)
	want := map[int]uint32{1: 0x0f000f00, 3: 0x0ff00fff, 4: 0xf00ff00f, 5: 0xf00ff00f, 0: 0x87}
	for r, w := range want {
		if m.R[r] != w {
			t.Errorf("R%d %08x want %08x", r, m.R[r], w)
		}
	}
	if m.SR&1 != 0 {
		t.Error("tst of overlapping values set T")
	}
	m = newMachine(t, A("TSTI", 0x80))
	m.R[0] = 0x7f
	m.steps(1)
	if m.SR&1 != 1 {
		t.Error("tst #0x80 of 0x7f should set T")
	}
}

func TestLogicOnMemory(t *testing.T) {
	m := newMachine(t, A("ANDM", 0x0f), A("ORM", 0x40), A("XORM", 0x01), A("TSTM", 0x80), A("TAS", 2), A("TAS", 2))
	m.GBR, m.R[0] = 0x2000, 0x10
	m.ram[0x2010] = 0xff
	m.R[2] = 0x2020
	m.steps(4)
	if m.ram[0x2010] != 0x4e || m.SR&1 != 1 {
		t.Fatalf("byte ops: %02x T%d", m.ram[0x2010], m.SR&1)
	}
	m.steps(1)
	if m.ram[0x2020] != 0x80 || m.SR&1 != 1 {
		t.Fatalf("tas.b of 0: %02x T%d", m.ram[0x2020], m.SR&1)
	}
	m.steps(1)
	if m.SR&1 != 0 {
		t.Fatal("tas.b of a set byte should clear T")
	}
}

func TestShifts(t *testing.T) {
	type tc struct {
		op      string
		in, t   uint32
		out, oT uint32
	}
	for _, c := range []tc{
		{"SHLL", 0x80000001, 0, 2, 1}, {"SHAL", 0x40000000, 1, 0x80000000, 0},
		{"SHLR", 0x80000001, 0, 0x40000000, 1}, {"SHAR", 0x80000002, 1, 0xc0000001, 0},
		{"ROTL", 0x80000001, 0, 3, 1}, {"ROTR", 0x80000001, 0, 0xc0000000, 1},
		{"ROTCL", 0x80000000, 1, 1, 1}, {"ROTCL", 0x00000001, 0, 2, 0},
		{"ROTCR", 0x00000001, 1, 0x80000000, 1}, {"ROTCR", 0x00000002, 0, 1, 0},
		{"SHLL2", 0xc0000001, 0, 4, 0}, {"SHLR2", 0x80000003, 1, 0x20000000, 1},
		{"SHLL8", 0x12345678, 0, 0x34567800, 0}, {"SHLR8", 0x12345678, 0, 0x00123456, 0},
		{"SHLL16", 0x12345678, 0, 0x56780000, 0}, {"SHLR16", 0x12345678, 0, 0x00001234, 0},
	} {
		m := newMachine(t, A(c.op, 4))
		m.R[4] = c.in
		m.SR = m.SR&^1 | c.t
		m.steps(1)
		if m.R[4] != c.out || m.SR&1 != c.oT {
			t.Errorf("%s %08x T%d: %08x T%d, want %08x T%d", c.op, c.in, c.t, m.R[4], m.SR&1, c.out, c.oT)
		}
	}
}

// ------------------------------------------------------------ branches

func TestBranchDelaySlot(t *testing.T) {
	// bra over two instructions; the slot runs, the skipped one does not
	m := newMachine(t,
		A("BRA", 2),     // 1000 -> 1000+4+4 = 1008
		A("ADDI", 1, 1), // 1002 slot
		A("ADDI", 2, 1), // 1004 skipped
		A("ADDI", 2, 1), // 1006 skipped
		A("ADDI", 3, 1), // 1008
		nop)
	m.runTo(t, 0x100a)
	if m.R[1] != 1 || m.R[2] != 0 || m.R[3] != 1 {
		t.Fatalf("R1 %d R2 %d R3 %d", m.R[1], m.R[2], m.R[3])
	}
}

func TestConditionalBranches(t *testing.T) {
	for _, c := range []struct {
		op     string
		t      uint32
		taken  bool
		slotOK bool // the slot runs (delayed forms)
	}{
		{"BT", 1, true, false}, {"BT", 0, false, false}, {"BF", 0, true, false}, {"BF", 1, false, false},
		{"BTS", 1, true, true}, {"BTS", 0, false, true}, {"BFS", 0, true, true}, {"BFS", 1, false, true},
	} {
		m := newMachine(t,
			A(c.op, 2),      // 1000 -> 1008
			A("ADDI", 1, 1), // 1002
			A("ADDI", 2, 1), // 1004
			nop,             // 1006
			A("ADDI", 3, 1), // 1008
			nop)
		m.SR = m.SR&^1 | c.t
		start := m.Cycles
		m.steps(1)
		cyc := m.Cycles - start
		if c.taken {
			if m.PC != 0x1008 {
				t.Errorf("%s T%d: PC %08x, want taken", c.op, c.t, m.PC)
			}
			if c.slotOK && m.R[1] != 1 {
				t.Errorf("%s: the delay slot did not run", c.op)
			}
			if !c.slotOK && m.R[1] != 0 {
				t.Errorf("%s: a non-delayed branch ran the next instruction", c.op)
			}
			wantCyc := int64(3)
			if c.slotOK {
				wantCyc = 3 // 2 for the branch, 1 for the slot
			}
			if cyc != wantCyc {
				t.Errorf("%s taken: %d cycles, want %d", c.op, cyc, wantCyc)
			}
		} else {
			if m.PC != 0x1002 {
				t.Errorf("%s T%d: PC %08x, want not taken", c.op, c.t, m.PC)
			}
			if cyc != 1 {
				t.Errorf("%s not taken: %d cycles", c.op, cyc)
			}
		}
	}
}

func TestBranchBackward(t *testing.T) {
	m := newMachine(t, nop, nop, nop, A("BRA", -3), nop) // 1006 -> 1006+4-6 = 1004
	m.steps(4)
	if m.PC != 0x1004 {
		t.Fatalf("PC %08x", m.PC)
	}
}

func TestSubroutines(t *testing.T) {
	m := newMachine(t,
		A("BSR", 4),     // 1000 -> 100c, PR = 1004
		A("MOVI", 1, 1), // 1002 slot
		A("MOVI", 2, 2), // 1004 return here
		A("BRA", -1),    // 1006: stop (branch to itself + 2... 1006+4-2 = 1008)
		nop,             // 1008
		nop,             // 100a
		A("RTS"),        // 100c
		A("MOVI", 3, 3), // 100e slot
	)
	m.steps(1)
	if m.PC != 0x100c || m.PR != 0x1004 || m.R[1] != 1 {
		t.Fatalf("bsr: PC %08x PR %08x R1 %d", m.PC, m.PR, m.R[1])
	}
	m.steps(1)
	if m.PC != 0x1004 || m.R[3] != 3 {
		t.Fatalf("rts: PC %08x R3 %d", m.PC, m.R[3])
	}
}

func TestRegisterBranches(t *testing.T) {
	// jsr @r4 / jmp @r5 / braf r6 / bsrf r7
	m := newMachine(t, A("JSR", 4), nop)
	m.R[4] = 0x2000
	m.load(0x2000, A("JMP", 5), nop)
	m.R[5] = 0x3000
	m.load(0x3000, A("BRAF", 6), nop) // 3000+4+r6
	m.R[6] = 0x100
	m.load(0x3104, A("BSRF", 7), nop) // 3104+4+r7, PR = 3108
	m.R[7] = 0x200
	m.steps(1)
	if m.PC != 0x2000 || m.PR != 0x1004 {
		t.Fatalf("jsr: PC %08x PR %08x", m.PC, m.PR)
	}
	m.steps(1)
	if m.PC != 0x3000 {
		t.Fatalf("jmp: PC %08x", m.PC)
	}
	m.steps(1)
	if m.PC != 0x3104 {
		t.Fatalf("braf: PC %08x", m.PC)
	}
	m.steps(1)
	if m.PC != 0x3308 || m.PR != 0x3108 {
		t.Fatalf("bsrf: PC %08x PR %08x", m.PC, m.PR)
	}
}

func TestJSRSlotChangesRegister(t *testing.T) {
	// the target is read before the slot runs
	m := newMachine(t, A("JSR", 4), A("MOVI", 4, 0))
	m.R[4] = 0x2000
	m.steps(1)
	if m.PC != 0x2000 {
		t.Fatalf("PC %08x", m.PC)
	}
}

func TestDelaySlotPCRelative(t *testing.T) {
	// mov.l @(disp,pc) in a delay slot reads relative to the branch + 4
	m := newMachine(t,
		A("BRA", 5),       // 1000 -> 100e
		A("MOVLPC", 1, 1), // 1002: (1000+4)&~3 + 4 = 1008
		nop, nop,          // 1004 1006
		0x1111, 0x2222, // 1008
		0x3333, 0x4444, // 100c
	)
	m.steps(1)
	if m.R[1] != 0x11112222 {
		t.Fatalf("slot load %08x", m.R[1])
	}
}

// ------------------------------------------------------------ system registers

func TestSystemRegisters(t *testing.T) {
	m := newMachine(t,
		A("LDCSR", 1), A("STCSR", 2),
		A("LDCVBR", 3), A("STCVBR", 4),
		A("LDSMACH", 5), A("LDSMACL", 6), A("LDSPR", 7),
		A("STSMACH", 8), A("STSMACL", 9), A("STSPR", 10),
		A("STCMSR", 15), A("STCMGBR", 15), A("STCMVBR", 15),
		A("STSMMACH", 15), A("STSMMACL", 15), A("STSMPR", 15),
		A("LDSMPR", 15), A("LDSMMACL", 15), A("LDSMMACH", 15),
		A("LDCMVBR", 15), A("LDCMGBR", 15), A("LDCMSR", 15),
	)
	m.R[1] = 0xffffffff
	m.R[3], m.R[5], m.R[6], m.R[7] = 0x400, 0x11, 0x22, 0x33
	m.GBR = 0x55
	m.steps(10)
	if m.R[2] != srMask {
		t.Errorf("SR keeps only its defined bits: %08x", m.R[2])
	}
	if m.R[4] != 0x400 || m.R[8] != 0x11 || m.R[9] != 0x22 || m.R[10] != 0x33 {
		t.Errorf("stc/sts: %x %x %x %x", m.R[4], m.R[8], m.R[9], m.R[10])
	}
	m.SR = 0x0f0 | srT
	sr := m.SR
	m.steps(6)
	if m.R[15] != stackAt-24 || m.mem32(stackAt-4) != sr || m.mem32(stackAt-8) != 0x55 {
		t.Errorf("stores: SP %08x %08x %08x", m.R[15], m.mem32(stackAt-4), m.mem32(stackAt-8))
	}
	m.MACH, m.MACL, m.PR, m.VBR, m.GBR, m.SR = 0, 0, 0, 0, 0, 0xf0
	m.steps(6)
	if m.R[15] != stackAt || m.MACH != 0x11 || m.MACL != 0x22 || m.PR != 0x33 || m.VBR != 0x400 || m.GBR != 0x55 || m.SR != sr {
		t.Errorf("loads: SP %08x MACH %x MACL %x PR %x VBR %x GBR %x SR %x", m.R[15], m.MACH, m.MACL, m.PR, m.VBR, m.GBR, m.SR)
	}
}

func TestClearSetT(t *testing.T) {
	m := newMachine(t, A("SETT"), A("CLRT"), A("CLRMAC"))
	m.MACH, m.MACL = 1, 2
	m.steps(1)
	if m.SR&1 != 1 {
		t.Fatal("sett")
	}
	m.steps(2)
	if m.SR&1 != 0 || m.MACH != 0 || m.MACL != 0 {
		t.Fatal("clrt/clrmac")
	}
}

// ------------------------------------------------------------ exceptions and interrupts

func TestTrapaAndRTE(t *testing.T) {
	m := newMachine(t, A("TRAPA", 0x20), A("MOVI", 2, 2), nop)
	m.VBR = 0x400
	m.setVector(0x20, 0x3000)
	m.load(0x3000, A("MOVI", 1, 1), A("RTE"), nop)
	m.SR = 0x0f1
	start := m.Cycles
	m.steps(1)
	if m.Cycles-start != 8 {
		t.Errorf("trapa: %d cycles", m.Cycles-start)
	}
	if m.PC != 0x3000 || m.R[15] != stackAt-8 || m.mem32(stackAt-4) != 0x0f1 || m.mem32(stackAt-8) != 0x1002 {
		t.Fatalf("trapa: PC %08x SP %08x stack %08x %08x", m.PC, m.R[15], m.mem32(stackAt-4), m.mem32(stackAt-8))
	}
	m.SR = 0
	m.steps(2)
	if m.PC != 0x1002 || m.SR != 0x0f1 || m.R[15] != stackAt || m.R[1] != 1 {
		t.Fatalf("rte: PC %08x SR %03x SP %08x", m.PC, m.SR, m.R[15])
	}
}

func TestIllegalInstruction(t *testing.T) {
	m := newMachine(t, 0xfffd)
	m.setVector(4, 0x3000)
	m.steps(1)
	if m.PC != 0x3000 || m.mem32(stackAt-8) != 0x1000 {
		t.Fatalf("PC %08x pushed %08x", m.PC, m.mem32(stackAt-8))
	}
}

func TestSlotIllegal(t *testing.T) {
	// a branch in a delay slot raises the slot illegal exception with the
	// first branch's address pushed
	m := newMachine(t, A("BRA", 4), A("BRA", 4))
	m.setVector(6, 0x3000)
	m.steps(1)
	if m.PC != 0x3000 || m.mem32(stackAt-8) != 0x1000 {
		t.Fatalf("PC %08x pushed %08x", m.PC, m.mem32(stackAt-8))
	}
}

func TestExternalInterrupt(t *testing.T) {
	m := newMachine(t, nop, nop, A("LDCSR", 1), nop, nop)
	m.setVector(66, 0x3000) // IRL 4 and 5: autovector 66
	m.load(0x3000, nop)
	m.SR = 0x050 // mask 5
	m.R[1] = 0x030
	m.SetIRL(4)
	m.steps(1)
	if m.PC != 0x1002 {
		t.Fatalf("a level below the mask was taken (PC %08x)", m.PC)
	}
	m.SetIRL(5)
	m.steps(1)
	if m.PC != 0x1004 {
		t.Fatal("a level equal to the mask was taken")
	}
	m.steps(1) // ldc r1,sr lowers the mask to 3
	m.steps(1)
	if m.PC != 0x3000 {
		t.Fatalf("interrupt not taken: PC %08x", m.PC)
	}
	if m.SR&0xf0 != 0x50 || m.mem32(stackAt-4) != 0x030 || m.mem32(stackAt-8) != 0x1006 {
		t.Fatalf("SR %03x stack %08x %08x", m.SR, m.mem32(stackAt-4), m.mem32(stackAt-8))
	}
	if len(m.bus.acks) != 1 || m.bus.acks[0] != 5 {
		t.Errorf("acks %v", m.bus.acks)
	}
	// still asserted but now masked: no second interrupt
	m.steps(1)
	if m.PC != 0x3002 {
		t.Fatalf("taken again: PC %08x", m.PC)
	}
}

func TestAutovectors(t *testing.T) {
	for level := 1; level <= 15; level++ {
		m := newMachine(t, nop, nop)
		vec := 64 + level/2
		m.setVector(vec, 0x3000+uint32(vec)*4)
		m.SR = 0
		m.SetIRL(level)
		m.steps(1)
		if m.PC != 0x3000+uint32(vec)*4 {
			t.Errorf("IRL %d: PC %08x, want vector %d", level, m.PC, vec)
		}
	}
}

func TestExternalVectorMode(t *testing.T) {
	bus := &testBus{mem: map[uint32]uint8{}, vector: 0x50}
	c := New(vecBus{bus})
	ram := make([]byte, 0x10000)
	c.Map(0, ram, true)
	binary.BigEndian.PutUint32(ram[0:], progAt)
	binary.BigEndian.PutUint32(ram[4:], stackAt)
	binary.BigEndian.PutUint32(ram[0x50*4:], 0x3000)
	binary.BigEndian.PutUint16(ram[progAt:], nop)
	c.Reset()
	c.Write16(0xfffffee0, 0x0001) // ICR VECMD
	c.SR = 0
	c.SetIRL(9)
	c.Step()
	if c.PC != 0x3000 {
		t.Fatalf("PC %08x", c.PC)
	}
}

func TestNMI(t *testing.T) {
	m := newMachine(t, nop, nop)
	m.setVector(11, 0x3000)
	m.NMI()
	if m.PC != 0x3000 || m.SR&0xf0 != 0xf0 {
		t.Fatalf("PC %08x SR %03x", m.PC, m.SR)
	}
}

func TestSleepWakesOnInterrupt(t *testing.T) {
	m := newMachine(t, A("SLEEP"), nop)
	m.setVector(64, 0x3000)
	m.load(0x3000, nop)
	m.SR = 0
	m.steps(1)
	if !m.Sleeping() {
		t.Fatal("not sleeping")
	}
	m.Run(m.Cycles + 1000)
	if m.PC != 0x1002 || !m.Sleeping() {
		t.Fatalf("woke up by itself (PC %08x)", m.PC)
	}
	m.SetIRL(1)
	m.Run(m.Cycles + 10)
	if m.Sleeping() || m.PC < 0x3000 {
		t.Fatalf("still asleep (PC %08x)", m.PC)
	}
	// the return address is the instruction after SLEEP
	if m.mem32(stackAt-8) != 0x1002 {
		t.Fatalf("pushed %08x", m.mem32(stackAt-8))
	}
}

func TestRunStopsAtTarget(t *testing.T) {
	m := newMachine(t, A("BRA", -2), nop) // a two-instruction loop of 3 cycles
	m.Run(1000)
	if m.Cycles < 1000 || m.Cycles > 1003 {
		t.Fatalf("Cycles %d", m.Cycles)
	}
	m.Run(500)
	if m.Cycles > 1003 {
		t.Fatal("ran when already past the target")
	}
}

// ------------------------------------------------------------ memory map

func TestBusAndAreas(t *testing.T) {
	m := newMachine(t, A("MOVLS", 1, 2), A("MOVLL", 3, 1), A("MOVLS", 4, 2), A("MOVLL", 5, 4), A("MOVLL", 6, 7))
	m.R[1] = 0x02000000 // not mapped: the bus
	m.R[2] = 0x13572468
	m.R[4] = 0x22000010 // cache-through mirror
	m.R[7] = 0xc0000010 // cache data array
	m.steps(2)
	if m.bus.mem[0x02000000] != 0x13 || m.R[3] != 0x13572468 {
		t.Fatalf("bus: %x %08x", m.bus.mem[0x02000000], m.R[3])
	}
	m.steps(2)
	if m.bus.mem[0x02000013] != 0x68 || m.R[5] != 0x13572468 {
		t.Fatal("the 0x20000000 mirror should reach the bus")
	}
	m.Write32(0xc0000010, 0xabcdef01)
	m.steps(1)
	if m.R[6] != 0xabcdef01 {
		t.Fatalf("cache RAM %08x", m.R[6])
	}
}

func TestMapReadOnly(t *testing.T) {
	rom := make([]byte, 0x10000)
	rom[0x10] = 0x55
	m := newMachine(t, A("MOVBS", 1, 2), A("MOVBL", 3, 1))
	m.Map(0x00100000, rom, false)
	m.R[1], m.R[2] = 0x00100010, 0xaa
	m.steps(2)
	if rom[0x10] != 0x55 || m.R[3] != 0x55 {
		t.Fatalf("rom %x r3 %x", rom[0x10], m.R[3])
	}
	m.Unmap(0x00100000, 0x10000)
	if m.Read8(0x00100010) != 0 || m.bus.reads == 0 {
		t.Fatal("unmapped pages should go to the bus")
	}
}

func TestMapPanicsOnPartialPage(t *testing.T) {
	defer func() {
		if recover() == nil {
			t.Fatal("no panic")
		}
	}()
	m := newMachine(t, nop)
	m.Map(0x1000, make([]byte, 0x1000), true)
}

// ------------------------------------------------------------ on-chip modules

const onBase = 0xfffffe00

func TestDivisionUnit32(t *testing.T) {
	m := newMachine(t, nop)
	for _, c := range []struct{ n, d int32 }{{100, 7}, {-100, 7}, {100, -7}, {-7, -100}, {0x7fffffff, 3}} {
		m.Write32(onBase+0x100, uint32(c.d)) // DVSR
		m.Write32(onBase+0x104, uint32(c.n)) // DVDNT
		if q, r := int32(m.Read32(onBase+0x104)), int32(m.Read32(onBase+0x110)); q != c.n/c.d || r != c.n%c.d {
			t.Errorf("%d/%d: q %d r %d", c.n, c.d, q, r)
		}
	}
	// the registers mirror every 0x20 bytes from 0x120
	m.Write32(onBase+0x120, 9)
	m.Write32(onBase+0x124, 81)
	if m.Read32(onBase+0x104) != 9 {
		t.Errorf("mirror: %d", m.Read32(onBase+0x104))
	}
	// division by zero: overflow flag, saturated quotient
	m.Write32(onBase+0x108, 0)
	m.Write32(onBase+0x100, 0)
	m.Write32(onBase+0x104, 5)
	if m.Read32(onBase+0x108)&1 == 0 || m.Read32(onBase+0x104) != 0x7fffffff {
		t.Errorf("divide by zero: DVCR %x q %08x", m.Read32(onBase+0x108), m.Read32(onBase+0x104))
	}
}

func TestDivisionUnit64(t *testing.T) {
	m := newMachine(t, nop)
	n := int64(-123456789012)
	d := int32(1000003)
	m.Write32(onBase+0x100, uint32(d))
	m.Write32(onBase+0x110, uint32(uint64(n)>>32))
	m.Write32(onBase+0x114, uint32(n))
	if q, r := int32(m.Read32(onBase+0x114)), int32(m.Read32(onBase+0x110)); int64(q) != n/int64(d) || int64(r) != n%int64(d) {
		t.Fatalf("q %d r %d", q, r)
	}
	// a quotient that does not fit: overflow
	m.Write32(onBase+0x108, 0)
	m.Write32(onBase+0x100, 1)
	m.Write32(onBase+0x110, 1)
	m.Write32(onBase+0x114, 0)
	if m.Read32(onBase+0x108)&1 == 0 {
		t.Fatal("no overflow")
	}
}

func TestDivisionUnitInterrupt(t *testing.T) {
	m := newMachine(t, nop, nop)
	m.Write16(onBase+0xe2, 0x9000) // IPRA: DIVU level 9
	m.Write32(onBase+0x10c, 0x55)  // VCRDIV
	m.Write32(onBase+0x108, 2)     // OVFIE
	m.setVector(0x55, 0x3000)
	m.SR = 0
	m.Write32(onBase+0x100, 0)
	m.Write32(onBase+0x104, 1)
	m.steps(1)
	if m.PC != 0x3000 || m.SR&0xf0 != 0x90 {
		t.Fatalf("PC %08x SR %03x", m.PC, m.SR)
	}
}

func TestFreeRunningTimer(t *testing.T) {
	m := newMachine(t, A("BRA", -2), nop)
	m.Write8(onBase+0x16, 0) // TCR: phi/8
	m.Write8(onBase+0x12, 0) // FRC = 0
	m.Write8(onBase+0x13, 0)
	start := m.Cycles
	m.Run(start + 8*1000)
	m.Read8(onBase + 0x12) // FRCH latches FRCL
	frc := int(m.Read8(onBase+0x12))<<8 | int(m.Read8(onBase+0x13))
	_ = frc
	hi := m.Read8(onBase + 0x12)
	lo := m.Read8(onBase + 0x13)
	got := int(hi)<<8 | int(lo)
	if got < 999 || got > 1002 {
		t.Fatalf("FRC %d after 8000 cycles at phi/8", got)
	}
}

func TestFreeRunningTimerCompareInterrupt(t *testing.T) {
	m := newMachine(t, A("BRA", -2), nop)
	m.setVector(0x60, 0x3000)
	m.load(0x3000, A("BRA", -2), nop)
	m.Write16(onBase+0x60, 0x0f00) // IPRB: FRT level 15
	m.Write16(onBase+0x66, 0x0060) // VCRC: OCI vector 0x60
	m.Write8(onBase+0x17, 0xe0)    // TOCR: OCRA selected
	m.Write8(onBase+0x14, 0x01)    // OCRA = 0x100
	m.Write8(onBase+0x15, 0x00)
	m.Write8(onBase+0x10, 0x08) // TIER: OCIAE
	m.Write8(onBase+0x16, 0)    // phi/8
	m.Write8(onBase+0x12, 0)
	m.Write8(onBase+0x13, 0)
	m.SR = 0
	m.Run(m.Cycles + 0x100*8 - 50)
	if m.PC >= 0x3000 {
		t.Fatal("interrupt too early")
	}
	m.Run(m.Cycles + 100)
	if m.PC < 0x3000 {
		t.Fatal("no compare-match interrupt")
	}
	if m.Read8(onBase+0x11)&0x08 == 0 {
		t.Fatal("OCFA not set")
	}
	// clearing the flag (write 0 after reading 1) drops the request
	m.Write8(onBase+0x11, 0)
	if m.Read8(onBase+0x11)&0x08 != 0 {
		t.Fatal("OCFA not cleared")
	}
}

func TestWatchdogIntervalTimer(t *testing.T) {
	m := newMachine(t, A("BRA", -2), nop)
	m.setVector(0x44, 0x3000)
	m.load(0x3000, A("BRA", -2), nop)
	m.Write16(onBase+0xe2, 0x00a0)      // IPRA: WDT level 10
	m.Write16(onBase+0xe4, 0x4400)      // VCRWDT: vector 0x44
	m.Write16(onBase+0x80, 0x5a00|0xf0) // WTCNT = 0xf0
	m.Write16(onBase+0x80, 0xa500|0x21) // WTCSR: interval mode, timer on, phi/64
	m.SR = 0
	m.Run(m.Cycles + 16*64 - 40)
	if m.PC >= 0x3000 {
		t.Fatal("interrupt too early")
	}
	m.Run(m.Cycles + 80)
	if m.PC < 0x3000 {
		t.Fatal("no interval interrupt")
	}
	if m.Read8(onBase+0x80)&0x80 == 0 {
		t.Fatal("OVF not set")
	}
}

func TestDMA(t *testing.T) {
	m := newMachine(t, nop)
	for i := 0; i < 64; i++ {
		m.ram[0x2000+i] = uint8(i * 3)
	}
	m.Write32(onBase+0x180, 0x2000) // SAR0
	m.Write32(onBase+0x184, 0x4000) // DAR0
	m.Write32(onBase+0x188, 16)     // TCR0: 16 longs
	m.Write32(onBase+0x1b0, 1)      // DMAOR: DME
	// CHCR0: dest +, src +, longs, auto request, enable
	m.Write32(onBase+0x18c, 1<<14|1<<12|2<<10|1<<9|1)
	for i := 0; i < 64; i++ {
		if m.ram[0x4000+i] != uint8(i*3) {
			t.Fatalf("byte %d: %x", i, m.ram[0x4000+i])
		}
	}
	if m.Read32(onBase+0x18c)&2 == 0 || m.Read32(onBase+0x188) != 0 {
		t.Fatalf("TE not set or count left: CHCR %x TCR %x", m.Read32(onBase+0x18c), m.Read32(onBase+0x188))
	}
	if m.Read32(onBase+0x180) != 0x2040 || m.Read32(onBase+0x184) != 0x4040 {
		t.Fatalf("addresses %x %x", m.Read32(onBase+0x180), m.Read32(onBase+0x184))
	}
	// a fixed destination with bytes: only the last byte remains
	m.Write32(onBase+0x190, 0x2000)
	m.Write32(onBase+0x194, 0x5000)
	m.Write32(onBase+0x198, 4)
	m.Write32(onBase+0x19c, 0<<14|1<<12|0<<10|1<<9|1)
	if m.ram[0x5000] != 9 || m.ram[0x5001] != 0 {
		t.Fatalf("fixed destination: %x %x", m.ram[0x5000], m.ram[0x5001])
	}
}

func TestOnChipByteWordAccess(t *testing.T) {
	m := newMachine(t, nop)
	m.Write16(onBase+0x60, 0x1234) // IPRB
	if m.Read8(onBase+0x60) != 0x12 || m.Read8(onBase+0x61) != 0x34 || m.Read16(onBase+0x60) != 0x1234 {
		t.Fatal("16-bit register access")
	}
	m.Write32(onBase+0x100, 0x11223344)
	if m.Read16(onBase+0x102) != 0x3344 || m.Read8(onBase+0x101) != 0x22 {
		t.Fatal("32-bit register parts")
	}
}

// ------------------------------------------------------------ state

func TestSaveStateRoundTrip(t *testing.T) {
	// a program that keeps changing registers and memory
	prog := []uint16{
		A("ADDI", 1, 3),   // 1000
		A("MOVLM", 15, 1), // push r1
		A("ADD", 2, 1),    // 1004
		A("DMULS", 1, 2),  // 1006
		A("MOVLP", 3, 15), // pop into r3
		A("BRA", -7),      // 100a -> 100a+4-14 = 1000
		A("XOR", 4, 3),    // slot
	}
	a := newMachine(t, prog...)
	a.Write16(onBase+0x80, 0x5a00) // start the WDT so on-chip state matters
	a.Write16(onBase+0x80, 0xa521)
	a.Write8(onBase+0x16, 1)
	a.Write32(0xc0000100, 0x12345678)
	a.Run(10000)
	st := a.AppendState(nil)
	ram := append([]byte(nil), a.ram...)
	a.Run(30000)

	b := newMachine(t, prog...)
	copy(b.ram, ram)
	rest, err := b.LoadState(append(st, 0xee))
	if err != nil {
		t.Fatal(err)
	}
	if len(rest) != 1 || rest[0] != 0xee {
		t.Fatalf("LoadState left %d bytes", len(rest))
	}
	b.Run(30000)
	if a.R != b.R || a.PC != b.PC || a.SR != b.SR || a.MACH != b.MACH || a.MACL != b.MACL || a.Cycles != b.Cycles {
		t.Fatalf("diverged:\n%v %x\n%v %x", a.R, a.PC, b.R, b.PC)
	}
	if a.Read8(onBase+0x81) != b.Read8(onBase+0x81) || b.Read32(0xc0000100) != 0x12345678 {
		t.Fatal("on-chip state differs")
	}
	if _, err := b.LoadState(st[:10]); err == nil {
		t.Fatal("a short state loaded")
	}
	bad := append([]byte(nil), st...)
	bad[0] = 99
	if _, err := b.LoadState(bad); err == nil {
		t.Fatal("a wrong version loaded")
	}
}

// ------------------------------------------------------------ misc

func TestEveryOpcodeRuns(t *testing.T) {
	// every opcode runs without panicking (from a sane state)
	m := newMachine(t)
	m.setVector(4, 0x3000)
	m.setVector(6, 0x3000)
	for op := 0; op < 0x10000; op++ {
		for i := range m.R {
			m.R[i] = 0x2000 + uint32(i)*0x100
		}
		m.R[15] = stackAt
		m.SR, m.GBR, m.VBR, m.PR = 0xf0, 0x2000, 0, 0x2000
		m.PC = 0x1000
		m.load(0x1000, uint16(op), nop)
		m.Step()
	}
}

func TestDisasm(t *testing.T) {
	read := func(a uint32) uint32 { return 0x12345678 }
	for _, c := range []struct {
		op   uint16
		pc   uint32
		want string
	}{
		{A("NOP"), 0, "nop"},
		{A("MOV", 3, 1), 0, "mov r1,r3"},
		{A("MOVI", 2, -1), 0, "mov #-1,r2"},
		{A("BRA", -2), 0x1000, "bra $00001000"},
		{A("RTS"), 0, "rts"},
		{A("JSR", 4), 0, "jsr @r4"},
		{A("STSMACL", 3), 0, "sts macl,r3"},
	} {
		got := Disasm(c.pc, c.op, read)
		if !strings.HasPrefix(strings.ToLower(got), c.want) {
			t.Errorf("%04x: %q, want %q", c.op, got, c.want)
		}
	}
	// every opcode disassembles to something
	for op := 0; op < 0x10000; op++ {
		if Disasm(0, uint16(op), nil) == "" {
			t.Fatalf("%04x: empty", op)
		}
	}
}

func BenchmarkLoop(b *testing.B) {
	t := &testing.T{}
	m := newMachine(t, A("ADDI", 1, 1), A("DT", 2), A("BF", -4), A("BRA", -5), A("MOVI", 2, 100))
	m.R[2] = 100
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		m.Run(m.Cycles + 1000)
	}
}
