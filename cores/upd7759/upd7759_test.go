package upd7759

import (
	"testing"

	"retropixels/cores/arcade"
)

// blocks is a sample's block data: a 256-nibble block, a counted block of
// 20 nibbles, a short silence and the end.
func blocks() []byte {
	b := []byte{0x40}
	for i := 0; i < 128; i++ {
		b = append(b, byte(i*37+11))
	}
	b = append(b, 0x81, 19)
	for i := 0; i < 10; i++ {
		b = append(b, byte(0x9a+i*5))
	}
	return append(b, 0x02, 0x00)
}

// outputs records the output each time it changes, until the chip is idle.
func outputs(t *testing.T, u *UPD7759, step func()) []float64 {
	t.Helper()
	var out []float64
	last := 0.0
	for i := 0; i < 400000; i++ {
		step()
		u.Clocks(4)
		if u.Out != last {
			out = append(out, u.Out)
			last = u.Out
		}
		if i > 10 && !u.Busy() {
			return out
		}
	}
	t.Fatal("sample never ended")
	return nil
}

// TestSlaveMatchesStandalone plays the same sample from a ROM (stand-alone
// mode) and fed byte by byte on DRQ (slave mode) and expects the same output.
func TestSlaveMatchesStandalone(t *testing.T) {
	// ROM: last sample 0, signature, the table (sample 0 at word 4 -> byte 9,
	// after a dummy byte), then the blocks
	rom := []byte{0, 0x5a, 0xa5, 0x69, 0x55, 0x00, 0x04, 0, 0, 0}
	rom = append(rom[:9], blocks()...)
	m := New(0, rom)
	m.Port(0)
	m.Start(true)
	m.Start(false)
	want := outputs(t, m, func() {})
	if len(want) < 100 {
		t.Fatalf("stand-alone mode played only %d changes", len(want))
	}

	// the slave stream: sample number, then the header bytes, then blocks
	stream := append([]byte{0, 0x00, 0x04, 0x00, 0x00}, blocks()...)
	s := NewSlave(0)
	drqs := 0
	s.OnDRQ = func(on bool) {
		if on {
			drqs++
		}
	}
	s.Port(0)
	pos, sinceFed := 0, 0
	got := outputs(t, s, func() {
		sinceFed++
		if s.DRQ() {
			if sinceFed < slaveGap {
				t.Fatalf("DRQ again %d steps after a byte", sinceFed)
			}
			if pos < len(stream) {
				s.Port(stream[pos])
				pos++
			}
			if s.DRQ() {
				t.Fatal("DRQ still up after the byte was written")
			}
			sinceFed = 0
		}
	})
	if pos != len(stream) || drqs != len(stream) {
		t.Errorf("fed %d bytes on %d DRQs, want %d", pos, drqs, len(stream))
	}
	if len(got) != len(want) {
		t.Fatalf("slave mode: %d output changes, stand-alone %d", len(got), len(want))
	}
	for i := range got {
		if got[i] != want[i] {
			t.Fatalf("output change %d: slave %v, stand-alone %v", i, got[i], want[i])
		}
	}
}

// TestSlaveResetAndState: RESET stops a sample and drops DRQ; the state
// round-trips mid-sample.
func TestSlaveResetAndState(t *testing.T) {
	stream := append([]byte{0, 0x00, 0x04, 0x00, 0x00}, blocks()...)
	run := func(u *UPD7759, pos *int, n int) {
		for i := 0; i < n; i++ {
			if u.DRQ() && *pos < len(stream) {
				u.Port(stream[*pos])
				*pos++
			}
			u.Clocks(4)
		}
	}
	a := NewSlave(0)
	a.Port(0)
	pa := 0
	run(a, &pa, 300)
	w := &arcade.StateW{}
	a.AppendState(w)
	b := NewSlave(0)
	b.LoadState(&arcade.StateR{B: w.B})
	pb := pa
	for i := 0; i < 2000; i++ {
		run(a, &pa, 1)
		run(b, &pb, 1)
		if a.Out != b.Out || a.DRQ() != b.DRQ() {
			t.Fatalf("step %d after loading: %v/%v vs %v/%v", i, a.Out, a.DRQ(), b.Out, b.DRQ())
		}
	}
	a.ResetLine(false)
	if a.Busy() || a.DRQ() || a.Out != 0 {
		t.Error("RESET left the chip playing")
	}
	a.Port(0) // ignored while held in reset
	if a.Busy() {
		t.Error("a write during RESET started a sample")
	}
}
