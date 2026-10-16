package main

// The Arcade (MAME) library window (RetroPixels.exe's Arcade (MAME) tab):
// the MAME versions (added, found or downloaded), the ROM folders, and the
// games in them, each with the MAME version it plays in.

import (
	"fmt"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/mame"
)

const (
	idMameVersionsL = 1850
	idMameVersions  = 1851
	idMameAdd       = 1852
	idMameDefault   = 1853
	idMameRemove    = 1854
	idMameFind      = 1855
	idMameRelsL     = 1856
	idMameRels      = 1857
	idMameDownload  = 1858
	idMameDlStatus  = 1859
	idMameFoldersL  = 1860
	idMameFolders   = 1861
	idMameAddFolder = 1862
	idMameRmFolder  = 1863
	idMameProblem   = 1864
	idMameSearch    = 1865
	idMameCount     = 1866
	idMameList      = 1867
	idMameHint      = 1868
	idMameWithL     = 1869
	idMameWith      = 1870
	idMamePlay      = 1871
	idMameClose     = 1872
	idMameEmbed     = 1873
	timerMamePad    = 1880
)

var (
	mameLibClassDone bool
	mameLibCallback  uintptr // mameLibProc (set in init: the window procedure leads back here)
)

func init() { mameLibCallback = syscall.NewCallback(mameLibProc) }

type mameLibrary struct {
	a        *app
	hwnd     uintptr
	ctl      map[int]uintptr
	installs []mame.Install // in the versions list, in order
	shown    []mame.Game    // in the games list, in order
	rels     []mame.Release // in the download list, in order
	withList []string       // the "Play with" choices' exes ("" = default)
	padPrev  PadState
	padHeld  time.Time
}

func (a *app) openMameLibrary() {
	if l := a.mame.lib; l != nil {
		call(pShowWindow, l.hwnd, swShow)
		call(pSetForegroundWindow, l.hwnd)
		return
	}
	if !dolCommonControlsLoaded {
		icc := struct{ Size, ICC uint32 }{8, 0x1 | 0x4000} // ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES
		call(dpInitCommonControlsEx, uintptr(unsafe.Pointer(&icc)))
		dolCommonControlsLoaded = true
	}
	cls := u16("RetroPixelsNativeMameLibrary")
	if !mameLibClassDone {
		wc := wndClassEx{WndProc: mameLibCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls} // COLOR_BTNFACE + 1
		wc.Icon = call(pLoadImageW, a.hinst, 1, 1, 0, 0, 0x40|0x8000)
		wc.IconSm = call(pLoadImageW, a.hinst, 1, 1, 16, 16, 0x8000)
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		mameLibClassDone = true
	}
	l := &mameLibrary{a: a, ctl: map[int]uintptr{}}
	a.mame.lib = l
	dpi := a.dpi()
	w, h := 900*dpi/96, 720*dpi/96
	wr := windowRect(a.hwnd)
	x := max(0, int(wr.Left)+(int(wr.Right-wr.Left)-w)/2)
	y := max(0, int(wr.Top)+(int(wr.Bottom-wr.Top)-h)/2)
	l.hwnd = call(pCreateWindowExW, wsExControlParent, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("Arcade games in MAME"))),
		wsOverlappedWindow|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), a.hwnd, 0, a.hinst, 0)
	l.build()
	call(pShowWindow, l.hwnd, swShow)
	call(pSetTimer, l.hwnd, timerMamePad, 50, 0)
	l.showVersions()
	l.showDownload()
	l.showProblem()
	a.mame.mu.Lock()
	loaded := a.mame.loaded
	a.mame.mu.Unlock()
	if loaded {
		l.fill()
	}
	if c := mame.Load(); !c.Detected {
		a.mameDetect() // the first time: look for MAME on this PC
	} else {
		a.mameReload() // the folders may have changed since (RetroPixels.exe shares them)
	}
	a.mameLoadReleases(false)
	l.fillReleases()
	call(pSetFocus, l.ctl[idMameList])
}

func (l *mameLibrary) scale(v int) int { return v * l.a.dpi() / 96 }

