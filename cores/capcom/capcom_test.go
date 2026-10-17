package capcom

// Tests on generated sets (no ROMs needed): the user's file names and sizes
// with a small program for each CPU, random graphics and PROMs.

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

// 1942: fill the characters, then keep moving the scroll and a sprite and
// copying the joystick into the background; send a sound command.
var prog1942 = []byte{
	0x31, 0x00, 0xf0, // ld sp,f000
	0x21, 0x00, 0xd0, // ld hl,d000
	0x01, 0x00, 0x08, // ld bc,0800
	0x7d, 0x77, 0x23, 0x0b, 0x78, 0xb1, 0x20, 0xf8, // fill: (hl)=l
	0x3e, 0x05, 0x32, 0x00, 0xc8, // sound command 5
	0x3a, 0x00, 0xe0, 0x3c, 0x32, 0x00, 0xe0, // loop: inc (e000)
	0x32, 0x02, 0xc8, // scroll
	0x32, 0x00, 0xcc, 0x32, 0x03, 0xcc, 0x32, 0x02, 0xcc, // sprite 0
	0x3a, 0x01, 0xc0, 0x32, 0x00, 0xd8, // joystick -> background
	0x18, 0xe5, // jr loop
}

// 1942 sound: tone A on AY 1 at full volume, its period from the command.
var snd1942 = []byte{
	0x31, 0x00, 0x48,
	0x3e, 0x07, 0x32, 0x00, 0x80, 0x3e, 0x3e, 0x32, 0x01, 0x80,
	0x3e, 0x08, 0x32, 0x00, 0x80, 0x3e, 0x0f, 0x32, 0x01, 0x80,
	0x3e, 0x00, 0x32, 0x00, 0x80, // loop: select register 0
	0x3a, 0x00, 0x60, 0x32, 0x01, 0x80, // period = command
	0x18, 0xf3,
}

// 1943: layers on, ask the MCU the second question of the table, keep the
// answer at e000, then scroll and move a sprite.
var prog1943 = map[int][]byte{
	0x0000: {0x31, 0x00, 0xf0, 0xc3, 0x00, 0x03},
	0x0300: {
		0x3e, 0x80, 0x32, 0x04, 0xc8,
		0x3e, 0x70, 0x32, 0x06, 0xd8,
		0x3a, 0x1c, 0x02, 0x32, 0x07, 0xc8, // question
		0x3a, 0x07, 0xc0, 0x32, 0x00, 0xe0, // answer -> e000
		0x3e, 0x05, 0x32, 0x00, 0xc8,
		0x2a, 0x02, 0xe0, 0x23, 0x22, 0x02, 0xe0, // loop: inc (e002)
		0x22, 0x00, 0xd8, 0x22, 0x03, 0xd8,
		0x7d, 0x32, 0x00, 0xf0, 0x32, 0x03, 0xf0, 0x32, 0x02, 0xf0,
		0x3a, 0x01, 0xc0, 0x32, 0x00, 0xd0,
		0x18, 0xe1,
	},
}

// 1943 sound: an SSG tone on the first YM2203, period from the command.
var snd1943 = []byte{
	0x31, 0x00, 0xc8,
	0x3e, 0x07, 0x32, 0x00, 0xe0, 0x3e, 0x3e, 0x32, 0x01, 0xe0,
	0x3e, 0x08, 0x32, 0x00, 0xe0, 0x3e, 0x0f, 0x32, 0x01, 0xe0,
	0x3e, 0x00, 0x32, 0x00, 0xe0,
	0x3a, 0x00, 0xc8, 0x32, 0x01, 0xe0,
	0x18, 0xf3,
}

// synthSet makes a set for a game: every file under its first name (or its
// MAME name when mame is set), random bytes but for the programs.
func synthSet(g *arcade.Game, mame bool) (names []string, sizes []int, data [][]byte) {
	r := &rng{0x1943}
	for _, f := range g.Files {
		if f.Optional {
			continue
		}
		b := make([]byte, f.Size)
		for i := range b {
			b[i] = r.byte()
		}
		switch {
		case f.Role == "cpu" && f.Offset == 0 && g.ID == "1942":
			copy(b, prog1942)
		case f.Role == "cpu" && f.Offset == 0:
			for at, p := range prog1943 {
				copy(b[at:], p)
			}
			for i := 0; i < 32; i++ {
				b[c43MCUTable+i*2] = byte(i*7 + 3) // distinct questions
			}
		case f.Role == "sound" && g.ID == "1942":
			copy(b, snd1942)
		case f.Role == "sound":
			copy(b, snd1943)
		}
		n := f.Name
		if mame {
			n = f.Alt[0]
		}
		names, sizes, data = append(names, n), append(sizes, len(b)), append(data, b)
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

func openSynth(t *testing.T, id string, mame bool) *arcade.Core {
	n, s, d := synthSet(gameByID(t, id), mame)
	c, err := arcade.NewFromFiles(n, s, d, "", core.Options{SampleRate: 44100})
	if err != nil {
		t.Fatal(err)
	}
	if c.GameID() != id {
		t.Fatalf("identified as %s, want %s", c.GameID(), id)
	}
	return c
}

// run plays frames with the joystick moving; returns a hash of the
// pictures and the sound.
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

var ids = []string{"1942", "1943", "1943kai"}

func TestSyntheticRuns(t *testing.T) {
	for _, id := range ids {
		for _, mame := range []bool{false, true} {
			c := openSynth(t, id, mame)
			if c.Width() != scrH || c.Height() != scrW {
				t.Fatalf("%s: picture %dx%d", id, c.Width(), c.Height())
			}
			_, audio := run(c, 90)
			// (the first frame after a reset is short: it starts at line 0)
			want := int(math.Round((90*lines - (lines - vblankLine)) * 44100 / (fps * lines)))
			if len(audio) < want-2 || len(audio) > want+2 {
				t.Errorf("%s: %d samples for 90 frames, want %d", id, len(audio), want)
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
			if len(colours) < 4 {
				t.Errorf("%s: only %d colours on screen", id, len(colours))
			}
		}
	}
}

func TestMCUAnswers(t *testing.T) {
	for _, id := range []string{"1943", "1943kai"} {
		c := openSynth(t, id, false)
		run(c, 2)
		b := c.Board().(*b1943)
		want := b.rom[c43MCUTable+3] // the answer to the second question
		if got := b.ram[0]; got != want {
			t.Errorf("%s: MCU answered %02x, want %02x", id, got, want)
		}
		if b.mcuReply(0xff) != 0 {
			t.Errorf("%s: unknown question answered", id)
		}
	}
}

func TestSaveStateAndReset(t *testing.T) {
	for _, id := range ids {
		c := openSynth(t, id, false)
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
			if math.Abs(float64(a1[i]-a2[i])) > 1e-6 {
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

func TestGfxDecode(t *testing.T) {
	// one character: line 0 = bytes 0x81, 0x10: pixel x takes bit (7-x) of
	// the high nibble as its low bit and of the low nibble as its high bit
	rom := make([]byte, 16)
	rom[0], rom[1] = 0x81, 0x10
	px := charLayout().decode(rom, 1)
	want := []uint8{1, 0, 0, 2, 0, 0, 0, 1}
	if !bytes.Equal(px[:8], want) {
		t.Fatalf("line 0 = %v, want %v", px[:8], want)
	}
	if promColour(15, 0, 8) != arcade.RGB(255, 0, 143) {
		t.Fatalf("colour %06x", promColour(15, 0, 8))
	}
}
