package sn76489

import "testing"

func TestToneNoiseAndState(t *testing.T) {
	p := New()
	for i := 0; i < 100; i++ {
		if p.Step() != 0 {
			t.Fatal("a reset chip makes sound")
		}
	}
	p.Write(0x80 | 0x05) // tone 0 period low bits
	p.Write(0x01)        // ...high bits: period 0x15
	p.Write(0x90)        // tone 0 full volume
	p.Write(0xe4)        // white noise
	p.Write(0xf8)        // noise volume
	pos, neg := 0, 0
	for i := 0; i < 1000; i++ {
		if v := p.Step(); v > 0 {
			pos++
		} else if v < 0 {
			neg++
		}
	}
	if pos < 100 || neg < 100 {
		t.Fatalf("no square wave: %d up, %d down", pos, neg)
	}
	s := p.AppendState(nil)
	if len(s) != StateSize {
		t.Fatalf("state %d bytes", len(s))
	}
	q := New()
	if rest, err := q.LoadState(append(s, 9)); err != nil || len(rest) != 1 {
		t.Fatal(err)
	}
	for i := 0; i < 5000; i++ {
		if a, b := p.Step(), q.Step(); a != b {
			t.Fatalf("step %d: %v vs %v after loading the state", i, a, b)
		}
	}
	if _, err := q.LoadState(s[:10]); err == nil {
		t.Fatal("short state accepted")
	}
}
