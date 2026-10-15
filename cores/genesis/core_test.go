package genesis

import (
	"bytes"
	"encoding/binary"
	"hash/fnv"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/core"
)

func testCart(t testing.TB, name string) []byte {
	t.Helper()
	for _, c := range cartTests() {
		if c.name == name {
			return c.rom
		}
	}
	t.Fatalf("no test cartridge %s", name)
	return nil
}

func newTestCore(t testing.TB, rom, sram []byte) *Genesis {
	t.Helper()
	c, err := New(rom, core.Options{SampleRate: 48000, SRAM: sram})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*Genesis)
}

type frameOut struct {
	pic   uint64
	w, h  int
	audio []float32
}

func runFrames(c core.Core, ins []frameInput) []frameOut {
	var out []frameOut
	for _, in := range ins {
		applyInput(c, in)
		c.RunFrame()
		h := fnv.New64a()
		b := make([]byte, 4)
		for _, p := range c.Screen() {
			binary.LittleEndian.PutUint32(b, p)
			h.Write(b)
		}
		out = append(out, frameOut{h.Sum64(), c.Width(), c.Height(), c.TakeAudio()})
	}
	return out
}

func sameFrames(t *testing.T, what string, a, b []frameOut) {
	t.Helper()
	for i := range a {
		if a[i].pic != b[i].pic || a[i].w != b[i].w || a[i].h != b[i].h {
			t.Fatalf("%s: frame %d's picture differs", what, i)
		}
		if len(a[i].audio) != len(b[i].audio) {
			t.Fatalf("%s: frame %d: %d samples, want %d", what, i, len(b[i].audio), len(a[i].audio))
		}
		for k := range a[i].audio {
			if a[i].audio[k] != b[i].audio[k] {
				t.Fatalf("%s: frame %d: sample %d differs", what, i, k)
			}
		}
	}
}

// A state saved mid-game and loaded (into the same machine and into a new
// one) carries on exactly as the game would have.
func TestStateRoundTrip(t *testing.T) {
	for _, name := range []string{"vdp-U.bin", "vdp-E.bin", "ee-acclaim32-c65.bin", "ee-cm-c16.bin", "sram.bin", "jcart.bin", "pad6.bin", "ssf2.bin", "realtec.bin"} {
		t.Run(name, func(t *testing.T) {
			rom := testCart(t, name)
			ins := script(uint64(len(name)), 600)
			a := newTestCore(t, rom, nil)
			runFrames(a, ins[:300])
			st, err := a.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			want := runFrames(a, ins[300:])
			if err := a.LoadState(st); err != nil {
				t.Fatal(err)
			}
			sameFrames(t, "loaded into the same machine", want, runFrames(a, ins[300:]))
			b := newTestCore(t, rom, nil)
			runFrames(b, ins[:7])
			if err := b.LoadState(st); err != nil {
				t.Fatal(err)
			}
			sameFrames(t, "loaded into a new machine", want, runFrames(b, ins[300:]))
			if !bytes.Equal(a.SRAM(), b.SRAM()) {
				t.Fatal("battery saves differ")
			}
		})
	}
}

// Interlace: the other field's lines come back with the state.
func TestStateKeepsInterlacedPicture(t *testing.T) {
	rom := testCart(t, "vdp-U.bin")
	a := newTestCore(t, rom, nil)
	ins := script(9, 400)
	for i, in := range ins {
		applyInput(a, in)
		a.RunFrame()
		a.TakeAudio()
		if a.vdp.im2() && i > 50 {
			st, _ := a.SaveState()
			b := newTestCore(t, rom, nil)
			if err := b.LoadState(st); err != nil {
				t.Fatal(err)
			}
			for k := 0; k < 3; k++ {
				applyInput(a, ins[(i+1+k)%len(ins)])
				applyInput(b, ins[(i+1+k)%len(ins)])
				a.RunFrame()
				b.RunFrame()
				if !slicesEqual(a.Screen(), b.Screen()) {
					t.Fatalf("frame %d after loading differs", k)
				}
			}
			return
		}
	}
	t.Skip("the test program never switched interlace on")
}

