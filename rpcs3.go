package main

// PlayStation 3 games in the player's own RPCS3 (internal/rpcs3): the PS3
// tab. RPCS3 is found in the folder the player chose, or where they keep it,
// or on their drives; its installed games (and the disc games it knows) are
// listed, and clicking one starts it through startProcess: one game at a
// time, fullscreen in RPCS3's own window or docked inside the RetroPixels
// window, with no RPCS3 game list. The tab says what's missing before a
// game can start (the PS3 system software, the game's licence) and has
// buttons that hand the player's own files to RPCS3: its installers for
// PS3UPDAT.PUP and game packages, and a copy into exdata for a licence.
//
// PS3 games aren't recorded: RPCS3 can't save its own picture and sound for
// RetroPixels the way Dolphin, PixelU, MAME and Supermodel do.

import (
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"retropixels/internal/dolphin"
	"retropixels/internal/rpcs3"
)

var (
	ps3Mu       sync.Mutex
	ps3Finding  bool
	ps3Searched bool           // looked on the drives since RetroPixels started
	ps3Problem  map[string]any // why the last game didn't start
	ps3Busy     string         // "Installing <file>…" while RPCS3's installer runs
	ps3BusyErr  string         // how the last install ended, when it went wrong
)

func ps3AppDir() string {
	if exe, err := os.Executable(); err == nil {
		return filepath.Dir(exe)
	}
	return ""
}

// ps3Exe is RPCS3's program ("" when it hasn't been found).
func ps3Exe() string { return rpcs3.Load().ExeFor(ps3AppDir()) }

// ps3Find looks for RPCS3 on the drives in the background (once, or again
// when asked) and remembers it.
func ps3Find(force bool) {
	ps3Mu.Lock()
	if ps3Finding || (ps3Searched && !force) {
		ps3Mu.Unlock()
		return
	}
	ps3Finding = true
	ps3Mu.Unlock()
	go func() {
		p := rpcs3.Search(dolphin.WiiKeysScanRoots())
		rpcs3.Update(func(c *rpcs3.Config) {
			if p != "" {
				c.Exe = p
			}
			c.Detected = true
		})
		ps3Mu.Lock()
		ps3Finding, ps3Searched = false, true
		ps3Mu.Unlock()
	}()
}

func ps3Status() map[string]any {
	c := rpcs3.Load()
	st := rpcs3.Look(c, ps3AppDir())
	if st.Exe == "" && c.Folder == "" {
		ps3Find(false) // the first time the tab opens
	}
	ps3Mu.Lock()
	defer ps3Mu.Unlock()
	return map[string]any{"rpcs3": st, "fullscreen": c.FullscreenOn(), "embed": c.EmbedOn(), "folder": c.Folder,
		"finding": ps3Finding, "searched": ps3Searched || c.Detected, "problem": ps3Problem, "busy": ps3Busy, "busyError": ps3BusyErr,
		"firmwareMessage": rpcs3.FirmwareMessage, "notFound": rpcs3.NotFoundMessage}
}

func ps3Running() bool {
	runMu.Lock()
	r := running != nil && running.Kind == "rpcs3"
	runMu.Unlock()
	return r || processRunning(rpcs3.ExeName)
}

// launchPS3 starts the game with this title ID in RPCS3.
func launchPS3(titleID string) error {
	c := rpcs3.Load()
	exe := c.ExeFor(ps3AppDir())
	if exe == "" {
		return errors.New(rpcs3.NotFoundMessage)
	}
	root := rpcs3.Root(exe)
	g, ok := rpcs3.Look(c, ps3AppDir()).Find(titleID)
	if !ok {
		return errors.New("that game isn't installed in RPCS3 any more")
	}
	if err := rpcs3.Ready(root, g); err != nil {
		return err
	}
	ps3Mu.Lock()
	busy := ps3Busy
	ps3Mu.Unlock()
	if busy != "" {
		return errors.New("RPCS3 is still installing (" + busy + "). Start the game once it has finished")
	}
	if ps3Running() {
		return errors.New("RPCS3 is already open. Close it first, then start the game here")
	}
	// The Xbox controller as player 1, unless the player set RPCS3's controls up themselves.
	_, _ = rpcs3.UseXboxPad(root, false)
	embed := c.EmbedOn() && hostWindowPresent()
	cmd, out := rpcs3.Command(exe, rpcs3.GameArgs(g.Eboot, rpcs3.LaunchOptions{Fullscreen: c.FullscreenOn(), Embed: embed})...)
	var onStart func(pid int, exited <-chan struct{})
	if embed {
		onStart = embedPixelU // RPCS3's Vulkan / OpenGL window stays over the page, as PixelU's does
	}
	ps3Mu.Lock()
	ps3Problem = nil
	ps3Mu.Unlock()
	started := time.Now()
	logFile := rpcs3.LogFile(exe)
	onExit := func(err error, ran time.Duration) {
		if ran > 20*time.Second {
			return // it played
		}
		why := rpcs3.Problem(out.String() + "\n" + rpcs3.LogErrors(logFile, started))
		if why == "" {
			if err == nil {
				return
			}
			why = "RPCS3 closed straight away (" + err.Error() + ")."
		}
		ps3Mu.Lock()
		ps3Problem = map[string]any{"game": g.Title, "text": why, "at": time.Now(), "log": logFile}
		ps3Mu.Unlock()
	}
	if err := startProcess(cmd, g.Eboot, g.Title, "rpcs3", "RPCS3", nil, onStart, onExit); err != nil {
		return err
	}
	_, err := rpcs3.Update(func(c *rpcs3.Config) { c.MarkPlayed(g.TitleID, time.Now()) })
	return err
}

