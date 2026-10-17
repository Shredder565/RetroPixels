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

func TestMAMESync(t *testing.T) {
	window := func(t int64) uint8 { // the generator t clocks after leaving reset
		var v uint8
		for i := int64(0); i < 8; i++ {
			v |= p17[(t+1+i)%131071] << i
		}
		return v
	}
	p := New()
	p.HoldOnReset = true
	p.SyncEvery = [2]int64{1000, 0}
	p.WriteAt(15, 0, 50)
	p.WriteAt(15, 3, 100) // leaves reset at 100
	// reads give the generator as of the last sync point: the last access
	// (or a multiple of 1000 clocks, if later)
	if v := p.Read(10, 300); v != window(0) {
		t.Errorf("first read %02x, want %02x", v, window(0))
	}
	if v := p.Read(10, 350); v != window(200) {
		t.Errorf("second read %02x, want %02x", v, window(200))
	}
	if v := p.Read(10, 1200); v != window(900) {
		t.Errorf("after a tick %02x, want %02x", v, window(900))
	}
	p.Sync(1300)
	if v := p.Read(10, 1400); v != window(1200) {
		t.Errorf("after Sync %02x, want %02x", v, window(1200))
	}
	q := New()
	q.HoldOnReset = true
	if _, err := q.LoadState(p.AppendState(nil)); err != nil {
		t.Fatal(err)
	}
	q.SyncEvery = p.SyncEvery
	if x, y := p.Read(10, 1500), q.Read(10, 1500); x != y {
		t.Errorf("after loading %02x, want %02x", y, x)
	}
}
