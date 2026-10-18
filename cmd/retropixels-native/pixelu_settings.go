package main

// Wii U through PixelU (internal/pixelu): the settings kept in
// settings.json. pixelu_windows.go has the menu and the launching.

// PixelUSettings: settings.json's "pixelu" part.
type PixelUSettings struct {
	Exe        string `json:"exe,omitempty"`        // PixelU.exe chosen by the player ("" = found by itself)
	Fullscreen *bool  `json:"fullscreen,omitempty"` // unset = on, as in RetroPixels.exe
}

func (p *PixelUSettings) fullscreen() bool { return boolOr(p.Fullscreen, true) }
