package mame

import (
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// InstallDirs: where downloaded MAME versions go, each in a folder named
// for its version: a "MAME" folder next to the program if it can write
// there, otherwise %LOCALAPPDATA%\RetroPixels\MAME.
func InstallDirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Join(filepath.Dir(exe), "MAME"))
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		dirs = append(dirs, filepath.Join(la, AppFolder, "MAME"))
	}
	return dirs
}

// The programs that are MAME itself. MAME's own downloads (mame0270b_x64.exe)
// are self-extracting archives, not MAME, and are left out.
var (
	reMameExe       = regexp.MustCompile(`(?i)^(mame|mameui|mamearcade)(32|64)?\.exe$`)
	reMameInstaller = regexp.MustCompile(`(?i)^mame\d{3,4}`)
)

// IsMameExe tells whether a file name is MAME's program.
func IsMameExe(name string) bool {
	return reMameExe.MatchString(name) && !reMameInstaller.MatchString(name)
}

var skipDirs = map[string]bool{"windows": true, "$recycle.bin": true, "system volume information": true, "programdata": true,
	"node_modules": true, ".git": true, "appdata": true, "steamapps": true, "roms": true, "samples": true, "artwork": true,
	"snap": true, "cfg": true, "nvram": true, "hash": true, "plugins": true, "shader": true, "bgfx": true, "docs": true}

// FindExes looks for MAME where people keep it: RetroPixels' download
// folders, near your ROM folders and the program, in Program Files, and in
// the top folders of each drive. It can take a while (run it in the background).
func FindExes(romFolders []string) []string {
	var roots []string
	roots = append(roots, InstallDirs()...)
	for _, p := range romFolders {
		roots = append(roots, filepath.Dir(p), filepath.Dir(filepath.Dir(p)))
	}
	if exe, err := os.Executable(); err == nil {
		roots = append(roots, filepath.Dir(exe), filepath.Dir(filepath.Dir(exe)))
	}
	for _, env := range []string{"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"} {
		if d := os.Getenv(env); d != "" {
			roots = append(roots, d)
		}
	}
	if os.PathSeparator == '\\' {
		for d := 'C'; d <= 'Z'; d++ {
			roots = append(roots, string(d)+`:\`)
		}
	}
	var found []string
	seen := map[string]bool{}
	for _, r := range roots {
		if _, err := os.Stat(r); err != nil || seen[strings.ToLower(r)] {
			continue
		}
		seen[strings.ToLower(r)] = true
		depth := 3
		if len(r) <= 3 {
			depth = 4 // emulators on game drives often sit a level deeper
		}
		limitedWalk(r, depth, func(p string, d fs.DirEntry) {
			if !d.IsDir() && IsMameExe(d.Name()) {
				k := strings.ToLower(p)
				if !seen[k] {
					seen[k] = true
					found = append(found, p)
				}
			}
		})
	}
	return found
}

func limitedWalk(dir string, depth int, fn func(string, fs.DirEntry)) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	for _, e := range entries {
		p := filepath.Join(dir, e.Name())
		fn(p, e)
		if e.IsDir() && depth > 1 && !skipDirs[strings.ToLower(e.Name())] {
			limitedWalk(p, depth-1, fn)
		}
	}
}

// GuessRomFolders: the "roms" folder next to each MAME, when it has games in it.
func GuessRomFolders(exes []string) []string {
	var out []string
	for _, exe := range exes {
		d := filepath.Join(filepath.Dir(exe), "roms")
		entries, err := os.ReadDir(d)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() && IsRomSet(e.Name()) {
				out = append(out, d)
				break
			}
		}
	}
	return out
}
