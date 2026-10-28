package main

import (
	"path/filepath"
	"strings"
)

// isNativeArcadeOnly: a Sega Model 3 set (Star Wars Trilogy Arcade), which
// RetroPixels plays itself (cores/model3) and MAME can't.
func isNativeArcadeOnly(path string) bool {
	name := strings.ToLower(filepath.Base(strings.ReplaceAll(path, `\`, "/")))
	switch strings.TrimSuffix(strings.TrimSuffix(name, ".zip"), ".7z") {
	case "swtrilgy", "swtrilgya":
		return true
	}
	return false
}
