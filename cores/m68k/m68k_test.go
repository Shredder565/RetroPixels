package m68k

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
)

// testBus: 16 MB of memory holding a pattern made from a seed and the
// address (the same one testdata/m68kref.js uses), with a log of every access.
type testBus struct {
	seed   uint32
	mem    map[uint32]uint8
	log    []uint32
	ackVec int
}

func (b *testBus) pat(a uint32) uint8 {
	h := a*0x9e3779b1 + b.seed
	return uint8((h >> 13) ^ (h >> 5) ^ (h >> 24))
}
func (b *testBus) rb(a uint32) uint8 {
	if v, ok := b.mem[a]; ok {
		return v
	}
	return b.pat(a)
}
func (b *testBus) Read8(a uint32) uint8 {
	v := b.rb(a)
	b.log = append(b.log, 1, a, uint32(v))
	return v
}
func (b *testBus) Read16(a uint32) uint16 {
	v := uint16(b.rb(a))<<8 | uint16(b.rb(a+1))
	b.log = append(b.log, 2, a, uint32(v))
	return v
}
func (b *testBus) Write8(a uint32, v uint8) {
	b.mem[a] = v
	b.log = append(b.log, 3, a, uint32(v))
}
func (b *testBus) Write16(a uint32, v uint16) {
	b.mem[a] = uint8(v >> 8)
	b.mem[a+1] = uint8(v)
	b.log = append(b.log, 4, a, uint32(v))
}
func (b *testBus) Ack(level int) int {
	b.log = append(b.log, 5, uint32(level), uint32(b.ackVec))
	if b.ackVec != 0 {
		return b.ackVec
	}
	return -1
}
func (b *testBus) Reset() { b.log = append(b.log, 6, 0, 0) }

type testCase struct {
	seed                   uint32
	d, a                   [8]uint32
	pc, usp, ssp, sr       uint32
	irq, ack, flags, steps uint32
	words                  []uint32
}

type rng struct{ s uint64 }

func (r *rng) next() uint32 {
	r.s ^= r.s << 13
	r.s ^= r.s >> 7
	r.s ^= r.s << 17
	return uint32(r.s >> 16)
}
func (r *rng) n(k int) int { return int(r.next() % uint32(k)) }

// interesting register values: small, near the sign bits, and random.
func (r *rng) val() uint32 {
	switch r.n(8) {
	case 0:
		return uint32(r.n(16))
	case 1:
		return 0x7fff + uint32(r.n(3)) - 1
	case 2:
		return 0x80000000 - uint32(r.n(3)) + 1
	case 3:
		return 0xffffffff - uint32(r.n(16))
	case 4:
		return r.next() & 0xffff
	case 5:
		return r.next() & 0xff
	}
	return r.next()
}

func makeCases(seed uint64, perOp int) []testCase {
	r := &rng{s: seed | 1}
	var out []testCase
	for op := 0; op < 65536; op++ {
		for k := 0; k < perOp; k++ {
			tc := testCase{seed: r.next()}
			for i := range tc.d {
				tc.d[i] = r.val()
				tc.a[i] = r.next()
				if r.n(2) == 0 {
					tc.a[i] &= 0xfffffe
				}
			}
			tc.pc = r.next() & 0xfffffe
			if r.n(8) == 0 {
				tc.pc |= 0xff000000 & r.next()
			}
			tc.usp, tc.ssp = r.next()&0xfffffe, r.next()&0xfffffe
			tc.sr = r.next() & 0xa71f
			if r.n(3) == 0 {
				tc.sr |= 0x2000
			}
			if r.n(6) == 0 {
				tc.irq = uint32(r.n(8))
			}
			if r.n(3) == 0 {
				tc.ack = uint32(r.n(256))
			}
			if r.n(10) == 0 {
				tc.flags |= 1
			}
			if r.n(4) == 0 {
				tc.flags |= 2
			}
			if r.n(2) == 0 {
				tc.flags |= 4
			}
			tc.steps = 1
			if r.n(4) == 0 {
				tc.steps += uint32(r.n(3))
			}
			tc.words = []uint32{uint32(op)}
			for i := 0; i < 5; i++ {
				w := r.next() & 0xffff
				if r.n(3) == 0 {
					w &= 0x8fff // brief extension words with small displacements
				}
				tc.words = append(tc.words, w)
			}
			out = append(out, tc)
		}
	}
	return out
}

