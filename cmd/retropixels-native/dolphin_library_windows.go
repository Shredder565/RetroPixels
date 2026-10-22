package main

// The GameCube / Wii library window (the app's GameCube and Wii tabs): the
// games in your games folders, with search, a filter by system and sorting;
// the folders themselves; and Dolphin (found, downloaded or chosen).

import (
	"fmt"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
)

const (
	idLibExe      = 1350
	idLibFolders  = 1353
	idLibAdd      = 1354
	idLibRemove   = 1355
	idLibRescan   = 1356
	idLibFilter   = 1357
	idLibSearch   = 1358
	idLibSort     = 1359
	idLibList     = 1360
	idLibPlay     = 1361
	idLibClose    = 1362
	idLibCount    = 1363
	idLibHint     = 1364
	idLibFoldersL = 1365
	timerLibPad   = 1370

	wsThickFrame      = 0x00040000
	wsMinimizeBox     = 0x00020000
	wsMaximizeBox     = 0x00010000
	wsBorder          = 0x00800000
	wsExControlParent = 0x00010000
	bsDefPushButton   = 0x1
	esAutoHScroll     = 0x80
	lbsNotify         = 0x1
	lbsNoIntegral     = 0x100

	wmNotify    = 0x004E
	wmSetRedraw = 0x000B

	lbAddString    = 0x0180
	lbResetContent = 0x0184
	lbGetCurSel    = 0x0188
	cbResetContent = 0x014B
	enChange       = 0x0300
	emSetCueBanner = 0x1501

	lvsReport        = 0x1
	lvsSingleSel     = 0x4
	lvsShowSelAlways = 0x8
	lvmDeleteAll     = 0x1009
	lvmGetNextItem   = 0x100C
	lvmEnsureVisible = 0x1013
	lvmSetColWidth   = 0x101E
	lvmSetItemState  = 0x102B
	lvmSetExStyle    = 0x1036
	lvmInsertItemW   = 0x104D
	lvmInsertColumnW = 0x1061
	lvmSetItemTextW  = 0x1074
	lvmGetItemCount  = 0x1004
	lvnColumnClick   = ^uint32(108 - 1) // LVN_COLUMNCLICK (-108)
	nmDblClk         = ^uint32(3 - 1)   // NM_DBLCLK (-3)
	nmReturn         = ^uint32(4 - 1)   // NM_RETURN (-4)
)

var (
	comctl32                = syscall.NewLazyDLL("comctl32.dll")
	dpInitCommonControlsEx  = comctl32.NewProc("InitCommonControlsEx")
	dpSHBrowseForFolderW    = shell32.NewProc("SHBrowseForFolderW")
	dpSHGetPathFromIDListW  = shell32.NewProc("SHGetPathFromIDListW")
	dpCoTaskMemFree         = syscall.NewLazyDLL("ole32.dll").NewProc("CoTaskMemFree")
	dpCoInitializeEx        = syscall.NewLazyDLL("ole32.dll").NewProc("CoInitializeEx")
	dpIsWindowVisible       = user32.NewProc("IsWindowVisible")
	dolLibClassDone         bool
	dolLibCallback          = syscall.NewCallback(dolLibProc)
	dolCommonControlsLoaded bool
	dolComReady             int // 0 untried, 1 ready, -1 failed
)

type lvColumn struct {
	Mask      uint32
	Fmt       int32
	Cx        int32
	Text      *uint16
	TextMax   int32
	SubItem   int32
	Image     int32
	Order     int32
	CxMin     int32
	CxDefault int32
	CxIdeal   int32
}

type lvItem struct {
	Mask      uint32
	Item      int32
	SubItem   int32
	State     uint32
	StateMask uint32
	Text      *uint16
	TextMax   int32
	Image     int32
	Param     uintptr
	Indent    int32
	GroupID   int32
	Columns   uint32
	ColFmt    *int32
	Group     int32
}

type nmHdr struct {
	From uintptr
	ID   uintptr
	Code uint32
}

