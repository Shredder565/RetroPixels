package main

// PlayStation 3 games in the player's own RPCS3, as RetroPixels.exe's PS3
// tab plays them (internal/rpcs3): the same RPCS3 (the folder chosen with
// PS3 > Choose RPCS3 folder…, else where the player keeps it, else found on
// the drives; kept in %APPDATA%\RetroPixels\rpcs3.json for both programs),
// the same games (installed in RPCS3, and its disc games), the same
// command line. By default a game is fullscreen in RPCS3's own window; Play
// inside this window docks it here (dock_windows.go) instead. The Xbox
// controller is player 1 unless the player set RPCS3's controls up.
//
// PS3 > Install game (.pkg)…, Install PS3 system software… and Add licence
// file… hand the player's own files to RPCS3 (its installers, or a copy
// into exdata). A .pkg, .rap or .edat opened or dropped does the same.
// PS3 games aren't recorded: RPCS3 can't save its own picture for RetroPixels.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/rpcs3"
)

// Menu commands 1940–1999.
const (
	cmdPS3Stop          = 1940
	cmdPS3InstallPkg    = 1941
	cmdPS3AddLicence    = 1942
	cmdPS3InstallFw     = 1943
	cmdPS3Fullscreen    = 1944
	cmdPS3Embed         = 1945
	cmdPS3XboxPad       = 1946
	cmdPS3ChooseFolder  = 1947
	cmdPS3DefaultFolder = 1948
	cmdPS3OpenFolder    = 1949
	cmdPS3Status        = 1950
	cmdPS3Game0         = 1960 // + the game's place in the list (up to 40)
	ps3MaxGames         = 40
	cmdPS3First         = 1940
	cmdPS3Last          = 1999
)

// ps3Run is the game playing in RPCS3.
type ps3Run struct {
	extRun   // pid, exited, the docked window (dock_windows.go)
	game     rpcs3.Game
	out      *rpcs3.Output
	logFile  string
	err      error // how RPCS3 ended (set before exited closes)
	started  time.Time
	embed    bool
	wentFull bool // this window went fullscreen for the game
}

// ps3State is the app's PS3 part (UI thread only).
type ps3State struct {
	run        *ps3Run
	games      []rpcs3.Game // as the Play menu lists them
	gamesMenu  uintptr
	finding    bool   // looking for RPCS3 on the drives
	searched   bool   // looked since RetroPixels Native started
	installing string // the file RPCS3's installer is working on
}

func ps3AppDir() string {
	if exe, err := os.Executable(); err == nil {
		return filepath.Dir(exe)
	}
	return ""
}

// ---------------- menu ----------------

func (a *app) addPS3Menu() {
	m := call(pCreatePopupMenu)
	a.ps3.gamesMenu = call(pCreatePopupMenu)
	appendMenu(m, mfPopup, a.ps3.gamesMenu, "&Play")
	appendMenu(m, mfString, cmdPS3Stop, "&Stop game")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdPS3InstallPkg, "&Install game (.pkg)…")
	appendMenu(m, mfString, cmdPS3AddLicence, "Add &licence file (.rap / .edat)…")
	appendMenu(m, mfString, cmdPS3InstallFw, "Install PS3 system &software (PS3UPDAT.PUP)…")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdPS3Fullscreen, "Start &fullscreen")
	appendMenu(m, mfString, cmdPS3Embed, "Play inside this &window")
	appendMenu(m, mfString, cmdPS3XboxPad, "Use the &Xbox controller for player 1")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdPS3ChooseFolder, "&Choose RPCS3 folder…")
	appendMenu(m, mfString, cmdPS3DefaultFolder, "Find RPCS3 by &itself")
	appendMenu(m, mfString, cmdPS3OpenFolder, "&Open RPCS3's folder")
	appendMenu(m, mfString, cmdPS3Status, "What RPCS3 &needs…")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString|mfGrayed, 0, "Games play in your own RPCS3 (PS3 emulator, GPL)")
	appendMenu(a.menu, mfPopup, m, "PS&3")
}

// ps3Look reads RPCS3's state; when it hasn't been found, the drives are
// searched once in the background.
func (a *app) ps3Look() (rpcs3.Config, rpcs3.Status) {
	c := rpcs3.Load()
	st := rpcs3.Look(c, ps3AppDir())
	if st.Exe == "" && c.Folder == "" && !a.ps3.finding && !a.ps3.searched {
		a.ps3.finding = true
		go func() {
			p := rpcs3.Search(dolphin.WiiKeysScanRoots())
			if p != "" {
				rpcs3.Update(func(c *rpcs3.Config) { c.Exe, c.Detected = p, true })
			}
			a.event("ps3-found", p)
		}()
	}
	return c, st
}

