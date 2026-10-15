package z80

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// testBus is 64 KB of RAM and a list of port values; it logs writes and outs.
type testBus struct {
	mem    [65536]uint8
	ports  [][3]any // port, value, "r" / "w" (as in the SingleStepTests files)
	writes []uint32
	outs   []uint32
	acks   int
}

func (b *testBus) Read(a uint16) uint8 { return b.mem[a] }
func (b *testBus) Write(a uint16, v uint8) {
	b.mem[a] = v
	b.writes = append(b.writes, uint32(a), uint32(v))
}
func (b *testBus) In(p uint16) uint8 {
	for _, e := range b.ports {
		if num(e[0]) == int(p) && e[2] == "r" {
			return uint8(num(e[1]))
		}
	}
	return 0xff
}
func (b *testBus) Out(p uint16, v uint8) { b.outs = append(b.outs, uint32(p), uint32(v)) }
func (b *testBus) Vector() uint8         { return 0x20 }
func (b *testBus) Ack()                  { b.acks++ }

func num(v any) int {
	switch x := v.(type) {
	case float64:
		return int(x)
	case int:
		return x
	}
	return -1
}

// prog assembles bytes at org in a fresh test bus.
func newProg(org uint16, code ...uint8) (*CPU, *testBus) {
	b := &testBus{}
	copy(b.mem[org:], code)
	c := New(b)
	r := c.Regs()
	r.PC, r.SP = org, 0xfff0
	c.SetRegs(r)
	return c, b
}

// A small program: a loop, a call, LDIR, an 8-bit multiply, DAA, IX
// addressing, and HALT.
func TestProgram(t *testing.T) {
	code := []uint8{
		0x31, 0x00, 0xf0, // ld sp,0xf000
		0x06, 0x0a, // ld b,10
		0xaf,       // xor a
		0x80,       // loop: add a,b
		0x10, 0xfd, // djnz loop
		0x32, 0x00, 0x90, // ld (0x9000),a        ; 55
		0xcd, 0x30, 0x00, // call sub
		0x21, 0x00, 0x90, // ld hl,0x9000
		0x11, 0x10, 0x90, // ld de,0x9010
		0x01, 0x04, 0x00, // ld bc,4
		0xed, 0xb0, // ldir
		0x3e, 0x19, // ld a,0x19
		0xc6, 0x28, // add a,0x28
		0x27,             // daa                   ; 0x47
		0x32, 0x20, 0x90, // ld (0x9020),a
		0xdd, 0x21, 0x00, 0x90, // ld ix,0x9000
		0xdd, 0x7e, 0x01, // ld a,(ix+1)         ; 0x99 from sub
		0xdd, 0x77, 0x21, // ld (ix+0x21),a
		0x76, // halt
	}
	sub := []uint8{
		0x3e, 0x99, // ld a,0x99
		0x32, 0x01, 0x90, // ld (0x9001),a
		0xc9, // ret
	}
	c, b := newProg(0, code...)
	copy(b.mem[0x30:], sub)
	for i := 0; i < 1000 && !c.Halted(); i++ {
		c.Step()
	}
	if !c.Halted() {
		t.Fatal("program didn't reach HALT")
	}
	want := map[uint16]uint8{0x9000: 55, 0x9001: 0x99, 0x9010: 55, 0x9011: 0x99, 0x9020: 0x47, 0x9021: 0x99}
	for a, v := range want {
		if b.mem[a] != v {
			t.Errorf("mem[%04x] = %02x, want %02x", a, b.mem[a], v)
		}
	}
	if c.PC() != uint16(len(code)) {
		t.Errorf("PC %04x, want %04x", c.PC(), len(code))
	}
}

