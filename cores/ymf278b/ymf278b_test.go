package ymf278b

import (
	"math"
	"testing"
)

// The tests build their own wave memory: headers for waves 0-3 at 0, and
// sample data from 0x1000.

func header(format int, start, loop, end uint32, regs [5]uint8) []byte {
	e := ^end & 0xffff
	return []byte{
		uint8(format)<<6 | uint8(start>>16)&0x3f, uint8(start >> 8), uint8(start),
		uint8(loop >> 8), uint8(loop), uint8(e >> 8), uint8(e),
		regs[0], regs[1], regs[2], regs[3], regs[4],
	}
}

// fast envelope: attack 15, no decay, release 15, no LFO
var fastEG = [5]uint8{0x00, 0xf0, 0x00, 0x0f, 0x00}

func newChip(t *testing.T) (*Chip, []byte) {
	t.Helper()
	mem := make([]byte, 0x20000)
	return New(mem), mem
}

func (c *Chip) pcm(r int, v uint8) {
	c.Write(4, uint8(r))
	c.Write(5, v)
}

// play plays wave on channel ch at F-number fn, octave oct, level tl.
func (c *Chip) play(ch, wave, fn, oct, tl, pan int) {
	c.pcm(0x68+ch, 0)
	c.pcm(0x50+ch, uint8(tl<<1|1))
	c.pcm(0x20+ch, uint8(fn&0x7f)<<1|uint8(wave>>8))
	c.pcm(0x38+ch, uint8(oct&15)<<4|uint8(fn>>7))
	c.pcm(0x08+ch, uint8(wave))
	c.pcm(0x68+ch, 0x80|uint8(pan))
}

func run(c *Chip, n int) (l, r []int32) {
	for i := 0; i < n; i++ {
		a, b := c.Sample()
		l, r = append(l, a), append(r, b)
	}
	return
}

func TestRate(t *testing.T) {
	if got := Rate(33868800); got != 44100 {
		t.Fatalf("Rate %v", got)
	}
}

func TestDeviceIDAndMemoryRead(t *testing.T) {
	c, mem := newChip(t)
	if c.Read(5) != 0 {
		t.Fatal("register 0 should read 0 after reset")
	}
	c.Write(4, 2)
	if id := c.Read(5) >> 5 & 7; id != 1 {
		t.Fatalf("device ID %d", id)
	}
	copy(mem[0x12345:], []byte{0xaa, 0xbb, 0xcc})
	c.pcm(3, 0x01)
	c.pcm(4, 0x23)
	c.pcm(5, 0x45)
	c.Write(4, 6)
	if a, b, d := c.Read(5), c.Read(5), c.Read(5); a != 0xaa || b != 0xbb || d != 0xcc {
		t.Fatalf("memory reads %02x %02x %02x", a, b, d)
	}
	if c.Read(0)&0x80 != 0 {
		t.Fatal("status busy")
	}
}

func TestEightBitPlayback(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x1000, 0, 255, fastEG))
	for i := 0; i < 256; i++ {
		mem[0x1000+i] = uint8(i - 128)
	}
	c.play(0, 0, 0, 1, 0, 0)
	l, r := run(c, 300)
	// F-number 0, octave 1: one memory sample per output sample, full level
	for i := 1; i < 200; i++ {
		want := int32(int8(uint8(i-128))) << 8
		if l[i] != r[i] || math.Abs(float64(l[i]-want)) > 2 {
			t.Fatalf("sample %d: %d %d, want %d", i, l[i], r[i], want)
		}
	}
}

func TestLoopPoint(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x1000, 4, 7, fastEG)) // 0..7, then 4..7 again
	for i := 0; i < 8; i++ {
		mem[0x1000+i] = uint8(i * 10)
	}
	c.play(0, 0, 0, 1, 0, 0)
	l, _ := run(c, 16)
	want := []int{0, 1, 2, 3, 4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7}
	for i, w := range want {
		if l[i]>>8 != int32(w*10) {
			t.Fatalf("sample %d: %d want %d (%v)", i, l[i]>>8, w*10, l)
		}
	}
}

