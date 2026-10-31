// Package mame plays arcade games in MAME (https://www.mamedev.org), any
// version of it: the MAME installs you add (or download from MAME's official
// releases), which one is the default, which one each game uses, and the
// folders your ROM sets are in. RetroPixels.exe and RetroPixels Native share
// it, so a MAME added in one shows up in the other.
//
// MAME is free software (GPL-2.0+) by the MAME team; RetroPixels only starts
// it, it doesn't change it.
package mame

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// Install is one MAME on this PC.
type Install struct {
	Exe     string    `json:"exe"`
	Version string    `json:"version"` // "0.270", "" when it couldn't be read
	Added   time.Time `json:"added,omitempty"`
	BuiltIn bool      `json:"builtIn,omitempty"` // came in the RetroPixels download
}

// Label names an install for people: "MAME 0.270".
func (i Install) Label() string {
	if i.Version == "" {
		return "MAME (" + filepath.Base(filepath.Dir(i.Exe)) + ")"
	}
	if i.BuiltIn {
		return "MAME " + i.Version + " (built in)"
	}
	return "MAME " + i.Version
}

// Config is %APPDATA%\RetroPixels\mame.json.
type Config struct {
	Installs []Install `json:"installs,omitempty"`
	Default  string    `json:"default,omitempty"` // the exe used unless a game picks another
	Folders  []string  `json:"romFolders,omitempty"`
	// which MAME each game uses, by its lower-cased short name (tmnt);
	// a game that isn't listed uses the default
	Games    map[string]string    `json:"gameVersion,omitempty"`
	Played   map[string]time.Time `json:"played,omitempty"` // by lower-cased short name
	Embed    *bool                `json:"playInWindow,omitempty"`
	Detected bool                 `json:"detected,omitempty"` // looked for MAME on this PC once
	// the built-in MAMEs (and their ROM folder) listed already, so
	// removing one sticks
	BuiltInSeen []string `json:"builtInSeen,omitempty"`
	// Seat: which player (1 to 4) the keyboard and Xbox controller play as;
	// 0 means player 1. SeatFor: the same for one game, by its lower-cased
	// short name.
	Seat    int            `json:"player,omitempty"`
	SeatFor map[string]int `json:"gamePlayer,omitempty"`
}

// SeatOf: the player (1 to 4) the player plays a game (its short name) as.
func (c Config) SeatOf(short string) int {
	if n := c.SeatFor[strings.ToLower(short)]; n >= 1 && n <= MaxSeats {
		return n
	}
	if c.Seat >= 1 && c.Seat <= MaxSeats {
		return c.Seat
	}
	return 1
}

// SetSeat makes the player play as player n (1 to 4): for one game (short)
// or, with short "", for every game that has no choice of its own.
func (c *Config) SetSeat(short string, n int) {
	if n < 1 || n > MaxSeats {
		n = 1
	}
	if short == "" {
		c.Seat = n
		return
	}
	if c.SeatFor == nil {
		c.SeatFor = map[string]int{}
	}
	c.SeatFor[strings.ToLower(short)] = n
}

// EmbedOn: games play inside the RetroPixels window (the default).
func (c Config) EmbedOn() bool { return c.Embed == nil || *c.Embed }

// Find returns the install with this exe.
func (c Config) Find(exe string) (Install, bool) {
	for _, i := range c.Installs {
		if strings.EqualFold(i.Exe, exe) {
			return i, true
		}
	}
	return Install{}, false
}

// Add lists an install (or updates its version). The first one becomes the default.
func (c *Config) Add(in Install) {
	for k := range c.Installs {
		if strings.EqualFold(c.Installs[k].Exe, in.Exe) {
			if in.Version != "" {
				c.Installs[k].Version = in.Version
			}
			return
		}
	}
	if in.Added.IsZero() {
		in.Added = time.Now()
	}
	c.Installs = append(c.Installs, in)
	if _, ok := c.Find(c.Default); !ok {
		c.Default = in.Exe
	}
	c.sortInstalls()
}

