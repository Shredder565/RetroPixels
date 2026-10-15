package main

// The interface's own menu items, keys and commands (command ids 1400-1599):
// save state slots and the Save states window, battery save import and
// export, screenshots, controller rumble, the system for .bin files,
// recording quality and folder, recent recordings, and help. app_windows.go
// calls in through the small hooks named a.ui*.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unsafe"

	"retropixels/internal/oldname"
)

const (
	cmdStatesWin   = 1400
	cmdNextSlot    = 1401
	cmdPrevSlot    = 1402
	cmdImportState = 1403
	cmdSlot0       = 1410 // ..1413
	cmdSramImport  = 1420
	cmdSramExport  = 1421
	cmdSavesFolder = 1422
	cmdScreenshot  = 1430
	cmdShotFolder  = 1431
	cmdRumble      = 1440
	cmdBin0        = 1450 // + index in binChoices
	cmdRecQ0       = 1460 // + index in recQualities
	cmdRecDir      = 1470
	cmdRecDirReset = 1471
	cmdKeysHelp    = 1480
	cmdReadme      = 1481
	cmdRecRecent0  = 1500 // ..1509

	vkF1  = 0x70
	vkF3  = 0x72
	vkF6  = 0x75
	vkF12 = 0x7B
)

// uiState is the interface's part of the window.
type uiState struct {
	small, smallBold, bold, med uintptr // fonts

	// start screen
	hits, prevHits []homeHit
	hover, sel     int
	pads           [4]PadState
	homeActive     bool
	padA           bool
	navDir         int
	navAt          time.Time

	// the save state slot F2 / F4 use, for the game in slotGame
	slot     int
	slotGame string

	slotMenu, battMenu, binMenu, qualMenu, recRecentMenu uintptr
	recList                                              []string

	states *statesDialog
}

func newUIState() *uiState { return &uiState{hover: -1, sel: -1} }

// initUI: after the window is made.
func (a *app) initUI() {
	call(pCoInitializeEx, 0, 2) // COINIT_APARTMENTTHREADED, for the folder picker
	// Folders from the program's old name (RetroConsole) move to RetroPixels,
	// as RetroPixels.exe does, so its ffmpeg and recordings are found.
	oldname.Migrate("RetroPixels")
	dpi := a.dpi()
	font := func(px, weight int) uintptr {
		return call(pCreateFontW, uintptr(-int32(px*dpi/96)), 0, 0, 0, uintptr(weight), 0, 0, 0, 1, 0, 0, 5, 0, uintptr(unsafe.Pointer(u16("Segoe UI"))))
	}
	a.ui.small = font(13, 400)
	a.ui.smallBold = font(12, 600)
	a.ui.bold = font(15, 600)
	a.ui.med = font(19, 600)
	call(pSetTimer, a.hwnd, timerHome, 50, 0)
}

// ---------------- menus ----------------

// uiFileMenu: after Close game.
func (a *app) uiFileMenu() {
	a.ui.binMenu = call(pCreatePopupMenu)
	for i, id := range binChoices {
		label := "&Guess from the file"
		if id != "" {
			label = systemByID(id).Label
		}
		appendMenu(a.ui.binMenu, mfString, uintptr(cmdBin0+i), label)
	}
	appendMenu(a.fileMenu, mfSeparator, 0, "")
	appendMenu(a.fileMenu, mfPopup, a.ui.binMenu, "Open .&bin files as")
	appendMenu(a.fileMenu, mfString, cmdSavesFolder, "Open &saves folder")
}

// uiGameMenu: after Undo load.
func (a *app) uiGameMenu() {
	a.ui.slotMenu = call(pCreatePopupMenu)
	appendMenu(a.gameMenu, mfPopup, a.ui.slotMenu, "State sl&ot")
	appendMenu(a.gameMenu, mfString, cmdStatesWin, "Save s&tates…\tF6")
	appendMenu(a.gameMenu, mfString, cmdImportState, "&Import state file…")
	appendMenu(a.gameMenu, mfSeparator, 0, "")
	a.ui.battMenu = call(pCreatePopupMenu)
	appendMenu(a.ui.battMenu, mfString, cmdSramImport, "&Import battery save…")
	appendMenu(a.ui.battMenu, mfString, cmdSramExport, "&Export battery save…")
	appendMenu(a.gameMenu, mfPopup, a.ui.battMenu, "&Battery save")
	appendMenu(a.gameMenu, mfString, cmdScreenshot, "Screens&hot\tF12")
}