// ps3Install runs RPCS3's own installer (a game package or the system
// software) in the background; the tab shows it until RPCS3 closes.
func ps3Install(args []string, file string) error {
	exe := ps3Exe()
	if exe == "" {
		return errors.New(rpcs3.NotFoundMessage)
	}
	if ps3Running() {
		return errors.New("RPCS3 is already open. Close it first")
	}
	ps3Mu.Lock()
	if ps3Busy != "" {
		ps3Mu.Unlock()
		return errors.New("RPCS3 is already installing " + ps3Busy)
	}
	ps3Busy, ps3BusyErr = filepath.Base(file), ""
	ps3Mu.Unlock()
	cmd, out := rpcs3.Command(exe, args...)
	started := time.Now()
	if err := cmd.Start(); err != nil {
		ps3Mu.Lock()
		ps3Busy = ""
		ps3Mu.Unlock()
		return errors.New("couldn't start RPCS3: " + err.Error())
	}
	go func() {
		err := cmd.Wait()
		ps3Mu.Lock()
		defer ps3Mu.Unlock()
		ps3Busy = ""
		if err != nil && time.Since(started) < 5*time.Second {
			why := rpcs3.Problem(out.String())
			if why == "" {
				why = "RPCS3 closed straight away (" + err.Error() + ")"
			}
			ps3BusyErr = why + ". If this RPCS3 can't install from RetroPixels, use its File > Install Packages/Raps/Edats (or Install Firmware) instead."
		}
	}()
	return nil
}

func registerPS3Routes(mux *http.ServeMux) {
	mux.HandleFunc("/api/ps3/status", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, ps3Status())
	}))
	mux.HandleFunc("/api/ps3/play", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			TitleID string `json:"titleId"`
		}
		if !readBody(w, r, &b) {
			return
		}
		if err := launchPS3(b.TitleID); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/ps3/action", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
			On     bool   `json:"on"`
		}
		if !readBody(w, r, &b) {
			return
		}
		var err error
		on := b.On
		root := ""
		if exe := ps3Exe(); exe != "" {
			root = rpcs3.Root(exe)
		}
		needRoot := func() bool {
			if root == "" {
				err = errors.New(rpcs3.NotFoundMessage)
			}
			return root != ""
		}
		switch b.Action {
		case "choose-folder":
			var d string
			d, err = pickFolder("Choose the RPCS3 folder (the one "+rpcs3.ExeName+" is in)", root)
			if err == nil && d != "" {
				if rpcs3.ExeIn(d) == "" {
					err = errors.New(rpcs3.ExeName + " isn't in " + d)
				} else {
					_, err = rpcs3.Update(func(c *rpcs3.Config) { c.Folder = d })
				}
			}
		case "default-folder":
			_, err = rpcs3.Update(func(c *rpcs3.Config) { c.Folder = "" })
		case "find":
			ps3Find(true)
		case "fullscreen":
			_, err = rpcs3.Update(func(c *rpcs3.Config) { c.Fullscreen = &on })
		case "embed":
			_, err = rpcs3.Update(func(c *rpcs3.Config) { c.Embed = &on })
		case "add-licence":
			if needRoot() {
				var p string
				p, err = pickFile("Choose your licence file for a PS3 game (.rap or .edat)", "PS3 licences (*.rap;*.edat)|*.rap;*.edat", "")
				if err == nil && p != "" {
					_, err = rpcs3.AddLicence(root, p)
				}
			}
		case "install-pkg":
			if needRoot() {
				var p string
				p, err = pickFile("Choose a PS3 game package to install in RPCS3 (.pkg)", "PS3 packages (*.pkg)|*.pkg", "")
				if err == nil && p != "" {
					if err = rpcs3.CheckPkg(p); err == nil {
						err = ps3Install(rpcs3.InstallPkgArgs(p), p)
					}
				}
			}
		case "install-firmware":
			if needRoot() {
				var p string
				p, err = pickFile("Choose the PS3 system software (PS3UPDAT.PUP)", "PS3 system software (*.PUP)|*.PUP", "")
				if err == nil && p != "" {
					if err = rpcs3.CheckPUP(p); err == nil {
						err = ps3Install(rpcs3.InstallFirmwareArgs(p), p)
					}
				}
			}
		case "xbox-pad":
			if needRoot() {
				_, err = rpcs3.UseXboxPad(root, true)
			}
		case "open-folder":
			if needRoot() {
				err = openFolder(root)
			}
		case "rescan":
		case "stop-game":
			err = stopEmbeddedGame()
		default:
			err = errors.New("unknown action")
		}
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, ps3Status())
	}))
}
