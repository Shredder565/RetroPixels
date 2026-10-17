package msm5205

import (
	"bytes"
	"math"
	"testing"
)

func TestDecode(t *testing.T) {
	c := New(384000, 48, 4)
	if c.Rate() != 8000 {
		t.Fatalf("rate %v", c.Rate())
	}
	// code 7 at step 0: 16/8 + 16/4 + 16/2 + 16 = 30, step +8
	c.Write(7)
	if s := c.Clock(); s != 30 || c.step != 8 {
		t.Fatalf("first: %d step %d", s, c.step)
	}
	// code 8 (negative zero magnitude) at step 8 (34): -4, step -1
	c.Write(8)
	if s := c.Clock(); s != 26 || c.step != 7 {
		t.Fatalf("second: %d step %d", s, c.step)
	}
	// saturates at the top
	for i := 0; i < 200; i++ {
		c.Write(7)
		c.Clock()
	}
	if c.Output() != 2047 || c.step != 48 {
		t.Errorf("saturation %d %d", c.Output(), c.step)
	}
	c.SetReset(true)
	if c.Clock() != 0 || c.step != 0 {
		t.Errorf("reset")
	}
}

// Encoding a sine with the chip's own rules and decoding it gives the sine
// back.
func TestRoundTrip(t *testing.T) {
	enc := New(384000, 48, 4)
	dec := New(384000, 48, 4)
	worst := 0.0
	for i := 0; i < 4000; i++ {
		want := int(1000 * math.Sin(float64(i)*2*math.Pi/40))
		st := steps[enc.step]
		diff := want - enc.signal
		code := 0
		if diff < 0 {
			code = 8
			diff = -diff
		}
		m := diff * 4 / st
		if m > 7 {
			m = 7
		}
		code |= m
		enc.Write(uint8(code))
		enc.Clock()
		dec.Write(uint8(code))
		got := dec.Clock()
		if i > 100 {
			worst = math.Max(worst, math.Abs(float64(got-want)))
		}
	}
	if worst > 150 {
		t.Errorf("round trip off by %v", worst)
	}
}

func TestThreeBit(t *testing.T) {
	c := New(384000, 64, 3)
	c.Write(3) // 16/4 + 16/2 + 16 = 28, step +4
	if s := c.Clock(); s != 28 || c.step != 4 {
		t.Errorf("3-bit %d %d", s, c.step)
	}
}

func TestState(t *testing.T) {
	c := New(384000, 48, 4)
	c.Write(5)
	c.Clock()
	c.Write(13)
	c.Clock()
	b := c.AppendState(nil)
	if len(b) != StateSize {
		t.Fatalf("size %d", len(b))
	}
	d := New(1, 96, 4)
	if _, err := d.LoadState(b); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(d.AppendState(nil), b) {
		t.Error("state did not round-trip")
	}
}