type dolLibrary struct {
	a        *app
	hwnd     uintptr
	ctl      map[int]uintptr
	shown    []dolphin.Game // in the list, in order
	padPrev  PadState
	padHeld  time.Time // when up / down started being held (for repeat)
	filterIx int
	sortIx   int
}

func (a *app) openDolLibrary() {
	if l := a.dol.lib; l != nil {
		call(pShowWindow, l.hwnd, swShow)
		call(pSetForegroundWindow, l.hwnd)
		return
	}
	if !dolCommonControlsLoaded {
		icc := struct{ Size, ICC uint32 }{8, 0x1 | 0x4000} // ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES
		call(dpInitCommonControlsEx, uintptr(unsafe.Pointer(&icc)))
		dolCommonControlsLoaded = true
	}
	cls := u16("RetroPixelsNativeGCLibrary")
	if !dolLibClassDone {
		wc := wndClassEx{WndProc: dolLibCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls} // COLOR_BTNFACE + 1
		wc.Icon = call(pLoadImageW, a.hinst, 1, 1, 0, 0, 0x40|0x8000)
		wc.IconSm = call(pLoadImageW, a.hinst, 1, 1, 16, 16, 0x8000)
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		dolLibClassDone = true
	}
	l := &dolLibrary{a: a, ctl: map[int]uintptr{}}
	for i, f := range libFilters {
		if f.id == a.settings.Dolphin.Filter {
			l.filterIx = i
		}
	}
	for i, s := range libSorts {
		if s.id == a.settings.Dolphin.Sort {
			l.sortIx = i
		}
	}
	a.dol.lib = l
	dpi := a.dpi()
	w, h := 860*dpi/96, 620*dpi/96
	wr := windowRect(a.hwnd)
	x := int(wr.Left) + (int(wr.Right-wr.Left)-w)/2
	y := int(wr.Top) + (int(wr.Bottom-wr.Top)-h)/2
	if x < 0 {
		x = 0
	}
	if y < 0 {
		y = 0
	}
	l.hwnd = call(pCreateWindowExW, wsExControlParent, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("GameCube / Wii library"))),
		wsOverlappedWindow|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), a.hwnd, 0, a.hinst, 0)
	l.build()
	call(pShowWindow, l.hwnd, swShow)
	call(pSetTimer, l.hwnd, timerLibPad, 50, 0)
	l.showStatus()
	a.dol.mu.Lock()
	loaded := a.dol.libLoaded
	a.dol.mu.Unlock()
	if loaded {
		l.fill()
	}
	a.dolReloadLibrary() // the folders may have changed since
	call(pSetFocus, l.ctl[idLibList])
}

func (l *dolLibrary) scale(v int) int { return v * l.a.dpi() / 96 }

func (l *dolLibrary) child(class, text string, style uint32, exStyle uint32, id int) uintptr {
	c := call(pCreateWindowExW, uintptr(exStyle), uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), 0, 0, 10, 10, l.hwnd, uintptr(id), l.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, l.a.font, 1)
	l.ctl[id] = c
	return c
}

