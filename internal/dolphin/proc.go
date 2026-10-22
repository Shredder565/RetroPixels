package dolphin

import "strings"

// baseName is a path's file name, with either kind of slash (the paths
// Windows reports for running programs use backslashes).
func baseName(p string) string {
	if i := strings.LastIndexAny(p, `\/`); i >= 0 {
		return p[i+1:]
	}
	return p
}

// IsProgram tells whether a running program (its path or file name) is exe
// (a path or file name), by file name, ignoring case. The engine is told by
// its own name (PixelU-Wii.exe): another program, a separately installed
// Dolphin.exe among them, never counts.
func IsProgram(path, exe string) bool {
	name, want := baseName(path), baseName(exe)
	return want != "" && want != "." && strings.EqualFold(name, want)
}
