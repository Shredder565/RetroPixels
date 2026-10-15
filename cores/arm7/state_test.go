package arm7

import (
	"bytes"
	"testing"
)

// TestStateRoundTrip: a processor restored from a saved state runs on
// exactly like the original.
func TestStateRoundTrip(t *testing.T) {
	for run := 0; run < 6; run++ {
		thumb := run%2 == 1
		mem := program(uint64(run)*0x2545f4914f6cdd1d+3, thumb)
		bus := &flatBus{mem: append([]byte(nil), mem...)}
		c := New(bus)
		c.Reset()
		c.R[13] = 0x8000
		if thumb {
			c.T = 1
		}
		c.Branch(0x100)
		for i := 0; i < 5000; i++ {
			c.Step()
		}
		st := c.SaveState(nil)
		if len(st) != StateSize {
			t.Fatalf("state is %d bytes, want %d", len(st), StateSize)
		}
		bus2 := &flatBus{mem: append([]byte(nil), bus.mem...)}
		d := New(bus2)
		if err := d.LoadState(st); err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 5000; i++ {
			c.Step()
			d.Step()
			if c.R != d.R || c.CPSR() != d.CPSR() || c.PC != d.PC || c.Cycles != d.Cycles {
				t.Fatalf("run %d step %d: restored processor differs", run, i)
			}
		}
		if !bytes.Equal(bus.mem, bus2.mem) {
			t.Fatalf("run %d: memory differs", run)
		}
		if err := d.LoadState(st[:10]); err == nil {
			t.Fatal("short state accepted")
		}
	}
}
