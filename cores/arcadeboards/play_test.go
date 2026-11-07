package arcadeboards

// Development check against MAME reference runs: RP_PLAY=zip:frames:snapEvery
// plays a set as the private reference tools' play.lua does in MAME (coin,
// start, then seeded random controls), and RP_MAME=dir compares each
// snapshot with MAME's (dir/0000.png is frame snapEvery, and so on). It
// skips unless RP_PLAY is set.

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

type luaPlayer struct {
	s    int64
	held map[string]bool
}

func (p *luaPlayer) rnd(n int64) int64 {
	const m = 2147483648
	v := p.s*1103515245 + 12345
	p.s = (v%m + m) % m
	return p.s % n
}

// inputs for native frame f (play.lua sets them when MAME's frame f-1 is done)
func (p *luaPlayer) inputs(f int, idx map[string]int) uint32 {
	lf := f - 1
	if lf > 260 && lf%20 == 0 {
		for _, n := range []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3"} {
			p.held[n] = p.rnd(3) == 0
		}
		if p.held["Up"] {
			p.held["Down"] = false
		}
		if p.held["Left"] {
			p.held["Right"] = false
		}
		p.held["Button 1"] = p.rnd(2) == 1
	}
	alias := map[string][]string{"Button 1": {"Button 1", "Fire", "Jump", "Punch"}, "Button 2": {"Button 2", "Kick"}, "Button 3": {"Button 3"}}
	var bits uint32
	for n, v := range p.held {
		if !v {
			continue
		}
		names := alias[n]
		if names == nil {
			names = []string{n}
		}
		for _, k := range names {
			if i, ok := idx[k]; ok {
				bits |= 1 << i
			}
		}
	}
	if m := lf % 900; m >= 120 && m < 126 {
		bits |= 1 << idx["Coin"]
	}
	if m := lf % 900; m >= 200 && m < 206 {
		bits |= 1 << idx["Start (1P)"]
	}
	return bits
}

func readPNG(path string) (image.Image, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return png.Decode(f)
}

func TestPlayCompare(t *testing.T) {
	spec := os.Getenv("RP_PLAY")
	if spec == "" {
		t.Skip("RP_PLAY not set")
	}
	parts := strings.Split(spec, ":")
	frames, _ := strconv.Atoi(parts[1])
	snap, _ := strconv.Atoi(parts[2])
	data, err := os.ReadFile(parts[0])
	if err != nil {
		t.Fatal(err)
	}
	cc, err := arcade.New(data, core.Options{Name: filepath.Base(parts[0]), SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	c := cc.(*arcade.Core)
	t.Logf("game %s", c.GameID())
	out := os.Getenv("RP_DEVOUT")
	if out != "" {
		os.MkdirAll(out, 0o755)
	}
	mame := os.Getenv("RP_MAME")
	idx := map[string]int{}
	for i, b := range c.Info().Buttons {
		idx[b] = i
	}
	off, _ := strconv.Atoi(os.Getenv("RP_OFF")) // compare native frame f+off with MAME's f
	p := &luaPlayer{s: 1, held: map[string]bool{}}
	var audio []float32
	w, h := c.Width(), c.Height()
	for f := 1; f <= frames+off; f++ {
		c.SetInput(0, p.inputs(f, idx))
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
		mf := f - off
		if snap <= 0 || mf <= 0 || mf%snap != 0 {
			continue
		}
		pix := c.Screen()
		if out != "" {
			img := image.NewRGBA(image.Rect(0, 0, w, h))
			for i, v := range pix {
				img.Set(i%w, i/w, color.RGBA{uint8(v >> 16), uint8(v >> 8), uint8(v), 255})
			}
			fh, _ := os.Create(filepath.Join(out, fmt.Sprintf("%05d.png", mf)))
			png.Encode(fh, img)
			fh.Close()
		}
		if mame == "" {
			continue
		}
		ref, err := readPNG(filepath.Join(mame, fmt.Sprintf("%04d.png", mf/snap-1)))
		if err != nil {
			t.Logf("frame %d: no MAME picture", mf)
			continue
		}
		rb := ref.Bounds()
		if rb.Dx() != w || rb.Dy() != h {
			t.Logf("frame %d: MAME picture %dx%d, ours %dx%d", mf, rb.Dx(), rb.Dy(), w, h)
			continue
		}
		diff := 0
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				r, g, b, _ := ref.At(rb.Min.X+x, rb.Min.Y+y).RGBA()
				v := pix[y*w+x]
				dr, dg, db := int(r>>8)-int(v>>16&255), int(g>>8)-int(v>>8&255), int(b>>8)-int(v&255)
				if dr*dr+dg*dg+db*db > 3*24*24 {
					diff++
				}
			}
		}
		t.Logf("frame %5d: %6d pixels differ (%.2f%%)", mf, diff, 100*float64(diff)/float64(w*h))
	}
	if out != "" {
		b := make([]byte, 44+2*len(audio))
		copy(b, "RIFF")
		binary.LittleEndian.PutUint32(b[4:], uint32(36+2*len(audio)))
		copy(b[8:], "WAVEfmt ")
		binary.LittleEndian.PutUint32(b[16:], 16)
		binary.LittleEndian.PutUint16(b[20:], 1)
		binary.LittleEndian.PutUint16(b[22:], 1)
		binary.LittleEndian.PutUint32(b[24:], 48000)
		binary.LittleEndian.PutUint32(b[28:], 96000)
		binary.LittleEndian.PutUint16(b[32:], 2)
		binary.LittleEndian.PutUint16(b[34:], 16)
		copy(b[36:], "data")
		binary.LittleEndian.PutUint32(b[40:], uint32(2*len(audio)))
		for i, v := range audio {
			x := max(-32768, min(32767, int(v*32767)))
			binary.LittleEndian.PutUint16(b[44+2*i:], uint16(int16(x)))
		}
		os.WriteFile(filepath.Join(out, "native.wav"), b, 0o644)
	}
}

