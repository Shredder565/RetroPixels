package main

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

// fourCore: the test core as a game for 4 players with named characters,
// keeping every player's input.
type fourCore struct {
	*testCore
	in [4]uint32
	an [4]float32
}

func (c *fourCore) Info() core.Info {
	i := c.testCore.Info()
	i.Players = 4
	return i
}
func (c *fourCore) SetInput(p int, bits uint32) { c.in[p] = bits }
func (c *fourCore) SetAnalog(p, axis int, v float32) {
	if axis == 0 {
		c.an[p] = v
	}
}
func (c *fourCore) Characters() []string {
	return []string{"Leonardo", "Michelangelo", "Donatello", "Raphael"}
}

func TestSeatRotation(t *testing.T) {
	s, h, _ := testSession(t)
	tc, _ := newTestCore([]byte{1}, core.Options{})
	fc := &fourCore{testCore: tc.(*testCore)}
	s.core, s.info = fc, fc.Info()
	s.cmap = loadMap("fake", ControlMap{"A": {Pad: []int{0}, Keys: []string{"x"}}, "B": {Pad: []int{1}, Keys: []string{}}})
	// the keyboard presses A; controller 0 presses B, controller 1 A+B and
	// pushes its stick right, controller 2 nothing
	s.keysMu.Lock()
	s.keys = map[string]bool{"x": true}
	s.keysMu.Unlock()
	h.mu.Lock()
	for i := 0; i < 3; i++ {
		h.padSt[i].Connected = true
	}
	h.padSt[0].Buttons[1] = true
	h.padSt[1].Buttons[0], h.padSt[1].Buttons[1] = true, true
	h.padSt[1].Axes[0] = 0.75
	h.mu.Unlock()
	if s.seats() != 4 {
		t.Fatalf("seats %d", s.seats())
	}
	const r = 3 | 1<<7 // A, B and Right
	cases := []struct {
		seat int
		want [4]uint32
	}{
		{0, [4]uint32{3, r, 0, 0}},
		{1, [4]uint32{0, 3, r, 0}},
		{3, [4]uint32{r, 0, 0, 3}},
		{7, [4]uint32{3, r, 0, 0}}, // out of range: player 1
	}
	for _, c := range cases {
		s.setSeat(c.seat)
		s.readInput()
		if fc.in != c.want {
			t.Errorf("seat %d: players get %v, want %v", c.seat, fc.in, c.want)
		}
	}
	s.setSeat(2)
	s.readInput()
	if fc.an[3] != 0.75 { // controller 1 plays as player 4, its stick too
		t.Errorf("analog %v", fc.an)
	}
	if got := s.seatMessage(1); got != "You are player 2 (Michelangelo)" {
		t.Errorf("message %q", got)
	}
	if got := s.seatName(3); got != "Player 4 (Raphael)" {
		t.Errorf("name %q", got)
	}

	// games for 1 or 2 players, and cores without characters, have no Player menu
	s.core, s.info = tc, tc.Info()
	if s.seats() != 0 || s.setSeat(1) != 0 {
		t.Error("a 2-player game offered seats")
	}
}

// The Konami games open as arcade games for 4 players, with their characters.
func TestKonamiGames(t *testing.T) {
	want := map[string][]string{
		"tmnt":     {"Leonardo", "Michelangelo", "Donatello", "Raphael"},
		"tmnt2":    {"Leonardo", "Michelangelo", "Donatello", "Raphael"},
		"simpsons": {"Marge", "Homer", "Bart", "Lisa"},
	}
	dir := t.TempDir()
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	for _, e := range arcade.AllGames() {
		chars, ok := want[e.Game.ID]
		if !ok {
			continue
		}
		delete(want, e.Game.ID)
		// a set of the right names and sizes (blank chips)
		var buf bytes.Buffer
		zw := zip.NewWriter(&buf)
		for _, f := range e.Game.Files {
			if !f.Optional {
				w, _ := zw.Create(f.Name)
				w.Write(make([]byte, f.Size))
			}
		}
		zw.Close()
		p := filepath.Join(dir, e.Game.ID+".zip")
		os.WriteFile(p, buf.Bytes(), 0o644)
		g, err := openGame(p)
		if err != nil || g.System != "arcade" {
			t.Fatalf("%s: %v", e.Game.ID, err)
		}
		s, err := newSession(g, &fakeHost{}, &fakeCard{}, loadMap("arcade", nil))
		if err != nil {
			t.Fatalf("%s: %v", e.Game.ID, err)
		}
		if s.seats() != 4 {
			t.Errorf("%s: %d seats", e.Game.ID, s.seats())
		}
		for i, c := range chars {
			if n := s.seatName(i); n != "Player "+string(rune('1'+i))+" ("+c+")" {
				t.Errorf("%s: %q", e.Game.ID, n)
			}
		}
		for _, b := range s.info.Buttons {
			if _, ok := s.cmap[b]; !ok {
				t.Errorf("%s: no default for %q", e.Game.ID, b)
			}
		}
		s.core.RunFrame() // (the session isn't running)
	}
	if len(want) != 0 {
		t.Errorf("not registered: %v", want)
	}
}
