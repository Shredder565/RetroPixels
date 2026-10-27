package main

// Wii U games, played in PixelU (RetroPixels' Wii U emulator, based on
// Cemu), as RetroPixels.exe does: the same PixelU.exe (chosen, next to
// RetroPixels, or in Downloads) and the same command line (internal/pixelu).
// By default PixelU shows only the game picture (--rp-embed) and that
// window is docked inside this one (dock_windows.go), as Dolphin's is.
// With Record > Record games automatically on, PixelU saves the game as it
// runs (--rp-record) and the video is made when it ends (pixelu_record.go).
//
// app_windows.go calls in through pixeluOpenPath, pixeluCommand and the
// other small hooks.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/ffmpeg"
	"retropixels/internal/pixelu"
)

// Menu commands 1900–1919.
const (
	cmdPixelUOpenGame   = 1900
	cmdPixelUFullscreen = 1901
	cmdPixelUOpenEmu    = 1902
	cmdPixelUChangeExe  = 1903
	cmdPixelUStop       = 1904
	cmdPixelUEmbed      = 1905
	cmdPixelUScreen     = 1906
	cmdPixelUFirst      = 1900
	cmdPixelULast       = 1919
)

// pixeluRun is one game playing in PixelU.
type pixeluRun struct {
	extRun   // pid, exited, the docked game window (dock_windows.go)
	game     pixelu.Game
	embed    bool
	wentFull bool    // this window went fullscreen for the game
	rec      *extRec // nil: not recorded
}

// pixeluState is the app's Wii U part (UI thread only).
type pixeluState struct {
	run         *pixeluRun
	pendingOpen string // open this once the game has ended
	gamepad     bool   // the docked game shows the GamePad's screen
}

func (a *app) addPixelUMenu() {
	m := call(pCreatePopupMenu)
	appendMenu(m, mfString, cmdPixelUOpenGame, "Open a Wii &U game…")
	appendMenu(m, mfString, cmdPixelUStop, "&Stop game")
	appendMenu(m, mfString, cmdPixelUScreen, "Show the &GamePad screen (touch it with the mouse or a finger)")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdPixelUFullscreen, "Start games &fullscreen")
	appendMenu(m, mfString, cmdPixelUEmbed, "Play inside this &window")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdPixelUOpenEmu, "&Open PixelU (controllers, graphics)")
	appendMenu(m, mfString, cmdPixelUChangeExe, "C&hange PixelU…")
	appendMenu(a.menu, mfPopup, m, "Wii &U")
}

func (a *app) pixeluUpdateMenus() {
	r := a.pu.run
	enableItem(a.menu, cmdPixelUStop, r != nil)
	enableItem(a.menu, cmdPixelUScreen, r != nil && r.child.Load() != 0)
	checkItem(a.menu, cmdPixelUScreen, r != nil && a.pu.gamepad)
	checkItem(a.menu, cmdPixelUFullscreen, a.settings.PixelU.fullscreen())
	checkItem(a.menu, cmdPixelUEmbed, a.settings.PixelU.embed())
	enableItem(a.menu, cmdPixelUOpenEmu, a.pixeluExe() != "" && r == nil)
}

func (a *app) pixeluCommand(id int) {
	p := &a.settings.PixelU
	switch id {
	case cmdPixelUOpenGame:
		path := a.fileDialog("Open a Wii U game",
			"Wii U games (*.wua *.wud *.wux *.iso *.rpx *.elf)\x00*.wua;*.wud;*.wux;*.iso;*.rpx;*.elf\x00All files\x00*.*\x00\x00", a.settings.LastFolder)
		if path != "" && !a.pixeluOpenPath(path) {
			messageBox(a.hwnd, filepath.Base(path)+" isn't a Wii U game PixelU can open.", "RetroPixels Native", mbIconWarning)
		}
	case cmdPixelUStop:
		a.pixeluStop()
	case cmdPixelUScreen:
		if r := a.pu.run; r != nil {
			pad, err := pixelu.SetScreen(r.child.Load(), pixelu.ScreenSwitch)
			if err != nil {
				messageBox(a.hwnd, err.Error(), "RetroPixels Native", mbIconWarning)
				return
			}
			a.pu.gamepad = pad
			if pad {
				a.showMessage("GamePad screen: click or touch it to use the touch screen")
			} else {
				a.showMessage("TV screen")
			}
			a.pixeluUpdateMenus()
		}
	case cmdPixelUFullscreen:
		v := !p.fullscreen()
		p.Fullscreen = &v
		saveSettings(a.settings)
	case cmdPixelUEmbed:
		v := !p.embed()
		p.Embed = &v
		saveSettings(a.settings)
		if a.pu.run != nil {
			a.showMessage("This applies from the next game")
		}
	case cmdPixelUOpenEmu:
		exe := a.pixeluExe()
		if exe == "" || a.pu.run != nil {
			return
		}
		if dolphin.Running(pixelu.ExeName) {
			messageBox(a.hwnd, "PixelU is already open.", "RetroPixels Native", mbIconInfo)
			return
		}
		if err := pixelu.Command(exe).Start(); err != nil {
			messageBox(a.hwnd, "Couldn't start PixelU: "+err.Error(), "RetroPixels Native", mbIconWarning)
		}
	case cmdPixelUChangeExe:
		a.pixeluChooseExe()
	}
}