func (l *mameLibrary) child(class, text string, style uint32, exStyle uint32, id int) uintptr {
	c := call(pCreateWindowExW, uintptr(exStyle), uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), 0, 0, 10, 10, l.hwnd, uintptr(id), l.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, l.a.font, 1)
	l.ctl[id] = c
	return c
}

func (l *mameLibrary) columns(lv uintptr, cols []struct {
	name string
	w    int
}) {
	call(pSendMessageW, lv, lvmSetExStyle, 0x20|0x10000, 0x20|0x10000) // full-row select, double-buffered
	for i, c := range cols {
		col := lvColumn{Mask: 1 | 2 | 4 | 8, Cx: int32(l.scale(c.w)), Text: u16(c.name), SubItem: int32(i)}
		call(pSendMessageW, lv, lvmInsertColumnW, uintptr(i), uintptr(unsafe.Pointer(&col)))
	}
}

func (l *mameLibrary) build() {
	l.child("STATIC", "MAME versions (add as many as you like; each game can use its own):", ssNoPrefix, 0, idMameVersionsL)
	v := l.child("SysListView32", "", lvsReport|lvsSingleSel|lvsShowSelAlways|wsTabStop, 0x200, idMameVersions)
	l.columns(v, []struct {
		name string
		w    int
	}{{"Version", 110}, {"", 70}, {"Program", 470}})
	l.child("BUTTON", "Add MAME…", bsPushButton|wsTabStop, 0, idMameAdd)
	l.child("BUTTON", "Make default", bsPushButton|wsTabStop, 0, idMameDefault)
	l.child("BUTTON", "Remove", bsPushButton|wsTabStop, 0, idMameRemove)
	l.child("BUTTON", "Find on this PC", bsPushButton|wsTabStop, 0, idMameFind)
	l.child("STATIC", "Download an official version:", ssNoPrefix, 0, idMameRelsL)
	l.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, idMameRels)
	l.child("BUTTON", "Download", bsPushButton|wsTabStop, 0, idMameDownload)
	l.child("STATIC", "", ssNoPrefix, 0, idMameDlStatus)
	l.child("STATIC", "ROM folders:", ssNoPrefix, 0, idMameFoldersL)
	l.child("LISTBOX", "", lbsNotify|lbsNoIntegral|wsVScroll|wsBorder|wsTabStop, 0, idMameFolders)
	l.child("BUTTON", "Add folder…", bsPushButton|wsTabStop, 0, idMameAddFolder)
	l.child("BUTTON", "Remove folder", bsPushButton|wsTabStop, 0, idMameRmFolder)
	l.child("STATIC", "", ssNoPrefix, 0, idMameProblem)
	s := l.child("EDIT", "", esAutoHScroll|wsTabStop, 0x200, idMameSearch)
	call(pSendMessageW, s, emSetCueBanner, 1, uintptr(unsafe.Pointer(u16("Search games…"))))
	l.child("STATIC", "", ssNoPrefix, 0, idMameCount)
	lv := l.child("SysListView32", "", lvsReport|lvsSingleSel|lvsShowSelAlways|wsTabStop, 0x200, idMameList)
	l.columns(lv, []struct {
		name string
		w    int
	}{{"Game", 300}, {"Set", 80}, {"Plays in", 300}, {"Last played", 110}})
	l.child("STATIC", "Double-click a game, or press Enter (or A on the controller), to play it. "+
		"ROM sets must match the MAME version they were made for: if a game doesn't start, pick another version for it below.", ssNoPrefix, 0, idMameHint)
	l.child("STATIC", "Play this game in:", ssNoPrefix, 0, idMameWithL)
	l.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, idMameWith)
	l.child("BUTTON", "Play inside this window", 0x3 /* BS_AUTOCHECKBOX */ |wsTabStop, 0, idMameEmbed)
	l.child("BUTTON", "Play", bsDefPushButton|wsTabStop, 0, idMamePlay)
	l.child("BUTTON", "Close", bsPushButton|wsTabStop, 0, idMameClose)
	l.fillFolders()
	l.layout()
}

