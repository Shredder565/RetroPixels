package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"retropixels/internal/games"
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

	uiSettings // rumble, recording quality and folder (ui_settings.go)
	apSettings // Archipelago: the original ROM for each game's patches (archipelago.go)
	// GameCube / Wii through Dolphin (dolphin_settings.go)
	Dolphin DolphinSettings `json:"dolphin"`
	// Wii U through PixelU (pixelu_settings.go)
	PixelU PixelUSettings `json:"pixelu"`
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
	s.migrateAutoRecord()
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

// Where saves are kept, and how: internal/games's Store (shared with the
// Android app's engine), in %APPDATA%\RetroPixels Native.

func store() games.Store { return games.Store{Dir: appDir()} }

// safeName makes a game's name usable as a Windows file name.
func safeName(s string) string { return games.SafeName(s) }

// firmwareDir: firmware\<chip>, the cartridge chip programs the player
// supplied (the Super Nintendo's DSP-3, DSP-4, ST010, ST011 and ST018 games
// need one), each asked for once.
func firmwareDir() string { return store().FirmwareDir() }

// loadFirmware reads every kept chip program, by chip name.
func loadFirmware() map[string][]byte { return store().LoadFirmware() }

// saveFirmware keeps a chip program the player chose.
func saveFirmware(chip string, b []byte) error { return store().SaveFirmware(chip, b) }

// needsFirmware tells whether a core refused a game for want of a chip
// program: the chip's name, the usual file name and the message to show.
func needsFirmware(err error) (chip, file, msg string, ok bool) { return games.NeedsFirmware(err) }

func savesDir(sys string) string { return store().SavesDir(sys) }

// sramPath: saves\<system>\<game>.sav
func sramPath(sys, game string) string { return store().SRAMPath(sys, game) }

// statePath: saves\<system>\<game>.state
func statePath(sys, game string) string { return store().StatePath(sys, game) }

// Save-state files: "RPN1", the system's id length and id, then the core's state.
var stateMagic = games.StateMagic

func encodeState(sys string, data []byte) []byte { return games.EncodeState(sys, data) }

func decodeState(sys string, file []byte) ([]byte, error) { return games.DecodeState(sys, file) }

// writeFileAtomic writes to a temporary file then renames it into place, so
// a crash never leaves half a save.
func writeFileAtomic(path string, data []byte) error { return games.WriteFileAtomic(path, data) }
