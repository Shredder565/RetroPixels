package main

// The start screen, painted when no game is running (the web page's
// launcher): the drop target with an Open button, the recent games (click,
// or arrow keys / the controller's D-pad and A), and a controller check that
// lights up the buttons being pressed. It is never in a recording: those
// hold only the game's picture.

import (
	"fmt"
	"os"
	"strings"
	"time"
	"unsafe"

	"retropixels/internal/apatch"
)

// colours (COLORREF is 0x00BBGGRR): the web page's palette
const (
	colBg      = 0x130f0e // #0e0f13
	colPanel   = 0x201917 // #171920
	colPanel2  = 0x2b221f // #1f222b
	colLine    = 0x3b302c // #2c303b
	colText    = 0xeee9e8 // #e8e9ee
	colMuted   = 0xa0908b // #8b90a0
	colAccent  = 0x107c10 // #107c10
	colAccent2 = 0x3dd13d // #3dd13d
	colSelBg   = 0x1a2415 // #15241a
	colHover   = 0x584a44 // #444a58
	colWarn    = 0x3aa7e0 // #e0a73a

	timerHome = 11
)

const (
	hitOpen = iota
	hitRecent
)

type homeHit struct {
	r    winRect
	kind int
	idx  int
}

// padChips: the controller check's buttons, in the browser's numbering.
var padChips = []struct {
	label string
	b     int
}{{"A", padA}, {"B", padB}, {"X", padX}, {"Y", padY}, {"LB", padLB}, {"RB", padRB}, {"LT", padLT}, {"RT", padRT},
	{"View", padView}, {"Menu", padMenu}, {"L3", padL3}, {"R3", padR3}, {"↑", padUp}, {"↓", padDown}, {"←", padLeft}, {"→", padRight}}

// homePad: the first connected controller, with the left stick counted as
// the D-pad (as the web page's controller picture).
func homePad(p [4]PadState) (PadState, int) {
	n := 0
	var first PadState
	for _, x := range p {
		if x.Connected {
			if n == 0 {
				first = x
			}
			n++
		}
	}
	if n > 0 {
		ax := first.Axes
		first.Buttons[padUp] = first.Buttons[padUp] || ax[1] < -0.5
		first.Buttons[padDown] = first.Buttons[padDown] || ax[1] > 0.5
		first.Buttons[padLeft] = first.Buttons[padLeft] || ax[0] < -0.5
		first.Buttons[padRight] = first.Buttons[padRight] || ax[0] > 0.5
		first.Axes = [4]float32{}
	}
	return first, n
}

func (a *app) sc(v int) int32 { return int32(v * a.dpi() / 96) }