func (l *mameLibrary) layout() {
	cr := clientRect(l.hwnd)
	W, H := int(cr.Right), int(cr.Bottom)
	m, gap := l.scale(12), l.scale(8)
	bw, bh := l.scale(130), l.scale(26)
	put := func(id, x, y, w, h int) {
		call(pSetWindowPos, l.ctl[id], 0, uintptr(x), uintptr(y), uintptr(max(1, w)), uintptr(max(1, h)), swpNoZOrder|swpNoActivate)
	}
	right := W - m - bw
	y := m
	put(idMameVersionsL, m, y, W-2*m, l.scale(18))
	y += l.scale(20)
	vh := 4*bh + 3*l.scale(4)
	put(idMameVersions, m, y, right-m-gap, vh)
	for i, id := range []int{idMameAdd, idMameDefault, idMameRemove, idMameFind} {
		put(id, right, y+i*(bh+l.scale(4)), bw, bh)
	}
	y += vh + gap
	put(idMameRelsL, m, y+l.scale(4), l.scale(190), l.scale(20))
	put(idMameRels, m+l.scale(195), y, l.scale(230), l.scale(300))
	put(idMameDownload, m+l.scale(435), y, l.scale(100), bh)
	put(idMameDlStatus, m+l.scale(545), y+l.scale(4), W-m-(m+l.scale(545)), l.scale(20))
	y += bh + gap
	put(idMameFoldersL, m, y, l.scale(200), l.scale(18))
	y += l.scale(20)
	fh := 2*bh + l.scale(4)
	put(idMameFolders, m, y, right-m-gap, fh)
	put(idMameAddFolder, right, y, bw, bh)
	put(idMameRmFolder, right, y+bh+l.scale(4), bw, bh)
	y += fh + gap
	put(idMameProblem, m, y, W-2*m, l.scale(34))
	y += l.scale(36)
	put(idMameSearch, m, y, l.scale(300), l.scale(24))
	put(idMameCount, m+l.scale(310), y+l.scale(4), W-m-(m+l.scale(310)), l.scale(20))
	y += l.scale(32)
	bottom := H - m - bh
	hintH := l.scale(36)
	put(idMameList, m, y, W-2*m, bottom-gap-hintH-y)
	put(idMameHint, m, bottom-hintH-l.scale(2), W-2*m, hintH)
	put(idMameWithL, m, bottom+l.scale(4), l.scale(120), l.scale(20))
	put(idMameWith, m+l.scale(122), bottom, l.scale(210), l.scale(300))
	put(idMameEmbed, m+l.scale(345), bottom, l.scale(200), bh)
	put(idMamePlay, W-m-2*l.scale(100)-gap, bottom, l.scale(100), bh)
	put(idMameClose, W-m-l.scale(100), bottom, l.scale(100), bh)
}

func (l *mameLibrary) setText(id int, s string) { setWindowText(l.ctl[id], s) }

func (l *mameLibrary) enable(id int, on bool) { call(pEnableWindow, l.ctl[id], boolArg(on)) }

// showVersions fills the MAME versions list (and what depends on it).
func (l *mameLibrary) showVersions() {
	c := mame.Load()
	def, _ := c.DefaultInstall()
	lv := l.ctl[idMameVersions]
	sel := -1
	if i := l.selectedIn(idMameVersions, len(l.installs)); i >= 0 {
		sel = i
	}
	prev := ""
	if sel >= 0 {
		prev = l.installs[sel].Exe
	}
	l.installs = c.Installs
	call(pSendMessageW, lv, lvmDeleteAll, 0, 0)
	selIx := -1
	for i, in := range c.Installs {
		label := in.Label()
		it := lvItem{Mask: 1, Item: int32(i), Text: u16(label)}
		call(pSendMessageW, lv, lvmInsertItemW, 0, uintptr(unsafe.Pointer(&it)))
		mark := ""
		if strings.EqualFold(in.Exe, def.Exe) {
			mark = "default"
		}
		for col, text := range []string{mark, in.Exe} {
			sub := lvItem{Mask: 1, Item: int32(i), SubItem: int32(col + 1), Text: u16(text)}
			call(pSendMessageW, lv, lvmSetItemTextW, uintptr(i), uintptr(unsafe.Pointer(&sub)))
		}
		if strings.EqualFold(in.Exe, prev) {
			selIx = i
		}
	}
	if selIx < 0 && len(c.Installs) > 0 {
		selIx = 0
	}
	if selIx >= 0 {
		l.selectRowIn(idMameVersions, selIx)
	}
	l.enable(idMameDefault, len(c.Installs) > 1)
	l.enable(idMameRemove, len(c.Installs) > 0)
	l.a.mame.mu.Lock()
	finding := l.a.mame.finding
	l.a.mame.mu.Unlock()
	l.enable(idMameFind, !finding)
	if finding {
		l.setText(idMameFind, "Looking…")
	} else {
		l.setText(idMameFind, "Find on this PC")
	}
	call(pSendMessageW, l.ctl[idMameEmbed], 0x00F1 /* BM_SETCHECK */, boolArg(c.EmbedOn()), 0)
	l.fill()
}

