package dolphin

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

// A Linux Dolphin's folders: ~/.dolphin-emu, else ~/.local/share/dolphin-emu
// with its settings in ~/.config/dolphin-emu, else the Flathub one's.
func TestLinuxDolphinFolders(t *testing.T) {
	t.Setenv("RETRO_DOLPHIN_USER", "")
	t.Setenv("RETRO_OLD_DOLPHIN_USER", "")
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_DATA_HOME", "")
	t.Setenv("XDG_CONFIG_HOME", "")
	data := filepath.Join(home, ".local", "share", "dolphin-emu")
	conf := filepath.Join(home, ".config", "dolphin-emu")

	if got := userDirFor("linux", ""); got != data {
		t.Errorf("engine's folder, nothing there: %q", got)
	}
	if got := configDirFor("linux", data); got != conf {
		t.Errorf("ConfigDir(data) = %q", got)
	}
	if got := configDirFor("windows", data); got != filepath.Join(data, "Config") {
		t.Errorf("Windows ConfigDir = %q", got)
	}
	if got := configDirFor("linux", "/x/PixelU/wii/User"); got != "/x/PixelU/wii/User/Config" {
		t.Errorf("portable ConfigDir = %q", got)
	}
	if got := legacyUserDirFor("linux", ""); got != "" {
		t.Errorf("no old Dolphin: %q", got)
	}

	// The Flathub Dolphin, settings only.
	fp := filepath.Join(home, ".var", "app", "org.DolphinEmu.dolphin-emu")
	os.MkdirAll(filepath.Join(fp, "config", "dolphin-emu"), 0o755)
	if got := legacyUserDirFor("linux", ""); got != filepath.Join(fp, "data", "dolphin-emu") {
		t.Errorf("Flathub: %q", got)
	}
	if got := configDirFor("linux", filepath.Join(fp, "data", "dolphin-emu")); got != filepath.Join(fp, "config", "dolphin-emu") {
		t.Errorf("Flathub ConfigDir = %q", got)
	}
	// An installed Dolphin (settings only, then with data too) comes before it.
	os.MkdirAll(conf, 0o755)
	if got := legacyUserDirFor("linux", ""); got != data {
		t.Errorf("installed, settings only: %q", got)
	}
	// XDG_DATA_HOME and XDG_CONFIG_HOME move both.
	xd, xc := t.TempDir(), t.TempDir()
	t.Setenv("XDG_DATA_HOME", xd)
	t.Setenv("XDG_CONFIG_HOME", xc)
	os.MkdirAll(filepath.Join(xd, "dolphin-emu"), 0o755)
	if got := legacyUserDirFor("linux", ""); got != filepath.Join(xd, "dolphin-emu") {
		t.Errorf("XDG_DATA_HOME: %q", got)
	}
	if got := ConfigFile(filepath.Join(xd, "dolphin-emu"), "GFX.ini"); runtime.GOOS != "windows" && got != filepath.Join(xc, "dolphin-emu", "GFX.ini") {
		t.Errorf("ConfigFile with XDG_CONFIG_HOME: %q", got)
	}
	t.Setenv("XDG_DATA_HOME", "")
	t.Setenv("XDG_CONFIG_HOME", "")
	// The old single folder wins, for the engine's folder too.
	old := filepath.Join(home, ".dolphin-emu")
	os.MkdirAll(old, 0o755)
	if got := legacyUserDirFor("linux", ""); got != old {
		t.Errorf("~/.dolphin-emu: %q", got)
	}
	if got := userDirFor("linux", ""); got != old {
		t.Errorf("engine's folder with ~/.dolphin-emu: %q", got)
	}
	if got := configDirFor("linux", old); got != filepath.Join(old, "Config") {
		t.Errorf("ConfigDir(~/.dolphin-emu) = %q", got)
	}
	// A portable Dolphin (a Linux build with portable.txt) still comes first.
	exe := filepath.Join(t.TempDir(), "dolphin-emu")
	os.MkdirAll(filepath.Join(filepath.Dir(exe), "User"), 0o755)
	os.WriteFile(filepath.Join(filepath.Dir(exe), "portable.txt"), nil, 0o644)
	if got := legacyUserDirFor("linux", exe); got != filepath.Join(filepath.Dir(exe), "User") {
		t.Errorf("portable: %q", got)
	}
	if got := userDirFor("linux", exe); got != filepath.Join(filepath.Dir(exe), "User") {
		t.Errorf("engine portable: %q", got)
	}
}

// The desktop's own folder names, from ~/.config/user-dirs.dirs.
func TestUserFolder(t *testing.T) {
	home := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", "")
	if got := userFolderFor("linux", home, "Downloads"); got != filepath.Join(home, "Downloads") {
		t.Errorf("no user-dirs.dirs: %q", got)
	}
	os.MkdirAll(filepath.Join(home, ".config"), 0o755)
	os.WriteFile(filepath.Join(home, ".config", "user-dirs.dirs"), []byte(
		"# This file is written by xdg-user-dirs-update\n"+
			"XDG_DESKTOP_DIR=\"$HOME/Bureau\"\n"+
			"XDG_DOWNLOAD_DIR=\"$HOME/Téléchargements\"\n"+
			"XDG_DOCUMENTS_DIR=\"/data/docs\"\n"+
			"XDG_MUSIC_DIR=\"$HOME/\"\n"+
			"XDG_VIDEOS_DIR=relative\n"), 0o644)
	for name, want := range map[string]string{
		"Downloads": filepath.Join(home, "Téléchargements"),
		"Desktop":   filepath.Join(home, "Bureau"),
		"Documents": "/data/docs",
		"Music":     home,
		"Videos":    filepath.Join(home, "Videos"), // not in the right form: the default
		"Pictures":  filepath.Join(home, "Pictures"),
	} {
		if got := userFolderFor("linux", home, name); got != want {
			t.Errorf("%s = %q, want %q", name, got, want)
		}
	}
	if got := userFolderFor("windows", home, "Downloads"); got != filepath.Join(home, "Downloads") {
		t.Errorf("Windows: %q", got)
	}
	if runtime.GOOS != "windows" {
		if got := DownloadDirs(home); len(got) != 2 || got[0] != filepath.Join(home, "Téléchargements") || got[1] != filepath.Join(home, "Downloads") {
			t.Errorf("DownloadDirs = %q", got)
		}
	}
	// XDG_CONFIG_HOME moves the file.
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	if got := userFolderFor("linux", home, "Downloads"); got != filepath.Join(home, "Downloads") {
		t.Errorf("XDG_CONFIG_HOME: %q", got)
	}
	if got := DownloadDirs(""); got != nil {
		t.Errorf("no home: %q", got)
	}
}
