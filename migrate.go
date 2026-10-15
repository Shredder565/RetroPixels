package main

// One-time move of data from the program's old name (RetroConsole) to RetroPixels,
// so settings, save states, recordings and backups carry over
// (internal/oldname, shared with RetroPixels Native).

import (
	"os"

	"retropixels/internal/oldname"
)

func migrateOldName() { oldname.Migrate(appName) }

// hasOriginalBackup: a first-time backup made under either name counts.
func hasOriginalBackup(path string) bool {
	for _, suffix := range []string{".retropixels-original", ".retroconsole-original"} {
		if _, err := os.Stat(path + suffix); err == nil {
			return true
		}
	}
	return false
}