func TestPitch(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x1000, 0, 0xffff, fastEG))
	for i := 0; i < 0x10000; i++ {
		mem[0x1000+i] = uint8(i & 0x7f)
	}
	for _, p := range []struct {
		fn, oct int
		step    float64
	}{{0, 0, 0.5}, {0, 1, 1}, {0, -1, 0.25}, {512, 0, 0.75}, {256, 2, 2.5}, {0, -3, 0.0625}, {1023, 7, 127.9375}} {
		c.play(0, 0, p.fn, p.oct, 0, 0)
		run(c, 1)
		c.Sample()
		before := c.slots[0].pos
		run(c, 64)
		got := float64(c.slots[0].pos-before) / (1 << 32) / 64
		if math.Abs(got-p.step) > 1e-6 {
			t.Errorf("fn %d oct %d: step %v, want %v", p.fn, p.oct, got, p.step)
		}
	}
}

func TestSampleFormats(t *testing.T) {
	c, mem := newChip(t)
	// 12-bit: two samples in three bytes (first's high byte, both low nibbles, second's high byte)
	copy(mem, header(1, 0x1000, 0, 3, fastEG))
	copy(mem[12:], header(2, 0x2000, 0, 3, fastEG))
	copy(mem[0x1000:], []byte{0x12, 0x34, 0x56, 0xfe, 0xdc, 0xba})
	copy(mem[0x2000:], []byte{0x12, 0x34, 0x80, 0x00, 0x7f, 0xff, 0xff, 0xff})
	c.play(0, 0, 0, 1, 0, 0)
	l, _ := run(c, 4)
	if l[0] != 0x1230 || l[1] != 0x5640 || l[2] != -0x130 || l[3] != -0x4540 {
		t.Errorf("12-bit: %x %x %d %d", l[0], l[1], l[2], l[3])
	}
	c.play(1, 1, 0, 1, 0, 0)
	c.pcm(0x68, 0) // channel 0 off
	c.slots[0] = slot{state: egOff, env: envMax}
	l, _ = run(c, 4)
	if l[0] != 0x1234 || l[1] != -0x8000 || l[2] != 0x7fff || l[3] != -1 {
		t.Errorf("16-bit: %d %d %d %d", l[0], l[1], l[2], l[3])
	}
}

func steadyLevel(c *Chip) (int32, int32) {
	run(c, 50)
	l, r := run(c, 1)
	return l[0], r[0]
}

func constWave(mem []byte) {
	copy(mem, header(0, 0x1000, 0, 0xff, fastEG))
	for i := 0; i < 256; i++ {
		mem[0x1000+i] = 0x40
	}
}

func TestTotalLevelPanAndMix(t *testing.T) {
	c, mem := newChip(t)
	constWave(mem)
	db := func(v int32) float64 { return 20 * math.Log10(float64(v)/0x4000) }
	c.play(0, 0, 0, 0, 0, 0)
	if l, r := steadyLevel(c); l != 0x4000 || r != 0x4000 {
		t.Fatalf("full level %x %x", l, r)
	}
	c.play(0, 0, 0, 0, 16, 0) // 16 * 0.375 = 6 dB
	if l, _ := steadyLevel(c); math.Abs(db(l)+6) > 0.1 {
		t.Errorf("TL 16: %.2f dB", db(l))
	}
	for _, p := range []struct {
		pan    int
		dl, dr float64 // dB; -100 = off
	}{{0, 0, 0}, {1, 0, -3}, {6, 0, -18}, {7, 0, -100}, {9, -100, 0}, {10, -18, 0}, {15, -3, 0}, {8, -100, -100}} {
		c.play(0, 0, 0, 0, 0, p.pan)
		l, r := steadyLevel(c)
		check := func(side string, v int32, want float64) {
			if want == -100 {
				if v != 0 {
					t.Errorf("pan %d %s: %d, want silence", p.pan, side, v)
				}
				return
			}
			if math.Abs(db(v)-want) > 0.1 {
				t.Errorf("pan %d %s: %.2f dB, want %v", p.pan, side, db(v), want)
			}
		}
		check("left", l, p.dl)
		check("right", r, p.dr)
	}
	c.play(0, 0, 0, 0, 0, 0)
	c.pcm(0xf9, 0x3a) // right 7 (off), left 2 (-6 dB)
	l, r := steadyLevel(c)
	if r != 0 || math.Abs(db(l)+6) > 0.1 {
		t.Errorf("mix: left %.2f dB right %d", db(l), r)
	}
}

