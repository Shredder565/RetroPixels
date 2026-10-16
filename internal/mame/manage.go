package mame

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// AddExe checks a MAME program someone chose, asks its version and lists it.
func AddExe(exe string) (Install, error) {
	st, err := os.Stat(exe)
	if err != nil || st.IsDir() {
		return Install{}, errors.New("that file isn't there")
	}
	if !strings.EqualFold(filepath.Ext(exe), ".exe") {
		return Install{}, errors.New("that isn't a program (.exe)")
	}
	if reMameInstaller.MatchString(filepath.Base(exe)) {
		return Install{}, errors.New("that's MAME's download file (it unpacks MAME). Use Download in RetroPixels instead, or run it and then choose the mame.exe it unpacks")
	}
	in := Install{Exe: exe, Version: ProbeVersion(exe), Added: time.Now()}
	_, err = Update(func(c *Config) { c.Add(in) })
	return in, err
}

// Detect looks for MAME on this PC (FindExes) and lists what it finds; if
// there are no ROM folders yet, the "roms" folders next to them are added.
// It takes a while: run it in the background.
func Detect() (Config, int) {
	c := Load()
	exes := FindExes(c.Folders)
	var found []Install
	for _, exe := range exes {
		if _, ok := c.Find(exe); ok {
			continue
		}
		found = append(found, Install{Exe: exe, Version: ProbeVersion(exe), Added: time.Now()})
	}
	roms := GuessRomFolders(exes)
	c, _ = Update(func(c *Config) {
		_, hadDefault := c.Find(c.Default)
		for _, in := range found {
			c.Add(in)
		}
		if !hadDefault && len(c.Installs) > 0 {
			c.sortInstalls()
			c.Default = c.Installs[0].Exe // the newest version found
		}
		if len(c.Folders) == 0 {
			for _, d := range roms {
				c.AddFolder(d)
			}
		}
		c.Detected = true
	})
	return c, len(found)
}

// Launch is everything needed to start one game.
type Launch struct {
	Install Install
	Short   string
	Name    string
	Args    []string
}

// Plan works out which MAME plays a ROM set and its command line. exe
// overrides the game's own pick ("" = the game's pick or the default).
func Plan(c Config, path, exe string, o LaunchOptions) (Launch, error) {
	in, ok := c.For(ShortName(path))
	if exe != "" {
		in, ok = c.Find(exe)
	}
	if !ok {
		return Launch{}, ErrNoMAME
	}
	if _, err := os.Stat(in.Exe); err != nil {
		return Launch{}, fmt.Errorf("%s isn't at %s any more. Add it again, or pick another version", in.Label(), in.Exe)
	}
	short := ShortName(path)
	name := short
	if n := KnownNames(in)[short]; n != "" {
		name = n
	}
	args := GameArgs(short, RomPath(in.Exe, filepath.Dir(path), c.Folders), OptionsOf(in.Exe), o)
	return Launch{Install: in, Short: short, Name: name, Args: args}, nil
}

// ErrNoMAME: no MAME has been added or downloaded yet.
var ErrNoMAME = errors.New("MAME isn't set up yet: add a MAME you have (its mame.exe), or download one")
