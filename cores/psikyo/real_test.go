package psikyo

// Tests with the real Dragon Blaze set; they are skipped when it is not
// there (it is never part of the repository).

import (
	"bytes"
	"math"
	"os"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const realZip = "/mnt/project-files/roms-private/arcade-batch2/dragnblz.zip"

func loadReal(t *testing.T, opts core.Options) *arcade.Core {
	t.Helper()
	if testing.Short() {
		t.Skip("slow")
	}
	data, err := os.ReadFile(realZip)
	if err != nil {
		t.Skip("dragnblz.zip not available")
	}
	opts.Name = "dragnblz"
	if opts.SampleRate == 0 {
		opts.SampleRate = 48000
	}
	c, err := arcade.New(data, opts)
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

// buttons: bit numbers in the board's button list
func button(c *arcade.Core, name string) uint32 {
	for i, b := range c.Info().Buttons {
		if b == name {
			return 1 << i
		}
	}
	panic("no button " + name)
}

type realPress struct {
	btn     string
	from, n int
}

// runReal runs frames from..to (1-based frame numbers), returning the audio.
func runReal(c *arcade.Core, from, to int, presses []realPress) []float32 {
	var audio []float32
	for f := from; f <= to; f++ {
		bits := uint32(0)
		for _, p := range presses {
			if f >= p.from && f < p.from+p.n {
				bits |= button(c, p.btn)
			}
		}
		c.SetInput(0, bits)
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
	}
	return audio
}

func rms(a []float32) float64 {
	s := 0.0
	for _, v := range a {
		s += float64(v) * float64(v)
	}
	return math.Sqrt(s / float64(max(1, len(a))))
}

func colours(scr []uint32) int {
	m := map[uint32]bool{}
	for _, p := range scr {
		m[p] = true
	}
	return len(m)
}

func TestRealBootCoinStartPlay(t *testing.T) {
	c := loadReal(t, core.Options{})
	if c.Width() != ScreenH || c.Height() != ScreenW {
		t.Fatalf("screen %dx%d, want portrait", c.Width(), c.Height())
	}
	presses := []realPress{{"Coin", 900, 6}, {"Button 1", 2500, 800}, {"Left", 2800, 60}}
	for f := 1700; f <= 2400; f += 100 {
		// the title screen takes Start only at times: keep pressing
		presses = append(presses, realPress{"Start (1P)", f, 6})
	}
	quiet := runReal(c, 1, 880, presses)
	if n := colours(c.Screen()); n < 8 {
		t.Fatalf("attract screen has %d colours", n)
	}
	if rms(quiet) != 0 {
		t.Errorf("sound before a coin (rms %g)", rms(quiet))
	}
	coin := runReal(c, 881, 1000, presses)
	if rms(coin) < 0.005 {
		t.Errorf("no coin sound (rms %g)", rms(coin))
	}
	runReal(c, 1001, 2800, presses)
	play := runReal(c, 2801, 3000, presses)
	if rms(play) < 0.01 {
		t.Errorf("no music in play (rms %g)", rms(play))
	}
	a := append([]uint32(nil), c.Screen()...)
	runReal(c, 3001, 3002, presses)
	if bytes.Equal(u32bytes(a), u32bytes(c.Screen())) {
		t.Error("the picture does not move in play")
	}
	if n := colours(c.Screen()); n < 200 {
		t.Errorf("play screen has %d colours", n)
	}
}

func u32bytes(p []uint32) []byte {
	b := make([]byte, len(p)*4)
	for i, v := range p {
		b[i*4], b[i*4+1], b[i*4+2], b[i*4+3] = byte(v>>24), byte(v>>16), byte(v>>8), byte(v)
	}
	return b
}

func TestRealSaveState(t *testing.T) {
	c := loadReal(t, core.Options{})
	presses := []realPress{{"Coin", 300, 6}}
	runReal(c, 1, 400, presses)
	st, err := c.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	a1 := runReal(c, 401, 520, presses)
	s1 := append([]uint32(nil), c.Screen()...)
	if err := c.LoadState(st); err != nil {
		t.Fatal(err)
	}
	a2 := runReal(c, 401, 520, presses)
	if !bytes.Equal(u32bytes(s1), u32bytes(c.Screen())) {
		t.Error("the picture differs after loading the state")
	}
	if len(a1) != len(a2) {
		t.Fatalf("audio %d vs %d samples", len(a1), len(a2))
	}
	for i := range a1 {
		if a1[i] != a2[i] {
			t.Fatalf("audio differs at %d", i)
		}
	}
	// a fresh machine takes the state too
	d := loadReal(t, core.Options{})
	if err := d.LoadState(st); err != nil {
		t.Fatal(err)
	}
	runReal(d, 401, 520, presses)
	if !bytes.Equal(u32bytes(s1), u32bytes(d.Screen())) {
		t.Error("a fresh machine differs after loading the state")
	}
}

func TestRealResetAndEEPROM(t *testing.T) {
	c := loadReal(t, core.Options{})
	runReal(c, 1, 700, nil)
	ee := c.SRAM()
	if len(ee) != 256 {
		t.Fatalf("EEPROM %d bytes", len(ee))
	}
	c.Reset()
	runReal(c, 1, 300, nil)
	// a fresh machine with the same EEPROM boots the same way
	d := loadReal(t, core.Options{SRAM: ee})
	runReal(d, 1, 300, nil)
	if !bytes.Equal(u32bytes(c.Screen()), u32bytes(d.Screen())) {
		t.Error("after Reset the board does not boot like a fresh one")
	}
}

func TestRealDips(t *testing.T) {
	c := loadReal(t, core.Options{})
	runReal(c, 1, 900, nil)
	normal := append([]uint32(nil), c.Screen()...)
	d := loadReal(t, core.Options{Dips: map[string]int{"test": 1}})
	runReal(d, 1, 900, nil)
	if bytes.Equal(u32bytes(normal), u32bytes(d.Screen())) {
		t.Error("service mode switch changes nothing")
	}
	if !c.SetDip("region", 0) || c.SetDip("region", 7) {
		t.Error("SetDip")
	}
}
