package main

// Star Wars Trilogy Arcade (Sega Model 3) in Supermodel: the Supermodel tab.
// It follows the other emulators' rules: Supermodel is found next to
// RetroPixels (it comes in the download, built from supermodel/) or chosen,
// the game is found once in the folders RetroPixels already looks in (or
// chosen) and remembered, and clicking its tile starts it through
// startProcess: one game at a time, docked inside the RetroPixels window
// (or fullscreen in Supermodel's own), recorded like the others, and
// with no console window. Supermodel's settings, NVRAM (the game's own
// settings and high scores) and save states are kept in
// %APPDATA%\RetroPixels\Supermodel (~/.config/RetroPixels/Supermodel on
// Linux). The finding, checking, settings and
// command line are in internal/supermodel.

import (
	"bufio"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"retropixels/internal/dolphin"
	"retropixels/internal/mame"
	"retropixels/internal/supermodel"
)

var (
	smMu       sync.Mutex
	smFinding  bool
	smSearched bool              // looked for the game in the usual folders since RetroPixels started
	smCheck    *supermodel.Check // the last check of the game file
	smProblem  map[string]any    // why the last game didn't start
)

// supermodelHome is the player's Supermodel folder (SUPERMODEL_HOME).
func supermodelHome() string {
	if d := os.Getenv("RETRO_SUPERMODEL_HOME"); d != "" { // tests
		return d
	}
	return filepath.Join(configDir(), "Supermodel")
}

func supermodelExe() string {
	cfgMu.Lock()
	chosen := cfg.SupermodelExe
	cfgMu.Unlock()
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	home, _ := os.UserHomeDir()
	return supermodel.FindExe(chosen, appDir, home)
}

// supermodelFolders: where the game is looked for: the folders RetroPixels
// already looks in for games (MAME's ROM folders first, since it's a MAME
// ROM set), the ROM folders next to RetroPixels and Supermodel, and the
// usual places for downloads.
func supermodelFolders() []string {
	var out []string
	add := func(p ...string) {
		for _, d := range p {
			if d != "" && !containsFold(out, d) {
				out = append(out, d)
			}
		}
	}
	mc := mame.Load()
	add(mc.Folders...)
	add(mame.BundleRoms())
	if exe := supermodelExe(); exe != "" {
		add(filepath.Join(filepath.Dir(exe), "ROMs"))
	}
	if exe, err := os.Executable(); err == nil {
		d := filepath.Dir(exe)
		add(filepath.Join(d, "ROMs"), filepath.Join(d, "roms"))
	}
	cfgMu.Lock()
	add(cfg.PixelUFolders...)
	add(cfg.DolphinFolders...)
	cfgMu.Unlock()
	if home, err := os.UserHomeDir(); err == nil {
		// The desktop's own Downloads, Desktop and Documents (on Linux they
		// can have other names; see dolphin.UserFolder).
		add(dolphin.DownloadDirs(home)...)
		add(dolphin.UserFolder(home, "Desktop"), dolphin.UserFolder(home, "Documents"),
			filepath.Join(home, "ROMs"), filepath.Join(home, "roms"))
	}
	return out
}

// supermodelROM is the remembered game file and what's in it ("" when
// there's none yet).
func supermodelROM() (string, *supermodel.Check) {
	cfgMu.Lock()
	p := cfg.SupermodelROM
	cfgMu.Unlock()
	if p == "" {
		return "", nil
	}
	smMu.Lock()
	c := smCheck
	smMu.Unlock()
	if c == nil || c.Path != p {
		cc := supermodel.CheckROM(p)
		c = &cc
		smMu.Lock()
		smCheck = c
		smMu.Unlock()
	}
	return p, c
}