func (l *mameLibrary) fillReleases() {
	a := l.a
	a.mame.mu.Lock()
	rels, relsErr, busy := a.mame.rels, a.mame.relsErr, a.mame.relsBusy
	a.mame.mu.Unlock()
	cb := l.ctl[idMameRels]
	keep := ""
	if i := int(int32(call(pSendMessageW, cb, cbGetCurSel, 0, 0))); i >= 0 && i < len(l.rels) {
		keep = l.rels[i].Version
	}
	have := map[string]bool{}
	for _, in := range mame.Load().Installs {
		have[in.Version] = true
	}
	call(pSendMessageW, cb, cbResetContent, 0, 0)
	l.rels = rels
	sel := 0
	for i, r := range rels {
		t := "MAME " + r.Version
		if !r.Published.IsZero() {
			t += fmt.Sprintf(" (%d)", r.Published.Year())
		}
		if have[r.Version] {
			t += " ✓ have it"
		}
		call(pSendMessageW, cb, cbAddString, 0, uintptr(unsafe.Pointer(u16(t))))
		if r.Version == keep {
			sel = i
		}
	}
	switch {
	case len(rels) > 0:
		call(pSendMessageW, cb, cbSetCurSel, uintptr(sel), 0)
	case busy:
		call(pSendMessageW, cb, cbAddString, 0, uintptr(unsafe.Pointer(u16("Loading the list…"))))
		call(pSendMessageW, cb, cbSetCurSel, 0, 0)
	default:
		call(pSendMessageW, cb, cbAddString, 0, uintptr(unsafe.Pointer(u16("Couldn't load the list (click Download to retry)"))))
		call(pSendMessageW, cb, cbSetCurSel, 0, 0)
	}
	if relsErr != "" && len(rels) == 0 {
		l.setText(idMameDlStatus, relsErr)
	}
	l.showDownload()
}

func (l *mameLibrary) showDownload() {
	a := l.a
	a.mame.mu.Lock()
	inst, relsErr, nRels := a.mame.install, a.mame.relsErr, len(a.mame.rels)
	a.mame.mu.Unlock()
	switch {
	case inst != nil:
		l.setText(idMameDlStatus, mameInstallText(*inst))
	case relsErr != "" && nRels == 0:
		l.setText(idMameDlStatus, relsErr)
	default:
		l.setText(idMameDlStatus, "From the MAME team's official releases (github.com/mamedev/mame), each in its own folder.")
	}
	l.enable(idMameDownload, inst == nil)
}

func (l *mameLibrary) showProblem() {
	l.setText(idMameProblem, l.a.mame.problem)
}

func (l *mameLibrary) fillFolders() {
	lb := l.ctl[idMameFolders]
	call(pSendMessageW, lb, lbResetContent, 0, 0)
	c := mame.Load()
	for _, f := range c.Folders {
		call(pSendMessageW, lb, lbAddString, 0, uintptr(unsafe.Pointer(u16(f))))
	}
	l.enable(idMameRmFolder, len(c.Folders) > 0)
}

