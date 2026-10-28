// Package rpcs3 plays PlayStation 3 games in RPCS3 (https://rpcs3.net,
// GPL-2.0), a separate program RetroPixels starts the way it starts MAME:
// the player's own, or the official build that comes in the Windows
// download, run from a lasting copy (bundled.go). It finds rpcs3.exe, lists the games installed in
// it (dev_hdd0\game) and the disc games it knows (games.yml), checks that
// the PS3 system software and each game's licence are there, sets an Xbox
// controller up as player 1, and builds RPCS3's command line.
// RetroPixels.exe's PS3 tab and RetroPixels Native share it.
//
// RetroPixels never downloads or ships PlayStation 3 system software,
// games, keys or licences: the player installs the system software
// (PS3UPDAT.PUP) and their games with RPCS3, and adds their own licence
// files (.rap or .edat), which RetroPixels only copies into RPCS3's folder
// the way RPCS3's own "Install Packages/Raps/Edats" does.
package rpcs3

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// AppFolder is RetroPixels' settings folder name in %APPDATA%.
const AppFolder = "RetroPixels"

// Config is %APPDATA%\RetroPixels\rpcs3.json, shared by both programs.
type Config struct {
	// Folder: the RPCS3 folder the player chose ("Choose RPCS3 folder…");
	// it overrides every other place RPCS3 is looked for.
	Folder     string               `json:"folder,omitempty"`
	Exe        string               `json:"exe,omitempty"` // rpcs3.exe, found by the search (remembered)
	Fullscreen *bool                `json:"fullscreen,omitempty"`
	Embed      *bool                `json:"playInWindow,omitempty"`
	Played     map[string]time.Time `json:"played,omitempty"`   // by title ID
	Detected   bool                 `json:"detected,omitempty"` // looked for RPCS3 on this PC once
}

// FullscreenOn: games start fullscreen in RPCS3's own window (the default).
func (c Config) FullscreenOn() bool { return c.Fullscreen == nil || *c.Fullscreen }

// EmbedOn: games play inside the RetroPixels window (off unless chosen).
func (c Config) EmbedOn() bool { return c.Embed != nil && *c.Embed }

// MarkPlayed remembers when a game was last played.
func (c *Config) MarkPlayed(titleID string, t time.Time) {
	if c.Played == nil {
		c.Played = map[string]time.Time{}
	}
	c.Played[strings.ToUpper(titleID)] = t
}

// ConfigPath: %APPDATA%\RetroPixels\rpcs3.json (RETRO_RPCS3_CONFIG
// overrides it, for tests).
func ConfigPath() string {
	if p := os.Getenv("RETRO_RPCS3_CONFIG"); p != "" {
		return p
	}
	d, err := os.UserConfigDir()
	if err != nil {
		d = "."
	}
	return filepath.Join(d, AppFolder, "rpcs3.json")
}

var fileMu sync.Mutex

// Load reads the settings (empty when there are none yet).
func Load() Config {
	fileMu.Lock()
	defer fileMu.Unlock()
	return load()
}

func load() Config {
	var c Config
	if b, err := os.ReadFile(ConfigPath()); err == nil {
		_ = json.Unmarshal(b, &c)
	}
	return c
}

// Update reads the settings fresh from the file, changes them and saves
// them, so the two programs don't undo each other's changes.
func Update(fn func(*Config)) (Config, error) {
	fileMu.Lock()
	defer fileMu.Unlock()
	c := load()
	fn(&c)
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
