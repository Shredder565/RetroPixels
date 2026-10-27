package main

// PlayStation 3 games in RPCS3 (internal/rpcs3): the RPCS3 folder that
// comes in the RetroPixels download. A game's .pkg, PARAM.SFO or EBOOT.BIN
// opened, dropped or double-clicked starts here when that folder is there
// (otherwise RetroPixels' own PS3 emulator plays it, as before). The first
// time, RPCS3 installs the PS3 system software (the player chooses Sony's
// PS3UPDAT.PUP) and then the game's package into its folder; after that the
// game starts straight away, in RPCS3 without its own window around it,
// docked inside this window (dock_windows.go).
//
// app_windows.go calls in through rpcs3OpenPath and the other small hooks.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/rpcs3"
)

// rpRun is the game playing in RPCS3.
type rpRun struct {
	extRun  // pid, exited, the docked window (dock_windows.go)
	eboot   string
	name    string
	err     error // how RPCS3 ended (set before exited closes)
	started time.Time
}

// rpState is the app's RPCS3 part (UI thread only).
type rpState struct {
	run         *rpRun
	preparing   string // what's being installed, while it is
	pendingOpen string // open this once the game has ended
}

// rpcs3OpenPath handles a file opened or dropped: a PS3 game starts in
// RPCS3 when RPCS3 came with RetroPixels. It returns false otherwise.
func (a *app) rpcs3OpenPath(path string) bool {
	if r := a.rp.run; r != nil {
		if messageBox(a.hwnd, fmt.Sprintf("Stop %s and open %q?", r.name, filepath.Base(path)), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.rp.pendingOpen = path
			a.rpcs3Stop()
		}
		return true
	}
	if !rpcs3.IsGameFile(path) {
		return false
	}
	exe := rpcs3.Exe()
	if exe == "" {
		return false // RetroPixels' own PS3 emulator plays it
	}
	if a.rp.preparing != "" {
		messageBox(a.hwnd, "RPCS3 is busy: "+a.rp.preparing+". Wait for it to finish.", "RetroPixels Native", mbIconInfo)
		return true
	}
	if a.mameBusy() || a.pixeluBusy() || a.supermodelBusy() {
		return true
	}
	if r := a.dol.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in PixelU (Wii/GameCube). Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	if dolphin.Running(rpcs3.ExeName) {
		messageBox(a.hwnd, "RPCS3 is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
		return true
	}
	dir := filepath.Dir(exe)
	if err := rpcs3.PrepareSettings(dir); err != nil {
		messageBox(a.hwnd, "Couldn't write RPCS3's settings: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return true
	}

	// What has to be installed first
	pup := ""
	if !rpcs3.HasFirmware(dir) {
		if messageBox(a.hwnd, "PlayStation 3 games play in RPCS3, which needs the PS3 system software once. "+
			"It's free from Sony: download the PS3 system update (PS3UPDAT.PUP) from playstation.com, then choose it here.\n\nChoose PS3UPDAT.PUP now?",
			"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return true
		}
		if pup = a.fileDialog("Choose the PS3 system software (PS3UPDAT.PUP)", "PS3 system software (*.PUP)\x00*.pup\x00All files (*.*)\x00*.*\x00\x00", a.settings.LastFolder); pup == "" {
			return true
		}
	}
	pkg, id, eboot := "", "", ""
	if strings.EqualFold(filepath.Ext(path), ".pkg") {
		if id = rpcs3.PkgTitleID(path); id == "" {
			messageBox(a.hwnd, filepath.Base(path)+" isn't a PlayStation 3 game package.", "RetroPixels Native", mbIconWarning)
			return true
		}
		if eboot = rpcs3.GameEboot(dir, id); eboot == "" {
			pkg = path
		}
	} else if eboot = rpcs3.Eboot(path); eboot == "" {
		messageBox(a.hwnd, "The game's EBOOT.BIN wasn't found next to "+filepath.Base(path)+".", "RetroPixels Native", mbIconWarning)
		return true
	}
	a.settings.addRecent(path)
	a.settings.LastFolder = filepath.Dir(path)
	saveSettings(a.settings)
	if pup == "" && pkg == "" {
		a.rpcs3Play(exe, eboot)
		return true
	}

	// Install in the background, then play
	a.closeGame()
	a.rp.preparing = "installing the PS3 system software"
	if pup == "" {
		a.rp.preparing = "installing " + filepath.Base(pkg)
	}
	a.updateTitle()
	call(pInvalidateRect, a.hwnd, 0, 0)
	go func() {
		if pup != "" {
			if err := rpcs3Install(exe, rpcs3.InstallFirmwareArgs(pup), filepath.Join(dir, "dev_flash"), func() bool { return rpcs3.HasFirmware(dir) }); err != nil {
				a.event("rpcs3-failed", "The PS3 system software wasn't installed: "+err.Error())
				return
			}
		}
		if pkg != "" {
			a.event("rpcs3-step", "installing "+filepath.Base(pkg))
			gameDir := filepath.Join(dir, "dev_hdd0", "game", id)
			if err := rpcs3Install(exe, rpcs3.InstallPkgArgs(pkg), gameDir, func() bool { return rpcs3.GameEboot(dir, id) != "" }); err != nil {
				a.event("rpcs3-failed", filepath.Base(pkg)+" wasn't installed: "+err.Error())
				return
			}
			eboot = rpcs3.GameEboot(dir, id)
		}
		a.event("rpcs3-ready", eboot)
	}()
	return true
}

// rpcs3Install runs RPCS3 to install something, waits until done() holds
// and the files under watch stop changing, then closes RPCS3.
func rpcs3Install(exe string, args []string, watch string, done func() bool) error {
	cmd := rpcs3.Command(exe, args...)
	if err := cmd.Start(); err != nil {
		return err
	}
	exited := make(chan struct{})
	go func() { cmd.Wait(); close(exited) }()
	ok := rpcs3.Settled(watch, done, 5*time.Second, 20*time.Minute, exited)
	select {
	case <-exited:
	default:
		dolphin.Shutdown(cmd.Process.Pid, exited, 10*time.Second)
	}
	if !ok {
		return errors.New("RPCS3 stopped before it was done (see RPCS3's log, RPCS3.log in the RPCS3 folder)")
	}
	return nil
}

// rpcs3Play starts the game at eboot in RPCS3, docked in this window.
func (a *app) rpcs3Play(exe, eboot string) {
	a.closeGame()
	cmd := rpcs3.Command(exe, rpcs3.PlayArgs(eboot)...)
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start RPCS3: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	name := "the PS3 game"
	if t := rpcs3.GameTitle(eboot); t != "" {
		name = t
	}
	r := &rpRun{extRun: newExtRun(cmd.Process.Pid), eboot: eboot, name: name, started: time.Now()}
	a.rp.run = r
	go func() {
		r.err = cmd.Wait()
		close(r.exited)
		a.event("rpcs3-exited", "")
	}()
	go a.dockWatch(&r.extRun, 5*time.Minute, "rpcs3") // the first start compiles the game for a while
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// rpcs3Stop ends the game the way RPCS3's own close button does.
func (a *app) rpcs3Stop() {
	if r := a.rp.run; r != nil {
		r.stop()
		a.showMessage("Stopping the game…")
	}
}

// rpcs3Busy: a game is on in RPCS3 (so another external game can't start). It says so.
func (a *app) rpcs3Busy() bool {
	if r := a.rp.run; r != nil {
		messageBox(a.hwnd, r.name+" is playing in RPCS3. Stop it first (Esc, then Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	if a.rp.preparing != "" {
		messageBox(a.hwnd, "RPCS3 is busy: "+a.rp.preparing+". Wait for it to finish.", "RetroPixels Native", mbIconInfo)
		return true
	}
	return false
}

// rpcs3Ended tidies up after RPCS3 has closed, and says why if the game
// didn't start.
func (a *app) rpcs3Ended() {
	r := a.rp.run
	if r == nil {
		return
	}
	a.rp.run = nil
	if a.dol.run == nil && a.mame.run == nil && a.pu.run == nil && a.sm.run == nil {
		a.dolUnhook()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if r.stopAt.Load() == 0 && time.Since(r.started) < 15*time.Second {
		why := "RPCS3 closed straight away"
		if r.err != nil {
			why += " (" + r.err.Error() + ")"
		}
		messageBox(a.hwnd, r.name+" didn't start in RPCS3: "+why+".\n\nRPCS3's log is RPCS3.log in the RPCS3 folder next to RetroPixels-Native.exe.",
			"RetroPixels Native", mbIconWarning)
	}
	if p := a.rp.pendingOpen; p != "" {
		a.rp.pendingOpen = ""
		if !a.dol.closeAfter {
			a.openPath(p)
		}
	}
	a.dolMaybeClose()
}

// rpcs3BeforeClose: closing the window while the game plays stops it (and
// RPCS3) first. It returns true to keep the window open for now.
func (a *app) rpcs3BeforeClose() bool {
	r := a.rp.run
	if r == nil {
		return false
	}
	if r.stoppingFor(15 * time.Second) {
		if p, err := os.FindProcess(r.pid); err == nil {
			_ = p.Kill()
		}
		return false
	}
	a.dol.closeAfter = true // rpcs3Ended closes the window
	if r.stopAt.Load() == 0 {
		a.rpcs3Stop()
	}
	return true
}

func (a *app) rpcs3Ext() *extRun {
	if r := a.rp.run; r != nil {
		return &r.extRun
	}
	return nil
}

func (a *app) rpcs3Event(name, detail string) {
	switch name {
	case "rpcs3-exited":
		a.rpcs3Ended()
	case "rpcs3-step":
		a.rp.preparing = detail
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	case "rpcs3-failed":
		a.rp.preparing = ""
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
		messageBox(a.hwnd, detail, "RetroPixels Native", mbIconWarning)
	case "rpcs3-ready":
		a.rp.preparing = ""
		if exe := rpcs3.Exe(); exe != "" && detail != "" {
			a.rpcs3Play(exe, detail)
		}
	case "rpcs3-docked":
		a.dolHook() // F11 / Alt+Enter / Esc reach this window while RPCS3 has the keyboard
		call(pInvalidateRect, a.hwnd, 0, 0)
		a.updateTitle()
	case "rpcs3-nodock":
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
}

// rpcs3Title: what's going on, for the title bar.
func (a *app) rpcs3Title() string {
	if p := a.rp.preparing; p != "" {
		return " - RPCS3: " + p + "…"
	}
	r := a.rp.run
	if r == nil {
		return ""
	}
	t := " - " + r.name + " (RPCS3)"
	if r.child.Load() == 0 {
		t += " - Starting…"
	}
	return t
}

// rpcs3Paint draws the window while RPCS3 installs or the game starts
// (docked, the game itself is RPCS3's window on top). Holds frameMu.
func (a *app) rpcs3Paint(hdc uintptr) bool {
	r := a.rp.run
	if r == nil && a.rp.preparing == "" {
		return false
	}
	cr := clientRect(a.hwnd)
	call(pFillRect, hdc, uintptr(unsafe.Pointer(&cr)), call(pGetStockObject, blackBrush))
	if r != nil && r.child.Load() != 0 {
		return true
	}
	text := "RPCS3 is " + a.rp.preparing + "…\n\nThis happens once. RPCS3's own window shows how it's going."
	if r != nil {
		text = "Starting " + r.name + " in RPCS3…\n\nThe first time, RPCS3 prepares the game for a few minutes."
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
