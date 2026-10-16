package main

// Arcade games in MAME, any version of it (internal/mame), as RetroPixels.exe
// plays them: the MAME versions you add or download (shared with
// RetroPixels.exe), which one each game uses, and the game docked inside this
// window (so OBS captures it), the same way as GameCube and Wii games.
//
// app_windows.go calls in through small hooks (mameOpenPath, mameCommand,
// mameEvent, ...); the library window is in mame_library_windows.go.

import (
	"fmt"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
	"retropixels/internal/mame"
)

// Menu commands 1800–1849 (the library window's controls are 1850–1899).
const (
	cmdMameLibrary  = 1800
	cmdMameStop     = 1801
	cmdMameEmbed    = 1802
	cmdMameAddExe   = 1803
	cmdMameFind     = 1804
	cmdMameDownload = 1805
	cmdMameOpen     = 1806
	cmdMameKeys     = 1807
	cmdMameFirst    = 1800
	cmdMameLast     = 1849
)

// mameRun is one game playing in MAME.
type mameRun struct {
	game    mame.Game
	label   string // "MAME 0.270"
	pid     int
	exited  chan struct{}
	out     *mame.Output
	err     error // how MAME ended (set before exited closes)
	started time.Time
	embed   bool
	child   atomic.Uintptr // MAME's window while it's docked here
	stopAt  atomic.Int64   // when Stop game was asked (Unix ns)
}

// mameState is the app's MAME part.
type mameState struct {
	run     *mameRun // UI thread only, like the rest unless noted
	menu    uintptr
	lib     *mameLibrary
	problem string // why the last game didn't start, for the library

	mu       sync.Mutex // the rest is shared with background work
	games    []mame.Game
	loaded   bool
	loading  bool
	again    bool
	finding  bool
	checking bool // asking each MAME which games it plays
	install  *mame.Progress
	rels     []mame.Release
	relsErr  string
	relsBusy bool
}

// ---------------- menu ----------------

func (a *app) addMameMenu() {
	m := call(pCreatePopupMenu)
	a.mame.menu = m
	appendMenu(m, mfString, cmdMameLibrary, "&Library and MAME versions…")
	appendMenu(m, mfString, cmdMameOpen, "&Open a game file in MAME…")
	appendMenu(m, mfString, cmdMameStop, "&Stop game")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdMameEmbed, "Play inside this &window")
	appendMenu(m, mfString, cmdMameKeys, "MAME's &keys")
	appendMenu(m, mfSeparator, 0, "")
	appendMenu(m, mfString, cmdMameAddExe, "&Add a MAME you have…")
	appendMenu(m, mfString, cmdMameFind, "&Find MAME on this PC")
	appendMenu(m, mfString, cmdMameDownload, "&Download a MAME version…")
	appendMenu(a.menu, mfPopup, m, "&Arcade (MAME)")
}

func (a *app) mameUpdateMenus() {
	c := mame.Load()
	r := a.mame.run
	enableItem(a.menu, cmdMameStop, r != nil)
	enableItem(a.menu, cmdMameOpen, r == nil)
	checkItem(a.menu, cmdMameEmbed, c.EmbedOn())
	a.mame.mu.Lock()
	finding := a.mame.finding
	a.mame.mu.Unlock()
	enableItem(a.menu, cmdMameFind, !finding)
}

func (a *app) mameCommand(id int) {
	switch id {
	case cmdMameLibrary:
		a.openMameLibrary()
	case cmdMameDownload:
		a.openMameLibrary()
		if l := a.mame.lib; l != nil {
			call(pSetFocus, l.ctl[idMameRels])
		}
	case cmdMameOpen:
		a.mameOpenFile()
	case cmdMameStop:
		a.mameStop()
	case cmdMameEmbed:
		c, _ := mame.Update(func(c *mame.Config) { v := !c.EmbedOn(); c.Embed = &v })
		if a.mame.run != nil {
			a.showMessage("This applies from the next game")
		} else if c.EmbedOn() {
			a.showMessage("MAME games play inside this window")
		} else {
			a.showMessage("MAME games play in MAME's own window")
		}
	case cmdMameAddExe:
		a.mameAddExe(a.hwnd)
	case cmdMameFind:
		a.mameDetect()
		a.showMessage("Looking for MAME on this PC…")
	case cmdMameKeys:
		messageBox(a.hwnd, mameKeysText, "MAME's keys", mbIconInfo)
	}
}

