package model3

import (
	"math"
	"os"
	"testing"

	"retropixels/cores/arcade"
)

// TestMouseAim: the mouse aims and fires alongside the controller, and
// whichever moved last does the aiming. Set M3_ROMSET to the set's zip.
func TestMouseAim(t *testing.T) {
	zip := os.Getenv("M3_ROMSET")
	if zip == "" {
		t.Skip("M3_ROMSET not set")
	}
	data, err := os.ReadFile(zip)
	if err != nil {
		t.Fatal(err)
	}
	names, _, contents, err := arcade.ReadZip(data)
	if err != nil {
		t.Fatal(err)
	}
	files := arcade.Files{}
	for i, n := range names {
		files[arcade.BaseName(n)] = contents[i]
	}
	g := arcadeGame(Find("swtrilgya"))
	bd, err := newArcadeBoard(&g, files, arcade.Opts{SampleRate: 48000, Dips: map[string]int{"flightstick": 1, "smoothness": 0}})
	if err != nil {
		t.Fatal(err)
	}
	b := bd.(*arcadeBoard)
	in := &b.m.Inputs
	nan := float32(math.NaN())

	b.SetInput([]arcade.Pad{{}})
	b.SetMouse(-1, -1, 1) // top left, left button
	b.RunFrame()
	if in.StickX != -1 || in.StickY != 1 || !in.Trigger || in.Event {
		t.Fatalf("mouse top left: stick %v,%v trigger %v event %v", in.StickX, in.StickY, in.Trigger, in.Event)
	}
	b.SetMouse(0.5, 0.5, 2) // right button
	b.RunFrame()
	if in.StickX != 0.5 || in.StickY != -0.5 || in.Trigger || !in.Event {
		t.Fatalf("mouse: stick %v,%v trigger %v event %v", in.StickX, in.StickY, in.Trigger, in.Event)
	}
	// the controller's stick takes over...
	b.SetMouse(0.5, 0.5, 0)
	b.SetAnalog(0, 0, 1)
	b.SetAnalog(0, 1, -1)
	b.RunFrame()
	if in.StickX != 1 || in.StickY != 1 {
		t.Fatalf("stick: %v,%v", in.StickX, in.StickY)
	}
	// ...the stick let go of: the mouse, not moved, doesn't take it back
	b.SetAnalog(0, 0, 0)
	b.SetAnalog(0, 1, 0)
	b.RunFrame()
	if in.StickX == 0.5 {
		t.Fatalf("mouse took over without moving")
	}
	// the mouse moves: it aims again; off the picture it keeps its last place
	b.SetMouse(-0.25, 0, 0)
	b.RunFrame()
	b.SetMouse(nan, nan, 0)
	b.RunFrame()
	if in.StickX != -0.25 || in.StickY != 0 {
		t.Fatalf("mouse again: %v,%v", in.StickX, in.StickY)
	}
}

// TestFlightShape: the round dead zone, the square gate and the curve.
func TestFlightShape(t *testing.T) {
	if _, _, on := flightShape(0.03, 0.03); on {
		t.Fatal("inside the dead zone")
	}
	// the round gate's diagonal reaches the corner
	x, y, _ := flightShape(0.7071, 0.7071)
	if x < 0.99 || y < 0.99 {
		t.Fatalf("diagonal: %v,%v", x, y)
	}
	// fine near the centre: half way out gives well under half
	x, _, _ = flightShape(0.5, 0)
	if x <= 0 || x > 0.35 {
		t.Fatalf("half: %v", x)
	}
	if x, _, _ = flightShape(-1, 0); x != -1 {
		t.Fatalf("full left: %v", x)
	}
}
