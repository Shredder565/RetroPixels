package pixelu

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"
)

func TestFindWiiExe(t *testing.T) {
	root := t.TempDir()
	app := filepath.Join(root, "RetroPixels")
	home := filepath.Join(root, "home")
	os.MkdirAll(app, 0o755)
	dl := filepath.Join(home, "Downloads")
	if got := FindWiiExe("", app, home); got != "" {
		t.Fatalf("nothing there: got %q", got)
	}
	// A Dolphin installed by itself, or one in a folder that isn't PixelU's, is never used.
	write(t, filepath.Join(dl, "Dolphin-x64", "Dolphin.exe"), []byte("x"))
	write(t, filepath.Join(dl, "Other", "PixelU", WiiDir, WiiExeName), []byte("x"))
	write(t, filepath.Join(app, "Dolphin", "Dolphin.exe"), []byte("x"))
	if got := FindWiiExe("", app, home); got != "" {
		t.Fatalf("not PixelU's: got %q", got)
	}

	// The Wii engine's own zip, unpacked into Downloads (the newest wins).
	old := filepath.Join(dl, "PixelU-Wii-old", "PixelU", WiiDir, WiiExeName)
	alone := filepath.Join(dl, "PixelU-Wii-2609a", "PixelU", WiiDir, WiiExeName)
	write(t, old, []byte("old"))
	write(t, alone, []byte("new"))
	os.Chtimes(old, time.Now().Add(-time.Hour), time.Now().Add(-time.Hour))
	if got := FindWiiExe("", app, home); got != alone {
		t.Fatalf("standalone in Downloads: got %q want %q", got, alone)
	}

	// A whole PixelU folder in Downloads (PixelU.exe and wii\ together) is
	// preferred: it's the PixelU that FindExe uses for Wii U games.
	full := filepath.Join(dl, "PixelU-full", "PixelU")
	write(t, filepath.Join(full, ExeName), []byte("x"))
	write(t, filepath.Join(full, WiiDir, WiiExeName), []byte("x"))
	os.Chtimes(filepath.Join(full, WiiDir, WiiExeName), time.Now().Add(-2*time.Hour), time.Now().Add(-2*time.Hour))
	if got := FindWiiExe("", app, home); got != filepath.Join(full, WiiDir, WiiExeName) {
		t.Fatalf("PixelU folder in Downloads: got %q", got)
	}

	// Next to RetroPixels: PixelU\wii, even when PixelU.exe itself is missing there.
	next := filepath.Join(app, "PixelU", WiiDir, WiiExeName)
	write(t, next, []byte("x"))
	if got := FindWiiExe("", app, home); got != next {
		t.Fatalf("next to RetroPixels: got %q", got)
	}

	// The PixelU.exe the player chose: its wii folder comes first.
	chosen := filepath.Join(root, "Emus", "PixelU", ExeName)
	write(t, chosen, []byte("x"))
	if got := FindWiiExe(chosen, app, home); got != next {
		t.Fatalf("chosen PixelU without wii\\: got %q want %q", got, next)
	}
	mine := filepath.Join(filepath.Dir(chosen), WiiDir, WiiExeName)
	write(t, mine, []byte("x"))
	if got := FindWiiExe(chosen, app, home); got != mine {
		t.Fatalf("chosen: got %q", got)
	}
	if !IsWiiExe(mine) || !IsWiiExe(`C:\x\pixelu-wii.EXE`) || IsWiiExe(`C:\Dolphin\Dolphin.exe`) || IsWiiExe(ExeName) {
		t.Error("IsWiiExe")
	}
}