// fill shows the games (after a search or a new scan).
func (l *mameLibrary) fill() {
	a := l.a
	a.mame.mu.Lock()
	games, loaded, loading, checking := a.mame.games, a.mame.loaded, a.mame.loading, a.mame.checking
	a.mame.mu.Unlock()
	c := mame.Load()
	query := windowString(l.ctl[idMameSearch])
	sel := ""
	if i := l.selectedIn(idMameList, len(l.shown)); i >= 0 {
		sel = l.shown[i].Path
	}
	l.shown = mame.Filter(games, query, "az")
	lv := l.ctl[idMameList]
	call(pSendMessageW, lv, wmSetRedraw, 0, 0)
	call(pSendMessageW, lv, lvmDeleteAll, 0, 0)
	now := time.Now()
	selIx := -1
	for i, g := range l.shown {
		it := lvItem{Mask: 1, Item: int32(i), Text: u16(g.Name)}
		call(pSendMessageW, lv, lvmInsertItemW, 0, uintptr(unsafe.Pointer(&it)))
		ver := "—"
		if st := c.Status(g); st.Label != "" {
			ver = st.Label
			if def, _ := c.DefaultInstall(); strings.EqualFold(def.Exe, st.Exe) {
				if _, own := c.Games[g.Short]; !own {
					ver += " (default)"
				}
			}
			switch st.Result {
			case mame.Good:
				ver = "✓ " + ver
			case mame.Bad:
				ver = "✗ " + ver + ": won't start"
				if len(st.PlaysIn) > 0 {
					ver += "; plays in " + strings.Join(st.PlaysIn, ", ")
				}
			}
		}
		for col, text := range []string{g.Short, ver, fmtPlayed(g.Played, now)} {
			sub := lvItem{Mask: 1, Item: int32(i), SubItem: int32(col + 1), Text: u16(text)}
			call(pSendMessageW, lv, lvmSetItemTextW, uintptr(i), uintptr(unsafe.Pointer(&sub)))
		}
		if strings.EqualFold(g.Path, sel) {
			selIx = i
		}
	}
	if selIx < 0 && len(l.shown) > 0 {
		selIx = 0
	}
	if selIx >= 0 {
		l.selectRowIn(idMameList, selIx)
	}
	call(pSendMessageW, lv, wmSetRedraw, 1, 0)
	call(pInvalidateRect, lv, 0, 1)

	var t string
	switch {
	case !loaded && loading:
		t = "Looking for games…"
	case len(c.Folders) == 0:
		t = "Add the folder with your MAME ROM sets (Add folder…), or open a game file (Arcade (MAME) > Open a game file)."
	case len(games) == 0:
		t = "No arcade games in your ROM folders. Use Add folder… if they're somewhere else."
	default:
		t = fmt.Sprintf("%d games", len(games))
		if loading {
			t += " (looking again…)"
		} else if checking {
			t += " (checking which MAME version plays each one…)"
		}
	}
	l.setText(idMameCount, t)
	l.enable(idMamePlay, len(l.shown) > 0 && a.mame.run == nil)
	l.fillWith()
}

// fillWith shows the selected game's version choice.
func (l *mameLibrary) fillWith() {
	cb := l.ctl[idMameWith]
	call(pSendMessageW, cb, cbResetContent, 0, 0)
	c := mame.Load()
	def, hasDef := c.DefaultInstall()
	l.withList = []string{""}
	label := "Default"
	if hasDef {
		label += " (" + def.Label() + ")"
	}
	call(pSendMessageW, cb, cbAddString, 0, uintptr(unsafe.Pointer(u16(label))))
	for _, in := range c.Installs {
		l.withList = append(l.withList, in.Exe)
		call(pSendMessageW, cb, cbAddString, 0, uintptr(unsafe.Pointer(u16(in.Label()))))
	}
	sel := 0
	i := l.selectedIn(idMameList, len(l.shown))
	if i >= 0 {
		if own, ok := c.Games[l.shown[i].Short]; ok {
			for k, exe := range l.withList {
				if strings.EqualFold(exe, own) {
					sel = k
				}
			}
		}
	}
	call(pSendMessageW, cb, cbSetCurSel, uintptr(sel), 0)
	l.enable(idMameWith, i >= 0 && len(c.Installs) > 0)
}

