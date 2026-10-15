package atari2600

import "testing"

func TestSharedSoundAndRIOT(t *testing.T) {
	s := NewTIASound(48000, 7159090.0/228)
	s.Write(0, 4)
	s.Write(2, 3)
	s.Write(4, 15)
	for i := 0; i < 500; i++ {
		s.Tick()
		s.AddToLast(0.25)
	}
	st := s.AppendState(nil)
	a := s.Take()
	if len(a) < 700 || len(a) > 800 {
		t.Fatalf("%d samples", len(a))
	}
	n := NewTIASound(48000, 7159090.0/228)
	if rest, err := n.LoadState(append(st, 9)); err != nil || len(rest) != 1 {
		t.Fatalf("sound state: %v %v", rest, err)
	}
	if string(n.AppendState(nil)) != string(st) {
		t.Fatal("sound state round trip")
	}
	if _, err := n.LoadState(st[:5]); err == nil {
		t.Fatal("short sound state accepted")
	}

	r := NewRIOT()
	r.SetPorts(0x5a, 0x0b)
	if r.Read(0x280) != 0x5a || r.Read(0x282) != 0x0b {
		t.Fatal("ports")
	}
	r.Write(0x294|0x10, 3) // TIM1T
	r.Tick()
	r.RAM()[5] = 77
	rs := r.AppendState(nil)
	q := NewRIOT()
	if _, err := q.LoadState(rs); err != nil || q.RAM()[5] != 77 || q.Read(0x284) != r.Read(0x284) {
		t.Fatal("RIOT state")
	}
}