const mameKeysText = "In MAME (keyboard): 5 = insert coin, 1 = start, arrow keys = move, " +
	"Left Ctrl / Left Alt / Space = buttons 1–3, Tab = MAME's menu (controls, DIP switches, cheats), " +
	"P = pause, Shift+F7 / F7 = save / load state, F3 = reset, Esc = quit the game.\n\n" +
	"Xbox controllers work too. Click the game if the keyboard doesn't reach it. " +
	"Each MAME keeps its own settings, high scores and save states in its folder.\n\n" +
	"While the game is inside this window, F11 and Alt+Enter make the window fullscreen and Esc leaves fullscreen."

// ---------------- background work ----------------

// mameReload scans the ROM folders again (asking each new MAME version for
// its game names the first time).
func (a *app) mameReload() {
	a.mame.mu.Lock()
	if a.mame.loading {
		a.mame.again = true
		a.mame.mu.Unlock()
		return
	}
	a.mame.loading = true
	a.mame.mu.Unlock()
	go func() {
		for {
			c := mame.Load()
			games := mame.Library(c.Folders, mame.AllNames(c.Installs), c.Played)
			a.mame.mu.Lock()
			if a.mame.again {
				a.mame.again = false
				a.mame.mu.Unlock()
				continue
			}
			a.mame.games, a.mame.loaded, a.mame.loading, a.mame.checking = games, true, false, true
			a.mame.mu.Unlock()
			a.event("mame-lib", "")
			mame.CheckGames(c, games)
			a.mame.mu.Lock()
			a.mame.checking = false
			a.mame.mu.Unlock()
			a.event("mame-lib", "")
			return
		}
	}()
}

func (a *app) mameDetect() {
	a.mame.mu.Lock()
	if a.mame.finding {
		a.mame.mu.Unlock()
		return
	}
	a.mame.finding = true
	a.mame.mu.Unlock()
	go func() {
		_, n := mame.Detect()
		a.mame.mu.Lock()
		a.mame.finding = false
		a.mame.mu.Unlock()
		a.event("mame-found", fmt.Sprint(n))
	}()
}

func (a *app) mameLoadReleases(refresh bool) {
	a.mame.mu.Lock()
	if a.mame.relsBusy || (a.mame.rels != nil && !refresh) {
		a.mame.mu.Unlock()
		return
	}
	a.mame.relsBusy = true
	a.mame.mu.Unlock()
	go func() {
		rels, err := mame.NewInstaller().Releases()
		a.mame.mu.Lock()
		a.mame.relsBusy = false
		if err != nil {
			a.mame.relsErr = err.Error()
		} else {
			a.mame.rels, a.mame.relsErr = rels, ""
		}
		a.mame.mu.Unlock()
		a.event("mame-rels", "")
	}()
}

// mameDownload downloads one official release in the background.
func (a *app) mameDownload(rel mame.Release) {
	a.mame.mu.Lock()
	if a.mame.install != nil {
		a.mame.mu.Unlock()
		a.showMessage("A MAME download is already running")
		return
	}
	a.mame.install = &mame.Progress{State: "downloading", Version: rel.Version, Total: rel.Size}
	a.mame.mu.Unlock()
	a.updateTitle()
	go func() {
		in := mame.NewInstaller()
		in.Progress = func(p mame.Progress) {
			a.mame.mu.Lock()
			if a.mame.install != nil {
				*a.mame.install = p
			}
			a.mame.mu.Unlock()
			a.event("mame-install", "")
		}
		exe, err := in.Install(rel)
		if err != nil {
			a.event("mame-install-error", "MAME "+rel.Version+" couldn't be downloaded: "+err.Error())
			return
		}
		ver := mame.ProbeVersion(exe)
		if ver == "" {
			ver = rel.Version
		}
		mame.Update(func(c *mame.Config) { c.Add(mame.Install{Exe: exe, Version: ver}) })
		a.event("mame-installed", "MAME "+ver)
	}()
}

func mameInstallText(p mame.Progress) string {
	switch p.State {
	case "downloading":
		if p.Total > 0 {
			return fmt.Sprintf("Downloading MAME %s… %d%% (%s of %s)", p.Version, p.Got*100/p.Total, fmtSize(p.Got), fmtSize(p.Total))
		}
		return fmt.Sprintf("Downloading MAME %s… %s", p.Version, fmtSize(p.Got))
	case "installing":
		return "Unpacking MAME " + p.Version + "…"
	}
	return ""
}

// ---------------- choosing files ----------------

