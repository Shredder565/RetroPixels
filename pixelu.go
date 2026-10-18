package main

// Wii U games through PixelU, RetroPixels' Wii U emulator (based on Cemu,
// built from pixelu/). It works like the Dolphin launcher in dolphin.go:
// PixelU.exe is found next to RetroPixels or in Downloads (or chosen), games
// are listed from the player's folders, and a game starts with PixelU's own
// command line through startEmulator. The finding, listing and command line
// are in internal/pixelu (RetroPixels Native uses them too).
//
// RetroPixels' own Go Wii U emulator (wiiu/, wiiuhost.go) is still there,
// but the Wii U tab no longer uses it.

import (
	"errors"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"retropixels/internal/pixelu"
)

func pixeluExe() string {
	cfgMu.Lock()
	chosen := cfg.PixelUExe
	cfgMu.Unlock()
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	home, _ := os.UserHomeDir() // %USERPROFILE% on Windows
	return pixelu.FindExe(chosen, appDir, home)
}

func pixeluLibrary() []pixelu.Game {
	cfgMu.Lock()
	folders := append([]string(nil), cfg.PixelUFolders...)
	played := map[string]time.Time{}
	for _, r := range cfg.PixelURecent {
		played[strings.ToLower(r.Path)] = r.LastPlayed
	}
	cfgMu.Unlock()
	return pixelu.Library(folders, played)
}

func pixeluRunning() bool {
	runMu.Lock()
	r := running != nil && running.Kind == "pixelu"
	runMu.Unlock()
	return r || processRunning(pixelu.ExeName)
}

// Settings, with their defaults. Call with cfgMu held.
func pixeluFullscreenOn() bool { return cfg.PixelUFullscreen == nil || *cfg.PixelUFullscreen }
func pixeluEmbedOn() bool      { return cfg.PixelUEmbed == nil || *cfg.PixelUEmbed }
func pixeluRecordOn() bool     { return cfg.PixelURecord != nil && *cfg.PixelURecord }

// pixeluRecording sets up a recording of the game about to start: a folder
// of its own for PixelU's raw files, and ffmpeg (PixelU encodes with it while
// the game runs). Without ffmpeg the game isn't recorded and ffmpeg is
// downloaded for next time.
func pixeluRecording(name string) (*recSession, pixelu.LaunchOptions, error) {
	var o pixelu.LaunchOptions
	ff := findFFmpeg()
	if ff == "" {
		startFFmpegInstall()
		setRecError("Recording needs ffmpeg, which is downloading now (see the Recordings tab), so this game isn't being recorded. " +
			"Wii U games are recorded from the next one you start once it's ready.")
		return nil, o, nil
	}
	tmp := filepath.Join(recordingsDir(), "temp")
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return nil, o, errors.New("couldn't make the recordings folder: " + err.Error())
	}
	dir, err := os.MkdirTemp(tmp, "PixelU-")
	if err != nil {
		return nil, o, errors.New("couldn't make the recordings folder: " + err.Error())
	}
	os.MkdirAll(filepath.Join(dir, "Frames"), 0o755)
	os.MkdirAll(filepath.Join(dir, "Audio"), 0o755)
	rec := newRecSession(name, dir)
	// Just the game's TV picture at its own size, so nothing is cropped,
	// and always the high-quality setting.
	rec.Emu, rec.Quality, rec.NoCrop, rec.ownDir = "PixelU", "high", true, true
	o.RecordDir, o.FFmpeg = dir, ff
	return rec, o, nil
}

