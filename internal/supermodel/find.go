package supermodel

// What both RetroPixels programs (RetroPixels.exe's Supermodel tab and
// RetroPixels Native) share: the player's Supermodel folder, where the game
// is looked for, how it's found and why Supermodel didn't start.

import (
	"bufio"
	"os"
	"path/filepath"
	"strings"

	"retropixels/internal/dolphin"
)

// Home is the player's Supermodel folder (SUPERMODEL_HOME) in RetroPixels'
// settings folder configDir: Supermodel's settings, NVRAM (the game's own
// settings and high scores) and save states. RETRO_SUPERMODEL_HOME
// overrides it, for tests.
func Home(configDir string) string {
	if d := os.Getenv("RETRO_SUPERMODEL_HOME"); d != "" {
		return d
	}
	return filepath.Join(configDir, "Supermodel")
}

// DefaultHome is Home in RetroPixels.exe's settings folder
// (%APPDATA%\RetroPixels\Supermodel), so both programs share the game's
// settings, high scores and save states.
func DefaultHome() string {
	d, err := os.UserConfigDir()
	if err != nil {
		d = "."
	}
	return Home(filepath.Join(d, dolphin.AppFolder))
}

// SearchFolders lists where the game is looked for, each folder once: first
// (MAME's ROM folders, since it's a MAME ROM set), the ROM folders next to
// Supermodel (exe) and to RetroPixels (appDir), extra (the program's other
// game folders), then the usual places for downloads in home.
func SearchFolders(first []string, exe, appDir, home string, extra []string) []string {
	var out []string
	add := func(p ...string) {
		for _, d := range p {
			if d != "" && !containsFold(out, d) {
				out = append(out, d)
			}
		}
	}
	add(first...)
	if exe != "" {
		add(filepath.Join(filepath.Dir(exe), "ROMs"))
	}
	if appDir != "" {
		add(filepath.Join(appDir, "ROMs"), filepath.Join(appDir, "roms"))
	}
	add(extra...)
	if home != "" {
		// The desktop's own Downloads, Desktop and Documents (on Linux they
		// can have other names; see dolphin.UserFolder).
		add(dolphin.DownloadDirs(home)...)
		add(dolphin.UserFolder(home, "Desktop"), dolphin.UserFolder(home, "Documents"),
			filepath.Join(home, "ROMs"), filepath.Join(home, "roms"))
	}
	return out
}

func containsFold(list []string, p string) bool {
	lp := strings.ToLower(filepath.Clean(p))
	for _, r := range list {
		if strings.ToLower(filepath.Clean(r)) == lp {
			return true
		}
	}
	return false
}

// Find looks for the game in folders (FindROM) and, when no copy there
// plays, through the game folders of each of driveRoots (FindROMOnDrives).
// It returns the first copy that plays, or else the first one found (its
// Check says what's wrong with it); "" when there's none.
func Find(folders, driveRoots []string) (string, Check) {
	p, c := FindROM(folders)
	if !c.OK && len(driveRoots) > 0 {
		if q, d := FindROMOnDrives(driveRoots); q != "" && (d.OK || p == "") {
			p, c = q, d
		}
	}
	return p, c
}

// SmoothnessName is the flight stick smoothing in use: s when it's one of
// Smoothness, else DefaultSmoothness.
func SmoothnessName(s string) string {
	if _, ok := Smoothness[s]; ok {
		return s
	}
	return DefaultSmoothness
}

// LogErrors is the error lines of Supermodel's log at path (it writes why a
// game didn't start there as well as to its console), the last 20 at most.
func LogErrors(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	var lines []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if l := sc.Text(); strings.Contains(strings.ToLower(l), "error") {
			lines = append(lines, l)
		}
	}
	if len(lines) > 20 {
		lines = lines[len(lines)-20:]
	}
	return strings.Join(lines, "\n")
}
