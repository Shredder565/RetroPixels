package main

// Arcade games in MAME, any version of it (internal/mame): the MAME tab's
// versions, downloads, ROM folders and library, and starting a game in the
// MAME it uses, docked inside the RetroPixels window like Dolphin.

import (
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"retropixels/internal/mame"
)

var (
	mameMu      sync.Mutex
	mameGames   []mame.Game
	mameLoaded  bool
	mameLoading bool
	mameAgain   bool // the folders changed while a scan was running
	mameFinding bool // looking for MAME on this PC
	mameCheck   bool // asking each MAME which games it plays
	mameInst    *mame.Progress
	mameRels    []mame.Release
	mameRelsAt  time.Time
	mameProblem map[string]any // why the last game didn't start
)

// mameReload scans the ROM folders again in the background (asking each new
// MAME version for its game names the first time).
func mameReload() {
	mameMu.Lock()
	if mameLoading {
		mameAgain = true
		mameMu.Unlock()
		return
	}
	mameLoading = true
	mameMu.Unlock()
	go func() {
		for {
			c := mame.Load()
			games := mame.Library(c.Folders, mame.AllNames(c.Installs), c.Played)
			mameMu.Lock()
			if mameAgain {
				mameAgain = false
				mameMu.Unlock()
				continue
			}
			mameGames, mameLoaded, mameLoading, mameCheck = games, true, false, true
			mameMu.Unlock()
			mame.CheckGames(c, games)
			mameMu.Lock()
			mameCheck = false
			mameMu.Unlock()
			return
		}
	}()
}

// mameDetect looks for MAME on this PC once (the first time the tab opens).
func mameDetect() {
	mameMu.Lock()
	if mameFinding {
		mameMu.Unlock()
		return
	}
	mameFinding = true
	mameMu.Unlock()
	go func() {
		mame.Detect()
		mameMu.Lock()
		mameFinding = false
		mameMu.Unlock()
		mameReload()
	}()
}

func mameStatus() map[string]any {
	c := mame.Load()
	def, _ := c.DefaultInstall()
	var installs []map[string]any
	for _, in := range c.Installs {
		_, err := os.Stat(in.Exe)
		installs = append(installs, map[string]any{"exe": in.Exe, "version": in.Version, "label": in.Label(),
			"default": strings.EqualFold(in.Exe, def.Exe), "missing": err != nil})
	}
	games := map[string]string{}
	for k, v := range c.Games {
		games[k] = v
	}
	mameMu.Lock()
	defer mameMu.Unlock()
	var inst any
	if mameInst != nil {
		inst = *mameInst
	}
	return map[string]any{"installs": installs, "default": def.Exe, "folders": c.Folders, "embed": c.EmbedOn(),
		"gameVersion": games, "install": inst, "finding": mameFinding, "detected": c.Detected, "problem": mameProblem}
}

// mameRow is one game in the library, with the MAME it starts in and
// whether that version is known to play it.
type mameRow struct {
	mame.Game
	Plays mame.GameStatus `json:"plays"`
	Note  string          `json:"note"`
}

func mameLibraryPayload() map[string]any {
	st := mameStatus()
	mameMu.Lock()
	games, loaded, loading, checking := mameGames, mameLoaded, mameLoading, mameCheck
	mameMu.Unlock()
	c := mame.Load()
	rows := make([]mameRow, 0, len(games))
	for _, g := range games {
		s := c.Status(g)
		rows = append(rows, mameRow{Game: g, Plays: s, Note: s.Text()})
	}
	return map[string]any{"games": rows, "loaded": loaded, "loading": loading, "checking": checking, "status": st}
}