// uiOptionsMenu: at the end of Options.
func (a *app) uiOptionsMenu() {
	appendMenu(a.optMenu, mfString, cmdRumble, "Controller &rumble")
}

// uiRecordMenu: after Open recordings folder.
func (a *app) uiRecordMenu() {
	a.ui.recRecentMenu = call(pCreatePopupMenu)
	appendMenu(a.recMenu, mfPopup, a.ui.recRecentMenu, "Recent re&cordings")
	appendMenu(a.recMenu, mfString, cmdRecDir, "C&hange recordings folder…")
	appendMenu(a.recMenu, mfString, cmdRecDirReset, "Use the &default folder (Videos\\RetroPixels)")
	a.ui.qualMenu = call(pCreatePopupMenu)
	for i, q := range recQualities {
		appendMenu(a.ui.qualMenu, mfString, uintptr(cmdRecQ0+i), q.label)
	}
	appendMenu(a.recMenu, mfPopup, a.ui.qualMenu, "&Quality")
	appendMenu(a.recMenu, mfSeparator, 0, "")
	appendMenu(a.recMenu, mfString, cmdShotFolder, "Open scr&eenshots folder")
}

// uiHelpMenu: before About.
func (a *app) uiHelpMenu(help uintptr) {
	appendMenu(help, mfString, cmdKeysHelp, "&Keys and shortcuts\tF1")
	appendMenu(help, mfString, cmdReadme, "&Read me")
	appendMenu(help, mfSeparator, 0, "")
}

// uiPopup fills the submenus that change as they open.
func (a *app) uiPopup(m uintptr) {
	u := a.ui
	switch m {
	case u.slotMenu:
		for call(pGetMenuItemCount, m) > 0 {
			call(pDeleteMenu, m, 0, mfByPosition)
		}
		if s := a.sess; s != nil {
			slots := readSlots(s.game.System, s.game.Name)
			now := time.Now()
			for i, si := range slots {
				appendMenu(m, mfString, uintptr(cmdSlot0+i), "&"+slotLabel(i, si, now))
			}
			call(pCheckMenuRadioItem, m, cmdSlot0, cmdSlot0+slotCount-1, uintptr(cmdSlot0+a.curSlot()), mfByCommand)
		} else {
			for i := 0; i < slotCount; i++ {
				appendMenu(m, mfString|mfGrayed, uintptr(cmdSlot0+i), fmt.Sprintf("Slot %d", i+1))
			}
		}
		appendMenu(m, mfSeparator, 0, "")
		appendMenu(m, mfString, cmdNextSlot, "&Next slot\tF3")
		appendMenu(m, mfString, cmdPrevSlot, "&Previous slot\tShift+F3")
		enableItem(m, cmdNextSlot, a.sess != nil)
		enableItem(m, cmdPrevSlot, a.sess != nil)
	case u.recRecentMenu:
		for call(pGetMenuItemCount, m) > 0 {
			call(pDeleteMenu, m, 0, mfByPosition)
		}
		u.recList = recentRecordings(a.settings.recDir(), 10)
		if len(u.recList) == 0 {
			appendMenu(m, mfString|mfGrayed, 0, "(none yet)")
		}
		for i, p := range u.recList {
			appendMenu(m, mfString, uintptr(cmdRecRecent0+i), strings.ReplaceAll(filepath.Base(p), "&", "&&"))
		}
	}
}

