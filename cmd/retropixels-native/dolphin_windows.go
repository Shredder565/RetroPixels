package main

// GameCube and Wii games, played in PixelU's Wii / GameCube engine
// (PixelU\wii\PixelU-Wii.exe, based on Dolphin, GPL-2.0+) with its picture
// docked inside the RetroPixels Native window (so OBS captures this window),
// as RetroPixels.exe does: the same engine (in the PixelU folder Wii U games
// use), the same command line, controller set-up, volume control and
// automatic recording. A separately installed Dolphin.exe isn't used.
//
// app_windows.go calls in through small hooks (dolOpenPath, dolCommand,
// dolEvent, ...); everything else is here and in internal/dolphin.

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/ffmpeg"
)

// Menu commands 1300–1399.
const (
	cmdDolLibrary    = 1300
	cmdDolStop       = 1301
	cmdDolAutoRec    = 1302 // now Record > Record games automatically (cmdRecAuto)
	cmdDolFullscreen = 1303
	cmdDolEmbed      = 1304
	cmdDolSettings   = 1305
	cmdDolPads       = 1306
	cmdDolOpenEmu    = 1307
	cmdDolOpenConfig = 1308
	cmdDolVolume0    = 1310 // 1310 + percent/10: 1311..1320
	cmdDolMute       = 1321
	cmdDolRecNext    = 1323
	cmdDolLayout     = 1324
	cmdDolFirst      = 1300
	cmdDolLast       = 1399

	dolQuality = "balanced" // RetroPixels.exe's default recording settings
	dolFPS     = 60
)

var (
	dpGetForegroundWindow = user32.NewProc("GetForegroundWindow")
	dpSetWindowsHookExW   = user32.NewProc("SetWindowsHookExW")
	dpUnhookWindowsHookEx = user32.NewProc("UnhookWindowsHookEx")
	dpCallNextHookEx      = user32.NewProc("CallNextHookEx")
	dpGetWindowTextW      = user32.NewProc("GetWindowTextW")
	dpGetWindowTextLength = user32.NewProc("GetWindowTextLengthW")

	dolHookCB = syscall.NewCallback(dolKeyHook)
)

// dolRun is one game playing in PixelU's Wii / GameCube engine.
type dolRun struct {
	extRun   // pid, exited, the docked game screen (dock_windows.go)
	game     dolphin.Game
	exe      string
	rec      *dolphin.Recording // nil: not recorded
	started  time.Time
	embed    bool
	wentFull bool // this window went fullscreen for the game
	volKick  chan struct{}
}

// dolState is the app's GameCube / Wii part.
type dolState struct {
	run         *dolRun // UI thread only, like the rest unless noted
	menu        uintptr
	volMenu     uintptr
	closeAfter  bool   // close the window once the game (and its recording) is done
	pendingOpen string // open this once the game has ended
	nextGame    bool   // record the next game (automatic recording off)
	recWarn     string
	ffAsks      int

	vol   atomic.Int32 // for the volume follower
	muted atomic.Bool

	mu         sync.Mutex // the rest is shared with background work
	pending    int        // recordings not saved yet (from launch until saved)
	saving     string     // the game whose recording is being saved
	saveStep   string     // "ffmpeg" or "saving"
	savePct    int
	detecting  bool
	found      *dolFound
	libGames   []dolphin.Game
	libLoaded  bool
	libLoading bool

	lib  *dolLibrary
	sdlg *dolSettingsDlg
	kdlg *wiiKeysDlg // the "keys file" step before a Wii game
}

type dolFound struct {
	exe     string
	folders []string
	played  map[string]time.Time
}

var dolSaveMu sync.Mutex // one recording is saved at a time

// ---------------- start-up ----------------

// dolStartup runs once the window exists: the first time, the games folders
// are looked for in the background.
func (a *app) dolStartup() {
	d := &a.settings.Dolphin
	a.dol.vol.Store(int32(d.volume()))
	a.dol.muted.Store(d.Muted)
	if !d.Detected {
		a.dolDetect()
	}
}

func (a *app) dolDetect() {
	a.dol.mu.Lock()
	if a.dol.detecting {
		a.dol.mu.Unlock()
		return
	}
	a.dol.detecting = true
	a.dol.mu.Unlock()
	have := a.settings.Dolphin
	have.Folders = append([]string(nil), have.Folders...)
	engine := a.dolExe()
	go func() {
		exe, folders, played := detectDolphin(have, engine)
		a.dol.mu.Lock()
		a.dol.detecting = false
		a.dol.found = &dolFound{exe, folders, played}
		a.dol.mu.Unlock()
		a.event("dol-detected", "")
	}()
}

