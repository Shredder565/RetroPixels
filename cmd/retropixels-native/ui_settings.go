package main

import "os"

// Settings the interface adds to settings.json (embedded in Settings, so
// the names sit at the top level of the file).
type uiSettings struct {
	RumbleOff  bool   `json:"rumbleOff,omitempty"`     // Options > Controller rumble unticked
	RecQuality string `json:"recordQuality,omitempty"` // "high", "balanced" (the default) or "small"
	RecDir     string `json:"recordDir,omitempty"`     // Record > Change recordings folder; "" = Videos\RetroPixels
	// Record > Record games automatically, for every system (unset: on).
	AutoRecord *bool `json:"autoRecord,omitempty"`
	// Options > Open the SNES Debugger with SNES games (unset: on).
	SNESDebugger *bool `json:"snesDebugger,omitempty"`
}

// autoRecord: every game is recorded from its start.
func (u uiSettings) autoRecord() bool { return boolOr(u.AutoRecord, true) }

// migrateAutoRecord: automatic recording used to be a GameCube / Wii
// setting only; turned off there, it starts off for every system.
func (s *Settings) migrateAutoRecord() {
	if s.Dolphin.AutoRecord != nil {
		if s.AutoRecord == nil {
			v := *s.Dolphin.AutoRecord
			s.AutoRecord = &v
		}
		s.Dolphin.AutoRecord = nil
	}
}

// recQualities: Record > Quality, as the Recordings tab of RetroPixels.exe.
var recQualities = []struct{ id, label string }{
	{"high", "&High (big files)"},
	{"balanced", "&Balanced"},
	{"small", "&Smaller files"},
}

func (u uiSettings) recQuality() string {
	for _, q := range recQualities {
		if q.id == u.RecQuality {
			return q.id
		}
	}
	return "balanced"
}

// recDir: where recordings go (RETRO_REC_DIR, for tests, still wins).
func (u uiSettings) recDir() string {
	if os.Getenv("RETRO_REC_DIR") == "" && u.RecDir != "" {
		return u.RecDir
	}
	return recordingsDir()
}

// binChoices: File > "Open .bin files as" (the web page's system buttons,
// which pick the system for .bin files instead of guessing). Not kept
// between runs, as on the web page.
var binChoices = []string{"", "segaMD", "sms", "atari2600", "atari7800"}

// binSystem is the choice ("" = guess from the file).
var binSystem string

// systemForBin: the system for a .bin file's contents.
func systemForBin(rom []byte) string {
	if binSystem != "" && available(binSystem) {
		return binSystem
	}
	return guessBin(rom)
}
