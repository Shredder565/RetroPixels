package atari2600

import (
	"math"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
)

// A tiny assembler, enough to build the test program below in the test itself
// (no game files in the repository).
type prog struct {
	b      []byte
	org    int
	labels map[string]int
	fix    []struct {
		at    int
		label string
		rel   bool
	}
}

func newProg(org int) *prog          { return &prog{org: org, labels: map[string]int{}} }
func (p *prog) op(b ...byte) *prog   { p.b = append(p.b, b...); return p }
func (p *prog) label(l string) *prog { p.labels[l] = p.org + len(p.b); return p }
func (p *prog) br(op byte, l string) *prog {
	p.op(op, 0)
	p.fix = append(p.fix, struct {
		at    int
		label string
		rel   bool
	}{len(p.b) - 1, l, true})
	return p
}
func (p *prog) jmp(l string) *prog {
	p.op(0x4c, 0, 0)
	p.fix = append(p.fix, struct {
		at    int
		label string
		rel   bool
	}{len(p.b) - 2, l, false})
	return p
}

// rom4k places the code at $F000 with the reset vector pointing at it.
func (p *prog) rom4k() []byte {
	for _, f := range p.fix {
		a := p.labels[f.label]
		if f.rel {
			p.b[f.at] = byte(a - (p.org + f.at + 1))
		} else {
			p.b[f.at], p.b[f.at+1] = byte(a), byte(a>>8)
		}
	}
	rom := make([]byte, 4096)
	copy(rom, p.b)
	rom[0xffc], rom[0xffd] = byte(p.org), byte(p.org>>8)
	return rom
}

// testROM: a 4K program with a normal 262-line frame that draws colour bars
// moved by the joystick, playfield from the RIOT timer, a player sprite, and
// two sound channels driven by the frame count and the fire button.
func testROM() []byte {
	p := newProg(0xf000)
	p.op(0x78, 0xd8, 0xa2, 0xff, 0x9a) // sei cld ldx #$ff txs
	p.label("frame")
	p.op(0xa9, 0x02, 0x85, 0x02, 0x85, 0x00, 0x85, 0x01)                                     // lda #2 sta WSYNC sta VSYNC sta VBLANK
	p.op(0x85, 0x02, 0x85, 0x02, 0x85, 0x02)                                                 // 3 lines
	p.op(0xa9, 0x00, 0x85, 0x00)                                                             // VSYNC off
	p.op(0xa9, 0x2b, 0x8d, 0x96, 0x02)                                                       // TIM64T = 43
	p.op(0xe6, 0x80)                                                                         // inc $80
	p.op(0xad, 0x80, 0x02, 0x49, 0xff, 0x4a, 0x4a, 0x4a, 0x4a, 0x18, 0x65, 0x81, 0x85, 0x81) // $81 += ~SWCHA>>4
	p.op(0xa5, 0x80, 0x4a, 0x85, 0x17, 0xa9, 0x04, 0x85, 0x15, 0xa9, 0x08, 0x85, 0x19)       // AUDF0 = frame/2, AUDC0 4, AUDV0 8
	p.op(0xa5, 0x0c, 0x29, 0x80, 0x4a, 0x4a, 0x4a, 0x85, 0x1a)                               // AUDV1 = fire ? 0 : 16>>..
	p.op(0xa9, 0x08, 0x85, 0x16, 0xa5, 0x81, 0x85, 0x18)                                     // AUDC1 8, AUDF1 = $81
	p.op(0xa5, 0x81, 0x85, 0x06, 0xa9, 0x3c, 0x85, 0x1b)                                     // COLUP0, GRP0 = $3c
	p.op(0x85, 0x02, 0xa5, 0x81, 0x29, 0x3f, 0xaa)                                           // sta WSYNC; x = $81 & 63
	p.label("pos").op(0xca).br(0x10, "pos")                                                  // coarse position by delay (dex/bpl)
	p.op(0x85, 0x10)                                                                         // RESP0
	p.label("vbw").op(0xad, 0x84, 0x02).br(0xd0, "vbw")                                      // wait for INTIM = 0
	p.op(0x85, 0x02, 0xa9, 0x00, 0x85, 0x01)                                                 // VBLANK off
	p.op(0xa0, 0xc0)                                                                         // ldy #192
	p.label("line")
	p.op(0x98, 0x65, 0x81, 0x85, 0x02, 0x85, 0x09) // tya adc $81 sta WSYNC sta COLUBK
	p.op(0xad, 0x84, 0x02, 0x85, 0x0e)             // PF1 = INTIM
	p.op(0x98, 0x45, 0x80, 0x85, 0x08)             // COLUPF = y ^ frame
	p.op(0x88).br(0xd0, "line")
	p.op(0xa9, 0x02, 0x85, 0x01, 0xa2, 0x1e) // VBLANK on; 30 lines overscan
	p.label("os").op(0x85, 0x02, 0xca).br(0xd0, "os")
	p.jmp("frame")
	return p.rom4k()
}

