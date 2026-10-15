package n64

import (
	"bytes"
	"encoding/binary"
	"os"
	"testing"

	"retropixels/core"
	machine "retropixels/n64"
)

func newTest(t *testing.T, rom []byte, sram []byte) *N64 {
	t.Helper()
	c, err := New(rom, core.Options{SampleRate: 48000, SRAM: sram})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*N64)
}

// square reads back one of the pad test's squares: row 0-4, b 0-39.
func square(c *N64, row, b int) bool {
	x, y := 8+b*7+2, 16+row*10+2
	return c.Screen()[y*c.Width()+x] == 0xffffff
}

// the 32 bits the pad test shows for a controller (buttons, stick X, stick Y) and its rx byte
func shown(c *N64, row int) (word uint32, rx byte) {
	for b := 0; b < 32; b++ {
		if square(c, row, b) {
			word |= 1 << (31 - b)
		}
	}
	for b := 0; b < 8; b++ {
		if square(c, row, 32+b) {
			rx |= 1 << (7 - b)
		}
	}
	return
}

func TestControls(t *testing.T) {
	c := newTest(t, padTestROM("NTTE", false), nil)
	if c.Width() != 320 || c.Height() != 240 || c.FPS() != 60 {
		t.Fatalf("%dx%d at %v", c.Width(), c.Height(), c.FPS())
	}
	for i := 0; i < 3; i++ {
		c.RunFrame()
	}
	if c.Width() != 320 || c.Height() != 240 {
		t.Fatalf("picture %dx%d", c.Width(), c.Height())
	}
	type pad struct {
		bits uint32
		ax   [4]float32
	}
	check := func(conn int, pads [4]pad, want [4]uint32) {
		t.Helper()
		c.SetConnected(conn)
		for p, s := range pads {
			c.SetInput(p, s.bits)
			for a, v := range s.ax {
				c.SetAnalog(p, a, v)
			}
		}
		c.RunFrame()
		c.RunFrame() // the program shows what it read the frame before
		for p := 0; p < 4; p++ {
			w, rx := shown(c, p)
			present := p == 0 || p < conn
			if present != (rx&0x80 == 0) {
				t.Errorf("controller %d: rx %02x with %d connected", p+1, rx, conn)
			}
			if present && w != want[p] {
				t.Errorf("controller %d: N64 read %08x, want %08x", p+1, w, want[p])
			}
		}
	}
	w := func(btn uint16, x, y int8) uint32 { return uint32(btn)<<16 | uint32(uint8(x))<<8 | uint32(uint8(y)) }
	// every button on its own, on controller 1
	for i, bit := range bits {
		check(1, [4]pad{{bits: 1 << i}}, [4]uint32{w(bit, 0, 0)})
	}
	// stick keys push the stick all the way; up is +Y on the N64
	check(1, [4]pad{{bits: bStickUp | bStickLeft}}, [4]uint32{w(0, -80, 80)})
	check(1, [4]pad{{bits: bStickDown | bStickRight}}, [4]uint32{w(0, 80, -80)})
	// the left stick, with its dead zone; the stick keys win over it
	check(2, [4]pad{{ax: [4]float32{0.1, -0.1}}, {ax: [4]float32{1, -1}}}, [4]uint32{w(0, 0, 0), w(0, 80, 80)})
	check(2, [4]pad{{ax: [4]float32{-0.575, 0.575}}, {bits: bStickLeft, ax: [4]float32{1, 0.5}}}, [4]uint32{w(0, -40, -40), w(0, -80, -33)})
	// the right stick is the C buttons
	check(4, [4]pad{{}, {}, {ax: [4]float32{0, 0, -0.6, -0.6}}, {ax: [4]float32{0, 0, 0.6, 0.6}}},
		[4]uint32{0, 0, w(machine.BtnCLeft|machine.BtnCUp, 0, 0), w(machine.BtnCRight|machine.BtnCDown, 0, 0)})
	check(3, [4]pad{{bits: 1 << 3}, {bits: 1 << 0}, {bits: 1<<8 | 1<<9}}, [4]uint32{w(machine.BtnStart, 0, 0), w(machine.BtnA, 0, 0), w(machine.BtnL|machine.BtnR, 0, 0)})
	// unplugged controllers read as missing
	check(0, [4]pad{{bits: 1}}, [4]uint32{w(machine.BtnA, 0, 0)})
}

func TestStickValue(t *testing.T) {
	for _, c := range []struct {
		v    float64
		want int
	}{{0, 0}, {0.149, 0}, {-0.149, 0}, {0.15, 0}, {1, 80}, {-1, -80}, {2, 80}, {0.575, 40}, {-0.575, -40}, {0.5, 33}, {-0.5, -33}} {
		if got := stickValue(c.v); got != c.want {
			t.Errorf("stickValue(%v) = %d, want %d", c.v, got, c.want)
		}
	}
}

