package main

// Star Wars Trilogy Arcade (Sega Model 3), played in Supermodel as
// RetroPixels.exe's Supermodel tab plays it (internal/supermodel): the same
// supermodel.exe (chosen, or the Supermodel folder next to RetroPixels),
// the same player's folder (%APPDATA%\RetroPixels\Supermodel: settings,
// high scores, save states), the same Supermodel.ini and command line. By
// default the game is fullscreen in Supermodel's own window with the Xbox
// controller's thumbstick feeling like a flight stick; Play inside this
// window docks it here (dock_windows.go) instead. With Record > Record
// games automatically on, RetroPixels' Supermodel saves the game as it runs
// (-rp-record) and the video is made when it ends (pixelu_record.go, the
// same files as PixelU's).
//
// swtrilgy.zip and swtrilgya.zip opened, dropped or double-clicked (and
// chosen in the MAME library) start here; every other file goes where it
// went before. app_windows.go calls in through supermodelOpenPath,
// supermodelCommand and the other small hooks.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/ffmpeg"
	"retropixels/internal/mame"
	"retropixels/internal/supermodel"
)

// Menu commands 1920–1939.
const (
	cmdSMPlay       = 1920
	cmdSMChooseROM  = 1921
	cmdSMStop       = 1922
	cmdSMFullscreen = 1923
	cmdSMEmbed      = 1924
	cmdSMFlight     = 1925
	cmdSMSmooth0    = 1926 // + index in smoothOrder (1926–1929)
	cmdSMInvert     = 1930
	cmdSMWide       = 1931
	cmdSMChangeExe  = 1932
	cmdSMSaves      = 1933
	cmdSMKeys       = 1934
	cmdSMFirst      = 1920
	cmdSMLast       = 1939
)

// smRun is the game playing in Supermodel.
type smRun struct {
	extRun   // pid, exited, the docked window (dock_windows.go)
	rom      string
	out      *supermodel.Output
	logFile  string
	err      error // how Supermodel ended (set before exited closes)
	started  time.Time
	embed    bool
	wentFull bool    // this window went fullscreen for the game
	rec      *extRec // nil: not recorded
}

// smState is the app's Supermodel part (UI thread only).
type smState struct {
	run         *smRun
	pendingOpen string // open this once the game has ended
	finding     bool   // looking for the game in the background
}

// ---------------- menu ----------------

// addSupermodelMenu adds the game to the Arcade (MAME) menu m.
func (a *app) addSupermodelMenu(m uintptr) {
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdSMPlay, "&Star Wars Trilogy Arcade (Supermodel)")
	sub := call(pCreatePopupMenu)
	appendMenu(sub, mfString, cmdSMChooseROM, "Choose the &game file (swtrilgy.zip)…")
	appendMenu(sub, mfString, cmdSMStop, "&Stop game")
	appendMenu(sub, mfSeparator, 0, "")
	appendMenu(sub, mfString, cmdSMFullscreen, "Start &fullscreen")
	appendMenu(sub, mfString, cmdSMEmbed, "Play inside this &window")
	appendMenu(sub, mfString, cmdSMWide, "Wide s&creen")
	appendMenu(sub, mfSeparator, 0, "")
	appendMenu(sub, mfString, cmdSMFlight, "Xbox thumbstick feels like a f&light stick")
	sm := call(pCreatePopupMenu)
	for i, n := range []string{"&Off", "&Light", "&Medium", "&Heavy"} {
		appendMenu(sm, mfString, uintptr(cmdSMSmooth0+i), n)
	}
	appendMenu(sub, mfPopup, sm, "Flight stick s&moothing")
	appendMenu(sub, mfString, cmdSMInvert, "Stick up &dives (like the arcade's yoke)")
	appendMenu(sub, mfSeparator, 0, "")
	appendMenu(sub, mfString, cmdSMKeys, "Supermodel's &keys")
	appendMenu(sub, mfString, cmdSMSaves, "Open Supermodel's &saves folder")
	appendMenu(sub, mfString, cmdSMChangeExe, "C&hange Supermodel…")
	appendMenu(m, mfPopup, sub, "S&upermodel settings")
}

