package ym3812

import (
	"bytes"
	"math"
	"testing"
)

func wr(c *Chip, r, v uint8) {
	c.Write(0, r)
	c.Write(1, v)
}

// a plain sine on channel 1 (carrier only, modulator silent)
func sineVoice(c *Chip, fnum uint16, block uint8) {
	wr(c, 0x20, 0x01) // modulator: mult 1
	wr(c, 0x40, 0x3f) // modulator silent
	wr(c, 0x23, 0x21) // carrier: sustained, mult 1
	wr(c, 0x43, 0x00)
	wr(c, 0x63, 0xf0) // instant attack
	wr(c, 0x83, 0x00)
	wr(c, 0xa0, uint8(fnum))
	wr(c, 0xb0, 0x20|block<<2|uint8(fnum>>8))
}

// TestPitch: F-number 0x241, block 4 at 3.579545 MHz is 440 Hz.
func TestPitch(t *testing.T) {
	c := New()
	sineVoice(c, 0x241, 4)
	rate := Rate(3579545)
	n := int(rate) // one second
	var prev float64
	cross := 0
	peak := 0.0
	for i := 0; i < n; i++ {
		c.Run(72)
		v := c.Take()
		if prev < 0 && v >= 0 {
			cross++
		}
		prev = v
		peak = math.Max(peak, math.Abs(v))
	}
	if cross < 436 || cross > 444 {
		t.Errorf("%d cycles a second, want 440", cross)
	}
	if peak < 0.9 || peak > 1.01 {
		t.Errorf("peak %.3f, want about 1", peak)
	}
}

// TestRelease: key off makes the note die away.
func TestRelease(t *testing.T) {
	c := New()
	sineVoice(c, 0x241, 4)
	wr(c, 0x83, 0x05) // release rate 5
	c.Run(72 * 1000)
	wr(c, 0xb0, 0x11) // key off
	c.Run(72 * 100000)
	c.Take()
	c.Run(72 * 100)
	if v := c.Take(); math.Abs(v) > 0.001 {
		t.Errorf("still sounding after release: %f", v)
	}
}

// TestTimer: timer 1 overflows after (256-T1)*4 samples and raises the
// interrupt, which register 4 bit 7 clears.
func TestTimer(t *testing.T) {
	c := New()
	wr(c, 0x02, 256-10)
	wr(c, 0x04, 0x01)
	c.Run(72 * 39)
	if c.IRQ() {
		t.Fatal("IRQ too early")
	}
	c.Run(72 * 2)
	if !c.IRQ() || c.Read(0)&0xc0 != 0xc0 {
		t.Fatalf("no IRQ: status %02x", c.Read(0))
	}
	wr(c, 0x04, 0x80)
	if c.IRQ() {
		t.Fatal("IRQ not cleared")
	}
	wr(c, 0x04, 0x41) // masked
	c.Run(72 * 100)
	if c.IRQ() {
		t.Fatal("masked timer raised the IRQ")
	}
}

func TestState(t *testing.T) {
	c := New()
	sineVoice(c, 0x123, 3)
	wr(c, 0xbd, 0x3f)
	c.Run(72 * 500)
	s := c.AppendState(nil)
	if len(s) != StateSize {
		t.Fatalf("state %d bytes, want %d", len(s), StateSize)
	}
	d := New()
	rest, err := d.LoadState(append(s, 9))
	if err != nil || len(rest) != 1 {
		t.Fatal(err, len(rest))
	}
	c.Run(72 * 300)
	d.Run(72 * 300)
	if a, b := c.AppendState(nil), d.AppendState(nil); !bytes.Equal(a, b) {
		t.Fatal("states differ after running the same")
	}
	if _, err := d.LoadState(s[:10]); err == nil {
		t.Fatal("short state accepted")
	}
}
