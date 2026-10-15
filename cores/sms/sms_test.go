package sms

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"hash/fnv"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
)

// The test program (testdata/fuzzrom.py, assembled): it keeps changing VDP
// registers, VRAM, CRAM, the PSG, the I/O control port and the mapper from a
// pseudo-random generator mixed with everything it reads back, with raster
// effects from line interrupts and Pause counted. segaProg uses the Sega
// mapper, cmProg the Codemasters one.
var segaProg = "f3ed5631f0dfc39700000000000000000000000000000000000000000000000000000000000000000000000000000000" +
	"0000000000000000f5e5dbbfcb7f28062102c0341809cd7100d3bf3e88d3bfe1f1fbed4d000000000000000000000000" +
	"000000000000f53a03c03c3203c0f1ed45e52a00c07c1f7d1fac677d1f7c1fad6fac672200c0e1c9e52a00c0ad6fb420" +
	"012c2200c0e1c92157132200c0af3202c03203c03204c0213902060b0e807ed3bf79d3bf230c10f6afd3bf3e40d3bf11" +
	"0040cd7100d3be1b7ab320f6afd3bf3ec0d3bf0620cd7100d3be10f9cd7100e6032805fb76f3180bfb0600db7e10fcf3" +
	"cd88003a02c02104c0be28e077cd020118dadbdccd8800dbddcd88003a03c0cd8800cd7100e6073c47c5cd7100e60f87" +
	"5f1600213301197e23666fcd3201c110e8c9e953015301530153018a018a018a01a701a701c101dc01dc01e7011d02fc" +
	"015301cd7100e60f4fcd71004779b7200dcd7100e603280478ee04471813fe01200f78f62047cd7100e607280478f640" +
	"4778d3bf79f680d3bfc9cd7100d3bfcd7100e63ff640d3bfcd7100e63f3c47cd7100d3be10f9c9cd7100e61fd3bf3ec0" +
	"d3bfcd7100e6073c47cd7100d3be10f9c9cd7100d3bfcd7100e63fd3bf0604dbbecd880010f9dbbfcd8800c9cd7100d3" +
	"7fcd7100d37fc9cd7100d33fdb7ecd8800db7fcd8800dbddcd8800c9cd7100e67fc64067cd71006f7ecd8800cd7100e6" +
	"3ff68067cd71006fcd710077c9cd7100e6035fcd7100577bb720077ae61f32fcffc921fcff856f72c936e0fffffffffb" +
	"00000020"

var cmProg = "f3ed5631f0dfc39700000000000000000000000000000000000000000000000000000000000000000000000000000000" +
	"0000000000000000f5e5dbbfcb7f28062102c0341809cd7100d3bf3e88d3bfe1f1fbed4d000000000000000000000000" +
	"000000000000f53a03c03c3203c0f1ed45e52a00c07c1f7d1fac677d1f7c1fad6fac672200c0e1c9e52a00c0ad6fb420" +
	"012c2200c0e1c92121432200c0af3202c03203c03204c0215202060b0e807ed3bf79d3bf230c10f6afd3bf3e40d3bf11" +
	"0040cd7100d3be1b7ab320f6afd3bf3ec0d3bf0620cd7100d3be10f9cd7100e6032805fb76f3180bfb0600db7e10fcf3" +
	"cd88003a02c02104c0be28e077cd020118dadbdccd8800dbddcd88003a03c0cd8800cd7100e6073c47c5cd7100e60f87" +
	"5f1600213301197e23666fcd3201c110e8c9e953015301530153018a018a018a01a701a701c101dc01dc01e7011d02fc" +
	"015301cd7100e60f4fcd71004779b7200dcd7100e603280478ee04471813fe01200f78f62047cd7100e607280478f640" +
	"4778d3bf79f680d3bfc9cd7100d3bfcd7100e63ff640d3bfcd7100e63f3c47cd7100d3be10f9c9cd7100e61fd3bf3ec0" +
	"d3bfcd7100e6073c47cd7100d3be10f9c9cd7100d3bfcd7100e63fd3bf0604dbbecd880010f9dbbfcd8800c9cd7100d3" +
	"7fcd7100d37fc9cd7100d33fdb7ecd8800db7fcd8800dbddcd8800c9cd7100e67fc64067cd71006f7ecd8800cd7100e6" +
	"3ff68067cd71006fcd710077c9cd7100e6035fcd7100577bb720057a320000c91d20057a320040c91d20057a320080c9" +
	"cd7100e61ff6a067cd71006f727ecd8800c936e0fffffffffb00000020"