func (a *app) ps3UpdateMenus() {
	c, st := a.ps3Look()
	m := a.ps3.gamesMenu
	for call(pGetMenuItemCount, m) > 0 {
		call(pDeleteMenu, m, 0, mfByPosition)
	}
	a.ps3.games = st.Games
	switch {
	case st.Exe == "" && a.ps3.finding:
		appendMenu(m, mfString|mfGrayed, 0, "(looking for RPCS3…)")
	case st.Exe == "":
		appendMenu(m, mfString|mfGrayed, 0, "(RPCS3 not found: PS3 > Choose RPCS3 folder…)")
	case len(st.Games) == 0:
		appendMenu(m, mfString|mfGrayed, 0, "(no games installed in RPCS3 yet: PS3 > Install game (.pkg)…)")
	}
	for i, g := range st.Games {
		if i >= ps3MaxGames {
			break
		}
		flags := uint32(mfString)
		if a.ps3.run != nil {
			flags |= mfGrayed
		}
		appendMenu(m, flags, uintptr(cmdPS3Game0+i), ps3GameLabel(g))
	}
	found := st.Exe != ""
	idle := a.ps3.run == nil && a.ps3.installing == ""
	enableItem(a.menu, cmdPS3Stop, a.ps3.run != nil)
	for _, id := range []int{cmdPS3InstallPkg, cmdPS3AddLicence, cmdPS3InstallFw, cmdPS3XboxPad, cmdPS3OpenFolder} {
		enableItem(a.menu, id, found && idle)
	}
	enableItem(a.menu, cmdPS3DefaultFolder, c.Folder != "")
	checkItem(a.menu, cmdPS3Fullscreen, c.FullscreenOn())
	checkItem(a.menu, cmdPS3Embed, c.EmbedOn())
	checkItem(a.menu, cmdPS3XboxPad, st.Pad == "XInput")
}

func (a *app) ps3Command(id int) {
	switch {
	case id >= cmdPS3Game0 && id < cmdPS3Game0+ps3MaxGames:
		if i := id - cmdPS3Game0; i < len(a.ps3.games) {
			a.ps3Play(a.ps3.games[i])
		}
	case id == cmdPS3Stop:
		a.ps3Stop()
	case id == cmdPS3InstallPkg:
		a.ps3InstallPkg("")
	case id == cmdPS3AddLicence:
		a.ps3AddLicence("")
	case id == cmdPS3InstallFw:
		a.ps3InstallFirmware()
	case id == cmdPS3Fullscreen:
		rpcs3.Update(func(c *rpcs3.Config) { v := !c.FullscreenOn(); c.Fullscreen = &v })
	case id == cmdPS3Embed:
		rpcs3.Update(func(c *rpcs3.Config) { v := !c.EmbedOn(); c.Embed = &v })
	case id == cmdPS3XboxPad:
		if root := a.ps3Root(); root != "" {
			if _, err := rpcs3.UseXboxPad(root, true); err != nil {
				messageBox(a.hwnd, "Couldn't change RPCS3's controller settings: "+err.Error(), "RetroPixels Native", mbIconWarning)
			} else {
				a.showMessage("Player 1 is the Xbox controller (RPCS3's file before is kept as Default.yml.bak)")
			}
		}
	case id == cmdPS3ChooseFolder:
		a.ps3ChooseFolder()
	case id == cmdPS3DefaultFolder:
		rpcs3.Update(func(c *rpcs3.Config) { c.Folder = "" })
		a.ps3.searched = false
	case id == cmdPS3OpenFolder:
		if root := a.ps3Root(); root != "" {
			shellOpen(root)
		}
	case id == cmdPS3Status:
		messageBox(a.hwnd, a.ps3StatusText(), "PlayStation 3 (RPCS3)", mbIconInfo)
	}
}

// ps3Root is RPCS3's folder ("" when it hasn't been found; it says so).
func (a *app) ps3Root() string {
	if exe := rpcs3.Load().ExeFor(ps3AppDir()); exe != "" {
		return rpcs3.Root(exe)
	}
	if messageBox(a.hwnd, rpcs3.NotFoundMessage+"\n\nChoose the RPCS3 folder now?", "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
		if a.ps3ChooseFolder() {
			return a.ps3Root()
		}
	}
	return ""
}

