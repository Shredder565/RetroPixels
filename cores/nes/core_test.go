package nes

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"math/rand"
	"os"
	"sort"
	"testing"

	"retropixels/core"
)

// The tests below run a small test program written for these tests (not a
// game): copied to RAM, it hammers random cartridge and sound addresses with
// writes and reads (reads feed its random generator, so any difference in
// banking changes everything after), while its NMI handler scrambles VRAM,
// scroll, PPU control and sprites. Every 8 KB PRG bank holds the same code and
// vectors plus its own random data. It works with any board.
var (
	exRAMCode, _   = hex.DecodeString("20d30385062903f00cc901f01220d30309804c260320d303291f09404c260320d303291f0960850320d3038502a506291cf00a20d303a00091024c0003a000b102450085000501d002e6004c0003489848ad154020d30309e0850520d303850420d303a000910420d3030980850520d303850420d303a000910468a86840488a489848a9028d1440ad022020d303293f8d062020d3038d0620a21020d3038d0720cad0f7ad022020d3038d052020d3038d052020d30309808d002020d30309188d012020d303aa20d3039d000268a868aa6840a008a5000a260190024939850088d0f260")
	exResetCode, _ = hex.DecodeString("78d8a2ff9aa9408d1740a9008d00208d01202c02202c022010fb2c022010fba200bd00f09d0003bd00f19d0004bd00e09d0002e8d0eba95a8500a9c38501a9808d0020a91e8d0120584c0003")
)

const exNMI, exIRQ, exReset = 0x37e, 0x34e, 0xf800

// exerciserROM builds an iNES file for the test program on board mapper.
func exerciserROM(mapper, prgK, chrK int, seed int64) []byte {
	r := rand.New(rand.NewSource(seed))
	var b bytes.Buffer
	b.Write([]byte{'N', 'E', 'S', 0x1a, byte(prgK / 16), byte(chrK / 8), byte(mapper&15)<<4 | 3, byte(mapper & 0xf0), 0, 0, 0, 0, 0, 0, 0, 0})
	for i := 0; i < prgK/8; i++ {
		bank := make([]byte, 0x2000)
		r.Read(bank)
		bank[0] = byte(i)
		copy(bank[0x1000:], exRAMCode)
		copy(bank[0x1800:], exResetCode)
		binary.LittleEndian.PutUint16(bank[0x1ffa:], exNMI)
		binary.LittleEndian.PutUint16(bank[0x1ffc:], exReset)
		binary.LittleEndian.PutUint16(bank[0x1ffe:], exIRQ)
		b.Write(bank)
	}
	chr := make([]byte, chrK*1024)
	r.Read(chr)
	b.Write(chr)
	return b.Bytes()
}

func mapperList() []int {
	var l []int
	for m := range mappers {
		l = append(l, m)
	}
	sort.Ints(l)
	return l
}

type frameRec struct {
	hash  uint32
	audio []float32
}

func runFrames(n *NES, in *scriptInput, from, count int) []frameRec {
	var out []frameRec
	for f := from; f < from+count; f++ {
		p1, p2 := in.next(f)
		n.SetInput(0, p1)
		n.SetInput(1, p2)
		n.RunFrame()
		out = append(out, frameRec{screenHash(n.Screen()), n.TakeAudio()})
	}
	return out
}

func sameFrames(t *testing.T, what string, a, b []frameRec) {
	t.Helper()
	for i := range a {
		if a[i].hash != b[i].hash {
			t.Fatalf("%s: frame %d picture differs", what, i)
		}
		if len(a[i].audio) != len(b[i].audio) {
			t.Fatalf("%s: frame %d audio length differs", what, i)
		}
		for j := range a[i].audio {
			if a[i].audio[j] != b[i].audio[j] {
				t.Fatalf("%s: frame %d sample %d differs", what, i, j)
			}
		}
	}
}

// TestSaveStateRoundTrip saves mid-game on every board, then checks that the
// game continues exactly as an uninterrupted run, both after loading into the
// same machine and into a freshly opened one.
func TestSaveStateRoundTrip(t *testing.T) {
	for _, m := range mapperList() {
		for _, chrK := range []int{128, 0} {
			if chrK == 0 && (m%4 != 0 || testing.Short()) {
				continue
			}
			rom := exerciserROM(m, 256, chrK, int64(m))
			c, err := New(rom, core.Options{SampleRate: 44100})
			if err != nil {
				t.Fatalf("mapper %d: %v", m, err)
			}
			n := c.(*NES)
			in := &scriptInput{seed: 99}
			runFrames(n, in, 0, 60)
			st, err := n.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			in2 := *in
			want := runFrames(n, in, 60, 30)
			if err := n.LoadState(st); err != nil {
				t.Fatalf("mapper %d: load: %v", m, err)
			}
			in3 := in2
			sameFrames(t, "same machine", want, runFrames(n, &in2, 60, 30))

			c2, _ := New(rom, core.Options{SampleRate: 44100})
			n2 := c2.(*NES)
			if err := n2.LoadState(st); err != nil {
				t.Fatalf("mapper %d: load into new machine: %v", m, err)
			}
			sameFrames(t, "new machine", want, runFrames(n2, &in3, 60, 30))
		}
	}
}