func (a *app) pickFile(owner uintptr, title, filter, dir string) string {
	buf := make([]uint16, 32768)
	o := openFileName{
		Owner:   owner,
		Filter:  u16z(filter),
		File:    &buf[0],
		MaxFile: uint32(len(buf)),
		Title:   u16(title),
		Flags:   ofnFileMustExist | ofnPathMustExist | ofnHideReadOnly | ofnExplorer | ofnNoChangeDir,
	}
	if dir != "" {
		o.InitialDir = u16(dir)
	}
	o.StructSize = uint32(unsafe.Sizeof(o))
	if call(pGetOpenFileNameW, uintptr(unsafe.Pointer(&o))) == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}

// mameAddExe asks for a MAME program and adds it.
func (a *app) mameAddExe(owner uintptr) {
	dir := ""
	if def, ok := mame.Load().DefaultInstall(); ok {
		dir = filepath.Dir(filepath.Dir(def.Exe))
	}
	p := a.pickFile(owner, "Choose a MAME (mame.exe) to add", "MAME (mame*.exe)\x00mame*.exe\x00Programs (*.exe)\x00*.exe\x00\x00", dir)
	if p == "" {
		return
	}
	in, err := mame.AddExe(p)
	if err != nil {
		messageBox(owner, "Couldn't add it: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.showMessage(in.Label() + " added")
	a.mameReload()
	if a.mame.lib != nil {
		a.mame.lib.showVersions()
	}
}

// mameOpenFile asks for a ROM set and plays it in MAME.
func (a *app) mameOpenFile() {
	p := a.pickFile(a.hwnd, "Choose an arcade game (MAME ROM set)", "MAME ROM sets (*.zip;*.7z)\x00*.zip;*.7z\x00\x00", a.settings.LastFolder)
	if p != "" {
		a.mamePlayPath(p, "")
	}
}

// ---------------- opening games ----------------

// mameOpenPath handles a file opened or dropped while a MAME game plays:
// that game is stopped first. It returns false otherwise.
func (a *app) mameOpenPath(path string) bool {
	r := a.mame.run
	if r == nil {
		return false
	}
	if messageBox(a.hwnd, fmt.Sprintf("Stop %s and open %q?", r.game.Name, filepath.Base(path)), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
		a.mameStop()
		go func() {
			<-r.exited
			a.event("mame-open", path)
		}()
	}
	return true
}

// mameOfferArcade: a zipped game RetroPixels' own emulators can't play is
// offered to MAME (when there is a MAME). It returns true when it was.
func (a *app) mameOfferArcade(path string, why error) bool {
	if !mame.IsRomSet(path) {
		return false
	}
	c := mame.Load()
	in, ok := c.For(mame.ShortName(path))
	if !ok {
		return false
	}
	msg := fmt.Sprintf("%v\n\nPlay %q in %s instead?", why, filepath.Base(path), in.Label())
	if messageBox(a.hwnd, msg, "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
		a.mamePlayPath(path, "")
	}
	return true
}

// mameBusy: a MAME game is on (so GameCube / Wii can't start). It says so.
func (a *app) mameBusy() bool {
	if r := a.mame.run; r != nil {
		messageBox(a.hwnd, r.game.Name+" is playing in "+r.label+". Stop it first (Arcade (MAME) > Stop game).", "RetroPixels Native", mbIconInfo)
		return true
	}
	return false
}

// mamePlayPath plays a ROM set file in MAME (exe: this version, "" = the game's own).
func (a *app) mamePlayPath(path, exe string) {
	g := mame.Game{Short: mame.ShortName(path), Name: mame.ShortName(path), Path: path}
	a.mame.mu.Lock()
	for _, x := range a.mame.games {
		if strings.EqualFold(x.Path, path) {
			g = x
		}
	}
	a.mame.mu.Unlock()
	a.mamePlay(g, exe)
}

// mamePlay starts a game in its MAME.
func (a *app) mamePlay(g mame.Game, exe string) {
	if r := a.mame.run; r != nil {
		if strings.EqualFold(r.game.Path, g.Path) {
			call(pSetForegroundWindow, a.hwnd)
			return
		}
		if messageBox(a.hwnd, fmt.Sprintf("Stop %s and start %s?", r.game.Name, g.Name), "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.mameStop()
			go func() {
				<-r.exited
				a.event("mame-open", g.Path)
			}()
		}
		return
	}
	if a.dol.run != nil {
		messageBox(a.hwnd, "A GameCube / Wii game is playing. Stop it first (GameCube / Wii > Stop game).", "RetroPixels Native", mbIconInfo)
		return
	}
	c := mame.Load()
	if len(c.Installs) == 0 {
		switch messageBox(a.hwnd, "Arcade games play in MAME, a free emulator, and no MAME is set up yet.\n\n"+
			"Yes: choose a mame.exe you already have (any version).\n"+
			"No: open the library to download an official MAME version.", "RetroPixels Native", 3 /* MB_YESNOCANCEL */ |mbIconInfo) {
		case idYes:
			a.mameAddExe(a.hwnd)
			if c = mame.Load(); len(c.Installs) == 0 {
				return
			}
		case 7: // IDNO
			a.openMameLibrary()
			return
		default:
			return
		}
	}
	embed := c.EmbedOn()
	l, err := mame.Plan(c, g.Path, exe, mame.LaunchOptions{Embed: embed})
	if err != nil {
		messageBox(a.hwnd, err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	if g.Name == g.Short && l.Name != "" {
		g.Name = l.Name
	}
	a.closeGame()
	cmd, out := mame.Command(l.Install.Exe, l.Args...)
	if err := cmd.Start(); err != nil {
		messageBox(a.hwnd, "Couldn't start "+l.Install.Label()+": "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	r := &mameRun{game: g, label: l.Install.Label(), pid: cmd.Process.Pid, exited: make(chan struct{}), out: out,
		started: time.Now(), embed: embed}
	a.mame.run = r
	a.mame.problem = ""
	go func() {
		r.err = cmd.Wait()
		close(r.exited)
		a.event("mame-exited", "")
	}()
	if embed {
		go a.mameWatch(r)
	}
	mame.Update(func(c *mame.Config) { c.MarkPlayed(g.Short, time.Now()) })
	a.mame.mu.Lock()
	for i := range a.mame.games {
		if a.mame.games[i].Short == g.Short {
			a.mame.games[i].Played = time.Now()
		}
	}
	a.mame.mu.Unlock()
	a.settings.addRecent(g.Path)
	a.settings.LastFolder = filepath.Dir(g.Path)
	saveSettings(a.settings)
	if a.mame.lib != nil {
		a.mame.lib.fill()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

// mameStop ends the game the way MAME's own close button does (high
// scores and NVRAM are saved); if MAME doesn't react it's closed.
func (a *app) mameStop() {
	r := a.mame.run
	if r == nil {
		return
	}
	r.stopAt.Store(time.Now().UnixNano())
	if c := r.child.Load(); c != 0 {
		dolphin.Close(c)
	} else {
		go dolphin.Shutdown(r.pid, r.exited, 10*time.Second)
	}
	a.showMessage("Stopping the game…")
}

// mameWatch docks MAME's window into this window and keeps it there (as
// dolWatch does for Dolphin).
func (a *app) mameWatch(r *mameRun) {
	host := a.hwnd
	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	deadline := time.Now().Add(60 * time.Second) // a big game's first start can take a while
	var child uintptr
	var lostAt time.Time
	for {
		select {
		case <-r.exited:
			return
		case <-tick.C:
		}
		if child != 0 {
			if dolphin.Docked(child, host) {
				if at := r.stopAt.Load(); at != 0 && time.Since(time.Unix(0, at)) > 8*time.Second {
					dolphin.Shutdown(r.pid, r.exited, 3*time.Second) // Stop game was asked but MAME didn't react
					return
				}
				a.mamePlace(child, false)
				continue
			}
			// MAME's window went away (it can make a new one, e.g. when its video settings change).
			child, lostAt = 0, time.Now()
			r.child.Store(0)
		}
		h := dolphin.GameWindow(r.pid)
		if h == 0 {
			if !lostAt.IsZero() && time.Since(lostAt) > 3*time.Second {
				dolphin.Shutdown(r.pid, r.exited, 5*time.Second)
				return
			}
			if lostAt.IsZero() && time.Now().After(deadline) {
				a.event("mame-nodock", "")
				return // never showed a window: leave MAME as it is
			}
			continue
		}
		dolphin.Dock(h, host)
		a.mamePlace(h, true)
		call(pSetForegroundWindow, host)
		dolphin.Focus(h)
		child, lostAt = h, time.Time{}
		r.child.Store(h)
		a.event("mame-docked", "")
	}
}

func (a *app) mamePlace(child uintptr, force bool) {
	cr := clientRect(a.hwnd)
	if cr.Right > 0 && cr.Bottom > 0 {
		dolphin.Place(child, a.hwnd, 0, 0, cr.Right, cr.Bottom, force)
	}
}

// mameResize follows the window's size (WM_SIZE).
func (a *app) mameResize() {
	if r := a.mame.run; r != nil {
		if c := r.child.Load(); c != 0 {
			a.mamePlace(c, false)
		}
	}
}

// mameEnded tidies up after MAME has closed, and says why if the game
// didn't start.
func (a *app) mameEnded() {
	r := a.mame.run
	if r == nil {
		return
	}
	a.mame.run = nil
	if a.dol.run == nil {
		a.dolUnhook()
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
	call(pSetForegroundWindow, a.hwnd)
	if r.err != nil && r.stopAt.Load() == 0 && time.Since(r.started) < 20*time.Second {
		why := mame.Problem(r.out.String())
		if why == "" {
			why = r.label + " closed straight away (" + r.err.Error() + ")."
		}
		{
			a.mame.problem = r.game.Name + " didn't start in " + r.label + ": " + why
			if a.mame.lib != nil {
				a.mame.lib.showProblem()
			}
			messageBox(a.hwnd, r.game.Name+" didn't start in "+r.label+".\n\n"+why+
				"\n\nPick another MAME version for it in Arcade (MAME) > Library.", "RetroPixels Native", mbIconWarning)
		}
	}
	if a.dol.closeAfter {
		call(pPostMessageW, a.hwnd, wmClose, 0, 0)
	}
}

// mameBeforeClose: closing the window while a MAME game plays stops the
// game first. It returns true to keep the window open for now.
func (a *app) mameBeforeClose() bool {
	r := a.mame.run
	if r == nil {
		return false
	}
	if at := r.stopAt.Load(); at != 0 && time.Since(time.Unix(0, at)) > 15*time.Second {
		return false // MAME isn't closing: don't hold the window hostage
	}
	a.dol.closeAfter = true // mameEnded closes the window
	if r.stopAt.Load() == 0 {
		a.mameStop()
	}
	return true
}

// ---------------- events ----------------

func (a *app) mameEvent(name, detail string) {
	switch name {
	case "mame-exited":
		a.mameEnded()
	case "mame-docked":
		a.dolHook() // F11 / Alt+Enter / Esc reach this window while MAME has the keyboard
		call(pInvalidateRect, a.hwnd, 0, 0)
		a.updateTitle()
	case "mame-nodock":
		a.updateTitle()
		call(pInvalidateRect, a.hwnd, 0, 0)
	case "mame-open":
		if !a.dol.closeAfter {
			a.openPath(detail)
		}
	case "mame-lib":
		if a.mame.lib != nil {
			a.mame.lib.fill()
		}
	case "mame-found":
		if detail == "0" {
			a.showMessage("No other MAME found: add one or download one")
		} else {
			a.showMessage("Found " + detail + " MAME version(s)")
		}
		a.mameReload()
		if a.mame.lib != nil {
			a.mame.lib.showVersions()
			a.mame.lib.fillFolders()
		}
	case "mame-rels":
		if a.mame.lib != nil {
			a.mame.lib.fillReleases()
		}
	case "mame-install":
		a.updateTitle()
		if a.mame.lib != nil {
			a.mame.lib.showDownload()
		}
	case "mame-installed", "mame-install-error":
		a.mame.mu.Lock()
		a.mame.install = nil
		a.mame.mu.Unlock()
		a.updateTitle()
		if name == "mame-installed" {
			a.showMessage(detail + " is ready")
			a.mameReload()
		} else {
			messageBox(a.hwnd, detail, "RetroPixels Native", mbIconWarning)
		}
		if a.mame.lib != nil {
			a.mame.lib.showVersions()
			a.mame.lib.showDownload()
			a.mame.lib.fillReleases()
		}
	}
}

// mameTitle: what's going on, for the title bar.
func (a *app) mameTitle() string {
	t := ""
	if r := a.mame.run; r != nil {
		t += " - " + r.game.Name + " (" + r.label + ")"
		if r.embed && r.child.Load() == 0 {
			t += " - Starting in MAME…"
		}
	}
	a.mame.mu.Lock()
	inst := a.mame.install
	a.mame.mu.Unlock()
	if inst != nil {
		t += " - " + mameInstallText(*inst)
	}
	return t
}

// mamePaint draws the window while a MAME game is on (the game itself is
// MAME's docked window on top). Holds frameMu.
func (a *app) mamePaint(hdc uintptr) bool {
	r := a.mame.run
	if r == nil {
		return false
	}
	cr := clientRect(a.hwnd)
	call(pFillRect, hdc, uintptr(unsafe.Pointer(&cr)), call(pGetStockObject, blackBrush))
	if r.child.Load() != 0 {
		return true
	}
	text := "Starting " + r.game.Name + " in " + r.label + "…"
	if !r.embed {
		text = r.game.Name + " is playing in " + r.label + "'s own window.\n\nQuit the game there (Esc) or use Arcade (MAME) > Stop game to come back."
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

// mameIdleNote: a line for the start screen.
func mameIdleNote() string {
	return "\nAny other arcade game plays in MAME, in any MAME version you add: Arcade (MAME) > Library."
}
