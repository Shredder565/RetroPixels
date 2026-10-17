package pokey

import "testing"

func TestToneAndState(t *testing.T) {
	p := New()
	p.Write(0, 20)   // AUDF1
	p.Write(1, 0xaf) // pure tone, volume 15
	var a []float64
	for i := 0; i < 100; i++ {
		a = append(a, p.Run(32))
	}
	lo, hi := 1.0, 0.0
	for _, v := range a {
		lo, hi = min(lo, v), max(hi, v)
	}
	if hi-lo < 0.2 {
		t.Fatalf("no square wave: %v..%v", lo, hi)
	}
	for _, hold := range []bool{false, true} {
		p.HoldOnReset = hold
		st := p.AppendState(nil)
		q := New()
		q.HoldOnReset = hold
		rest, err := q.LoadState(append(st, 1, 2))
		if err != nil || len(rest) != 2 {
			t.Fatalf("load: %v, %d left", err, len(rest))
		}
		for i := 0; i < 50; i++ {
			if x, y := p.Run(32), q.Run(32); x != y {
				t.Fatalf("after loading: %v, want %v", y, x)
			}
		}
		if _, err := q.LoadState(st[:len(st)-1]); err == nil {
			t.Fatal("a cut-off state loaded")
		}
	}
}

func TestRandomAndPots(t *testing.T) {
	p := New()
	if p.Read(10, 100) == p.Read(10, 101) && p.Read(10, 102) == p.Read(10, 103) {
		t.Error("RANDOM doesn't change")
	}
	p.HoldOnReset = true
	p.Write(15, 0) // SKCTL: reset
	if p.Read(10, 100) != p.Read(10, 5000) {
		t.Error("RANDOM runs while the chip is held")
	}
	p.Write(15, 3)
	if p.Read(10, 100) == p.Read(10, 101) && p.Read(10, 102) == p.Read(10, 103) {
		t.Error("RANDOM doesn't run again")
	}
	if p.Read(8, 0) != 0xff {
		t.Error("ALLPOT should read $FF when unused")
	}
	p.Pot[8] = 0x5a
	if p.Read(8, 0) != 0x5a {
		t.Error("ALLPOT")
	}
}
