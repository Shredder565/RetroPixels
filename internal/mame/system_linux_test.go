package mame

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestIsMameExeLinux(t *testing.T) {
	for name, want := range map[string]bool{"mame": true, "mamearcade": true, "mame64": true, "mame.exe": true,
		"mame0289": false, "mame0281b_x64.exe": false, "chdman": false, "mame.ini": false, "Mame": false} {
		if IsMameExe(name) != want {
			t.Errorf("IsMameExe(%q) = %v", name, !want)
		}
	}
	if ExeName != "mame" || ExePattern != "mame*" || DownloadsAvailable {
		t.Errorf("ExeName %q, ExePattern %q, DownloadsAvailable %v", ExeName, ExePattern, DownloadsAvailable)
	}
}

func TestAddExeLinux(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(dir, "cfg", "mame.json"))
	t.Setenv("RETRO_MAME_BUNDLE", filepath.Join(dir, "none"))
	plain := filepath.Join(dir, "plain", "mame")
	win := filepath.Join(dir, "win", "mame.exe")
	good := filepath.Join(dir, "good", "mame")
	for p, body := range map[string]string{plain: "#!/bin/sh\n", win: "MZ\x90\x00", good: "#!/bin/sh\necho '0.289 (mame0289)'\n"} {
		os.MkdirAll(filepath.Dir(p), 0o755)
		mode := os.FileMode(0o755)
		if p == plain {
			mode = 0o644
		}
		if err := os.WriteFile(p, []byte(body), mode); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := AddExe(plain); err == nil || !strings.Contains(err.Error(), "executable") {
		t.Errorf("not executable: %v", err)
	}
	if _, err := AddExe(win); err == nil || !strings.Contains(err.Error(), "Windows") {
		t.Errorf("Windows mame.exe: %v", err)
	}
	in, err := AddExe(good)
	if err != nil || in.Version != "0.289" {
		t.Fatalf("AddExe = %+v, %v", in, err)
	}
	if !strings.Contains(ErrNoMAME.Error(), "mame package") {
		t.Errorf("ErrNoMAME = %v", ErrNoMAME)
	}
}

func TestSystemMameLinux(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	exe := "/usr/games/mame"
	if !isSystemExe(exe) || isSystemExe(filepath.Join(home, "MAME 0.289", "mame")) {
		t.Error("isSystemExe")
	}
	// a system MAME runs in ~/.mame (it can't write to /usr/games)
	if d := WorkDir(exe); d != filepath.Join(home, ".mame") {
		t.Errorf("WorkDir = %q", d)
	}
	if st, err := os.Stat(filepath.Join(home, ".mame")); err != nil || !st.IsDir() {
		t.Error("~/.mame wasn't made")
	}
	own := filepath.Join(home, "MAME 0.289", "mame")
	os.MkdirAll(filepath.Dir(own), 0o755)
	if d := WorkDir(own); d != filepath.Dir(own) {
		t.Errorf("WorkDir(portable) = %q", d)
	}
	rp := RomPath(exe, "/games", nil)
	if want := "/games;/usr/games/roms;" + filepath.Join(home, "mame", "roms") + ";/usr/local/share/games/mame/roms;/usr/share/games/mame/roms"; rp != want {
		t.Errorf("RomPath = %q\nwant %q", rp, want)
	}
	if rp := RomPath(own, "", nil); rp != filepath.Join(home, "MAME 0.289", "roms") {
		t.Errorf("RomPath(portable) = %q", rp)
	}
	// ~/mame/roms is guessed for a system MAME
	roms := filepath.Join(home, "mame", "roms")
	os.MkdirAll(roms, 0o755)
	os.WriteFile(filepath.Join(roms, "pacman.zip"), []byte("x"), 0o644)
	if g := GuessRomFolders([]string{exe}); len(g) != 1 || g[0] != roms {
		t.Errorf("GuessRomFolders = %q", g)
	}
}

func TestFindExesLinux(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_DATA_HOME", "")
	t.Setenv("PATH", filepath.Join(home, "bin"))
	mk := func(p string, mode os.FileMode) {
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, []byte("#!/bin/sh\n"), mode); err != nil {
			t.Fatal(err)
		}
	}
	games := filepath.Join(home, "Games", "MAME 0.289", "mame")
	onPath := filepath.Join(home, "bin", "mame")
	mk(games, 0o755)
	mk(onPath, 0o755)
	mk(filepath.Join(home, "Games", "win", "mame.exe"), 0o755)     // a Windows MAME
	mk(filepath.Join(home, "Games", "notexec", "mame"), 0o644)     // not a program
	mk(filepath.Join(home, ".cache", "x", "mame"), 0o755)          // skipped folder
	mk(filepath.Join(home, "Games", "dl", "mame0289b_x64.exe"), 0) // the Windows download
	found := FindExes(nil)
	has := map[string]bool{}
	for _, p := range found {
		has[p] = true
	}
	if !has[games] || !has[onPath] {
		t.Errorf("FindExes missed one: %q", found)
	}
	for _, p := range found {
		if strings.HasPrefix(p, home) && p != games && p != onPath {
			t.Errorf("FindExes found %q", p)
		}
	}
}