func newTestCore(t testing.TB, rom []byte, name string) core.Core {
	t.Helper()
	c, err := New(rom, core.Options{SampleRate: 48000, Name: name})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func frameSum(c core.Core) (uint64, []float32) {
	return hashPixels(c.Screen()), c.TakeAudio()
}

func TestTestROMRuns(t *testing.T) {
	c := newTestCore(t, testROM(), "test.bin")
	info := c.Info()
	if info.System != "atari2600" || len(info.Buttons) != 10 || info.Players != 2 || info.Aspect != 4.0/3.0 || info.Stereo {
		t.Fatalf("info %+v", info)
	}
	if c.Width() != 160 || c.Height() != 210 || math.Abs(c.FPS()-59.92) > 0.01 {
		t.Fatalf("picture %dx%d at %g fps", c.Width(), c.Height(), c.FPS())
	}
	seen := map[uint64]bool{}
	samples := 0
	for i := 0; i < 120; i++ {
		c.SetInput(0, uint32(i/30%4*btnRight))
		c.RunFrame()
		h, a := frameSum(c)
		seen[h] = true
		samples += len(a)
		for _, s := range a {
			if s < -1 || s > 1 || s != s {
				t.Fatalf("sample %g", s)
			}
		}
	}
	if len(seen) < 100 {
		t.Errorf("only %d different pictures in 120 frames", len(seen))
	}
	if samples < 120*790 || samples > 120*810 {
		t.Errorf("%d samples for 120 frames", samples)
	}
	colours := map[uint32]bool{}
	for _, p := range c.Screen() {
		colours[p] = true
	}
	if len(colours) < 20 {
		t.Errorf("only %d colours on screen", len(colours))
	}
}

// saveStateRoundTrip checks that a state saved after frame k and loaded into
// a fresh core (and into the same core) continues exactly like the
// uninterrupted run.
func saveStateRoundTrip(t *testing.T, rom []byte, name string, frames int) {
	ins := script(42, frames)
	c := newTestCore(t, rom, name)
	type rec struct {
		h uint64
		a []float32
	}
	var want []rec
	var states [][]byte
	var at []int
	for i := 0; i < frames; i++ {
		applyInput(c, ins[i])
		c.RunFrame()
		h, a := frameSum(c)
		want = append(want, rec{h, a})
		if i%(frames/5) == frames/10 {
			st, err := c.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			states = append(states, st)
			at = append(at, i)
		}
	}
	check := func(c core.Core, from int, how string) {
		for i := from + 1; i < frames; i++ {
			applyInput(c, ins[i])
			c.RunFrame()
			h, a := frameSum(c)
			if h != want[i].h {
				t.Fatalf("%s: state from frame %d: picture differs at frame %d", how, from, i)
			}
			if len(a) != len(want[i].a) {
				t.Fatalf("%s: state from frame %d: %d samples at frame %d, want %d", how, from, len(a), i, len(want[i].a))
			}
			for k := range a {
				if a[k] != want[i].a[k] {
					t.Fatalf("%s: state from frame %d: sound differs at frame %d", how, from, i)
				}
			}
		}
	}
	for k, st := range states {
		fresh := newTestCore(t, rom, name)
		// a fresh core has the buttons the shell last set
		applyInput(fresh, frameInput{p0: ins[at[k]].p0, p1: ins[at[k]].p1, a0: ins[at[k]].a0, a1: ins[at[k]].a1})
		if err := fresh.LoadState(st); err != nil {
			t.Fatal(err)
		}
		check(fresh, at[k], "fresh core")
		if err := c.LoadState(st); err != nil {
			t.Fatal(err)
		}
		check(c, at[k], "same core")
	}
	t.Logf("%s: %d states (%d-%d KB) continue identically", name, len(states), len(states[0])/1024, len(states[len(states)-1])/1024)
}

func TestSaveState(t *testing.T) {
	saveStateRoundTrip(t, testROM(), "test.bin", 600)
}

// TestSaveStateGames runs the round trip on real games when RP_A2600_ROMS is set.
func TestSaveStateGames(t *testing.T) {
	for _, f := range romFiles(t) {
		rom, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := New(rom, core.Options{Name: filepath.Base(f)}); err != nil {
			continue
		}
		t.Run(filepath.Base(f), func(t *testing.T) { saveStateRoundTrip(t, rom, filepath.Base(f), 1000) })
	}
}

func TestLoadStateRejectsJunk(t *testing.T) {
	c := newTestCore(t, testROM(), "test.bin")
	for i := 0; i < 10; i++ {
		c.RunFrame()
	}
	st, _ := c.SaveState()
	before := hashPixels(c.Screen())
	for _, bad := range [][]byte{nil, []byte("hello"), st[:len(st)/2]} {
		if err := c.LoadState(bad); err == nil {
			t.Errorf("LoadState(%d bytes of junk) succeeded", len(bad))
		}
	}
	// a state from a different game (another scheme) is refused too
	other := newTestCore(t, blocks(8192), "x (F8SC)")
	ost, _ := other.SaveState()
	if err := c.LoadState(ost); err == nil {
		t.Error("loaded a state from another cartridge type")
	}
	if hashPixels(c.Screen()) != before {
		t.Error("a refused state changed the game")
	}
	if err := c.LoadState(st); err != nil {
		t.Error(err)
	}
}

func TestResetKeepsGoing(t *testing.T) {
	rom := testROM()
	a := newTestCore(t, rom, "t.bin")
	b := newTestCore(t, rom, "t.bin")
	for i := 0; i < 50; i++ {
		a.RunFrame()
	}
	a.TakeAudio()
	a.Reset()
	for i := 0; i < 30; i++ {
		a.RunFrame()
		b.RunFrame()
		if hashPixels(a.Screen()) != hashPixels(b.Screen()) {
			t.Fatalf("after Reset, frame %d differs from a new game", i)
		}
	}
}

func TestPalette(t *testing.T) {
	// the web core's palette (makePalette in atari2600.js), as 0x00RRGGBB
	want := [128]uint32{
		0x000000, 0x404040, 0x666666, 0x878787, 0xa6a6a6, 0xbfbfbf, 0xd6d6d6, 0xebebeb, 0x2d2d00, 0x636300, 0x848400, 0xa0a000, 0xbaba0f, 0xd0d025, 0xe3e338, 0xf5f449,
		0x521400, 0x884a00, 0xa86b00, 0xc48715, 0xdea12f, 0xf4b745, 0xffca58, 0xffdb6a, 0x6a0000, 0xa03313, 0xc05333, 0xdd6f50, 0xf7896a, 0xff9f7f, 0xffb293, 0xffc4a4,
		0x700024, 0xa7215b, 0xc7427b, 0xe35e97, 0xfd78b1, 0xff8ec7, 0xffa1da, 0xffb2ec, 0x64006a, 0x9a1aa0, 0xbb3bc0, 0xd757dc, 0xf171f6, 0xff86ff, 0xff9aff, 0xffabff,
		0x48009d, 0x7e1fd4, 0x9e3ff4, 0xbb5bff, 0xd575ff, 0xea8bff, 0xfe9eff, 0xffb0ff, 0x2100b4, 0x572eeb, 0x784eff, 0x946aff, 0xae84ff, 0xc49aff, 0xd7aeff, 0xe9bfff,
		0x000eaa, 0x2f44e0, 0x5065ff, 0x6c81ff, 0x869bff, 0x9bb1ff, 0xafc4ff, 0xc0d6ff, 0x00277f, 0x0e5db6, 0x2e7ed6, 0x4b9af2, 0x65b4ff, 0x7acaff, 0x8eddff, 0x9fefff,
		0x003d3f, 0x007475, 0x1c9496, 0x38b0b2, 0x52cacc, 0x67e0e2, 0x7bf3f5, 0x8cffff, 0x004c00, 0x00822d, 0x1ba24d, 0x37be69, 0x51d883, 0x67ee99, 0x7bffad, 0x8cffbe,
		0x004f00, 0x0d8500, 0x2da60c, 0x4ac228, 0x64dc42, 0x79f258, 0x8dff6c, 0x9eff7d, 0x004700, 0x2e7d00, 0x4e9d00, 0x6aba00, 0x84d417, 0x9ae92d, 0xadfd40, 0xbfff52,
		0x203500, 0x566b00, 0x768b00, 0x92a700, 0xacc10b, 0xc2d721, 0xd6eb35, 0xe7fc46, 0x461d00, 0x7d5300, 0x9d7300, 0xb98f07, 0xd3a921, 0xe9bf37, 0xfcd34a, 0xffe45c,
	}
	if palette != want {
		for i := range want {
			if palette[i] != want[i] {
				t.Errorf("palette[%#x] = %06x, want %06x", i, palette[i], want[i])
			}
		}
	}
}

func benchROM(b *testing.B) ([]byte, string) {
	if f := os.Getenv("RP_A2600_BENCH"); f != "" {
		rom, err := os.ReadFile(f)
		if err != nil {
			b.Fatal(err)
		}
		return rom, filepath.Base(f)
	}
	return testROM(), "test.bin"
}

// BenchmarkRunFrame: one frame with its sound. RP_A2600_BENCH=<game file>
// benchmarks a real game instead of the test program.
func BenchmarkRunFrame(b *testing.B) {
	rom, name := benchROM(b)
	c := newTestCore(b, rom, name)
	for i := 0; i < 120; i++ {
		c.SetInput(0, btnReset*uint32(i/10%2))
		c.RunFrame()
		c.TakeAudio()
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		c.SetInput(0, uint32(i/30%2*btnFire))
		c.RunFrame()
		c.TakeAudio()
	}
	b.ReportMetric(float64(b.N)/b.Elapsed().Seconds(), "frames/s")
}
