//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMigrateOldName(t *testing.T) {
	home, conf, local := t.TempDir(), t.TempDir(), t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", conf)
	t.Setenv("LOCALAPPDATA", local)
	mk := func(p string) { os.MkdirAll(filepath.Dir(p), 0o755); os.WriteFile(p, []byte("x"), 0o644) }
	mk(filepath.Join(conf, "RetroConsole", "config.json"))
	mk(filepath.Join(local, "RetroConsole", "BrowserProfile", "Default", "IndexedDB", "db"))
	mk(filepath.Join(home, "Videos", "RetroConsole", "Mario Kart.mp4"))
	mk(filepath.Join(home, "Documents", "RetroConsole Backups", "Switch saves", "a.zip"))
	mk(filepath.Join(home, "Videos", "RetroPixels", "already-here.mp4")) // new folder exists: leave both alone
	migrateOldName()
	for _, p := range []string{
		filepath.Join(conf, "RetroPixels", "config.json"),
		filepath.Join(local, "RetroPixels", "BrowserProfile", "Default", "IndexedDB", "db"),
		filepath.Join(home, "Documents", "RetroPixels Backups", "Switch saves", "a.zip"),
		filepath.Join(home, "Videos", "RetroConsole", "Mario Kart.mp4"),
		filepath.Join(home, "Videos", "RetroPixels", "already-here.mp4"),
	} {
		if _, err := os.Stat(p); err != nil {
			t.Errorf("missing %s", p)
		}
	}
	f := filepath.Join(t.TempDir(), "Config.json")
	os.WriteFile(f+".retroconsole-original", []byte("old"), 0o644)
	if !hasOriginalBackup(f) {
		t.Error("old-name backup not recognised")
	}
}
