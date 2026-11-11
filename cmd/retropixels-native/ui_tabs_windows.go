package main

// The start screen's tabs, laid out like RetroPixels.exe's window: Classic
// consoles (the system cards, the drop target, recent games and the
// controller check), the GameCube, Wii, Wii U, 3DS / DS and Arcade (MAME)
// libraries as grids of box art, and Recordings. Everything is painted
// here (no child windows), in the web page's colours.

import (
	"fmt"
	"hash/fnv"
	"image"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode"
	"unsafe"

	"retropixels/internal/boxart"
	"retropixels/internal/dolphin"
	"retropixels/internal/handheld"
	"retropixels/internal/mame"
	"retropixels/internal/pixelu"
)

// the tabs, in RetroPixels.exe's order
const (
	tabClassic = iota
	tabGC
	tabWii
	tabWiiU
	tabHandheld
	tabMame
	tabRecordings
	numTabs
)

var homeTabs = [numTabs]struct {
	label string
	dot   int64  // the coloured dot before the name (-1: none)
	color uint32 // the tab's colour: tile borders and its main button
	title string // the library's heading
}{
	{"Classic consoles", -1, colAccent2, ""},
	{"GameCube", 0xff8c9b, 0xcd5a6a, "GameCube library"}, // #9b8cff, button #6a5acd
	{"Wii", 0xffc35f, 0xc48c2b, "Wii library"},           // #5fc3ff, button #2b8cc4
	{"Wii U", 0xffb42f, 0xd68f0b, "Wii U library"},       // #2fb4ff, button #0b8fd6
	{"3DS / DS", 0x625aff, 0x2e26c4, "3DS / DS library"}, // #ff5a62, button #c4262e
	{"Arcade (MAME)", 0x6f6bff, 0x2e26c4, "Arcade (MAME)"},
	{"Recordings", -1, colAccent, "Recordings"},
}

// more hit kinds (ui_home_windows.go has hitOpen and hitRecent)
const (
	hitTab = iota + 100
	hitSys
	hitTile
	hitAction
	hitRecording
)

// actions (hitAction's idx)
const (
	actAddFolder = iota
	actRescan
	actSettings
	actOpenFile
	actSort
	actRecFolder
	actRecChange
	actFilter
)

// a library game, whatever its system
type libTile struct {
	name, tag string
	tagCol    uint32
	cover     boxart.Game
	played    time.Time
	play      func()
}

// homeLibs: the Wii U and 3DS / DS libraries (the GameCube / Wii and MAME
// ones are in a.dol and a.mame).
type homeLibs struct {
	mu                  sync.Mutex
	wiiu                []pixelu.Game
	hh                  []handheld.Game
	wiiuDone, hhDone    bool
	wiiuBusy, hhBusy    bool
	recs                []recFile
	recsAt              time.Time
	hhFilter            string // "", "3ds" or "ds"
	loadedOnce          [numTabs]bool
	art                 *tileArt
	shown               []libTile
	cols                int
	tileSel             [numTabs]int
	scroll              [numTabs]int32
	contentH            [numTabs]int32
	viewTop, viewBottom int32
}

type recFile struct {
	path string
	size int64
	when time.Time
}

func (a *app) libs() *homeLibs {
	if a.ui.libs == nil {
		a.ui.libs = &homeLibs{art: newTileArt()}
		for i := range a.ui.libs.tileSel {
			a.ui.libs.tileSel[i] = -1
		}
	}
	return a.ui.libs
}

// ---------------- the header and the tab row ----------------

// drawHeader paints RETROPIXELS, the version and the controller pill, with
// the line under them, and gives the header's height.
func (a *app) drawHeader(dc uintptr, W int32) int32 {
	u := a.ui
	s := a.sc
	padX, padY := s(24), s(16)
	if W < s(520) {
		padX = s(12)
	}
	hh := s(32)
	x, y := padX, padY
	for _, part := range []struct {
		t string
		c uint32
	}{{"RETRO", colText}, {"PIXELS", colAccent2}} {
		w := textWidth(dc, a.bigFont, part.t)
		paintText(dc, a.bigFont, part.t, winRect{x, y, x + w + 2, y + hh}, part.c, dtSingleLine|dtVCenter)
		x += w
	}
	v := " v" + version
	paintText(dc, u.small, v, winRect{x + s(4), y + s(3), x + s(4) + textWidth(dc, u.small, v) + 2, y + hh}, colMuted, dtSingleLine|dtVCenter)
	x += s(4) + textWidth(dc, u.small, v)

	_, npads := homePad(u.pads)
	status := "No controller — press any button on your Xbox controller"
	if npads == 1 {
		status = "Xbox controller connected"
	} else if npads > 1 {
		status = fmt.Sprintf("%d controllers connected", npads)
	}
	pw := textWidth(dc, u.small, status) + s(44)
	py := y
	bottom := y + hh
	if W-padX-pw < x+s(16) { // no room beside the title: under it
		py = y + hh + s(8)
		bottom = py + hh
	}
	px := max(padX, W-padX-pw)
	pill := winRect{px, py, min(W-padX, px+pw), py + hh}
	roundBox(dc, pill, pill.Bottom-pill.Top, colPanel, colLine, 1)
	dcol := uint32(0x555555)
	if npads > 0 {
		dcol = colAccent2
	}
	d := s(10)
	paintDot(dc, px+s(13), (pill.Top+pill.Bottom-d)/2, d, dcol)
	paintText(dc, u.small, status, winRect{px + s(32), pill.Top, pill.Right - s(10), pill.Bottom}, colText, dtSingleLine|dtVCenter|dtEndEllipsis)
	h := bottom + padY
	fillColor(dc, winRect{0, h - 1, W, h}, colLine)
	return h
}