func TestInterrupts(t *testing.T) {
	// im 2 with I=0x40 and vector 0x20 -> handler address at 0x4020
	c, b := newProg(0x100, 0xed, 0x5e, 0x3e, 0x40, 0xed, 0x47, 0xfb, 0x00, 0x76) // im 2; ld a,0x40; ld i,a; ei; nop; halt
	b.mem[0x4020], b.mem[0x4021] = 0x00, 0x80
	c.IRQ = true
	c.Step()                              // im 2
	c.Step()                              // ld a
	c.Step()                              // ld i,a
	c.Step()                              // ei
	if c.Step() != 4 || c.PC() != 0x108 { // the instruction after EI runs before the interrupt
		t.Fatalf("interrupt taken right after EI (pc %04x)", c.PC())
	}
	if n := c.Step(); n != 19 || c.PC() != 0x8000 || b.acks != 1 {
		t.Fatalf("IM 2 interrupt: %d T-states, pc %04x, acks %d", n, c.PC(), b.acks)
	}
	c.NMI()
	if n := c.Step(); n != 11 || c.PC() != 0x66 {
		t.Fatalf("NMI: %d T-states, pc %04x", n, c.PC())
	}
}

func TestStateRoundTrip(t *testing.T) {
	c, _ := newProg(0, 0x00)
	r := Regs{A: 1, F: 2, B: 3, C: 4, D: 5, E: 6, H: 7, L: 8, IX: 0x1234, IY: 0x5678, SP: 0x9abc, PC: 0xdef0,
		I: 9, R: 10, WZ: 0x1111, AF_: 0x2222, BC_: 0x3333, DE_: 0x4444, HL_: 0x5555,
		IFF1: true, IFF2: false, IM: 2, Halted: true, EI: false, Q: 0x28, NMIPending: true, IRQ: true}
	c.SetRegs(r)
	s := c.AppendState([]byte{0xaa})
	if len(s) != 1+StateSize {
		t.Fatalf("state is %d bytes, want %d", len(s)-1, StateSize)
	}
	d, _ := newProg(0, 0x00)
	rest, err := d.LoadState(append(s[1:], 0xbb))
	if err != nil || !bytes.Equal(rest, []byte{0xbb}) {
		t.Fatalf("LoadState: %v, rest %x", err, rest)
	}
	if d.Regs() != r {
		t.Fatalf("got %+v\nwant %+v", d.Regs(), r)
	}
	if _, err := d.LoadState(s[1:10]); err == nil {
		t.Fatal("a short state was accepted")
	}
}

// ---- comparison with the web Z80 ----

type ssCase struct {
	Name    string            `json:"name"`
	Initial map[string]any    `json:"initial"`
	Final   map[string]any    `json:"final"`
	Ports   [][3]any          `json:"ports"`
	Cycles  []json.RawMessage `json:"cycles"`
}

var stateKeys = []string{"a", "f", "b", "c", "d", "e", "h", "l", "ix", "iy", "sp", "pc", "i", "r", "wz", "af_", "bc_", "de_", "hl_", "iff1", "iff2", "im"}

func regsFrom(m map[string]any) Regs {
	g := func(k string) int { return num(m[k]) }
	return Regs{A: uint8(g("a")), F: uint8(g("f")), B: uint8(g("b")), C: uint8(g("c")), D: uint8(g("d")), E: uint8(g("e")),
		H: uint8(g("h")), L: uint8(g("l")), IX: uint16(g("ix")), IY: uint16(g("iy")), SP: uint16(g("sp")), PC: uint16(g("pc")),
		I: uint8(g("i")), R: uint8(g("r")), WZ: uint16(g("wz")), AF_: uint16(g("af_")), BC_: uint16(g("bc_")),
		DE_: uint16(g("de_")), HL_: uint16(g("hl_")), IFF1: g("iff1") != 0, IFF2: g("iff2") != 0, IM: uint8(g("im")),
		EI: g("ei") != 0, Q: uint8(g("q"))}
}