// testROM puts the program at the start of every 16 KB bank (so any bank
// switching is safe), fills the rest of each bank with numbers, and adds the
// headers.
func testROM(banks int, cm bool) []byte {
	prog, err := hex.DecodeString(segaProg)
	if cm {
		prog, err = hex.DecodeString(cmProg)
	}
	if err != nil {
		panic(err)
	}
	rom := make([]byte, banks*0x4000)
	x := uint32(12345)
	for k := 0; k < banks; k++ {
		b := rom[k*0x4000 : (k+1)*0x4000]
		copy(b, prog)
		for i := 0x2000; i < 0x3fe0; i++ {
			x ^= x << 13
			x ^= x >> 17
			x ^= x << 5
			b[i] = byte(x)
		}
		b[0x3fe0] = byte(k)
		if cm {
			copy(b[0x3fe6:], []byte{0x34, 0x12, 0xcc, 0xed})
		}
		copy(b[0x3ff0:], "TMR SEGA")
	}
	return rom
}

func newTestCore(t testing.TB, rom []byte, sram []byte) *SMS {
	t.Helper()
	c, err := New(rom, core.Options{SampleRate: 48000, SRAM: sram})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*SMS)
}

func frameHash(c core.Core) uint64 {
	h := fnv.New64a()
	b := make([]byte, 4)
	for _, v := range c.Screen() {
		binary.LittleEndian.PutUint32(b, v)
		h.Write(b)
	}
	return h.Sum64()
}

// runScript runs frames of the scripted input from frame `from`, returning a
// hash of every picture and all the sound.
func runScript(c core.Core, ins []frameInput, from, n int) ([]uint64, []float32) {
	var hs []uint64
	var aud []float32
	for i := from; i < from+n; i++ {
		applyInput(c, ins[i])
		c.RunFrame()
		hs = append(hs, frameHash(c))
		aud = append(aud, c.TakeAudio()...)
	}
	return hs, aud
}

func TestTestROMRuns(t *testing.T) {
	for _, cm := range []bool{false, true} {
		c := newTestCore(t, testROM(8, cm), nil)
		if c.codemasters != cm {
			t.Fatalf("Codemasters mapper detected: %v, want %v", c.codemasters, cm)
		}
		info := c.Info()
		if info.System != "sms" || len(info.Buttons) != 8 || info.Players != 2 || info.Stereo {
			t.Fatalf("info %+v", info)
		}
		ins := script(7, 600)
		hs, aud := runScript(c, ins, 0, 600)
		distinct := map[uint64]bool{}
		for _, h := range hs {
			distinct[h] = true
		}
		if len(distinct) < 100 {
			t.Errorf("cm=%v: only %d different pictures in 600 frames", cm, len(distinct))
		}
		perFrame := float64(len(aud)) / 600
		if perFrame < 780 || perFrame > 860 {
			t.Errorf("cm=%v: %.1f samples a frame", cm, perFrame)
		}
		loud := 0
		for _, s := range aud {
			if s > 0.01 || s < -0.01 {
				loud++
			}
		}
		if loud < len(aud)/10 {
			t.Errorf("cm=%v: almost silent (%d of %d samples)", cm, loud, len(aud))
		}
		if c.SRAM() == nil || len(c.SRAM()) != 0x8000 {
			t.Errorf("cm=%v: no battery save", cm)
		}
		if h := c.Height(); h != 192 && h != 224 || c.Width() != 256 || len(c.Screen()) != 256*h {
			t.Errorf("picture %dx%d, %d pixels", c.Width(), h, len(c.Screen()))
		}
	}
}