func (l *dolLibrary) build() {
	l.child("STATIC", "", ssNoPrefix, 0, idLibExe)
	l.child("STATIC", "Games folders:", ssNoPrefix, 0, idLibFoldersL)
	l.child("LISTBOX", "", lbsNotify|lbsNoIntegral|wsVScroll|wsBorder|wsTabStop, 0, idLibFolders)
	l.child("BUTTON", "Add folder…", bsPushButton|wsTabStop, 0, idLibAdd)
	l.child("BUTTON", "Remove", bsPushButton|wsTabStop, 0, idLibRemove)
	l.child("BUTTON", "Find games again", bsPushButton|wsTabStop, 0, idLibRescan)
	f := l.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, idLibFilter)
	for _, x := range libFilters {
		call(pSendMessageW, f, cbAddString, 0, uintptr(unsafe.Pointer(u16(x.label))))
	}
	call(pSendMessageW, f, cbSetCurSel, uintptr(l.filterIx), 0)
	s := l.child("EDIT", "", esAutoHScroll|wsTabStop, 0x200 /* WS_EX_CLIENTEDGE */, idLibSearch)
	call(pSendMessageW, s, emSetCueBanner, 1, uintptr(unsafe.Pointer(u16("Search games…"))))
	so := l.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, idLibSort)
	for _, x := range libSorts {
		call(pSendMessageW, so, cbAddString, 0, uintptr(unsafe.Pointer(u16(x.label))))
	}
	call(pSendMessageW, so, cbSetCurSel, uintptr(l.sortIx), 0)
	l.child("STATIC", "", ssNoPrefix, 0, idLibCount)
	lv := l.child("SysListView32", "", lvsReport|lvsSingleSel|lvsShowSelAlways|wsTabStop, 0x200, idLibList)
	call(pSendMessageW, lv, lvmSetExStyle, 0x20|0x10000, 0x20|0x10000) // full-row select, double-buffered
	for i, c := range []struct {
		name string
		w    int
	}{{"Game", 330}, {"System", 90}, {"ID", 80}, {"Size", 80}, {"Last played", 110}} {
		col := lvColumn{Mask: 1 | 2 | 4 | 8, Cx: int32(l.scale(c.w)), Text: u16(c.name), SubItem: int32(i)}
		if i == 3 {
			col.Fmt = 1 // LVCFMT_RIGHT
		}
		call(pSendMessageW, lv, lvmInsertColumnW, uintptr(i), uintptr(unsafe.Pointer(&col)))
	}
	l.child("STATIC", "Double-click a game, or press Enter (or A on the controller), to play it here. "+
		"Save states are in PixelU: Shift+F1 saves, F1 loads. Esc closes this window.", ssNoPrefix, 0, idLibHint)
	l.child("BUTTON", "Play", bsDefPushButton|wsTabStop, 0, idLibPlay)
	l.child("BUTTON", "Close", bsPushButton|wsTabStop, 0, idLibClose)
	l.fillFolders()
	l.layout()
}

// layout places everything for the window's size.
func (l *dolLibrary) layout() {
	cr := clientRect(l.hwnd)
	W, H := int(cr.Right), int(cr.Bottom)
	m, gap := l.scale(12), l.scale(8)
	bw, bh := l.scale(130), l.scale(26)
	put := func(id, x, y, w, h int) {
		if w < 1 {
			w = 1
		}
		if h < 1 {
			h = 1
		}
		call(pSetWindowPos, l.ctl[id], 0, uintptr(x), uintptr(y), uintptr(w), uintptr(h), swpNoZOrder|swpNoActivate)
	}
	y := m
	right := W - m - bw
	put(idLibExe, m, y, W-2*m, l.scale(42))
	y += l.scale(48)
	put(idLibFoldersL, m, y, l.scale(200), l.scale(18))
	y += l.scale(20)
	fh := 3*bh + 2*l.scale(4)
	put(idLibFolders, m, y, right-m-gap, fh)
	put(idLibAdd, right, y, bw, bh)
	put(idLibRemove, right, y+bh+l.scale(4), bw, bh)
	put(idLibRescan, right, y+2*(bh+l.scale(4)), bw, bh)
	y += fh + gap + l.scale(4)
	put(idLibFilter, m, y, l.scale(150), l.scale(200))
	put(idLibSearch, m+l.scale(160), y, l.scale(240), l.scale(24))
	put(idLibSort, m+l.scale(410), y, l.scale(160), l.scale(200))
	put(idLibCount, m+l.scale(580), y+l.scale(4), W-m-(m+l.scale(580)), l.scale(20))
	y += l.scale(32)
	bottom := H - m - bh
	hintH := l.scale(36)
	put(idLibList, m, y, W-2*m, bottom-gap-hintH-y)
	put(idLibHint, m, bottom-hintH-l.scale(2), W-2*m, hintH)
	put(idLibPlay, W-m-2*l.scale(100)-gap, bottom, l.scale(100), bh)
	put(idLibClose, W-m-l.scale(100), bottom, l.scale(100), bh)
}