// uiUpdateMenus: ticks, greyed items and labels (end of updateMenus).
func (a *app) uiUpdateMenus() {
	s := a.sess
	on := s != nil
	slot := a.curSlot()
	renameItem(a.menu, cmdSaveState, fmt.Sprintf("&Save state (slot %d)\tF2", slot+1))
	renameItem(a.menu, cmdLoadState, fmt.Sprintf("&Load state (slot %d)\tF4", slot+1))
	for _, id := range []int{cmdStatesWin, cmdImportState, cmdScreenshot} {
		enableItem(a.menu, id, on)
	}
	sram := on && s.hasSRAM()
	enableItem(a.menu, cmdSramImport, sram)
	enableItem(a.menu, cmdSramExport, sram)
	checkItem(a.menu, cmdRumble, !a.settings.RumbleOff)
	bi := 0
	for i, id := range binChoices {
		if id == binSystem {
			bi = i
		}
		enableItem(a.menu, cmdBin0+i, id == "" || available(id))
	}
	call(pCheckMenuRadioItem, a.menu, cmdBin0, uintptr(cmdBin0+len(binChoices)-1), uintptr(cmdBin0+bi), mfByCommand)
	qi := 0
	for i, q := range recQualities {
		if q.id == a.settings.recQuality() {
			qi = i
		}
	}
	call(pCheckMenuRadioItem, a.menu, cmdRecQ0, uintptr(cmdRecQ0+len(recQualities)-1), uintptr(cmdRecQ0+qi), mfByCommand)
	enableItem(a.menu, cmdRecDirReset, a.settings.RecDir != "")
	enableItem(a.menu, cmdReadme, readmePath() != "")
}

// ---------------- keys ----------------

// uiKeyDown: the interface's keys (before the game gets them). With no
// game, the start screen's.
func (a *app) uiKeyDown(vk int, repeat bool) bool {
	if a.sess == nil {
		switch vk {
		case vkF1:
			a.command(cmdKeysHelp)
			return true
		}
		return a.homeKey(vk)
	}
	if repeat {
		switch vk {
		case vkF1, vkF3, vkF6, vkF12:
			return true
		}
		return false
	}
	switch vk {
	case vkF1:
		a.command(cmdKeysHelp)
	case vkF3:
		if keyDown(vkShift) {
			a.command(cmdPrevSlot)
		} else {
			a.command(cmdNextSlot)
		}
	case vkF6:
		a.command(cmdStatesWin)
	case vkF12:
		a.command(cmdScreenshot)
	default:
		return false
	}
	return true
}

// ---------------- events ----------------

// uiEvent: events from the game's thread the window doesn't handle itself.
func (a *app) uiEvent(name, detail string) {
	switch name {
	case "pad-slot":
		if detail == "-1" {
			a.command(cmdPrevSlot)
		} else {
			a.command(cmdNextSlot)
		}
	}
}

// uiTimer: the interface's timers.
func (a *app) uiTimer(id uintptr) bool {
	if id == timerHome {
		a.homeTick()
		return true
	}
	return false
}

// ---------------- slots ----------------

// curSlot: the slot F2 / F4 use. Each game starts on slot 1, as on the web page.
func (a *app) curSlot() int {
	s := a.sess
	if s == nil {
		return a.ui.slot
	}
	if a.ui.slotGame != s.game.Path {
		a.ui.slotGame, a.ui.slot = s.game.Path, 0
	}
	return a.ui.slot
}

func (a *app) setSlot(i int) {
	a.curSlot()
	a.ui.slot = (i + slotCount) % slotCount
	if s := a.sess; s != nil {
		si := readSlots(s.game.System, s.game.Name)[a.ui.slot]
		msg := fmt.Sprintf("Slot %d selected", a.ui.slot+1)
		if si.Used {
			msg += " (saved " + slotTime(si.Time, time.Now()) + ")"
		} else {
			msg += " (empty)"
		}
		a.showMessage(msg)
	}
	if a.ui.states != nil {
		a.ui.states.refresh()
	}
}