// launchMameGame starts a ROM set in its MAME (exe overrides the game's own
// pick for this time only).
func launchMameGame(path, exe string) error {
	if !mame.IsRomSet(path) {
		return errors.New("MAME plays ROM sets: .zip or .7z files named as MAME names the game (for example tmnt.zip)")
	}
	if _, err := os.Stat(path); err != nil {
		return errors.New("that game file isn't there any more")
	}
	c := mame.Load()
	embed := c.EmbedOn()
	l, err := mame.Plan(c, path, exe, mame.LaunchOptions{Embed: embed})
	if err != nil {
		return err
	}
	// Recording: MAME saves the game's own picture and sound (-aviwrite);
	// the .mp4 is made from it when the game ends (needs ffmpeg only then).
	args := l.Args
	recDir := ""
	if recordingWanted() {
		base := filepath.Join(recordingsDir(), "temp")
		os.MkdirAll(base, 0o755)
		if d, err := os.MkdirTemp(base, "MAME-"); err == nil {
			if ra := mame.RecordArgs(d, mame.OptionsOf(l.Install.Exe)); ra != nil {
				args = append(append([]string(nil), args...), ra...)
				recDir = d
			} else {
				os.Remove(d) // this MAME can't record itself
			}
		}
	}
	cmd, out := mame.Command(l.Install.Exe, args...)
	var onStart func(pid int, exited <-chan struct{})
	if embed {
		onStart = embedMAME // docks MAME's window over the page
	}
	mameMu.Lock()
	mameProblem = nil
	mameMu.Unlock()
	label := l.Install.Label()
	mameExe := l.Install.Exe
	onExit := func(err error, ran time.Duration) {
		if recDir != "" {
			go saveMameRecording(l.Name, recDir, mameExe)
		}
		if err == nil || ran > 20*time.Second {
			return // it played (MAME may still end with an error code, e.g. when it's closed for us)
		}
		why := mame.Problem(out.String())
		if why == "" {
			why = label + " closed straight away (" + err.Error() + ")."
		}
		mameMu.Lock()
		mameProblem = map[string]any{"game": l.Name, "version": label, "text": why, "at": time.Now()}
		mameMu.Unlock()
	}
	if err := startProcess(cmd, path, l.Name, "mame", label, nil, onStart, onExit); err != nil {
		if recDir != "" {
			os.RemoveAll(recDir)
		}
		return err
	}
	clearManualWanted()
	_, err = mame.Update(func(c *mame.Config) { c.MarkPlayed(l.Short, time.Now()) })
	mameMu.Lock()
	for i := range mameGames {
		if mameGames[i].Short == l.Short {
			mameGames[i].Played = time.Now()
		}
	}
	mameMu.Unlock()
	return err
}

// mameReleases lists the MAME team's ready-made releases (Windows only: on
// Linux the list is empty, and the page says to use the built-in MAME or the
// system's mame package).
func mameReleases(refresh bool) ([]mame.Release, error) {
	if !mame.DownloadsAvailable {
		return []mame.Release{}, nil
	}
	mameMu.Lock()
	if !refresh && mameRels != nil && time.Since(mameRelsAt) < time.Hour {
		r := mameRels
		mameMu.Unlock()
		return r, nil
	}
	mameMu.Unlock()
	r, err := mame.NewInstaller().Releases()
	if err != nil {
		return nil, err
	}
	mameMu.Lock()
	mameRels, mameRelsAt = r, time.Now()
	mameMu.Unlock()
	return r, nil
}

// startMameInstall downloads one official release in the background.
func startMameInstall(version string) error {
	if !mame.DownloadsAvailable {
		return mame.ErrNoDownloads
	}
	rels, err := mameReleases(false)
	if err != nil {
		return err
	}
	var rel *mame.Release
	for i := range rels {
		if rels[i].Version == version {
			rel = &rels[i]
		}
	}
	if rel == nil {
		return errors.New("MAME " + version + " isn't among the official Windows downloads")
	}
	mameMu.Lock()
	if mameInst != nil && mameInst.State != "done" && mameInst.State != "error" {
		mameMu.Unlock()
		return errors.New("a MAME download is already running")
	}
	mameInst = &mame.Progress{State: "downloading", Version: rel.Version, Total: rel.Size}
	mameMu.Unlock()
	go func() {
		in := mame.NewInstaller()
		in.Progress = func(p mame.Progress) {
			mameMu.Lock()
			mameInst = &p
			mameMu.Unlock()
		}
		exe, err := in.Install(*rel)
		mameMu.Lock()
		defer mameMu.Unlock()
		if err != nil {
			mameInst = &mame.Progress{State: "error", Version: rel.Version, Error: err.Error()}
			return
		}
		mameInst = &mame.Progress{State: "done", Version: rel.Version}
		ver := mame.ProbeVersion(exe)
		if ver == "" {
			ver = rel.Version
		}
		mame.Update(func(c *mame.Config) { c.Add(mame.Install{Exe: exe, Version: ver}) })
		go mameReload()
	}()
	return nil
}

func registerMameRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/mame/library", guard(func(w http.ResponseWriter, r *http.Request) {
		mameMu.Lock()
		loaded, loading := mameLoaded, mameLoading
		mameMu.Unlock()
		if !loaded && !loading {
			if c := mame.Load(); !c.Detected {
				mameDetect()
			} else {
				mameReload()
			}
		}
		writeJSON(w, 200, mameLibraryPayload())
	}))
	mux.HandleFunc("/api/mame/play", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Path string `json:"path"`
			Exe  string `json:"exe"`
		}
		if !readBody(w, r, &b) {
			return
		}
		if err := launchMameGame(b.Path, b.Exe); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/mame/releases", guard(func(w http.ResponseWriter, r *http.Request) {
		rels, err := mameReleases(r.URL.Query().Get("refresh") == "1")
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"releases": rels})
	}))
	mux.HandleFunc("/api/mame/install", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			var b struct {
				Version string `json:"version"`
			}
			if !readBody(w, r, &b) {
				return
			}
			if err := startMameInstall(b.Version); err != nil {
				fail(w, err)
				return
			}
		}
		writeJSON(w, 200, mameLibraryPayload())
	}))
	mux.HandleFunc("/api/mame/action", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
			Exe    string `json:"exe"`
			Folder string `json:"folder"`
			Short  string `json:"short"`
			On     bool   `json:"on"`
		}
		if !readBody(w, r, &b) {
			return
		}
		var err error
		reload := false
		switch b.Action {
		case "add-exe":
			start := ""
			if def, ok := mame.Load().DefaultInstall(); ok {
				start = filepath.Dir(filepath.Dir(def.Exe))
			}
			var p string
			p, err = pickFile("Choose a MAME ("+mame.ExeName+") to add", programFilter("MAME", mame.ExePattern), start)
			if err == nil && p != "" {
				_, err = mame.AddExe(p)
				reload = true
			}
		case "remove-exe":
			_, err = mame.Update(func(c *mame.Config) { c.Remove(b.Exe) })
		case "set-default":
			_, err = mame.Update(func(c *mame.Config) {
				if _, ok := c.Find(b.Exe); ok {
					c.Default = b.Exe
				}
			})
		case "set-game":
			_, err = mame.Update(func(c *mame.Config) {
				if _, ok := c.Find(b.Exe); ok || b.Exe == "" {
					c.SetGame(b.Short, b.Exe)
				}
			})
		case "add-folder":
			var d string
			d, err = pickFolder("Add a folder with MAME ROM sets (.zip)", "")
			if err == nil && d != "" {
				_, err = mame.Update(func(c *mame.Config) { c.AddFolder(d) })
				reload = true
			}
		case "remove-folder":
			_, err = mame.Update(func(c *mame.Config) { c.RemoveFolder(b.Folder) })
			reload = true
		case "find":
			mameDetect()
		case "rescan":
			reload = true
		case "embed":
			_, err = mame.Update(func(c *mame.Config) { v := b.On; c.Embed = &v })
		case "open-file":
			var p string
			p, err = pickFile("Choose an arcade game (MAME ROM set)", "MAME ROM sets (*.zip;*.7z)|*.zip;*.7z", "")
			if err == nil && p != "" {
				err = launchMameGame(p, "")
			}
		case "open-folder":
			err = openFolder(filepath.Dir(b.Exe))
		case "stop-game":
			err = stopEmbeddedGame()
		default:
			err = errors.New("unknown action")
		}
		if reload {
			mameReload()
		}
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, mameLibraryPayload())
	}))
}

// saveMameRecording makes the .mp4 from the AVIs MAME saved in dir once the
// game has ended, in the recordings folder, then removes dir.
func saveMameRecording(game, dir, exe string) {
	recMu.Lock()
	recQueued++
	recMu.Unlock()
	saveMu.Lock()
	defer saveMu.Unlock()
	recMu.Lock()
	recQueued--
	recSaving = &saveState{Game: game, Step: "saving"}
	recMu.Unlock()
	defer func() { recMu.Lock(); recSaving = nil; recMu.Unlock() }()
	defer os.RemoveAll(dir)
	mame.KeepSnapshots(dir, exe) // F12 screenshots taken meanwhile
	if len(mame.AVIs(dir)) == 0 {
		return
	}
	ff := waitForFFmpeg(func(step string) { setSaving(func(st *saveState) { st.Step = step }) })
	setSaving(func(st *saveState) { st.Step = "saving" })
	out := uniqueOut(recordingsDir(), game, time.Now())
	err := mame.Convert(ff, dir, out, func(p int) { setSaving(func(st *saveState) { st.Percent = p }) })
	switch {
	case errors.Is(err, mame.ErrTooShort):
	case err != nil:
		setRecError("The arcade recording of " + game + " couldn't be saved: " + err.Error())
	default:
		recMu.Lock()
		recLastMsg = "Saved " + filepath.Base(out)
		recLastErr = ""
		recMu.Unlock()
	}
}
