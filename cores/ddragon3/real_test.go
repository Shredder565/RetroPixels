package ddragon3

// Tests with the user's real set (skipped when it isn't there).

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const realZip = "/mnt/project-files/roms-private/arcade-batch2/ddragon3.zip"

func realData(t *testing.T) []byte {
	t.Helper()
	data, err := os.ReadFile(realZip)
	if err != nil {
		t.Skip("ddragon3.zip not available")
	}
	return data
}

// loadRealFiles reads the set's files.
func loadRealFiles(t *testing.T) arcade.Files {
	names, _, contents, err := arcade.ReadZip(realData(t))
	if err != nil {
		t.Fatal(err)
	}
	files := arcade.Files{}
	for i, n := range names {
		files[arcade.BaseName(n)] = contents[i]
	}
	return files
}

func loadReal(t *testing.T) *arcade.Core {
	t.Helper()
	c, err := arcade.New(realData(t), core.Options{Name: "ddragon3.zip", SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

// press holds a button for frames [from, from+n).
type press struct {
	btn     string
	from, n int
}

func buttonBits(c *arcade.Core, presses []press, f int) uint32 {
	v := uint32(0)
	for _, p := range presses {
		if f >= p.from && f < p.from+p.n {
			for i, b := range c.Info().Buttons {
				if b == p.btn {
					v |= 1 << i
				}
			}
		}
	}
	return v
}

// run plays frames 1..n (presses on player 1), saving a picture every
// `every` frames into out (if set) and collecting the sound.
func run(t *testing.T, c *arcade.Core, n int, presses []press, out string, every int, audio *[]float32) {
	t.Helper()
	for f := 1; f <= n; f++ {
		c.SetInput(0, buttonBits(c, presses, f))
		c.RunFrame()
		a := c.TakeAudio()
		if audio != nil {
			*audio = append(*audio, a...)
		}
		if out != "" && every > 0 && f%every == 0 {
			if err := writePNG(filepath.Join(out, "n"+itoa5(f)+".png"), c.Width(), c.Height(), c.Screen()); err != nil {
				t.Fatal(err)
			}
		}
	}
}

func itoa5(n int) string {
	s := []byte("00000")
	for i := 4; i >= 0 && n > 0; i-- {
		s[i] = byte('0' + n%10)
		n /= 10
	}
	return string(s)
}

// TestRealBoots runs the attract mode, puts a coin in and starts a game.
func TestRealBoots(t *testing.T) {
	c := loadReal(t)
	if c.GameID() != "ddragon3" {
		t.Fatalf("identified as %s", c.GameID())
	}
	out := os.Getenv("DD3_OUT")
	var audio []float32
	run(t, c, 1200, []press{{"Coin", 900, 6}, {"Start (1P)", 1000, 6}}, out, 100, &audio)
	if len(audio) == 0 {
		t.Fatal("no sound")
	}
}

// TestRealSound writes the sound of the first DD3_FRAMES frames of the
// attract mode (a coin at DD3_COIN if set) to DD3_WAV, for comparing with
// MAME's recording.
func TestRealSound(t *testing.T) {
	path := os.Getenv("DD3_WAV")
	if path == "" {
		t.Skip("DD3_WAV not set")
	}
	if os.Getenv("DD3_ONLY") == "fm" {
		okiLevel = 0
	} else if os.Getenv("DD3_ONLY") == "oki" {
		fmLevel = 0
	}
	c := loadReal(t)
	n, coin := 3000, 0
	if s := os.Getenv("DD3_FRAMES"); s != "" {
		n, _ = strconv.Atoi(s)
	}
	if s := os.Getenv("DD3_COIN"); s != "" {
		coin, _ = strconv.Atoi(s)
	}
	var presses []press
	if coin > 0 {
		presses = []press{{"Coin", coin + 1, 6}, {"Start (1P)", coin + 101, 6}}
	}
	var audio []float32
	if p := os.Getenv("DD3_SCHED"); p != "" {
		presses = schedPresses(t, p)
	}
	run(t, c, n, presses, "", 0, &audio)
	if err := writeWAV(path, audio, 48000); err != nil {
		t.Fatal(err)
	}
}

func writeWAV(path string, s []float32, rate int) error {
	b := make([]byte, 44+len(s)*2)
	copy(b, "RIFF")
	binary.LittleEndian.PutUint32(b[4:], uint32(36+len(s)*2))
	copy(b[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(b[16:], 16)
	binary.LittleEndian.PutUint16(b[20:], 1)
	binary.LittleEndian.PutUint16(b[22:], 1)
	binary.LittleEndian.PutUint32(b[24:], uint32(rate))
	binary.LittleEndian.PutUint32(b[28:], uint32(rate*2))
	binary.LittleEndian.PutUint16(b[32:], 2)
	binary.LittleEndian.PutUint16(b[34:], 16)
	copy(b[36:], "data")
	binary.LittleEndian.PutUint32(b[40:], uint32(len(s)*2))
	for i, v := range s {
		x := int(v * 32767)
		x = max(-32768, min(32767, x))
		binary.LittleEndian.PutUint16(b[44+i*2:], uint16(int16(x)))
	}
	return os.WriteFile(path, b, 0o644)
}
