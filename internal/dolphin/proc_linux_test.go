package dolphin

import (
	"os"
	"path/filepath"
	"testing"
)

func TestRunningIn(t *testing.T) {
	proc := t.TempDir()
	mk := func(pid, exe, cmdline string) {
		d := filepath.Join(proc, pid)
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
		if exe != "" {
			if err := os.Symlink(exe, filepath.Join(d, "exe")); err != nil {
				t.Fatal(err)
			}
		}
		if err := os.WriteFile(filepath.Join(d, "cmdline"), []byte(cmdline), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	mk("12", "/opt/RetroPixels/PixelU/wii/PixelU-Wii", "/opt/RetroPixels/PixelU/wii/PixelU-Wii\x00-b\x00game.iso\x00")
	mk("13", "", `Z:\home\me\PixelU\PixelU.exe`+"\x00-g\x00x.rpx\x00") // Wine: no readable exe link
	mk("self", "/usr/bin/other", "other\x00")                          // not a pid
	mk("14", "/usr/bin/dolphin-emu", "dolphin-emu\x00")

	for image, want := range map[string]bool{
		"PixelU-Wii.exe": true,
		"PixelU-Wii":     true,
		"pixelu-wii.EXE": true,
		"PixelU.exe":     true,
		"Dolphin.exe":    false,
		"other":          false,
		"":               false,
		".exe":           false,
		"supermodel.exe": false,
	} {
		if got := runningIn(proc, image); got != want {
			t.Errorf("runningIn(%q) = %v, want %v", image, got, want)
		}
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	if !Running(filepath.Base(self)) {
		t.Errorf("the test itself (%s) isn't found running", self)
	}
}