func (a *app) pixeluExe() string {
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	home, _ := os.UserHomeDir()
	return pixelu.FindExe(a.settings.PixelU.Exe, appDir, home)
}

func (a *app) pixeluChooseExe() string {
	start := ""
	if exe := a.pixeluExe(); exe != "" {
		start = filepath.Dir(exe)
	}
	p := a.fileDialog("Choose PixelU.exe", "PixelU (PixelU.exe)\x00PixelU.exe\x00Programs (*.exe)\x00*.exe\x00\x00", start)
	if p == "" {
		return ""
	}
	if !strings.EqualFold(filepath.Ext(p), ".exe") {
		messageBox(a.hwnd, "That isn't a program (.exe).", "RetroPixels Native", mbIconWarning)
		return ""
	}
	a.settings.PixelU.Exe = p
	saveSettings(a.settings)
	return p
}

// ---------------- opening games ----------------

// pixeluOpenPath handles a file opened or dropped: Wii U games start in
// PixelU, and while one plays, any other file stops it first. It returns
// false for files that are for the other systems.
func (a *app) pixeluOpenPath(path string) bool {
	if r := a.pu.run; r != nil {
		if strings.EqualFold(r.game.Path, path) {
			call(pSetForegroundWindow, a.hwnd)
			return true
		}
		if messageBox(a.hwnd, fmt.Sprintf("Stop %s and open %q?", r.game.Name, filepath.Base(path)), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.pu.pendingOpen = path
			a.pixeluStop()
		}
		return true
	}
	g, ok := pixelu.GameFor(path)
	if !ok {
		return false
	}
	a.pixeluPlay(g)
	return true
}