func TestKeyOffRelease(t *testing.T) {
	c, mem := newChip(t)
	constWave(mem)
	c.play(0, 0, 0, 0, 0, 0)
	steadyLevel(c)
	c.pcm(0x68, 0)
	l, _ := run(c, 2000)
	if l[len(l)-1] != 0 || c.slots[0].on {
		t.Fatal("still sounding after key off with release rate 15")
	}
	// a slow release takes longer
	copy(mem, header(0, 0x1000, 0, 0xff, [5]uint8{0, 0xf0, 0, 0x04, 0}))
	c.play(0, 0, 0, 0, 0, 0)
	steadyLevel(c)
	c.pcm(0x68, 0)
	l, _ = run(c, 2000)
	if l[len(l)-1] == 0 {
		t.Fatal("release rate 4 finished within 2000 samples")
	}
}

func TestDamp(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x1000, 0, 0xff, [5]uint8{0, 0xf0, 0, 0x01, 0}))
	for i := 0; i < 256; i++ {
		mem[0x1000+i] = 0x40
	}
	c.play(0, 0, 0, 0, 0, 0)
	steadyLevel(c)
	c.pcm(0x68, 0x40) // key off with damp
	run(c, 3000)
	if c.slots[0].on {
		t.Fatal("damp did not silence the voice")
	}
}

func TestAttackDecay(t *testing.T) {
	c, mem := newChip(t)
	// attack 8, decay 1 to level 3 (9 dB) at rate 10, then hold
	copy(mem, header(0, 0x1000, 0, 0xff, [5]uint8{0, 0x8a, 0x30, 0x0f, 0}))
	for i := 0; i < 256; i++ {
		mem[0x1000+i] = 0x40
	}
	c.play(0, 0, 0, 0, 0, 0)
	l, _ := run(c, 20000)
	peak, last := int32(0), l[len(l)-1]
	for _, v := range l {
		peak = max(peak, v)
	}
	if l[0] >= peak/4 {
		t.Errorf("no attack: first sample %d of peak %d", l[0], peak)
	}
	if peak != 0x4000 {
		t.Errorf("attack peak %x", peak)
	}
	if d := 20 * math.Log10(float64(last)/float64(peak)); math.Abs(d+9) > 0.3 {
		t.Errorf("sustain %.2f dB, want -9", d)
	}
}

func TestLevelSlide(t *testing.T) {
	// a total level written without "direct" moves there step by step
	c, mem := newChip(t)
	constWave(mem)
	c.play(0, 0, 0, 0, 0, 0)
	steadyLevel(c)
	c.pcm(0x50, 0x40<<1)
	l1, _ := steadyLevel(c)
	if l1 < 0x3000 {
		t.Fatalf("level jumped at once: %x", l1)
	}
	run(c, 54*0x40)
	l2, _ := steadyLevel(c)
	if want := int32(0x4000 * math.Pow(10, -24.0/20)); math.Abs(float64(l2-want)) > 8 {
		t.Fatalf("level did not slide to -24 dB: %x want %x", l2, want)
	}
}

func TestTremolo(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x1000, 0, 0xff, [5]uint8{7 << 3, 0xf0, 0, 0x0f, 7}))
	for i := 0; i < 256; i++ {
		mem[0x1000+i] = 0x40
	}
	c.play(0, 0, 0, 0, 0, 0)
	l, _ := run(c, 12000)
	lo, hi := int32(math.MaxInt32), int32(0)
	for _, v := range l[100:] {
		lo, hi = min(lo, v), max(hi, v)
	}
	if d := 20 * math.Log10(float64(hi)/float64(lo)); d < 10 || d > 13 {
		t.Fatalf("tremolo depth 7: %.1f dB swing", d)
	}
}

func TestVibrato(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x1000, 0, 0xffff, [5]uint8{7<<3 | 7, 0xf0, 0, 0x0f, 0}))
	c.play(0, 0, 0, 0, 0, 0)
	lo, hi := uint64(math.MaxUint64), uint64(0)
	last := c.slots[0].pos
	for i := 0; i < 12000; i++ {
		c.Sample()
		d := c.slots[0].pos - last
		last = c.slots[0].pos
		lo, hi = min(lo, d), max(hi, d)
	}
	cents := 1200 * math.Log2(float64(hi)/float64(lo))
	if cents < 150 || cents > 170 {
		t.Fatalf("vibrato depth 7 swing %.0f cents", cents)
	}
}

