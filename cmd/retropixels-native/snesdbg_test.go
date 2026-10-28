package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"retropixels/internal/snesdbg"
)

// A SNES game with the debugger: its session is made, a capture request is
// answered with the capture's files, and the session ends with the game.
func TestSNESDebuggerCapture(t *testing.T) {
	base := t.TempDir()
	t.Setenv("RETRO_SNESDBG_DIR", base)
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	t.Setenv("RETRO_SNESDBG_EXE", filepath.Join(base, "none")) // nothing to start
	rom, err := os.ReadFile(filepath.Join("..", "..", "tools", "snes-debugger", "testroms", "bgtest-direct.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	g := &loadedGame{System: "snes", ROM: rom, Name: "bgtest"}
	s, err := newSession(g, &fakeHost{}, &fakeCard{}, loadMap("snes", nil))
	if err != nil {
		t.Fatal(err)
	}
	if err := s.attachSNESDebugger(false); err != nil {
		t.Fatal(err)
	}
	var act struct {
		Session string `json:"session"`
	}
	b, _ := os.ReadFile(filepath.Join(base, "active.json"))
	if json.Unmarshal(b, &act); act.Session == "" {
		t.Fatalf("active.json: %s", b)
	}
	if r, _ := os.ReadFile(filepath.Join(act.Session, "rom.sfc")); len(r) != len(rom) {
		t.Fatal("rom.sfc not written")
	}
	s.start()
	if err := snesdbg.Request(act.Session, "c1"); err != nil {
		t.Fatal(err)
	}
	cap := filepath.Join(act.Session, "captures", "c1")
	deadline := time.Now().Add(10 * time.Second)
	for {
		if _, err := os.Stat(filepath.Join(cap, "capture.json")); err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("no capture")
		}
		time.Sleep(50 * time.Millisecond)
	}
	for _, f := range []string{"vram.bin", "cgram.bin", "vram_src.bin", "screen.png"} {
		if _, err := os.Stat(filepath.Join(cap, f)); err != nil {
			t.Errorf("%s: %v", f, err)
		}
	}
	s.stop()
	b, _ = os.ReadFile(filepath.Join(base, "active.json"))
	json.Unmarshal(b, &act)
	if act.Session != "" {
		t.Errorf("session still active after the game stopped: %s", b)
	}
}
