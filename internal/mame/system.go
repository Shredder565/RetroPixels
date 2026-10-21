package mame

import (
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// What differs between Windows and Linux. On Windows MAME is mame.exe, any
// number of official releases sit in folders of their own, and each keeps its
// settings and saves in its own folder. On Linux MAME is "mame": the one that
// comes with RetroPixels (built from the MAME team's official source, see
// tools/bundlemame), your distribution's mame package (/usr/games/mame on
// Debian and Ubuntu, /usr/bin/mame on Fedora and Arch), or one you built.
// The MAME team publishes ready-made builds for Windows only, so there are
// no downloads on Linux.

// ExeName is MAME's program file name on this system.
var ExeName = func() string {
	if runtime.GOOS == "windows" {
		return "mame.exe"
	}
	return "mame"
}()

// ExePattern is the "choose a MAME" dialog's file filter.
var ExePattern = func() string {
	if runtime.GOOS == "windows" {
		return "mame*.exe"
	}
	return "mame*"
}()

// DownloadsAvailable: the MAME team's ready-made releases run here (Windows only).
var DownloadsAvailable = runtime.GOOS == "windows"

// ErrNoDownloads: the in-app download on a system without ready-made MAME builds.
var ErrNoDownloads = errors.New("the MAME team publishes ready-made MAME builds for Windows only. On Linux, play with the MAME that comes with RetroPixels (built from the MAME team's official source) or your system's mame package (Add MAME… or Find MAME on this PC)")

// systemExes are where Linux distributions put MAME (the wrapper scripts
// that set up ~/.mame, not the program behind them in /usr/lib/mame).
var systemExes = []string{"/usr/games/mame", "/usr/bin/mame", "/usr/local/games/mame", "/usr/local/bin/mame", "/snap/bin/mame"}

// systemRomDirs are the ROM folders the distributions' MAME looks in
// (Debian's and Ubuntu's mame.ini; the first one is in your home folder).
func systemRomDirs() []string {
	var out []string
	if home, err := os.UserHomeDir(); err == nil {
		out = append(out, filepath.Join(home, "mame", "roms"))
	}
	return append(out, "/usr/local/share/games/mame/roms", "/usr/share/games/mame/roms")
}

// isSystemExe: a MAME installed for the whole system (Linux), which keeps
// its settings in ~/.mame rather than in its own folder.
func isSystemExe(exe string) bool {
	if runtime.GOOS == "windows" {
		return false
	}
	dir := filepath.Dir(exe)
	if strings.HasPrefix(dir, "/usr/") || strings.HasPrefix(dir, "/snap/") {
		return true
	}
	st, err := os.Stat(dir)
	return err == nil && st.IsDir() && !writable(dir)
}

func writable(dir string) bool {
	f, err := os.CreateTemp(dir, ".rp-write-*.tmp")
	if err != nil {
		return false
	}
	f.Close()
	os.Remove(f.Name())
	return true
}

// WorkDir is the folder MAME runs in, where it keeps mame.ini, its saves,
// settings and screenshots: its own folder (always on Windows), or ~/.mame
// for a MAME installed for the whole system, whose folder (/usr/games) it
// can't write to (that's the folder the distributions' MAME uses too).
func WorkDir(exe string) string {
	dir := filepath.Dir(exe)
	if !isSystemExe(exe) {
		return dir
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return dir
	}
	d := filepath.Join(home, ".mame")
	if os.MkdirAll(d, 0o755) != nil {
		return dir
	}
	return d
}

// checkProgram tells whether a chosen file can be MAME's program here: a
// .exe on Windows; on Linux a file marked as executable that isn't a
// Windows program.
func checkProgram(exe string) error {
	if runtime.GOOS == "windows" {
		if !strings.EqualFold(filepath.Ext(exe), ".exe") {
			return errors.New("that isn't a program (.exe)")
		}
		return nil
	}
	st, err := os.Stat(exe)
	if err != nil {
		return errors.New("that file isn't there")
	}
	if !st.Mode().IsRegular() || st.Mode().Perm()&0o111 == 0 {
		return errors.New("that isn't a program (it isn't marked as executable)")
	}
	if f, err := os.Open(exe); err == nil {
		head := make([]byte, 2)
		n, _ := io.ReadFull(f, head)
		f.Close()
		if n == 2 && string(head) == "MZ" {
			return errors.New("that's a Windows MAME, which doesn't run on Linux. Use the MAME that comes with RetroPixels, or install your system's mame package")
		}
	}
	return nil
}

// linuxExes lists the distribution's MAME and any "mame" on PATH (one entry
// per program: /usr/bin/mame is often a link to /usr/games/mame).
func linuxExes() []string {
	if runtime.GOOS == "windows" {
		return nil
	}
	cands := append([]string(nil), systemExes...)
	if p, err := exec.LookPath("mame"); err == nil {
		if a, err := filepath.Abs(p); err == nil {
			cands = append(cands, a)
		}
	}
	var out []string
	seen := map[string]bool{}
	for _, p := range cands {
		st, err := os.Stat(p)
		if err != nil || !st.Mode().IsRegular() || st.Mode().Perm()&0o111 == 0 {
			continue
		}
		real := p
		if r, err := filepath.EvalSymlinks(p); err == nil {
			real = r
		}
		if seen[real] {
			continue
		}
		seen[real] = true
		out = append(out, p)
	}
	return out
}
