package main

// Star Wars Trilogy Arcade (Sega Model 3) through Supermodel
// (internal/supermodel), as RetroPixels.exe's Supermodel tab plays it: the
// settings kept in settings.json, which files go to Supermodel and how the
// game is started. supermodel_windows.go has the menu and the window.

import (
	"encoding/json"
	"os"
	"strings"

	"retropixels/internal/supermodel"
)

// SupermodelSettings: settings.json's "supermodel" part. Unset switches take
// RetroPixels.exe's defaults: fullscreen in Supermodel's own window, the
// Xbox thumbstick feeling like a flight stick, medium smoothing.
type SupermodelSettings struct {
	Exe         string `json:"exe,omitempty"` // supermodel.exe chosen by the player ("" = the one next to RetroPixels)
	ROM         string `json:"rom,omitempty"` // the game's .zip, once found or chosen
	Fullscreen  *bool  `json:"fullscreen,omitempty"`
	Embed       *bool  `json:"playInWindow,omitempty"` // unset = off: fullscreen in Supermodel's own window
	FlightStick *bool  `json:"flightStick,omitempty"`  // unset = on
	Smooth      string `json:"smoothness,omitempty"`   // off, light, medium, heavy; unset = medium
	InvertY     bool   `json:"invertY,omitempty"`      // stick up dives, like the arcade's flight yoke
	Wide        bool   `json:"wideScreen,omitempty"`   // the 3D view fills a wide screen
}

func (s *SupermodelSettings) fullscreen() bool { return boolOr(s.Fullscreen, true) }
func (s *SupermodelSettings) embed() bool      { return boolOr(s.Embed, false) }
func (s *SupermodelSettings) flight() bool     { return boolOr(s.FlightStick, true) }
func (s *SupermodelSettings) smooth() string   { return supermodel.SmoothnessName(s.Smooth) }

// smoothOrder: the smoothing choices, as the menu lists them.
var smoothOrder = []string{"off", "light", "medium", "heavy"}

// isSupermodelGame: the file is Star Wars Trilogy Arcade's ROM set
// (swtrilgy.zip or swtrilgya.zip), which plays in Supermodel; every other
// file (and every other .zip) goes where it went before.
func isSupermodelGame(path string) bool {
	return supermodel.IsRomFile(path[strings.LastIndexAny(path, `/\`)+1:]) // the file's name, with either slash
}

// smPlan is how Supermodel is started for one game.
type smPlan struct {
	Args     []string            // Supermodel's command line
	Controls supermodel.Controls // what goes in Supermodel.ini
	Env      string              // the flight stick variable (FlightStickEnv)
	Embed    bool                // docked inside this window
}

// plan builds the start of the game in rom (c is its check) for a
// Supermodel build (patched: RetroPixels' own, which has the flight stick,
// docking and recording). recDir and ff: record into recDir with that
// ffmpeg ("" = not recorded). The same choices as RetroPixels.exe's
// launchSupermodel.
func (s *SupermodelSettings) plan(rom string, c supermodel.Check, patched bool, recDir, ff string) smPlan {
	flight := s.flight() && patched
	embed := s.embed() && patched
	o := supermodel.LaunchOptions{
		Embed:         embed,
		Hidden:        embed, // off-screen until it's docked
		Fullscreen:    s.fullscreen() && !embed,
		WideScreen:    s.Wide,
		ForceFeedback: c.DriveBoard,
		Patched:       patched,
	}
	if patched && recDir != "" && ff != "" {
		o.RecordDir, o.FFmpeg = recDir, ff
	}
	return smPlan{
		Args:     supermodel.GameArgs(rom, o),
		Controls: supermodel.Controls{InvertY: s.InvertY, FlightStick: flight},
		Env:      supermodel.FlightStickEnv(flight, s.smooth()),
		Embed:    embed,
	}
}

// webSupermodelROM is the game file RetroPixels.exe found or was given
// (its config.json at path), "" when there's none.
func webSupermodelROM(path string) string {
	var c struct {
		ROM string `json:"supermodelRom"`
	}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &c)
	}
	return c.ROM
}