func TestImportDolphinUser(t *testing.T) {
	root := t.TempDir()
	old := filepath.Join(root, "Documents", "Dolphin Emulator")
	user := filepath.Join(root, "PixelU", WiiDir, "User")
	write(t, filepath.Join(old, "Config", "GCPadNew.ini"), []byte("[GCPad1]\r\nDevice = DInput/0/Keyboard Mouse\r\n"))
	write(t, filepath.Join(old, "Config", "WiimoteNew.ini"), []byte("[Wiimote1]\r\nSource = 2\r\n"))
	write(t, filepath.Join(old, "Config", "Dolphin.ini"), []byte("[Core]\r\nSIDevice0 = 6\r\n"))
	write(t, filepath.Join(old, "Config", "Qt.ini"), []byte("x")) // not brought over
	write(t, filepath.Join(old, "GC", "Card A.USA.raw"), []byte("old card A"))
	write(t, filepath.Join(old, "GC", "USA", "Card B", "01-GMSE-save.gci"), []byte("gci"))
	write(t, filepath.Join(old, "Wii", "title", "00010000", "524d4745", "data", "banner.bin"), []byte("mario galaxy"))
	write(t, filepath.Join(old, "Wii", "title", "00010000", "52535045", "data", "save.bin"), []byte("old sports"))
	write(t, filepath.Join(old, "Wii", "shared2", "sys", "SYSCONF"), []byte("x")) // not a save

	// PixelU already has some things of its own: they stay as they are.
	write(t, filepath.Join(user, "Config", "Dolphin.ini"), []byte("mine"))
	write(t, filepath.Join(user, "GC", "Card A.USA.raw"), []byte("my card"))
	write(t, filepath.Join(user, "Wii", "title", "00010000", "52535045", "data", "other.bin"), []byte("my sports"))

	got, err := ImportDolphinUser(old, user)
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(got)
	want := []string{"Config/GCPadNew.ini", "Config/WiimoteNew.ini", "GC/USA/Card B/01-GMSE-save.gci", "Wii/title/00010000/524d4745"}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Fatalf("copied %v\nwant %v", got, want)
	}
	read := func(rel string) string {
		b, err := os.ReadFile(filepath.Join(user, filepath.FromSlash(rel)))
		if err != nil {
			return "<missing>"
		}
		return string(b)
	}
	for rel, w := range map[string]string{
		"Config/GCPadNew.ini":                         "[GCPad1]\r\nDevice = DInput/0/Keyboard Mouse\r\n",
		"Config/Dolphin.ini":                          "mine",
		"Config/Qt.ini":                               "<missing>",
		"GC/Card A.USA.raw":                           "my card",
		"GC/USA/Card B/01-GMSE-save.gci":              "gci",
		"Wii/title/00010000/524d4745/data/banner.bin": "mario galaxy",
		"Wii/title/00010000/52535045/data/other.bin":  "my sports",
		"Wii/title/00010000/52535045/data/save.bin":   "<missing>", // never mixed into PixelU's own save
		"Wii/shared2/sys/SYSCONF":                     "<missing>",
	} {
		if g := read(rel); g != w {
			t.Errorf("%s = %q want %q", rel, g, w)
		}
	}
	if !strings.Contains(read(ImportMarker), "Config/GCPadNew.ini") {
		t.Errorf("marker: %q", read(ImportMarker))
	}
	// Leftover temporary files: none.
	filepath.Walk(user, func(p string, _ os.FileInfo, _ error) error {
		if strings.Contains(p, ".retropixels-tmp") {
			t.Errorf("left behind: %s", p)
		}
		return nil
	})

	// Only once: settings changed in old Dolphin afterwards stay there.
	os.Remove(filepath.Join(user, "Config", "WiimoteNew.ini"))
	if got, err := ImportDolphinUser(old, user); err != nil || len(got) != 0 || read("Config/WiimoteNew.ini") != "<missing>" {
		t.Fatalf("second time: %v %v", got, err)
	}

	// No old Dolphin, or the same folder: nothing to do, but it's noted.
	fresh := filepath.Join(root, "fresh", "User")
	if got, err := ImportDolphinUser(filepath.Join(root, "nowhere"), fresh); err != nil || len(got) != 0 {
		t.Fatalf("no old Dolphin: %v %v", got, err)
	}
	if _, err := os.Stat(filepath.Join(fresh, ImportMarker)); err != nil {
		t.Fatal("no marker")
	}
	same := filepath.Join(root, "same")
	write(t, filepath.Join(same, "Config", "GCPadNew.ini"), []byte("x"))
	if got, err := ImportDolphinUser(same, same); err != nil || len(got) != 0 {
		t.Fatalf("same folder: %v %v", got, err)
	}
}