func (a *app) ps3ChooseFolder() bool {
	d := a.pickFolder(a.hwnd, "Choose the RPCS3 folder (the one "+rpcs3.ExeName+" is in)")
	if d == "" {
		return false
	}
	if rpcs3.ExeIn(d) == "" {
		messageBox(a.hwnd, rpcs3.ExeName+" isn't in "+d+".", "RetroPixels Native", mbIconWarning)
		return false
	}
	if _, err := rpcs3.Update(func(c *rpcs3.Config) { c.Folder = d }); err != nil {
		messageBox(a.hwnd, "Couldn't save the setting: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return false
	}
	a.showMessage("RPCS3: " + d)
	return true
}

// ps3StatusText: RPCS3, the system software and each game, with what's missing.
func (a *app) ps3StatusText() string {
	c, st := a.ps3Look()
	if st.Exe == "" {
		if a.ps3.finding {
			return "Looking for RPCS3 on your drives…"
		}
		if c.Folder != "" {
			return rpcs3.ExeName + " isn't in your chosen folder, " + c.Folder + "."
		}
		return rpcs3.NotFoundMessage
	}
	var b strings.Builder
	fmt.Fprintf(&b, "RPCS3: %s\n\n", st.Exe)
	if st.Firmware {
		fmt.Fprintf(&b, "PS3 system software: installed %s\n", st.FirmwareVersion)
	} else {
		b.WriteString(rpcs3.FirmwareMessage + "\n")
	}
	switch st.Pad {
	case "XInput":
		b.WriteString("Player 1: Xbox controller\n")
	case "":
		b.WriteString("Player 1: set to the Xbox controller when the first game starts\n")
	default:
		b.WriteString("Player 1: " + st.Pad + " (your RPCS3 setting; PS3 > Use the Xbox controller for player 1 changes it)\n")
	}
	b.WriteString("\nGames:\n")
	if len(st.Games) == 0 {
		b.WriteString("None installed yet: PS3 > Install game (.pkg)… (or RPCS3's File > Install Packages/Raps/Edats).\n")
	}
	for _, g := range st.Games {
		fmt.Fprintf(&b, "• %s (%s)", g.Title, g.TitleID)
		if !g.Licence.OK() {
			fmt.Fprintf(&b, ": needs its licence file %s.rap (or .edat). PS3 > Add licence file… and choose your own.", g.Licence.ContentID)
		}
		b.WriteString("\n")
	}
	return b.String()
}

// ---------------- your files to RPCS3 ----------------

// ps3AddLicence copies the player's licence file (p, or asked for) into RPCS3.
func (a *app) ps3AddLicence(p string) {
	root := a.ps3Root()
	if root == "" {
		return
	}
	if p == "" {
		p = a.fileDialog("Choose your licence file for a PS3 game (.rap or .edat)", "PS3 licences (*.rap;*.edat)\x00*.rap;*.edat\x00\x00", a.settings.LastFolder)
		if p == "" {
			return
		}
	}
	dst, err := rpcs3.AddLicence(root, p)
	if err != nil {
		messageBox(a.hwnd, "Couldn't add the licence file: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage("Licence added to RPCS3: " + filepath.Base(dst))
}

func (a *app) ps3InstallPkg(p string) {
	root := a.ps3Root()
	if root == "" {
		return
	}
	if p == "" {
		p = a.fileDialog("Choose a PS3 game package to install in RPCS3", "PS3 packages (*.pkg)\x00*.pkg\x00\x00", a.settings.LastFolder)
		if p == "" {
			return
		}
	}
	if err := rpcs3.CheckPkg(p); err != nil {
		messageBox(a.hwnd, err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.settings.LastFolder = filepath.Dir(p)
	saveSettings(a.settings)
	a.ps3RunInstaller(filepath.Join(root, rpcs3.ExeName), rpcs3.InstallPkgArgs(p), p)
}

func (a *app) ps3InstallFirmware() {
	root := a.ps3Root()
	if root == "" {
		return
	}
	p := a.fileDialog("Choose the PS3 system software (PS3UPDAT.PUP)", "PS3 system software (*.PUP)\x00*.PUP\x00\x00", a.settings.LastFolder)
	if p == "" {
		return
	}
	if err := rpcs3.CheckPUP(p); err != nil {
		messageBox(a.hwnd, err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.ps3RunInstaller(filepath.Join(root, rpcs3.ExeName), rpcs3.InstallFirmwareArgs(p), p)
}

// ps3RunInstaller runs RPCS3's own installer; ps3-installed comes when it closes.
func (a *app) ps3RunInstaller(exe string, args []string, file string) {
	if a.ps3.run != nil || a.ps3.installing != "" || dolphin.Running(rpcs3.ExeName) {
		messageBox(a.hwnd, "RPCS3 is already open. Close it first.", "RetroPixels Native", mbIconInfo)
		return
	}
	cmd, out := rpcs3.Command(exe, args...)
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start RPCS3: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.ps3.installing = filepath.Base(file)
	a.updateTitle()
	started := time.Now()
	go func() {
		err := cmd.Wait()
		msg := ""
		if err != nil && time.Since(started) < 5*time.Second {
			why := rpcs3.Problem(out.String())
			if why == "" {
				why = "RPCS3 closed straight away (" + err.Error() + ")"
			}
			msg = why
		}
		a.event("ps3-installed", msg)
	}()
}

// ps3OpenPath: a PS3 package or licence file opened or dropped goes to RPCS3.
func (a *app) ps3OpenPath(path string) bool {
	switch ps3FileKind(path) {
	case "pkg":
		if messageBox(a.hwnd, "Install "+filepath.Base(path)+" in RPCS3?", "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.ps3InstallPkg(path)
		}
		return true
	case "licence":
		if messageBox(a.hwnd, "Add the licence file "+filepath.Base(path)+" to RPCS3?", "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.ps3AddLicence(path)
		}
		return true
	}
	return false
}

// ---------------- playing ----------------

// ps3Busy: a PS3 game is on (so another external game can't start). It says so.
func (a *app) ps3Busy() bool {
	if r := a.ps3.run; r != nil {
		messageBox(a.hwnd, r.game.Title+" is playing in RPCS3. Stop it first (PS3 > Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	return false
}

func (a *app) ps3Play(g rpcs3.Game) {
	if a.ps3.run != nil {
		call(pSetForegroundWindow, a.hwnd)
		return
	}
	if a.mameBusy() || a.pixeluBusy() || a.supermodelBusy() {
		return
	}
	if r := a.dol.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in PixelU (Wii/GameCube). Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return
	}
	if a.ps3.installing != "" {
		messageBox(a.hwnd, "RPCS3 is still installing "+a.ps3.installing+". Start the game once it has finished.", "RetroPixels Native", mbIconInfo)
		return
	}
	root := a.ps3Root()
	if root == "" {
		return
	}
	exe := filepath.Join(root, rpcs3.ExeName)
	if ok, _ := rpcs3.Firmware(root); !ok {
		if messageBox(a.hwnd, rpcs3.FirmwareMessage+"\n\nInstall your PS3UPDAT.PUP now?", "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.ps3InstallFirmware()
		}
		return
	}
	if l := rpcs3.CheckLicence(root, g.Eboot); !l.OK() {
		if messageBox(a.hwnd, g.Title+": "+l.Message()+"\n\nChoose your licence file now?", "RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
			return
		}
		a.ps3AddLicence("")
		if !rpcs3.CheckLicence(root, g.Eboot).OK() {
			return
		}
	}
	if err := rpcs3.Ready(root, g); err != nil {
		messageBox(a.hwnd, g.Title+": "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	if dolphin.Running(rpcs3.ExeName) {
		messageBox(a.hwnd, "RPCS3 is already open. Close it first, then start the game here.", "RetroPixels Native", mbIconInfo)
		return
	}
	_, _ = rpcs3.UseXboxPad(root, false) // the Xbox controller as player 1, unless the player set RPCS3 up
	a.closeGame()
	c := rpcs3.Load()
	embed := c.EmbedOn()
	cmd, out := rpcs3.Command(exe, rpcs3.GameArgs(g.Eboot, rpcs3.LaunchOptions{Fullscreen: c.FullscreenOn(), Embed: embed})...)
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start RPCS3: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	r := &ps3Run{extRun: newExtRun(cmd.Process.Pid), game: g, out: out, logFile: rpcs3.LogFile(exe), started: time.Now(), embed: embed}
	a.ps3.run = r
	go func() {
		r.err = cmd.Wait()
		close(r.exited)
		a.event("ps3-exited", "")
	}()
	if embed {
		if c.FullscreenOn() && !a.fullscreen {
			a.toggleFullscreen()
			r.wentFull = true
		}
		go a.dockWatch(&r.extRun, 90*time.Second, "ps3") // shaders can take a while on the first start
	}
	rpcs3.Update(func(c *rpcs3.Config) { c.MarkPlayed(g.TitleID, time.Now()) })
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// ps3Stop ends the game the way closing RPCS3's game window does; if it
// doesn't react it's closed.
func (a *app) ps3Stop() {
	if r := a.ps3.run; r != nil {
		r.stop()
		a.showMessage("Stopping the game…")
	}
}

func (a *app) ps3Ended() {
	r := a.ps3.run
	if r == nil {
		return
	}
	a.ps3.run = nil
	if a.dol.run == nil && a.mame.run == nil && a.pu.run == nil && a.sm.run == nil {
		a.dolUnhook()
	}
	if r.wentFull && a.fullscreen {
		a.toggleFullscreen()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if r.stopAt.Load() == 0 && time.Since(r.started) < 20*time.Second {
		why := rpcs3.Problem(r.out.String() + "\n" + rpcs3.LogErrors(r.logFile, r.started))
		if why == "" && r.err != nil {
			why = "RPCS3 closed straight away (" + r.err.Error() + ")."
		}
		if why != "" {
			messageBox(a.hwnd, r.game.Title+" didn't start in RPCS3.\n\n"+why+"\n\nRPCS3's log: "+r.logFile, "RetroPixels Native", mbIconWarning)
		}
	}
	a.dolMaybeClose()
}

// ps3BeforeClose: closing the window while a game plays stops it first.
// It returns true to keep the window open for now.
func (a *app) ps3BeforeClose() bool {
	r := a.ps3.run
	if r == nil {
		return false
	}
	if r.stoppingFor(15 * time.Second) {
		if p, err := os.FindProcess(r.pid); err == nil {
			_ = p.Kill()
		}
		return false
	}
	a.dol.closeAfter = true // ps3Ended closes the window
	if r.stopAt.Load() == 0 {
		a.ps3Stop()
	}
	return true
}

// ps3ToggleRecording is F9 / Record while a PS3 game plays.
func (a *app) ps3ToggleRecording() bool {
	if a.ps3.run == nil {
		return false
	}
	a.showMessage("PS3 games aren't recorded: RPCS3 can't save its picture for RetroPixels (OBS Studio can record its window)")
	return true
}

func (a *app) ps3Ext() *extRun {
	if r := a.ps3.run; r != nil {
		return &r.extRun
	}
	return nil
}

// ---------------- events ----------------

func (a *app) ps3Event(name, detail string) {
	switch name {
	case "ps3-exited":
		a.ps3Ended()
	case "ps3-found":
		a.ps3.finding, a.ps3.searched = false, true
		if detail != "" {
			a.showMessage("RPCS3 found: " + detail)
		}
	case "ps3-installed":
		a.ps3.installing = ""
		a.updateTitle()
		if detail != "" {
			messageBox(a.hwnd, detail+".\n\nIf this RPCS3 can't install from RetroPixels, use its File > Install Packages/Raps/Edats (or File > Install Firmware) instead.",
				"RetroPixels Native", mbIconWarning)
		} else {
			a.showMessage("RPCS3's installer closed: PS3 > Play lists the installed games")
		}
	case "ps3-docked":
		a.dolHook() // F11 / Alt+Enter / Esc reach this window while RPCS3 has the keyboard
		call(pInvalidateRect, a.hwnd, 0, 0)
		a.updateTitle()
	case "ps3-nodock":
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
}

func (a *app) ps3Title() string {
	if a.ps3.installing != "" {
		return " - RPCS3 is installing " + a.ps3.installing
	}
	r := a.ps3.run
	if r == nil {
		return ""
	}
	t := " - " + r.game.Title + " (RPCS3)"
	if r.embed && r.child.Load() == 0 {
		t += " - Starting in RPCS3…"
	}
	return t
}

// ps3Paint draws the window while a game is on. Holds frameMu.
func (a *app) ps3Paint(hdc uintptr) bool {
	r := a.ps3.run
	if r == nil {
		return false
	}
	cr := clientRect(a.hwnd)
	call(pFillRect, hdc, uintptr(unsafe.Pointer(&cr)), call(pGetStockObject, blackBrush))
	if r.child.Load() != 0 {
		return true
	}
	text := "Starting " + r.game.Title + " in RPCS3…"
	if !r.embed {
		text = r.game.Title + " is playing in RPCS3's own window.\n\nClose that window, or use PS3 > Stop game, to come back."
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