func TestHeaderBankAndWaveNumberHighBit(t *testing.T) {
	c, mem := newChip(t)
	mem2 := make([]byte, 0x200000)
	copy(mem2, mem)
	c = New(mem2)
	// wave 384 with header bank 1 (register 2 bits 4-2): header at 0x80000
	copy(mem2[0x80000:], header(0, 0x1000, 0, 0xff, fastEG))
	// wave 300 (above 255) from the ordinary table
	copy(mem2[300*12:], header(0, 0x1100, 0, 0xff, fastEG))
	for i := 0; i < 256; i++ {
		mem2[0x1000+i] = 0x10
		mem2[0x1100+i] = 0x20
	}
	c.pcm(2, 1<<2)
	c.play(0, 384, 0, 0, 0, 0)
	if l, _ := steadyLevel(c); l != 0x1000 {
		t.Errorf("header bank wave: %x", l)
	}
	c.play(0, 300, 0, 0, 0, 0)
	if l, _ := steadyLevel(c); l != 0x2000 {
		t.Errorf("wave 300: %x", l)
	}
}

func TestMemoryBeyondROMReadsZero(t *testing.T) {
	c, mem := newChip(t)
	copy(mem, header(0, 0x3f0000, 0, 0xff, fastEG))
	c.play(0, 0, 0, 0, 0, 0)
	if l, _ := steadyLevel(c); l != 0 {
		t.Fatal("reading past the ROM")
	}
}

// ------------------------------------------------------------ FM

func (c *Chip) fmw(bank int, r, v uint8) {
	c.Write(bank*2, r)
	c.Write(bank*2+1, v)
}

// fmTone sets up channel 0 as a plain sine on the carrier.
func fmTone(c *Chip, fnum, block int, outs uint8) {
	c.pcm(0xf8, 0)
	c.fmw(1, 0x05, 1)
	c.fmw(0, 0x20, 0x21) // modulator: sustain, mult 1
	c.fmw(0, 0x23, 0x21)
	c.fmw(0, 0x40, 0x3f) // modulator silent
	c.fmw(0, 0x43, 0x00)
	c.fmw(0, 0x60, 0xf0)
	c.fmw(0, 0x63, 0xf0)
	c.fmw(0, 0x80, 0x0f)
	c.fmw(0, 0x83, 0x0f)
	c.fmw(0, 0xc0, outs)
	c.fmw(0, 0xa0, uint8(fnum))
	c.fmw(0, 0xb0, 0x20|uint8(block)<<2|uint8(fnum>>8))
}

func zeroCrossings(v []int32) int {
	n := 0
	for i := 1; i < len(v); i++ {
		if (v[i-1] < 0) != (v[i] < 0) {
			n++
		}
	}
	return n
}

func TestFMSineFrequency(t *testing.T) {
	c, _ := newChip(t)
	fmTone(c, 0x200, 4, 0x30)
	l, r := run(c, 37286) // one second at the 28.6 MHz board's rate
	// FM samples come at clock/684: f = fnum * 2^block * (clock/684) / 2^20
	want := 0x200 * 16 * (28636350.0 / 684) / (1 << 20)
	got := float64(zeroCrossings(l)) / 2
	if math.Abs(got-want) > 2 {
		t.Fatalf("%.1f Hz, want %.1f", got, want)
	}
	peak := int32(0)
	for i := range l {
		peak = max(peak, l[i])
		if l[i] != r[i] {
			t.Fatal("left and right differ")
		}
	}
	if peak < 30000 || peak > 33000 {
		t.Errorf("peak %d", peak)
	}
}

func TestFMOutputsAndMix(t *testing.T) {
	c, _ := newChip(t)
	fmTone(c, 0x200, 4, 0x10) // A only: left
	l, r := run(c, 2000)
	if zeroCrossings(l) == 0 || zeroCrossings(r) != 0 {
		t.Fatal("output A should only reach the left side")
	}
	c.pcm(0xf8, 0x07) // FM left muted
	l, _ = run(c, 1000)
	for _, v := range l {
		if v != 0 {
			t.Fatal("FM mix level 7 should mute")
		}
	}
}

func TestFMKeyOffAndWaveforms(t *testing.T) {
	c, _ := newChip(t)
	fmTone(c, 0x200, 4, 0x30)
	run(c, 1000)
	c.fmw(0, 0xb0, 0x10|0x02)
	run(c, 3000)
	if c.fm.ops[3].state != egOff {
		t.Fatal("carrier still on after key off")
	}
	// waveform 1 (half sine) never goes negative
	c.fmw(0, 0xe3, 1)
	c.fmw(0, 0xb0, 0x20|0x10|0x02)
	l, _ := run(c, 3000)
	for _, v := range l {
		if v < -1 {
			t.Fatalf("half sine went to %d", v)
		}
	}
}

