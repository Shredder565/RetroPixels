package technos

// Tests on generated sets (no ROMs needed): the user's file names and sizes,
// random graphics and samples, and a small program for each CPU.

import (
	"bytes"
	"hash/fnv"
	"math"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

type rng struct{ s uint64 }

func (r *rng) byte() byte {
	r.s ^= r.s << 13
	r.s ^= r.s >> 7
	r.s ^= r.s << 17
	return byte(r.s >> 24)
}

// mainProg is the 6309's program at 8000 (both boards): fill the character
// layer and the palette (palLo..palHi) with their address bits, let the
// sub CPU run, send a sound command, then keep scrolling and copying the
// first joystick into the character layer.
func mainProg(palLo, palHi uint16) []byte {
	p := []byte{
		0x10, 0xce, 0x0f, 0x00, // lds #$0f00
		0x8e, 0x18, 0x00, // ldx #$1800
		0x1f, 0x10, 0xe7, 0x80, // tfr x,d; stb ,x+
		0x8c, 0x20, 0x00, 0x26, 0xf7, // cmpx #$2000; bne
		0x8e, byte(palLo >> 8), byte(palLo),
		0x1f, 0x10, 0xe7, 0x80,
		0x8c, byte(palHi >> 8), byte(palHi), 0x26, 0xf7,
		0x86, 0x08, 0xb7, 0x38, 0x08, // sub out of reset
		0x86, 0x01, 0xb7, 0x38, 0x0e, // sound command 1
		0x7c, 0x00, 0x10, // loop: inc $0010
		0xb6, 0x00, 0x10, 0xb7, 0x38, 0x09, // x scroll
		0xb6, 0x38, 0x00, 0xb7, 0x18, 0x00, // joystick -> character layer
		0x20, 0xef, // bra loop
		0x3b, // rti (every interrupt)
	}
	return p
}

// vectors6809 points the 6809 vectors at the end of the ROM to rti, and
// reset to start.
func vectors6809(rom []byte, start, rti uint16) {
	n := len(rom)
	for v := n - 10; v < n-2; v += 2 {
		rom[v], rom[v+1] = byte(rti>>8), byte(rti)
	}
	rom[n-2], rom[n-1] = byte(start>>8), byte(start)
}

// Double Dragon's sound 6809: play the first 16 KB of the first ADPCM ROM.
var ddSndProg = []byte{
	0x10, 0xce, 0x0f, 0x00,
	0x86, 0x00, 0xb7, 0x38, 0x04, // start address 0
	0x86, 0x20, 0xb7, 0x38, 0x02, // end address $20
	0xb7, 0x38, 0x00, // go
	0x20, 0xfe,
	0x3b,
}

// Double Dragon's 63701: write $5a into the shared RAM.
var ddSubProg = []byte{0x8e, 0x01, 0x3f, 0x86, 0x5a, 0xb7, 0x80, 0x00, 0x20, 0xfe}

// Double Dragon II's sub Z80: the same.
var dd2SubProg = []byte{0x3e, 0x5a, 0x32, 0x00, 0xc0, 0x18, 0xfe}

// Double Dragon II's sound Z80: play OKI sample 1 on channel 1.
var dd2SndProg = []byte{
	0x31, 0xff, 0x87, 0xf3,
	0x3e, 0x81, 0x32, 0x00, 0x98,
	0x3e, 0x10, 0x32, 0x00, 0x98,
	0x18, 0xfe,
}

func synthSet(g *arcade.Game) (names []string, sizes []int, data [][]byte) {
	r := &rng{0x21}
	for _, f := range g.Files {
		if f.Optional {
			continue
		}
		b := make([]byte, f.Size)
		for i := range b {
			b[i] = r.byte()
		}
		switch {
		case f.Role == "cpu" && g.ID == "ddragon":
			copy(b, mainProg(0x1000, 0x1400))
			vectors6809(b, 0x8000, 0x8037)
		case f.Role == "cpu":
			copy(b, mainProg(0x3c00, 0x4000))
			vectors6809(b, 0x8000, 0x8037)
		case f.Role == "sound" && g.ID == "ddragon":
			copy(b, ddSndProg)
			vectors6809(b, 0x8000, 0x8013)
		case f.Role == "sound":
			copy(b, dd2SndProg)
			copy(b[0x66:], []byte{0xed, 0x45}) // retn
		case f.Role == "mcu":
			copy(b, ddSubProg)
			b[len(b)-2], b[len(b)-1] = 0xc0, 0x00
		case f.Role == "sub":
			copy(b, dd2SubProg)
			copy(b[0x66:], []byte{0xed, 0x45})
		case f.Role == "oki" && f.Offset == 0:
			copy(b[8:], []byte{0x00, 0x04, 0x00, 0x00, 0x20, 0x00}) // sample 1
		}
		names, sizes, data = append(names, f.Name), append(sizes, len(b)), append(data, b)
	}
	return
}

func gameByID(t *testing.T, id string) *arcade.Game {
	for _, e := range arcade.AllGames() {
		if e.Game.ID == id {
			return e.Game
		}
	}
	t.Fatal("no game " + id)
	return nil
}

func openSynth(t *testing.T, id string, dips map[string]int) *arcade.Core {
	n, s, d := synthSet(gameByID(t, id))
	c, err := arcade.NewFromFiles(n, s, d, "", core.Options{SampleRate: 44100, Dips: dips})
	if err != nil {
		t.Fatal(err)
	}
	if c.GameID() != id {
		t.Fatalf("identified as %s, want %s", c.GameID(), id)
	}
	return c
}

func run(c *arcade.Core, frames int) (uint64, []float32) {
	h := fnv.New64a()
	var audio []float32
	for f := 0; f < frames; f++ {
		c.SetInput(0, uint32(f/7)&15)
		c.RunFrame()
		for _, p := range c.Screen() {
			h.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16)})
		}
		audio = append(audio, c.TakeAudio()...)
	}
	return h.Sum64(), audio
}

