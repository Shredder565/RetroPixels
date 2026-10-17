package ym2203

import "testing"

func reg(y *Chip, r, v uint8) {
	y.Write(0, r)
	y.Write(1, v)
}

// sine sets up channel 0 as one sine-wave operator (S4) at fnum / block.
func sine(y *Chip, fnum int, block uint8) {
	reg(y, 0xb0, 7) // algorithm 7: all operators heard
	for _, r := range []uint8{0x40, 0x44, 0x48} {
		reg(y, r, 0x7f) // S1-S3 silent
	}
	reg(y, 0x4c, 0) // S4 loud
	for s := uint8(0); s < 4; s++ {
		reg(y, 0x30+s*4, 1)    // multiple 1
		reg(y, 0x50+s*4, 0x1f) // fastest attack
		reg(y, 0x80+s*4, 0x0f)
	}
	reg(y, 0xa4, block<<3|uint8(fnum>>8))
	reg(y, 0xa0, uint8(fnum))
	reg(y, 0x28, 0xf0) // key on, channel 0
}

func crossings(y *Chip, samples, per int) int {
	n, last := 0, 0.0
	for i := 0; i < samples; i++ {
		y.Run(per)
		v, _ := y.Take()
		if last <= 0 && v > 0 {
			n++
		}
		last = v
	}
	return n
}

func TestFMFrequency(t *testing.T) {
	y := New()
	sine(y, 0x400, 4) // a cycle every 128 samples
	if n := crossings(y, 128*100, 72); n < 99 || n > 101 {
		t.Fatalf("%d cycles in 100 periods", n)
	}
	// prescaler 0x2e: samples every 36 clocks, same pitch per sample
	y.Write(0, 0x2d)
	y.Write(0, 0x2e)
	if y.FMRate(1500000) != 1500000/36.0 {
		t.Fatalf("FM rate %v", y.FMRate(1500000))
	}
	if n := crossings(y, 128*100, 36); n < 99 || n > 101 {
		t.Fatalf("after 0x2e: %d cycles in 100 periods", n)
	}
}

func TestKeyOffDecays(t *testing.T) {
	y := New()
	sine(y, 0x300, 4)
	reg(y, 0x8c, 0x0f) // fastest release
	crossings(y, 1000, 72)
	reg(y, 0x28, 0x00)
	crossings(y, 2000, 72)
	for i := 0; i < 100; i++ {
		y.Run(72)
		if v, _ := y.Take(); v != 0 {
			t.Fatal("still sounding after key off")
		}
	}
}

func TestTimers(t *testing.T) {
	y := New()
	reg(y, 0x24, 0xff)
	reg(y, 0x25, 0x03) // timer A: 1 sample
	reg(y, 0x27, 0x05) // load and flag A
	y.Run(72)
	if !y.IRQ() || y.Read(0)&1 == 0 {
		t.Fatal("timer A didn't fire")
	}
	reg(y, 0x27, 0x15) // reset flag A
	if y.Read(0)&1 != 0 {
		t.Fatal("flag A not cleared")
	}
	reg(y, 0x26, 0xfe) // timer B: 2 x 16 samples
	reg(y, 0x27, 0x0a) // load and flag B
	y.Run(72 * 31)
	if y.Read(0)&2 != 0 {
		t.Fatal("timer B early")
	}
	y.Run(72)
	if y.Read(0)&2 == 0 {
		t.Fatal("timer B didn't fire")
	}
}

func TestSSG(t *testing.T) {
	y := New()
	reg(y, 0x07, 0x3e)
	reg(y, 0x08, 0x0f)
	reg(y, 0x00, 50)
	y.Write(0, 0x00)
	if y.Read(1) != 50 {
		t.Fatal("SSG register read")
	}
	// clock / 4 into the SSG, a step every 4 of those: a tone cycle every
	// 50 * 2 * 4 * 4 = 1600 clocks
	n, last := 0, 0.0
	for i := 0; i < 1600*40/32; i++ {
		y.Run(32)
		_, v := y.Take()
		if last == 0 && v > 0 {
			n++
		}
		last = v
	}
	if n < 39 || n > 41 {
		t.Fatalf("%d SSG cycles in 40 periods", n)
	}
}

func TestState(t *testing.T) {
	y := New()
	sine(y, 0x2a0, 3)
	reg(y, 0x07, 0x38)
	reg(y, 0x08, 0x0c)
	reg(y, 0x02, 33)
	y.Run(72 * 500)
	y.Take()
	s := y.AppendState(nil)
	z := New()
	rest, err := z.LoadState(append(s, 1, 2))
	if err != nil || len(rest) != 2 {
		t.Fatal(err)
	}
	for i := 0; i < 500; i++ {
		y.Run(72)
		z.Run(72)
		a, b := y.Take()
		c, d := z.Take()
		if a != c || b != d {
			t.Fatal("outputs differ after loading")
		}
	}
	if _, err := z.LoadState(s[:len(s)-3]); err == nil {
		t.Fatal("short state accepted")
	}
}
