package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestWinFilter(t *testing.T) {
	names, pats := winFilter("MAME ROM sets (*.zip;*.7z)|*.zip;*.7z|Programs (*.exe)|*.exe|broken")
	if !reflect.DeepEqual(names, []string{"MAME ROM sets (*.zip;*.7z)", "Programs (*.exe)"}) {
		t.Errorf("names %q", names)
	}
	if !reflect.DeepEqual(pats, [][]string{{"*.zip", "*.7z"}, {"*.exe"}}) {
		t.Errorf("patterns %q", pats)
	}
	if n, p := winFilter(""); n != nil || p != nil {
		t.Errorf("empty filter gave %q %q", n, p)
	}
}

func TestPickArgs(t *testing.T) {
	dir := t.TempDir()
	z := pickArgs("zenity", "Choose", "ZIP files (*.zip)|*.zip;*.7z", dir)
	want := []string{"--file-selection", "--title=Choose", "--filename=" + dir + "/",
		"--file-filter=ZIP files (*.zip) | *.zip *.7z", "--file-filter=All files | *"}
	if !reflect.DeepEqual(z, want) {
		t.Errorf("zenity: %q", z)
	}
	// A start folder that doesn't exist is left out.
	if z := pickArgs("zenity", "Choose", "", filepath.Join(dir, "gone")); strings.Contains(strings.Join(z, " "), "--filename") {
		t.Errorf("zenity with a missing folder: %q", z)
	}
	k := pickArgs("kdialog", "Choose", "ZIP files (*.zip)|*.zip", dir)
	if !reflect.DeepEqual(k, []string{"--title", "Choose", "--getopenfilename", dir, "*.zip|ZIP files (*.zip)\n*|All files"}) {
		t.Errorf("kdialog: %q", k)
	}
	if f := folderArgs("zenity", "Folder", dir); !reflect.DeepEqual(f, []string{"--file-selection", "--directory", "--title=Folder", "--filename=" + dir + "/"}) {
		t.Errorf("zenity folder: %q", f)
	}
	if f := folderArgs("kdialog", "Folder", dir); !reflect.DeepEqual(f, []string{"--title", "Folder", "--getexistingdirectory", dir}) {
		t.Errorf("kdialog folder: %q", f)
	}
}

func TestPickEnv(t *testing.T) {
	t.Setenv("RETRO_TEST_PICK", "/games/a.zip")
	t.Setenv("RETRO_TEST_PICK_DIR", "")
	if p, err := pickFile("t", "", ""); err != nil || p != "/games/a.zip" {
		t.Errorf("pickFile = %q, %v", p, err)
	}
	if p, err := pickFolder("t", ""); err != nil || p != "" {
		t.Errorf("pickFolder (cancelled) = %q, %v", p, err)
	}
}

func TestDataDir(t *testing.T) {
	t.Setenv("XDG_DATA_HOME", "/xdg/data")
	if d := dataDir(); d != "/xdg/data/RetroPixels" {
		t.Errorf("dataDir = %q", d)
	}
	t.Setenv("XDG_DATA_HOME", "relative") // not absolute: ignored, as the XDG spec says
	home, _ := os.UserHomeDir()
	if d := dataDir(); d != filepath.Join(home, ".local", "share", "RetroPixels") {
		t.Errorf("dataDir = %q", d)
	}
}

func TestOpenFolderMissing(t *testing.T) {
	if err := openFolder(filepath.Join(t.TempDir(), "gone")); err == nil {
		t.Error("no error for a missing folder")
	}
}
