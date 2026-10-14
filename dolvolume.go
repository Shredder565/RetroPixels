package main

// GameCube / Wii volume: one setting, kept in RetroPixels' config, applied to
// Dolphin's sound while a game plays (Windows volume mixer, see dolvolume_windows.go).

import (
	"errors"
	"math"
	"net/http"
)

// dolphinVolume returns the volume (0–100) and whether the sound is muted.
func dolphinVolume() (int, bool) {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	v := 100
	if cfg.DolphinVolume != nil {
		v = *cfg.DolphinVolume
	}
	return v, cfg.DolphinMuted
}

// setDolphinVolume saves the volume and applies it to a game that's playing.
// A nil value leaves that part as it is.
func setDolphinVolume(volume *float64, muted *bool) error {
	if volume != nil && (math.IsNaN(*volume) || *volume < 0 || *volume > 100) {
		return errors.New("volume must be between 0 and 100")
	}
	cfgMu.Lock()
	if volume != nil {
		v := int(math.Round(*volume))
		cfg.DolphinVolume = &v
	}
	if muted != nil {
		cfg.DolphinMuted = *muted
	}
	err := saveConfigLocked()
	cfgMu.Unlock()
	applyDolphinVolumeNow()
	return err
}

func volumePayload() map[string]any {
	v, m := dolphinVolume()
	return map[string]any{"volume": v, "muted": m}
}

func registerDolphinVolumeRoute(mux *http.ServeMux) {
	mux.HandleFunc("/api/dol/volume", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			var b struct {
				Volume *float64 `json:"volume"`
				Muted  *bool    `json:"muted"`
			}
			if !readBody(w, r, &b) {
				return
			}
			if err := setDolphinVolume(b.Volume, b.Muted); err != nil {
				fail(w, err)
				return
			}
		}
		writeJSON(w, 200, volumePayload())
	}))
}