func launchPixelUGame(path string) error {
	exe := pixeluExe()
	if exe == "" {
		return errors.New("PixelU isn't set up yet — choose PixelU.exe in the Wii U tab")
	}
	var g *pixelu.Game
	for _, x := range pixeluLibrary() {
		if strings.EqualFold(x.Path, path) {
			x := x
			g = &x
		}
	}
	if g == nil {
		return errors.New("that game isn't in your Wii U games folders")
	}
	if pixeluRunning() {
		return errors.New("PixelU is already open. Close it first, then start the game here")
	}
	cfgMu.Lock()
	fs, embed, record := pixeluFullscreenOn(), pixeluEmbedOn(), pixeluRecordOn()
	cfgMu.Unlock()
	var rec *recSession
	var opts pixelu.LaunchOptions
	if record || manualWanted() {
		var err error
		if rec, opts, err = pixeluRecording(g.Name); err != nil {
			return err
		}
	}
	// Docked into this window, PixelU stays a plain window and this window
	// goes fullscreen instead (as with Dolphin).
	opts.Fullscreen = fs && !embed
	opts.Embed = embed
	var onStart func(pid int, exited <-chan struct{})
	if embed {
		onStart = embedDolphin // docks PixelU's window the same way
	}
	if err := startEmulator(exe, pixelu.GameArgs(path, opts), path, g.Name, "pixelu", "PixelU", rec, onStart); err != nil {
		if rec != nil {
			os.RemoveAll(rec.dumpDir)
		}
		return err
	}
	clearManualWanted()
	cfgMu.Lock()
	defer cfgMu.Unlock()
	list := []recentGame{{Path: path, Name: g.Name, LastPlayed: time.Now()}}
	for _, r := range cfg.PixelURecent {
		if !strings.EqualFold(r.Path, path) {
			list = append(list, r)
		}
	}
	if len(list) > 50 {
		list = list[:50]
	}
	cfg.PixelURecent = list
	return saveConfigLocked()
}

// openPixelUUI opens PixelU by itself, for its own settings (controllers,
// graphics packs, ...).
func openPixelUUI() error {
	exe := pixeluExe()
	if exe == "" {
		return errors.New("PixelU isn't set up yet")
	}
	if pixeluRunning() {
		return errors.New("PixelU is already open")
	}
	cmd := exec.Command(exe)
	cmd.Dir = filepath.Dir(exe)
	return cmd.Start()
}

// ---------------- routes ----------------

func pixeluStatus() map[string]any {
	cfgMu.Lock()
	st := map[string]any{"folders": append([]string{}, cfg.PixelUFolders...), "fullscreen": pixeluFullscreenOn(),
		"embed": pixeluEmbedOn(), "record": pixeluRecordOn()}
	cfgMu.Unlock()
	st["exe"] = pixeluExe()
	return st
}

func registerPixelURoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/pixelu/library", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"games": pixeluLibrary(), "status": pixeluStatus()})
	}))
	mux.HandleFunc("/api/pixelu/play", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Path string `json:"path"`
		}
		if !readBody(w, r, &b) {
			return
		}
		if err := launchPixelUGame(b.Path); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/pixelu/action", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
			Folder string `json:"folder"`
			On     bool   `json:"on"`
		}
		if !readBody(w, r, &b) {
			return
		}
		var err error
		set := func(f func()) {
			cfgMu.Lock()
			f()
			_ = saveConfigLocked()
			cfgMu.Unlock()
		}
		switch b.Action {
		case "choose-exe":
			var p string
			p, err = pickFile("Choose PixelU.exe", "PixelU (PixelU.exe)|PixelU.exe|Programs (*.exe)|*.exe", filepath.Dir(pixeluExe()))
			if err == nil && p != "" {
				if !strings.EqualFold(filepath.Ext(p), ".exe") {
					err = errors.New("that isn't a program (.exe)")
				} else {
					set(func() { cfg.PixelUExe = p })
				}
			}
		case "add-folder":
			var d string
			d, err = pickFolder("Add a folder with Wii U games", "")
			if err == nil && d != "" {
				set(func() {
					if !containsFold(cfg.PixelUFolders, d) {
						cfg.PixelUFolders = append(cfg.PixelUFolders, d)
					}
				})
			}
		case "remove-folder":
			set(func() {
				out := cfg.PixelUFolders[:0]
				for _, f := range cfg.PixelUFolders {
					if !strings.EqualFold(f, b.Folder) {
						out = append(out, f)
					}
				}
				cfg.PixelUFolders = out
			})
		case "fullscreen":
			v := b.On
			set(func() { cfg.PixelUFullscreen = &v })
		case "embed":
			v := b.On
			set(func() { cfg.PixelUEmbed = &v })
		case "record":
			v := b.On
			set(func() { cfg.PixelURecord = &v })
		case "open-pixelu":
			err = openPixelUUI()
		case "open-folder":
			if exe := pixeluExe(); exe == "" {
				err = errors.New("PixelU isn't set up yet")
			} else {
				err = openFolder(filepath.Dir(exe))
			}
		default:
			err = errors.New("unknown action")
		}
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true, "games": pixeluLibrary(), "status": pixeluStatus()})
	}))
}