// drawHome paints the start screen (double-buffered: no flicker): the
// header, the tabs and the open tab's page (ui_tabs_windows.go), which
// scrolls under the tabs.
func (a *app) drawHome(hdc uintptr) {
	u := a.ui
	cr := clientRect(a.hwnd)
	W, H := cr.Right, cr.Bottom
	if W <= 0 || H <= 0 {
		return
	}
	dc := call(pCreateCompatibleDC, hdc)
	bmp := call(pCreateCompatibleBitmap, hdc, uintptr(W), uintptr(H))
	oldBmp := call(pSelectObject, dc, bmp)
	defer func() {
		call(pBitBlt, hdc, 0, 0, uintptr(W), uintptr(H), dc, 0, 0, srcCopy)
		call(pSelectObject, dc, oldBmp)
		call(pDeleteObject, bmp)
		call(pDeleteDC, dc)
	}()
	call(pSetBkMode, dc, transparent)
	fillColor(dc, cr, colBg)
	u.hits = u.hits[:0]
	s := a.sc
	l := a.libs()

	headH := a.drawHeader(dc, W) // (painted again on top at the end)
	m := s(16)
	if W < s(520) {
		m = s(10)
	}
	x0 := max(m, (W-s(1100))/2)
	x1 := W - x0
	tabTop := headH + s(16)
	contentTop := tabTop + s(44) + s(20)

	t := u.tab
	if maxScroll := max(0, l.contentH[t]-(H-contentTop)); l.scroll[t] > maxScroll {
		l.scroll[t] = maxScroll
	}
	if l.scroll[t] < 0 {
		l.scroll[t] = 0
	}
	l.viewTop, l.viewBottom = contentTop, H
	y := contentTop - l.scroll[t]
	var end int32
	switch t {
	case tabClassic:
		end = a.drawClassic(dc, x0, x1, y, H)
	case tabRecordings:
		end = a.panelled(dc, x0, x1, y, func(d uintptr) int32 { return a.drawRecordings(d, x0, x1, y) })
	default:
		end = a.panelled(dc, x0, x1, y, func(d uintptr) int32 { return a.drawLibrary(d, t, x0, x1, y) })
	}
	l.contentH[t] = end - y

	// the selected tile kept in view (arrow keys, D-pad)
	if a.scrollToTile {
		a.scrollToTile = false
		for _, h := range u.hits {
			if h.kind == hitTile && h.idx == l.tileSel[t] {
				if h.r.Top < contentTop {
					l.scroll[t] -= contentTop - h.r.Top + s(8)
					call(pInvalidateRect, a.hwnd, 0, 0)
				} else if h.r.Bottom > H {
					l.scroll[t] += h.r.Bottom - H + s(8)
					call(pInvalidateRect, a.hwnd, 0, 0)
				}
			}
		}
	}
	// what scrolled under the tabs can't be clicked
	kept := u.hits[:0]
	for _, h := range u.hits {
		if h.r.Bottom <= contentTop || h.r.Top >= H {
			continue
		}
		h.r.Top = max(h.r.Top, contentTop)
		kept = append(kept, h)
	}
	u.hits = kept

	// the header and the tabs on top
	fillColor(dc, winRect{0, 0, W, contentTop}, colBg)
	a.drawHeader(dc, W)
	a.drawTabs(dc, x0, x1, tabTop)

	// a scroll bar when the page is taller than the window
	if viewH := H - contentTop; l.contentH[t] > viewH && viewH > 0 {
		bh := max(s(30), viewH*viewH/l.contentH[t])
		by := contentTop + (viewH-bh)*l.scroll[t]/max(1, l.contentH[t]-viewH)
		roundBox(dc, winRect{W - s(8), by, W - s(3), by + bh}, s(5), colHover, -1, 0)
	}
	u.prevHits = append(u.prevHits[:0], u.hits...)
}

// dashedBorder: the drop target's dashed edge, drawn inside r.
func dashedBorder(dc uintptr, r winRect, dash, gap, w int32, color uint32) {
	inset := r.Bottom - r.Top
	if inset > 0 {
		inset = 6 * w
	}
	for x := r.Left + inset; x < r.Right-inset; x += dash + gap {
		e := min(x+dash, r.Right-inset)
		fillColor(dc, winRect{x, r.Top, e, r.Top + w}, color)
		fillColor(dc, winRect{x, r.Bottom - w, e, r.Bottom}, color)
	}
	for y := r.Top + inset; y < r.Bottom-inset; y += dash + gap {
		e := min(y+dash, r.Bottom-inset)
		fillColor(dc, winRect{r.Left, y, r.Left + w, e}, color)
		fillColor(dc, winRect{r.Right - w, y, r.Right, e}, color)
	}
}

// recentLabel: the system a recent game is for, from its extension
// ("NES", "Arcade or zipped game"...).
func recentLabel(path string) string {
	x := extOf(path)
	if apatch.IsPatchName(path) {
		return "Archipelago"
	}
	if id := systemForExt(x); id != "" {
		return systemByID(id).Label
	}
	switch x {
	case "zip", "7z":
		return "." + x
	case "bin":
		return ".bin"
	}
	return strings.ToUpper(x)
}

