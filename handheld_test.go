//go:build !windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// One tab: 3DS and DS games both start in PixelDS.
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
	pds := fake("PixelDS")
	t.Setenv("FAKE_ARGS", argsFile)
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	cfgMu.Lock()
	off := false // recording is checked in TestPixelDSRecordEndToEnd
	cfg = config{HandheldExe: map[string]string{"pixelds": pds}, HandheldFolders: []string{games}, AutoRecord: &off}
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
	b, _ := os.ReadFile(argsFile)
	want := "PixelDS -f " + g3 + "\nPixelDS -f " + gd
	if got := strings.TrimSpace(string(b)); got != want {
		t.Errorf("args:\n%s\nwant:\n%s", got, want)
	}
	cfgMu.Lock()
	n := len(cfg.HandheldRecent)
	cfgMu.Unlock()
	if n != 2 {
		t.Errorf("recent: %d", n)
	}
}

// Recording a DS game: RetroPixels asks PixelDS (the real one, run for
// 150 frames without a window) to save the game, and makes one .mp4.
func TestPixelDSRecordEndToEnd(t *testing.T) {
	testFFmpeg(t)
	dir := t.TempDir()
	real := filepath.Join(dir, "pixelds-real")
	if out, err := exec.Command("go", "build", "-o", real, "./cmd/pixelds").CombinedOutput(); err != nil {
		t.Fatalf("building PixelDS: %v\n%s", err, out)
	}
	exe := filepath.Join(dir, "PixelDS", "PixelDS")
	os.MkdirAll(filepath.Dir(exe), 0o755)
	os.WriteFile(exe, []byte("#!/bin/sh\nexec \""+real+"\" -frames 150 \"$@\"\n"), 0o755)
	games := filepath.Join(dir, "games")
	os.MkdirAll(games, 0o755)
	game := filepath.Join(games, "Self Test.nds")
	b, err := os.ReadFile("cores/pixelds/nds/testdata/selftest.nds")
	if err != nil {
		t.Fatal(err)
	}
	os.WriteFile(game, b, 0o644)
	t.Setenv("RETRO_REC_DIR", t.TempDir())
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	cfgMu.Lock()
	on := true
	cfg = config{HandheldExe: map[string]string{"pixelds": exe}, HandheldFolders: []string{games}, AutoRecord: &on}
	cfgMu.Unlock()
	recMu.Lock()
	recLastErr, recLastMsg = "", ""
	recMu.Unlock()
	if err := launchHandheldGame(game); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	if st := recStatus(); st["error"] != "" || !strings.HasPrefix(st["message"].(string), "Saved Self Test") {
		t.Fatalf("status: %v", st)
	}
	files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4"))
	if len(files) != 1 {
		t.Fatalf("want 1 recording, got %v", files)
	}
}