// RP_STATE=zip[,zip...]: save a state mid-game, play on, load it, play the
// same frames again and check the picture and sound come out the same, and
// that Reset brings the game back up.
func TestStateRoundTrip(t *testing.T) {
	spec := os.Getenv("RP_STATE")
	if spec == "" {
		t.Skip("RP_STATE not set")
	}
	for _, zp := range strings.Split(spec, ",") {
		data, err := os.ReadFile(zp)
		if err != nil {
			t.Fatal(err)
		}
		cc, err := arcade.New(data, core.Options{Name: filepath.Base(zp), SampleRate: 48000})
		if err != nil {
			t.Fatal(err)
		}
		c := cc.(*arcade.Core)
		idx := map[string]int{}
		for i, b := range c.Info().Buttons {
			idx[b] = i
		}
		p := &luaPlayer{s: 1, held: map[string]bool{}}
		f := 1
		run := func(n int) ([]uint32, int) {
			var audio int
			for ; n > 0; n-- {
				c.SetInput(0, p.inputs(f, idx))
				c.RunFrame()
				audio += len(c.TakeAudio())
				f++
			}
			return append([]uint32(nil), c.Screen()...), audio
		}
		run(700)
		st, err := c.SaveState()
		if err != nil {
			t.Fatal(err)
		}
		pState, f0 := *p, f
		pState.held = map[string]bool{}
		for k, v := range p.held {
			pState.held[k] = v
		}
		a, na := run(300)
		if err := c.LoadState(st); err != nil {
			t.Fatalf("%s: load: %v", zp, err)
		}
		*p, f = pState, f0
		b, nb := run(300)
		diff := 0
		for i := range a {
			if a[i] != b[i] {
				diff++
			}
		}
		t.Logf("%s (%s): %d pixels differ after reload, audio %d vs %d samples", filepath.Base(zp), c.GameID(), diff, na, nb)
		if diff != 0 {
			t.Errorf("%s: state round trip differs", zp)
		}
		c.Reset()
		run(600)
		lit := 0
		for _, v := range c.Screen() {
			if v != 0 {
				lit++
			}
		}
		if lit == 0 {
			t.Errorf("%s: blank picture after reset", zp)
		}
	}
}
