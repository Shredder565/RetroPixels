package cpu6502

import (
	"bufio"
	"bytes"
	"fmt"
	"os/exec"
	"path/filepath"
	"testing"
)

type traceBus struct {
	mem [65536]uint8
	h   uint32
	cpu *CPU
	mp  bool
}

func (b *traceBus) mix(x uint32) { b.h = (b.h ^ x) * 16777619 }
func (b *traceBus) Read(a uint16) uint8 {
	b.mix(uint32(a))
	b.mix(uint32(b.mem[a]))
	if b.mp {
		b.cpu.Poll()
	}
	return b.mem[a]
}
func (b *traceBus) Write(a uint16, v uint8) {
	b.mix(uint32(a) | 0x10000)
	b.mix(uint32(v))
	if b.mp {
		b.cpu.Poll()
	}
	b.mem[a] = v
}

func goTrace(seed uint32, steps int, decimal, mp bool) []string {
	if seed == 0 {
		seed = 1
	}
	rnd := func() uint32 { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed }
	b := &traceBus{mp: mp}
	for i := range b.mem {
		b.mem[i] = uint8(rnd())
	}
	c := New(b, Options{Decimal: decimal, MachinePolls: mp})
	b.cpu = c
	c.Reset()
	out := make([]string, 0, steps)
	for i := 0; i < steps; i++ {
		r := rnd()
		if r&63 == 0 {
			c.NMIPending = true
		}
		if (r>>8)&15 == 0 {
			c.IRQLine = !c.IRQLine
		}
		if c.Jammed && (r>>12)&7 == 0 {
			c.Reset()
		}
		b.h = 2166136261
		n := c.Step()
		out = append(out, fmt.Sprintf("%d %d %d %d %d %d %d %d %d", c.A, c.X, c.Y, c.S, c.P, c.PC, c.Cycles, n, b.h))
	}
	return out
}

// TestMatchesJS runs the JavaScript CPU (web/emu/cpu6502.js) in Node and this
// port on the same pseudo-random programs, comparing every bus access.
func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	js, _ := filepath.Abs("../../web/emu/cpu6502.js")
	const steps = 200000
	for _, tc := range []struct {
		seed        uint32
		decimal, mp bool
	}{{1, false, false}, {2, true, false}, {3, false, true}, {4, true, true}, {5, false, true}, {6, true, false}} {
		cmd := exec.Command(node, "testdata/trace.js", js, fmt.Sprint(tc.seed), fmt.Sprint(steps), b01(tc.decimal), b01(tc.mp))
		outJS, err := cmd.Output()
		if err != nil {
			t.Fatalf("node: %v", err)
		}
		want := goTrace(tc.seed, steps, tc.decimal, tc.mp)
		sc := bufio.NewScanner(bytes.NewReader(outJS))
		i := 0
		for sc.Scan() {
			if i >= len(want) || sc.Text() != want[i] {
				g := ""
				if i < len(want) {
					g = want[i]
				}
				t.Fatalf("seed %d step %d: js %q go %q (prev %q)", tc.seed, i, sc.Text(), g, want[max(i-1, 0)])
			}
			i++
		}
		if i != steps {
			t.Fatalf("seed %d: js gave %d steps", tc.seed, i)
		}
	}
}

func b01(b bool) string {
	if b {
		return "1"
	}
	return "0"
}

func TestStateRoundTrip(t *testing.T) {
	b := &traceBus{}
	c := New(b, Options{Decimal: true})
	b.cpu = c
	c.Reset()
	for i := 0; i < 1000; i++ {
		c.Step()
	}
	s := c.SaveState()
	d := New(b, Options{Decimal: true})
	if err := d.LoadState(s); err != nil {
		t.Fatal(err)
	}
	d.bus = b
	if !bytes.Equal(d.SaveState(), s) || d.PC != c.PC || d.Cycles != c.Cycles {
		t.Fatal("state differs after round trip")
	}
}