// supermodelFind looks for the game in the usual folders (in the
// background) and remembers it: the first copy found that plays, or else
// the first one found, so the tab can say what's wrong with it.
func supermodelFind(force bool) {
	smMu.Lock()
	if smFinding || (smSearched && !force) {
		smMu.Unlock()
		return
	}
	smFinding = true
	smMu.Unlock()
	go func() {
		p, c := supermodel.FindROM(supermodelFolders())
		if !c.OK {
			// Not in the usual places: look through each drive's game folders.
			if q, d := supermodel.FindROMOnDrives(dolphin.WiiKeysScanRoots()); q != "" && (d.OK || p == "") {
				p, c = q, d
			}
		}
		cfgMu.Lock()
		cur := cfg.SupermodelROM
		cfgMu.Unlock()
		// A remembered copy that plays stays; otherwise a copy that plays
		// replaces it, and any copy is better than none (its check says
		// what's wrong with it).
		keep := cur != "" && supermodel.CheckROM(cur).OK
		if p != "" && !keep && (c.OK || cur == "") {
			cfgMu.Lock()
			cfg.SupermodelROM = p
			_ = saveConfigLocked()
			cfgMu.Unlock()
		}
		smMu.Lock()
		smFinding, smSearched, smCheck = false, true, nil
		smMu.Unlock()
	}()
}

// useSupermodelROM remembers a game file the player chose, if it's the game.
func useSupermodelROM(p string) error {
	c := supermodel.CheckROM(p)
	if c.Set == "" {
		return errors.New(filepath.Base(p) + ": " + c.Problem)
	}
	cfgMu.Lock()
	cfg.SupermodelROM = p
	err := saveConfigLocked()
	cfgMu.Unlock()
	smMu.Lock()
	smCheck = &c
	smMu.Unlock()
	return err
}

// Settings, with their defaults. Call with cfgMu held.
func supermodelFullscreenOn() bool {
	return cfg.SupermodelFullscreen == nil || *cfg.SupermodelFullscreen
}

// supermodelFlightOn: the Xbox thumbstick feels like a flight stick (on
// unless the player turned it off).
// smoothName: the flight stick smoothing in use (DefaultSmoothness when none
// or an unknown one is saved).
func smoothName(s string) string {
	if _, ok := supermodel.Smoothness[s]; ok {
		return s
	}
	return supermodel.DefaultSmoothness
}

func supermodelFlightOn() bool { return cfg.SupermodelPadStick == nil || !*cfg.SupermodelPadStick }

func supermodelEmbedOn() bool { return cfg.SupermodelDock != nil && *cfg.SupermodelDock }

func supermodelRunning() bool {
	runMu.Lock()
	r := running != nil && running.Kind == "supermodel"
	runMu.Unlock()
	return r || processRunning(supermodel.ExeName)
}

// supermodelRecording: a folder of its own for Supermodel's raw files, and
// ffmpeg (Supermodel encodes the picture with it while the game runs). As
// with PixelU, without ffmpeg the game isn't recorded and ffmpeg is
// downloaded for next time.
func supermodelRecording() (*recSession, string, string, error) {
	ff := findFFmpeg()
	if ff == "" {
		startFFmpegInstall()
		setRecError("Recording needs ffmpeg, which is downloading now (see the Recordings tab), so this game isn't being recorded. " +
			"It's recorded from the next time you start it once ffmpeg is ready.")
		return nil, "", "", nil
	}
	tmp := filepath.Join(recordingsDir(), "temp")
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return nil, "", "", errors.New("couldn't make the recordings folder: " + err.Error())
	}
	dir, err := os.MkdirTemp(tmp, "Supermodel-")
	if err != nil {
		return nil, "", "", errors.New("couldn't make the recordings folder: " + err.Error())
	}
	os.MkdirAll(filepath.Join(dir, "Frames"), 0o755)
	os.MkdirAll(filepath.Join(dir, "Audio"), 0o755)
	rec := newRecSession(supermodel.Title, dir)
	// The window's picture: black bars beside the 4:3 game are cropped off.
	rec.Emu, rec.Quality, rec.ownDir = "Supermodel", "high", true
	return rec, dir, ff, nil
}

