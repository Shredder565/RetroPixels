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
	if mk.System != System3DS || EngineFor(mk.System).ID != "azahar" {
		t.Errorf("3DS game: %+v", mk)
	}
	if ds.System != SystemDS || EngineFor(ds.System).ID != "pixelds" || EngineChoice(ds.System, "melonds").ID != "melonds" ||
		EngineChoice(mk.System, "pixelds").ID != "pixelds" || EngineChoice(mk.System, "").ID != "azahar" {
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
		{gd, false, []string{"b.nds"}},
		{Game{Path: "c.cia", System: System3DS, Install: true}, true, []string{"-i", "c.cia"}},
	}
	for _, c := range cases {
		if got := GameArgs(EngineFor(c.g.System), c.g, c.full); !reflect.DeepEqual(got, c.want) {
			t.Errorf("GameArgs(%s, %v) = %q, want %q", c.g.Path, c.full, got, c.want)
		}
	}
	// PixelDS plays a .cia instead of installing it
	if got := GameArgs(PixelDS, Game{Path: "c.cia", System: System3DS, Install: true}, true); !reflect.DeepEqual(got, []string{"-f", "c.cia"}) {
		t.Errorf("a .cia in PixelDS: %q", got)
	}
}

// The engines are found in their folders next to RetroPixels, or unpacked
// in Downloads.
func TestFindExe(t *testing.T) {
	root := t.TempDir()
	app := filepath.Join(root, "RetroPixels")
	home := filepath.Join(root, "home")
	t.Setenv("XDG_CONFIG_HOME", "")
	if got := Azahar.FindExe("", app, home); got != "" {
		t.Fatalf("found %q in an empty folder", got)
	}
	az := filepath.Join(app, "Azahar", Azahar.Exes[0])
	write(t, az)
	if got := Azahar.FindExe("", app, home); got != az {
		t.Errorf("Azahar = %q", got)
	}
	dl := filepath.Join(home, "Downloads", "melonDS-1.1-windows-x86_64", MelonDS.Exes[0])
	write(t, dl)
	if got := MelonDS.FindExe("", app, home); got != dl {
		t.Errorf("melonDS in Downloads = %q", got)
	}
	chosen := filepath.Join(root, "elsewhere", MelonDS.Exes[0])
	write(t, chosen)
	if got := MelonDS.FindExe(chosen, app, home); got != chosen {
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
