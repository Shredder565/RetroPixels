package namco

// Tests on generated sets (no ROMs needed): the user's file names and sizes
// (or MAME's), small programs for the processors, random graphics and PROMs.

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

// mainProg: let the sub processors run, turn the stars on, start voice 1,
// fill the video RAM, then keep counting at $8900, moving sprite 0 with the
// count and patting the watchdog.
func mainProg() []byte {
	p := []byte{
		0x31, 0x00, 0x8c, // ld sp,8c00
		0x3e, 0x01, // ld a,1
		0x32, 0x23, 0x68, // ld (6823),a: sub processors run
		0x32, 0x05, 0xa0, // ld (a005),a: stars on
		0x3e, 0x0f, 0x32, 0x15, 0x68, // voice 1 volume 15
		0x3e, 0x04, 0x32, 0x13, 0x68, // voice 1 frequency $4000
		0x3e, 0x90, 0x32, 0x80, 0x93, // sprite 0 line
		0x3e, 0x20, 0x32, 0x81, 0x8b, // sprite 0 colour
		0x21, 0x00, 0x80, // ld hl,8000
	}
	fill := len(p)
	p = append(p,
		0x7d, 0x77, 0x23, // ld a,l; ld (hl),a; inc hl
		0x7c, 0xfe, 0x88, // ld a,h; cp 88
		0x20, 0) // jr nz,fill
	p[len(p)-1] = byte(fill - len(p))
	loop := len(p)
	p = append(p,
		0x3a, 0x00, 0x89, 0x3c, 0x32, 0x00, 0x89, // inc (8900)
		0x32, 0x81, 0x93, // sprite 0 column
		0x32, 0x30, 0x68, // watchdog
		0x18, 0) // jr loop
	p[len(p)-1] = byte(loop - len(p))
	return p
}

// subProg: wait forever; answer the NMI.
func subProg() []byte {
	p := make([]byte, 0x68)
	p[0], p[1] = 0x18, 0xfe
	p[0x66], p[0x67] = 0xed, 0x45
	return p
}