// launchSupermodel starts Star Wars Trilogy Arcade.
func launchSupermodel() error {
	exe := supermodelExe()
	if exe == "" {
		return errors.New("Supermodel isn't here: it comes in the RetroPixels download (the Supermodel folder next to " + exeFile("RetroPixels") + "). " +
			"Unpack the whole download, or choose a " + supermodel.ExeName)
	}
	rom, c := supermodelROM()
	if rom == "" {
		return errNoSupermodelROM
	}
	if _, err := os.Stat(rom); err != nil {
		return errors.New("the game file isn't there any more (" + rom + "). Choose it again")
	}
	if c != nil && !c.OK {
		return errors.New(filepath.Base(rom) + ": " + c.Problem)
	}
	if supermodelRunning() {
		return errors.New("Supermodel is already open. Close it first, then start the game here")
	}
	patched := supermodel.Patched(exe)
	home := supermodelHome()
	if err := os.MkdirAll(home, 0o755); err != nil {
		return errors.New("couldn't make Supermodel's folder: " + err.Error())
	}
	cfgMu.Lock()
	fs, embed, wide, invert, flight := supermodelFullscreenOn(), supermodelEmbedOn(), cfg.SupermodelWide, cfg.SupermodelInvertY, supermodelFlightOn()
	cfgMu.Unlock()
	flight = flight && patched
	if _, err := supermodel.WriteINI(supermodel.ConfigFile(home, exe), supermodel.Controls{InvertY: invert, FlightStick: flight}); err != nil {
		return errors.New("couldn't write Supermodel's settings: " + err.Error())
	}
	// Docked, Supermodel draws at the size of the page (RetroPixels' own
	// build follows its window's size), and this window goes fullscreen.
	embed = embed && patched
	o := supermodel.LaunchOptions{Embed: embed, Fullscreen: fs && !embed, WideScreen: wide,
		ForceFeedback: c != nil && c.DriveBoard, Patched: patched}
	if embed {
		o.Hidden = hostWindowPresent() // off-screen until it's docked
	}
	var rec *recSession
	if patched && recordingWanted() {
		var err error
		if rec, o.RecordDir, o.FFmpeg, err = supermodelRecording(); err != nil {
			return err
		}
	}
	cmd, out := supermodel.Command(exe, home, supermodel.GameArgs(rom, o)...)
	cfgMu.Lock()
	smooth := cfg.SupermodelSmooth
	cfgMu.Unlock()
	cmd.Env = append(cmd.Env, supermodel.FlightStickEnv(flight, smooth))
	var onStart func(pid int, exited <-chan struct{})
	if embed {
		onStart = embedSupermodel
	}
	smMu.Lock()
	smProblem = nil
	smMu.Unlock()
	logFile := supermodel.LogFile(home, exe)
	onExit := func(err error, ran time.Duration) {
		if ran > 20*time.Second {
			return // it played
		}
		why := supermodel.Problem(out.String() + "\n" + logErrors(logFile))
		if why == "" {
			if err == nil {
				return
			}
			why = "Supermodel closed straight away (" + err.Error() + ")."
		}
		smMu.Lock()
		smProblem = map[string]any{"text": why, "at": time.Now(), "log": logFile}
		smMu.Unlock()
	}
	if err := startProcess(cmd, rom, supermodel.Title, "supermodel", "Supermodel", rec, onStart, onExit); err != nil {
		if rec != nil {
			os.RemoveAll(rec.dumpDir)
		}
		return err
	}
	clearManualWanted()
	cfgMu.Lock()
	defer cfgMu.Unlock()
	cfg.SupermodelPlayed = time.Now()
	return saveConfigLocked()
}

var errNoSupermodelROM = errors.New("RetroPixels hasn't found Star Wars Trilogy Arcade yet (swtrilgya.zip or swtrilgy.zip). " +
	"Click \"Choose the game file…\" to pick it")

// logErrors: the error lines of Supermodel's log (it writes why a game
// didn't start there as well as to its console).
func logErrors(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	var lines []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if l := sc.Text(); strings.Contains(strings.ToLower(l), "error") {
			lines = append(lines, l)
		}
	}
	if len(lines) > 20 {
		lines = lines[len(lines)-20:]
	}
	return strings.Join(lines, "\n")
}

