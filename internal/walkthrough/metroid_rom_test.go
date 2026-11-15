package walkthrough

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
	"retropixels/cores/nes"
)

// These tests play the real cartridge. Set METROID_ROM to a Metroid (USA)
// .nes file to run them (RetroPixels doesn't ship games).

const (
	bA = 1 << iota
	bB
	bSelect
	bStart
	bUp
	bDown
	bLeft
	bRight
)

type rig struct {
	t *testing.T
	c core.Core
	g *Guide
}

func newRig(t *testing.T) *rig {
	p := os.Getenv("METROID_ROM")
	if p == "" {
		t.Skip("METROID_ROM not set")
	}
	rom, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	c, err := nes.New(rom, core.Options{SampleRate: 44100, Name: filepath.Base(p)})
	if err != nil {
		t.Fatal(err)
	}
	g := New("nes", c.(core.Memory))
	if g == nil {
		t.Fatal("no guide for Metroid")
	}
	return &rig{t, c, g}
}

func (r *rig) run(n int, bits uint32) {
	for i := 0; i < n; i++ {
		r.c.SetInput(0, bits)
		r.c.RunFrame()
		r.c.TakeAudio()
	}
}

func (r *rig) start() {
	r.run(120, 0)
	r.run(5, bStart)
	r.run(60, 0)
	r.run(5, bStart)
	for i := 0; i < 600; i++ {
		r.run(1, 0)
		if r.g.Update(); r.g.State().Playing {
			return
		}
	}
	r.t.Fatal("the game didn't start")
}

func (r *rig) shot(name string) {
	dir := os.Getenv("WALK_SHOTS")
	if dir == "" {
		return
	}
	pix := append([]uint32(nil), r.c.Screen()...)
	Draw(pix, 256, 240, r.g.Update())
	img := image.NewRGBA(image.Rect(0, 0, 256, 240))
	for i, p := range pix {
		img.Set(i%256, i/256, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	f, _ := os.Create(filepath.Join(dir, name))
	png.Encode(f, img)
	f.Close()
}

func TestMetroidNotOtherGames(t *testing.T) {
	r := newRig(t)
	if New("snes", r.c.(core.Memory)) != nil {
		t.Fatal("guide for another system")
	}
}

func TestMetroidStartPointsToMorphBall(t *testing.T) {
	r := newRig(t)
	if h := r.g.Update(); h.Show {
		t.Fatalf("hint on the title screen: %+v", h)
	}
	r.start()
	h := r.g.Update()
	s := r.g.State()
	t.Logf("state %+v hint %+v", s, h)
	if !s.Playing || s.Area != Brinstar || s.RoomX != 3 || s.RoomY != 14 {
		t.Fatalf("start state %+v", s)
	}
	if h.Step != 0 || h.Title != "GET THE MORPH BALL" {
		t.Fatalf("step %d %q", h.Step, h.Title)
	}
	if math.Abs(math.Abs(h.Angle)-math.Pi) > 0.01 {
		t.Fatalf("arrow angle %v, want left", h.Angle)
	}
	r.shot("01-start.png")
}

// TestMetroidFollowTheArrow: a bot that only follows the arrow (walk its
// way, jumping now and then) gets the Morph Ball, and then the guide moves
// on to the missiles.
func TestMetroidFollowTheArrow(t *testing.T) {
	r := newRig(t)
	r.start()
	for f := 0; f < 60*60; f++ {
		h := r.g.Update()
		if h.Step > 0 {
			t.Logf("morph ball after %d frames; next: %+v", f, h)
			r.run(200, 0)
			r.shot("03-next-step.png")
			if r.g.Update().Title != "GET MISSILES" {
				t.Fatalf("next step %q", r.g.Update().Title)
			}
			return
		}
		var bits uint32
		if !math.IsNaN(h.Angle) {
			dx := math.Cos(h.Angle)
			if dx < -0.3 {
				bits |= bLeft
			} else if dx > 0.3 {
				bits |= bRight
			}
		}
		if (f/45)%4 != 3 { // hop over things (hold A for a high jump)
			bits |= bA
		}
		if f%300 == 0 {
			r.shot(fmt.Sprintf("02-on-the-way-%04d.png", f))
		}
		r.c.SetInput(0, bits)
		r.c.RunFrame()
		r.c.TakeAudio()
	}
	t.Fatalf("no morph ball after a minute; state %+v hint %+v", r.g.State(), r.g.Hint())
}

// TestMetroidPathToMissiles: from the start, the way to the first missiles
// is through Brinstar's rooms, and the path search finds it.
func TestMetroidRoomDoors(t *testing.T) {
	r := newRig(t)
	r.start()
	r.g.Update()
	tl, _ := r.g.r.prgByte(Brinstar, 0x959A)
	th, _ := r.g.r.prgByte(Brinstar, 0x959B)
	if got := int(th)<<8 | int(tl); got != 0xA314 {
		t.Fatalf("Brinstar room table at %04X, want A314", got)
	}
	s := r.g.State()
	for _, st := range metroidRoute {
		if st.Area != Brinstar {
			continue
		}
		dx, dy, n := r.g.path(s, st.X, st.Y)
		t.Logf("%-24s (%02X,%02X): first step %d,%d, %d rooms", st.Text, st.X, st.Y, dx, dy, n)
		if n == 0 {
			t.Errorf("no path to %s", st.Text)
		}
	}
	for _, e := range elevators {
		if e.From == Brinstar {
			if _, _, n := r.g.path(s, e.X, e.Y); n == 0 {
				t.Errorf("no path to the elevator to %s", AreaName(e.To))
			}
		}
	}
}
