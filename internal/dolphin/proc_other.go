//go:build !windows && !linux

package dolphin

// Running tells whether a program (by file name, e.g. PixelU-Wii.exe) is running.
// Only Windows and Linux are checked.
func Running(image string) bool { return false }