// drawTabs paints the tab row at y across [x0, x1] and gives its bottom.
func (a *app) drawTabs(dc uintptr, x0, x1, y int32) int32 {
	u := a.ui
	s := a.sc
	font := u.bold
	padX := s(16)
	total := int32(0)
	for _, t := range homeTabs {
		total += textWidth(dc, font, t.label) + 2*padX + s(6)
		if t.dot >= 0 {
			total += s(14)
		}
	}
	if total > x1-x0 { // a narrow window: tighter, then the smaller font
		padX = s(8)
		total = 0
		for _, t := range homeTabs {
			total += textWidth(dc, font, t.label) + 2*padX + s(6)
			if t.dot >= 0 {
				total += s(14)
			}
		}
		if total > x1-x0 {
			font = u.small
			padX = s(6)
		}
	}
	h := s(44)
	fillColor(dc, winRect{x0, y + h - 1, x1, y + h}, colLine)
	x := x0
	for i, t := range homeTabs {
		w := textWidth(dc, font, t.label) + 2*padX
		if t.dot >= 0 {
			w += s(14)
		}
		r := winRect{x, y, x + w, y + h}
		sel := i == u.tab
		hov := a.hovering(hitTab, i)
		col := uint32(colMuted)
		if sel || hov {
			col = colText
		}
		tx := x + padX
		if t.dot >= 0 {
			d := s(9)
			paintDot(dc, tx, y+(h-d)/2, d, uint32(t.dot))
			tx += s(14)
		}
		paintText(dc, font, t.label, winRect{tx, y, x + w, y + h}, col, dtSingleLine|dtVCenter)
		if sel {
			fillColor(dc, winRect{x, y + h - s(3), x + w, y + h}, colAccent2)
		}
		u.hits = append(u.hits, homeHit{r, hitTab, i})
		x += w + s(6)
	}
	return y + h
}

func (a *app) hovering(kind, idx int) bool {
	u := a.ui
	return u.hover >= 0 && u.hover < len(u.prevHits) && u.prevHits[u.hover].kind == kind && u.prevHits[u.hover].idx == idx
}

