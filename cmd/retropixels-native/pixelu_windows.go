package main

// Wii U games, played in PixelU (RetroPixels' Wii U emulator, based on
// Cemu), as RetroPixels.exe does: the same PixelU.exe (chosen, next to
// RetroPixels, or in Downloads) and the same command line (internal/pixelu).
// PixelU shows the game in its own window; RetroPixels Native just starts it.
// (Docking it into this window and recording are RetroPixels.exe's for now.)
//
// app_windows.go calls in through pixeluOpenPath, pixeluCommand and the menu hooks.

import (
	"os"
	"path/filepath"
	"strings"

	"retropixels/internal/dolphin"
	"retropixels/internal/pixelu"
)

// Menu commands 1900–1919.
const (
	cmdPixelUOpenGame   = 1900
	cmdPixelUFullscreen = 1901
	cmdPixelUOpenEmu    = 1902
	cmdPixelUChangeExe  = 1903
	cmdPixelUFirst      = 1900
	cmdPixelULast       = 1919
)

func (a *app) addPixelUMenu() {
	m := call(pCreatePopupMenu)
	appendMenu(m, mfString, cmdPixelUOpenGame, "Open a Wii &U game…")
	appendMenu(m, mfString, cmdPixelUFullscreen, "Start games &fullscreen")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdPixelUOpenEmu, "&Open PixelU (controllers, graphics)")
	appendMenu(m, mfString, cmdPixelUChangeExe, "C&hange PixelU…")
	appendMenu(a.menu, mfPopup, m, "Wii &U")
}

func (a *app) pixeluUpdateMenus() {
	checkItem(a.menu, cmdPixelUFullscreen, a.settings.PixelU.fullscreen())
	enableItem(a.menu, cmdPixelUOpenEmu, a.pixeluExe() != "")
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
	case cmdPixelUFullscreen:
		v := !p.fullscreen()
		p.Fullscreen = &v
		saveSettings(a.settings)
	case cmdPixelUOpenEmu:
		exe := a.pixeluExe()
		if exe == "" {
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

// pixeluOpenPath handles a file opened or dropped: Wii U games start in
// PixelU. It returns false for files that are for the other systems.
func (a *app) pixeluOpenPath(path string) bool {
	g, ok := pixelu.GameFor(path)
	if !ok {
		return false
	}
	if a.mameBusy() {
		return true
	}
	if r := a.dol.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in Dolphin. Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	exe := a.pixeluExe()
	if exe == "" {
		if messageBox(a.hwnd, "Wii U games play in PixelU (RetroPixels' Wii U emulator, based on Cemu), and it wasn't found: "+
			"put its PixelU folder next to RetroPixels, or keep it in your Downloads folder.\n\nChoose PixelU.exe now?",
			"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return true
		}
		if exe = a.pixeluChooseExe(); exe == "" {
			return true
		}
	}
	if dolphin.Running(pixelu.ExeName) {
		messageBox(a.hwnd, "PixelU is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
		return true
	}
	a.closeGame()
	cmd := pixelu.Command(exe, pixelu.GameArgs(g.Path, pixelu.LaunchOptions{Fullscreen: a.settings.PixelU.fullscreen()})...)
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start PixelU: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return true
	}
	go cmd.Wait()
	a.settings.addRecent(g.Path)
	a.settings.LastFolder = filepath.Dir(g.Path)
	saveSettings(a.settings)
	a.showMessage("Starting " + g.Name + " in PixelU…")
	return true
}
