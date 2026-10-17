package technos

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// luaPlayer presses buttons exactly as the MAME reference runs'
// tools/play.lua does (coin, start, then random controls from its seeded
// generator), so a native run can be compared frame for frame.
type luaPlayer struct {
	s    int64
	held map[string]bool
}

func newLuaPlayer(seed int64) *luaPlayer { return &luaPlayer{s: seed, held: map[string]bool{}} }

func (p *luaPlayer) rnd(n int64) int64 {
	const m = 2147483648
	v := p.s*1103515245 + 12345 // wraps as Lua's 64-bit integers do
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
	if m := lf % 900; m >= 200 && m < 206 {
		bits |= 1 << idx["Start (1P)"]
	}
	return bits
}

// TestPlayRun (RP_PLAY=set:frames:snapEvery, RP_DEVOUT=dir) plays a set as
// tools/run.sh plays it in MAME (seed 1) and writes snapshots and a WAV.
func TestPlayRun(t *testing.T) {
	spec := os.Getenv("RP_PLAY")
	if spec == "" {
		t.Skip("RP_PLAY not set")
	}
	parts := strings.Split(spec, ":")
	set := parts[0]
	frames, _ := strconv.Atoi(parts[1])
	snap, _ := strconv.Atoi(parts[2])
	out := os.Getenv("RP_DEVOUT")
	os.MkdirAll(out, 0o755)
	c := openReal(t, set)
	if v := os.Getenv("RP_GAINS"); v != "" {
		a, b, _ := strings.Cut(v, ",")
		ddFMGain, _ = strconv.ParseFloat(a, 64)
		ddPCMGain, _ = strconv.ParseFloat(b, 64)
	}
	idx := map[string]int{}
	for i, b := range c.Info().Buttons {
		idx[b] = i
	}
	p := newLuaPlayer(1)
	var audio []float32
	for f := 1; f <= frames; f++ {
		c.SetInput(0, p.inputs(f, idx))
		c.RunFrame()
		audio = append(audio, c.TakeAudio()...)
		if snap > 0 && f%snap == 0 {
			writePix(t, c.Screen(), c.Width(), c.Height(), filepath.Join(out, set+"_"+strconv.Itoa(f)+".png"))
		}
	}
	writeWAV(filepath.Join(out, set+".wav"), audio, 48000)
}
