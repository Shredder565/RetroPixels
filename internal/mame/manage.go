package mame

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// AddExe checks a MAME program someone chose, asks its version and lists it.
func AddExe(exe string) (Install, error) {
	st, err := os.Stat(exe)
	if err != nil || st.IsDir() {
		return Install{}, errors.New("that file isn't there")
	}
	if err := checkProgram(exe); err != nil {
		return Install{}, err
	}
	if reMameInstaller.MatchString(filepath.Base(exe)) {
		if !DownloadsAvailable {
			return Install{}, errors.New("that's the MAME team's Windows download, which doesn't run on Linux. Use the MAME that comes with RetroPixels, or install your system's mame package")
		}
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
	// Built is the set RetroPixels put together for this game ("" = the
	// archive is played as it is)
	Built string
	bios  string
}

// Plan works out which MAME plays a ROM set and its command line. exe
// overrides the game's own pick ("" = the game's pick, else the default
// unless it's known not to play the set and another version is: see Best).
func Plan(c Config, path, exe string, o LaunchOptions) (Launch, error) {
	in, ok := c.Best(Game{Short: ShortName(path), Path: path})
	if exe != "" {
		in, ok = c.Find(exe)
	}
	if !ok {
		return Launch{}, ErrNoMAME
	}
	if _, err := os.Stat(in.Exe); err != nil {
		return Launch{}, fmt.Errorf("%s isn't at %s any more. Add it again, or pick another version", in.Label(), in.Exe)
	}
	if l, handled, err := planAnySet(c, path, exe, in); handled {
		if err == nil {
			l.Args = GameArgs(l.Short, l.Args[0], OptionsOf(l.Install.Exe), o)
			if l.bios != "" {
				l.Args = append(l.Args, "-bios", l.bios)
			}
		}
		return l, err
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
var ErrNoMAME = func() error {
	if !DownloadsAvailable {
		return errors.New("MAME isn't set up yet: add a MAME you have (its mame program), or install your system's mame package and use Find MAME on this PC")
	}
	return errors.New("MAME isn't set up yet: add a MAME you have (its mame.exe), or download one")
}()

// NeedDownload: the set plays only in a MAME version that isn't on this PC
// yet; Version is the newest official release that accepts it.
type NeedDownload struct {
	Version string
	Game    string // the game's full name
}

func (e *NeedDownload) Error() string {
	return fmt.Sprintf("%s plays in MAME %s, which isn't on this PC yet. Download MAME %s (MAME tab > Download) and the game will start in it.", e.Game, e.Version, e.Version)
}

// planAnySet plays a set the MAME it's in can't play as it is: recognised by
// its files (see setfix.go), in a MAME version that accepts it, from a set put
// together for it when needed. handled is false when the set is played the
// usual way (its MAME is known to accept it, or the history doesn't know it).
// The returned Launch's Args holds only the ROM path (Plan adds the rest).
func planAnySet(c Config, path, exe string, best Install) (Launch, bool, error) {
	g := Game{Short: ShortName(path), Path: path}
	if Checked(best, g) == Good {
		return Launch{}, false, nil
	}
	db := History()
	if db == nil {
		return Launch{}, false, nil
	}
	installs := c.Installs
	if exe != "" {
		installs = []Install{best} // the version picked for this time only
	}
	var poolCache Pool
	pool := func() Pool {
		if poolCache == nil {
			poolCache = NewPool(c.Folders, path)
		}
		return poolCache
	}
	downloadable := func(v string) bool {
		return DownloadsAvailable && exe == "" && CompareVersions(v, "0.171") >= 0
	}
	res, known := ResolveLazy(db, path, installs, best.Exe, pool, downloadable)
	if !known {
		return Launch{}, false, nil
	}
	title := res.Machine.Desc
	if title == "" {
		title = res.Machine.Name
	}
	switch {
	case res.Install != nil:
		if miss := res.AddDevices(pool); len(miss) > 0 {
			return Launch{}, true, errors.New(DevicesText(title, miss))
		}
		in := *res.Install
		l := Launch{Install: in, Short: res.Machine.Name, Name: title, bios: res.Bios}
		dirs := c.Folders
		gameDir := filepath.Dir(path)
		if !res.AsIs {
			built, err := res.Build(filepath.Join(SetsDir(), "MAME "+in.Version))
			if err != nil {
				return Launch{}, true, fmt.Errorf("couldn't put the set for %s together: %v", title, err)
			}
			l.Built = built
			// the put-together set comes first, then your folders (for
			// files only MAME's own devices list)
			dirs = append([]string{gameDir}, dirs...)
			gameDir = filepath.Dir(built)
		}
		l.Args = []string{RomPath(in.Exe, gameDir, dirs)}
		return l, true, nil
	case res.Download != "":
		return Launch{}, true, &NeedDownload{Version: res.Download, Game: title}
	default:
		// named as the game and not known to fail: let MAME try it (the
		// history can list a file or two MAME doesn't ask for)
		if g.Short == res.Machine.Name && Checked(best, g) != Bad {
			return Launch{}, false, nil
		}
		return Launch{}, true, errors.New(res.MissingText())
	}
}