// ---------------- menu ----------------

func (a *app) addDolphinMenu() {
	m := call(pCreatePopupMenu)
	a.dol.menu = m
	appendMenu(m, mfString|mfGrayed, 0, "Games play in PixelU (Wii/GameCube engine based on Dolphin, GPL)")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdDolLibrary, "&Library…")
	appendMenu(m, mfString, cmdDolStop, "&Stop game")
	appendMenu(m, mfSeparator, 0, "")
	a.dol.volMenu = call(pCreatePopupMenu)
	for v := 100; v >= 10; v -= 10 {
		appendMenu(a.dol.volMenu, mfString, uintptr(cmdDolVolume0+v/10), fmt.Sprintf("%d%%", v))
	}
	appendMenu(a.dol.volMenu, mfSeparator, 0, "")
	appendMenu(a.dol.volMenu, mfString, cmdDolMute, "&Mute")
	appendMenu(m, mfPopup, a.dol.volMenu, "Game &volume")
	appendMenu(m, mfString, cmdDolRecNext, "Record the &next game (when not recording automatically)")
	appendMenu(m, mfString, cmdDolFullscreen, "Start games &fullscreen")
	appendMenu(m, mfString, cmdDolEmbed, "Play inside this &window")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdDolSettings, "&PixelU settings…")
	appendMenu(m, mfString, cmdDolPads, "Set up &Xbox controller")
	appendMenu(m, mfString, cmdDolLayout, "&Controller layout")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdDolOpenEmu, "&Open PixelU (Wii/GameCube)")
	appendMenu(m, mfString, cmdDolOpenConfig, "Open PixelU's Wii/GameCube settings f&older")
	appendMenu(a.menu, mfPopup, m, "GameCube / &Wii")
}

// dolUpdateMenus sets ticks and labels as a menu opens (after updateMenus).
func (a *app) dolUpdateMenus() {
	d := &a.settings.Dolphin
	r := a.dol.run
	if r != nil {
		enableItem(a.menu, cmdRecord, true)
		switch {
		case r.rec != nil:
			if on, _ := r.rec.Active(); on {
				renameItem(a.menu, cmdRecord, "&Stop recording\tF9")
			} else {
				renameItem(a.menu, cmdRecord, "&Record from now on\tF9")
			}
		case a.dol.nextGame:
			renameItem(a.menu, cmdRecord, "Don't record the &next game\tF9")
		default:
			renameItem(a.menu, cmdRecord, "Record the &next game\tF9")
		}
	}
	enableItem(a.menu, cmdDolStop, r != nil)
	auto := a.settings.autoRecord()
	enableItem(a.menu, cmdDolRecNext, !auto)
	checkItem(a.menu, cmdDolRecNext, !auto && a.dol.nextGame)
	checkItem(a.menu, cmdDolFullscreen, d.fullscreen())
	checkItem(a.menu, cmdDolEmbed, d.embed())
	call(pCheckMenuRadioItem, a.menu, cmdDolVolume0+1, cmdDolVolume0+10, uintptr(cmdDolVolume0+max(1, (d.volume()+5)/10)), mfByCommand)
	checkItem(a.menu, cmdDolMute, d.Muted)
	has := a.dolExe() != ""
	enableItem(a.menu, cmdDolOpenEmu, has && r == nil)
	enableItem(a.menu, cmdDolOpenConfig, has)
	enableItem(a.menu, cmdDolPads, has && r == nil)
}