func (tc *testCase) encode(b []byte) []byte {
	put := func(v uint32) { b = binary.LittleEndian.AppendUint32(b, v) }
	put(tc.seed)
	for _, v := range tc.d {
		put(v)
	}
	for _, v := range tc.a {
		put(v)
	}
	for _, v := range []uint32{tc.pc, tc.usp, tc.ssp, tc.sr, tc.irq, tc.ack, tc.flags, tc.steps, uint32(len(tc.words))} {
		put(v)
	}
	for _, w := range tc.words {
		put(w)
	}
	return b
}

// run runs a case in the Go processor and returns its result in the JS
// harness's layout.
func (tc *testCase) run() []uint32 {
	bus := &testBus{seed: tc.seed, mem: map[uint32]uint8{}, ackVec: int(tc.ack)}
	for i, w := range tc.words {
		ad := (tc.pc + uint32(i)*2) & 0xffffff
		bus.mem[ad] = uint8(w >> 8)
		bus.mem[(ad+1)&0xffffff] = uint8(w)
	}
	c := New(bus)
	c.D, c.A, c.PC, c.USP, c.SSP = tc.d, tc.a, tc.pc, tc.usp, tc.ssp
	c.s, c.t, c.ipl = (tc.sr>>13)&1, (tc.sr>>15)&1, (tc.sr>>8)&7
	c.setCCR(tc.sr & 0x1f)
	c.Stopped, c.nmiSeen, c.TASWrites = tc.flags&1 != 0, tc.flags&2 != 0, tc.flags&4 != 0
	c.irq = int(tc.irq)
	for i := uint32(0); i < tc.steps; i++ {
		c.Step()
	}
	out := []uint32{uint32(c.Cycles)}
	out = append(out, c.D[:]...)
	out = append(out, c.A[:]...)
	fl := uint32(0)
	if c.Stopped {
		fl |= 1
	}
	if c.nmiSeen {
		fl |= 2
	}
	out = append(out, c.PC, c.USP, c.SSP, c.SR(), fl, uint32(c.irq), uint32(len(bus.log)/3))
	return append(out, bus.log...)
}

var resultNames = []string{"cycles", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "PC", "USP", "SSP", "SR", "flags", "irq", "accesses"}

func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	perOp := 4
	if testing.Short() {
		perOp = 1
	}
	if s := os.Getenv("RP_M68K_PEROP"); s != "" {
		perOp, _ = strconv.Atoi(s)
	}
	cases := makeCases(0x68000, perOp)
	var buf []byte
	for i := range cases {
		buf = cases[i].encode(buf)
	}
	file := filepath.Join(t.TempDir(), "cases.bin")
	if err := os.WriteFile(file, buf, 0o644); err != nil {
		t.Fatal(err)
	}
	emu, _ := filepath.Abs("../../web/emu")
	cmd := exec.Command(node, "testdata/m68kref.js", emu, file)
	cmd.Stderr = os.Stderr
	stdout, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Wait()
	defer stdout.Close() // (first: lets node exit if the test stops early)
	r := bufio.NewReaderSize(stdout, 1<<20)
	u32 := func() (uint32, error) {
		var b [4]byte
		_, err := io.ReadFull(r, b[:])
		return binary.LittleEndian.Uint32(b[:]), err
	}
	fails := 0
	for i := range cases {
		js := make([]uint32, 24)
		for k := range js {
			if js[k], err = u32(); err != nil {
				t.Fatalf("case %d: reading JS output: %v", i, err)
			}
		}
		for k := 0; k < int(js[23])*3; k++ {
			v, err := u32()
			if err != nil {
				t.Fatalf("case %d: reading JS output: %v", i, err)
			}
			js = append(js, v)
		}
		gor := cases[i].run()
		if diff := compareResult(gor, js); diff != "" {
			fails++
			tc := &cases[i]
			t.Errorf("opcode %04x (case %d, pc %06x sr %04x irq %d steps %d words %04x): %s", tc.words[0], i, tc.pc, tc.sr, tc.irq, tc.steps, tc.words, diff)
			if fails > 20 {
				t.FailNow()
			}
		}
	}
	t.Logf("%d cases (%d per opcode) identical", len(cases), perOp)
}

func compareResult(gor, js []uint32) string {
	for k := 0; k < 24; k++ {
		if gor[k] != js[k] {
			return fmt.Sprintf("%s: go %08x js %08x", resultNames[k], gor[k], js[k])
		}
	}
	for k := 24; k < len(js); k += 3 {
		if k+2 >= len(gor) || gor[k] != js[k] || gor[k+1] != js[k+1] || gor[k+2] != js[k+2] {
			if k+2 >= len(gor) {
				return fmt.Sprintf("access %d: go none, js %v", (k-24)/3, js[k:k+3])
			}
			return fmt.Sprintf("access %d: go %v js %v", (k-24)/3, gor[k:k+3], js[k:k+3])
		}
	}
	return ""
}
