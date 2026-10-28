package rpcs3

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

// writeTree writes files (slash paths → contents) under dir.
func writeTree(t *testing.T, dir string, files map[string]string) {
	t.Helper()
	for rel, body := range files {
		p := filepath.Join(dir, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func readOr(p string) string {
	b, err := os.ReadFile(p)
	if err != nil {
		return "<missing>"
	}
	return string(b)
}

// bundle makes a bundled RPCS3 like bundle-test.yml's (the official build's
// files plus BundleMarker) in appDir/RPCS3.
func bundle(t *testing.T, appDir, version string, files map[string]string) string {
	t.Helper()
	src := filepath.Join(appDir, "RPCS3")
	all := map[string]string{ExeName: "exe " + version, BundleMarker: "RPCS3 " + version + "\nhttps://github.com/RPCS3/rpcs3/tree/abc\n"}
	for k, v := range files {
		all[k] = v
	}
	writeTree(t, src, all)
	return src
}

func TestProtected(t *testing.T) {
	for _, rel := range []string{
		"dev_hdd0/game/TEST00001/USRDIR/EBOOT.BIN", "dev_flash/vsh/module/vsh.self", "dev_usb000/x", "DEV_HDD0/home",
		"dev_hdd1/caches", "config/config.yml", "config/input_configs/gamecontrollerdb.txt", "Config/Games.yml",
		"games.yml", "GuiConfigs/CurrentSettings.ini", "RPCS3.log", "patches/patch.yml", "savestates/a.SAVESTAT", "cache/x",
	} {
		if !Protected(rel) {
			t.Errorf("%s isn't protected", rel)
		}
	}
	for _, rel := range []string{ExeName, "Qt6Core.dll", "qt6/plugins/platforms/qwindows.dll", "GuiConfigs/Kot.qss", "Icons/ui/play.png", "GuiConfigs/compat_database.dat", BundleMarker} {
		if Protected(rel) {
			t.Errorf("%s is protected", rel)
		}
	}
}

func TestInstallBundledFirstTime(t *testing.T) {
	app := t.TempDir()
	src := bundle(t, app, "0.0.43-1", map[string]string{
		"Qt6Core.dll":                               "qt",
		"qt6/plugins/platforms/qwindows.dll":        "plugin",
		"config/input_configs/gamecontrollerdb.txt": "db v1",
	})
	home := filepath.Join(t.TempDir(), "RetroPixels", "RPCS3")
	exe, changed, err := InstallBundled(src, home)
	if err != nil || !changed || exe != filepath.Join(home, ExeName) {
		t.Fatalf("install: %q %v %v", exe, changed, err)
	}
	for rel, want := range map[string]string{ExeName: "exe 0.0.43-1", "Qt6Core.dll": "qt", "qt6/plugins/platforms/qwindows.dll": "plugin", "config/input_configs/gamecontrollerdb.txt": "db v1"} {
		if got := readOr(filepath.Join(home, filepath.FromSlash(rel))); got != want {
			t.Errorf("%s: %q", rel, got)
		}
	}
	if BundledVersion(home) != "RPCS3 0.0.43-1" {
		t.Errorf("version %q", BundledVersion(home))
	}
	// The same build again: nothing to do.
	if _, changed, err := InstallBundled(src, home); err != nil || changed {
		t.Errorf("again: %v %v", changed, err)
	}
	// The bundle itself was only read.
	if readOr(filepath.Join(src, ExeName)) != "exe 0.0.43-1" {
		t.Error("source changed")
	}
}

// The player's things in the lasting copy survive an update to a newer
// bundled build, even when the new build has files at the same paths.
func TestInstallBundledUpdateKeepsPlayerData(t *testing.T) {
	app1 := t.TempDir()
	src1 := bundle(t, app1, "0.0.43-1", map[string]string{
		"Qt6Core.dll":        "qt 1",
		"oldonly.dll":        "gone in v2",
		"GuiConfigs/Kot.qss": "theme 1",
		"config/input_configs/gamecontrollerdb.txt": "db v1",
	})
	home := filepath.Join(t.TempDir(), "RPCS3")
	if _, _, err := InstallBundled(src1, home); err != nil {
		t.Fatal(err)
	}
	player := map[string]string{
		"dev_hdd0/game/BLUS00001/USRDIR/EBOOT.BIN":             "game",
		"dev_hdd0/home/00000001/exdata/XX0000-TEST.rap":        "licence",
		"dev_hdd0/home/00000001/savedata/BLUS00001-SAVE/A.DAT": "save",
		"dev_flash/vsh/module/vsh.self":                        "firmware",
		"dev_flash/vsh/etc/version.txt":                        "release:04.9200:",
		"dev_usb000/stuff.bin":                                 "usb",
		"config/config.yml":                                    "my settings",
		"config/input_configs/global/Default.yml":              "my pad",
		"config/input_configs/gamecontrollerdb.txt":            "my db",
		"config/games.yml":                                     "BLUS00002: D:/Games/x/",
		"games.yml":                                            "old place",
		"GuiConfigs/CurrentSettings.ini":                       "gui",
		"patches/patch.yml":                                    "patches",
		"my notes.txt":                                         "notes",
		"Qt6Extra.dll":                                         "the player's own dll",
	}
	writeTree(t, home, player)

	app2 := t.TempDir()
	src2 := bundle(t, app2, "0.0.44-2", map[string]string{
		"Qt6Core.dll":        "qt 2",
		"newonly.dll":        "new in v2",
		"GuiConfigs/Kot.qss": "theme 2",
		"config/input_configs/gamecontrollerdb.txt": "db v2",
		"config/config.yml":                         "bundled default settings",
		"dev_flash/vsh/module/vsh.self":             "never shipped, but even so",
		"GuiConfigs/CurrentSettings.ini":            "bundled gui",
	})
	exe, changed, err := InstallBundled(src2, home)
	if err != nil || !changed || exe == "" {
		t.Fatalf("update: %v %v", changed, err)
	}
	for rel, want := range player {
		if got := readOr(filepath.Join(home, filepath.FromSlash(rel))); got != want {
			t.Errorf("player's %s became %q", rel, got)
		}
	}
	for rel, want := range map[string]string{ExeName: "exe 0.0.44-2", "Qt6Core.dll": "qt 2", "newonly.dll": "new in v2", "GuiConfigs/Kot.qss": "theme 2", "oldonly.dll": "<missing>"} {
		if got := readOr(filepath.Join(home, filepath.FromSlash(rel))); got != want {
			t.Errorf("%s: %q, want %q", rel, got, want)
		}
	}
	if BundledVersion(home) != "RPCS3 0.0.44-2" {
		t.Errorf("version %q", BundledVersion(home))
	}
}

// A manifest that names the player's files, or paths outside the folder,
// still never deletes them.
func TestInstallBundledManifestCantDelete(t *testing.T) {
	outside := filepath.Join(t.TempDir(), "outside.txt")
	writeTree(t, filepath.Dir(outside), map[string]string{"outside.txt": "keep"})
	home := filepath.Join(t.TempDir(), "RPCS3")
	writeTree(t, home, map[string]string{
		bundleManifest:                  "dev_hdd0/game/X/EBOOT.BIN\nconfig/config.yml\n../outside.txt\n" + filepath.ToSlash(outside) + "\nsub/../../outside.txt\n",
		BundleMarker:                    "RPCS3 old\n",
		ExeName:                         "old",
		"dev_hdd0/game/X/EBOOT.BIN":     "game",
		"config/config.yml":             "settings",
		"dev_flash/vsh/module/vsh.self": "fw",
	})
	src := bundle(t, t.TempDir(), "new", nil)
	if _, _, err := InstallBundled(src, home); err != nil {
		t.Fatal(err)
	}
	if readOr(outside) != "keep" || readOr(filepath.Join(home, "dev_hdd0", "game", "X", "EBOOT.BIN")) != "game" ||
		readOr(filepath.Join(home, "config", "config.yml")) != "settings" || readOr(filepath.Join(home, "dev_flash", "vsh", "module", "vsh.self")) != "fw" {
		t.Error("a listed file of the player's was deleted")
	}
}

func TestInstallBundledNeedsMarker(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, map[string]string{ExeName: "x"})
	if _, _, err := InstallBundled(src, filepath.Join(t.TempDir(), "h")); err == nil {
		t.Error("a folder without the marker was installed")
	}
}

// Finding order: the chosen folder, then the player's own RPCS3, then the
// bundled copy, which runs from BundledHome and never in the download.
func TestFindExeBundledOrder(t *testing.T) {
	home := filepath.Join(t.TempDir(), "RetroPixels", "RPCS3")
	t.Setenv("RETRO_RPCS3_HOME", home)
	app := t.TempDir()
	src := bundle(t, app, "0.0.43-1", nil)

	exe, source := WhichExe("", "", app)
	if exe != filepath.Join(home, ExeName) || source != SourceBundled {
		t.Fatalf("bundled: %q %q", exe, source)
	}
	c := Config{}
	st := Look(c, app)
	if st.Source != SourceBundled || st.Version != "RPCS3 0.0.43-1" || st.Root != home || !c.NeedsSearch(st) {
		t.Errorf("status: %+v", st)
	}
	c.Detected = true
	if c.NeedsSearch(st) {
		t.Error("searches again")
	}

	yours := fakeRPCS3(t, false)
	if exe, source := WhichExe("", filepath.Join(yours, ExeName), app); exe != filepath.Join(yours, ExeName) || source != SourceYours {
		t.Errorf("the player's own RPCS3 doesn't come first: %q %q", exe, source)
	}
	chosen := fakeRPCS3(t, false)
	if exe, source := WhichExe(chosen, filepath.Join(yours, ExeName), app); exe != filepath.Join(chosen, ExeName) || source != SourceChosen {
		t.Errorf("chosen: %q %q", exe, source)
	}
	// Choosing the download's own RPCS3 folder still runs the lasting copy.
	if exe, source := WhichExe(src, "", ""); exe != filepath.Join(home, ExeName) || source != SourceBundled {
		t.Errorf("chose the bundle: %q %q", exe, source)
	}
	// A remembered path inside a bundle isn't the player's own.
	if exe, _ := WhichExe("", filepath.Join(src, ExeName), ""); exe != filepath.Join(home, ExeName) {
		t.Errorf("remembered bundle: %q", exe)
	}
	// The drive search never picks a download's bundled RPCS3.
	if got := Search([]string{app}); got != "" {
		t.Errorf("search found the bundle: %q", got)
	}
	// An RPCS3 of the player's own next to RetroPixels (no marker) runs where it is.
	app2 := t.TempDir()
	writeTree(t, app2, map[string]string{"RPCS3/" + ExeName: "mine"})
	if exe, source := WhichExe("", "", app2); exe != filepath.Join(app2, "RPCS3", ExeName) || source != SourceYours {
		t.Errorf("own next to RetroPixels: %q %q", exe, source)
	}
	if runtime.GOOS != "windows" {
		// no bundle and no copy: nothing
		t.Setenv("RETRO_RPCS3_HOME", filepath.Join(t.TempDir(), "none"))
		if exe, _ := WhichExe("", "", t.TempDir()); exe != "" {
			t.Errorf("found %q", exe)
		}
	}
}
