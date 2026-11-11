package main

// 3DS and DS games through PixelDS (internal/handheld): the settings kept in
// settings.json. handheld_windows.go has the menu.

import "time"

// HandheldSettings: settings.json's "handheld" part.
type HandheldSettings struct {
	Exe        map[string]string `json:"exe,omitempty"`        // engine ID -> program chosen by the player
	Fullscreen *bool             `json:"fullscreen,omitempty"` // unset = on
	// the 3DS / DS tab's games folders, and when each game (lower-case path) was last played
	Folders []string             `json:"folders,omitempty"`
	Played  map[string]time.Time `json:"played,omitempty"`
}

func (h *HandheldSettings) fullscreen() bool { return boolOr(h.Fullscreen, true) }