// synthSet makes a set for one of the games: every file it needs under the
// user's name (or MAME's first one), random bytes but for the programs.
func synthSet(g *arcade.Game, mame bool) (names []string, sizes []int, data [][]byte) {
	r := &rng{0x1981}
	for _, f := range g.Files {
		if f.Optional {
			continue
		}
		b := make([]byte, f.Size)
		for i := range b {
			b[i] = r.byte()
		}
		switch {
		case f.Role == "cpu1" && f.Offset == 0:
			p := mainProg()
			copy(b, p)
			copy(b[0x66:], []byte{0xed, 0x45})
		case (f.Role == "cpu2" || f.Role == "cpu3") && f.Offset == 0:
			copy(b, subProg())
		}
		n := f.Name
		if mame && len(f.Alt) > 0 {
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
	t.Helper()
	n, s, d := synthSet(gameByID(t, id), mame)
	c, err := arcade.NewFromFiles(n, s, d, "", core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	if c.GameID() != id {
		t.Fatalf("identified as %s, want %s", c.GameID(), id)
	}
	return c
}

// run plays frames; returns a hash of the pictures and the sound.
func run(c *arcade.Core, frames int) (uint64, []float32) {
	h := fnv.New64a()
	var audio []float32
	for f := 0; f < frames; f++ {
		c.RunFrame()
		for _, p := range c.Screen() {
			h.Write([]byte{byte(p), byte(p >> 8), byte(p >> 16)})
		}
		audio = append(audio, c.TakeAudio()...)
	}
	return h.Sum64(), audio
}

var ids = []string{"galaga", "digdug"}

func TestSyntheticRuns(t *testing.T) {
	for _, id := range ids {
		for _, mame := range []bool{false, true} {
			c := openSynth(t, id, mame)
			if c.Width() != scrH || c.Height() != scrW {
				t.Fatalf("%s: picture %dx%d", id, c.Width(), c.Height())
			}
			_, audio := run(c, 60)
			want := 60 * 48000 / FPS
			if math.Abs(float64(len(audio))-want) > 3 {
				t.Errorf("%s: %d samples for 60 frames, want %.0f", id, len(audio), want)
			}
			rms := 0.0
			for _, v := range audio[len(audio)/2:] {
				rms += float64(v) * float64(v)
			}
			if rms = math.Sqrt(rms / float64(len(audio)/2)); rms < 1e-2 {
				t.Errorf("%s: silent (rms %g)", id, rms)
			}
			colours := map[uint32]bool{}
			for _, p := range c.Screen() {
				colours[p] = true
			}
			if len(colours) < 4 {
				t.Errorf("%s: only %d colours on screen", id, len(colours))
			}
			b := c.Board().(*board)
			if b.ram[0][0x100] == 0 {
				t.Errorf("%s: the program isn't running", id)
			}
			if b.wdog > 1 {
				t.Errorf("%s: watchdog %d", id, b.wdog)
			}
		}
	}
}

func TestSaveStateAndReset(t *testing.T) {
	for _, id := range ids {
		c := openSynth(t, id, false)
		first, _ := run(c, 30)
		s, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		h1, a1 := run(c, 30)
		if err := c.LoadState(s); err != nil {
			t.Fatalf("%s: %v", id, err)
		}
		h2, a2 := run(c, 30)
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
		if h, _ := run(c, 30); h != first {
			t.Errorf("%s: reset doesn't start over", id)
		}
		s3, _ := c.SaveState()
		s4, _ := c.SaveState()
		if !bytes.Equal(s3, s4) {
			t.Errorf("%s: saving changes the state", id)
		}
	}
}

// io: the main processor's side of the 06xx.
func ioWrite(b *board, ctl uint8, data ...uint8) {
	b.write(0, 0x7100, ctl)
	for _, d := range data {
		b.write(0, 0x7000, d)
	}
}

func ioRead(b *board, ctl uint8, n int) []uint8 {
	b.write(0, 0x7100, ctl)
	out := make([]uint8, n)
	for i := range out {
		out[i] = b.read(0, 0x7000)
	}
	return out
}

func TestN51(t *testing.T) {
	c := openSynth(t, "galaga", false)
	b := c.Board().(*board)
	press := func(p0 arcade.Pad) { b.SetInput([]arcade.Pad{p0, {}}) }
	// switch mode: buttons, then joysticks, active low
	ioWrite(b, 0x01, 5)
	press(arcade.Pad{"Fire": true, "Left": true})
	if got := ioRead(b, 0x11, 2); got[0] != 0xfe || got[1]&15 != 0x07 {
		t.Errorf("switch mode read %02x", got)
	}
	// coinage 2 coins 1 credit / 1 coin 3 credits, credit mode
	press(arcade.Pad{})
	ioWrite(b, 0x01, 1, 2, 1, 1, 3, 2)
	credits := func() uint8 { return ioRead(b, 0x11, 1)[0] }
	credits()
	for i := 0; i < 2; i++ {
		press(arcade.Pad{"Coin": true})
		credits()
		press(arcade.Pad{})
		credits()
	}
	if got := credits(); got != 0x01 {
		t.Errorf("two coins: %02x credits", got)
	}
	press(arcade.Pad{"Coin 2": true})
	credits()
	press(arcade.Pad{})
	if got := credits(); got != 0x04 {
		t.Errorf("slot 2 coin: %02x credits", got)
	}
	press(arcade.Pad{"Start (1P)": true})
	credits()
	press(arcade.Pad{})
	if got := credits(); got != 0x03 {
		t.Errorf("after start: %02x credits", got)
	}
	press(arcade.Pad{"Start (1P)": true}) // starts are off until the next 2
	credits()
	press(arcade.Pad{})
	if got := credits(); got != 0x03 {
		t.Errorf("start during play took a credit: %02x", got)
	}
	// joystick as directions with the fire button's edge
	ioWrite(b, 0x01, 4)
	press(arcade.Pad{"Right": true, "Fire": true})
	r := ioRead(b, 0x11, 2)
	if r[1] != 0xc0|2 {
		t.Errorf("right + fire pressed: %02x, want %02x", r[1], 0xc0|2)
	}
	r = ioRead(b, 0x11, 2)
	if r[1] != 0xd0|2 {
		t.Errorf("right + fire held: %02x, want %02x", r[1], 0xd0|2)
	}
	press(arcade.Pad{})
	if r = ioRead(b, 0x11, 2); r[1] != 0xf8 {
		t.Errorf("centred: %02x", r[1])
	}
	// free play
	ioWrite(b, 0x01, 1, 0, 0, 0, 0, 2)
	if got := credits(); got != 0xa0 {
		t.Errorf("free play: %02x", got)
	}
	// the NMI rate: bits 5-7 of the control byte
	b.write(0, 0x7100, 0x71)
	if b.io.period() != 64<<3 || b.read(0, 0x7100) != 0x71 {
		t.Errorf("06xx control")
	}
}

func TestN53Banks(t *testing.T) {
	c := openSynth(t, "digdug", false)
	b := c.Board().(*board)
	if !c.SetDip("lives", 0xc0) || !c.SetDip("difficulty", 0x03) {
		t.Fatal("SetDip")
	}
	got := ioRead(b, 0x12, 4)
	a, bb := b.dsw()
	if got[0] != a || got[1] != bb || got[2] != a || got[3] != bb {
		t.Errorf("53xx read %02x, banks %02x %02x", got, a, bb)
	}
	if a&0xc0 != 0xc0 || bb&0x03 != 0x03 {
		t.Errorf("banks %02x %02x", a, bb)
	}
}

func TestDips(t *testing.T) {
	for _, ds := range [][]dip{galagaDips, digdugDips} {
		for _, d := range ds {
			ok := false
			for _, o := range d.Options {
				ok = ok || o.Value == d.Default
				if d.mask != 0 && uint8(o.Value)&^d.mask != 0 {
					t.Errorf("%s: option %q outside the switches", d.ID, o.Label)
				}
			}
			if !ok {
				t.Errorf("%s: default isn't an option", d.ID)
			}
		}
	}
}

func TestStarSequence(t *testing.T) {
	seen := make(map[uint16]bool, starPeriod)
	stars := 0
	for _, s := range starSeq {
		if seen[s] {
			t.Fatalf("the sequence repeats early")
		}
		seen[s] = true
		if s&0xf429 == 0xf000 {
			stars++
		}
	}
	if stars != 256 {
		t.Errorf("%d star positions", stars)
	}
	// speed: code 7 holds the stars still, code 0 moves them a pixel a frame
	var s starfield
	b := &board{}
	b.vlat = [8]uint8{1, 1, 1}
	s.step(b)
	if s.scroll != 0 {
		t.Errorf("code 7 moved the stars")
	}
	b.vlat = [8]uint8{}
	s.step(b)
	if s.scroll != 1 {
		t.Errorf("code 0 moved the stars %d", s.scroll)
	}
}

func TestWaves(t *testing.T) {
	var s wsg
	s.setWaves(nil, false)
	for w := 0; w < 8; w++ {
		lo, hi := uint8(15), uint8(0)
		for _, v := range s.waves[w*32 : w*32+32] {
			lo, hi = min(lo, v), max(hi, v)
		}
		if hi > 15 || hi-lo < 6 {
			t.Errorf("waveform %d spans %d-%d", w, lo, hi)
		}
	}
	prom := make([]uint8, 256)
	for i := range prom {
		prom[i] = 0xf0 | uint8(i)&15
	}
	s.setWaves(prom, true)
	if s.waves[17] != 1 {
		t.Errorf("PROM top bits kept")
	}
}

func TestEAROM(t *testing.T) {
	c := openSynth(t, "digdug", false)
	b := c.Board().(*board)
	// write 0x5a at address 3: set address/data, erase, then write
	b.write(0, 0xb803, 0x5a)
	b.write(0, 0xb840, 0x0e)
	b.write(0, 0xb840, 0x0a)
	b.write(0, 0xb840, 0x00)
	sram := c.SRAM()
	if len(sram) != 64 || sram[3] != 0x5a {
		t.Fatalf("EAROM %x", sram)
	}
}
