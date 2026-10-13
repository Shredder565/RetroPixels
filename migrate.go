package main

// One-time move of data from the program's old name (RetroConsole) to RetroPixels,
// so settings, save states, recordings and backups carry over.

import (
	"os"
	"path/filepath"
)

const oldAppName = "RetroConsole"

func migrateOldName() {
	home, _ := os.UserHomeDir()
	var pairs [][2]string
	if d, err := os.UserConfigDir(); err == nil { // settings (config.json)
		pairs = append(pairs, [2]string{filepath.Join(d, oldAppName), filepath.Join(d, appName)})
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" { // window profile with save states, ffmpeg fallback
		pairs = append(pairs, [2]string{filepath.Join(la, oldAppName), filepath.Join(la, appName)})
	}
	if home != "" {
		pairs = append(pairs,
			[2]string{filepath.Join(home, "Videos", oldAppName), filepath.Join(home, "Videos", appName)},
			[2]string{filepath.Join(home, "Documents", oldAppName+" Backups"), filepath.Join(home, "Documents", appName+" Backups")})
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

// hasOriginalBackup: a first-time backup made under either name counts.
func hasOriginalBackup(path string) bool {
	for _, suffix := range []string{".retropixels-original", ".retroconsole-original"} {
		if _, err := os.Stat(path + suffix); err == nil {
			return true
		}
	}
	return false
}