func (a *app) supermodelUpdateMenus() {
	s := &a.settings.Supermodel
	r := a.sm.run
	enableItem(a.menu, cmdSMPlay, r == nil && !a.sm.finding)
	enableItem(a.menu, cmdSMStop, r != nil)
	enableItem(a.menu, cmdSMChangeExe, a.supermodelOwnExe() == "") // RetroPixels' own is always used when it's there
	checkItem(a.menu, cmdSMFullscreen, s.fullscreen())
	checkItem(a.menu, cmdSMEmbed, s.embed())
	checkItem(a.menu, cmdSMWide, s.Wide)
	checkItem(a.menu, cmdSMFlight, s.flight())
	checkItem(a.menu, cmdSMInvert, s.InvertY)
	sel := 0
	for i, n := range smoothOrder {
		if n == s.smooth() {
			sel = i
		}
	}
	call(pCheckMenuRadioItem, a.menu, cmdSMSmooth0, uintptr(cmdSMSmooth0+len(smoothOrder)-1), uintptr(cmdSMSmooth0+sel), mfByCommand)
	for i := range smoothOrder {
		enableItem(a.menu, cmdSMSmooth0+i, s.flight())
	}
}

func (a *app) supermodelCommand(id int) {
	s := &a.settings.Supermodel
	toggle := func(p **bool, cur bool) {
		v := !cur
		*p = &v
		saveSettings(a.settings)
		if a.sm.run != nil {
			a.showMessage("This applies from the next game")
		}
	}
	switch {
	case id == cmdSMPlay:
		a.supermodelStart()
	case id == cmdSMChooseROM:
		if p := a.supermodelChooseROM(); p != "" {
			a.showMessage(supermodel.Title + ": " + filepath.Base(p))
		}
	case id == cmdSMStop:
		a.supermodelStop()
	case id == cmdSMFullscreen:
		toggle(&s.Fullscreen, s.fullscreen())
	case id == cmdSMEmbed:
		toggle(&s.Embed, s.embed())
	case id == cmdSMFlight:
		toggle(&s.FlightStick, s.flight())
	case id >= cmdSMSmooth0 && id < cmdSMSmooth0+len(smoothOrder):
		s.Smooth = smoothOrder[id-cmdSMSmooth0]
		saveSettings(a.settings)
	case id == cmdSMInvert:
		s.InvertY = !s.InvertY
		saveSettings(a.settings)
	case id == cmdSMWide:
		s.Wide = !s.Wide
		saveSettings(a.settings)
	case id == cmdSMKeys:
		messageBox(a.hwnd, supermodelKeysText, "Supermodel's keys", mbIconInfo)
	case id == cmdSMSaves:
		home := supermodel.DefaultHome()
		os.MkdirAll(home, 0o755)
		shellOpen(home)
	case id == cmdSMChangeExe:
		a.supermodelChooseExe()
	}
}

const supermodelKeysText = "Xbox controller: the left stick is the flight yoke, RT or A fires, RB or B is the event button " +
	"(force / lock-on), Back inserts a coin, Start starts, Back + Start leaves the game.\n\n" +
	"Mouse, at the same time: point to aim, left button fires, right button = event button, middle button = second trigger " +
	"(whichever of the mouse and the stick moved last steers).\n\n" +
	"Keyboard: 5 = insert coin, 1 = start, arrow keys = yoke, Left Ctrl or A = fire, Left Alt or S = event button, " +
	"F5 / F7 = save / load state, F6 = change slot, Alt+P = pause, Alt+R = reset, F2 = test menu, 9 = service, Esc = leave the game.\n\n" +
	"Supermodel keeps the game's settings, high scores and save states in " + `%APPDATA%\RetroPixels\Supermodel` +
	" (shared with RetroPixels.exe's Supermodel tab)."

// supermodelExe is Supermodel's program, found as RetroPixels.exe finds it.
func (a *app) supermodelExe() string {
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	home, _ := os.UserHomeDir()
	return supermodel.FindExe(a.settings.Supermodel.Exe, appDir, home)
}

// supermodelOwnExe is the Supermodel that comes with RetroPixels, if it's there.
func (a *app) supermodelOwnExe() string {
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	return supermodel.OwnExe(appDir)
}

func (a *app) supermodelChooseExe() string {
	start := ""
	if exe := a.supermodelExe(); exe != "" {
		start = filepath.Dir(exe)
	}
	p := a.fileDialog("Choose "+supermodel.ExeName, "Supermodel ("+supermodel.ExeName+")\x00"+supermodel.ExeName+"\x00Programs (*.exe)\x00*.exe\x00\x00", start)
	if p == "" {
		return ""
	}
	if !strings.EqualFold(filepath.Ext(p), ".exe") {
		messageBox(a.hwnd, "That isn't a program (.exe).", "RetroPixels Native", mbIconWarning)
		return ""
	}
	a.settings.Supermodel.Exe = p
	saveSettings(a.settings)
	return p
}

