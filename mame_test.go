//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"retropixels/internal/mame"
)

// fakeMame writes a stand-in mame.exe (a shell script) that answers the
// questions RetroPixels asks and plays only the games it's told to.
func fakeMame(t *testing.T, dir, version, plays string) string {
	exe := filepath.Join(dir, "mame.exe")
	os.MkdirAll(dir, 0o755)
	script := `#!/bin/sh
case "$1" in
  -version) echo "` + version + ` (mame0` + strings.ReplaceAll(version, "0.", "") + `)"; exit 0;;
  -showconfig) printf 'readconfig 1\nrompath roms\nhashpath hash\nsamplepath samples\nartpath artwork\nctrlrpath ctrlr\ninipath .\nfontpath .\ncheatpath cheat\nskip_gameinfo 0\njoystick 1\nkeyboardprovider auto\nmaximize 1\nwindow 0\n'; exit 0;;
  -listfull) printf 'Name: Description:\ntmnt "Teenage Mutant Ninja Turtles (World 4 Players)"\nsimpsons "The Simpsons (4 Players World, set 1)"\n'; exit 0;;
esac
echo "$@" > "` + filepath.Join(dir, "args.txt") + `"
case " ` + plays + ` " in *" $1 "*) sleep 1; exit 0;; esac
echo "$1.rom NOT FOUND (tried in $1)"
echo "Fatal error: Required files are missing, the machine cannot be run."
exit 2
`
	if err := os.WriteFile(exe, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return exe
}

func waitMameEnded(t *testing.T) {
	for i := 0; i < 100; i++ {
		runMu.Lock()
		idle := running == nil
		runMu.Unlock()
		if idle {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatal("the game never ended")
}

func TestMameVersionPerGame(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(dir, "cfg", "mame.json"))
	newExe := fakeMame(t, filepath.Join(dir, "mame0270"), "0.270", "")
	oldExe := fakeMame(t, filepath.Join(dir, "mame0139"), "0.139", "tmnt simpsons")
	roms := filepath.Join(dir, "roms")
	os.MkdirAll(roms, 0o755)
	for _, g := range []string{"tmnt", "simpsons", "notarcade"} {
		os.WriteFile(filepath.Join(roms, g+".zip"), []byte("x"), 0o644)
	}
	for _, exe := range []string{newExe, oldExe} {
		if _, err := mame.AddExe(exe); err != nil {
			t.Fatal(err)
		}
	}
	mame.Update(func(c *mame.Config) {
		c.AddFolder(roms)
		c.Default = newExe
		c.SetGame("tmnt", oldExe)
	})
	c := mame.Load()
	if len(c.Installs) != 2 || c.Installs[0].Version != "0.270" || c.Installs[1].Version != "0.139" {
		t.Fatalf("installs: %+v", c.Installs)
	}
	games := mame.Library(c.Folders, mame.AllNames(c.Installs), nil)
	if len(games) != 2 || games[0].Name != "Teenage Mutant Ninja Turtles (World 4 Players)" {
		t.Fatalf("library: %+v", games)
	}

	// tmnt plays in its own pick, 0.139
	if err := launchMameGame(filepath.Join(roms, "tmnt.zip"), ""); err != nil {
		t.Fatal(err)
	}
	runMu.Lock()
	if running == nil || running.Emu != "MAME 0.139" || running.Kind != "mame" {
		t.Errorf("running: %+v", running)
	}
	runMu.Unlock()
	if err := launchMameGame(filepath.Join(roms, "simpsons.zip"), ""); err == nil {
		t.Error("a second game started while one plays")
	}
	waitMameEnded(t)
	args, _ := os.ReadFile(filepath.Join(dir, "mame0139", "args.txt"))
	if !strings.HasPrefix(string(args), "tmnt -rompath "+roms+";") || !strings.Contains(string(args), "-skip_gameinfo") {
		t.Errorf("tmnt's command line: %s", args)
	}
	if mame.Load().Played["tmnt"].IsZero() {
		t.Error("last played not kept")
	}

	// simpsons uses the default, 0.270, whose ROM set doesn't match: the reason is kept
	if err := launchMameGame(filepath.Join(roms, "simpsons.zip"), ""); err != nil {
		t.Fatal(err)
	}
	waitMameEnded(t)
	time.Sleep(50 * time.Millisecond)
	st := mameStatus()
	p, _ := st["problem"].(map[string]any)
	if p == nil || p["version"] != "MAME 0.270" || !strings.Contains(p["text"].(string), "simpsons.rom NOT FOUND") {
		t.Errorf("problem: %v", st["problem"])
	}
	// and choosing another version for this one time works
	if err := launchMameGame(filepath.Join(roms, "simpsons.zip"), oldExe); err != nil {
		t.Fatal(err)
	}
	waitMameEnded(t)
	if p, _ := mameStatus()["problem"].(map[string]any); p != nil {
		t.Errorf("problem after a good start: %v", p)
	}
	if err := launchMameGame(filepath.Join(roms, "readme.txt"), ""); err == nil {
		t.Error("a non-ROM-set file was started")
	}
}