// button paints one of the page's buttons (primary: filled with fill;
// otherwise the outlined "ghost" kind) and adds it as an action.
func (a *app) button(dc uintptr, x, y int32, label string, primary bool, fill uint32, act int) winRect {
	u := a.ui
	s := a.sc
	w := textWidth(dc, u.smallBold, label) + s(22)
	r := winRect{x, y, x + w, y + s(32)}
	hov := a.hovering(hitAction, act)
	if primary {
		if hov {
			fill = lighten(fill)
		}
		roundBox(dc, r, s(8), fill, -1, 0)
		paintText(dc, u.smallBold, label, r, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
	} else {
		bg := uint32(colPanel)
		if hov {
			bg = colPanel2
		}
		roundBox(dc, r, s(8), bg, colLine, 1)
		paintText(dc, u.smallBold, label, r, colText, dtCenter|dtSingleLine|dtVCenter)
	}
	u.hits = append(u.hits, homeHit{r, hitAction, act})
	return r
}

func lighten(c uint32) uint32 {
	r, g, b := c&0xff, (c>>8)&0xff, (c>>16)&0xff
	f := func(v uint32) uint32 { return min(255, v+v/8+8) }
	return f(r) | f(g)<<8 | f(b)<<16
}

// section paints a panel (the page's .sw-sec / .card) and gives its inside.
func (a *app) section(dc uintptr, r winRect) winRect {
	s := a.sc
	roundBox(dc, r, s(14), colPanel, colLine, 1)
	p := s(18)
	return winRect{r.Left + p, r.Top + p, r.Right - p, r.Bottom - p}
}

// cardHeading: the small capitals headings of the Classic tab's cards.
func (a *app) cardHeading(dc uintptr, x0, x1, y int32, text string) int32 {
	s := a.sc
	paintText(dc, a.ui.smallBold, strings.ToUpper(text), winRect{x0, y, x1, y + s(20)}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis)
	return y + s(30)
}

// ---------------- Classic consoles ----------------

// classicCards: the system cards, as on RetroPixels.exe's first tab.
var classicCards = []struct{ id, name, exts string }{
	{"nes", "NES", ".nes .fds .unf"},
	{"snes", "Super Nintendo", ".sfc .smc .fig .swc"},
	{"gb", "Game Boy / Color", ".gb .gbc"},
	{"gba", "Game Boy Advance", ".gba"},
	{"n64", "Nintendo 64", ".z64 .n64 .v64"},
	{"wiiu", "Wii U", ".wua .wud .wux · PixelU"},
	{"handheld", "3DS / DS", ".3ds .cia .nds · PixelDS"},
	{"segaMD", "Sega Genesis", ".md .gen .smd .bin"},
	{"sms", "Master System", ".sms"},
	{"atari2600", "Atari 2600", ".a26 .bin"},
	{"atari7800", "Atari 7800", ".a78 .bin"},
	{"arcade", "Arcade", ".zip ROM sets"},
}

func (a *app) drawClassic(dc uintptr, x0, x1, y int32, H int32) int32 {
	u := a.ui
	s := a.sc
	gap := s(12)

	// the system cards
	cols := int32(4)
	if x1-x0 < s(640) {
		cols = 2
	}
	cw := (x1 - x0 - gap*(cols-1)) / cols
	ch := s(72)
	for i, c := range classicCards {
		col, row := int32(i)%cols, int32(i)/cols
		r := winRect{x0 + col*(cw+gap), y + row*(ch+gap), x0 + col*(cw+gap) + cw, y + row*(ch+gap) + ch}
		bg, border := uint32(colPanel), int64(colLine)
		if a.hovering(hitSys, i) {
			border = colHover
		}
		if c.id == binSystem && binSystem != "" {
			bg, border = colSelBg, colAccent2
		}
		roundBox(dc, r, s(12), bg, border, max(1, s(2)))
		paintText(dc, u.bold, c.name, winRect{r.Left + s(14), r.Top + s(12), r.Right - s(10), r.Top + s(36)}, colText, dtSingleLine|dtVCenter|dtEndEllipsis)
		paintText(dc, u.small, c.exts, winRect{r.Left + s(14), r.Top + s(36), r.Right - s(10), r.Top + s(58)}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis)
		u.hits = append(u.hits, homeHit{r, hitSys, i})
	}
	rows := (int32(len(classicCards)) + cols - 1) / cols
	y += rows*(ch+gap) + s(8)

	// the drop target
	hint := "The system is picked automatically from the file extension. Zipped games work too. For .bin files (used by Genesis and both Ataris) the system is guessed — click it above first if it's guessed wrong. Archipelago patch files (.apsm, .aplttp, .apred, .apemerald...) open here too."
	th := textHeight(dc, u.small, hint, x1-x0-s(48))
	dropH := s(40) + s(30) + th + s(16) + s(40) + s(40)
	box := winRect{x0, y, x1, y + dropH}
	roundBox(dc, box, s(14), colPanel, -1, 0)
	dashedBorder(dc, box, s(8), s(5), max(1, s(2)), colLine)
	ty := box.Top + s(40)
	paintText(dc, u.med, "Drop a game file here", winRect{x0 + s(12), ty, x1 - s(12), ty + s(28)}, colText, dtCenter|dtSingleLine|dtVCenter)
	ty += s(32)
	paintText(dc, u.small, hint, winRect{x0 + s(24), ty, x1 - s(24), ty + th}, colMuted, dtCenter|dtWordBreak)
	ty += th + s(16)
	bt := "Choose game file…"
	bw := textWidth(dc, u.bold, bt) + s(36)
	btn := winRect{(x0 + x1 - bw) / 2, ty, (x0 + x1 + bw) / 2, ty + s(40)}
	fill := uint32(colAccent)
	if a.hovering(hitOpen, 0) {
		fill = 0x138a13
	}
	roundBox(dc, btn, s(8), fill, -1, 0)
	paintText(dc, u.bold, bt, btn, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
	u.hits = append(u.hits, homeHit{btn, hitOpen, 0})
	y = box.Bottom + s(20)

	// recent games and the controller, side by side when there's room
	rec := a.settings.Recent
	rowH, rgap := s(40), s(6)
	nrec := min(len(rec), maxRecent)
	recInner := s(30) + max(s(22), int32(nrec)*(rowH+rgap))
	padH := s(30) + a.padCheckHeight(dc, x1-x0-s(36))
	wide := x1-x0 >= s(760)
	var recBox, padBox winRect
	if wide {
		half := (x1 - x0 - s(20)) / 2
		hgt := max(recInner, padH) + s(36)
		recBox = winRect{x0, y, x0 + half, y + hgt}
		padBox = winRect{x0 + half + s(20), y, x1, y + hgt}
		y += hgt + s(20)
	} else {
		recBox = winRect{x0, y, x1, y + recInner + s(36)}
		y = recBox.Bottom + s(20)
		padBox = winRect{x0, y, x1, y + padH + s(36)}
		y = padBox.Bottom + s(20)
	}
	in := a.section(dc, recBox)
	ry := a.cardHeading(dc, in.Left, in.Right, in.Top, "Recent games")
	if len(rec) == 0 {
		paintText(dc, u.small, "Games you open show up here, newest first.", winRect{in.Left, ry, in.Right, ry + s(22)}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis)
	}
	for i := 0; i < nrec; i++ {
		r := winRect{in.Left, ry, in.Right, ry + rowH}
		bg, border := uint32(colPanel2), int64(colLine)
		if a.hovering(hitRecent, i) {
			border = colHover
		}
		if i == u.sel {
			bg, border = colSelBg, colAccent2
		}
		roundBox(dc, r, s(10), bg, border, 1)
		label := recentLabel(rec[i])
		lw := textWidth(dc, u.small, label)
		paintText(dc, u.small, label, winRect{r.Right - s(12) - lw, r.Top, r.Right - s(12), r.Bottom}, colMuted, dtSingleLine|dtVCenter)
		paintText(dc, u.bold, gameNameOf(rec[i]), winRect{r.Left + s(12), r.Top, r.Right - s(24) - lw, r.Bottom}, colText, dtSingleLine|dtVCenter|dtEndEllipsis)
		u.hits = append(u.hits, homeHit{r, hitRecent, i})
		ry += rowH + rgap
	}
	in = a.section(dc, padBox)
	py := a.cardHeading(dc, in.Left, in.Right, in.Top, "Xbox controller — live")
	a.drawPadCheck(dc, in.Left, in.Right, py)

	// what this build plays
	plays := strings.ReplaceAll(dolIdleNote()+mameIdleNote(), "\n", " ") + " Press F1 for the keys and shortcuts."
	plays = strings.TrimSpace(plays)
	ph := textHeight(dc, u.small, plays, x1-x0)
	paintText(dc, u.small, plays, winRect{x0, y, x1, y + ph}, colMuted, dtWordBreak)
	return y + ph + s(24)
}

// the controller check: buttons light up while pressed
func (a *app) padCheckHeight(dc uintptr, w int32) int32 {
	s := a.sc
	rows, x := int32(1), int32(0)
	for _, c := range padChips {
		cw := textWidth(dc, a.ui.small, c.label) + s(20) + s(6)
		if x+cw > w && x > 0 {
			rows++
			x = 0
		}
		x += cw
	}
	return rows*s(34) + s(46)
}

func (a *app) drawPadCheck(dc uintptr, x0, x1, y int32) {
	u := a.ui
	s := a.sc
	pad, npads := homePad(u.pads)
	x := x0
	for _, c := range padChips {
		w := textWidth(dc, u.small, c.label) + s(20)
		if x+w > x1 && x > x0 {
			x = x0
			y += s(34)
		}
		r := winRect{x, y, x + w, y + s(28)}
		if npads > 0 && pad.Buttons[c.b] {
			roundBox(dc, r, s(7), colAccent, -1, 0)
			paintText(dc, u.small, c.label, r, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
		} else {
			roundBox(dc, r, s(7), colPanel2, colLine, 1)
			paintText(dc, u.small, c.label, r, colMuted, dtCenter|dtSingleLine|dtVCenter)
		}
		x += w + s(6)
	}
	y += s(40)
	paintText(dc, u.small, "Buttons light up when pressed, so you can confirm the controller is working before you start a game.",
		winRect{x0, y, x1, y + s(40)}, colMuted, dtWordBreak)
}

// ---------------- the libraries ----------------

// libraryTiles: the tab's games, sorted, and what the info boxes say.
func (a *app) libraryTiles(tab int) (tiles []libTile, emu, emuWarn, folders string, foldersWarn bool, count, empty string) {
	l := a.libs()
	switch tab {
	case tabGC, tabWii:
		a.dol.mu.Lock()
		games, loaded, loading := a.dol.libGames, a.dol.libLoaded, a.dol.libLoading
		a.dol.mu.Unlock()
		want := map[string]bool{"GameCube": tab == tabGC, "Wii": tab == tabWii, "WiiWare": tab == tabWii}
		for _, g := range games {
			if !want[g.Platform] {
				continue
			}
			g := g
			col := uint32(0xa83f4b) // #4b3fa8
			if g.Platform == "Wii" {
				col = 0xc48c2b
			} else if g.Platform == "WiiWare" {
				col = 0x6d7a1f
			}
			tiles = append(tiles, libTile{name: g.Name, tag: g.Platform, tagCol: col, cover: dolCover(g), played: g.Played,
				play: func() { a.dolPlay(g) }})
		}
		if exe := a.dolExe(); exe != "" {
			emu = "PixelU-Wii: " + exe
		} else {
			emuWarn = "PixelU's Wii/GameCube engine wasn't found. Put the PixelU folder next to RetroPixels (it has wii\\PixelU-Wii inside)."
		}
		folders, foldersWarn = folderList(a.settings.Dolphin.Folders)
		what := homeTabs[tab].label
		switch {
		case !loaded && loading:
			empty = "Looking for games…"
		case len(a.settings.Dolphin.Folders) == 0:
			empty = "Add the folder with your " + what + " games."
		case len(tiles) == 0:
			empty = "No " + what + " games in your games folders. Use Add games folder… if they're somewhere else."
		}
		count = fmt.Sprintf("%d %s games", len(tiles), what)
	case tabWiiU:
		l.mu.Lock()
		games, done := l.wiiu, l.wiiuDone
		l.mu.Unlock()
		for _, g := range games {
			g := g
			tiles = append(tiles, libTile{name: g.Name, tag: g.Kind, tagCol: 0xd68f0b, cover: boxart.Game{System: boxart.WiiU, Path: g.Path, Name: g.Name},
				played: a.settings.PixelU.Played[strings.ToLower(g.Path)], play: func() { a.homePlayWiiU(g) }})
		}
		if exe := a.pixeluExe(); exe != "" {
			emu = "PixelU: " + exe
		} else {
			emuWarn = "PixelU wasn't found. Put the PixelU folder next to RetroPixels, or choose it in the Wii U menu."
		}
		folders, foldersWarn = folderList(a.settings.PixelU.Folders)
		switch {
		case !done && len(a.settings.PixelU.Folders) > 0:
			empty = "Looking for games…"
		case len(a.settings.PixelU.Folders) == 0:
			empty = "Add the folder with your Wii U games (.wua, .wud, .wux, or unpacked games)."
		case len(tiles) == 0:
			empty = "No Wii U games in your games folders."
		}
		count = fmt.Sprintf("%d Wii U games", len(tiles))
	case tabHandheld:
		l.mu.Lock()
		games, done, filter := l.hh, l.hhDone, l.hhFilter
		l.mu.Unlock()
		for _, g := range games {
			if filter == "3ds" && g.System != handheld.System3DS || filter == "ds" && g.System != handheld.SystemDS {
				continue
			}
			g := g
			sys, tag, col := boxart.DS, "NDS", uint32(0x786f6b)
			if g.System == handheld.System3DS {
				sys, tag, col = boxart.N3DS, "3DS", 0x2e26c4
			}
			tiles = append(tiles, libTile{name: g.Name, tag: tag, tagCol: col, cover: boxart.Game{System: sys, Path: g.Path, Name: g.Name},
				played: a.settings.Handheld.Played[strings.ToLower(g.Path)], play: func() { a.homePlayHandheld(g) }})
		}
		if exe := a.hhExe(handheld.PixelDS); exe != "" {
			emu = "PixelDS: " + exe
		} else {
			emuWarn = "PixelDS wasn't found. Put the PixelDS folder next to RetroPixels."
		}
		folders, foldersWarn = folderList(a.settings.Handheld.Folders)
		switch {
		case !done && len(a.settings.Handheld.Folders) > 0:
			empty = "Looking for games…"
		case len(a.settings.Handheld.Folders) == 0:
			empty = "Add the folder with your 3DS and DS games (.3ds, .cia, .nds)."
		case len(tiles) == 0:
			empty = "No 3DS or DS games here."
		}
		count = fmt.Sprintf("%d games", len(tiles))
	case tabMame:
		a.mame.mu.Lock()
		games, loaded, loading, checking := a.mame.games, a.mame.loaded, a.mame.loading, a.mame.checking
		a.mame.mu.Unlock()
		for _, g := range games {
			g := g
			name := g.Name
			if name == "" {
				name = g.Short
			}
			tiles = append(tiles, libTile{name: name, tag: g.Short, tagCol: 0x2e26c4, cover: mameCover(g), played: g.Played,
				play: func() { a.mamePlay(g, "") }})
		}
		c := mame.Load()
		if n := len(c.Installs); n > 0 {
			emu = fmt.Sprintf("%d MAME version(s), and RetroPixels' own arcade emulation", n)
		} else {
			emu = "RetroPixels' own arcade emulation. Add or download a MAME version for every other game (Library and MAME versions…)."
		}
		folders, foldersWarn = folderList(c.Folders)
		switch {
		case !loaded && loading:
			empty = "Looking for games…"
		case len(c.Folders) == 0:
			empty = "Add the folder with your arcade ROM sets (.zip, keep them zipped)."
		case len(tiles) == 0:
			empty = "No ROM sets in your ROM folders."
		}
		count = fmt.Sprintf("%d arcade games", len(tiles))
		if checking {
			count += " (checking which MAME plays each…)"
		}
	}
	if a.settings.HomeSortPlayed {
		sort.SliceStable(tiles, func(i, j int) bool { return tiles[i].played.After(tiles[j].played) })
	} else {
		sort.SliceStable(tiles, func(i, j int) bool { return strings.ToLower(tiles[i].name) < strings.ToLower(tiles[j].name) })
	}
	return
}

func folderList(f []string) (string, bool) {
	if len(f) == 0 {
		return "None yet", true
	}
	return strings.Join(f, "\n"), false
}

func (a *app) drawLibrary(dc uintptr, tab int, x0, x1, y int32) int32 {
	u := a.ui
	l := a.libs()
	s := a.sc
	tiles, emu, emuWarn, folders, foldersWarn, count, empty := a.libraryTiles(tab)
	l.shown = tiles
	t := homeTabs[tab]
	top := y

	in := winRect{x0 + s(18), y + s(18), x1 - s(18), 0}
	cy := in.Top
	paintText(dc, u.med, t.title, winRect{in.Left, cy, in.Right, cy + s(28)}, colText, dtSingleLine|dtVCenter)
	cy += s(36)

	// Emulator and Games folders
	boxW := (in.Right - in.Left - s(10)) / 2
	stack := in.Right-in.Left < s(520)
	if stack {
		boxW = in.Right - in.Left
	}
	info := func(x, y int32, head, body string, warn bool, minH int32) int32 {
		bh := max(minH, textHeight(dc, u.small, body, boxW-s(24)))
		h := s(30) + bh + s(10)
		r := winRect{x, y, x + boxW, y + h}
		roundBox(dc, r, s(8), colPanel2, colLine, 1)
		paintText(dc, u.small, head, winRect{x + s(12), y + s(8), r.Right - s(12), y + s(28)}, colMuted, dtSingleLine|dtVCenter)
		col := uint32(colText)
		f := u.small
		if warn {
			col, f = colWarn, u.smallBold
		}
		paintText(dc, f, body, winRect{x + s(12), y + s(28), r.Right - s(12), y + s(28) + bh}, col, dtWordBreak)
		return h
	}
	emuText, emuW := emu, false
	if emuWarn != "" {
		emuText, emuW = emuWarn, true
	}
	// both boxes as tall as the taller one
	h1 := textHeight(dc, u.small, emuText, boxW-s(24))
	h2 := textHeight(dc, u.small, folders, boxW-s(24))
	hm := int32(0)
	if !stack {
		hm = max(h1, h2) // side by side: the same height
	}
	bh := info(in.Left, cy, "Emulator", emuText, emuW, hm)
	if stack {
		cy += bh + s(10)
		bh = info(in.Left, cy, "Games folders", folders, foldersWarn, 0)
	} else {
		info(in.Left+boxW+s(10), cy, "Games folders", folders, foldersWarn, hm)
	}
	cy += bh + s(12)

	// the buttons
	bx := in.Left
	put := func(label string, primary bool, act int) {
		w := textWidth(dc, u.smallBold, label) + s(22)
		if bx+w > in.Right && bx > in.Left {
			bx = in.Left
			cy += s(40)
		}
		r := a.button(dc, bx, cy, label, primary, t.color, act)
		bx = r.Right + s(8)
	}
	switch tab {
	case tabMame:
		put("Open a game file…", true, actOpenFile)
		put("Add ROM folder…", false, actAddFolder)
		put("Look again", false, actRescan)
		put("Library and MAME versions…", false, actSettings)
	case tabGC, tabWii:
		put("Add games folder…", false, actAddFolder)
		put("Find games again", false, actRescan)
		put("Library and folders…", false, actSettings)
		put("PixelU settings…", false, actFilter)
	case tabWiiU:
		put("Open a Wii U game…", true, actOpenFile)
		put("Add games folder…", false, actAddFolder)
		put("Find games again", false, actRescan)
	case tabHandheld:
		put("Open a 3DS or DS game…", true, actOpenFile)
		put("Add games folder…", false, actAddFolder)
		put("Find games again", false, actRescan)
		show := map[string]string{"": "Showing: 3DS and DS", "3ds": "Showing: 3DS only", "ds": "Showing: DS only"}[l.hhFilter]
		put(show, false, actFilter)
	}
	sortLabel := "Sort: A – Z"
	if a.settings.HomeSortPlayed {
		sortLabel = "Sort: Recently played"
	}
	put(sortLabel, false, actSort)
	cw := textWidth(dc, u.small, count)
	if bx+cw+s(8) > in.Right {
		bx = in.Left
		cy += s(40)
	}
	paintText(dc, u.small, count, winRect{bx + s(4), cy, in.Right, cy + s(32)}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis)
	cy += s(46)

	// the grid
	if empty != "" && len(tiles) == 0 {
		paintText(dc, u.small, empty, winRect{in.Left + s(4), cy, in.Right, cy + s(40)}, colMuted, dtWordBreak)
		cy += s(48)
	}
	gap := s(12)
	cols := max(1, int((in.Right-in.Left+gap)/(s(150)+gap)))
	l.cols = cols
	tw := (in.Right - in.Left - gap*int32(cols-1)) / int32(cols)
	artH := tw * 4 / 3
	th := artH + s(62)
	sel := l.tileSel[tab]
	if sel >= len(tiles) {
		sel = len(tiles) - 1
		l.tileSel[tab] = sel
	}
	for i, g := range tiles {
		col, row := int32(i%cols), int32(i/cols)
		r := winRect{in.Left + col*(tw+gap), cy + row*(th+gap), in.Left + col*(tw+gap) + tw, cy + row*(th+gap) + th}
		u.hits = append(u.hits, homeHit{r, hitTile, i})
		if r.Bottom < l.viewTop || r.Top > l.viewBottom {
			continue // out of view
		}
		border := int64(colLine)
		if a.hovering(hitTile, i) || i == sel {
			border = int64(t.dot)
		}
		roundBox(dc, r, s(12), colPanel2, border, max(1, s(2)))
		art := winRect{r.Left + s(2), r.Top + s(2), r.Right - s(2), r.Top + s(2) + artH}
		if !l.art.draw(dc, g.cover, art) {
			fillColor(dc, art, tileColour(g.name))
			paintText(dc, a.bigFont, initials(g.name), art, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
		}
		l.art.ask(g.cover)
		if g.tag != "" {
			tw := textWidth(dc, u.smallBold, g.tag) + s(12)
			tr := winRect{art.Left + s(6), art.Top + s(6), art.Left + s(6) + tw, art.Top + s(24)}
			roundBox(dc, tr, s(9), g.tagCol, -1, 0)
			paintText(dc, u.smallBold, g.tag, tr, 0xffffff, dtCenter|dtSingleLine|dtVCenter)
		}
		paintText(dc, u.smallBold, g.name, winRect{r.Left + s(10), art.Bottom + s(8), r.Right - s(8), r.Bottom - s(18)}, colText, dtWordBreak|dtEndEllipsis)
		if !g.played.IsZero() {
			paintText(dc, u.small, "Played "+fmtPlayed(g.played, time.Now()), winRect{r.Left + s(10), r.Bottom - s(22), r.Right - s(8), r.Bottom - s(4)}, colMuted, dtSingleLine|dtEndEllipsis)
		}
	}
	if len(tiles) > 0 {
		rows := int32((len(tiles) + cols - 1) / cols)
		cy += rows*(th+gap) + s(4)
	}
	paintText(dc, u.small, "Controller: D-pad or left stick to move, A to play, LB / RB to change tabs.", winRect{in.Left, cy, in.Right, cy + s(22)}, colMuted, dtSingleLine|dtEndEllipsis)
	cy += s(22) + s(18)

	_ = top
	return cy + s(24)
}

// panelled draws a page section on a panel: once on a 1x1 bitmap to learn
// its height, then for real on the panel.
func (a *app) panelled(dc uintptr, x0, x1, y int32, draw func(dc uintptr) int32) int32 {
	mdc := call(pCreateCompatibleDC, dc)
	bmp := call(pCreateCompatibleBitmap, dc, 1, 1)
	old := call(pSelectObject, mdc, bmp)
	call(pSetBkMode, mdc, transparent)
	n := len(a.ui.hits)
	end := draw(mdc)
	a.ui.hits = a.ui.hits[:n]
	call(pSelectObject, mdc, old)
	call(pDeleteObject, bmp)
	call(pDeleteDC, mdc)
	roundBox(dc, winRect{x0, y, x1, end - a.sc(24)}, a.sc(14), colPanel, colLine, 1)
	return draw(dc)
}

// drawFrame: a rounded outline only (the inside is left as it is).
func drawFrame(dc uintptr, r winRect, radius int32, color uint32) {
	pen := call(pCreatePen, 0, 1, uintptr(color))
	old := call(pSelectObject, dc, pen)
	oldBrush := call(pSelectObject, dc, call(pGetStockObject, 5)) // NULL_BRUSH
	call(pRoundRect, dc, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right), uintptr(r.Bottom), uintptr(radius), uintptr(radius))
	call(pSelectObject, dc, oldBrush)
	call(pSelectObject, dc, old)
	call(pDeleteObject, pen)
}

func initials(name string) string {
	var out []rune
	for _, w := range strings.Fields(name) {
		r := []rune(w)
		if len(r) > 0 && (unicode.IsLetter(r[0]) || unicode.IsDigit(r[0])) {
			out = append(out, unicode.ToUpper(r[0]))
		}
		if len(out) == 2 {
			break
		}
	}
	if len(out) == 0 {
		return "?"
	}
	return string(out)
}

// tileColour: a game without box art gets a colour of its own (as the web
// page's initials tiles).
func tileColour(name string) uint32 {
	h := fnv.New32a()
	h.Write([]byte(name))
	cols := []uint32{0x8a3b2f, 0x2f6a8a, 0x2f8a52, 0x7a2f8a, 0x8a6f2f, 0x2f3b8a, 0x6a8a2f, 0x2f8a85}
	return cols[h.Sum32()%uint32(len(cols))]
}

// ---------------- Recordings ----------------

func (a *app) drawRecordings(dc uintptr, x0, x1, y int32) int32 {
	u := a.ui
	l := a.libs()
	s := a.sc
	dir := a.settings.recDir()
	if time.Since(l.recsAt) > 5*time.Second {
		l.recs = listRecordings(dir, 200)
		l.recsAt = time.Now()
	}
	top := y
	in := winRect{x0 + s(18), y + s(18), x1 - s(18), 0}
	cy := in.Top
	paintText(dc, u.med, "Recordings", winRect{in.Left, cy, in.Right, cy + s(28)}, colText, dtSingleLine|dtVCenter)
	cy += s(34)
	intro := "Games are recorded with only the game's picture and sound. Recordings are saved in " + dir
	ih := textHeight(dc, u.small, intro, in.Right-in.Left)
	paintText(dc, u.small, intro, winRect{in.Left, cy, in.Right, cy + ih}, colMuted, dtWordBreak)
	cy += ih + s(12)
	bx := in.Left
	for _, b := range []struct {
		label string
		act   int
	}{{"Open recordings folder", actRecFolder}, {"Change folder…", actRecChange}} {
		r := a.button(dc, bx, cy, b.label, b.act == actRecFolder, colAccent, b.act)
		bx = r.Right + s(8)
	}
	auto := "Automatic recording is on (Record menu)"
	if !a.settings.autoRecord() {
		auto = "Automatic recording is off: F9 records (Record menu)"
	}
	paintText(dc, u.small, auto, winRect{bx + s(6), cy, in.Right, cy + s(32)}, colMuted, dtSingleLine|dtVCenter|dtEndEllipsis)
	cy += s(46)
	if len(l.recs) == 0 {
		paintText(dc, u.small, "No recordings yet. Play a game and it's recorded here.", winRect{in.Left, cy, in.Right, cy + s(24)}, colMuted, dtSingleLine|dtVCenter)
		cy += s(32)
	}
	rowH := s(44)
	now := time.Now()
	for i, r := range l.recs {
		rr := winRect{in.Left, cy, in.Right, cy + rowH}
		u.hits = append(u.hits, homeHit{rr, hitRecording, i})
		if rr.Bottom >= l.viewTop && rr.Top <= l.viewBottom {
			border := int64(colLine)
			if a.hovering(hitRecording, i) {
				border = colHover
			}
			roundBox(dc, rr, s(10), colPanel2, border, 1)
			meta := fmtPlayed(r.when, now) + " · " + fmtSize(r.size)
			mw := textWidth(dc, u.small, meta)
			paintText(dc, u.small, meta, winRect{rr.Right - s(12) - mw, rr.Top, rr.Right - s(12), rr.Bottom}, colMuted, dtSingleLine|dtVCenter)
			paintText(dc, u.bold, "▶  "+filepath.Base(r.path), winRect{rr.Left + s(12), rr.Top, rr.Right - s(24) - mw, rr.Bottom}, colText, dtSingleLine|dtVCenter|dtEndEllipsis)
		}
		cy += rowH + s(6)
	}
	cy += s(12)
	_ = top
	return cy + s(24)
}

// listRecordings: the newest videos in dir, newest first.
func listRecordings(dir string, n int) []recFile {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []recFile
	for _, e := range ents {
		if e.IsDir() {
			continue
		}
		switch strings.ToLower(filepath.Ext(e.Name())) {
		case ".mp4", ".mkv", ".webm", ".avi", ".mov":
		default:
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, recFile{filepath.Join(dir, e.Name()), info.Size(), info.ModTime()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].when.After(out[j].when) })
	if len(out) > n {
		out = out[:n]
	}
	return out
}

// ---------------- what clicks, keys and the controller do ----------------

func (a *app) setTab(t int) {
	if t < 0 || t >= numTabs || t == a.ui.tab {
		return
	}
	a.ui.tab = t
	a.settings.HomeTab = t
	saveSettings(a.settings)
	a.ui.hover = -1
	a.tabOpened()
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// tabOpened: the first time a library tab shows, its games are looked for.
func (a *app) tabOpened() {
	l := a.libs()
	t := a.ui.tab
	if l.loadedOnce[t] {
		return
	}
	l.loadedOnce[t] = true
	switch t {
	case tabGC, tabWii:
		a.dolReloadLibrary()
	case tabWiiU:
		a.homeReloadWiiU()
	case tabHandheld:
		a.homeReloadHandheld()
	case tabMame:
		a.mameReload()
	}
}

func (a *app) homeReloadWiiU() {
	l := a.libs()
	l.mu.Lock()
	if l.wiiuBusy {
		l.mu.Unlock()
		return
	}
	l.wiiuBusy = true
	l.mu.Unlock()
	folders := append([]string(nil), a.settings.PixelU.Folders...)
	played := copyPlayed(a.settings.PixelU.Played)
	go func() {
		games := pixelu.Library(folders, played)
		l.mu.Lock()
		l.wiiu, l.wiiuDone, l.wiiuBusy = games, true, false
		l.mu.Unlock()
		a.event("home-lib", "")
	}()
}

func (a *app) homeReloadHandheld() {
	l := a.libs()
	l.mu.Lock()
	if l.hhBusy {
		l.mu.Unlock()
		return
	}
	l.hhBusy = true
	l.mu.Unlock()
	folders := append([]string(nil), a.settings.Handheld.Folders...)
	played := copyPlayed(a.settings.Handheld.Played)
	go func() {
		games := handheld.Library(folders, played)
		l.mu.Lock()
		l.hh, l.hhDone, l.hhBusy = games, true, false
		l.mu.Unlock()
		a.event("home-lib", "")
	}()
}

func copyPlayed(m map[string]time.Time) map[string]time.Time {
	out := map[string]time.Time{}
	for k, v := range m {
		out[k] = v
	}
	return out
}

func (a *app) homePlayWiiU(g pixelu.Game) {
	if a.settings.PixelU.Played == nil {
		a.settings.PixelU.Played = map[string]time.Time{}
	}
	a.settings.PixelU.Played[strings.ToLower(g.Path)] = time.Now()
	saveSettings(a.settings)
	a.pixeluPlay(g)
}

func (a *app) homePlayHandheld(g handheld.Game) {
	if a.settings.Handheld.Played == nil {
		a.settings.Handheld.Played = map[string]time.Time{}
	}
	a.settings.Handheld.Played[strings.ToLower(g.Path)] = time.Now()
	saveSettings(a.settings)
	a.hhPlay(g)
}

// tabsEvent: background work done (the libraries).
func (a *app) tabsEvent(name string) {
	switch name {
	case "home-lib", "dol-lib", "mame-lib":
		if a.sess == nil {
			call(pInvalidateRect, a.hwnd, 0, 0)
		}
	}
}

func (a *app) homeAction(act int) {
	t := a.ui.tab
	l := a.libs()
	switch act {
	case actSort:
		a.settings.HomeSortPlayed = !a.settings.HomeSortPlayed
		saveSettings(a.settings)
	case actFilter:
		if t == tabHandheld {
			l.mu.Lock()
			l.hhFilter = map[string]string{"": "3ds", "3ds": "ds", "ds": ""}[l.hhFilter]
			l.mu.Unlock()
		} else {
			a.command(cmdDolSettings)
		}
	case actOpenFile:
		switch t {
		case tabMame:
			a.mameOpenFile()
		case tabWiiU:
			a.command(cmdPixelUOpenGame)
		case tabHandheld:
			a.command(cmdHHOpenGame)
		}
	case actSettings:
		switch t {
		case tabMame:
			a.openMameLibrary()
		case tabGC, tabWii:
			a.openDolLibrary()
		}
	case actRescan:
		boxart.Forget()
		l.art.forget()
		switch t {
		case tabGC, tabWii:
			a.dolReloadLibrary()
		case tabWiiU:
			a.homeReloadWiiU()
		case tabHandheld:
			a.homeReloadHandheld()
		case tabMame:
			a.mameReload()
		}
	case actAddFolder:
		switch t {
		case tabGC, tabWii:
			if dir := a.pickFolder(a.hwnd, "Add a folder with GameCube or Wii games"); a.settings.Dolphin.addFolder(dir) {
				saveSettings(a.settings)
				a.dolReloadLibrary()
			}
		case tabWiiU:
			if dir := a.pickFolder(a.hwnd, "Add a folder with Wii U games"); dir != "" && !dolphin.ContainsFold(a.settings.PixelU.Folders, dir) {
				a.settings.PixelU.Folders = append(a.settings.PixelU.Folders, dir)
				saveSettings(a.settings)
				a.homeReloadWiiU()
			}
		case tabHandheld:
			if dir := a.pickFolder(a.hwnd, "Add a folder with 3DS or DS games"); dir != "" && !dolphin.ContainsFold(a.settings.Handheld.Folders, dir) {
				a.settings.Handheld.Folders = append(a.settings.Handheld.Folders, dir)
				saveSettings(a.settings)
				a.homeReloadHandheld()
			}
		case tabMame:
			if dir := a.pickFolder(a.hwnd, "Add a folder with MAME ROM sets (.zip)"); dir != "" {
				mame.Update(func(c *mame.Config) { c.AddFolder(dir) })
				a.mameReload()
			}
		}
	case actRecFolder:
		dir := a.settings.recDir()
		_ = os.MkdirAll(dir, 0o755)
		shellOpen(dir)
	case actRecChange:
		a.command(cmdRecDir)
		l.recsAt = time.Time{}
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// sysCard: a system card clicked. Wii U and 3DS / DS open their tabs; the
// rest open a game, and a system that uses .bin files takes them from then on.
func (a *app) sysCard(i int) {
	c := classicCards[i]
	switch c.id {
	case "wiiu":
		a.setTab(tabWiiU)
		return
	case "handheld":
		a.setTab(tabHandheld)
		return
	}
	if strings.Contains(c.exts, ".bin") {
		if binSystem == c.id {
			binSystem = ""
		} else {
			binSystem = c.id
		}
		call(pInvalidateRect, a.hwnd, 0, 0)
		if binSystem == "" {
			return
		}
	}
	sys := systemByID(c.id)
	if sys == nil {
		a.command(cmdOpen)
		return
	}
	pat := "*" + strings.Join(sys.Exts, ";*") + ";*.zip;*.7z"
	filter := sys.Label + " games\x00" + pat + "\x00All files\x00*.*\x00\x00"
	if p := a.fileDialog("Open a "+sys.Label+" game", filter, a.settings.LastFolder); p != "" {
		a.openPath(p)
	}
}

// libKey: arrows and Enter on a library tab.
func (a *app) libKey(vk int) bool {
	l := a.libs()
	t := a.ui.tab
	n := len(l.shown)
	if n == 0 {
		return false
	}
	sel := l.tileSel[t]
	switch vk {
	case 0x25: // left
		sel--
	case 0x27: // right
		sel++
	case 0x26: // up
		sel -= l.cols
	case 0x28: // down
		sel += l.cols
	case 0x24:
		sel = 0
	case 0x23:
		sel = n - 1
	case vkReturn:
		if sel >= 0 && sel < n {
			l.shown[sel].play()
		}
		return true
	default:
		return false
	}
	if l.tileSel[t] < 0 {
		sel = 0
	}
	l.tileSel[t] = max(0, min(n-1, sel))
	a.scrollToTile = true
	call(pInvalidateRect, a.hwnd, 0, 0)
	return true
}

// homeWheel scrolls the page under the tabs.
func (a *app) homeWheel(delta int) {
	l := a.libs()
	t := a.ui.tab
	l.scroll[t] -= int32(delta) * a.sc(60) / 120
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// ---------------- box art for the tiles ----------------

// tileArt: covers found in the background, kept as pictures to paint.
type tileArt struct {
	mu    sync.Mutex
	asked map[string]bool
	ready map[string]*image.RGBA
	pix   map[string]artPix
}

type artPix struct {
	p    []uint32
	w, h int
}

func newTileArt() *tileArt {
	coverAgentOnce.Do(func() { boxart.UserAgent = "RetroPixels-Native/" + version })
	return &tileArt{asked: map[string]bool{}, ready: map[string]*image.RGBA{}, pix: map[string]artPix{}}
}

func (t *tileArt) ask(g boxart.Game) {
	k := coverKey(g)
	t.mu.Lock()
	if t.asked[k] {
		t.mu.Unlock()
		return
	}
	t.asked[k] = true
	t.mu.Unlock()
	go func() {
		p, err := boxart.Find(g)
		if err != nil || p == "" {
			return
		}
		img, err := boxart.Decode(p, 300, 400)
		if err != nil {
			return
		}
		t.mu.Lock()
		t.ready[k] = img
		t.mu.Unlock()
		if theApp != nil && theApp.hwnd != 0 {
			theApp.event("home-lib", "")
		}
	}()
}

func (t *tileArt) forget() {
	t.mu.Lock()
	for k := range t.asked {
		if _, ok := t.pix[k]; !ok {
			if _, ok := t.ready[k]; !ok {
				delete(t.asked, k)
			}
		}
	}
	t.mu.Unlock()
}

// draw paints the game's cover in r (kept whole, centred on the page's
// background colour); false when there's none (yet).
func (t *tileArt) draw(dc uintptr, g boxart.Game, r winRect) bool {
	k := coverKey(g)
	t.mu.Lock()
	ap, ok := t.pix[k]
	if !ok {
		if img := t.ready[k]; img != nil {
			delete(t.ready, k)
			b := img.Bounds()
			ap = artPix{make([]uint32, b.Dx()*b.Dy()), b.Dx(), b.Dy()}
			for y := 0; y < ap.h; y++ {
				for x := 0; x < ap.w; x++ {
					o := img.PixOffset(b.Min.X+x, b.Min.Y+y)
					ap.p[y*ap.w+x] = uint32(img.Pix[o])<<16 | uint32(img.Pix[o+1])<<8 | uint32(img.Pix[o+2])
				}
			}
			t.pix[k] = ap
			ok = true
		}
	}
	t.mu.Unlock()
	if !ok {
		return false
	}
	fillColor(dc, r, colBg)
	rw, rh := float64(r.Right-r.Left), float64(r.Bottom-r.Top)
	sc := min(rw/float64(ap.w), rh/float64(ap.h))
	w, h := int32(float64(ap.w)*sc), int32(float64(ap.h)*sc)
	x, y := r.Left+(r.Right-r.Left-w)/2, r.Top+(r.Bottom-r.Top-h)/2
	bmi := bitmapInfoHeader{Width: int32(ap.w), Height: -int32(ap.h), Planes: 1, BitCount: 32}
	bmi.Size = uint32(unsafe.Sizeof(bmi))
	call(pSetStretchBltMode, dc, halftone)
	call(pSetBrushOrgEx, dc, 0, 0, 0)
	call(pStretchDIBits, dc, uintptr(x), uintptr(y), uintptr(w), uintptr(h), 0, 0, uintptr(ap.w), uintptr(ap.h),
		uintptr(unsafe.Pointer(&ap.p[0])), uintptr(unsafe.Pointer(&bmi)), dibRGBColors, srcCopy)
	return true
}
