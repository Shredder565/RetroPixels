package ay8910

import (
	"math"
	"testing"
)

// edges counts rising edges of the output over n steps of 8 clocks.
func edges(p *PSG, steps int) int {
	n, last := 0, 0.0
	for i := 0; i < steps; i++ {
		p.Run(8)
		v := p.Take()
		if last == 0 && v > 0 {
			n++
		}
		last = v
	}
	return n
}

func TestToneFrequency(t *testing.T) {
	p := New(false)
	p.Write(7, 0x3e) // tone A only
	p.Write(8, 15)
	p.Write(0, 100) // period 100: a cycle every 1600 clocks = 200 steps
	if n := edges(p, 200*50); n < 49 || n > 51 {
		t.Fatalf("got %d cycles in 50 periods", n)
	}
	// the YM SSG steps at the same rate from the same clock
	y := New(true)
	y.Write(7, 0x3e)
	y.Write(8, 15)
	y.Write(0, 100)
	if n := edges(y, 200*50); n < 49 || n > 51 {
		t.Fatalf("YM: got %d cycles in 50 periods", n)
	}
}

func TestVolumeAndMixer(t *testing.T) {
	p := New(false)
	p.Write(7, 0x3f) // all off: outputs are high (the channel shows its volume)
	p.Write(8, 15)
	p.Write(9, 0)
	p.Write(10, 7)
	p.Run(8)
	if v := p.Take(); math.Abs(v-(1+ayLevels[7])) > 1e-9 {
		t.Fatalf("levels: %v", v)
	}
	if p.Read(1) != 0 || p.Read(13) != 0 {
		t.Fatal("unused registers should read 0")
	}
	p.Write(1, 0xff)
	if p.Read(1) != 0x0f {
		t.Fatalf("register 1 reads %x", p.Read(1))
	}
}

// envelope shapes: the level after the first ramp and after the second.
func TestEnvelopeShapes(t *testing.T) {
	want := map[uint8][2]int{
		0x0: {0, 0}, 0x4: {0, 0}, 0x8: {15, 15}, 0x9: {0, 0}, 0xa: {0, 15}, 0xb: {15, 15},
		0xc: {0, 0}, 0xd: {15, 15}, 0xe: {15, 0}, 0xf: {0, 0},
	}
	for shape, w := range want {
		p := New(false)
		p.Write(11, 1) // a level every 16 clocks
		p.Write(13, shape)
		p.Run(16 * 16) // one whole ramp
		a := p.envLevel()
		p.Run(16 * 16)
		b := p.envLevel()
		if [2]int{a, b} != w {
			t.Errorf("shape %x: levels %d, %d; want %v", shape, a, b, w)
		}
	}
}

func TestNoiseIsNotSilent(t *testing.T) {
	p := New(false)
	p.Write(7, 0x37) // noise on A only
	p.Write(6, 1)
	p.Write(8, 15)
	ones := 0
	for i := 0; i < 10000; i++ {
		p.Run(8)
		if p.Take() > 0 {
			ones++
		}
	}
	if ones < 3000 || ones > 7000 {
		t.Fatalf("noise high %d of 10000 steps", ones)
	}
}

func TestPorts(t *testing.T) {
	p := New(false)
	var got uint8
	p.PortIn = func(port int) uint8 { return 0x40 + uint8(port) }
	p.PortOut = func(port int, v uint8) { got = v }
	if p.Read(14) != 0x40 || p.Read(15) != 0x41 {
		t.Fatal("input ports")
	}
	p.Write(7, 0x40)
	p.Write(14, 0x99)
	if got != 0x99 || p.Read(14) != 0x99 {
		t.Fatal("output port")
	}
}

func TestState(t *testing.T) {
	p := New(true)
	for r, v := range []uint8{30, 1, 77, 0, 200, 2, 9, 0x30, 0x10, 12, 13, 50, 0, 0xe} {
		p.Write(r, v)
	}
	p.Run(12345)
	p.Take()
	s := p.AppendState(nil)
	if len(s) != StateSize {
		t.Fatalf("state is %d bytes, want %d", len(s), StateSize)
	}
	q := New(true)
	rest, err := q.LoadState(append(s, 7))
	if err != nil || len(rest) != 1 {
		t.Fatal(err)
	}
	for i := 0; i < 1000; i++ {
		p.Run(8)
		q.Run(8)
		if p.Take() != q.Take() {
			t.Fatal("outputs differ after loading")
		}
	}
	if _, err := q.LoadState(s[:10]); err == nil {
		t.Fatal("short state accepted")
	}
}
