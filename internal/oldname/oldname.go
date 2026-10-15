// Package oldname moves data from the program's old name (RetroConsole) to
// RetroPixels, once, so settings, save states, recordings, backups and the
// downloaded ffmpeg carry over. RetroPixels.exe (migrate.go) and
// RetroPixels Native both run it when they start; whichever runs first does
// the move.
package oldname

import (
	"os"
	"path/filepath"
)

// Old is the program's old name.
const Old = "RetroConsole"

// Migrate renames each old folder to the new name, unless a folder with the
// new name is already there (then both are left alone).
func Migrate(newName string) {
	home, _ := os.UserHomeDir()
	var pairs [][2]string
	if d, err := os.UserConfigDir(); err == nil { // settings (config.json)
		pairs = append(pairs, [2]string{filepath.Join(d, Old), filepath.Join(d, newName)})
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" { // window profile with save states, ffmpeg fallback
		pairs = append(pairs, [2]string{filepath.Join(la, Old), filepath.Join(la, newName)})
	}
	if home != "" {
		pairs = append(pairs,
			[2]string{filepath.Join(home, "Videos", Old), filepath.Join(home, "Videos", newName)},
			[2]string{filepath.Join(home, "Documents", Old+" Backups"), filepath.Join(home, "Documents", newName+" Backups")})
	}
	for _, p := range pairs {
		if _, err := os.Stat(p[1]); err == nil {
			continue // already has the new name
		}
		if st, err := os.Stat(p[0]); err == nil && st.IsDir() {
			_ = os.Rename(p[0], p[1])
		}
	}
}
