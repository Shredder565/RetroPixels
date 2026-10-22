package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"retropixels/internal/mame"
)

func fakeBuild(t *testing.T, info string) string {
	b := filepath.Join(t.TempDir(), "stage")
	for p, body := range map[string]string{"mame": "#!/bin/sh\necho '0.289 (mame0289)'\n", "hash/neogeo.xml": "<x/>", BuildInfo: info} {
		f := filepath.Join(b, p)
		os.MkdirAll(filepath.Dir(f), 0o755)
		mode := os.FileMode(0o644)
		if p == "mame" {
			mode = 0o755
		}
		if err := os.WriteFile(f, []byte(body), mode); err != nil {
			t.Fatal(err)
		}
	}
	return b
}

func TestPlaceLinux(t *testing.T) {
	build := fakeBuild(t, "version=0.289\ntag=mame0289\nsubtarget=arcade\nsource_sha256="+strings.Repeat("ab", 32)+"\n")
	dir := filepath.Join(t.TempDir(), "MAME")
	if _, err := placeLinux(build, dir, "0.288"); err == nil {
		t.Error("a build of another version was placed")
	}
	info, err := placeLinux(build, dir, "")
	if err != nil || info["tag"] != "mame0289" {
		t.Fatalf("placeLinux = %v, %v", info, err)
	}
	exe := filepath.Join(dir, "MAME 0.289", "mame")
	st, err := os.Stat(exe)
	if err != nil || st.Mode().Perm()&0o111 == 0 {
		t.Fatalf("mame: %v %v", st, err)
	}
	if _, err := os.Stat(filepath.Join(dir, "MAME 0.289", "hash", "neogeo.xml")); err != nil {
		t.Error(err)
	}
	mark, _ := os.ReadFile(filepath.Join(dir, "MAME 0.289", mame.BuiltInMarker))
	if !strings.HasPrefix(string(mark), "version=0.289\n") || !strings.Contains(string(mark), "SUBTARGET=arcade") {
		t.Errorf("marker:\n%s", mark)
	}
	if _, err := os.Stat(filepath.Join(dir, "roms", "PUT-ROM-SETS-HERE.txt")); err != nil {
		t.Error(err)
	}
	// RetroPixels lists it as the built-in MAME
	if runtime.GOOS != "windows" {
		t.Setenv("RETRO_MAME_BUNDLE", dir)
		got := mame.BuiltIn()
		if len(got) != 1 || got[0].Exe != exe || got[0].Version != "0.289" {
			t.Errorf("BuiltIn = %+v", got)
		}
	}
}

func TestPlaceLinuxRefuses(t *testing.T) {
	dir := t.TempDir()
	for _, info := range []string{"", "version=0.289\ntag=mame0288\n", "version=0.289\n"} {
		if _, err := placeLinux(fakeBuild(t, info), dir, ""); err == nil {
			t.Errorf("placed a build with info %q", info)
		}
	}
	b := fakeBuild(t, "version=0.289\ntag=mame0289\n")
	os.Chmod(filepath.Join(b, "mame"), 0o644)
	if _, err := placeLinux(b, dir, ""); err == nil {
		t.Error("placed a mame that isn't executable")
	}
	b = fakeBuild(t, "version=0.289\ntag=mame0289\n")
	os.Symlink("/etc/passwd", filepath.Join(b, "link"))
	if _, err := placeLinux(b, dir, ""); err == nil {
		t.Error("placed a build with a symlink in it")
	}
}
