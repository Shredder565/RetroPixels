package rpcs3

// Finding RPCS3: the folder the player chose, then the player's own RPCS3
// (the folders RetroPixels knows the player keeps it in, KnownDirs, and a
// search of each drive's game and emulator folders), then the RPCS3 that
// comes with RetroPixels (bundled.go).

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// KnownDirs are where this player keeps RPCS3 (in that order); they're
// looked at before any search. Only on Windows.
var KnownDirs = []string{
	`E:\Roms All Systems\008 - PS3\- PS3\rpcs3-v0.0.21-13400-e53bbd66_win64`,
	`D:\Roms All Systems\PS3\- PS3\rpcs3-v0.0.21-13400-e53bbd66_win64`,
}

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// ExeIn is RPCS3's program in dir ("" when it isn't there).
func ExeIn(dir string) string {
	if dir == "" {
		return ""
	}
	if p := filepath.Join(dir, ExeName); isFile(p) {
		return p
	}
	return ""
}

// Root is RPCS3's own folder (where dev_hdd0, dev_flash and config are)
// for its program exe.
func Root(exe string) string { return filepath.Dir(exe) }

// Where the RPCS3 in use comes from (Status.Source).
const (
	SourceChosen  = "chosen"  // the folder the player chose
	SourceYours   = "yours"   // the player's own RPCS3, found where they keep it or on the drives
	SourceBundled = "bundled" // the RPCS3 that comes with RetroPixels, run from BundledHome
)

// FindExe returns RPCS3's program: in the folder the player chose (which
// overrides everything else), else the player's own RPCS3 (the one found
// on the drives before, remembered; the known folders, KnownDirs, on
// Windows; an RPCS3 folder of their own next to RetroPixels, appDir),
// else the RPCS3 that comes with RetroPixels, copied to BundledHome
// (BundledExe). "" when none of those has it; Search then looks through
// the drives.
func FindExe(chosenDir, remembered, appDir string) string {
	exe, _ := WhichExe(chosenDir, remembered, appDir)
	return exe
}

// WhichExe is FindExe and where that RPCS3 comes from (SourceChosen,
// SourceYours or SourceBundled; "" when there's none).
func WhichExe(chosenDir, remembered, appDir string) (exe, source string) {
	if chosenDir != "" {
		// The player's choice: nothing else is used. If they chose a
		// RetroPixels download's RPCS3 folder, its lasting copy is run.
		if IsBundle(chosenDir) && ExeIn(chosenDir) != "" && !sameDir(chosenDir, BundledHome()) {
			p, _, err := InstallBundled(chosenDir, BundledHome())
			if err != nil && IsBundle(BundledHome()) {
				p = ExeIn(BundledHome())
			}
			if p == "" {
				return "", ""
			}
			return p, SourceBundled
		}
		if p := ExeIn(chosenDir); p != "" {
			if IsBundle(chosenDir) {
				return p, SourceBundled
			}
			return p, SourceChosen
		}
		return "", ""
	}
	if remembered != "" && isFile(remembered) && !IsBundle(Root(remembered)) {
		return remembered, SourceYours
	}
	if runtime.GOOS == "windows" {
		for _, d := range KnownDirs {
			if p := ExeIn(d); p != "" {
				return p, SourceYours
			}
		}
	}
	if appDir != "" {
		for _, d := range []string{filepath.Join(appDir, "RPCS3"), filepath.Join(appDir, "rpcs3")} {
			if p := ExeIn(d); p != "" && !IsBundle(d) {
				return p, SourceYours
			}
		}
	}
	if p := BundledExe(appDir); p != "" {
		return p, SourceBundled
	}
	return "", ""
}

// searchWords: folder names worth going into when looking for RPCS3 on a
// whole drive (E:\Roms All Systems\008 - PS3\- PS3\rpcs3-v0.0.21-...).
var searchWords = []string{"rom", "game", "emu", "ps3", "rpcs3", "playstation", "retro", "console", "program files"}

var skipDirs = map[string]bool{"windows": true, "$recycle.bin": true, "system volume information": true,
	"node_modules": true, ".git": true, "appdata": true, "steamapps": true,
	// RPCS3's own folders: nothing to find inside them
	"dev_hdd0": true, "dev_hdd1": true, "dev_flash": true, "dev_flash2": true, "dev_flash3": true, "dev_bdvd": true, "dev_usb000": true,
	"shaderlog": true, "cache": true}

func searchable(name string) bool {
	n := strings.ToLower(name)
	if skipDirs[n] {
		return false
	}
	for _, w := range searchWords {
		if strings.Contains(n, w) {
			return true
		}
	}
	return false
}

// Search looks for RPCS3 under each of roots (the drives, Downloads,
// Desktop…): in the root itself and up to five folders down, going only
// into folders whose names look like they hold games or emulators. A folder
// named after RPCS3 is always looked in. It returns the first one found.
func Search(roots []string) string {
	var found string
	var walk func(dir string, depth int)
	walk = func(dir string, depth int) {
		if found != "" {
			return
		}
		if p := ExeIn(dir); p != "" && !IsBundle(dir) {
			// (RetroPixels' bundled copy is not the player's own)
			found = p
			return
		}
		if depth == 0 {
			return
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			return
		}
		for _, e := range entries {
			if found != "" {
				return
			}
			if e.IsDir() && searchable(e.Name()) {
				walk(filepath.Join(dir, e.Name()), depth-1)
			}
		}
	}
	for _, r := range roots {
		if r != "" && found == "" {
			walk(r, 5)
		}
	}
	return found
}