func TestLoadStateRejects(t *testing.T) {
	a, _ := New(exerciserROM(4, 128, 128, 1), core.Options{})
	b, _ := New(exerciserROM(4, 128, 128, 2), core.Options{})
	a.RunFrame()
	st, _ := a.SaveState()
	if b.LoadState(st) == nil {
		t.Fatal("loaded another game's state")
	}
	if a.LoadState(st[:len(st)-10]) == nil {
		t.Fatal("loaded a truncated state")
	}
	before, _ := a.SaveState()
	if a.LoadState(append(st[:40:40], make([]byte, 100)...)) == nil {
		t.Fatal("loaded a damaged state")
	}
	after, _ := a.SaveState()
	if !bytes.Equal(before, after) {
		t.Fatal("a refused state changed the game")
	}
}

func TestBatterySRAM(t *testing.T) {
	rom := exerciserROM(1, 128, 0, 5) // header says battery
	c, _ := New(rom, core.Options{})
	for i := 0; i < 30; i++ {
		c.RunFrame()
	}
	s := c.SRAM()
	if len(s) != 0x2000 {
		t.Fatalf("SRAM length %d", len(s))
	}
	c2, _ := New(rom, core.Options{SRAM: s})
	if !bytes.Equal(c2.SRAM(), s) {
		t.Fatal("SRAM not restored")
	}
	rom[6] &^= 2 // no battery
	c3, _ := New(rom, core.Options{})
	if c3.SRAM() != nil {
		t.Fatal("SRAM without battery")
	}
}

func TestErrors(t *testing.T) {
	for _, tc := range []struct {
		rom  []byte
		want string
	}{
		{[]byte("hello"), "This isn't an NES game file (.nes)"},
		{append([]byte("FDS\x1a"), make([]byte, 64)...), "This is a Famicom Disk System game."},
		{exerciserROM(251, 32, 8, 1), "This NES game uses cartridge hardware RetroPixels doesn't support yet (mapper 251)."},
	} {
		_, err := New(tc.rom, core.Options{})
		if err == nil || !bytes.HasPrefix([]byte(err.Error()), []byte(tc.want)) {
			t.Errorf("got %v, want %q", err, tc.want)
		}
	}
}

func TestInfo(t *testing.T) {
	c, _ := New(exerciserROM(0, 32, 8, 1), core.Options{})
	i := c.Info()
	if i.System != "nes" || len(i.Buttons) != 8 || i.Buttons[0] != "A" || i.Buttons[7] != "Right" || i.Players != 2 || i.Stereo {
		t.Fatalf("info %+v", i)
	}
	if c.Width() != 256 || c.Height() != 240 || len(c.Screen()) != 256*240 || c.FPS() != 60.0988 {
		t.Fatal("picture size")
	}
}

// BenchmarkFrame times one emulated frame (set NES_BENCH_ROM to use a real game).
func BenchmarkFrame(b *testing.B) {
	rom := exerciserROM(4, 256, 128, 1)
	if p := os.Getenv("NES_BENCH_ROM"); p != "" {
		var err error
		if rom, err = os.ReadFile(p); err != nil {
			b.Fatal(err)
		}
	}
	c, err := New(rom, core.Options{SampleRate: 48000})
	if err != nil {
		b.Fatal(err)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		c.SetInput(0, uint32(i>>3)&0xf7)
		c.RunFrame()
		c.TakeAudio()
	}
	b.ReportMetric(float64(b.N)/b.Elapsed().Seconds(), "frames/s")
}

// TestRandomProgramsDontCrash runs every board with PRG full of random bytes
// (so the CPU executes garbage, touching every register) and odd ROM sizes.
func TestRandomProgramsDontCrash(t *testing.T) {
	r := rand.New(rand.NewSource(7))
	for _, m := range mapperList() {
		for k := 0; k < 3; k++ {
			prg := []int{8, 24, 40, 128, 512}[r.Intn(5)]
			chr := []int{0, 8, 16, 56, 128}[r.Intn(5)]
			rom := make([]byte, 16+prg*1024+chr*1024)
			r.Read(rom)
			copy(rom, []byte{'N', 'E', 'S', 0x1a})
			rom[4], rom[5] = byte(prg/16), byte(chr/8)
			if prg%16 != 0 { // NES 2.0 exponent-multiplier size: 8 KB × odd
				rom[4] = byte(13<<2 | (prg/8-1)/2)
				rom[9] = 0x0f
				rom[7] = 0x08
			}
			rom[6] = byte(m&15)<<4 | rom[6]&0x0b
			rom[7] = rom[7]&0x0c | byte(m&0xf0)
			if rom[7]&0x0c == 0x08 {
				rom[8] = byte(r.Intn(16)) << 4
			} else {
				rom[7] &^= 0x0c
				copy(rom[8:16], make([]byte, 8))
			}
			c, err := New(rom, core.Options{SampleRate: 22050})
			if err != nil {
				continue
			}
			for f := 0; f < 20; f++ {
				c.SetInput(0, uint32(r.Intn(256)))
				c.RunFrame()
				c.TakeAudio()
				if f == 10 {
					c.Reset()
				}
			}
			st, _ := c.SaveState()
			if err := c.LoadState(st); err != nil {
				t.Fatalf("mapper %d: %v", m, err)
			}
		}
	}
}