func (a *app) dolCommand(id int) {
	d := &a.settings.Dolphin
	switch {
	case id == cmdDolLibrary:
		a.openDolLibrary()
	case id == cmdDolStop:
		a.dolStop()
	case id == cmdDolAutoRec:
		a.toggleAutoRecord()
	case id == cmdDolRecNext:
		a.dol.nextGame = !a.dol.nextGame
		a.updateTitle()
	case id == cmdDolFullscreen:
		v := !d.fullscreen()
		d.Fullscreen = &v
		saveSettings(a.settings)
	case id == cmdDolEmbed:
		v := !d.embed()
		d.Embed = &v
		saveSettings(a.settings)
		if a.dol.run != nil {
			a.showMessage("This applies from the next game")
		}
	case id > cmdDolVolume0 && id <= cmdDolVolume0+10:
		d.setVolume((id - cmdDolVolume0) * 10)
		d.Muted = false
		a.dolApplyVolume()
		a.showMessage(fmt.Sprintf("GameCube / Wii volume %d%%", d.volume()))
	case id == cmdDolMute:
		d.Muted = !d.Muted
		a.dolApplyVolume()
		if d.Muted {
			a.showMessage("GameCube / Wii sound muted")
		} else {
			a.showMessage("GameCube / Wii sound on")
		}
	case id == cmdDolSettings:
		a.openDolSettings()
	case id == cmdDolPads:
		a.dolSetupPads()
	case id == cmdDolLayout:
		a.dolShowLayout()
	case id == cmdDolOpenEmu:
		a.dolOpenEmulator()
	case id == cmdDolOpenConfig:
		exe := a.dolExe()
		if exe == "" {
			messageBox(a.hwnd, noEngine, "RetroPixels Native", mbIconInfo)
			return
		}
		d.importOld(exe)
		p := filepath.Join(dolphin.UserDir(exe), "Config")
		os.MkdirAll(p, 0o755)
		shellOpen(p)
	}
}

func (a *app) dolApplyVolume() {
	d := &a.settings.Dolphin
	saveSettings(a.settings)
	a.dol.vol.Store(int32(d.volume()))
	a.dol.muted.Store(d.Muted)
	if r := a.dol.run; r != nil {
		select {
		case r.volKick <- struct{}{}:
		default:
		}
	}
}

func (a *app) dolVolumeNow() (int, bool) { return int(a.dol.vol.Load()), a.dol.muted.Load() }

// ---------------- opening games ----------------

// dolOpenPath handles a file opened or dropped: GameCube and Wii games start
// in PixelU's Wii / GameCube engine. It returns false for files that are for the other systems.
func (a *app) dolOpenPath(path string) bool {
	if !dolphin.IsGameFile(path) {
		ext := strings.ToLower(filepath.Ext(path))
		if ext == ".zip" || ext == ".7z" {
			if inner := discInArchive(path); inner != "" {
				messageBox(a.hwnd, fmt.Sprintf("%q holds a GameCube or Wii game (%s).\n\nPixelU can't play games from inside a %s file. "+
					"Unpack it first, then open the %s file (or add its folder in GameCube / Wii > Library).",
					filepath.Base(path), inner, ext, strings.ToLower(filepath.Ext(inner))), "RetroPixels Native", mbIconInfo)
				return true
			}
		}
		if r := a.dol.run; r != nil {
			if messageBox(a.hwnd, fmt.Sprintf("Stop %s and open %q?", r.game.Name, filepath.Base(path)), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
				a.dol.pendingOpen = path
				a.dolStop()
			}
			return true
		}
		return false
	}
	if dolphin.IsDLC(path) {
		messageBox(a.hwnd, fmt.Sprintf("%q is add-on content (DLC) for a WiiWare game, not a game you can start.\n\n"+
			"To add it: GameCube / Wii > Open PixelU (Wii/GameCube), then in PixelU use Tools > Install WAD... and choose this file once. "+
			"Then start the game itself.", filepath.Base(path)), "RetroPixels Native", mbIconInfo)
		return true
	}
	g, ok := dolphin.GameFor(path)
	if !ok {
		if _, err := os.Stat(path); err != nil {
			messageBox(a.hwnd, err.Error(), "RetroPixels Native", mbIconWarning)
		} else {
			messageBox(a.hwnd, fmt.Sprintf("%q isn't a GameCube or Wii game RetroPixels recognises (Wii U and PC or PlayStation discs aren't).",
				filepath.Base(path)), "RetroPixels Native", mbIconWarning)
		}
		return true
	}
	if t, ok := a.settings.Dolphin.Played[strings.ToLower(path)]; ok {
		g.Played = t
	}
	a.dolPlay(g)
	return true
}