// supermodelChooseROM asks for the game file and remembers it if it's the game.
func (a *app) supermodelChooseROM() string {
	start := a.settings.LastFolder
	if r := a.settings.Supermodel.ROM; r != "" {
		start = filepath.Dir(r)
	}
	p := a.fileDialog("Choose Star Wars Trilogy Arcade (swtrilgya.zip or swtrilgy.zip)",
		"Star Wars Trilogy (swtrilgy*.zip)\x00swtrilgy*.zip\x00ZIP files (*.zip)\x00*.zip\x00\x00", start)
	if p == "" {
		return ""
	}
	if c := supermodel.CheckROM(p); c.Set == "" {
		messageBox(a.hwnd, filepath.Base(p)+": "+c.Problem, "RetroPixels Native", mbIconWarning)
		return ""
	}
	a.settings.Supermodel.ROM = p
	saveSettings(a.settings)
	return p
}

// supermodelFolders: where the game is looked for, as RetroPixels.exe does
// (MAME's ROM folders, the ROM folders next to Supermodel and RetroPixels,
// the GameCube / Wii games folders, Downloads, Desktop, Documents).
func (a *app) supermodelFolders() []string {
	mc := mame.Load()
	appDir, home := "", ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	if h, err := os.UserHomeDir(); err == nil {
		home = h
	}
	first := append(append([]string(nil), mc.Folders...), mame.BundleRoms())
	return supermodel.SearchFolders(first, a.supermodelExe(), appDir, home, a.settings.Dolphin.Folders)
}

// ---------------- starting the game ----------------

// supermodelStart is Arcade (MAME) > Star Wars Trilogy Arcade: the game
// file remembered, else the one RetroPixels.exe found, else it's looked
// for (in the background) and, failing that, asked for.
func (a *app) supermodelStart() {
	if r := a.sm.run; r != nil {
		call(pSetForegroundWindow, a.hwnd)
		return
	}
	s := &a.settings.Supermodel
	if s.ROM != "" {
		if _, err := os.Stat(s.ROM); err == nil {
			a.supermodelPlayPath(s.ROM)
			return
		}
	}
	if p := webSupermodelROM(appConfigPath()); p != "" && supermodel.CheckROM(p).OK {
		a.supermodelPlayPath(p)
		return
	}
	if a.sm.finding {
		return
	}
	a.sm.finding = true
	a.updateTitle()
	folders := a.supermodelFolders()
	go func() {
		p, _ := supermodel.Find(folders, dolphin.WiiKeysScanRoots())
		a.event("supermodel-found", p)
	}()
}

// supermodelFound: the search is done (p: the best copy found, "" = none).
func (a *app) supermodelFound(p string) {
	a.sm.finding = false
	a.updateTitle()
	msg := "RetroPixels didn't find Star Wars Trilogy Arcade (swtrilgya.zip or swtrilgy.zip) in your ROM folders, " +
		"Downloads, Desktop, Documents or the game folders on your drives."
	if p != "" {
		c := supermodel.CheckROM(p)
		if c.OK {
			a.supermodelPlayPath(p)
			return
		}
		msg = "RetroPixels found " + p + ", but it can't be played: " + c.Problem
	}
	if messageBox(a.hwnd, msg+"\n\nChoose the game file now?", "RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
		return
	}
	if p := a.supermodelChooseROM(); p != "" {
		a.supermodelPlayPath(p)
	}
}

// supermodelOpenPath handles a file opened or dropped: Star Wars Trilogy
// Arcade's ROM set starts in Supermodel, and while it plays, any other file
// stops it first. It returns false for every other file (and leaves the
// game to the engine that's playing, which asks to stop first).
func (a *app) supermodelOpenPath(path string) bool {
	if r := a.sm.run; r != nil {
		if strings.EqualFold(r.rom, path) {
			call(pSetForegroundWindow, a.hwnd)
			return true
		}
		if messageBox(a.hwnd, fmt.Sprintf("Stop %s and open %q?", supermodel.Title, filepath.Base(path)), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.sm.pendingOpen = path
			a.supermodelStop()
		}
		return true
	}
	if !isSupermodelGame(path) || a.pu.run != nil || a.dol.run != nil {
		return false
	}
	a.supermodelPlayPath(path)
	return true
}

