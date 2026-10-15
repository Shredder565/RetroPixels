package main

// Archipelago in the window: opening patch files (asking for the original ROM),
// the clients' connection to the running game, its status in the title bar
// and the Game menu, and Help > Archipelago. app_windows.go calls in through
// the hooks named a.ap*.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"retropixels/internal/apatch"
	"retropixels/internal/apbridge"
)

const (
	cmdAPStatus = 1700 // Game > Archipelago: …
	cmdAPHelp   = 1701 // Help > Archipelago…
)

// apState is the window's part.
type apState struct {
	hub     *apbridge.Hub
	target  *apTarget // the running game's, while the clients can reach it
	msgs    []string  // messages from the clients, shown one at a time
	msgEnds time.Time
}

// apStartup: after the window is made. The SNI Client's address is served
// from the start (as RetroPixels.exe does) once there's a Super Nintendo core;
// the BizHawk ports only while a game runs.
func (a *app) apStartup() {
	h := apbridge.NewHub(apbridge.BizHawkFirstPort, apbridge.SNIPort)
	h.Name = "RetroPixels Native " + version
	h.OnChange = func() { a.event("ap-status", "") }
	a.ap.hub = h
	h.Start(available("snes"))
}

// apGameStarted: the clients can reach the game's memory.
func (a *app) apGameStarted(s *session) {
	if a.ap.hub == nil {
		return
	}
	g := apGameFor(s.game, s.core)
	if g == nil {
		return
	}
	t := &apTarget{s: s}
	a.ap.target = t
	a.ap.hub.SetGame(g, t)
}

// apGameClosed: before the session stops.
func (a *app) apGameClosed() {
	if t := a.ap.target; t != nil {
		a.ap.target = nil
		a.ap.hub.ClearGame(t)
	}
	a.ap.msgs = nil
}

func (a *app) apStatus() apbridge.Status {
	if a.ap.hub == nil {
		return apbridge.Status{}
	}
	return a.ap.hub.Status()
}

// apTitle: the status for the title bar (" - Archipelago: …"), and the
// clients' messages one after another, while the game shows them.
func (a *app) apTitle() string {
	s := a.sess
	if s == nil || a.ap.target == nil {
		return ""
	}
	if len(a.ap.msgs) > 0 && time.Now().After(a.ap.msgEnds) {
		a.flash, a.flashUntil = a.ap.msgs[0], time.Now().Add(1700*time.Millisecond)
		a.ap.msgs = a.ap.msgs[1:]
		a.ap.msgEnds = a.flashUntil
	}
	st := a.apStatus()
	if !apShown(s.game, st) {
		return ""
	}
	return " - " + apStatusText(s.game.System, st)
}

func (a *app) apEvent(name, detail string) {
	switch name {
	case "ap-status":
		a.updateTitle()
	case "ap-msg":
		if len(a.ap.msgs) < 20 {
			a.ap.msgs = append(a.ap.msgs, detail)
		}
		a.updateTitle()
	}
}

// ---------------- menus ----------------

// apGameMenu: in the Game menu (its label shows the status).
func (a *app) apGameMenu() {
	appendMenu(a.gameMenu, mfSeparator, 0, "")
	appendMenu(a.gameMenu, mfString, cmdAPStatus, "&Archipelago…")
}

// apHelpMenu: in the Help menu.
func (a *app) apHelpMenu(help uintptr) {
	appendMenu(help, mfString, cmdAPHelp, "&Archipelago (multiworld)…")
}

func (a *app) apUpdateMenus() {
	label := "&Archipelago…"
	if s := a.sess; s != nil && a.ap.target != nil {
		if st := a.apStatus(); apShown(s.game, st) {
			label = "&" + strings.ReplaceAll(apStatusText(s.game.System, st), "&", "&&") + "…"
		}
	}
	renameItem(a.menu, cmdAPStatus, label)
}

func (a *app) apCommand(id int) {
	switch id {
	case cmdAPStatus, cmdAPHelp:
		var g *loadedGame
		if a.sess != nil {
			g = a.sess.game
		}
		messageBox(a.hwnd, apHelpText(g, a.apStatus(), a.ap.target != nil), "Archipelago", mbIconInfo)
	}
}

// ---------------- opening a patch ----------------

// openAPPatch makes the game from an Archipelago patch file and starts it.
func (a *app) openAPPatch(path string) {
	p, err := readAPPatchFile(path)
	if err != nil {
		icon := uint32(mbIconWarning)
		if _, ok := err.(errNotYet); ok {
			icon = mbIconInfo
		}
		messageBox(a.hwnd, err.Error(), "RetroPixels Native", icon)
		return
	}
	name := filepath.Base(path)
	// the original ROM kept from last time
	if kept := a.settings.apBase(p); kept != "" {
		if base, err := readLimited(kept, apatch.MaxROM); err == nil {
			g, err := apMakeGame(p, path, base)
			if err == nil {
				a.startGame(g, path)
				return
			}
			if !apWrongBase(err) {
				messageBox(a.hwnd, fmt.Sprintf("Couldn't make the game from %q: %v", name, err), "RetroPixels Native", mbIconWarning)
				return
			}
		}
		a.settings.setAPBase(p, "")
		saveSettings(a.settings)
	}
	// ask for it
	if messageBox(a.hwnd, apAskText(p, path), "Archipelago", mbOKCancel|mbIconInfo) != idOK {
		return
	}
	exts := apBaseExts(p.System)
	filter := apGameName(p) + " ROM (" + strings.Join(exts, " ") + ")\x00*" + strings.Join(exts, ";*") + "\x00All files\x00*.*"
	dir := a.settings.LastFolder
	for {
		bp := fileDialog(a.hwnd, false, "Choose your "+apGameName(p)+" ROM", filter, dir, "", "")
		if bp == "" {
			return
		}
		dir = filepath.Dir(bp)
		base, err := readLimited(bp, apatch.MaxROM)
		var g *loadedGame
		if err == nil {
			g, err = apMakeGame(p, path, base)
		}
		if err != nil {
			if apWrongBase(err) {
				if messageBox(a.hwnd, capitalize(err.Error())+".\n\nChoose another ROM?", "Archipelago", mbOKCancel|mbIconWarning) != idOK {
					return
				}
				continue
			}
			messageBox(a.hwnd, fmt.Sprintf("Couldn't make the game from %q: %v", name, err), "RetroPixels Native", mbIconWarning)
			return
		}
		a.settings.setAPBase(p, bp)
		saveSettings(a.settings)
		a.startGame(g, path)
		return
	}
}

// apOpenPath: patch files open here (true); anything else is a game.
func (a *app) apOpenPath(path string) bool {
	if !apatch.IsPatchName(path) {
		return false
	}
	if _, err := os.Stat(path); err != nil {
		messageBox(a.hwnd, err.Error(), "RetroPixels Native", mbIconWarning)
		return true
	}
	a.openAPPatch(path)
	return true
}

func capitalize(s string) string {
	if s == "" {
		return s
	}
	return strings.ToUpper(s[:1]) + s[1:]
}