// dolPlay starts a game (stopping the one playing first, if asked).
func (a *app) dolPlay(g dolphin.Game) {
	if a.mameBusy() || a.pixeluBusy() || a.supermodelBusy() || a.rpcs3Busy() {
		return
	}
	if r := a.dol.run; r != nil {
		if strings.EqualFold(r.game.Path, g.Path) {
			call(pSetForegroundWindow, a.hwnd)
			return
		}
		if messageBox(a.hwnd, fmt.Sprintf("Stop %s and start %s?", r.game.Name, g.Name), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.dol.pendingOpen = g.Path
			a.dolStop()
		}
		return
	}
	d := &a.settings.Dolphin
	exe := a.dolExe()
	if exe == "" {
		messageBox(a.hwnd, "GameCube and Wii games play in PixelU (Wii/GameCube engine based on Dolphin, GPL).\n\n"+noEngine,
			"RetroPixels Native", mbIconInfo)
		return
	}
	if dolphin.Running(filepath.Base(exe)) {
		messageBox(a.hwnd, "PixelU (Wii/GameCube) is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
		return
	}
	user := dolphin.UserDir(exe)
	d.importOld(exe)                       // the first time: the old Dolphin's settings and saves
	_ = dolphin.SetupXboxPads(user, false) // only fills in controllers that aren't set up yet
	if a.dolNeedsWiiKeys(g, user) {        // Wii games need the keys file from the player's own Wii
		return
	}

	// Recorded from the start (the engine can't start later); ffmpeg is
	// only needed to make the video once the game ends.
	record := false
	if a.dol.nextGame {
		record = a.recFFmpeg(false, true)
	} else if a.settings.autoRecord() {
		record = a.recFFmpeg(false, false)
	}
	a.closeGame()
	opts := dolphin.LaunchOptions{Embed: d.embed(), Fullscreen: d.fullscreen(), Record: record, Quality: dolQuality}
	args := dolphin.GameArgs(g.Path, opts)
	cmd := dolphin.Command(exe, args...)
	if opts.Embed {
		cmd.Env = append(os.Environ(), "PIXELU_EMBED=1") // its game screen stays off-screen until it's docked here
	}
	var rec *dolphin.Recording
	if record {
		rec = dolphin.NewRecording(g.Name, dolphin.DumpDir(user), filepath.Join(recordingsDir(), "logs", "last-recording.log"))
	}
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start PixelU: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	r := &dolRun{extRun: newExtRun(cmd.Process.Pid), game: g, exe: exe, rec: rec, started: time.Now(),
		embed: opts.Embed, volKick: make(chan struct{}, 1)}
	a.dol.run = r
	a.dol.nextGame = false
	a.dol.recWarn = ""
	go func() {
		cmd.Wait()
		close(r.exited)
		a.event("dol-exited", "")
	}()
	go dolphin.FollowVolume(r.pid, exe, a.dolVolumeNow, r.volKick, r.exited)
	if rec != nil {
		rec.Start(args)
		a.dol.mu.Lock()
		a.dol.pending++
		a.dol.mu.Unlock()
		dir := a.settings.recDir()
		go func() {
			rec.Follow(r.exited, func(m string) { a.event("dol-rec-warn", m) })
			a.dolSave(rec, dir)
		}()
	}
	if r.embed {
		if d.fullscreen() && !a.fullscreen {
			a.toggleFullscreen()
			r.wentFull = true
		}
		go a.dockWatch(&r.extRun, 90*time.Second, "dol") // slow disc / shader compile on first boot
	}
	d.markPlayed(g.Path, time.Now())
	a.settings.addRecent(g.Path)
	a.settings.LastFolder = filepath.Dir(g.Path)
	saveSettings(a.settings)
	if a.dol.lib != nil {
		a.dol.lib.markPlayed(g.Path)
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// dolStop ends the game the way the engine's own close button does (memory
// cards are saved); if Dolphin doesn't react it's closed.
func (a *app) dolStop() {
	r := a.dol.run
	if r == nil {
		return
	}
	r.stop()
	a.showMessage("Stopping the game…")
}

// dolEnded tidies up after the engine has closed.
func (a *app) dolEnded() {
	r := a.dol.run
	if r == nil {
		return
	}
	a.dol.run = nil
	a.dolUnhook()
	if r.wentFull && a.fullscreen {
		a.toggleFullscreen()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if p := a.dol.pendingOpen; p != "" {
		a.dol.pendingOpen = ""
		if !a.dol.closeAfter {
			a.openPath(p)
		}
	}
	a.dolMaybeClose()
}

// ---------------- recording ----------------

// dolToggleRecording is F9 / Record while a GameCube or Wii game plays:
// the engine can only be told to save a game as it starts, so without
// recording on when the game started it's for the next game.
func (a *app) dolToggleRecording() bool {
	r := a.dol.run
	if r == nil {
		return false
	}
	if r.rec != nil {
		if on, _ := r.rec.Active(); on {
			r.rec.Stop()
			a.showMessage("Recording stopped. The video is saved when the game ends")
		} else {
			r.rec.Resume()
			a.showMessage("Recording (saved as a separate video)")
		}
	} else if a.dol.nextGame = !a.dol.nextGame; a.dol.nextGame {
		a.showMessage("Your next game will be recorded (PixelU can only record from the start of a game)")
	} else {
		a.showMessage("Your next game won't be recorded")
	}
	a.updateTitle()
	return true
}

// dolSave makes the video once the game has ended (in the background).
func (a *app) dolSave(rec *dolphin.Recording, dir string) {
	dolSaveMu.Lock()
	defer dolSaveMu.Unlock()
	set := func(step string, pc int) {
		a.dol.mu.Lock()
		a.dol.saving, a.dol.saveStep, a.dol.savePct = rec.Game, step, pc
		a.dol.mu.Unlock()
		a.event("dol-progress", "")
	}
	set("saving", 0)
	saved, err := rec.Save(dolphin.SaveOptions{Dir: dir, Quality: dolQuality, FPS: dolFPS,
		FFmpeg: a.dolWaitFFmpeg, Progress: set})
	a.dol.mu.Lock()
	a.dol.saving = ""
	a.dol.pending--
	a.dol.mu.Unlock()
	switch {
	case err != nil:
		a.event("dol-save-error", err.Error())
	case len(saved) > 0:
		a.event("dol-saved", strings.Join(saved, ", "))
	default:
		a.event("dol-progress", "")
	}
}

// dolWaitFFmpeg: the video needs ffmpeg; if it isn't there it's downloaded
// now and the recording waits for it (as RetroPixels.exe does).
func (a *app) dolWaitFFmpeg() string {
	for i := 0; i < 1800; i++ { // up to an hour
		if ff := ffmpeg.Find(); ff != "" {
			return ff
		}
		if i%30 == 0 {
			a.event("dol-need-ffmpeg", "")
		}
		time.Sleep(2 * time.Second)
	}
	return ""
}

// dolBusySaving: recordings still to be saved (a game playing counts).
func (a *app) dolBusySaving() (int, string, int) {
	a.dol.mu.Lock()
	defer a.dol.mu.Unlock()
	return a.dol.pending, a.dol.saving, a.dol.savePct
}

// ---------------- closing ----------------

// dolBeforeClose: closing the window while a game plays stops the game
// first; while a recording is being saved, the window can wait for it.
// It returns true to keep the window open for now.
func (a *app) dolBeforeClose() bool {
	keep := a.dolHoldWindow()
	if !keep {
		dolStopKeys()
	}
	return keep
}

func (a *app) dolHoldWindow() bool {
	if a.dol.run != nil {
		if !a.dol.closeAfter {
			a.dol.closeAfter = true
			a.dolStop()
		} else if a.dol.run.stoppingFor(15 * time.Second) {
			return false // PixelU isn't closing: don't hold the window hostage
		}
		return true
	}
	pending, game, pc := a.dolBusySaving()
	if pending == 0 {
		return false
	}
	if game == "" {
		game = "your GameCube / Wii game"
	}
	switch messageBox(a.hwnd, fmt.Sprintf("The recording of %s is still being saved (%d%%).\n\n"+
		"Yes: close RetroPixels Native by itself once it's saved.\nNo: close now (the recording is lost).", game, pc),
		"RetroPixels Native", 3 /* MB_YESNOCANCEL */ |mbIconInfo) {
	case idYes:
		a.dol.closeAfter = true
		a.showMessage("Closing once the recording is saved…")
		return true
	case 7: // IDNO
		return false
	}
	a.dol.closeAfter = false
	return true
}

func (a *app) dolMaybeClose() {
	if !a.dol.closeAfter || a.dol.run != nil || a.pu.run != nil || a.mame.run != nil || a.sm.run != nil || a.rp.run != nil {
		return
	}
	if pending, _, _ := a.dolBusySaving(); pending == 0 {
		call(pPostMessageW, a.hwnd, wmClose, 0, 0)
	}
}

// ---------------- events ----------------

func (a *app) dolEvent(name, detail string) {
	switch name {
	case wiiKeysScanEventPrefix + "progress", wiiKeysScanEventPrefix + "done":
		a.wiiKeysEvent(name)
	case "dol-exited":
		a.dolEnded()
	case "dol-docked":
		a.dolHook()
		call(pInvalidateRect, a.hwnd, 0, 0)
		a.updateTitle()
	case "dol-nodock":
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	case "dol-key":
		switch detail {
		case "fullscreen":
			a.toggleFullscreen()
		case "unfullscreen":
			if a.fullscreen {
				a.toggleFullscreen()
			}
		case "record":
			a.toggleRecording()
		case "stop":
			switch {
			case a.dol.run != nil:
				a.dolStop()
			case a.pu.run != nil:
				a.pixeluStop()
			case a.mame.run != nil:
				a.mameStop()
			case a.sm.run != nil:
				a.supermodelStop()
			case a.rp.run != nil:
				a.rpcs3Stop()
			}
		case "close":
			call(pPostMessageW, a.hwnd, wmClose, 0, 0)
		}
		dolKeys.full.Store(a.fullscreen)
	case "dol-rec-warn":
		a.dol.recWarn = detail
		a.updateTitle()
	case "dol-progress":
		a.updateTitle()
		a.dolMaybeClose() // a recording with nothing to keep counts as done too
	case "dol-saved":
		a.showMessage("Recording saved: " + detail)
		a.dolMaybeClose()
	case "dol-save-error":
		a.updateTitle()
		if !a.dol.closeAfter {
			messageBox(a.hwnd, "GameCube / Wii recording: "+detail, "RetroPixels Native", mbIconWarning)
		}
		a.dolMaybeClose()
	case "ext-save-error": // a Wii U or MAME recording
		a.updateTitle()
		if !a.dol.closeAfter {
			messageBox(a.hwnd, "The recording couldn't be saved: "+detail, "RetroPixels Native", mbIconWarning)
		}
		a.dolMaybeClose()
	case "dol-need-ffmpeg":
		if a.ffDownload == "" && ffmpeg.Find() == "" && a.dol.ffAsks < 3 {
			a.dol.ffAsks++
			a.downloadFFmpeg()
		}
	case "dol-detected":
		a.dol.mu.Lock()
		f := a.dol.found
		a.dol.found = nil
		a.dol.mu.Unlock()
		if f == nil {
			return
		}
		d := &a.settings.Dolphin
		if d.Exe == "" && f.exe != "" {
			d.Exe = f.exe // the old Dolphin: only for bringing its settings over
		}
		if len(d.Folders) == 0 {
			d.Folders = f.folders
		}
		for k, t := range f.played {
			if _, ok := d.Played[k]; !ok {
				if d.Played == nil {
					d.Played = map[string]time.Time{}
				}
				d.Played[k] = t
			}
		}
		d.Detected = true
		saveSettings(a.settings)
		a.dolReloadLibrary()
	case "dol-lib":
		if a.dol.lib != nil {
			a.dol.lib.fill()
		}
	}
}

// dolTitle: what's going on, for the title bar.
func (a *app) dolTitle() string {
	dolKeys.full.Store(a.fullscreen) // for the keyboard hook (fullscreen can change from the menu too)
	t := ""
	if r := a.dol.run; r != nil {
		t += " - " + r.game.Name + " (" + r.game.Platform + ")"
		if r.embed && r.child.Load() == 0 {
			t += " - Starting in PixelU…"
		}
		if r.rec != nil {
			if on, since := r.rec.Active(); on {
				s := int(time.Since(since).Seconds())
				t += fmt.Sprintf(" - ● REC %d:%02d", s/60, s%60)
			}
		} else if a.dol.nextGame {
			t += " - next game will be recorded"
		}
		if a.dol.recWarn != "" {
			t += " - Not recording: PixelU isn't saving the picture"
		}
	}
	a.dol.mu.Lock()
	saving, step, pc := a.dol.saving, a.dol.saveStep, a.dol.savePct
	a.dol.mu.Unlock()
	if saving != "" {
		if step == "ffmpeg" {
			t += " - Recording of " + saving + " waiting for ffmpeg"
		} else {
			t += fmt.Sprintf(" - Saving the recording of %s… %d%%", saving, pc)
		}
	}
	return t
}

// ---------------- the picture area ----------------

// dolPaint draws the window while a GameCube or Wii game is on (the game
// itself is the engine's docked window on top). Holds frameMu.
func (a *app) dolPaint(hdc uintptr) bool {
	r := a.dol.run
	if r == nil {
		return false
	}
	cr := clientRect(a.hwnd)
	call(pFillRect, hdc, uintptr(unsafe.Pointer(&cr)), call(pGetStockObject, blackBrush))
	if r.child.Load() != 0 {
		return true
	}
	text := "Starting " + r.game.Name + " in PixelU…"
	if !r.embed {
		text = r.game.Name + " is playing in PixelU's own window.\n\nClose the game there (or GameCube / Wii > Stop game) to come back."
	}
	call(pSetBkMode, hdc, transparent)
	old := call(pSelectObject, hdc, a.font)
	call(pSetTextColor, hdc, 0xa09088)
	body := cr
	dpi := int32(a.dpi())
	body.Left += 16 * dpi / 96
	body.Right -= 16 * dpi / 96
	body.Top = cr.Bottom/2 - 30*dpi/96
	call(pDrawTextW, hdc, uintptr(unsafe.Pointer(u16(text))), ^uintptr(0), uintptr(unsafe.Pointer(&body)), dtCenter|dtWordBreak|dtNoPrefix)
	call(pSelectObject, hdc, old)
	return true
}

// dolIdleNote: a line for the start screen.
func dolIdleNote() string {
	return "\nGameCube and Wii games (" + strings.Join(dolphinExts(), " ") + ") play in PixelU (Wii/GameCube engine based on Dolphin, GPL): GameCube / Wii > Library."
}

// ---------------- keys while the engine has the keyboard ----------------

// While the docked game screen has the keyboard, this window's own keys
// don't arrive here, so a keyboard hook takes F11 / Alt+Enter (fullscreen),
// Esc (leave fullscreen; when not fullscreen, stop the game), F9 (record)
// and Alt+F4 (close) while this window
// is in front.
//
// The hook lives on a thread of its own that does nothing but wait for
// messages: Windows sends every key press to the hook's thread and waits
// for it, so the hook mustn't share a thread with the window, which can be
// busy waiting on Dolphin (moving its game screen) at the same moment.
type kbdLL struct {
	VkCode, ScanCode, Flags, Time uint32
	Extra                         uintptr
}

var (
	dpGetCurrentThreadId = syscall.NewLazyDLL("kernel32.dll").NewProc("GetCurrentThreadId")
	dpPostThreadMessageW = user32.NewProc("PostThreadMessageW")
	dpGetAsyncKeyState   = user32.NewProc("GetAsyncKeyState")

	dolKeys struct {
		hwnd   atomic.Uintptr // this window while a game screen is docked, else 0
		full   atomic.Bool    // this window is fullscreen
		thread atomic.Uint32  // the hook's thread
		held   map[uint32]bool
	}
)

// dolHook turns the keys on (started once, then kept).
func (a *app) dolHook() {
	dolKeys.full.Store(a.fullscreen)
	dolKeys.hwnd.Store(a.hwnd)
	if dolKeys.thread.Load() != 0 {
		return
	}
	dolKeys.thread.Store(^uint32(0)) // starting
	go func() {
		runtime.LockOSThread()
		dolKeys.held = map[uint32]bool{}
		hook := call(dpSetWindowsHookExW, 13 /* WH_KEYBOARD_LL */, dolHookCB, a.hinst, 0)
		dolKeys.thread.Store(uint32(call(dpGetCurrentThreadId)))
		var m msg
		for int32(call(pGetMessageW, uintptr(unsafe.Pointer(&m)), 0, 0, 0)) > 0 {
		}
		if hook != 0 {
			call(dpUnhookWindowsHookEx, hook)
		}
	}()
}

// dolUnhook turns the keys off while no game screen is docked.
func (a *app) dolUnhook() {
	dolKeys.hwnd.Store(0)
}

// dolStopKeys ends the hook's thread (the window is closing).
func dolStopKeys() {
	dolKeys.hwnd.Store(0)
	if t := dolKeys.thread.Load(); t != 0 && t != ^uint32(0) {
		call(dpPostThreadMessageW, uintptr(t), 0x0012 /* WM_QUIT */, 0, 0)
	}
}

func dolKeyHook(code, wp, lp uintptr) uintptr {
	if h := dolKeys.hwnd.Load(); int32(code) == 0 && h != 0 && call(dpGetForegroundWindow) == h {
		k := *(**kbdLL)(unsafe.Pointer(&lp)) // lp points at Windows' KBDLLHOOKSTRUCT
		down := wp == wmKeyDown || wp == wmSysKeyDown
		if !down && dolKeys.held[k.VkCode] {
			delete(dolKeys.held, k.VkCode)
			return 1
		}
		alt := k.Flags&0x20 != 0 /* LLKHF_ALTDOWN */ || call(dpGetAsyncKeyState, 0x12 /* VK_MENU */)&0x8000 != 0
		act := ""
		switch {
		case k.VkCode == vkF11, k.VkCode == vkReturn && alt:
			act = "fullscreen"
		case k.VkCode == vkEscape && dolKeys.full.Load():
			act = "unfullscreen"
		case k.VkCode == vkEscape:
			act = "stop" // not fullscreen: Esc ends the game
		case k.VkCode == vkF9:
			act = "record"
		case k.VkCode == vkF4 && alt: // the game screen would take it
			act = "close"
		}
		if act != "" && down {
			if !dolKeys.held[k.VkCode] {
				dolKeys.held[k.VkCode] = true
				if a := theApp; a != nil {
					a.event("dol-key", act)
				}
			}
			return 1
		}
	}
	return call(dpCallNextHookEx, 0, code, wp, lp)
}

// ---------------- the engine itself ----------------

// dolExe is PixelU's Wii / GameCube engine, or "".
func (a *app) dolExe() string { return a.settings.Dolphin.exe(a.settings.PixelU.Exe) }

func (a *app) dolOpenEmulator() {
	exe := a.dolExe()
	if exe == "" {
		messageBox(a.hwnd, noEngine, "RetroPixels Native", mbIconInfo)
		return
	}
	if dolphin.Running(filepath.Base(exe)) {
		messageBox(a.hwnd, "PixelU (Wii/GameCube) is already open.", "RetroPixels Native", mbIconInfo)
		return
	}
	a.settings.Dolphin.importOld(exe)
	_ = dolphin.SetupXboxPads(dolphin.UserDir(exe), false)
	if err := dolphin.Command(exe).Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start PixelU: "+err.Error(), "RetroPixels Native", mbIconWarning)
	}
}

func (a *app) dolSetupPads() {
	exe := a.dolExe()
	if exe == "" {
		messageBox(a.hwnd, noEngine, "RetroPixels Native", mbIconInfo)
		return
	}
	user := dolphin.UserDir(exe)
	if a.dol.run != nil || dolphin.Running(filepath.Base(exe)) {
		messageBox(a.hwnd, "Close PixelU (Wii/GameCube) first: it saves its own settings when it closes.", "RetroPixels Native", mbIconInfo)
		return
	}
	a.settings.Dolphin.importOld(exe)
	gc, wii := dolphin.PadState(user, "GCPadNew.ini", "GCPad1"), dolphin.PadState(user, "WiimoteNew.ini", "Wiimote1")
	if gc == "custom" || wii == "custom" {
		if messageBox(a.hwnd, "Replace your current PixelU controller set-up with the Xbox layout? (Your old settings file is backed up.)",
			"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return
		}
	}
	if err := dolphin.SetupXboxPads(user, true); err != nil {
		messageBox(a.hwnd, "Couldn't set up the controller: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage("Xbox controller set up in PixelU")
}

func (a *app) dolShowLayout() {
	user := dolphin.UserDir(a.dolExe())
	label := map[string]string{"xbox": "Xbox controller ✓", "custom": "your own set-up", "none": "not set up yet"}
	messageBox(a.hwnd, "GameCube controller (port 1): "+label[dolphin.PadState(user, "GCPadNew.ini", "GCPad1")]+"\n"+
		"Wii Remote + Nunchuk: "+label[dolphin.PadState(user, "WiimoteNew.ini", "Wiimote1")]+"\n\n"+
		dolphin.PadLayout+"\n\n"+
		"The controller is set up automatically the first time a game starts; fine-tune it in PixelU > Controllers "+
		"(GameCube / Wii > Open PixelU). Save states are in PixelU: Shift+F1 saves, F1 loads.",
		"Controller layout", mbIconInfo)
}

// ---------------- the library's games ----------------

// dolReloadLibrary looks through the games folders again (in the background).
func (a *app) dolReloadLibrary() {
	a.dol.mu.Lock()
	if a.dol.libLoading {
		a.dol.mu.Unlock()
		return
	}
	a.dol.libLoading = true
	a.dol.mu.Unlock()
	folders := append([]string(nil), a.settings.Dolphin.Folders...)
	played := map[string]time.Time{}
	for k, v := range a.settings.Dolphin.Played {
		played[k] = v
	}
	go func() {
		games := dolphin.Library(folders, played)
		a.dol.mu.Lock()
		a.dol.libGames, a.dol.libLoaded, a.dol.libLoading = games, true, false
		a.dol.mu.Unlock()
		a.event("dol-lib", "")
	}()
}

func windowString(hwnd uintptr) string {
	n := call(dpGetWindowTextLength, hwnd)
	buf := make([]uint16, n+1)
	call(dpGetWindowTextW, hwnd, uintptr(unsafe.Pointer(&buf[0])), n+1)
	return syscall.UTF16ToString(buf)
}