// supermodelVersion: the first line of VERSION.txt next to it.
func supermodelVersion(exe string) string {
	b, err := os.ReadFile(filepath.Join(filepath.Dir(exe), "VERSION.txt"))
	if err != nil {
		return ""
	}
	l, _, _ := strings.Cut(strings.ReplaceAll(string(b), "\r", ""), "\n")
	return strings.TrimSpace(l)
}

func supermodelStatus() map[string]any {
	exe := supermodelExe()
	rom, c := supermodelROM()
	cfgMu.Lock()
	st := map[string]any{"fullscreen": supermodelFullscreenOn(), "embed": supermodelEmbedOn(), "wide": cfg.SupermodelWide,
		"invertY": cfg.SupermodelInvertY, "flight": supermodelFlightOn(), "smooth": smoothName(cfg.SupermodelSmooth), "record": pixeluRecordOn(), "played": cfg.SupermodelPlayed, "chosenExe": cfg.SupermodelExe != ""}
	cfgMu.Unlock()
	smMu.Lock()
	st["finding"], st["searched"], st["problem"] = smFinding, smSearched, smProblem
	smMu.Unlock()
	st["exe"], st["rom"], st["check"], st["home"], st["title"] = exe, rom, c, supermodelHome(), supermodel.Title
	if exe != "" {
		st["patched"], st["version"] = supermodel.Patched(exe), supermodelVersion(exe)
	}
	return st
}

func registerSupermodelRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/supermodel/status", guard(func(w http.ResponseWriter, r *http.Request) {
		if rom, _ := supermodelROM(); rom == "" {
			supermodelFind(false) // the first time the tab opens
		}
		writeJSON(w, 200, supermodelStatus())
	}))
	mux.HandleFunc("/api/supermodel/play", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if err := launchSupermodel(); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/supermodel/action", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
			On     bool   `json:"on"`
			Value  string `json:"value"`
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
		on := b.On
		switch b.Action {
		case "choose-rom":
			start := ""
			if rom, _ := supermodelROM(); rom != "" {
				start = filepath.Dir(rom)
			}
			var p string
			p, err = pickFile("Choose Star Wars Trilogy Arcade (swtrilgya.zip or swtrilgy.zip)", "Star Wars Trilogy (swtrilgy*.zip)|swtrilgy*.zip|ZIP files (*.zip)|*.zip", start)
			if err == nil && p != "" {
				err = useSupermodelROM(p)
			}
		case "find-rom":
			supermodelFind(true)
		case "choose-exe":
			var p string
			p, err = pickFile("Choose Supermodel ("+supermodel.ExeName+")", programFilter("Supermodel", supermodel.ExeName), filepath.Dir(supermodelExe()))
			if err == nil && p != "" {
				if err = checkProgram(p); err == nil {
					set(func() { cfg.SupermodelExe = p })
				}
			}
		case "default-exe":
			set(func() { cfg.SupermodelExe = "" })
		case "fullscreen":
			set(func() { cfg.SupermodelFullscreen = &on })
		case "embed":
			set(func() { cfg.SupermodelDock = &on })
		case "wide":
			set(func() { cfg.SupermodelWide = on })
		case "flight-stick":
			off := !on
			set(func() { cfg.SupermodelPadStick = &off })
		case "smoothness":
			if _, ok := supermodel.Smoothness[b.Value]; !ok {
				err = errors.New("unknown smoothness: " + b.Value)
			} else {
				set(func() { cfg.SupermodelSmooth = b.Value })
			}
		case "invert-y":
			set(func() { cfg.SupermodelInvertY = on })
		case "record":
			set(func() { cfg.AutoRecord = &on }) // the one "record games automatically" setting
		case "open-saves":
			home := supermodelHome()
			os.MkdirAll(home, 0o755)
			err = openFolder(home)
		case "open-folder":
			if exe := supermodelExe(); exe == "" {
				err = errors.New("Supermodel isn't set up yet")
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
		writeJSON(w, 200, supermodelStatus())
	}))
}