// uiBackHome: after File > Close game, the window goes back to the size it
// starts at (a 4:3 picture at the chosen window size), so the start screen
// has room.
func (a *app) uiBackHome() {
	a.frameMu.Lock()
	a.fh, a.aspect = 240, 4.0/3
	a.frameMu.Unlock()
	a.resizeToScale()
}

// ---------------- the start screen's mouse, keys and controller ----------------

func (a *app) homeHitAt(x, y int) int {
	for i, h := range a.ui.prevHits {
		if h.r.contains(x, y) {
			return i
		}
	}
	return -1
}

func (a *app) homeMouseMove(x, y int) {
	if i := a.homeHitAt(x, y); i != a.ui.hover {
		a.ui.hover = i
		if !a.mouseTracking {
			t := trackMouseEvent{Flags: 2, Track: a.hwnd} // TME_LEAVE
			t.Size = uint32(unsafe.Sizeof(t))
			call(pTrackMouseEvent, uintptr(unsafe.Pointer(&t)))
			a.mouseTracking = true
		}
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
}

func (a *app) homeMouseLeave() {
	if a.ui.hover >= 0 {
		a.ui.hover = -1
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
}

// homeCursor: the hand over things that can be clicked.
func (a *app) homeCursor() bool {
	if a.ui.hover >= 0 {
		call(pSetCursor, call(pLoadCursorW, 0, idcHand))
		return true
	}
	return false
}

func (a *app) homeClick(x, y int) {
	i := a.homeHitAt(x, y)
	if i < 0 {
		return
	}
	h := a.ui.prevHits[i]
	switch h.kind {
	case hitOpen:
		a.command(cmdOpen)
	case hitRecent:
		a.openRecent(h.idx)
	case hitTab:
		a.setTab(h.idx)
	case hitSys:
		a.sysCard(h.idx)
	case hitTile:
		l := a.libs()
		if h.idx < len(l.shown) {
			l.tileSel[a.ui.tab] = h.idx
			l.shown[h.idx].play()
		}
	case hitAction:
		a.homeAction(h.idx)
	case hitRecording:
		if l := a.libs(); h.idx < len(l.recs) {
			shellOpen(l.recs[h.idx].path)
		}
	}
}

// openRecent opens a recent game, offering to take it off the list when the
// file has gone.
func (a *app) openRecent(i int) {
	if i < 0 || i >= len(a.settings.Recent) {
		return
	}
	p := a.settings.Recent[i]
	if _, err := os.Stat(p); err != nil {
		if messageBox(a.hwnd, fmt.Sprintf("%q isn't there any more (it may have been moved or deleted).\n\nTake it off the recent games list?", p),
			"RetroPixels", mbYesNo|mbIconWarning) == idYes {
			a.settings.Recent = append(a.settings.Recent[:i:i], a.settings.Recent[i+1:]...)
			saveSettings(a.settings)
			a.ui.sel = min(a.ui.sel, len(a.settings.Recent)-1)
			call(pInvalidateRect, a.hwnd, 0, 0)
		}
		return
	}
	a.openPath(p)
	if a.sess != nil {
		a.ui.sel = 0 // (the game is first in the list now)
	}
}

// homeKey: arrow keys pick a recent game, Enter opens it.
func (a *app) homeKey(vk int) bool {
	if vk == 0x09 && keyDown(0x11) { // Ctrl+Tab, Ctrl+Shift+Tab: the next or previous tab
		d := 1
		if keyDown(0x10) {
			d = numTabs - 1
		}
		a.setTab((a.ui.tab + d) % numTabs)
		return true
	}
	switch vk {
	case 0x21, 0x22: // Page Up / Down
		d := 600
		if vk == 0x22 {
			d = -600
		}
		a.homeWheel(d)
		return true
	}
	if a.ui.tab != tabClassic {
		return a.libKey(vk)
	}
	n := len(a.settings.Recent)
	switch vk {
	case 0x26: // up
		a.moveSel(-1)
	case 0x28: // down
		a.moveSel(1)
	case 0x24: // Home
		if n > 0 {
			a.ui.sel = 0
		}
	case 0x23: // End
		if n > 0 {
			a.ui.sel = n - 1
		}
	case vkReturn:
		if a.ui.sel >= 0 {
			a.openRecent(a.ui.sel)
		} else {
			a.command(cmdOpen)
		}
		return true
	default:
		return false
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
	return true
}

func (a *app) moveSel(d int) {
	n := len(a.settings.Recent)
	if n == 0 {
		return
	}
	if a.ui.sel < 0 {
		a.ui.sel = 0
		return
	}
	a.ui.sel = max(0, min(n-1, a.ui.sel+d))
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// homeTick: the controller, 20 times a second while no game runs (the
// controller check, and the D-pad and A to pick and start a recent game).
// A game in an engine (GameCube / Wii, Wii U, arcade) counts as running too:
// its buttons are the game's, not the home screen's.
func (a *app) homeTick() {
	u := a.ui
	if a.sess != nil || a.engineRunning() {
		u.homeActive = false
		return
	}
	p := a.xin.read()
	if !u.homeActive { // just back from a game: a button still held from it does nothing
		u.homeActive = true
		f, _ := homePad(p)
		u.padA = f.Buttons[padA]
		u.padLB, u.padRB = f.Buttons[padLB], f.Buttons[padRB]
		u.navDir = 0
		if f.Buttons[padUp] {
			u.navDir = 0x26
		} else if f.Buttons[padDown] {
			u.navDir = 0x28
		}
		u.navAt = time.Now().Add(time.Hour)
	}
	old, _ := homePad(u.pads)
	now, n := homePad(p)
	_, oldN := homePad(u.pads)
	changed := n != oldN || now.Buttons != old.Buttons
	u.pads = p
	if changed {
		call(pInvalidateRect, a.hwnd, 0, 0)
	}
	if n == 0 || a.controls != nil || u.states != nil || call(pGetForegroundWindow) != a.hwnd {
		u.navDir, u.padA = 0, now.Buttons[padA]
		return
	}
	// LB / RB: the previous or next tab
	if now.Buttons[padLB] && !u.padLB {
		a.setTab((u.tab + numTabs - 1) % numTabs)
	}
	if now.Buttons[padRB] && !u.padRB {
		a.setTab((u.tab + 1) % numTabs)
	}
	u.padLB, u.padRB = now.Buttons[padLB], now.Buttons[padRB]
	dir := 0 // the arrow key the D-pad stands for
	switch {
	case now.Buttons[padUp]:
		dir = 0x26
	case now.Buttons[padDown]:
		dir = 0x28
	case now.Buttons[padLeft] && u.tab != tabClassic:
		dir = 0x25
	case now.Buttons[padRight] && u.tab != tabClassic:
		dir = 0x27
	}
	nav := func() {
		if u.tab == tabClassic {
			a.moveSel(map[int]int{0x26: -1, 0x28: 1}[dir])
		} else {
			a.libKey(dir)
		}
	}
	t := time.Now()
	if dir != 0 {
		if dir != u.navDir {
			nav()
			u.navAt = t.Add(250 * time.Millisecond)
		} else if t.After(u.navAt) {
			nav()
			u.navAt = t.Add(110 * time.Millisecond)
		}
	}
	u.navDir = dir
	if now.Buttons[padA] && !u.padA {
		u.padA = true
		if u.tab != tabClassic {
			a.libKey(vkReturn)
		} else if u.sel >= 0 {
			a.openRecent(u.sel)
		}
		return
	}
	u.padA = now.Buttons[padA]
}

// engineRunning: a game is playing in PixelU, PixelU-Wii or MAME.
func (a *app) engineRunning() bool {
	return a.dol.run != nil || a.pu.run != nil || a.mame.run != nil
}