var ids = []string{"ddragon", "ddragon2"}

func TestSyntheticRuns(t *testing.T) {
	for _, id := range ids {
		c := openSynth(t, id, nil)
		if c.Width() != scrW || c.Height() != scrH {
			t.Fatalf("%s: picture %dx%d", id, c.Width(), c.Height())
		}
		_, audio := run(c, 60)
		want := 60 * 44100 / fps
		if math.Abs(float64(len(audio))-want) > 44100/fps {
			t.Errorf("%s: %d samples for 60 frames, want about %.0f", id, len(audio), want)
		}
		rms := 0.0
		for _, v := range audio[len(audio)/2:] {
			rms += float64(v) * float64(v)
		}
		if rms = math.Sqrt(rms / float64(len(audio)/2)); rms < 1e-3 {
			t.Errorf("%s: silent (rms %g)", id, rms)
		}
		colours := map[uint32]bool{}
		for _, p := range c.Screen() {
			colours[p] = true
		}
		if len(colours) < 8 {
			t.Errorf("%s: only %d colours on screen", id, len(colours))
		}
		var shared byte
		switch b := c.Board().(type) {
		case *ddragon:
			shared = b.shared[0]
		case *ddragon2:
			shared = b.shared[0]
		}
		if shared != 0x5a {
			t.Errorf("%s: the sub CPU didn't write the shared RAM (%02x)", id, shared)
		}
	}
}

func TestSaveStateAndReset(t *testing.T) {
	for _, id := range ids {
		c := openSynth(t, id, nil)
		first, _ := run(c, 40)
		s, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		h1, a1 := run(c, 40)
		if err := c.LoadState(s); err != nil {
			t.Fatalf("%s: %v", id, err)
		}
		h2, a2 := run(c, 40)
		if h1 != h2 || len(a1) != len(a2) {
			t.Errorf("%s: a loaded state plays differently", id)
		}
		for i := range a1 {
			if i < len(a2) && math.Abs(float64(a1[i]-a2[i])) > 1e-6 {
				t.Errorf("%s: sound differs after loading at sample %d", id, i)
				break
			}
		}
		s2, _ := c.SaveState()
		if err := c.LoadState(s2[:len(s2)/2]); err == nil {
			t.Errorf("%s: half a state was accepted", id)
		}
		c.Reset()
		if h, _ := run(c, 40); h != first {
			t.Errorf("%s: reset doesn't start over", id)
		}
		s3, _ := c.SaveState()
		s4, _ := c.SaveState()
		if !bytes.Equal(s3, s4) {
			t.Errorf("%s: saving changes the state", id)
		}
	}
}

func TestDips(t *testing.T) {
	c := openSynth(t, "ddragon", nil)
	b := c.Board().(*ddragon)
	if b.read(0x3803) != 0xff || b.read(0x3804) != 0xff {
		t.Errorf("default switches %02x %02x", b.read(0x3803), b.read(0x3804))
	}
	c = openSynth(t, "ddragon", map[string]int{"lives": 0, "coina": 5, "difficulty": 0})
	b = c.Board().(*ddragon)
	if b.read(0x3803) != 0xfd || b.read(0x3804) != 0x3c {
		t.Errorf("switches %02x %02x", b.read(0x3803), b.read(0x3804))
	}
	c = openSynth(t, "ddragon2", nil)
	b2 := c.Board().(*ddragon2)
	if b2.read(0x3803) != 0xff {
		t.Errorf("DD2 SW1 %02x", b2.read(0x3803))
	}
}

func TestGfxDecode(t *testing.T) {
	// one character with a single bit set lights one pixel of its first line
	rom := make([]byte, 32)
	rom[0] = 0x01
	px := charLayout().decode(rom, 1)
	n := 0
	for _, v := range px[:8] {
		if v != 0 {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("one set bit gave %d lit pixels: %v", n, px[:8])
	}
	for _, v := range px[8:] {
		if v != 0 {
			t.Fatal("bit spilled into another line")
		}
	}
	if lpCoef(48000) <= 0 || lpCoef(48000) >= 1 {
		t.Fatal("low-pass coefficient")
	}
}
