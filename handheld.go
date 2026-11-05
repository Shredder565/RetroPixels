package main

// The 3DS / DS tab: Nintendo 3DS and DS games in one library, all played in
// PixelDS, RetroPixels' own DS and 3DS emulator, which comes in the download
// (internal/handheld). The games play in PixelDS's own window (fullscreen by
// default).

import (
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"retropixels/internal/handheld"
)

func handheldExe(e handheld.Engine) string {
	cfgMu.Lock()
	chosen := cfg.HandheldExe[e.ID]
	cfgMu.Unlock()
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	home, _ := os.UserHomeDir()
	return e.FindExe(chosen, appDir, home)
}

func handheldLibrary() []handheld.Game {
	cfgMu.Lock()
	folders := append([]string(nil), cfg.HandheldFolders...)
	played := map[string]time.Time{}
	for _, r := range cfg.HandheldRecent {
		played[strings.ToLower(r.Path)] = r.LastPlayed
	}
	cfgMu.Unlock()
	return handheld.Library(folders, played)
}

// Call with cfgMu held.
func handheldFullscreenOn() bool { return cfg.HandheldFullscreen == nil || *cfg.HandheldFullscreen }

func handheldRunning(e handheld.Engine) bool {
	runMu.Lock()
	r := running != nil && running.Kind == "handheld"
	runMu.Unlock()
	if r {
		return true
	}
	for _, n := range e.Exes {
		if processRunning(n) {
			return true
		}
	}
	return false
}

// handheldEngineFor: the emulator for a game's system (PixelDS) and its
// program.
func handheldEngineFor(system string) (handheld.Engine, string) {
	e := handheld.EngineFor(system)
	return e, handheldExe(e)
}

// launchHandheldGame starts a DS or 3DS game in PixelDS.
func launchHandheldGame(path string) error {
	g, ok := handheld.GameFor(path)
	if !ok {
		return errors.New("that isn't a 3DS or DS game")
	}
	e, exe := handheldEngineFor(g.System)
	if exe == "" {
		return errors.New(e.Name + " wasn't found. Keep its " + e.Dir + " folder next to " + exeFile("RetroPixels") +
			" (it comes in the RetroPixels download), or choose " + e.Exes[0] + " in the 3DS / DS tab")
	}
	if handheldRunning(e) {
		return errors.New(e.Name + " is already open. Close it first, then start the game here")
	}
	cfgMu.Lock()
	fs := handheldFullscreenOn()
	cfgMu.Unlock()
	label := e.Name
	args := handheld.GameArgs(e, g, fs)
	// PixelDS records the game itself when RetroPixels records games
	var rec *recSession
	if e.ID == handheld.PixelDS.ID && (autoRecordOn() || manualWanted()) {
		var err error
		var recArgs []string
		if rec, recArgs, err = pixeldsRecording(g.Name); err != nil {
			return err
		}
		args = append(recArgs, args...)
	}
	if err := startEmulator(exe, args, path, g.Name, "handheld", label, rec, nil); err != nil {
		if rec != nil {
			os.RemoveAll(rec.dumpDir)
		}
		return err
	}
	if rec != nil {
		clearManualWanted()
	}
	cfgMu.Lock()
	defer cfgMu.Unlock()
	list := []recentGame{{Path: path, Name: g.Name, LastPlayed: time.Now()}}
	for _, r := range cfg.HandheldRecent {
		if !strings.EqualFold(r.Path, path) {
			list = append(list, r)
		}
	}
	if len(list) > 50 {
		list = list[:50]
	}
	cfg.HandheldRecent = list
	return saveConfigLocked()
}

// ---------------- routes ----------------

func handheldStatus() map[string]any {
	cfgMu.Lock()
	st := map[string]any{"folders": append([]string{}, cfg.HandheldFolders...), "fullscreen": handheldFullscreenOn()}
	cfgMu.Unlock()
	st["pixelds"] = handheldExe(handheld.PixelDS)
	return st
}

func registerHandheldRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/handheld/library", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"games": handheldLibrary(), "status": handheldStatus()})
	}))
	mux.HandleFunc("/api/handheld/play", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Path string `json:"path"`
		}
		if !readBody(w, r, &b) {
			return
		}
		// Only games from the player's folders (or chosen in the dialog).
		found := false
		for _, g := range handheldLibrary() {
			if strings.EqualFold(g.Path, b.Path) {
				found = true
			}
		}
		if !found {
			fail(w, errors.New("that game isn't in your 3DS / DS games folders"))
			return
		}
		if err := launchHandheldGame(b.Path); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/handheld/action", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
			Engine string `json:"engine"`
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
		e, okEngine := handheld.EngineByID(b.Engine)
		switch b.Action {
		case "choose-exe":
			if !okEngine {
				err = errors.New("unknown emulator")
				break
			}
			var p string
			p, err = pickFile("Choose "+e.Exes[0], programFilter(e.Name, e.Exes[0]), filepath.Dir(handheldExe(e)))
			if err == nil && p != "" {
				if err = checkProgram(p); err == nil {
					set(func() {
						if cfg.HandheldExe == nil {
							cfg.HandheldExe = map[string]string{}
						}
						cfg.HandheldExe[e.ID] = p
					})
				}
			}
		case "add-folder":
			var d string
			d, err = pickFolder("Add a folder with 3DS or DS games", "")
			if err == nil && d != "" {
				set(func() {
					if !containsFold(cfg.HandheldFolders, d) {
						cfg.HandheldFolders = append(cfg.HandheldFolders, d)
					}
				})
			}
		case "remove-folder":
			set(func() {
				out := cfg.HandheldFolders[:0]
				for _, f := range cfg.HandheldFolders {
					if !strings.EqualFold(f, b.Folder) {
						out = append(out, f)
					}
				}
				cfg.HandheldFolders = out
			})
		case "fullscreen":
			v := b.On
			set(func() { cfg.HandheldFullscreen = &v })
		case "open-emu": // the emulator by itself, for its settings (controls, graphics, BIOS)
			if !okEngine {
				err = errors.New("unknown emulator")
				break
			}
			exe := handheldExe(e)
			switch {
			case exe == "":
				err = errors.New(e.Name + " wasn't found")
			case handheldRunning(e):
				err = errors.New(e.Name + " is already open")
			default:
				err = handheld.Command(exe).Start()
			}
		case "open-folder":
			if !okEngine {
				err = errors.New("unknown emulator")
			} else if exe := handheldExe(e); exe == "" {
				err = errors.New(e.Name + " wasn't found")
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
		writeJSON(w, 200, map[string]any{"ok": true, "games": handheldLibrary(), "status": handheldStatus()})
	}))
}

// pixeldsRecording sets up a recording of the 3DS or DS game about to
// start in PixelDS: a folder of its own for PixelDS's picture and sound
// (it encodes the picture with ffmpeg while the game runs). Without
// ffmpeg the game isn't recorded and ffmpeg is downloaded for next time.
func pixeldsRecording(name string) (*recSession, []string, error) {
	ff := findFFmpeg()
	if ff == "" {
		startFFmpegInstall()
		setRecError("Recording needs ffmpeg, which is downloading now (see the Recordings tab), so this game isn't being recorded. " +
			"3DS and DS games are recorded from the next one you start once it's ready.")
		return nil, nil, nil
	}
	tmp := filepath.Join(recordingsDir(), "temp")
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return nil, nil, errors.New("couldn't make the recordings folder: " + err.Error())
	}
	dir, err := os.MkdirTemp(tmp, "PixelDS-")
	if err != nil {
		return nil, nil, errors.New("couldn't make the recordings folder: " + err.Error())
	}
	os.MkdirAll(filepath.Join(dir, "Frames"), 0o755)
	os.MkdirAll(filepath.Join(dir, "Audio"), 0o755)
	rec := newRecSession(name, dir)
	// PixelDS's screens are small pixel pictures: the .mp4 is made at the
	// best quality and scaled up to 1080p or more with crisp pixels.
	rec.Emu, rec.Quality, rec.NoCrop, rec.Sharp, rec.ownDir = "PixelDS", "best", true, true, true
	return rec, []string{"-record", dir, "-ffmpeg", ff}, nil
}
