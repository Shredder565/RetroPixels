package konami2

import (
	"encoding/binary"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
)

type ram struct{ m [65536]uint8 }

func (r *ram) Read(a uint16) uint8     { return r.m[a] }
func (r *ram) Write(a uint16, v uint8) { r.m[a] = v }

type ackBus struct {
	*ram
	cpu *CPU
}

func (b *ackBus) Ack(kind int) {
	switch kind {
	case AckIRQ:
		b.cpu.IRQ = false
	case AckFIRQ:
		b.cpu.FIRQ = false
	}
}

type xorshift struct{ s uint64 }

func (x *xorshift) next() uint32 {
	x.s ^= x.s << 13
	x.s ^= x.s >> 7
	x.s ^= x.s << 17
	return uint32(x.s)
}

// TestMatchesJS runs the Go and JS processors on the same pseudo-random
// memory (random programs, with random interrupts) and compares every
// register after every instruction, and the memory now and then.
func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	emu, _ := filepath.Abs("../../web/emu")
	steps := 200000
	if testing.Short() {
		steps = 20000
	}
	for seed := uint64(1); seed <= 12; seed++ {
		out, err := exec.Command(node, "testdata/jsref.js", emu, strconv.FormatUint(seed*0x9e3779b9, 10), strconv.Itoa(steps)).Output()
		if err != nil {
			t.Fatal(err)
		}
		rng := &xorshift{s: seed*0x9e3779b9 | 1}
		r := &ram{}
		for i := range r.m {
			r.m[i] = uint8(rng.next() >> 24)
		}
		b := &ackBus{ram: r}
		c := New(b)
		b.cpu = c
		c.Reset()
		for i := 0; i < steps; i++ {
			x := rng.next()
			if x&0xff == 1 {
				c.IRQ = true
			}
			if x&0xff == 2 {
				c.FIRQ = true
			}
			if x&0xfff == 3 {
				c.NMI()
			}
			cyc := c.Step()
			sum := 0
			if i&4095 == 4095 {
				for k := range r.m {
					sum = (sum*31 + int(r.m[k])) & 0xffff
				}
			}
			got := []int{c.A, c.B, c.X, c.Y, c.U, c.S, c.PC, c.DP, c.CC, cyc, c.Lines, sum}
			for k, v := range got {
				if w := int(binary.LittleEndian.Uint16(out[i*24+k*2:])); w != v&0xffff {
					t.Fatalf("seed %d step %d: register %d is %#x, JS %#x (go %x)", seed, i, k, v, w, got)
				}
			}
		}
	}
}

func TestStateRoundTrip(t *testing.T) {
	r := &ram{}
	c := New(r)
	c.A, c.B, c.X, c.Y, c.U, c.S, c.PC, c.DP, c.CC, c.IRQ, c.Lines = 1, 2, 0x1234, 0x5678, 0x9abc, 0xdef0, 0x8000, 0x12, 0xd5, true, 0x3f
	c.NMI()
	st := c.AppendState(nil)
	d := New(r)
	if rest, err := d.LoadState(st); err != nil || len(rest) != 0 {
		t.Fatal(err)
	}
	if string(d.AppendState(nil)) != string(st) {
		t.Fatal("state differs after loading")
	}
}
