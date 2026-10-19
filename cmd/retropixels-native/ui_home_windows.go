package main

// The start screen, painted when no game is running (the web page's
// launcher): the drop target with an Open button, the recent games (click,
// or arrow keys / the controller's D-pad and A), and a controller check that
// lights up the buttons being pressed. It is never in a recording: those
// hold only the game's picture.

import (
	"fmt"
	"os"
	"path/filepath"
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
	colLine    = 0x4c3f3a // #3a3f4c
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

// drawHome paints the start screen (double-buffered: no flicker).
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

	m := s(24)
	if W < s(520) {
		m = s(12)
	}
	x0, x1 := m, W-m
	y := m

	// header: RETROPIXELS NATIVE, the version, and the controller
	hh := s(30)
	x := x0
	for _, part := range []struct {
		t string
		c uint32
	}{{"RETRO", colText}, {"PIXELS", colAccent2}, {" NATIVE", colMuted}} {
		w := textWidth(dc, a.bigFont, part.t)
		paintText(dc, a.bigFont, part.t, winRect{x, y, x + w + 2, y + hh}, part.c, dtSingleLine|dtVCenter)
		x += w
	}
	v := "  v" + version
	paintText(dc, u.small, v, winRect{x, y + s(3), x + textWidth(dc, u.small, v) + 2, y + hh}, colMuted, dtSingleLine|dtVCenter)
	x += textWidth(dc, u.small, v)

	pad, npads := homePad(u.pads)
	status := "No controller - press any button on your Xbox controller"
	if npads == 1 {
		status = "Xbox controller connected"
	} else if npads > 1 {
		status = fmt.Sprintf("%d controllers connected", npads)
	}
	pw := textWidth(dc, u.small, status) + s(40)
	py := y
	if x1-pw < x+s(16) { // no room beside the title: under it
		py = y + hh + s(8)
		y += hh + s(8)
	}
	px := x1 - pw
	if px < x0 {
		px = x0
	}
	pill := winRect{px, py + s(2), x1, py + hh - s(2)}
	roundBox(dc, pill, pill.Bottom-pill.Top, colPanel, colLine, 1)
	dcol := uint32(0x555555)
	if npads > 0 {
		dcol = colAccent2
	}
	d := s(9)
	paintDot(dc, px+s(12), (pill.Top+pill.Bottom-d)/2, d, dcol)
	paintText(dc, u.small, status, winRect{px + s(28), pill.Top, x1 - s(8), pill.Bottom}, colText, dtSingleLine|dtVCenter|dtEndEllipsis)
	y += hh + s(16)

	// the footer (controller check and what this build plays), from the bottom
	plays := "This test build plays " + availableLabels() + " games." + strings.ReplaceAll(dolIdleNote()+mameIdleNote(), "\n", " ") + " Press F1 for the keys and shortcuts."
	playsH := textHeight(dc, u.small, plays, x1-x0)
	chipH := s(24)
	chipsW := s(84)
	for _, c := range padChips {
		chipsW += textWidth(dc, u.small, c.label) + s(16) + s(4)
	}
	showChips := chipsW <= x1-x0 && H > s(420)
	footH := playsH + s(8)
	if showChips {
		footH += chipH + s(10)
	}
	if H < s(300) {
		footH = 0
	}

	// the drop target
	dropH := s(150)
	if H < s(380) {
		dropH = s(104)
	}
	box := winRect{x0, y, x1, y + dropH}
	roundBox(dc, box, s(14), colPanel, -1, 0)
	dashedBorder(dc, box, s(8), s(5), max(1, s(2)), colLine)
	ty := box.Top + s(14)
	if dropH > s(110) {
		paintText(dc, u.med, "Drop a game file here", winRect{x0 + s(12), ty, x1 - s(12), ty + s(28)}, colText, dtCenter|dtSingleLine|dtVCenter)
		ty += s(30)
		hint := "The system is picked from the file's extension. Zipped games (.zip, .7z) work too."
		th := textHeight(dc, u.small, hint, x1-x0-s(40))
		paintText(dc, u.small, hint, winRect{x0 + s(20), ty, x1 - s(20), ty + th}, colMuted, dtCenter|dtWordBreak)
	} else {
		paintText(dc, u.med, "Drop a game file here", winRect{x0 + s(12), ty, x1 - s(12), ty + s(28)}, colText, dtCenter|dtSingleLine|dtVCenter)
	}
	bt := "Open game…   Ctrl+O"
	bw := textWidth(dc, u.bold, bt) + s(36)
	bh := s(38)
	btn := winRect{(x0 + x1 - bw) / 2, box.Bottom - bh - s(14), (x0 + x1 + bw) / 2, box.Bottom - s(14)}
	fill := uint32(colAccent)
	if u.hover >= 0 && u.hover < len(u.prevHits) && u.prevHits[u.hover].kind == hitOpen {
		fill = 0x138a13
	}
	roundBox(dc, btn, s(8), fill, -1, 0)
	paintText(dc, u.bold, bt, btn, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
	u.hits = append(u.hits, homeHit{btn, hitOpen, 0})
	y = box.Bottom + s(18)

	// recent games
	bottom := H - m - footH
	if bottom-y > s(60) {
		paintText(dc, u.smallBold, "RECENT GAMES", winRect{x0, y, x1, y + s(20)}, colMuted, dtSingleLine|dtVCenter)
		y += s(26)
		rowH, gap := s(36), s(6)
		rec := a.settings.Recent
		if len(rec) == 0 {
			paintText(dc, u.small, "Games you open show up here, newest first.", winRect{x0, y, x1, y + s(22)}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis)
		}
		fit := int((bottom - y + gap) / (rowH + gap))
		if fit < len(rec) && fit > 0 {
			fit-- // room for "N more"
		}
		for i := 0; i < len(rec) && i < fit; i++ {
			r := winRect{x0, y, x1, y + rowH}
			bg, border := uint32(colPanel2), int64(colPanel2)
			hovered := u.hover >= 0 && u.hover < len(u.prevHits) && u.prevHits[u.hover].kind == hitRecent && u.prevHits[u.hover].idx == i
			if hovered {
				border = colHover
			}
			if i == u.sel {
				bg, border = colSelBg, colAccent2
			}
			roundBox(dc, r, s(10), bg, border, max(1, s(2)))
			name := gameNameOf(rec[i])
			label := recentLabel(rec[i])
			lw := textWidth(dc, u.small, label)
			paintText(dc, u.small, label, winRect{x1 - s(14) - lw, r.Top, x1 - s(14), r.Bottom}, colMuted, dtSingleLine|dtVCenter)
			nr := winRect{x0 + s(14), r.Top, x1 - s(28) - lw, r.Bottom}
			paintText(dc, u.bold, name, nr, colText, dtSingleLine|dtVCenter|dtEndEllipsis)
			if nw := textWidth(dc, u.bold, name); nw+s(24) < nr.Right-nr.Left {
				dir := filepath.Dir(rec[i])
				paintText(dc, u.small, dir, winRect{nr.Left + nw + s(14), r.Top, nr.Right, r.Bottom}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis|0x4000) // DT_PATH_ELLIPSIS
			}
			u.hits = append(u.hits, homeHit{r, hitRecent, i})
			y += rowH + gap
		}
		if more := len(rec) - fit; fit > 0 && more > 0 {
			paintText(dc, u.small, fmt.Sprintf("%d more in File > Recent games", more), winRect{x0, y, x1, y + s(22)}, colMuted, dtSingleLine|dtVCenter)
		}
	}

	// footer
	if footH > 0 {
		fy := H - m - footH + s(8)
		if showChips {
			paintText(dc, u.small, "Controller:", winRect{x0, fy, x0 + s(84), fy + chipH}, colMuted, dtSingleLine|dtVCenter)
			cx := x0 + s(84)
			for _, c := range padChips {
				w := textWidth(dc, u.small, c.label) + s(16)
				r := winRect{cx, fy, cx + w, fy + chipH}
				if npads > 0 && pad.Buttons[c.b] {
					roundBox(dc, r, s(6), colAccent, -1, 0)
					paintText(dc, u.small, c.label, r, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
				} else {
					roundBox(dc, r, s(6), colPanel2, colLine, 1)
					paintText(dc, u.small, c.label, r, colMuted, dtCenter|dtSingleLine|dtVCenter)
				}
				cx += w + s(4)
			}
			fy += chipH + s(10)
		}
		paintText(dc, u.small, plays, winRect{x0, fy, x1, fy + playsH}, colMuted, dtWordBreak)
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
			"RetroPixels Native", mbYesNo|mbIconWarning) == idYes {
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
		u.navDir = 0
		if f.Buttons[padUp] {
			u.navDir = -1
		} else if f.Buttons[padDown] {
			u.navDir = 1
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
	dir := 0
	if now.Buttons[padUp] {
		dir = -1
	} else if now.Buttons[padDown] {
		dir = 1
	}
	t := time.Now()
	if dir != 0 {
		if dir != u.navDir {
			a.moveSel(dir)
			u.navAt = t.Add(250 * time.Millisecond)
		} else if t.After(u.navAt) {
			a.moveSel(dir)
			u.navAt = t.Add(110 * time.Millisecond)
		}
	}
	u.navDir = dir
	if now.Buttons[padA] && !u.padA && u.sel >= 0 {
		u.padA = true
		a.openRecent(u.sel)
		return
	}
	u.padA = now.Buttons[padA]
}

// engineRunning: a game is playing in PixelU, PixelU-Wii or MAME.
func (a *app) engineRunning() bool {
	return a.dol.run != nil || a.pu.run != nil || a.mame.run != nil
}