func TestSaves(t *testing.T) {
	for _, flash := range []bool{false, true} {
		id := "NTTE"
		if flash {
			id = "NB7P"
		}
		rom := padTestROM(id, flash)
		c := newTest(t, rom, nil)
		blank := append([]byte(nil), c.SRAM()...)
		if string(blank[:4]) != "RP64" || len(blank) != 6+512*(1+3*int(b2i(flash)))+2+0x20000+4*0x8000 {
			t.Fatalf("battery save %q, %d bytes", blank[:4], len(blank))
		}
		if flash && c.FPS() != 50 {
			t.Errorf("PAL game at %v fps", c.FPS())
		}
		c.SetInput(0, 1) // A
		for i := 0; i < 300; i++ {
			c.RunFrame()
		}
		s := c.SRAM()
		if bytes.Equal(s, blank) {
			t.Fatal("the save didn't change")
		}
		eeprom := s[6 : 6+binary.BigEndian.Uint16(s[4:])]
		if bytes.Count(eeprom, []byte{0xff}) == len(eeprom) {
			t.Error("nothing in the EEPROM")
		}
		area := s[6+len(eeprom)+2:][:0x20000]
		if bytes.Count(area, []byte{0xff}) == len(area) {
			t.Error("nothing in the SRAM / FlashRAM")
		}
		if s[6+len(eeprom)] != b2i(flash) {
			t.Errorf("FlashRAM flag %d", s[6+len(eeprom)])
		}
		pak := s[6+len(eeprom)+2+0x20000:][:0x8000]
		if bytes.Equal(pak[0x600:0x800], blank[6+len(eeprom)+2+0x20000+0x600:][:0x200]) {
			t.Error("nothing in the Controller Pak")
		}
		// starting again from the save picks it all up
		c2 := newTest(t, rom, s)
		if !bytes.Equal(c2.SRAM(), s) {
			t.Error("the save didn't load")
		}
		// as does a plain EEPROM file from another emulator
		c3 := newTest(t, rom, eeprom)
		if !bytes.Equal(c3.SRAM()[6:6+len(eeprom)], eeprom) {
			t.Error("the .eep didn't load")
		}
	}
}

func b2i(b bool) byte {
	if b {
		return 1
	}
	return 0
}

func runFrames(c *N64, n int, seed uint32) (pics [][]uint32, snd []float32) {
	for i := 0; i < n; i++ {
		seed = seed*1103515245 + 12345
		c.SetInput(0, seed>>8&0x3ffff)
		c.SetAnalog(0, 0, float32(int8(seed>>16))/100)
		c.RunFrame()
		pics = append(pics, append([]uint32(nil), c.Screen()...))
		snd = append(snd, c.TakeAudio()...)
	}
	return
}

func TestSaveStateRoundTrip(t *testing.T) {
	roms := [][]byte{padTestROM("NTTE", false)}
	if p := os.Getenv("N64_SOUND_ROM"); p != "" {
		if b, err := os.ReadFile(p); err == nil {
			roms = append(roms, b)
		}
	} else if b, err := os.ReadFile("/tmp/claude-0/roms/pl/Sound/PCM/LongShot/STEREO/16BIT/LongShotSTEREO16BIT.N64"); err == nil {
		roms = append(roms, b)
	}
	for _, rom := range roms {
		c := newTest(t, rom, nil)
		runFrames(c, 100, 1)
		st, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		wantPics, wantSnd := runFrames(c, 60, 2)
		wantSave := append([]byte(nil), c.SRAM()...)
		if err := c.LoadState(st); err != nil {
			t.Fatal(err)
		}
		pics, snd := runFrames(c, 60, 2)
		for i := range pics {
			if !equalU32(pics[i], wantPics[i]) {
				t.Fatalf("frame %d after loading differs", i)
			}
		}
		if len(snd) != len(wantSnd) {
			t.Fatalf("sound after loading: %d samples, want %d", len(snd), len(wantSnd))
		}
		for i := range snd {
			if snd[i] != wantSnd[i] {
				t.Fatalf("sound sample %d after loading differs", i)
			}
		}
		if !bytes.Equal(c.SRAM(), wantSave) {
			t.Error("battery save after loading differs")
		}
		// the state is the web player's ("RPN1" + deflate) with the sound position after it
		if string(st[:4]) != "RPN1" || string(st[len(st)-4:]) != "RPNw" {
			t.Errorf("state %q...%q", st[:4], st[len(st)-4:])
		}
		// a web player state (no sound position) loads too
		if err := c.LoadState(st[:len(st)-4-tailLen]); err != nil {
			t.Error(err)
		}
		if err := c.LoadState([]byte("RPN1junk")); err == nil {
			t.Error("loaded junk")
		}
	}
}

func equalU32(a, b []uint32) bool {
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

func TestNotAGame(t *testing.T) {
	if _, err := New(make([]byte, 0x2000), core.Options{SampleRate: 48000}); err == nil {
		t.Error("started a file of zeros")
	}
}

func TestReset(t *testing.T) {
	c := newTest(t, padTestROM("NTTE", false), nil)
	c.SetInput(0, 1)
	for i := 0; i < 10; i++ {
		c.RunFrame()
	}
	c.Reset()
	if len(c.TakeAudio()) != 0 {
		t.Error("sound left over from before the reset")
	}
	c.RunFrame()
	c.RunFrame()
	c.RunFrame()
	if w, _ := shown(c, 0); w != uint32(machine.BtnA)<<16 {
		t.Errorf("after reset the game reads %08x", w)
	}
}

// One frame of a busy game: the N64NICCC demo when it's there, else the pad test.
func BenchmarkFrame(b *testing.B) {
	rom, err := os.ReadFile("/tmp/claude-0/roms/pl/N64NICCC/N64NICCC.N64")
	if err != nil {
		rom = padTestROM("NTTE", false)
	}
	c, _ := New(rom, core.Options{SampleRate: 48000})
	for i := 0; i < 120; i++ {
		c.RunFrame()
		c.TakeAudio()
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		c.RunFrame()
		c.TakeAudio()
	}
}

func BenchmarkSaveState(b *testing.B) {
	c, _ := New(padTestROM("NTTE", false), core.Options{SampleRate: 48000})
	c.RunFrame()
	for i := 0; i < b.N; i++ {
		st, _ := c.SaveState()
		if err := c.LoadState(st); err != nil {
			b.Fatal(err)
		}
	}
}
