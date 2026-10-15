package atari7800

import (
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/core"
)

func newTest(t testing.TB, rom []byte) *Atari {
	t.Helper()
	c, err := New(rom, core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*Atari)
}

type frameRec struct {
	pix   uint64
	audio []float32
}

func runFrames(m *Atari, ins []frameInput) []frameRec {
	out := make([]frameRec, len(ins))
	for i, in := range ins {
		applyInput(m, in)
		m.RunFrame()
		out[i] = frameRec{hashPixels(m.Screen()), m.TakeAudio()}
	}
	return out
}

func sameFrames(t *testing.T, what string, a, b []frameRec) {
	t.Helper()
	for i := range a {
		if a[i].pix != b[i].pix {
			t.Fatalf("%s: frame %d: pictures differ", what, i)
		}
		if len(a[i].audio) != len(b[i].audio) {
			t.Fatalf("%s: frame %d: %d samples, then %d", what, i, len(a[i].audio), len(b[i].audio))
		}
		for k := range a[i].audio {
			if a[i].audio[k] != b[i].audio[k] {
				t.Fatalf("%s: frame %d: sample %d differs", what, i, k)
			}
		}
	}
}

func TestBoardsRun(t *testing.T) {
	for _, c := range exCarts() {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			rom := exerciserROM(c, 7)
			cc, err := New(rom, core.Options{SampleRate: 44100})
			if c.mapper == mapSouper {
				if err == nil || !strings.Contains(err.Error(), "SOUPER") {
					t.Fatalf("SOUPER board: got %v", err)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			m := cc.(*Atari)
			if m.cart.kind != c.mapper {
				t.Fatalf("board %s, want %s", mapperNames[m.cart.kind], mapperNames[c.mapper])
			}
			if len(m.cart.pokeys) != len(c.pokeys) {
				t.Fatalf("%d POKEYs, want %d", len(m.cart.pokeys), len(c.pokeys))
			}
			pics := map[uint64]bool{}
			loud := false
			for _, f := range runFrames(m, script(3, 80)) {
				pics[f.pix] = true
				if n := len(f.audio); n < 730 || n > 740 {
					t.Fatalf("%d samples in a frame at 44100 Hz", n)
				}
				for _, s := range f.audio {
					if math.Abs(float64(s)) > 0.01 {
						loud = true
					}
				}
			}
			if len(pics) < 20 || !loud {
				t.Fatalf("%d different pictures, sound %v", len(pics), loud)
			}
			if m.cart.romMissing {
				t.Fatal("read past the end of the ROM")
			}
		})
	}
}

func TestParseCart(t *testing.T) {
	for _, c := range exCarts() {
		rom := exerciserROM(c, 1)
		_, info, _ := parseCart(rom)
		if info.mapper != c.mapper || info.ram != c.ram || info.rom4000 != c.rom4000 || info.bankset != c.bankset || info.header != (c.header != 0) {
			// headerless dumps are guessed from their size
			if c.header != 0 || info.mapper != c.mapper {
				t.Errorf("%s: parsed %+v", c.name, info)
			}
		}
		if len(info.pokeys) != len(c.pokeys) {
			t.Errorf("%s: POKEYs %v, want %v", c.name, info.pokeys, c.pokeys)
		}
	}
	if _, err := New(nil, core.Options{}); err == nil {
		t.Error("an empty file was accepted")
	}
}

func saveStateRoundTrip(t *testing.T, rom []byte, frames int) {
	m := newTest(t, rom)
	ins := script(11, frames+150)
	runFrames(m, ins[:frames])
	st, err := m.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	a := runFrames(m, ins[frames:])
	if err := m.LoadState(st); err != nil {
		t.Fatal(err)
	}
	b := runFrames(m, ins[frames:])
	sameFrames(t, "same core", a, b)
	// into a freshly started copy of the game
	n := newTest(t, rom)
	runFrames(n, ins[:3])
	if err := n.LoadState(st); err != nil {
		t.Fatal(err)
	}
	sameFrames(t, "new core", a, runFrames(n, ins[frames:]))
	// saving again gives the same state
	if err := n.LoadState(st); err != nil {
		t.Fatal(err)
	}
	st2, _ := n.SaveState()
	if string(st) != string(st2) {
		t.Fatal("saving a loaded state gives a different state")
	}
}

