package supermodel

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestHome(t *testing.T) {
	t.Setenv("RETRO_SUPERMODEL_HOME", "")
	if got, want := Home(filepath.Join("cfg", "RetroPixels")), filepath.Join("cfg", "RetroPixels", "Supermodel"); got != want {
		t.Fatalf("Home = %q, want %q", got, want)
	}
	if filepath.Base(DefaultHome()) != "Supermodel" || filepath.Base(filepath.Dir(DefaultHome())) != "RetroPixels" {
		t.Fatalf("DefaultHome = %q", DefaultHome())
	}
	t.Setenv("RETRO_SUPERMODEL_HOME", "/x")
	if Home("cfg") != "/x" || DefaultHome() != "/x" {
		t.Fatal("RETRO_SUPERMODEL_HOME not used")
	}
}

func TestSearchFolders(t *testing.T) {
	home := t.TempDir()
	os.MkdirAll(filepath.Join(home, "Downloads"), 0o755)
	exe := filepath.Join("C", "RP", "Supermodel", ExeName)
	app := filepath.Join("C", "RP")
	got := SearchFolders([]string{"M1", "", "m1"}, exe, app, home, []string{"G1", filepath.Join(app, "ROMs")})
	want := []string{"M1", filepath.Join("C", "RP", "Supermodel", "ROMs"), filepath.Join(app, "ROMs"), "G1"} // ROMs and roms are one folder
	if len(got) < len(want) || !reflect.DeepEqual(got[:len(want)], want) {
		t.Fatalf("SearchFolders = %q, want it to start %q", got, want)
	}
	rest := got[len(want):]
	for _, w := range []string{filepath.Join(home, "Downloads"), filepath.Join(home, "ROMs")} {
		if !containsFold(rest, w) {
			t.Errorf("%q not looked in: %q", w, rest)
		}
	}
	if SearchFolders(nil, "", "", "", nil) != nil {
		t.Error("nothing to search, but folders listed")
	}
}

func TestFind(t *testing.T) {
	root := t.TempDir()
	broken := filepath.Join(root, "roms", "swtrilgya.zip")
	writeZip(t, broken, swtrilgya.Regions[0].Files)
	drive := filepath.Join(root, "drive")
	good := filepath.Join(drive, "Arcade", "swtrilgy.zip")
	writeZip(t, good, filesOf(swtrilgy.merged(), true))
	// A copy that plays on a drive beats a broken one in the usual places.
	if p, c := Find([]string{filepath.Join(root, "roms")}, []string{drive}); p != good || !c.OK {
		t.Fatalf("Find = %q %+v, want %q", p, c, good)
	}
	// No copy that plays: the broken one, with what's wrong.
	if p, c := Find([]string{filepath.Join(root, "roms")}, nil); p != broken || c.OK {
		t.Fatalf("Find broken = %q %+v", p, c)
	}
	if p, _ := Find(nil, []string{filepath.Join(root, "none")}); p != "" {
		t.Fatalf("Find nothing = %q", p)
	}
}

func TestSmoothnessNameAndLogErrors(t *testing.T) {
	if SmoothnessName("heavy") != "heavy" || SmoothnessName("") != DefaultSmoothness || SmoothnessName("x") != DefaultSmoothness {
		t.Fatal("SmoothnessName")
	}
	log := filepath.Join(t.TempDir(), "Supermodel.log")
	os.WriteFile(log, []byte("info: ok\nError: no OpenGL\nfine\nERROR again\n"), 0o644)
	if got := LogErrors(log); got != "Error: no OpenGL\nERROR again" {
		t.Fatalf("LogErrors = %q", got)
	}
	if LogErrors(log+".missing") != "" {
		t.Fatal("LogErrors of no file")
	}
}