// showStatus: the engine's line at the top.
func (l *dolLibrary) showStatus() {
	exe := l.a.dolExe()
	t := "Games play in PixelU (Wii/GameCube engine based on Dolphin, GPL).\n" + noEngine
	if exe != "" {
		t = "Games play in PixelU (Wii/GameCube engine based on Dolphin, GPL)"
		if v := dolphin.Version(exe); v != "" {
			t += " · " + v
		}
		t += "\n" + exe
	}
	setWindowText(l.ctl[idLibExe], t)
}

func (l *dolLibrary) fillFolders() {
	lb := l.ctl[idLibFolders]
	call(pSendMessageW, lb, lbResetContent, 0, 0)
	for _, f := range l.a.settings.Dolphin.Folders {
		call(pSendMessageW, lb, lbAddString, 0, uintptr(unsafe.Pointer(u16(f))))
	}
	call(pEnableWindow, l.ctl[idLibRemove], boolArg(len(l.a.settings.Dolphin.Folders) > 0))
}

// fill shows the games (after a search, a filter or a new scan).
func (l *dolLibrary) fill() {
	a := l.a
	a.dol.mu.Lock()
	games, loaded, loading := a.dol.libGames, a.dol.libLoaded, a.dol.libLoading
	a.dol.mu.Unlock()
	counts := map[string]int{}
	for _, g := range games {
		counts[g.Platform]++
	}
	// the filter list shows how many of each
	f := l.ctl[idLibFilter]
	call(pSendMessageW, f, cbResetContent, 0, 0)
	for _, x := range libFilters {
		n := len(games)
		if x.id != "all" {
			n = counts[x.id]
		}
		call(pSendMessageW, f, cbAddString, 0, uintptr(unsafe.Pointer(u16(fmt.Sprintf("%s (%d)", x.label, n)))))
	}
	call(pSendMessageW, f, cbSetCurSel, uintptr(l.filterIx), 0)

	query := windowString(l.ctl[idLibSearch])
	sel := ""
	if i := l.selected(); i >= 0 {
		sel = l.shown[i].Path
	}
	l.shown = filterGames(games, libFilters[l.filterIx].id, query, libSorts[l.sortIx].id)
	lv := l.ctl[idLibList]
	call(pSendMessageW, lv, wmSetRedraw, 0, 0)
	call(pSendMessageW, lv, lvmDeleteAll, 0, 0)
	now := time.Now()
	selIx := -1
	for i, g := range l.shown {
		it := lvItem{Mask: 1, Item: int32(i), Text: u16(g.Name)}
		call(pSendMessageW, lv, lvmInsertItemW, 0, uintptr(unsafe.Pointer(&it)))
		for col, text := range []string{g.Platform, g.ID, fmtSize(g.Size), fmtPlayed(g.Played, now)} {
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
		l.selectRow(selIx)
	}
	call(pSendMessageW, lv, wmSetRedraw, 1, 0)
	call(pInvalidateRect, lv, 0, 1)

	var c string
	switch {
	case !loaded && loading:
		c = "Looking for games…"
	case len(a.settings.Dolphin.Folders) == 0:
		c = "Add the folder with your GameCube and Wii games (Add folder…)."
	case len(games) == 0:
		c = "No GameCube or Wii games in your games folders. Use Add folder… if they're somewhere else."
	default:
		c = fmt.Sprintf("%d GameCube · %d Wii · %d WiiWare", counts["GameCube"], counts["Wii"], counts["WiiWare"])
		if loading {
			c += " (looking again…)"
		}
	}
	setWindowText(l.ctl[idLibCount], c)
	call(pEnableWindow, l.ctl[idLibPlay], boolArg(len(l.shown) > 0))
}

func (l *dolLibrary) selected() int {
	i := int(int32(call(pSendMessageW, l.ctl[idLibList], lvmGetNextItem, ^uintptr(0), 2))) // LVNI_SELECTED
	if i >= len(l.shown) {
		return -1
	}
	return i
}

func (l *dolLibrary) selectRow(i int) {
	lv := l.ctl[idLibList]
	clear := lvItem{StateMask: 3}
	call(pSendMessageW, lv, lvmSetItemState, ^uintptr(0), uintptr(unsafe.Pointer(&clear)))
	it := lvItem{State: 3, StateMask: 3} // selected and focused
	call(pSendMessageW, lv, lvmSetItemState, uintptr(i), uintptr(unsafe.Pointer(&it)))
	call(pSendMessageW, lv, lvmEnsureVisible, uintptr(i), 0)
}

// markPlayed updates a game's "last played" in the list.
func (l *dolLibrary) markPlayed(path string) {
	a := l.a
	a.dol.mu.Lock()
	for i := range a.dol.libGames {
		if strings.EqualFold(a.dol.libGames[i].Path, path) {
			a.dol.libGames[i].Played = time.Now()
		}
	}
	a.dol.mu.Unlock()
}

func (l *dolLibrary) play() {
	i := l.selected()
	if i < 0 {
		return
	}
	g := l.shown[i]
	a := l.a
	l.close()
	a.dolPlay(g)
}

func (l *dolLibrary) close() {
	a := l.a
	call(pKillTimer, l.hwnd, timerLibPad)
	a.dol.lib = nil
	call(pDestroyWindow, l.hwnd)
	call(pSetForegroundWindow, a.hwnd)
}

func (l *dolLibrary) addFolder() {
	a := l.a
	dir := a.pickFolder(l.hwnd, "Add a folder with GameCube or Wii games")
	if dir == "" {
		return
	}
	if a.settings.Dolphin.addFolder(dir) {
		saveSettings(a.settings)
		l.fillFolders()
		a.dolReloadLibrary()
		l.fill()
	}
}

// pollPad: the controller moves through the list (D-pad or left stick) and
// A plays, B closes, while this window is in front.
func (l *dolLibrary) pollPad() {
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
	step := 0
	switch {
	case up && !wasUp, down && !wasDown:
		l.padHeld = time.Now()
	case (up || down) && time.Since(l.padHeld) > 400*time.Millisecond:
		l.padHeld = time.Now().Add(-320 * time.Millisecond) // repeat about 12 a second
	default:
		return
	}
	if up {
		step = -1
	} else if down {
		step = 1
	}
	if n := len(l.shown); n > 0 && step != 0 {
		i := l.selected() + step
		if i < 0 {
			i = 0
		}
		if i >= n {
			i = n - 1
		}
		l.selectRow(i)
	}
}

func dolLibProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.dol.lib == nil || a.dol.lib.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	l := a.dol.lib
	switch uint32(m) {
	case wmSize:
		l.layout()
		return 0
	case wmGetMinMaxInfo:
		mm := *(**minMaxInfo)(unsafe.Pointer(&lp))
		mm.MinTrackSize = point{int32(l.scale(720)), int32(l.scale(460))}
		return 0
	case wmCommand:
		id, code := loword(wp)&0xffff, hiword(wp)&0xffff
		switch id {
		case idLibPlay, idOK:
			l.play()
		case idLibClose, 2: // IDCANCEL (Esc)
			l.close()
		case idLibAdd:
			if code == bnClicked {
				l.addFolder()
			}
		case idLibRemove:
			if code == bnClicked {
				i := int(int32(call(pSendMessageW, l.ctl[idLibFolders], lbGetCurSel, 0, 0)))
				if i < 0 || i >= len(a.settings.Dolphin.Folders) {
					a.showMessage("Pick a folder in the list first")
					break
				}
				a.settings.Dolphin.removeFolder(a.settings.Dolphin.Folders[i])
				saveSettings(a.settings)
				l.fillFolders()
				a.dolReloadLibrary()
			}
		case idLibRescan:
			if code == bnClicked {
				// Find games again: near the Dolphin used before, else near
				// PixelU's engine, as RetroPixels.exe's "Find games again".
				exe := a.settings.Dolphin.oldExe()
				if exe == "" {
					exe = a.dolExe()
				}
				if exe == "" {
					a.showMessage("Use Add folder… to add your games folder")
					break
				}
				setWindowText(l.ctl[idLibCount], "Looking for games…")
				for _, f := range dolphin.GuessFolders(exe) {
					a.settings.Dolphin.addFolder(f)
				}
				saveSettings(a.settings)
				l.fillFolders()
				a.dolReloadLibrary()
			}
		case idLibFilter:
			if code == cbnSelChange {
				if i := int(call(pSendMessageW, lp, cbGetCurSel, 0, 0)); i >= 0 && i < len(libFilters) {
					l.filterIx = i
					a.settings.Dolphin.Filter = libFilters[i].id
					saveSettings(a.settings)
					l.fill()
				}
			}
		case idLibSort:
			if code == cbnSelChange {
				if i := int(call(pSendMessageW, lp, cbGetCurSel, 0, 0)); i >= 0 && i < len(libSorts) {
					l.sortIx = i
					a.settings.Dolphin.Sort = libSorts[i].id
					saveSettings(a.settings)
					l.fill()
				}
			}
		case idLibSearch:
			if code == enChange {
				l.fill()
			}
		}
		return 0
	case wmNotify:
		h := *(**nmHdr)(unsafe.Pointer(&lp))
		if h.ID == idLibList && (h.Code == nmDblClk || h.Code == nmReturn) {
			l.play()
		}
		return 0
	case wmTimer:
		if wp == timerLibPad {
			l.pollPad()
		}
		return 0
	case wmClose:
		l.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}

// dolDialogMessage lets Tab, Enter and Esc work in the library window
// (called from the message loop).
func (a *app) dolDialogMessage(m *msg) bool {
	// Typing a name in the game list searches for it.
	if l := a.dol.lib; l != nil && m.Message == 0x0102 /* WM_CHAR */ && m.Hwnd == l.ctl[idLibList] && m.WParam > ' ' {
		s := l.ctl[idLibSearch]
		call(pSetFocus, s)
		call(pSendMessageW, s, 0x00B1 /* EM_SETSEL */, 0x7fffffff, 0x7fffffff)
		m.Hwnd = s
	}
	for _, h := range []uintptr{a.dolLibHwnd(), a.dolSettingsHwnd()} {
		if h != 0 && (m.Hwnd == h || call(pGetAncestor, m.Hwnd, 2 /* GA_ROOT */) == h) {
			return call(pIsDialogMessageW, h, uintptr(unsafe.Pointer(m))) != 0
		}
	}
	return false
}

func (a *app) dolLibHwnd() uintptr {
	if a.dol.lib != nil {
		return a.dol.lib.hwnd
	}
	return 0
}

// ---------------- choosing a folder ----------------

type dolBrowseInfo struct {
	Owner       uintptr
	Root        uintptr
	DisplayName *uint16
	Title       *uint16
	Flags       uint32
	Fn          uintptr
	Param       uintptr
	Image       int32
}

// pickFolder asks for a folder ("" = cancelled).
func (a *app) pickFolder(owner uintptr, title string) string {
	if dolComReady == 0 {
		dolComReady = -1
		if hr := call(dpCoInitializeEx, 0, 2 /* COINIT_APARTMENTTHREADED */); int32(hr) >= 0 {
			dolComReady = 1
		}
	}
	name := make([]uint16, 260)
	bi := dolBrowseInfo{Owner: owner, DisplayName: &name[0], Title: u16(title), Flags: 0x1 /* BIF_RETURNONLYFSDIRS */}
	if dolComReady == 1 {
		bi.Flags |= 0x40 // BIF_NEWDIALOGSTYLE (resizable, with an edit box)
	}
	pidl := call(dpSHBrowseForFolderW, uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return ""
	}
	defer call(dpCoTaskMemFree, pidl)
	buf := make([]uint16, 32768)
	if call(dpSHGetPathFromIDListW, pidl, uintptr(unsafe.Pointer(&buf[0]))) == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}