// pixeluBusy: a Wii U game is on (so another external game can't start). It says so.
func (a *app) pixeluBusy() bool {
	if r := a.pu.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in PixelU. Stop it first (Wii U > Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	return false
}

// pixeluPlay starts a game in PixelU.
func (a *app) pixeluPlay(g pixelu.Game) {
	if a.mameBusy() || a.supermodelBusy() || a.rpcs3Busy() {
		return
	}
	if r := a.dol.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in PixelU (Wii/GameCube). Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return
	}
	exe := a.pixeluExe()
	if exe == "" {
		if messageBox(a.hwnd, "Wii U games play in PixelU (RetroPixels' Wii U emulator, based on Cemu), and it wasn't found: "+
			"put its PixelU folder next to RetroPixels, or keep it in your Downloads folder.\n\nChoose PixelU.exe now?",
			"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return
		}
		if exe = a.pixeluChooseExe(); exe == "" {
			return
		}
	}
	if dolphin.Running(pixelu.ExeName) || dolphin.Running(filepath.Base(exe)) {
		messageBox(a.hwnd, "PixelU is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
		return
	}
	p := &a.settings.PixelU
	if !p.OpenGL {
		if err := pixelu.CheckVulkan(); err != nil {
			if messageBox(a.hwnd, "Wii U games need Vulkan, which isn't working on this PC: "+err.Error()+".\n\n"+pixelu.VulkanHelp+
				"\n\nPlay Wii U games with OpenGL instead? (Slower, but it works.)", "RetroPixels Native", mbYesNo|mbIconWarning) != idYes {
				return
			}
			p.OpenGL = true
			saveSettings(a.settings)
		}
	}
	a.closeGame()
	opts := p.launchOptions()
	// Recorded from the start: PixelU encodes with ffmpeg while the game runs.
	var rec *extRec
	if a.settings.autoRecord() && a.recFFmpeg(true, false) {
		if dir, err := pixeluRecDir(filepath.Join(a.settings.recDir(), "temp")); err != nil {
			a.showMessage("Not recording: " + err.Error())
		} else {
			opts.RecordDir, opts.FFmpeg = dir, ffmpeg.Find()
			rec = a.newExtRec(g.Name, dir)
		}
	}
	cmd := pixelu.Command(exe, pixelu.GameArgs(g.Path, opts)...)
	if err := cmd.Start(); err != nil {
		if rec != nil {
			a.extRecDropped(rec)
		}
		messageBox(a.hwnd, "Couldn't start PixelU: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	r := &pixeluRun{extRun: newExtRun(cmd.Process.Pid), game: g, embed: opts.Embed, rec: rec}
	a.pu.run = r
	go func() {
		cmd.Wait()
		close(r.exited)
		a.event("pixelu-exited", "")
		if rec != nil {
			enc := a.encoder(opts.FFmpeg)
			a.extSave(rec, func(ff string, progress func(int)) error {
				return pixeluSave(ff, enc, rec.dir, rec.out, time.Since(rec.started).Seconds(), progress)
			})
		}
	}()
	if r.embed {
		if p.fullscreen() && !a.fullscreen {
			a.toggleFullscreen()
			r.wentFull = true
		}
		go a.dockWatch(&r.extRun, 120*time.Second, "pixelu") // the first start compiles shaders
	}
	a.settings.addRecent(g.Path)
	a.settings.LastFolder = filepath.Dir(g.Path)
	saveSettings(a.settings)
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// pixeluStop ends the game the way PixelU's own close button does; if
// PixelU doesn't react it's closed (dock_windows.go).
func (a *app) pixeluStop() {
	if r := a.pu.run; r != nil {
		r.stop()
		a.showMessage("Stopping the game…")
	}
}

// pixeluEnded tidies up after PixelU has closed.
func (a *app) pixeluEnded() {
	r := a.pu.run
	if r == nil {
		return
	}
	a.pu.gamepad = false
	a.pu.run = nil
	if a.dol.run == nil && a.mame.run == nil && a.sm.run == nil && a.rp.run == nil {
		a.dolUnhook()
	}
	if r.wentFull && a.fullscreen {
		a.toggleFullscreen()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if p := a.pu.pendingOpen; p != "" {
		a.pu.pendingOpen = ""
		if !a.dol.closeAfter {
			a.openPath(p)
		}
	}
	a.dolMaybeClose() // closes the window if that was asked (once GameCube / Wii recordings are saved)
}

// pixeluBeforeClose: closing the window while a Wii U game plays stops the
// game (and PixelU) first. It returns true to keep the window open for now.
func (a *app) pixeluBeforeClose() bool {
	r := a.pu.run
	if r == nil {
		return false
	}
	if r.stoppingFor(15 * time.Second) {
		// PixelU isn't closing: end it, and don't hold the window hostage.
		if p, err := os.FindProcess(r.pid); err == nil {
			_ = p.Kill()
		}
		return false
	}
	a.dol.closeAfter = true // pixeluEnded closes the window
	if r.stopAt.Load() == 0 {
		a.pixeluStop()
	}
	return true
}

// pixeluToggleRecording is F9 / Record while a Wii U game plays: PixelU
// can only be told to record as a game starts.
func (a *app) pixeluToggleRecording() bool {
	r := a.pu.run
	if r == nil {
		return false
	}
	if r.rec != nil {
		a.showMessage("Recording: the video is saved when the game ends")
	} else {
		a.showMessage("Wii U games are recorded from their start: turn on Record > Record games automatically")
	}
	return true
}

// ---------------- events ----------------

func (a *app) pixeluEvent(name, detail string) {
	switch name {
	case "pixelu-exited":
		a.pixeluEnded()
	case "pixelu-docked":
		a.dolHook() // F11 / Alt+Enter / Esc / Alt+F4 reach this window while PixelU has the keyboard
		call(pInvalidateRect, a.hwnd, 0, 0)
		a.updateTitle()
	case "pixelu-nodock":
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
}

// pixeluTitle: what's going on, for the title bar.
func (a *app) pixeluTitle() string {
	r := a.pu.run
	if r == nil {
		return ""
	}
	t := " - " + r.game.Name + " (Wii U)"
	if r.embed && r.child.Load() == 0 {
		t += " - Starting in PixelU…"
	}
	if r.rec != nil {
		t += recTitle(r.rec.started)
	}
	return t
}

// pixeluPaint draws the window while a Wii U game is on (the game itself
// is PixelU's docked window on top). Holds frameMu.
func (a *app) pixeluPaint(hdc uintptr) bool {
	r := a.pu.run
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
		text = r.game.Name + " is playing in PixelU's own window.\n\nClose the game there (or Wii U > Stop game) to come back."
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
