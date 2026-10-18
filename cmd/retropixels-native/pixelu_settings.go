package main

// Wii U through PixelU (internal/pixelu): the settings kept in
// settings.json and how a game is launched. pixelu_windows.go has the menu
// and the window.

import "retropixels/internal/pixelu"

// PixelUSettings: settings.json's "pixelu" part. Unset switches take
// RetroPixels.exe's defaults (fullscreen, inside the window).
type PixelUSettings struct {
	Exe        string `json:"exe,omitempty"` // PixelU.exe chosen by the player ("" = found by itself)
	Fullscreen *bool  `json:"fullscreen,omitempty"`
	Embed      *bool  `json:"playInWindow,omitempty"`
}

func (p *PixelUSettings) fullscreen() bool { return boolOr(p.Fullscreen, true) }
func (p *PixelUSettings) embed() bool      { return boolOr(p.Embed, true) }

// launchOptions: how PixelU starts a game. Docked into this window, PixelU
// shows only the game picture (--rp-embed) and isn't fullscreen itself:
// this window goes fullscreen instead.
func (p *PixelUSettings) launchOptions() pixelu.LaunchOptions {
	if p.embed() {
		return pixelu.LaunchOptions{Embed: true}
	}
	return pixelu.LaunchOptions{Fullscreen: p.fullscreen()}
}