func TestBatterySaveLoads(t *testing.T) {
	rom := testROM(4, false)
	sram := make([]byte, 0x8000)
	for i := range sram {
		sram[i] = byte(i * 7)
	}
	c := newTestCore(t, rom, sram)
	if !bytes.Equal(c.SRAM(), sram) {
		t.Fatal("the battery save given isn't the one kept")
	}
	if c := newTestCore(t, rom, nil); c.SRAM() != nil {
		t.Fatal("a battery save before the game used its RAM")
	}
}

func TestCopierHeaderSkipped(t *testing.T) {
	rom := testROM(2, false)
	a := newTestCore(t, rom, nil)
	b := newTestCore(t, append(make([]byte, 512), rom...), nil)
	ins := script(3, 200)
	ha, aa := runScript(a, ins, 0, 200)
	hb, ab := runScript(b, ins, 0, 200)
	if !equalU64(ha, hb) || !equalF32(aa, ab) {
		t.Fatal("a ROM with a 512-byte header plays differently")
	}
}

func equalU64(a, b []uint64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func equalF32(a, b []float32) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// A state saved mid-game and loaded (into a new core or the same one) carries
// on exactly as the game did.
func TestSaveStateRoundTrip(t *testing.T) {
	for _, cm := range []bool{false, true} {
		rom := testROM(8, cm)
		ins := script(11, 1400)
		c := newTestCore(t, rom, nil)
		runScript(c, ins, 0, 500)
		st, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		h1, a1 := runScript(c, ins, 500, 400)

		d := newTestCore(t, rom, nil)
		if err := d.LoadState(st); err != nil {
			t.Fatal(err)
		}
		h2, a2 := runScript(d, ins, 500, 400)
		if !equalU64(h1, h2) || !equalF32(a1, a2) {
			t.Fatalf("cm=%v: a loaded state plays differently in a new core", cm)
		}
		if err := c.LoadState(st); err != nil {
			t.Fatal(err)
		}
		h3, a3 := runScript(c, ins, 500, 400)
		if !equalU64(h1, h3) || !equalF32(a1, a3) {
			t.Fatalf("cm=%v: a loaded state plays differently in the same core", cm)
		}
		if !bytes.Equal(c.SRAM(), d.SRAM()) {
			t.Fatal("battery saves differ")
		}
		// damaged states are refused and change nothing
		before, _ := c.SaveState()
		for _, bad := range [][]byte{nil, st[:len(st)/2], append(append([]byte{}, st...), 0), []byte("RPSMS\x01xyz")} {
			if err := c.LoadState(bad); err == nil {
				t.Fatalf("a damaged state (%d bytes) was accepted", len(bad))
			}
		}
		if after, _ := c.SaveState(); !bytes.Equal(before, after) {
			t.Fatal("a refused state changed the machine")
		}
	}
}

func TestEmptyROM(t *testing.T) {
	if _, err := New(nil, core.Options{}); err == nil {
		t.Fatal("an empty file was accepted")
	}
}

// BenchmarkRunFrame: one frame with its sound. RP_SMS_BENCH=<game file>
// benchmarks a real game instead of the test program.
func BenchmarkRunFrame(b *testing.B) {
	rom := testROM(16, false)
	if f := os.Getenv("RP_SMS_BENCH"); f != "" {
		var err error
		if rom, err = os.ReadFile(f); err != nil {
			b.Fatal(err)
		}
		b.Logf("benchmarking %s", filepath.Base(f))
	}
	c := newTestCore(b, rom, nil)
	ins := script(5, 300)
	runScript(c, ins, 0, 300)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		c.SetInput(0, uint32(i/30%2*b1))
		c.RunFrame()
		c.TakeAudio()
	}
	b.ReportMetric(float64(b.N)/b.Elapsed().Seconds(), "frames/s")
}
