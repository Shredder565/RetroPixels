package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
)

// Settings is %APPDATA%\RetroPixels Native\settings.json.
type Settings struct {
	Volume      int                   `json:"volume"` // 0..100
	Muted       bool                  `json:"muted"`
	Scale       int                   `json:"windowScale"` // 1..5
	Smooth      bool                  `json:"smoothPixels"`
	MousePaddle bool                  `json:"mousePaddle"` // the mouse turns player 1's paddle (Atari 2600)
	Recent      []string              `json:"recentGames"`
	LastFolder  string                `json:"lastFolder,omitempty"`
	Maps        map[string]ControlMap `json:"controls,omitempty"` // per system, only the changed buttons
	// arcade machine settings (DIP switches) the player changed, by game (the zip's name)
	Dips map[string]map[string]int `json:"arcadeDips,omitempty"`
	// the player picked in arcade games for 3 or 4 players, by game (the zip's name)
	Seats map[string]int `json:"arcadeSeats,omitempty"`
	// GameCube / Wii through Dolphin (dolphin_settings.go)
	Dolphin DolphinSettings `json:"dolphin"`
}

const maxRecent = 10

func defaultSettings() Settings {
	return Settings{Volume: 80, Scale: 3} // 80%: host.js's starting volume
}

var settingsMu sync.Mutex

// appDir: %APPDATA%\RetroPixels Native (RETRO_NATIVE_DIR overrides, for tests).
func appDir() string {
	if d := os.Getenv("RETRO_NATIVE_DIR"); d != "" {
		return d
	}
	base := os.Getenv("APPDATA")
	if base == "" {
		base, _ = os.UserConfigDir()
	}
	if base == "" {
		base = "."
	}
	return filepath.Join(base, "RetroPixels Native")
}

func settingsPath() string { return filepath.Join(appDir(), "settings.json") }

func loadSettings() Settings {
	s := defaultSettings()
	data, err := os.ReadFile(settingsPath())
	if err == nil {
		_ = json.Unmarshal(data, &s)
	}
	s.normalize()
	return s
}

func (s *Settings) normalize() {
	if s.Volume < 0 || s.Volume > 100 {
		s.Volume = 80
	}
	if s.Scale < 1 || s.Scale > 5 {
		s.Scale = 3
	}
	if len(s.Recent) > maxRecent {
		s.Recent = s.Recent[:maxRecent]
	}
}

func saveSettings(s Settings) error {
	settingsMu.Lock()
	defer settingsMu.Unlock()
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	return writeFileAtomic(settingsPath(), data)
}

// addRecent puts path first in the recent games list.
func (s *Settings) addRecent(path string) {
	out := []string{path}
	for _, p := range s.Recent {
		if !strings.EqualFold(p, path) {
			out = append(out, p)
		}
	}
	if len(out) > maxRecent {
		out = out[:maxRecent]
	}
	s.Recent = out
}

// controlMap: the system's controls with the player's changes.
func (s *Settings) controlMap(sys string) ControlMap { return loadMap(sys, s.Maps[sys]) }

// setControlMap keeps only the buttons that differ from the defaults (so new
// defaults in later versions still arrive).
func (s *Settings) setControlMap(sys string, m ControlMap) {
	def := loadMap(sys, nil)
	changed := ControlMap{}
	for k, v := range m {
		d, ok := def[k]
		if !ok {
			continue
		}
		a, _ := json.Marshal(v)
		bb, _ := json.Marshal(d)
		if !bytes.Equal(a, bb) {
			changed[k] = v
		}
	}
	if s.Maps == nil {
		s.Maps = map[string]ControlMap{}
	}
	if len(changed) == 0 {
		delete(s.Maps, sys)
	} else {
		s.Maps[sys] = changed
	}
}

// ---------------- saves ----------------

var unsafeName = regexp.MustCompile(`[<>:"/\\|?*\x00-\x1f]+`)

// safeName makes a game's name usable as a Windows file name.
func safeName(s string) string {
	s = unsafeName.ReplaceAllString(s, "")
	s = strings.TrimSpace(strings.TrimRight(s, ". "))
	switch strings.ToUpper(strings.SplitN(s, ".", 2)[0]) {
	case "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3":
		s = "_" + s
	}
	if s == "" {
		return "game"
	}
	if r := []rune(s); len(r) > 120 {
		s = string(r[:120])
	}
	return s
}

func savesDir(sys string) string { return filepath.Join(appDir(), "saves", sys) }

// sramPath: saves\<system>\<game>.sav
func sramPath(sys, game string) string {
	return filepath.Join(savesDir(sys), safeName(game)+".sav")
}

// statePath: saves\<system>\<game>.state
func statePath(sys, game string) string {
	return filepath.Join(savesDir(sys), safeName(game)+".state")
}

// Save-state files: "RPN1", the system's id length and id, then the core's state.
var stateMagic = []byte("RPN1")

func encodeState(sys string, data []byte) []byte {
	out := make([]byte, 0, len(data)+len(sys)+5)
	out = append(out, stateMagic...)
	out = append(out, byte(len(sys)))
	out = append(out, sys...)
	return append(out, data...)
}

func decodeState(sys string, file []byte) ([]byte, error) {
	if len(file) < 5 || !bytes.Equal(file[:4], stateMagic) {
		return nil, errors.New("that isn't a RetroPixels Native save state")
	}
	n := int(file[4])
	if len(file) < 5+n {
		return nil, errors.New("that save state is damaged")
	}
	if string(file[5:5+n]) != sys {
		return nil, errors.New("that save state is for a different system")
	}
	return file[5+n:], nil
}

// writeFileAtomic writes to a temporary file then renames it into place, so
// a crash never leaves half a save.
func writeFileAtomic(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}
