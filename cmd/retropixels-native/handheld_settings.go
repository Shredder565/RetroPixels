package main

// 3DS and DS games through PixelDS, Azahar and melonDS (internal/handheld):
// the settings kept in settings.json. handheld_windows.go has the menu.

// HandheldSettings: settings.json's "handheld" part.
type HandheldSettings struct {
	Exe        map[string]string `json:"exe,omitempty"`        // engine ID -> program chosen by the player
	Fullscreen *bool             `json:"fullscreen,omitempty"` // unset = on
	DSEngine   string            `json:"dsEngine,omitempty"`   // "pixelds" (default) or "melonds"
	// 3DS games: "azahar" (default) or "pixelds"
	ThreeDSEngine string `json:"threeDSEngine,omitempty"`
}

// choice: the player's emulator for a system's games.
func (h *HandheldSettings) choice(system string) string {
	if system == "3ds" {
		return h.ThreeDSEngine
	}
	return h.DSEngine
}

func (h *HandheldSettings) fullscreen() bool { return boolOr(h.Fullscreen, true) }