func (l *mameLibrary) selectedIn(id, n int) int {
	i := int(int32(call(pSendMessageW, l.ctl[id], lvmGetNextItem, ^uintptr(0), 2))) // LVNI_SELECTED
	if i >= n {
		return -1
	}
	return i
}

func (l *mameLibrary) selectRowIn(id, i int) {
	lv := l.ctl[id]
	clear := lvItem{StateMask: 3}
	call(pSendMessageW, lv, lvmSetItemState, ^uintptr(0), uintptr(unsafe.Pointer(&clear)))
	it := lvItem{State: 3, StateMask: 3} // selected and focused
	call(pSendMessageW, lv, lvmSetItemState, uintptr(i), uintptr(unsafe.Pointer(&it)))
	call(pSendMessageW, lv, lvmEnsureVisible, uintptr(i), 0)
}

func (l *mameLibrary) play() {
	i := l.selectedIn(idMameList, len(l.shown))
	if i < 0 {
		return
	}
	g := l.shown[i]
	a := l.a
	l.close()
	a.mamePlay(g, "")
}

func (l *mameLibrary) close() {
	a := l.a
	call(pKillTimer, l.hwnd, timerMamePad)
	a.mame.lib = nil
	call(pDestroyWindow, l.hwnd)
	call(pSetForegroundWindow, a.hwnd)
}

func (l *mameLibrary) command(id, code int, lp uintptr) {
	a := l.a
	clicked := code == bnClicked
	switch id {
	case idMamePlay, idOK:
		l.play()
	case idMameClose, 2: // IDCANCEL (Esc)
		l.close()
	case idMameAdd:
		if clicked {
			a.mameAddExe(l.hwnd)
		}
	case idMameFind:
		if clicked {
			a.mameDetect()
			l.showVersions()
		}
	case idMameDefault, idMameRemove:
		if !clicked {
			break
		}
		i := l.selectedIn(idMameVersions, len(l.installs))
		if i < 0 {
			a.showMessage("Pick a MAME version in the list first")
			break
		}
		in := l.installs[i]
		if id == idMameDefault {
			mame.Update(func(c *mame.Config) { c.Default = in.Exe })
			a.showMessage(in.Label() + " is the default now")
		} else {
			if messageBox(l.hwnd, "Remove "+in.Label()+" from RetroPixels? Its files stay where they are.\n\n"+in.Exe,
				"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
				break
			}
			mame.Update(func(c *mame.Config) { c.Remove(in.Exe) })
		}
		l.showVersions()
	case idMameDownload:
		if !clicked {
			break
		}
		cb := l.ctl[idMameRels]
		i := int(int32(call(pSendMessageW, cb, cbGetCurSel, 0, 0)))
		if i < 0 || i >= len(l.rels) {
			a.mameLoadReleases(true)
			l.fillReleases()
			break
		}
		a.mameDownload(l.rels[i])
		l.showDownload()
	case idMameAddFolder:
		if clicked {
			if dir := a.pickFolder(l.hwnd, "Add a folder with MAME ROM sets (.zip)"); dir != "" {
				mame.Update(func(c *mame.Config) { c.AddFolder(dir) })
				l.fillFolders()
				a.mameReload()
				l.fill()
			}
		}
	case idMameRmFolder:
		if clicked {
			c := mame.Load()
			i := int(int32(call(pSendMessageW, l.ctl[idMameFolders], lbGetCurSel, 0, 0)))
			if i < 0 || i >= len(c.Folders) {
				a.showMessage("Pick a folder in the list first")
				break
			}
			dir := c.Folders[i]
			mame.Update(func(c *mame.Config) { c.RemoveFolder(dir) })
			l.fillFolders()
			a.mameReload()
		}
	case idMameEmbed:
		if clicked {
			on := call(pSendMessageW, l.ctl[idMameEmbed], 0x00F0 /* BM_GETCHECK */, 0, 0) == 1
			mame.Update(func(c *mame.Config) { c.Embed = &on })
		}
	case idMameWith:
		if code != cbnSelChange {
			break
		}
		g := l.selectedIn(idMameList, len(l.shown))
		k := int(call(pSendMessageW, lp, cbGetCurSel, 0, 0))
		if g < 0 || k < 0 || k >= len(l.withList) {
			break
		}
		game, exe := l.shown[g], l.withList[k]
		mame.Update(func(c *mame.Config) { c.SetGame(game.Short, exe) })
		l.fill()
	case idMameSearch:
		if code == enChange {
			l.fill()
		}
	}
}

