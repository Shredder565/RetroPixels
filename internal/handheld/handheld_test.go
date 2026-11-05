package handheld

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

func write(t *testing.T, p string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
}

// One library holds both systems, and each game names the engine for it.
func TestLibraryBothSystems(t *testing.T) {
	dir := t.TempDir()
	write(t, filepath.Join(dir, "3DS", "Mario Kart 7 (USA) (En,Fr,Es).3ds"))
	write(t, filepath.Join(dir, "DS", "0042 - New Super Mario Bros. (USA).nds"))
	write(t, filepath.Join(dir, "3DS", "Pokemon Y.cia"))
	write(t, filepath.Join(dir, "other", "readme.txt"))
	played := map[string]time.Time{}
	games := Library([]string{dir, dir}, played) // the same folder twice: listed once
	if len(games) != 3 {
		t.Fatalf("games = %+v", games)
	}
	byName := map[string]Game{}
	for _, g := range games {
		byName[g.Name] = g
	}
	mk, ds, cia := byName["Mario Kart 7"], byName["New Super Mario Bros."], byName["Pokemon Y"]
	if mk.System != System3DS || EngineFor(mk.System).ID != "pixelds" {
		t.Errorf("3DS game: %+v", mk)
	}
	if ds.System != SystemDS || EngineFor(ds.System).ID != "pixelds" || EngineChoice(ds.System, "melonds").ID != "pixelds" ||
		EngineChoice(mk.System, "azahar").ID != "pixelds" || EngineChoice(mk.System, "").ID != "pixelds" {
		t.Errorf("DS game: %+v", ds)
	}
	if !cia.Install || cia.System != System3DS {
		t.Errorf("CIA: %+v", cia)
	}
}

func TestGameArgs(t *testing.T) {
	g3 := Game{Path: "a.3ds", System: System3DS}
	gd := Game{Path: "b.nds", System: SystemDS}
	cases := []struct {
		g    Game
		full bool
		want []string
	}{
		{g3, true, []string{"-f", "a.3ds"}},
		{g3, false, []string{"-w", "a.3ds"}},
		{gd, true, []string{"-f", "b.nds"}},
		{gd, false, []string{"-w", "b.nds"}},
		{Game{Path: "c.cia", System: System3DS, Install: true}, true, []string{"-f", "c.cia"}},
	}
	for _, c := range cases {
		if got := GameArgs(PixelDS, c.g, c.full); !reflect.DeepEqual(got, c.want) {
			t.Errorf("GameArgs(%s, %v) = %q, want %q", c.g.Path, c.full, got, c.want)
		}
	}
}

// PixelDS is found in its folder next to RetroPixels, or unpacked in
// Downloads.
func TestFindExe(t *testing.T) {
	root := t.TempDir()
	app := filepath.Join(root, "RetroPixels")
	home := filepath.Join(root, "home")
	t.Setenv("XDG_CONFIG_HOME", "")
	if got := PixelDS.FindExe("", app, home); got != "" {
		t.Fatalf("found %q in an empty folder", got)
	}
	dl := filepath.Join(home, "Downloads", "PixelDS-2.34-windows", PixelDS.Exes[0])
	write(t, dl)
	if got := PixelDS.FindExe("", app, home); got != dl {
		t.Errorf("PixelDS in Downloads = %q", got)
	}
	pd := filepath.Join(app, "PixelDS", PixelDS.Exes[0])
	write(t, pd)
	if got := PixelDS.FindExe("", app, home); got != pd {
		t.Errorf("PixelDS = %q", got)
	}
	chosen := filepath.Join(root, "elsewhere", PixelDS.Exes[0])
	write(t, chosen)
	if got := PixelDS.FindExe(chosen, app, home); got != chosen {
		t.Errorf("chosen = %q", got)
	}
}

func TestDisplayName(t *testing.T) {
	for in, want := range map[string]string{
		"0123 - Mario Kart 7 (USA) (En,Fr,Es)":              "Mario Kart 7",
		"3DS - The Legend of Zelda - A Link Between Worlds": "The Legend of Zelda: A Link Between Worlds",
		"Pokemon_Black_Version":                             "Pokemon Black Version",
	} {
		if got := DisplayName(in); got != want {
			t.Errorf("DisplayName(%q) = %q, want %q", in, got, want)
		}
	}
}
