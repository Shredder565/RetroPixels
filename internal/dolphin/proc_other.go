//go:build !windows

package dolphin

// Running tells whether a program (by file name, e.g. PixelU-Wii.exe) is running.
// Only Windows is checked.
func Running(image string) bool { return false }
