package dolphin

import (
	"os"
	"path/filepath"
	"strings"
)

// Running tells whether a program (by file name, e.g. PixelU-Wii.exe) is
// running. Linux builds of the engines have no .exe, so "PixelU-Wii" counts
// for "PixelU-Wii.exe" as well; a Windows build run through Wine is found by
// the path in its command line.
func Running(image string) bool { return runningIn("/proc", image) }

func runningIn(proc, image string) bool {
	if image == "" {
		return false
	}
	want := []string{image}
	if ext := filepath.Ext(image); strings.EqualFold(ext, ".exe") && len(image) > len(ext) {
		want = append(want, strings.TrimSuffix(image, ext))
	}
	ents, err := os.ReadDir(proc)
	if err != nil {
		return false
	}
	for _, e := range ents {
		if !isPID(e.Name()) {
			continue
		}
		dir := filepath.Join(proc, e.Name())
		var names []string
		if exe, err := os.Readlink(filepath.Join(dir, "exe")); err == nil {
			names = append(names, strings.TrimSuffix(exe, " (deleted)"))
		}
		if b, err := os.ReadFile(filepath.Join(dir, "cmdline")); err == nil && len(b) > 0 {
			arg0, _, _ := strings.Cut(string(b), "\x00")
			names = append(names, arg0)
		}
		for _, n := range names {
			for _, w := range want {
				if IsProgram(n, w) {
					return true
				}
			}
		}
	}
	return false
}

func isPID(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}