// pollPad: the controller moves through the games (D-pad or left stick),
// A plays, B closes, while this window is in front.
func (l *mameLibrary) pollPad() {
	if call(dpGetForegroundWindow) != l.hwnd {
		l.padPrev = PadState{}
		return
	}
	p := firstPad(l.a.xin.read())
	prev := l.padPrev
	l.padPrev = p
	pressed := func(b int) bool { return p.Buttons[b] && !prev.Buttons[b] }
	if pressed(padA) {
		l.play()
		return
	}
	if pressed(padB) {
		l.close()
		return
	}
	up := p.Buttons[padUp] || p.Axes[1] < -0.6
	down := p.Buttons[padDown] || p.Axes[1] > 0.6
	wasUp := prev.Buttons[padUp] || prev.Axes[1] < -0.6
	wasDown := prev.Buttons[padDown] || prev.Axes[1] > 0.6
	switch {
	case up && !wasUp, down && !wasDown:
		l.padHeld = time.Now()
	case (up || down) && time.Since(l.padHeld) > 400*time.Millisecond:
		l.padHeld = time.Now().Add(-320 * time.Millisecond)
	default:
		return
	}
	step := 1
	if up {
		step = -1
	}
	if n := len(l.shown); n > 0 {
		i := min(n-1, max(0, l.selectedIn(idMameList, n)+step))
		l.selectRowIn(idMameList, i)
		l.fillWith()
	}
}

func mameLibProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.mame.lib == nil || a.mame.lib.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	l := a.mame.lib
	switch uint32(m) {
	case wmSize:
		l.layout()
		return 0
	case wmGetMinMaxInfo:
		mm := *(**minMaxInfo)(unsafe.Pointer(&lp))
		mm.MinTrackSize = point{int32(l.scale(760)), int32(l.scale(600))}
		return 0
	case wmCommand:
		l.command(int(loword(wp)&0xffff), int(hiword(wp)&0xffff), lp)
		return 0
	case wmNotify:
		h := *(**nmHdr)(unsafe.Pointer(&lp))
		switch {
		case h.ID == idMameList && (h.Code == nmDblClk || h.Code == nmReturn):
			l.play()
		case h.ID == idMameList && h.Code == lvnItemChanged:
			l.fillWith()
		}
		return 0
	case wmTimer:
		if wp == timerMamePad {
			l.pollPad()
		}
		return 0
	case wmClose:
		l.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}

const lvnItemChanged = ^uint32(101 - 1) // LVN_ITEMCHANGED (-101)

// mameDialogMessage lets Tab, Enter and Esc work in the library window
// (called from the message loop).
func (a *app) mameDialogMessage(m *msg) bool {
	l := a.mame.lib
	if l == nil {
		return false
	}
	// Typing a name in the game list searches for it.
	if m.Message == 0x0102 /* WM_CHAR */ && m.Hwnd == l.ctl[idMameList] && m.WParam > ' ' {
		s := l.ctl[idMameSearch]
		call(pSetFocus, s)
		call(pSendMessageW, s, 0x00B1 /* EM_SETSEL */, 0x7fffffff, 0x7fffffff)
		m.Hwnd = s
	}
	if m.Hwnd == l.hwnd || call(pGetAncestor, m.Hwnd, 2 /* GA_ROOT */) == l.hwnd {
		return call(pIsDialogMessageW, l.hwnd, uintptr(unsafe.Pointer(m))) != 0
	}
	return false
}
