package main

// 3DS and DS games, as RetroPixels.exe's 3DS / DS tab plays them
// (internal/handheld): a 3DS game opens in Azahar, a DS game in melonDS,
// both in their own window (fullscreen by default). The player only picks
// "Open a 3DS or DS game…": the emulator follows from the file.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/handheld"
)

// Menu commands 1950–1969.
const (
	cmdHHOpenGame     = 1950
	cmdHHFullscreen   = 1951
	cmdHHStop         = 1952
	cmdHHOpenAzahar   = 1953
	cmdHHOpenMelonDS  = 1954
	cmdHHChangeAzahar = 1955
	cmdHHChangeMelon  = 1956
	cmdHHFirst        = 1950
	cmdHHLast         = 1969
)

// hhRun is one game playing in Azahar or melonDS.
type hhRun struct {
	extRun
	game   handheld.Game
	engine handheld.Engine
}

// hhState is the app's 3DS / DS part (UI thread only).
type hhState struct {
	run         *hhRun
	pendingOpen string
}

func (a *app) addHandheldMenu() {
	m := call(pCreatePopupMenu)
	appendMenu(m, mfString, cmdHHOpenGame, "Open a &3DS or DS game…")
	appendMenu(m, mfString, cmdHHStop, "&Stop game")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdHHFullscreen, "Start games &fullscreen")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdHHOpenAzahar, "Open &Azahar (3DS: controls, graphics, installed games)")
	appendMenu(m, mfString, cmdHHOpenMelonDS, "Open &melonDS (DS: controls, screen layout)")
	appendMenu(m, mfString, cmdHHChangeAzahar, "Change Azahar…")
	appendMenu(m, mfString, cmdHHChangeMelon, "Change melonDS…")
	appendMenu(a.menu, mfPopup, m, "&3DS / DS")
}

func (a *app) hhUpdateMenus() {
	r := a.hh.run
	enableItem(a.menu, cmdHHStop, r != nil)
	checkItem(a.menu, cmdHHFullscreen, a.settings.Handheld.fullscreen())
	enableItem(a.menu, cmdHHOpenAzahar, r == nil && a.hhExe(handheld.Azahar) != "")
	enableItem(a.menu, cmdHHOpenMelonDS, r == nil && a.hhExe(handheld.MelonDS) != "")
}

func hhFilter() string {
	var pats []string
	for _, e := range handheld.Extensions() {
		pats = append(pats, "*"+e)
	}
	p := strings.Join(pats, ";")
	return "3DS and DS games (" + strings.Join(pats, " ") + ")\x00" + p + "\x00All files\x00*.*\x00\x00"
}

func (a *app) hhCommand(id int) {
	h := &a.settings.Handheld
	switch id {
	case cmdHHOpenGame:
		path := a.fileDialog("Open a 3DS or DS game", hhFilter(), a.settings.LastFolder)
		if path != "" && !a.hhOpenPath(path) {
			messageBox(a.hwnd, filepath.Base(path)+" isn't a 3DS or DS game.", "RetroPixels Native", mbIconWarning)
		}
	case cmdHHStop:
		if r := a.hh.run; r != nil {
			r.stop()
			a.showMessage("Stopping the game…")
		}
	case cmdHHFullscreen:
		v := !h.fullscreen()
		h.Fullscreen = &v
		saveSettings(a.settings)
	case cmdHHOpenAzahar, cmdHHOpenMelonDS:
		e := handheld.Azahar
		if id == cmdHHOpenMelonDS {
			e = handheld.MelonDS
		}
		exe := a.hhExe(e)
		if exe == "" || a.hh.run != nil {
			return
		}
		if err := handheld.Command(exe).Start(); err != nil {
			messageBox(a.hwnd, "Couldn't start "+e.Name+": "+err.Error(), "RetroPixels Native", mbIconWarning)
		}
	case cmdHHChangeAzahar:
		a.hhChooseExe(handheld.Azahar)
	case cmdHHChangeMelon:
		a.hhChooseExe(handheld.MelonDS)
	}
}

func (a *app) hhExe(e handheld.Engine) string {
	appDir := ""
	if exe, err := os.Executable(); err == nil {
		appDir = filepath.Dir(exe)
	}
	home, _ := os.UserHomeDir()
	return e.FindExe(a.settings.Handheld.Exe[e.ID], appDir, home)
}

