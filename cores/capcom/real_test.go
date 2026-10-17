package capcom

// Tests with the user's own ROM sets; they skip when the sets aren't there.

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const realDir = "/mnt/project-files/roms-private/arcade-new/"

func openReal(t testing.TB, set string) *arcade.Core {
	t.Helper()
	data, err := os.ReadFile(realDir + set + ".zip")
	if err != nil {
		t.Skip("no ROM set " + set)
	}
	dips := map[string]int{} // RP_DIPS=id=value,... (development runs)
	for _, kv := range strings.Split(os.Getenv("RP_DIPS"), ",") {
		if k, v, ok := strings.Cut(kv, "="); ok {
			dips[k], _ = strconv.Atoi(v)
		}
	}
	c, err := arcade.New(data, core.Options{Name: set, SampleRate: 48000, Dips: dips})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

// writePNG writes the core's upright picture.
func writePNG(t testing.TB, c *arcade.Core, path string) {
	writePix(t, c.Screen(), c.Width(), c.Height(), path)
}

func writePix(t testing.TB, pix []uint32, w, h int, path string) {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix {
		img.Set(i%w, i/w, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	png.Encode(f, img)
}

func writeWAV(path string, s []float32, rate int) error {
	b := make([]byte, 44+2*len(s))
	copy(b, "RIFF")
	binary.LittleEndian.PutUint32(b[4:], uint32(36+2*len(s)))
	copy(b[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(b[16:], 16)
	binary.LittleEndian.PutUint16(b[20:], 1)
	binary.LittleEndian.PutUint16(b[22:], 1)
	binary.LittleEndian.PutUint32(b[24:], uint32(rate))
	binary.LittleEndian.PutUint32(b[28:], uint32(rate*2))
	binary.LittleEndian.PutUint16(b[32:], 2)
	binary.LittleEndian.PutUint16(b[34:], 16)
	copy(b[36:], "data")
	binary.LittleEndian.PutUint32(b[40:], uint32(2*len(s)))
	for i, v := range s {
		x := int(v * 32767)
		x = max(-32768, min(32767, x))
		binary.LittleEndian.PutUint16(b[44+2*i:], uint16(int16(x)))
	}
	return os.WriteFile(path, b, 0o644)
}

// TestRealSets boots each of the user's sets, puts a coin in, starts a game
// and plays it a little: the picture and sound must be alive, a save state
// must play on exactly as the game does, and on 1943 the protection check
// must pass (the game counts its restarts at e000; a wrong MCU answer makes
// it start over).
func TestRealSets(t *testing.T) {
	for _, set := range []string{"1942", "1943", "1943kai"} {
		t.Run(set, func(t *testing.T) {
			c := openReal(t, set)
			idx := map[string]int{}
			for i, b := range c.Info().Buttons {
				idx[b] = i
			}
			p := newLuaPlayer(7)
			var audio []float32
			f := 1
			for ; f <= 1500; f++ {
				c.SetInput(0, p.inputs(f, idx))
				c.RunFrame()
				audio = append(audio, c.TakeAudio()...)
			}
			rms := 0.0
			for _, v := range audio {
				rms += float64(v) * float64(v)
			}
			if rms = math.Sqrt(rms / float64(len(audio))); rms < 0.005 {
				t.Errorf("sound rms %g", rms)
			}
			colours := map[uint32]bool{}
			for _, px := range c.Screen() {
				colours[px] = true
			}
			if len(colours) < 8 {
				t.Errorf("%d colours on screen", len(colours))
			}
			if b, ok := c.Board().(*b1943); ok && b.ram[0] != 1 {
				t.Errorf("the game restarted %d times: protection check failed", b.ram[0]-1)
			}
			s, err := c.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			play := func() []uint32 {
				q := newLuaPlayer(9)
				for i := 0; i < 120; i++ {
					c.SetInput(0, q.inputs(f+i, idx))
					c.RunFrame()
				}
				return append([]uint32(nil), c.Screen()...)
			}
			a := play()
			if err := c.LoadState(s); err != nil {
				t.Fatal(err)
			}
			if b := play(); !slicesEqual(a, b) {
				t.Error("a loaded state plays differently")
			}
		})
	}
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

// TestDevRun (RP_DEV=set:frames:snapEvery, RP_DEVOUT=dir) plays a set as
// tools/play.lua does in MAME (coin at 120, start at 200 in every 900
// frames) and writes snapshots and a WAV, for comparing with MAME by hand.
func TestDevRun(t *testing.T) {
	spec := os.Getenv("RP_DEV")
	if spec == "" {
		t.Skip("RP_DEV not set")
	}
	parts := strings.Split(spec, ":")
	set := parts[0]
	frames, _ := strconv.Atoi(parts[1])
	snap, _ := strconv.Atoi(parts[2])
	out := os.Getenv("RP_DEVOUT")
	os.MkdirAll(out, 0o755)
	c := openReal(t, set)
	if g := os.Getenv("RP_GAINS"); g != "" {
		if b, ok := c.Board().(*b1943); ok {
			fmt.Sscanf(g, "%g,%g", &b.fmGain, &b.ssgGain)
		}
	}
	var audio []float32
	idx := map[string]int{}
	for i, b := range c.Info().Buttons {
		idx[b] = i
	}
	snaps := 0
	for f := 1; f <= frames; f++ {
		var bits uint32
		if m := (f - 1) % 900; m >= 120 && m < 126 {
			bits |= 1 << idx["Coin"]
		}
		if m := (f - 1) % 900; m >= 200 && m < 206 {
			bits |= 1 << idx["Start (1P)"]
		}
		c.SetInput(0, bits)
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
		if snap > 0 && f%snap == 0 {
			writePNG(t, c, filepath.Join(out, set+"_"+strconv.Itoa(f)+".png"))
			snaps++
		}
	}
	writeWAV(filepath.Join(out, set+".wav"), audio, 48000)
	t.Logf("%d snapshots, %d samples", snaps, len(audio))
}
