package main

import (
	"os"
	"os/exec"
	"strings"
	"testing"
)

func TestAutoRecordSetting(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	if !loadSettings().autoRecord() {
		t.Fatal("games are recorded automatically unless that's turned off")
	}
	write := func(js string) {
		t.Helper()
		os.MkdirAll(appDir(), 0o755)
		if err := os.WriteFile(settingsPath(), []byte(js), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// turned off for GameCube / Wii in an older version: off for every system
	write(`{"volume": 50, "dolphin": {"autoRecord": false}}`)
	s := loadSettings()
	if s.autoRecord() || s.Dolphin.AutoRecord != nil {
		t.Fatalf("old GameCube / Wii setting off: autoRecord %v, dolphin %v", s.autoRecord(), s.Dolphin.AutoRecord)
	}
	if err := saveSettings(s); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(settingsPath())
	if strings.Count(string(b), `"autoRecord"`) != 1 || !strings.Contains(string(b), `"autoRecord": false`) {
		t.Fatalf("saved once, at the top:\n%s", b)
	}
	if loadSettings().autoRecord() {
		t.Fatal("read back")
	}
	// old setting on: stays on
	write(`{"dolphin": {"autoRecord": true}}`)
	if !loadSettings().autoRecord() {
		t.Fatal("old setting on")
	}
	// the new setting wins over a leftover old one
	write(`{"autoRecord": true, "dolphin": {"autoRecord": false}}`)
	if !loadSettings().autoRecord() {
		t.Fatal("new setting first")
	}
}

// An automatic recording of a game closed after a moment leaves nothing.
func TestShortRecordingDropped(t *testing.T) {
	ff, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Skip("no ffmpeg")
	}
	dir := t.TempDir()
	r, err := startRecorder(ff, "libx264", "balanced", dir, "Quick", 256, 240, 4.0/3, 60, sampleRate)
	if err != nil {
		t.Fatal(err)
	}
	r.minFrames = int64(minRecording * 60)
	pix := make([]uint32, 256*240)
	for i := 0; i < 60; i++ { // one second
		r.frame(pix, 256, 240)
	}
	if _, err := r.stop(); err != errRecTooShort {
		t.Fatalf("got %v", err)
	}
	if left, _ := os.ReadDir(dir); len(left) != 0 {
		t.Fatalf("left behind %v", left)
	}
}