func slicesEqual(a, b []uint32) bool {
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

func TestStateRejected(t *testing.T) {
	a := newTestCore(t, testCart(t, "ee-sega.bin"), nil)
	runFrames(a, script(1, 60))
	st, _ := a.SaveState()
	b := newTestCore(t, testCart(t, "sram.bin"), nil)
	runFrames(b, script(2, 60))
	before, _ := b.SaveState()
	if err := b.LoadState(st); err == nil {
		t.Fatal("a state from an EEPROM game loaded into a game without one")
	}
	for _, bad := range [][]byte{nil, []byte("RPGEN\x01"), st[:len(st)/2], append(append([]byte(nil), st...), 0)} {
		if err := a.LoadState(bad); err == nil {
			t.Fatalf("a damaged state (%d bytes) loaded", len(bad))
		}
	}
	after, _ := b.SaveState()
	if !bytes.Equal(before, after) {
		t.Fatal("a refused state changed the machine")
	}
}

// Battery saves come back: SRAM and each EEPROM kind.
func TestBatterySaves(t *testing.T) {
	for _, name := range []string{"sram.bin", "ee-sega.bin", "ee-acclaim16.bin", "ee-acclaim32-c65.bin", "ee-cm-c08.bin"} {
		rom := testCart(t, name)
		a := newTestCore(t, rom, nil)
		runFrames(a, script(5, 500))
		s := a.SRAM()
		used := 0
		for _, v := range s {
			if v != 0xff {
				used++
			}
		}
		if used == 0 {
			t.Fatalf("%s: nothing saved", name)
		}
		b := newTestCore(t, rom, s)
		if !bytes.Equal(b.SRAM(), s) {
			t.Fatalf("%s: the save didn't come back", name)
		}
	}
	if s := newTestCore(t, testCart(t, "pad6.bin"), nil).SRAM(); s != nil {
		t.Fatalf("a game without a battery save has one (%d bytes)", len(s))
	}
}

func TestRefusesNonGames(t *testing.T) {
	if _, err := New(nil, core.Options{}); err == nil {
		t.Fatal("an empty file started")
	}
	if _, err := New(testCart(t, "svp.bin"), core.Options{}); err == nil || !strings.Contains(err.Error(), "SVP") {
		t.Fatalf("Virtua Racing: %v", err)
	}
}

func TestInfo(t *testing.T) {
	c := newTestCore(t, testCart(t, "vdp-U.bin"), nil)
	in := c.Info()
	if in.System != "segaMD" || in.Players != 4 || !in.Stereo || len(in.Buttons) != 12 || in.Buttons[7] != "Mode" {
		t.Fatalf("info: %+v", in)
	}
	if math.Abs(c.FPS()-59.92) > 0.01 {
		t.Fatalf("NTSC fps %g", c.FPS())
	}
	if p := newTestCore(t, testCart(t, "vdp-E.bin"), nil); math.Abs(p.FPS()-49.70) > 0.01 || !p.pal {
		t.Fatalf("PAL fps %g", p.FPS())
	}
	c.RunFrame()
	if c.Width() != 320 || c.Height() != 224 || len(c.Screen()) != 320*224 {
		t.Fatalf("picture %dx%d", c.Width(), c.Height())
	}
	// stereo, at the asked rate: about 48000/59.92 pairs a frame
	if n := len(c.TakeAudio()); n < 2*790 || n > 2*812 {
		t.Fatalf("%d samples in a frame", n)
	}
}

// The header's region field, in all its spellings.
func TestRegion(t *testing.T) {
	for s, want := range map[string]string{"JUE": "us", "J": "jp", "E": "eu", "U": "us", "EJ": "jp", "4": "us", "1": "jp", "8": "eu",
		"A": "eu", "F": "us", "e": "eu", "   ": "us", "\xa04 ": "us", "C": "us", "j  ": "jp"} {
		rom := make([]byte, 0x200)
		copy(rom[0x1f0:], s+"   ")
		if got := headerRegion(rom); got != want {
			t.Errorf("region %q: %s, want %s", s, got, want)
		}
	}
}

func TestGameCode(t *testing.T) {
	for s, want := range map[string]string{"GM MK-1215 -00": "MK-1215", "GM T-50176-00": "T-50176", "T-081326 00": "T-081326",
		"GM 00001009-00": "00001009", "  AL T-12 ": "T", "MK-1229": "MK-1229", "GMT-5000 -01": "GMT-5000", "-00": "", "": ""} {
		rom := make([]byte, 0x200)
		copy(rom[0x180:], s+strings.Repeat(" ", 14))
		if got := gameCode(rom); got != want {
			t.Errorf("code %q: %q, want %q", s, got, want)
		}
	}
}

// RP_MD_BENCH_ROM picks a game for the benchmark (else the VDP test program).
func BenchmarkFrame(b *testing.B) {
	rom := testCart(b, "vdp-U.bin")
	if p := os.Getenv("RP_MD_BENCH_ROM"); p != "" {
		var err error
		if rom, err = os.ReadFile(p); err != nil {
			b.Fatal(err)
		}
		b.Log(filepath.Base(p))
	}
	c := newTestCore(b, rom, nil)
	ins := script(3, 600)
	for i := 0; i < 300; i++ {
		applyInput(c, ins[i])
		c.RunFrame()
		c.TakeAudio()
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		applyInput(c, ins[300+i%300])
		c.RunFrame()
		c.TakeAudio()
	}
}