// Remove forgets an install (the files stay). Games that used it go back to the default.
func (c *Config) Remove(exe string) {
	out := c.Installs[:0]
	for _, i := range c.Installs {
		if !strings.EqualFold(i.Exe, exe) {
			out = append(out, i)
		}
	}
	c.Installs = out
	for k, v := range c.Games {
		if strings.EqualFold(v, exe) {
			delete(c.Games, k)
		}
	}
	if strings.EqualFold(c.Default, exe) {
		c.Default = ""
		if len(c.Installs) > 0 {
			c.Default = c.Installs[0].Exe
		}
	}
}

// sortInstalls: newest version first.
func (c *Config) sortInstalls() {
	sort.SliceStable(c.Installs, func(a, b int) bool {
		return CompareVersions(c.Installs[a].Version, c.Installs[b].Version) > 0
	})
}

// DefaultInstall is the default MAME, if there is one.
func (c Config) DefaultInstall() (Install, bool) {
	if i, ok := c.Find(c.Default); ok {
		return i, true
	}
	if len(c.Installs) > 0 {
		return c.Installs[0], true
	}
	return Install{}, false
}

// For returns the MAME a game plays in: its own pick, else the default.
func (c Config) For(short string) (Install, bool) {
	if exe, ok := c.Games[strings.ToLower(short)]; ok {
		if i, ok := c.Find(exe); ok {
			return i, true
		}
	}
	return c.DefaultInstall()
}

// SetGame picks the MAME for one game; "" goes back to the default.
func (c *Config) SetGame(short, exe string) {
	short = strings.ToLower(short)
	if exe == "" {
		delete(c.Games, short)
		return
	}
	if c.Games == nil {
		c.Games = map[string]string{}
	}
	c.Games[short] = exe
}

// AddFolder adds a ROM folder unless it's listed already.
func (c *Config) AddFolder(dir string) bool {
	for _, f := range c.Folders {
		if sameDir(f, dir) {
			return false
		}
	}
	c.Folders = append(c.Folders, dir)
	return true
}

// RemoveFolder forgets a ROM folder.
func (c *Config) RemoveFolder(dir string) {
	out := c.Folders[:0]
	for _, f := range c.Folders {
		if !sameDir(f, dir) {
			out = append(out, f)
		}
	}
	c.Folders = out
}

func sameDir(a, b string) bool {
	return strings.EqualFold(strings.TrimRight(a, `\/`), strings.TrimRight(b, `\/`))
}

// MarkPlayed remembers when a game was last played.
func (c *Config) MarkPlayed(short string, t time.Time) {
	if c.Played == nil {
		c.Played = map[string]time.Time{}
	}
	c.Played[strings.ToLower(short)] = t
}

// ---------------- the file ----------------

// AppFolder is RetroPixels.exe's settings folder name in %APPDATA% (and its
// download folder name in %LOCALAPPDATA%).
const AppFolder = "RetroPixels"

// ConfigPath is where the MAME settings are kept: %APPDATA%\RetroPixels\mame.json
// for both programs (RETRO_MAME_CONFIG overrides it, for tests).
func ConfigPath() string {
	if p := os.Getenv("RETRO_MAME_CONFIG"); p != "" {
		return p
	}
	d, err := os.UserConfigDir()
	if err != nil {
		d = "."
	}
	return filepath.Join(d, AppFolder, "mame.json")
}

var fileMu sync.Mutex

// Load reads the settings (empty when there are none yet), listing a MAME
// that came with the download first if it's new.
func Load() Config {
	useBuiltIn()
	return loadLocked()
}

func loadLocked() Config {
	fileMu.Lock()
	defer fileMu.Unlock()
	return load()
}

func load() Config {
	var c Config
	if b, err := os.ReadFile(ConfigPath()); err == nil {
		_ = json.Unmarshal(b, &c)
	}
	c.sortInstalls()
	return c
}

// Update reads the settings fresh from the file, changes them and saves
// them, so the two programs don't undo each other's changes.
func Update(fn func(*Config)) (Config, error) {
	fileMu.Lock()
	defer fileMu.Unlock()
	c := load()
	fn(&c)
	c.sortInstalls()
	p := ConfigPath()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return c, err
	}
	b, _ := json.MarshalIndent(c, "", "  ")
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return c, err
	}
	return c, os.Rename(tmp, p)
}
