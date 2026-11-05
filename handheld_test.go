//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// One tab: a 3DS game starts in Azahar, a DS game in PixelDS (or melonDS,
// when PixelDS isn't there or the player picks it).
func TestHandheldLaunchPicksEngine(t *testing.T) {
	dir := t.TempDir()
	games := filepath.Join(dir, "games")
	os.MkdirAll(games, 0o755)
	g3 := filepath.Join(games, "Mario Kart 7 (USA).3ds")
	gd := filepath.Join(games, "Mario Kart DS (USA).nds")
	os.WriteFile(g3, []byte("x"), 0o644)
	os.WriteFile(gd, []byte("x"), 0o644)
	argsFile := filepath.Join(dir, "args")
	fake := func(name string) string {
		p := filepath.Join(dir, name, name)
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("#!/bin/sh\necho \""+name+" $*\" >> \"$FAKE_ARGS\"\n"), 0o755)
		return p
	}
	az, mel, pds := fake("azahar"), fake("melonDS"), fake("PixelDS")
	t.Setenv("FAKE_ARGS", argsFile)
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	cfgMu.Lock()
	cfg = config{HandheldExe: map[string]string{"azahar": az, "melonds": mel, "pixelds": pds}, HandheldFolders: []string{games}}
	cfgMu.Unlock()

	if lib := handheldLibrary(); len(lib) != 2 {
		t.Fatalf("library: %+v", lib)
	}
	if err := launchHandheldGame(g3); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	if err := launchHandheldGame(gd); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	cfgMu.Lock()
	cfg.HandheldDSEngine = "melonds"
	cfgMu.Unlock()
	if err := launchHandheldGame(gd); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	b, _ := os.ReadFile(argsFile)
	want := "azahar -f " + g3 + "\nPixelDS -f " + gd + "\nmelonDS -f " + gd
	if got := strings.TrimSpace(string(b)); got != want {
		t.Errorf("args:\n%s\nwant:\n%s", got, want)
	}
	cfgMu.Lock()
	n := len(cfg.HandheldRecent)
	cfgMu.Unlock()
	if n != 2 { // the DS game twice is one recent game
		t.Errorf("recent: %d", n)
	}
}