func TestSaveState(t *testing.T) {
	for _, c := range exCarts() {
		if c.mapper == mapSouper {
			continue
		}
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			saveStateRoundTrip(t, exerciserROM(c, 5), 100)
		})
	}
}

// TestSaveStateGames: RP_A7800_ROMS=dir go test -run SaveStateGames
func TestSaveStateGames(t *testing.T) {
	dir := os.Getenv("RP_A7800_ROMS")
	if dir == "" {
		t.Skip("RP_A7800_ROMS not set")
	}
	ents, _ := os.ReadDir(dir)
	for _, e := range ents {
		if strings.HasSuffix(strings.ToLower(e.Name()), ".a78") {
			rom, err := os.ReadFile(filepath.Join(dir, e.Name()))
			if err != nil {
				t.Fatal(err)
			}
			t.Run(e.Name(), func(t *testing.T) { saveStateRoundTrip(t, rom, 600) })
		}
	}
}

func TestLoadStateRejects(t *testing.T) {
	c := exCarts()[11] // SuperGame, banked RAM, POKEY
	m := newTest(t, exerciserROM(c, 1))
	runFrames(m, script(1, 30))
	st, _ := m.SaveState()
	for _, bad := range [][]byte{nil, []byte("junk"), st[:len(st)/2]} {
		if err := m.LoadState(bad); err == nil {
			t.Fatal("a damaged state was accepted")
		}
	}
	// a state from a board with a different number of POKEYs
	other := newTest(t, exerciserROM(exCarts()[0], 1))
	ost, _ := other.SaveState()
	if err := m.LoadState(ost); err == nil {
		t.Fatal("a state from another board was accepted")
	}
	// the game is unharmed
	if err := m.LoadState(st); err != nil {
		t.Fatal(err)
	}
}

func TestResetKeepsInput(t *testing.T) {
	m := newTest(t, exerciserROM(exCarts()[8], 2))
	runFrames(m, script(2, 50))
	m.SetInput(0, btnB1|btnLeft)
	m.Reset()
	if m.input[0] != btnB1|btnLeft {
		t.Fatal("reset lost the buttons held")
	}
	a := runFrames(m, script(4, 100))
	n := newTest(t, exerciserROM(exCarts()[8], 2))
	n.SetInput(0, btnB1|btnLeft)
	sameFrames(t, "after reset", a, runFrames(n, script(4, 100)))
}

func TestInfo(t *testing.T) {
	m := newTest(t, exerciserROM(exCarts()[0], 1))
	i := m.Info()
	if i.System != "atari7800" || len(i.Buttons) != 11 || i.Players != 2 || i.Stereo || i.Aspect != 4.0/3 {
		t.Fatalf("%+v", i)
	}
	if m.Width() != 320 || m.Height() != 240 || len(m.Screen()) != 320*240 || m.SRAM() != nil {
		t.Fatal("picture size")
	}
	if math.Abs(m.FPS()-59.9573) > 0.001 {
		t.Fatalf("fps %v", m.FPS())
	}
	// the web palette: hue 0 is grey, colour kill turns everything grey
	if palette[0] != 0 || palette[15] != 0xededed || grey[0x47] != palette[7] {
		t.Fatalf("palette %06x %06x", palette[0], palette[15])
	}
}

func benchROM(b *testing.B) []byte {
	if p := os.Getenv("RP_A7800_BENCH"); p != "" {
		rom, err := os.ReadFile(p)
		if err != nil {
			b.Fatal(err)
		}
		return rom
	}
	// SuperGame with two POKEYs: the most work per frame
	return exerciserROM(exCarts()[25], 1)
}

func BenchmarkFrame(b *testing.B) {
	m := newTest(b, benchROM(b))
	ins := script(1, 600)
	for i := 0; i < 120; i++ {
		applyInput(m, ins[i])
		m.RunFrame()
		m.TakeAudio()
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		applyInput(m, ins[i%len(ins)])
		m.RunFrame()
		m.TakeAudio()
	}
}
