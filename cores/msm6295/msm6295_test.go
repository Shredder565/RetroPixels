package msm6295

import "testing"

// rom makes a sample ROM with phrase 1 at 0x400 (n bytes of b).
func rom(n int, b byte) []byte {
	r := make([]byte, 0x1000)
	start, end := 0x400, 0x400+n-1
	r[8], r[9], r[10] = byte(start>>16), byte(start>>8), byte(start)
	r[11], r[12], r[13] = byte(end>>16), byte(end>>8), byte(end)
	for i := 0; i < n; i++ {
		r[start+i] = b
	}
	return r
}

func TestDecode(t *testing.T) {
	c := New(rom(2, 0x77))
	c.Write(0x81) // phrase 1
	c.Write(0x10) // voice 0, full volume
	if c.Read() != 0xf1 {
		t.Fatalf("status %#x, want voice 0 playing", c.Read())
	}
	// step 16: nibble 7 adds 16*15/8 = 30 and moves 8 steps up (to 34),
	// then 34*15/8 = 63 more
	want := []int{30, 93}
	for i, w := range want {
		if got := c.Sample(); got != w {
			t.Errorf("sample %d = %d, want %d", i, got, w)
		}
	}
	c.Sample()
	c.Sample()
	if c.Read() != 0xf0 {
		t.Errorf("status %#x after the phrase, want 0xf0", c.Read())
	}
	if c.Sample() != 0 {
		t.Error("a stopped voice still plays")
	}
}

func TestVolumeStopAndBusy(t *testing.T) {
	c := New(rom(0x100, 0x77))
	c.Write(0x81)
	c.Write(0x22) // voice 1 at -6 dB (16/32)
	if got := c.Sample(); got != 15 {
		t.Errorf("attenuated sample = %d, want 15", got)
	}
	// a busy voice ignores a new phrase
	c.Write(0x81)
	c.Write(0x20)
	if c.voices[1].vol != 16 {
		t.Error("a busy voice took a new phrase")
	}
	c.Write(0x10) // stop voice 1 (bit 4 = voice 1 for stops)
	if c.Read() != 0xf0 {
		t.Errorf("status %#x after stop", c.Read())
	}
}

func TestState(t *testing.T) {
	c := New(rom(0x100, 0x3b))
	c.Write(0x81)
	c.Write(0x80)
	for i := 0; i < 37; i++ {
		c.Sample()
	}
	s := c.AppendState(nil)
	var want []int
	for i := 0; i < 50; i++ {
		want = append(want, c.Sample())
	}
	d := New(c.rom)
	if rest, err := d.LoadState(s); err != nil || len(rest) != 0 {
		t.Fatal(err, len(rest))
	}
	for i, w := range want {
		if got := d.Sample(); got != w {
			t.Fatalf("sample %d after load = %d, want %d", i, got, w)
		}
	}
	if _, err := d.LoadState(s[:10]); err == nil {
		t.Error("short state loaded")
	}
}