func (a *app) saveToSlot() {
	s := a.sess
	if s == nil {
		return
	}
	slot := a.curSlot()
	if err := s.saveSlot(slot); err != nil {
		messageBox(a.hwnd, "Couldn't save the state: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage(fmt.Sprintf("Saved to slot %d", slot+1))
	if a.ui.states != nil {
		a.ui.states.refresh()
	}
}

func (a *app) loadFromSlot() {
	s := a.sess
	if s == nil {
		return
	}
	slot := a.curSlot()
	if err := s.loadSlot(slot); err != nil {
		messageBox(a.hwnd, "Couldn't load the state: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage(fmt.Sprintf("Loaded slot %d (Ctrl+Z undoes it)", slot+1))
	if a.ui.states != nil {
		a.ui.states.refresh()
	}
}

// importStateFile: Game > Import state file... (and a .state file dropped on a game).
func (a *app) importStateFile(owner uintptr, path string) {
	s := a.sess
	if s == nil {
		return
	}
	if path == "" {
		path = fileDialog(owner, false, "Import state file", "Save states (*.state)\x00*.state\x00All files\x00*.*", savesDir(s.game.System), "", "")
		if path == "" {
			return
		}
	}
	st, err := os.Stat(path)
	if err == nil && st.Size() > 1<<30 {
		err = fmt.Errorf("it's too big to be a save state")
	}
	var data []byte
	if err == nil {
		data, err = os.ReadFile(path)
	}
	if err == nil {
		err = s.importState(data)
	}
	if err != nil {
		messageBox(owner, "Couldn't load that file: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage("State file loaded - save it to a slot to keep it (Ctrl+Z undoes it)")
}

// ---------------- battery saves ----------------

// importBattery replaces the game's battery save with a file from another
// emulator (or exported from RetroPixels.exe) and restarts the game, as the
// web page's "Import battery save".
func (a *app) importBattery(path string) {
	s := a.sess
	if s == nil {
		return
	}
	if !s.hasSRAM() {
		messageBox(a.hwnd, s.game.Name+" doesn't keep a battery save, so there's nothing to replace.", "Import battery save", mbIconInfo)
		return
	}
	if path == "" {
		path = fileDialog(a.hwnd, false, "Import battery save", "Battery saves (*.sav;*.srm;*.sram;*.bin;*.eep;*.sra;*.fla;*.mpk)\x00*.sav;*.srm;*.sram;*.bin;*.eep;*.sra;*.fla;*.mpk\x00All files\x00*.*", a.settings.LastFolder, "", "")
		if path == "" {
			return
		}
	}
	if messageBox(a.hwnd, fmt.Sprintf("Replace %s's battery save with %q?\n\nThe game will restart. The battery save it has now is kept as a .bak file in the saves folder.",
		s.game.Name, filepath.Base(path)), "Import battery save", mbYesNo|mbIconWarning) != idYes {
		return
	}
	st, err := os.Stat(path)
	if err == nil && st.Size() > maxSRAMImport {
		err = fmt.Errorf("it's too big to be a battery save")
	}
	var data []byte
	if err == nil {
		data, err = os.ReadFile(path)
	}
	if err != nil {
		messageBox(a.hwnd, "Couldn't read that file: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	g := s.game
	a.closeGame() // (writes the game's own battery save first, so it can be kept as .bak)
	if err := importSRAM(g.System, g.Name, data); err != nil {
		messageBox(a.hwnd, "Couldn't import the battery save: "+err.Error(), "RetroPixels Native", mbIconWarning)
	}
	a.openPath(g.Path)
	if a.sess != nil {
		a.showMessage("Battery save imported - game restarted")
	}
}

func (a *app) exportBattery() {
	s := a.sess
	if s == nil {
		return
	}
	data := s.sram()
	if data == nil {
		a.showMessage("This game has no battery save")
		return
	}
	p := fileDialog(a.hwnd, true, "Export battery save", "Battery saves (*.sav)\x00*.sav\x00All files\x00*.*", a.settings.LastFolder, safeName(s.game.Name)+".sav", "sav")
	if p == "" {
		return
	}
	if err := writeFileAtomic(p, data); err != nil {
		messageBox(a.hwnd, "Couldn't save the file: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage("Battery save exported: " + filepath.Base(p))
}

// ---------------- dropped files ----------------

// uiDrop: a battery save or save state dropped on a running game is
// imported into it; anything else opens as a game.
func (a *app) uiDrop(path string) {
	if a.sess != nil {
		x := extOf(path)
		if sramFileExts[x] {
			a.importBattery(path)
			return
		}
		if x == "state" {
			a.importStateFile(a.hwnd, path)
			return
		}
	}
	a.openPath(path)
}

// ---------------- commands ----------------

func (a *app) uiCommand(id int) {
	s := a.sess
	switch {
	case id == cmdStatesWin && s != nil:
		a.openStates()
	case id == cmdNextSlot && s != nil:
		a.setSlot(a.curSlot() + 1)
	case id == cmdPrevSlot && s != nil:
		a.setSlot(a.curSlot() - 1)
	case id >= cmdSlot0 && id < cmdSlot0+slotCount && s != nil:
		a.setSlot(id - cmdSlot0)
	case id == cmdImportState && s != nil:
		a.importStateFile(a.hwnd, "")
	case id == cmdSramImport && s != nil:
		a.importBattery("")
	case id == cmdSramExport && s != nil:
		a.exportBattery()
	case id == cmdSavesFolder:
		d := filepath.Join(appDir(), "saves")
		if s != nil {
			d = savesDir(s.game.System)
		}
		os.MkdirAll(d, 0o755)
		shellOpen(d)
	case id == cmdScreenshot && s != nil:
		a.screenshot()
	case id == cmdShotFolder:
		d := screenshotsDir()
		os.MkdirAll(d, 0o755)
		shellOpen(d)
	case id == cmdRumble:
		a.settings.RumbleOff = !a.settings.RumbleOff
		saveSettings(a.settings)
		if a.settings.RumbleOff {
			a.xin.stopRumble()
			a.showMessage("Controller rumble off")
		} else {
			a.showMessage("Controller rumble on")
		}
	case id >= cmdBin0 && id < cmdBin0+len(binChoices):
		binSystem = binChoices[id-cmdBin0]
		if binSystem == "" {
			a.showMessage(".bin files: the system is guessed from the file")
		} else {
			a.showMessage(".bin files open as " + systemByID(binSystem).Label)
		}
	case id >= cmdRecQ0 && id < cmdRecQ0+len(recQualities):
		a.settings.RecQuality = recQualities[id-cmdRecQ0].id
		saveSettings(a.settings)
		a.showMessage("Recording quality: " + strings.ReplaceAll(recQualities[id-cmdRecQ0].label, "&", "") + " (from the next recording)")
	case id == cmdRecDir:
		if d := pickFolder(a.hwnd, "Choose the folder for recordings", a.settings.recDir()); d != "" {
			a.settings.RecDir = d
			saveSettings(a.settings)
			a.showMessage("Recordings go to " + d)
		}
	case id == cmdRecDirReset:
		a.settings.RecDir = ""
		saveSettings(a.settings)
		a.showMessage("Recordings go to " + recordingsDir())
	case id >= cmdRecRecent0 && id < cmdRecRecent0+10:
		if i := id - cmdRecRecent0; i < len(a.ui.recList) {
			shellOpen(a.ui.recList[i])
		}
	case id == cmdKeysHelp:
		messageBox(a.hwnd, keysHelp, "Keys and shortcuts", mbIconInfo)
	case id == cmdReadme:
		if p := readmePath(); p != "" {
			shellOpen(p)
		}
	}
}

// screenshot saves the game's picture (only the game) as a .png.
func (a *app) screenshot() {
	s := a.sess
	if s == nil {
		return
	}
	a.frameMu.Lock()
	pix := append([]uint32{}, a.frame...)
	w, h, aspect := a.fw, a.fh, a.aspect
	a.frameMu.Unlock()
	p, err := saveScreenshot(screenshotsDir(), s.game.Name, pix, w, h, aspect, time.Now())
	if err != nil {
		messageBox(a.hwnd, "Couldn't save the screenshot: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage("Screenshot saved: " + filepath.Base(p))
}

// readmePath: README.txt next to the program, if it's there.
func readmePath() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	p := filepath.Join(filepath.Dir(exe), "README.txt")
	if _, err := os.Stat(p); err != nil {
		return ""
	}
	return p
}

const keysHelp = "Keyboard\n" +
	"F2\tSave state (to the chosen slot)\n" +
	"F4\tLoad state\n" +
	"F3 / Shift+F3\tNext / previous save state slot\n" +
	"F6\tSave states window (all four slots)\n" +
	"Ctrl+Z\tUndo the last load\n" +
	"F9\tStart / stop recording\n" +
	"F12\tScreenshot (only the game's picture)\n" +
	"P\tPause\n" +
	"M\tMute\n" +
	"Tab (hold)\tFast forward\n" +
	"Ctrl+R\tReset\n" +
	"Ctrl+O\tOpen a game\n" +
	"F11, Alt+Enter\tFullscreen (Esc leaves it)\n" +
	"\nXbox controller\n" +
	"Right stick click (R3)\tSave state\n" +
	"Left stick click (L3)\tLoad state\n" +
	"Right stick left / right\tChange slot\n" +
	"Right stick down (1 s)\tStart / stop recording\n" +
	"Nintendo 64: View + D-pad left / right changes the slot, View + D-pad down records.\n" +
	"\nOn the start screen, the arrow keys or the D-pad pick a recent game and Enter or A starts it.\n" +
	"\nEach system's buttons are in Options > Controls (change them there)."