func (a *app) hhChooseExe(e handheld.Engine) string {
	start := ""
	if exe := a.hhExe(e); exe != "" {
		start = filepath.Dir(exe)
	}
	n := e.Exes[0]
	p := a.fileDialog("Choose "+n, e.Name+" ("+n+")\x00"+n+"\x00Programs (*.exe)\x00*.exe\x00\x00", start)
	if p == "" {
		return ""
	}
	if !strings.EqualFold(filepath.Ext(p), ".exe") {
		messageBox(a.hwnd, "That isn't a program (.exe).", "RetroPixels Native", mbIconWarning)
		return ""
	}
	if a.settings.Handheld.Exe == nil {
		a.settings.Handheld.Exe = map[string]string{}
	}
	a.settings.Handheld.Exe[e.ID] = p
	saveSettings(a.settings)
	return p
}

// hhOpenPath handles a file opened or dropped: 3DS and DS games start in
// their emulator. It returns false for files that are for other systems.
func (a *app) hhOpenPath(path string) bool {
	g, ok := handheld.GameFor(path)
	if !ok {
		return false
	}
	if r := a.hh.run; r != nil {
		if strings.EqualFold(r.game.Path, path) {
			return true
		}
		if messageBox(a.hwnd, fmt.Sprintf("Stop %s and open %q?", r.game.Name, filepath.Base(path)), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.hh.pendingOpen = path
			r.stop()
		}
		return true
	}
	a.hhPlay(g)
	return true
}

func (a *app) hhPlay(g handheld.Game) {
	if a.mameBusy() || a.pixeluBusy() {
		return
	}
	if r := a.dol.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in PixelU (Wii/GameCube). Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return
	}
	e := handheld.EngineFor(g.System)
	exe := a.hhExe(e)
	if exe == "" {
		sys := "3DS games play in Azahar"
		if g.System == handheld.SystemDS {
			sys = "DS games play in melonDS"
		}
		if messageBox(a.hwnd, sys+", and it wasn't found: keep its "+e.Dir+" folder from the RetroPixels download next to RetroPixels.\n\nChoose "+e.Exes[0]+" now?",
			"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return
		}
		if exe = a.hhChooseExe(e); exe == "" {
			return
		}
	}
	for _, n := range append([]string{filepath.Base(exe)}, e.Exes...) {
		if dolphin.Running(n) {
			messageBox(a.hwnd, e.Name+" is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
			return
		}
	}
	a.closeGame()
	cmd := handheld.Command(exe, handheld.GameArgs(g, a.settings.Handheld.fullscreen())...)
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start "+e.Name+": "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	r := &hhRun{extRun: newExtRun(cmd.Process.Pid), game: g, engine: e}
	a.hh.run = r
	go func() {
		cmd.Wait()
		close(r.exited)
		a.event("handheld-exited", "")
	}()
	a.settings.addRecent(g.Path)
	a.settings.LastFolder = filepath.Dir(g.Path)
	saveSettings(a.settings)
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// hhBusy: a 3DS or DS game is on. It says so.
func (a *app) hhBusy() bool {
	if r := a.hh.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in "+r.engine.Name+". Stop it first (3DS / DS > Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	return false
}

func (a *app) hhEvent(name, detail string) {
	if name != "handheld-exited" || a.hh.run == nil {
		return
	}
	a.hh.run = nil
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if p := a.hh.pendingOpen; p != "" {
		a.hh.pendingOpen = ""
		a.openPath(p)
	}
}

// hhBeforeClose: closing the window while a game plays asks the emulator to
// close too (it saves as it does on its own close button).
func (a *app) hhBeforeClose() bool {
	if r := a.hh.run; r != nil {
		r.stop()
	}
	return false
}

func (a *app) hhTitle() string {
	r := a.hh.run
	if r == nil {
		return ""
	}
	if r.game.Install {
		return " - Installing " + r.game.Name + " into Azahar"
	}
	return " - " + r.game.Name + " (" + strings.ToUpper(r.game.System) + ", " + r.engine.Name + ")"
}

// hhPaint draws the window while a 3DS or DS game plays in its emulator's
// own window. Holds frameMu.
func (a *app) hhPaint(hdc uintptr) bool {
	r := a.hh.run
	if r == nil {
		return false
	}
	cr := clientRect(a.hwnd)
	call(pFillRect, hdc, uintptr(unsafe.Pointer(&cr)), call(pGetStockObject, blackBrush))
	text := r.game.Name + " is playing in " + r.engine.Name + "'s own window.\n\nClose the game there (or 3DS / DS > Stop game) to come back."
	if r.game.Install {
		text = "Installing " + r.game.Name + " into Azahar.\n\nWhen it's done, open Azahar (3DS / DS > Open Azahar) and play it from Azahar's game list."
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
