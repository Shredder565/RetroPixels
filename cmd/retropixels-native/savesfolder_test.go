package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestSavesFolderDefaultAndChosen(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	defer useSavesFolder("")

	useSavesFolder("")
	if got, want := sramPath("nes", "Zelda"), filepath.Join(appDir(), "saves", "nes", "Zelda.sav"); got != want {
		t.Fatalf("default battery save at %q, want %q", got, want)
	}
	chosen := t.TempDir()
	useSavesFolder(chosen)
	if got, want := statePath("snes", "Mario"), filepath.Join(chosen, "snes", "Mario.state"); got != want {
		t.Fatalf("state at %q, want %q", got, want)
	}
	if got, want := slotPath("snes", "Mario", 2), filepath.Join(chosen, "snes", "Mario.slot3.state"); got != want {
		t.Fatalf("slot 3 at %q, want %q", got, want)
	}
	if got, want := firmwareDir(), filepath.Join(appDir(), "firmware"); got != want {
		t.Fatalf("firmware moved to %q, want it kept at %q", got, want)
	}
}

func TestSavesFolderKeptInSettings(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	s := loadSettings()
	if s.SavesDir != "" {
		t.Fatalf("new settings have a saves folder: %q", s.SavesDir)
	}
	s.SavesDir = `D:\Saves`
	if err := saveSettings(s); err != nil {
		t.Fatal(err)
	}
	if got := loadSettings().SavesDir; got != `D:\Saves` {
		t.Fatalf("saves folder read back as %q", got)
	}
}

func TestCopySaves(t *testing.T) {
	from, to := t.TempDir(), t.TempDir()
	write := func(p, s string) {
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(from, "nes", "Zelda.sav"), "old sram")
	write(filepath.Join(from, "snes", "Mario.state"), "state")
	write(filepath.Join(to, "nes", "Zelda.sav"), "newer sram")

	n, err := copySaves(from, to)
	if err != nil || n != 1 {
		t.Fatalf("copied %d, %v; want 1 file", n, err)
	}
	if b, _ := os.ReadFile(filepath.Join(to, "nes", "Zelda.sav")); string(b) != "newer sram" {
		t.Fatalf("a save already in the new folder was replaced: %q", b)
	}
	if b, _ := os.ReadFile(filepath.Join(to, "snes", "Mario.state")); string(b) != "state" {
		t.Fatalf("save state not copied: %q", b)
	}
	if _, err := os.Stat(filepath.Join(from, "snes", "Mario.state")); err != nil {
		t.Fatal("the original was removed")
	}

	// a new folder inside the old one isn't copied into itself
	inner := filepath.Join(from, "mine")
	if n, err := copySaves(from, inner); err != nil || n != 2 {
		t.Fatalf("into a folder inside: copied %d, %v; want 2", n, err)
	}
	if _, err := os.Stat(filepath.Join(inner, "mine")); err == nil {
		t.Fatal("copied the new folder into itself")
	}
	// no saves yet
	if n, err := copySaves(filepath.Join(from, "missing"), to); err != nil || n != 0 {
		t.Fatalf("missing folder: %d, %v", n, err)
	}
}