func TestFMModulationAddsHarmonics(t *testing.T) {
	c, _ := newChip(t)
	fmTone(c, 0x200, 4, 0x30)
	pure, _ := run(c, 8000)
	c.fmw(0, 0x40, 0x00) // modulator at full level
	mod, _ := run(c, 8000)
	if zeroCrossings(mod[2000:]) <= zeroCrossings(pure[2000:])*2 {
		t.Fatalf("modulation: %d crossings vs %d", zeroCrossings(mod[2000:]), zeroCrossings(pure[2000:]))
	}
}

func TestFMTimers(t *testing.T) {
	c, _ := newChip(t)
	c.fmw(0, 0x02, 0xff) // timer 1: one tick
	c.fmw(0, 0x04, 0x01) // start timer 1
	run(c, 10)
	if c.Read(0)&0xc0 != 0xc0 {
		t.Fatalf("status %02x", c.Read(0))
	}
	c.fmw(0, 0x04, 0x80) // IRQ reset
	if c.Read(0) != 0 {
		t.Fatalf("status after reset %02x", c.Read(0))
	}
}

func TestFMRhythmAndFourOperator(t *testing.T) {
	// these only need to make sound and not crash
	c, _ := newChip(t)
	c.pcm(0xf8, 0)
	c.fmw(1, 0x05, 1)
	for ch := 0; ch < 9; ch++ {
		c.fmw(0, uint8(0xa0+ch), 0x80)
		c.fmw(0, uint8(0xb0+ch), 0x14)
		c.fmw(0, uint8(0xc0+ch), 0x30)
	}
	for o := uint8(0); o < 0x16; o++ {
		c.fmw(0, 0x20+o, 0x01)
		c.fmw(0, 0x40+o, 0x00)
		c.fmw(0, 0x60+o, 0xf4)
		c.fmw(0, 0x80+o, 0x44)
	}
	c.fmw(0, 0xbd, 0x3f) // rhythm mode, all drums
	l, _ := run(c, 4000)
	if zeroCrossings(l) == 0 {
		t.Fatal("rhythm mode silent")
	}
	c.fmw(0, 0xbd, 0)
	c.fmw(1, 0x04, 0x3f) // all four-operator voices
	c.fmw(0, 0xb0, 0x34)
	c.fmw(0, 0xb3, 0x34)
	l, _ = run(c, 4000)
	if zeroCrossings(l) == 0 {
		t.Fatal("four-operator voice silent")
	}
}

// ------------------------------------------------------------ state

func TestStateRoundTrip(t *testing.T) {
	a, mem := newChip(t)
	copy(mem, header(0, 0x1000, 0, 0xfff, [5]uint8{3<<3 | 2, 0x9a, 0x35, 0x05, 3}))
	for i := 0; i < 0x1000; i++ {
		mem[0x1000+i] = uint8(i * 7)
	}
	a.play(0, 0, 100, 1, 3, 2)
	a.play(5, 0, 300, -1, 10, 12)
	fmTone(a, 0x155, 5, 0x30)
	run(a, 777)
	st := a.AppendState(nil)
	want, _ := run(a, 5000)

	b := New(mem)
	rest, err := b.LoadState(append(st, 1, 2))
	if err != nil {
		t.Fatal(err)
	}
	if len(rest) != 2 {
		t.Fatalf("%d bytes left", len(rest))
	}
	got, _ := run(b, 5000)
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("sample %d: %d want %d", i, got[i], want[i])
		}
	}
	if _, err := b.LoadState(st[:100]); err == nil {
		t.Fatal("short state loaded")
	}
	if _, err := b.LoadState([]byte{9}); err == nil {
		t.Fatal("wrong version loaded")
	}
}

func TestReset(t *testing.T) {
	c, mem := newChip(t)
	constWave(mem)
	c.play(0, 0, 0, 0, 0, 0)
	fmTone(c, 0x200, 4, 0x30)
	run(c, 100)
	c.Reset()
	l, r := run(c, 100)
	for i := range l {
		if l[i] != 0 || r[i] != 0 {
			t.Fatal("sound after reset")
		}
	}
}

func BenchmarkSample(b *testing.B) {
	c := New(make([]byte, 0x20000))
	mem := c.mem
	copy(mem, header(0, 0x1000, 0, 0xffff, fastEG))
	for ch := 0; ch < 24; ch++ {
		c.play(ch, 0, ch*20, 0, 0, 0)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		c.Sample()
	}
}
