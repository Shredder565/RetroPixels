//go:build !windows

package pixelu

import (
	"os"
	"path/filepath"
	"testing"
)

// Linux: the programs have no .exe, and a download in the desktop's own
// Downloads folder (named in user-dirs.dirs) is found.
func TestFindExeLinux(t *testing.T) {
	if ExeName != "PixelU" || WiiExeName != "PixelU-Wii" {
		t.Fatalf("names %q %q", ExeName, WiiExeName)
	}
	root := t.TempDir()
	home := filepath.Join(root, "home")
	t.Setenv("XDG_CONFIG_HOME", "")
	write(t, filepath.Join(home, ".config", "user-dirs.dirs"), []byte("XDG_DOWNLOAD_DIR=\"$HOME/Téléchargements\"\n"))
	dl := filepath.Join(home, "Téléchargements", "PixelU-2610a", "PixelU")
	// The PixelU folder itself (a folder named like the program) isn't it.
	os.MkdirAll(dl, 0o755)
	if got := FindExe("", "", home); got != "" {
		t.Fatalf("folder taken for the program: %q", got)
	}
	write(t, filepath.Join(dl, ExeName), []byte("x"))
	write(t, filepath.Join(dl, WiiDir, WiiExeName), []byte("x"))
	if got := FindExe("", "", home); got != filepath.Join(dl, ExeName) {
		t.Errorf("FindExe = %q", got)
	}
	if got := FindWiiExe("", "", home); got != filepath.Join(dl, WiiDir, WiiExeName) {
		t.Errorf("FindWiiExe = %q", got)
	}
	// ~/Downloads is still looked in.
	other := filepath.Join(home, "Downloads", "PixelU-Wii-x", "PixelU", WiiDir, WiiExeName)
	write(t, other, []byte("x"))
	os.Remove(filepath.Join(dl, WiiDir, WiiExeName))
	os.Remove(filepath.Join(dl, ExeName))
	if got := FindWiiExe("", "", home); got != other {
		t.Errorf("FindWiiExe in ~/Downloads = %q", got)
	}
	if !IsWiiExe("/opt/PixelU/wii/PixelU-Wii") || IsWiiExe("/opt/PixelU/PixelU") {
		t.Error("IsWiiExe")
	}
}

// An installed Linux Dolphin keeps its settings in ~/.config/dolphin-emu
// and its saves in ~/.local/share/dolphin-emu: both are brought over.
func TestImportLinuxDolphin(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_DATA_HOME", "")
	t.Setenv("XDG_CONFIG_HOME", "")
	data := filepath.Join(home, ".local", "share", "dolphin-emu")
	conf := filepath.Join(home, ".config", "dolphin-emu")
	write(t, filepath.Join(conf, "GFX.ini"), []byte("[Settings]\n"))
	user := filepath.Join(t.TempDir(), "User")
	// Settings only: the data folder isn't there yet.
	got, err := ImportDolphinUser(data, user)
	if err != nil || len(got) != 1 || got[0] != "Config/GFX.ini" {
		t.Fatalf("settings only: %q %v", got, err)
	}
	user2 := filepath.Join(t.TempDir(), "User")
	write(t, filepath.Join(data, "GC", "USA", "Card A", "x.gci"), []byte("card"))
	got, err = ImportDolphinUser(data, user2)
	if err != nil || len(got) != 2 {
		t.Fatalf("with data: %q %v", got, err)
	}
	if _, err := os.Stat(filepath.Join(user2, "Config", "GFX.ini")); err != nil {
		t.Error(err)
	}
	if _, err := os.Stat(filepath.Join(user2, "GC", "USA", "Card A", "x.gci")); err != nil {
		t.Error(err)
	}
}
