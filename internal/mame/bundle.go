package mame

import (
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The RetroPixels download comes with MAME: the MAME team's official Windows
// release, unchanged, in MAME\MAME 0.xxx next to RetroPixels.exe, with a
// BuiltInMarker file beside its mame.exe. It's listed by itself the first
// time either program sees it, and becomes the default when there's no other.

// BuiltInMarker marks a MAME that came in the RetroPixels download.
const BuiltInMarker = "RETROPIXELS-BUILT-IN.txt"

// BundleDir is the MAME folder next to the program (RETRO_MAME_BUNDLE
// overrides it, for tests).
func BundleDir() string {
	if d := os.Getenv("RETRO_MAME_BUNDLE"); d != "" {
		return d
	}
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return filepath.Join(filepath.Dir(exe), "MAME")
}

// BundleRoms is the ROM folder that comes with the download: MAME\roms.
func BundleRoms() string {
	if d := BundleDir(); d != "" {
		return filepath.Join(d, "roms")
	}
	return ""
}

// BuiltIn lists the MAMEs that came with the download.
func BuiltIn() []Install {
	dir := BundleDir()
	if dir == "" {
		return nil
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []Install
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		d := filepath.Join(dir, e.Name())
		mark, err := os.ReadFile(filepath.Join(d, BuiltInMarker))
		if err != nil {
			continue
		}
		exe := findExe(d, 1)
		if exe == "" {
			continue
		}
		v := ""
		for _, l := range strings.Split(string(mark), "\n") {
			if s, ok := strings.CutPrefix(strings.TrimSpace(l), "version="); ok {
				v = s
			}
		}
		if v == "" {
			v = VersionFromPath(d)
		}
		out = append(out, Install{Exe: exe, Version: v, BuiltIn: true})
	}
	return out
}

// useBuiltIn lists a MAME that came with the download, once (so removing
// it sticks), and forgets built-in ones whose folder is gone (an older
// RetroPixels folder that was deleted). A new built-in MAME becomes the
// default when the default is gone or was the old built-in one.
func useBuiltIn() {
	found := BuiltIn()
	c := loadLocked()
	need := false
	for _, in := range found {
		if !c.seen(in.Exe) {
			need = true
		}
	}
	for _, in := range c.Installs {
		if in.BuiltIn && !exists(in.Exe) {
			need = true
		}
	}
	if !need {
		return
	}
	Update(func(c *Config) {
		def, hasDef := c.Find(c.Default)
		takeDefault := !hasDef || !exists(def.Exe) || def.BuiltIn
		var gone []string
		for _, in := range c.Installs {
			if in.BuiltIn && !exists(in.Exe) {
				gone = append(gone, in.Exe)
			}
		}
		for _, exe := range gone {
			c.Remove(exe)
		}
		newest := Install{}
		for _, in := range found {
			if c.seen(in.Exe) {
				continue
			}
			c.BuiltInSeen = append(c.BuiltInSeen, in.Exe)
			in.Added = time.Now()
			c.Add(in)
			if newest.Exe == "" || CompareVersions(in.Version, newest.Version) > 0 {
				newest = in
			}
		}
		if newest.Exe == "" {
			return
		}
		if takeDefault {
			c.Default = newest.Exe
		}
		if r := BundleRoms(); r != "" && !c.seen(r) {
			c.BuiltInSeen = append(c.BuiltInSeen, r)
			os.MkdirAll(r, 0o755)
			c.AddFolder(r)
		}
	})
}

func (c Config) seen(p string) bool {
	for _, s := range c.BuiltInSeen {
		if strings.EqualFold(s, p) {
			return true
		}
	}
	return false
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}