func stateWords(r Regs) []uint32 {
	b := func(v bool) uint32 {
		if v {
			return 1
		}
		return 0
	}
	return []uint32{uint32(r.A), uint32(r.F), uint32(r.B), uint32(r.C), uint32(r.D), uint32(r.E), uint32(r.H), uint32(r.L),
		uint32(r.IX), uint32(r.IY), uint32(r.SP), uint32(r.PC), uint32(r.I), uint32(r.R), uint32(r.WZ),
		uint32(r.AF_), uint32(r.BC_), uint32(r.DE_), uint32(r.HL_), b(r.IFF1), b(r.IFF2), uint32(r.IM),
		b(r.Halted), b(r.EI), uint32(r.Q), b(r.NMIPending), b(r.IRQ)}
}

// TestMatchesJS runs every SingleStepTests/z80 case through the Go processor
// and the web one (in Node) and requires the same registers, T-states,
// memory writes and port writes.
//
//	RP_Z80_TESTS=/path/to/SingleStepTests/z80/v1 go test ./cores/z80 -run MatchesJS
//
// RP_Z80_FILES limits it to files whose names start with one of the
// comma-separated prefixes. It also reports (without failing) how many
// cases differ from the test suite's own expected results.
func TestMatchesJS(t *testing.T) {
	dir := os.Getenv("RP_Z80_TESTS")
	if dir == "" {
		t.Skip("RP_Z80_TESTS not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	emu, _ := filepath.Abs("../../web/emu")
	ref, _ := filepath.Abs("testdata/z80ref.js")
	files, _ := filepath.Glob(filepath.Join(dir, "*.json"))
	sort.Strings(files)
	var only []string
	if s := os.Getenv("RP_Z80_FILES"); s != "" {
		only = strings.Split(s, ",")
	}
	total, suiteDiff := 0, 0
	for _, file := range files {
		base := filepath.Base(file)
		if only != nil {
			ok := false
			for _, p := range only {
				ok = ok || strings.HasPrefix(base, p)
			}
			if !ok {
				continue
			}
		}
		raw, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		var cases []ssCase
		if err := json.Unmarshal(raw, &cases); err != nil {
			t.Fatalf("%s: %v", base, err)
		}
		cmd := exec.Command(node, ref, emu, file)
		cmd.Stderr = os.Stderr
		out, err := cmd.Output()
		if err != nil {
			t.Fatalf("%s: node: %v", base, err)
		}
		words := make([]uint32, len(out)/4)
		for i := range words {
			words[i] = binary.LittleEndian.Uint32(out[i*4:])
		}
		b := &testBus{}
		c := New(b)
		for _, tc := range cases {
			b.mem = [65536]uint8{}
			for _, e := range tc.Initial["ram"].([]any) {
				p := e.([]any)
				b.mem[num(p[0])] = uint8(num(p[1]))
			}
			b.ports, b.writes, b.outs = tc.Ports, nil, nil
			c.SetRegs(regsFrom(tc.Initial))
			used := c.Step()
			got := []uint32{uint32(used)}
			got = append(got, stateWords(c.Regs())...)
			got = append(got, uint32(len(b.writes)/2))
			got = append(got, b.writes...)
			got = append(got, uint32(len(b.outs)/2))
			got = append(got, b.outs...)
			if len(words) < len(got) {
				t.Fatalf("%s: JS output ends early", tc.Name)
			}
			want := words[:len(got)]
			for i := range got {
				if got[i] != want[i] {
					t.Fatalf("%s: differs from the JS Z80 at word %d:\n go %v\n js %v", tc.Name, i, got, want)
				}
			}
			words = words[len(got):]
			total++
			// against the suite's expected values (information only)
			exp := regsFrom(tc.Final)
			r := c.Regs()
			r.Halted, r.Q, r.EI = false, exp.Q, exp.EI
			if r != exp || used != len(tc.Cycles) {
				suiteDiff++
			}
		}
		if len(words) != 0 {
			t.Fatalf("%s: %d words of JS output left over", base, len(words))
		}
	}
	t.Logf("%d cases identical to the JS Z80 (%d differ from the suite's expected registers or cycle counts, as the JS core does)", total, suiteDiff)
}
