package arcade

import (
	"archive/zip"
	"bytes"
	"strings"
	"testing"

	"retropixels/core"
)

// A tiny board for testing the framework: it draws its frame counter.
type testBoard struct {
	screen []uint32
	frame  int
	pads   []Pad
	dips   map[string]int
	rom    []byte
}

func (b *testBoard) RunFrame() {
	b.frame++
	for i := range b.screen {
		b.screen[i] = uint32(i + b.frame)
	}
	if b.pads != nil && b.pads[0]["Fire"] {
		b.screen[0] = 0xffffff
	}
}
func (b *testBoard) Screen() []uint32     { return b.screen }
func (b *testBoard) SetInput(p []Pad)     { b.pads = p }
func (b *testBoard) TakeAudio() []float32 { return nil }
func (b *testBoard) Reset()               { b.frame = 0 }
func (b *testBoard) SaveState() ([]byte, error) {
	w := &StateW{}
	w.Int(b.frame)
	return w.B, nil
}
func (b *testBoard) LoadState(s []byte) error {
	r := &StateR{B: s}
	b.frame = r.Int()
	return r.Done()
}

var testDef = &BoardDef{
	ID: "zztest", Title: "Test board", Width: 4, Height: 3, Rotate: 90, FPS: 50,
	Buttons: []string{"Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)"},
	Dips:    []Dip{{ID: "lives", Label: "Lives", Default: 1, Options: []DipOption{{"3", 1}, {"5", 2}}}},
	Games: []Game{
		{ID: "zzgame", Title: "Test Game", Files: []File{
			{Name: "zz.1", Size: 4, Role: "cpu", Offset: Next},
			{Name: "zz.2", Alt: []string{"other.2"}, Size: 4, Role: "cpu", Offset: Next},
			{Name: "zz.p", Size: 2, Role: "prom", Optional: true},
		}},
		{ID: "zzclone", Title: "Test Clone", Parent: "zzgame", Loose: true, Files: []File{
			{Name: "963a28.h27", Size: 8, Role: "cpu"},
			{Name: "zc.b", Size: 6, Role: "gfx"},
			{Name: "zc.c", Size: 6, Role: "gfx", Offset: 6},
		}},
	},
	New: func(g *Game, f Files, o Opts) (Board, error) {
		return &testBoard{screen: make([]uint32, 12), dips: o.Dips, rom: Region(g, f, "cpu", 0)}, nil
	},
}

func init() { Register(testDef) }

func makeZip(t *testing.T, files map[string][]byte, order []string) []byte {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, n := range order {
		w, err := zw.Create(n)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(files[n])
	}
	zw.Close()
	return buf.Bytes()
}

func TestFramework(t *testing.T) {
	z := makeZip(t, map[string][]byte{"dir/ZZ.1": {1, 2, 3, 4}, "other.2": {5, 6, 7, 8}}, []string{"dir/ZZ.1", "other.2"})
	cc, err := New(z, core.Options{Name: "whatever", Dips: map[string]int{"lives": 2}})
	if err != nil {
		t.Fatal(err)
	}
	c := cc.(*Core)
	if c.GameID() != "zzgame" || c.Width() != 3 || c.Height() != 4 || c.Info().Aspect != 0.75 {
		t.Fatalf("game %s %dx%d aspect %v", c.GameID(), c.Width(), c.Height(), c.Info().Aspect)
	}
	b := c.Board().(*testBoard)
	if string(b.rom) != "\x01\x02\x03\x04\x05\x06\x07\x08" || b.dips["lives"] != 2 {
		t.Fatalf("rom % x dips %v", b.rom, b.dips)
	}
	c.SetDip("lives", 1)
	if b.dips["lives"] != 1 || c.SetDip("lives", 7) {
		t.Fatal("SetDip")
	}
	c.SetInput(0, 4)
	c.RunFrame()
	// the board's (x=0,y=0) goes to the top right when turned clockwise
	if c.Screen()[2] != 0xffffff {
		t.Fatalf("rotation: %x", c.Screen())
	}
	st, _ := c.SaveState()
	c.RunFrame()
	if err := c.LoadState(st); err != nil || b.frame != 1 {
		t.Fatal("state", err, b.frame)
	}

	// named after a game: picks it and says what's missing
	_, err = New(z, core.Options{Name: "zzclone"})
	if err == nil || !strings.Contains(err.Error(), "zzclone.zip is missing these chip files for Test Clone: 963a28.h27, zc.b, zc.c.") ||
		!strings.Contains(err.Error(), "The zip holds: zz.1 (4 bytes), other.2 (4 bytes).") {
		t.Fatalf("error: %v", err)
	}
	// tolerant matching
	z2 := makeZip(t, map[string][]byte{"tmnt_a28.rom": make([]byte, 8), "b": {1, 1, 1, 1, 1, 1}, "c": {2, 2, 2, 2, 2, 2}}, []string{"tmnt_a28.rom", "c", "b"})
	cc, err = New(z2, core.Options{Name: "zzclone (1)"})
	if err != nil {
		t.Fatal(err)
	}
	if cc.(*Core).GameID() != "zzclone" {
		t.Fatal("clone")
	}
	if e, ok := Identify([]string{"a/zz.1", "zz.2"}, []int{4, 4}, "x.zip"); !ok || e.Game.ID != "zzgame" {
		t.Fatal("Identify")
	}
	if commas(1234567) != "1,234,567" || commas(12) != "12" {
		t.Fatal(commas(1234567))
	}
	if p := nameParts("963a28.h27"); !p["a28"] || !p["h27"] || !p["963a28"] {
		t.Fatal(p)
	}
}
