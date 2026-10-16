//go:build !windows

package mame

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestParseVerify(t *testing.T) {
	out := `galaga      : prom-4.2n (256 bytes) - NOT FOUND
romset galaga is bad
romset gradius [nemesis] is good
romset kangaroo is best available
romset oldone correct
No matching systems found for 'nosuch'
2 romsets found, 1 were OK.`
	got := ParseVerify(out)
	want := map[string]string{"galaga": Bad, "gradius": Good, "kangaroo": Good, "oldone": Good}
	if len(got) != len(want) {
		t.Fatalf("got %v", got)
	}
	for k, v := range want {
		if got[k] != v {
			t.Errorf("%s: %q, want %q", k, got[k], v)
		}
	}
}

// fakeVerifier: a stand-in MAME whose -verifyroms passes only the sets in good.
func fakeVerifier(t *testing.T, dir, version, good string) string {
	os.MkdirAll(dir, 0o755)
	exe := filepath.Join(dir, "mame.exe")
	script := `#!/bin/sh
[ "$1" = -version ] && { echo "` + version + `"; exit 0; }
shift 2; shift
echo run >> "` + filepath.Join(dir, "runs.txt") + `"
for s in "$@"; do
  case " ` + good + ` " in *" $s "*) echo "romset $s is good";; *) [ "$s" = nosuch ] && echo "No matching systems found for '$s'" || echo "romset $s is bad";; esac
done
`
	if err := os.WriteFile(exe, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return exe
}

func TestCheckGamesAndBest(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(dir, "cfg", "mame.json"))
	t.Setenv("RETRO_MAME_BUNDLE", filepath.Join(dir, "none"))
	newExe := fakeVerifier(t, filepath.Join(dir, "MAME 0.281"), "0.281", "pacman")
	oldExe := fakeVerifier(t, filepath.Join(dir, "MAME 0.139"), "0.139", "galaga pacman")
	roms := filepath.Join(dir, "roms")
	os.MkdirAll(roms, 0o755)
	var games []Game
	for _, s := range []string{"galaga", "pacman", "nosuch"} {
		p := filepath.Join(roms, s+".zip")
		os.WriteFile(p, []byte("x"), 0o644)
		games = append(games, Game{Short: s, Path: p})
	}
	c, _ := Update(func(c *Config) {
		c.Add(Install{Exe: newExe, Version: "0.281"})
		c.Add(Install{Exe: oldExe, Version: "0.139"})
		c.Default = newExe
		c.AddFolder(roms)
	})
	res := CheckGames(c, games)
	if res["galaga"][newExe] != Bad || res["galaga"][oldExe] != Good || res["pacman"][newExe] != Good || res["nosuch"][oldExe] != Bad {
		t.Fatalf("results: %v", res)
	}
	// galaga goes to the version that plays it; pacman stays on the default
	if in, _ := c.Best(games[0]); in.Exe != oldExe {
		t.Errorf("galaga plays in %s", in.Exe)
	}
	if in, _ := c.Best(games[1]); in.Exe != newExe {
		t.Errorf("pacman plays in %s", in.Exe)
	}
	// the game's own pick wins
	c.SetGame("galaga", newExe)
	if in, _ := c.Best(games[0]); in.Exe != newExe {
		t.Errorf("own pick ignored: %s", in.Exe)
	}
	// answers are kept: asking again doesn't run MAME
	checkMu.Lock()
	checkCache = nil // as a new start of the program
	checkMu.Unlock()
	CheckGames(c, games)
	if b, _ := os.ReadFile(filepath.Join(dir, "MAME 0.281", "runs.txt")); strings.Count(string(b), "run") != 1 {
		t.Errorf("MAME asked again: %q", b)
	}
}

func TestBuiltIn(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(dir, "cfg", "mame.json"))
	bundle := filepath.Join(dir, "RetroPixels-2.17.0", "MAME")
	t.Setenv("RETRO_MAME_BUNDLE", bundle)
	mine := fakeVerifier(t, filepath.Join(dir, "my mame"), "0.250", "")
	Update(func(c *Config) { c.Add(Install{Exe: mine, Version: "0.250"}) })

	// no built-in MAME: nothing changes
	if c := Load(); len(c.Installs) != 1 || c.Default != mine {
		t.Fatalf("%+v", c)
	}
	exe := fakeVerifier(t, filepath.Join(bundle, "MAME 0.281"), "0.281", "")
	os.WriteFile(filepath.Join(bundle, "MAME 0.281", BuiltInMarker), []byte("version=0.281\n"), 0o644)
	c := Load()
	in, ok := c.Find(exe)
	if !ok || !in.BuiltIn || in.Version != "0.281" || in.Label() != "MAME 0.281 (built in)" {
		t.Fatalf("built-in MAME: %+v", c.Installs)
	}
	if c.Default != mine {
		t.Error("the user's own default was replaced")
	}
	if len(c.Folders) != 1 || c.Folders[0] != filepath.Join(bundle, "roms") {
		t.Errorf("ROM folder: %v", c.Folders)
	}
	// removing it sticks
	Update(func(c *Config) { c.Remove(exe) })
	if _, ok := Load().Find(exe); ok {
		t.Error("removed built-in MAME came back")
	}

	// a fresh setup: the built-in one is the default; a newer RetroPixels
	// folder replaces it once the old folder is gone
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(dir, "cfg2", "mame.json"))
	if c := Load(); c.Default != exe {
		t.Fatalf("default: %q", c.Default)
	}
	bundle2 := filepath.Join(dir, "RetroPixels-2.18.0", "MAME")
	t.Setenv("RETRO_MAME_BUNDLE", bundle2)
	exe2 := fakeVerifier(t, filepath.Join(bundle2, "MAME 0.282"), "0.282", "")
	os.WriteFile(filepath.Join(bundle2, "MAME 0.282", BuiltInMarker), []byte("version=0.282\n"), 0o644)
	os.RemoveAll(filepath.Join(dir, "RetroPixels-2.17.0"))
	c = Load()
	if c.Default != exe2 || len(c.Installs) != 1 {
		t.Errorf("after upgrading: default %q, installs %+v", c.Default, c.Installs)
	}
}

// With a real MAME and ROM sets (RETRO_TEST_REAL_MAME=path to mame,
// RETRO_TEST_ROMS=folder), the check agrees with MAME itself.
func TestCheckRealMame(t *testing.T) {
	exe, roms := os.Getenv("RETRO_TEST_REAL_MAME"), os.Getenv("RETRO_TEST_ROMS")
	if exe == "" || roms == "" || runtime.GOOS == "windows" {
		t.Skip("set RETRO_TEST_REAL_MAME and RETRO_TEST_ROMS")
	}
	if _, err := exec.LookPath(exe); err != nil {
		t.Skip(err)
	}
	dir := t.TempDir()
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(dir, "mame.json"))
	t.Setenv("RETRO_MAME_BUNDLE", filepath.Join(dir, "none"))
	t.Setenv("HOME", dir)
	in := Install{Exe: exe, Version: ProbeVersion(exe)}
	c, _ := Update(func(c *Config) { c.Add(in); c.AddFolder(roms) })
	games := Library(c.Folders, NamesOf(in), nil)
	res := CheckGames(c, games)
	good := 0
	for _, g := range games {
		if res[g.Short][exe] == Good {
			good++
		}
		t.Logf("%-10s %s", g.Short, res[g.Short][exe])
	}
	t.Logf("%d of %d play in %s", good, len(games), in.Label())
	if len(res) != len(games) {
		t.Errorf("%d results for %d games", len(res), len(games))
	}
}
