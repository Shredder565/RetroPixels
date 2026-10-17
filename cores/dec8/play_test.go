package dec8

import (
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// luaPlayer presses buttons as the MAME reference runs' tools/play.lua does
// (a coin every 900 frames, then random controls from its seeded
// generator), so a native run can be compared with MAME's.
type luaPlayer struct {
	s    int64
	held map[string]bool
}

func newLuaPlayer(seed int64) *luaPlayer { return &luaPlayer{s: seed, held: map[string]bool{}} }

func (p *luaPlayer) rnd(n int64) int64 {
	const m = 2147483648
	v := p.s*1103515245 + 12345
	p.s = (v%m + m) % m
	return p.s % n
}

// inputs for native frame f (play.lua sets them when frame f-1 is done).
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
	var bits uint32
	for n, v := range p.held {
		if i, ok := idx[n]; ok && v {
			bits |= 1 << i
		}
	}
	if m := lf % 900; m >= 120 && m < 126 {
		bits |= 1 << idx["Coin"]
	}
	return bits
}

// openReal opens the user's zip through the framework.
func openReal(t *testing.T) *arcade.Core {
	t.Helper()
	data, err := os.ReadFile(romZip)
	if err != nil {
		t.Skip("no ROM set:", err)
	}
	names, sizes, contents, err := arcade.ReadZip(data)
	if err != nil {
		t.Fatal(err)
	}
	c, err := arcade.NewFromFiles(names, sizes, contents, "ghostb", core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func writePNG(path string, px []uint32, w, h int) {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range px {
		img.Set(i%w, i/w, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	if f, err := os.Create(path); err == nil {
		png.Encode(f, img)
		f.Close()
	}
}

// writeWAV writes mono 16-bit samples.
func writeWAV(path string, s []float32, rate int) {
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
		x := math.Max(-1, math.Min(1, float64(v)))
		binary.LittleEndian.PutUint16(b[44+2*i:], uint16(int16(x*32767)))
	}
	os.WriteFile(path, b, 0o644)
}

// TestPlayRun (RP_PLAY=frames:snapEvery, RP_DEVOUT=dir) plays the user's set
// as tools/run.sh plays it in MAME (seed 1) and writes snapshots and a WAV.
func TestPlayRun(t *testing.T) {
	spec := os.Getenv("RP_PLAY")
	if spec == "" {
		t.Skip("RP_PLAY not set")
	}
	var frames, snap int
	for i, p := range splitColon(spec) {
		n, _ := strconv.Atoi(p)
		if i == 0 {
			frames = n
		} else {
			snap = n
		}
	}
	out := os.Getenv("RP_DEVOUT")
	os.MkdirAll(out, 0o755)
	c := openReal(t)
	idx := map[string]int{}
	for i, b := range c.Info().Buttons {
		idx[b] = i
	}
	p := newLuaPlayer(1)
	var audio []float32
	var split []float32
	if os.Getenv("RP_SPLIT") != "" {
		c.Board().(*ghostb).onSample = func(fm, ssg, opl float64) {
			split = append(split, float32(fm), float32(ssg), float32(opl))
		}
	}
	for f := 1; f <= frames; f++ {
		c.SetInput(0, p.inputs(f, idx))
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
		if snap > 0 && f%snap == 0 {
			writePNG(filepath.Join(out, "f"+strconv.Itoa(f)+".png"), c.Screen(), c.Width(), c.Height())
		}
	}
	writeWAV(filepath.Join(out, "ghostb.wav"), audio, 48000)
	if split != nil {
		raw := make([]byte, 4*len(split))
		for i, v := range split {
			binary.LittleEndian.PutUint32(raw[4*i:], math.Float32bits(v))
		}
		os.WriteFile(filepath.Join(out, "split.f32"), raw, 0o644)
	}
}

func splitColon(s string) []string {
	var out []string
	st := 0
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == ':' {
			out = append(out, s[st:i])
			st = i + 1
		}
	}
	return out
}
