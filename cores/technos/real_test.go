package technos

// Tests with the user's own ROM sets; they skip when the sets aren't there.

import (
	"encoding/binary"
	"hash/fnv"
	"image"
	"image/color"
	"image/png"
	"maps"
	"math"
	"os"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const realDir = "/mnt/project-files/roms-private/arcade-batch2/"

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

// TestRealBoot runs a set for RP_FRAMES frames (default 600) and writes a
// picture every 100 frames to RP_OUT (development runs).
func TestRealBoot(t *testing.T) {
	for _, set := range []string{"ddragon", "ddragon2"} {
		c := openReal(t, set)
		frames := 600
		if v, err := strconv.Atoi(os.Getenv("RP_FRAMES")); err == nil {
			frames = v
		}
		out := os.Getenv("RP_OUT")
		var audio []float32
		for f := 1; f <= frames; f++ {
			c.RunFrame()
			audio = append(audio, c.TakeAudio()...)
			if out != "" && f%100 == 0 {
				writePix(t, c.Screen(), c.Width(), c.Height(), out+"/"+set+"-"+strconv.Itoa(f)+".png")
			}
		}
		if out != "" {
			writeWAV(out+"/"+set+".wav", audio, 48000)
		}
	}
}

// TestRealPlay plays each set through coin, start and play (the MAME
// reference script's inputs), checks the sound comes alive, that a saved
// state replays identically and that a reset starts over.
func TestRealPlay(t *testing.T) {
	for _, set := range []string{"ddragon", "ddragon2"} {
		c := openReal(t, set)
		idx := map[string]int{}
		for i, x := range c.Info().Buttons {
			idx[x] = i
		}
		p := newLuaPlayer(1)
		hash := func(from, to int) (uint64, float64) {
			h := fnv.New64a()
			sum, n := 0.0, 0
			for f := from; f <= to; f++ {
				c.SetInput(0, p.inputs(f, idx))
				c.RunFrame()
				for _, px := range c.Screen() {
					h.Write([]byte{byte(px), byte(px >> 8), byte(px >> 16)})
				}
				for _, v := range c.TakeAudio() {
					sum += float64(v) * float64(v)
					n++
				}
			}
			return h.Sum64(), math.Sqrt(sum / float64(max(n, 1)))
		}
		boot, _ := hash(1, 300)
		hash(301, 600)
		s, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		saved := *p
		saved.held = maps.Clone(p.held)
		h1, rms := hash(601, 900)
		if rms < 0.005 {
			t.Errorf("%s: quiet while playing (rms %.4f)", set, rms)
		}
		if err := c.LoadState(s); err != nil {
			t.Fatal(err)
		}
		*p = saved
		p.held = maps.Clone(saved.held)
		if h2, _ := hash(601, 900); h2 != h1 {
			t.Errorf("%s: a loaded state plays differently", set)
		}
		c.Reset()
		*p = *newLuaPlayer(1)
		if h, _ := hash(1, 300); h != boot {
			t.Errorf("%s: reset doesn't start over", set)
		}
	}
}