// supermodelPlayPath plays the game file at path (remembering it).
func (a *app) supermodelPlayPath(path string) {
	c := supermodel.CheckROM(path)
	if c.Set == "" {
		messageBox(a.hwnd, filepath.Base(path)+" isn't Star Wars Trilogy Arcade: "+c.Problem, "RetroPixels Native", mbIconWarning)
		return
	}
	if s := &a.settings.Supermodel; !strings.EqualFold(s.ROM, path) {
		s.ROM = path
		saveSettings(a.settings)
	}
	if !c.OK {
		messageBox(a.hwnd, filepath.Base(path)+": "+c.Problem, "RetroPixels Native", mbIconWarning)
		return
	}
	a.supermodelPlay(path, c)
}

// supermodelBusy: the game is on (so another external game can't start). It says so.
func (a *app) supermodelBusy() bool {
	if a.sm.run != nil {
		messageBox(a.hwnd, supermodel.Title+" is playing in Supermodel. Stop it first (Arcade (MAME) > Supermodel settings > Stop game, or Esc in the game).",
			"RetroPixels Native", mbIconInfo)
		return true
	}
	return false
}

// supermodelPlay starts the game in rom (c: its check) in Supermodel.
func (a *app) supermodelPlay(rom string, c supermodel.Check) {
	if a.sm.run != nil {
		call(pSetForegroundWindow, a.hwnd)
		return
	}
	if a.mameBusy() || a.pixeluBusy() {
		return
	}
	if r := a.dol.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in PixelU (Wii/GameCube). Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return
	}
	exe := a.supermodelExe()
	if exe == "" {
		if messageBox(a.hwnd, supermodel.Title+" plays in Supermodel, which comes in the RetroPixels download (the Supermodel folder next to RetroPixels-Native.exe), "+
			"and it wasn't found. Unpack the whole download, or choose a "+supermodel.ExeName+".\n\nChoose "+supermodel.ExeName+" now?",
			"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return
		}
		if exe = a.supermodelChooseExe(); exe == "" {
			return
		}
	}
	if dolphin.Running(supermodel.ExeName) || dolphin.Running(filepath.Base(exe)) {
		messageBox(a.hwnd, "Supermodel is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
		return
	}
	patched := supermodel.Patched(exe)
	home := supermodel.DefaultHome()
	if err := os.MkdirAll(home, 0o755); err != nil {
		messageBox(a.hwnd, "Couldn't make Supermodel's folder: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	s := &a.settings.Supermodel
	if _, err := supermodel.WriteINI(supermodel.ConfigFile(home, exe), s.plan(rom, c, patched, "", "").Controls); err != nil {
		messageBox(a.hwnd, "Couldn't write Supermodel's settings: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.closeGame()
	// Recorded from the start (RetroPixels' build): Supermodel encodes the
	// picture with ffmpeg while the game runs, as PixelU does.
	var rec *extRec
	recDir, ff := "", ""
	if patched && a.settings.autoRecord() && a.recFFmpeg(true, false) {
		if dir, err := engineRecDir(filepath.Join(a.settings.recDir(), "temp"), "Supermodel-"); err != nil {
			a.showMessage("Not recording: " + err.Error())
		} else {
			recDir, ff = dir, ffmpeg.Find()
			rec = a.newExtRec(supermodel.Title, dir)
		}
	}
	pl := s.plan(rom, c, patched, recDir, ff)
	cmd, out := supermodel.Command(exe, home, pl.Args...)
	cmd.Env = append(cmd.Env, pl.Env)
	if err := cmd.Start(); err != nil {
		if rec != nil {
			a.extRecDropped(rec)
		}
		messageBox(a.hwnd, "Couldn't start Supermodel: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	r := &smRun{extRun: newExtRun(cmd.Process.Pid), rom: rom, out: out, logFile: supermodel.LogFile(home, exe),
		started: time.Now(), embed: pl.Embed, rec: rec}
	a.sm.run = r
	go func() {
		r.err = cmd.Wait()
		close(r.exited)
		a.event("supermodel-exited", "")
		if rec != nil {
			enc := a.encoder(ff)
			a.extSave(rec, func(ff string, progress func(int)) error {
				err := pixeluSave(ff, enc, rec.dir, rec.out, time.Since(rec.started).Seconds(), progress)
				if errors.Is(err, errNoPicture) {
					err = errors.New("Supermodel didn't save the game's picture, so nothing was recorded")
				}
				return err
			})
		}
	}()
	if r.embed {
		if s.fullscreen() && !a.fullscreen {
			a.toggleFullscreen()
			r.wentFull = true
		}
		go a.dockWatch(&r.extRun, 60*time.Second, "supermodel")
	}
	a.settings.addRecent(rom)
	a.settings.LastFolder = filepath.Dir(rom)
	saveSettings(a.settings)
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// supermodelStop ends the game the way Supermodel's own close button does
// (NVRAM and high scores are saved); if it doesn't react it's closed.
func (a *app) supermodelStop() {
	if r := a.sm.run; r != nil {
		r.stop()
		a.showMessage("Stopping the game…")
	}
}

// supermodelEnded tidies up after Supermodel has closed, and says why if
// the game didn't start.
func (a *app) supermodelEnded() {
	r := a.sm.run
	if r == nil {
		return
	}
	a.sm.run = nil
	if a.dol.run == nil && a.mame.run == nil && a.pu.run == nil {
		a.dolUnhook()
	}
	if r.wentFull && a.fullscreen {
		a.toggleFullscreen()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if r.stopAt.Load() == 0 && time.Since(r.started) < 20*time.Second {
		why := supermodel.Problem(r.out.String() + "\n" + supermodel.LogErrors(r.logFile))
		if why == "" && r.err != nil {
			why = "Supermodel closed straight away (" + r.err.Error() + ")."
		}
		if why != "" {
			messageBox(a.hwnd, supermodel.Title+" didn't start in Supermodel.\n\n"+why+"\n\nSupermodel's log: "+r.logFile,
				"RetroPixels Native", mbIconWarning)
		}
	}
	if p := a.sm.pendingOpen; p != "" {
		a.sm.pendingOpen = ""
		if !a.dol.closeAfter {
			a.openPath(p)
		}
	}
	a.dolMaybeClose() // closes the window if that was asked (once recordings are saved)
}

// supermodelBeforeClose: closing the window while the game plays stops it
// (and Supermodel) first. It returns true to keep the window open for now.
func (a *app) supermodelBeforeClose() bool {
	r := a.sm.run
	if r == nil {
		return false
	}
	if r.stoppingFor(15 * time.Second) {
		if p, err := os.FindProcess(r.pid); err == nil {
			_ = p.Kill()
		}
		return false // Supermodel isn't closing: don't hold the window hostage
	}
	a.dol.closeAfter = true // supermodelEnded closes the window
	if r.stopAt.Load() == 0 {
		a.supermodelStop()
	}
	return true
}

// supermodelToggleRecording is F9 / Record while the game plays: Supermodel
// can only be told to record as the game starts.
func (a *app) supermodelToggleRecording() bool {
	r := a.sm.run
	if r == nil {
		return false
	}
	if r.rec != nil {
		a.showMessage("Recording: the video is saved when the game ends")
	} else {
		a.showMessage("Supermodel games are recorded from their start: turn on Record > Record games automatically")
	}
	return true
}

func (a *app) supermodelExt() *extRun {
	if r := a.sm.run; r != nil {
		return &r.extRun
	}
	return nil
}

// ---------------- events ----------------

func (a *app) supermodelEvent(name, detail string) {
	switch name {
	case "supermodel-exited":
		a.supermodelEnded()
	case "supermodel-found":
		a.supermodelFound(detail)
	case "supermodel-docked":
		a.dolHook() // F11 / Alt+Enter / Esc reach this window while Supermodel has the keyboard
		call(pInvalidateRect, a.hwnd, 0, 0)
		a.updateTitle()
	case "supermodel-nodock":
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
}

// supermodelTitle: what's going on, for the title bar.
func (a *app) supermodelTitle() string {
	if a.sm.finding {
		return " - Looking for " + supermodel.Title + "…"
	}
	r := a.sm.run
	if r == nil {
		return ""
	}
	t := " - " + supermodel.Title + " (Supermodel)"
	if r.embed && r.child.Load() == 0 {
		t += " - Starting in Supermodel…"
	}
	if r.rec != nil {
		t += recTitle(r.rec.started)
	}
	return t
}

// supermodelPaint draws the window while the game is on (docked, the game
// itself is Supermodel's window on top). Holds frameMu.
func (a *app) supermodelPaint(hdc uintptr) bool {
	r := a.sm.run
	if r == nil {
		return false
	}
	cr := clientRect(a.hwnd)
	call(pFillRect, hdc, uintptr(unsafe.Pointer(&cr)), call(pGetStockObject, blackBrush))
	if r.child.Load() != 0 {
		return true
	}
	text := "Starting " + supermodel.Title + " in Supermodel…"
	if !r.embed {
		text = supermodel.Title + " is playing in Supermodel's own window.\n\nLeave the game there (Esc, or Back + Start on the controller) " +
			"or use Arcade (MAME) > Supermodel settings > Stop game to come back."
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
