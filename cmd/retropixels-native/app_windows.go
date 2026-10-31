package main

import (
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/ffmpeg"
)

// The window: a plain Win32 window with a menu bar, the game drawn with GDI
// (what OBS Studio captures most reliably), drag and drop, and the dialogs.

const (
	cmdOpen        = 100
	cmdExit        = 101
	cmdCloseGame   = 102
	cmdClearRecent = 109
	cmdRecent0     = 110 // ..119
	cmdPause       = 200
	cmdReset       = 201
	cmdFast        = 202
	cmdSaveState   = 203
	cmdLoadState   = 204
	cmdUndoLoad    = 205
	cmdControls    = 300
	cmdVolume0     = 310 // 310 + percent/10 .. 320
	cmdMute        = 321
	cmdScale1      = 331 // ..335
	cmdFullscreen  = 340
	cmdSharp       = 350
	cmdSmooth      = 351
	cmdMousePaddle = 360
	cmdFileTypes   = 370
	cmdRecord      = 400
	cmdRecFolder   = 401
	cmdAbout       = 500

	wmAppEvent = wmApp + 1
	timerTitle = 1
)

type appEvent struct{ name, detail string }

type app struct {
	hwnd, hinst uintptr
	menu        uintptr
	fileMenu    uintptr
	recentMenu  uintptr
	gameMenu    uintptr
	optMenu     uintptr
	volMenu     uintptr
	sizeMenu    uintptr
	recMenu     uintptr
	dipMenu     uintptr
	seatMenu    uintptr
	font        uintptr
	bigFont     uintptr

	settings Settings
	sess     *session
	snd      soundOut
	wave     *waveOut
	xin      *xinput

	frameMu sync.Mutex
	frame   []uint32
	fw, fh  int
	aspect  float64
	smooth  bool
	bmi     bitmapInfoHeader

	fullscreen bool
	savedStyle uintptr
	savedRect  winRect

	evMu   sync.Mutex
	events []appEvent

	flash      string
	flashUntil time.Time
	lastTitle  string

	recOn     bool
	recStart  time.Time
	recSaving int
	recWG     sync.WaitGroup

	encMu      sync.Mutex
	encoders   map[string]string
	ffDownload string // progress text while ffmpeg downloads
	ffDeclined bool   // said No to downloading ffmpeg for automatic recording (asked once a run)

	controls      *controlsDialog
	mouseTracking bool

	ui     *uiState    // the start screen, save state slots and the rest (ui_*_windows.go)
	dol    dolState    // GameCube / Wii (dolphin_windows.go)
	mame   mameState   // arcade games in MAME (mame_windows.go)
	pu     pixeluState // Wii U games in PixelU (pixelu_windows.go)
	sm     smState     // Star Wars Trilogy Arcade in Supermodel (supermodel_windows.go)
	ap     apState     // Archipelago (archipelago_windows.go)
	stream streamState // live streaming to YouTube (stream_windows.go)
}

var theApp *app

func runApp() {
	runtime.LockOSThread()
	if len(os.Args) > 1 && !strings.HasPrefix(os.Args[1], "-") && handToRunningCopy(os.Args[1]) {
		return // the RetroPixels Native already open plays it
	}
	// If the program ever crashes, the reason goes to crash.log next to the settings.
	if os.MkdirAll(appDir(), 0o755) == nil {
		if f, err := os.Create(filepath.Join(appDir(), "crash.log")); err == nil {
			debug.SetCrashOutput(f, debug.CrashOptions{})
			f.Close()
		}
	}
	a := &app{settings: loadSettings(), xin: newXInput(), encoders: map[string]string{}, ui: newUIState()}
	theApp = a
	a.smooth = a.settings.Smooth
	a.hinst = call(pGetModuleHandleW, 0)
	if pSetProcessDpiAwarenessCx.Find() == nil {
		call(pSetProcessDpiAwarenessCx, ^uintptr(3)) // PER_MONITOR_AWARE_V2 (also in the manifest)
	}
	// The engines' game screens are docked into this window. Windows refuses
	// that for a window whose DPI awareness differs from ours unless the
	// host window allows mixed DPI content; it's made on this thread.
	if pSetThreadDpiHostingBehavior.Find() == nil {
		call(pSetThreadDpiHostingBehavior, 1) // DPI_HOSTING_BEHAVIOR_MIXED
	}
	call(pTimeBeginPeriod, 1)

	if w := openWaveOut(sampleRate); w != nil {
		a.wave, a.snd = w, w
	} else {
		a.snd = noSound{}
	}

	cls := u16(mainClass)
	icon := call(pLoadImageW, a.hinst, 1, 1, 0, 0, 0x40|0x8000) // IMAGE_ICON, LR_DEFAULTSIZE|LR_SHARED
	small := call(pLoadImageW, a.hinst, 1, 1, 16, 16, 0x8000)   // LR_SHARED
	wc := wndClassEx{
		WndProc:   syscall.NewCallback(wndProc),
		Instance:  a.hinst,
		Icon:      icon,
		IconSm:    small,
		Cursor:    call(pLoadCursorW, 0, idcArrow),
		ClassName: cls,
	}
	wc.Size = uint32(unsafe.Sizeof(wc))
	if call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc))) == 0 {
		messageBox(0, "Couldn't start: the window could not be made.", "RetroPixels Native", mbIconError)
		return
	}
	a.buildMenu()
	a.hwnd = call(pCreateWindowExW, wsExAcceptFiles, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16(a.baseTitle()))),
		wsOverlappedWindow|wsClipChildren, cwUseDefault, cwUseDefault, 960, 720, 0, a.menu, a.hinst, 0)
	if a.hwnd == 0 {
		messageBox(0, "Couldn't start: the window could not be made.", "RetroPixels Native", mbIconError)
		return
	}
	a.makeFonts()
	a.initUI()
	a.aspect = 4.0 / 3
	a.resizeToScale()
	call(pDragAcceptFiles, a.hwnd, 1)
	call(pShowWindow, a.hwnd, swShow)
	call(pUpdateWindow, a.hwnd)
	call(pSetTimer, a.hwnd, timerTitle, 250, 0)
	go registerFileTypes(false)
	a.dolStartup()
	a.apStartup()

	// Find the best video encoder in the background, so recording starts at once.
	go func() {
		if ff := ffmpeg.Find(); ff != "" {
			a.setEncoder(ff, ffmpeg.PickEncoder(ff))
		}
	}()

	if len(os.Args) > 1 && !strings.HasPrefix(os.Args[1], "-") {
		a.openPath(os.Args[1])
	}

	var m msg
	for {
		r := call(pGetMessageW, uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		if a.controls != nil && a.controls.capturing(&m) {
			continue
		}
		if a.dolDialogMessage(&m) || a.mameDialogMessage(&m) || a.streamDialogMessage(&m) {
			continue
		}
		call(pTranslateMessage, uintptr(unsafe.Pointer(&m)))
		call(pDispatchMessageW, uintptr(unsafe.Pointer(&m)))
	}
	a.recWG.Wait() // a recording still being saved finishes first
	if a.wave != nil {
		a.wave.Close()
	}
}

func (a *app) makeFonts() {
	dpi := a.dpi()
	a.font = call(pCreateFontW, uintptr(-int32(15*dpi/96)), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, uintptr(unsafe.Pointer(u16("Segoe UI"))))
	a.bigFont = call(pCreateFontW, uintptr(-int32(22*dpi/96)), 0, 0, 0, 600, 0, 0, 0, 1, 0, 0, 5, 0, uintptr(unsafe.Pointer(u16("Segoe UI"))))
}

func (a *app) dpi() int {
	if pGetDpiForWindow.Find() == nil && a.hwnd != 0 {
		if d := int(call(pGetDpiForWindow, a.hwnd)); d > 0 {
			return d
		}
	}
	return 96
}

// ---------------- menus ----------------

func (a *app) buildMenu() {
	a.menu = call(pCreateMenu)
	a.fileMenu = call(pCreatePopupMenu)
	a.recentMenu = call(pCreatePopupMenu)
	appendMenu(a.fileMenu, mfString, cmdOpen, "&Open game…\tCtrl+O")
	appendMenu(a.fileMenu, mfPopup, a.recentMenu, "&Recent games")
	appendMenu(a.fileMenu, mfString, cmdCloseGame, "&Close game")
	a.uiFileMenu()
	appendMenu(a.fileMenu, mfSeparator, 0, "")
	appendMenu(a.fileMenu, mfString, cmdExit, "E&xit\tAlt+F4")
	appendMenu(a.menu, mfPopup, a.fileMenu, "&File")

	a.gameMenu = call(pCreatePopupMenu)
	appendMenu(a.gameMenu, mfString, cmdPause, "&Pause\tP")
	appendMenu(a.gameMenu, mfString, cmdReset, "&Reset\tCtrl+R")
	appendMenu(a.gameMenu, mfString, cmdFast, "&Fast forward (hold Tab)")
	appendMenu(a.gameMenu, mfSeparator, 0, "")
	appendMenu(a.gameMenu, mfString, cmdSaveState, "&Save state\tF2")
	appendMenu(a.gameMenu, mfString, cmdLoadState, "&Load state\tF4")
	appendMenu(a.gameMenu, mfString, cmdUndoLoad, "&Undo load\tCtrl+Z")
	a.uiGameMenu()
	a.addDipMenu()
	a.addSeatMenu()
	a.apGameMenu()
	appendMenu(a.menu, mfPopup, a.gameMenu, "&Game")
	a.addDolphinMenu()
	a.addPixelUMenu()
	a.addMameMenu()

	a.optMenu = call(pCreatePopupMenu)
	appendMenu(a.optMenu, mfString, cmdControls, "&Controls…")
	a.volMenu = call(pCreatePopupMenu)
	for v := 100; v >= 10; v -= 10 {
		appendMenu(a.volMenu, mfString, uintptr(cmdVolume0+v/10), fmt.Sprintf("%d%%", v))
	}
	appendMenu(a.volMenu, mfSeparator, 0, "")
	appendMenu(a.volMenu, mfString, cmdMute, "&Mute\tM")
	appendMenu(a.optMenu, mfPopup, a.volMenu, "&Volume")
	a.sizeMenu = call(pCreatePopupMenu)
	for i := 1; i <= 5; i++ {
		appendMenu(a.sizeMenu, mfString, uintptr(cmdScale1+i-1), fmt.Sprintf("&%dx", i))
	}
	appendMenu(a.optMenu, mfPopup, a.sizeMenu, "&Window size")
	appendMenu(a.optMenu, mfString, cmdFullscreen, "&Fullscreen\tF11")
	appendMenu(a.optMenu, mfSeparator, 0, "")
	appendMenu(a.optMenu, mfString, cmdSharp, "&Sharp pixels")
	appendMenu(a.optMenu, mfString, cmdSmooth, "Smoot&h pixels")
	appendMenu(a.optMenu, mfSeparator, 0, "")
	appendMenu(a.optMenu, mfString, cmdMousePaddle, "Mouse turns the &paddle (Atari 2600)")
	appendMenu(a.optMenu, mfSeparator, 0, "")
	appendMenu(a.optMenu, mfString, cmdFileTypes, "Open game files with RetroPixels &Native…")
	a.uiOptionsMenu()
	appendMenu(a.menu, mfPopup, a.optMenu, "&Options")

	a.recMenu = call(pCreatePopupMenu)
	appendMenu(a.recMenu, mfString, cmdRecord, "&Start recording\tF9")
	appendMenu(a.recMenu, mfString, cmdRecFolder, "&Open recordings folder")
	a.uiRecordMenu()
	appendMenu(a.menu, mfPopup, a.recMenu, "&Record")
	a.addStreamMenu()

	help := call(pCreatePopupMenu)
	a.uiHelpMenu(help)
	a.apHelpMenu(help)
	appendMenu(help, mfString, cmdAbout, "&About RetroPixels Native")
	appendMenu(a.menu, mfPopup, help, "&Help")
}

// updateMenus sets the ticks, greyed items and labels before a menu opens.
func (a *app) updateMenus() {
	s := a.sess
	on := s != nil
	for _, id := range []int{cmdPause, cmdReset, cmdFast, cmdSaveState, cmdLoadState, cmdCloseGame} {
		enableItem(a.menu, id, on)
	}
	enableItem(a.menu, cmdUndoLoad, on && s.canUndo())
	paused := on && s.paused.Load()
	if paused {
		renameItem(a.menu, cmdPause, "&Resume\tP")
	} else {
		renameItem(a.menu, cmdPause, "&Pause\tP")
	}
	checkItem(a.menu, cmdFast, on && s.fast.Load())
	call(pCheckMenuRadioItem, a.menu, cmdVolume0+1, cmdVolume0+10, uintptr(cmdVolume0+(a.settings.Volume+5)/10), mfByCommand)
	checkItem(a.menu, cmdMute, a.settings.Muted)
	call(pCheckMenuRadioItem, a.menu, cmdScale1, cmdScale1+4, uintptr(cmdScale1+a.settings.Scale-1), mfByCommand)
	checkItem(a.menu, cmdFullscreen, a.fullscreen)
	sm := cmdSharp
	if a.settings.Smooth {
		sm = cmdSmooth
	}
	call(pCheckMenuRadioItem, a.menu, cmdSharp, cmdSmooth, uintptr(sm), mfByCommand)
	checkItem(a.menu, cmdMousePaddle, a.settings.MousePaddle)
	if a.recOn {
		renameItem(a.menu, cmdRecord, "&Stop recording\tF9")
	} else {
		renameItem(a.menu, cmdRecord, "&Start recording\tF9")
	}
	enableItem(a.menu, cmdRecord, on || a.recOn)
	a.uiUpdateMenus()
	a.dolUpdateMenus()
	a.pixeluUpdateMenus()
	a.supermodelUpdateMenus()
	a.mameUpdateMenus()
	a.apUpdateMenus()
	a.streamUpdateMenus()
}

func (a *app) fillRecent() {
	for call(pGetMenuItemCount, a.recentMenu) > 0 {
		call(pDeleteMenu, a.recentMenu, 0, mfByPosition)
	}
	if len(a.settings.Recent) == 0 {
		appendMenu(a.recentMenu, mfString|mfGrayed, 0, "(none yet)")
		return
	}
	for i, p := range a.settings.Recent {
		label := strings.ReplaceAll(filepath.Base(p), "&", "&&")
		if i < 9 {
			label = fmt.Sprintf("&%d  %s", i+1, label)
		} else {
			label = "1&0  " + label
		}
		appendMenu(a.recentMenu, mfString, uintptr(cmdRecent0+i), label)
	}
	appendMenu(a.recentMenu, mfSeparator, 0, "")
	appendMenu(a.recentMenu, mfString, cmdClearRecent, "&Clear the list")
}

// ---------------- window procedure ----------------

func wndProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || (a.hwnd != 0 && hwnd != a.hwnd) {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	switch uint32(m) {
	case wmEraseBkgnd:
		return 1 // everything is painted in WM_PAINT: no flicker
	case wmPaint:
		var ps paintStruct
		hdc := call(pBeginPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		a.paint(hdc)
		call(pEndPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		return 0
	case wmSize:
		call(pInvalidateRect, hwnd, 0, 0)
		a.dockResize()
		a.streamAreaChanged()
		return 0
	case wmMove: // a game window floating over this one moves with it
		a.dockResize()
		a.streamAreaChanged()
		return 0
	case wmGetMinMaxInfo:
		mm := *(**minMaxInfo)(unsafe.Pointer(&lp)) // lp is a pointer Windows gave us
		mm.MinTrackSize = point{240, 200}
		return 0
	case wmCommand:
		a.command(loword(wp) & 0xffff)
		return 0
	case wmInitMenuPopup:
		if wp == a.recentMenu {
			a.fillRecent()
		}
		if wp == a.seatMenu {
			a.fillSeats()
		}
		if wp == a.dipMenu {
			a.fillDips()
		}
		a.uiPopup(wp)
		a.updateMenus()
		return 0
	case wmCopyData:
		if a.copyDataOpen(lp) {
			return 1
		}
	case wmDropFiles:
		buf := make([]uint16, 32768)
		n := call(pDragQueryFileW, wp, 0, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
		call(pDragFinish, wp)
		if n > 0 {
			call(pSetForegroundWindow, hwnd)
			a.uiDrop(syscall.UTF16ToString(buf[:n]))
		}
		return 0
	case wmKeyDown, wmSysKeyDown:
		if a.keyDown(uint32(m), int(wp), lp) {
			return 0
		}
	case wmKeyUp, wmSysKeyUp:
		if a.keyUp(int(wp)) && uint32(m) == wmKeyUp {
			return 0
		}
	case wmActivateApp:
		if wp == 0 && a.sess != nil {
			a.sess.clearKeys()
		}
	case wmKillFocus:
		if a.sess != nil {
			a.sess.clearKeys()
		}
	case wmSetCursor:
		if a.sess == nil && lp&0xffff == 1 && a.homeCursor() {
			return 1
		}
		if a.fullscreen && lp&0xffff == 1 { // over the picture in fullscreen: no pointer
			call(pSetCursor, 0)
			return 1
		}
	case wmMouseMove:
		if a.sess == nil {
			a.homeMouseMove(loword(lp), hiword(lp))
		}
		a.mouseMove(loword(lp), hiword(lp))
	case wmMouseLeave:
		a.mouseTracking = false
		a.homeMouseLeave()
		if a.sess != nil {
			a.sess.mouseX.Store(math.Float32bits(float32(math.NaN())))
		}
	case wmLButtonDown, wmLButtonUp:
		if a.sess == nil && uint32(m) == wmLButtonDown {
			a.homeClick(loword(lp), hiword(lp))
		}
		if a.sess != nil && a.settings.MousePaddle {
			a.sess.mouseFire.Store(uint32(m) == wmLButtonDown)
		}
	case wmTimer:
		if wp == timerTitle {
			a.updateTitle()
			return 0
		}
		if a.uiTimer(wp) {
			return 0
		}
	case wmAppEvent:
		a.handleEvents()
		return 0
	case wmDpiChanged:
		r := *(**winRect)(unsafe.Pointer(&lp))
		call(pSetWindowPos, hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top), swpNoZOrder|swpNoActivate)
		return 0
	case wmClose:
		if a.mameBeforeClose() || a.supermodelBeforeClose() || a.pixeluBeforeClose() || a.dolBeforeClose() {
			return 0
		}
		a.streamStop(false)
		a.closeGame()
		saveSettings(a.settings)
		call(pDestroyWindow, hwnd)
		return 0
	case wmDestroy:
		call(pPostQuitMessage, 0)
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}

// ---------------- keyboard and mouse ----------------

func (a *app) keyDown(m uint32, vk int, lp uintptr) bool {
	repeat := lp&(1<<30) != 0
	alt := lp&(1<<29) != 0
	if (vk == vkReturn && alt) || vk == vkF11 {
		if !repeat {
			a.toggleFullscreen()
		}
		return true
	}
	if vk == vkEscape && a.fullscreen {
		a.toggleFullscreen()
		return true
	}
	if alt {
		return false // Alt+F4, Alt+letter for the menus
	}
	if keyDown(vkControl) {
		if !repeat {
			switch vk {
			case vkF9:
				a.streamToggle()
				return true
			case 'O':
				a.command(cmdOpen)
			case 'Z':
				a.command(cmdUndoLoad)
			case 'R':
				a.command(cmdReset)
			}
		}
		return true
	}
	if vk == vkF10 {
		return false // the menu key
	}
	if a.uiKeyDown(vk, repeat) {
		return true
	}
	if !repeat {
		switch vk {
		case vkF2:
			a.command(cmdSaveState)
			return true
		case vkF4:
			a.command(cmdLoadState)
			return true
		case vkF9:
			a.command(cmdRecord)
			return true
		case 'P':
			a.command(cmdPause)
			return true
		case 'M':
			a.command(cmdMute)
			return true
		}
	}
	s := a.sess
	if s == nil {
		return m == wmKeyDown
	}
	name := keyName(vk)
	if vk == vkTab || (name == " " && !s.usesKey(" ")) {
		s.holdFast.Store(true)
	}
	if name != "" {
		s.setKey(name, true)
	}
	return true
}

func (a *app) keyUp(vk int) bool {
	s := a.sess
	if s == nil {
		return false
	}
	name := keyName(vk)
	if vk == vkTab || name == " " {
		s.holdFast.Store(false)
	}
	if name != "" {
		s.setKey(name, false)
	}
	return vk != vkMenu && vk != vkF10
}

func (a *app) mouseMove(x, y int) {
	s := a.sess
	if s == nil || !a.settings.MousePaddle {
		return
	}
	if !a.mouseTracking {
		t := trackMouseEvent{Flags: 2, Track: a.hwnd} // TME_LEAVE
		t.Size = uint32(unsafe.Sizeof(t))
		call(pTrackMouseEvent, uintptr(unsafe.Pointer(&t)))
		a.mouseTracking = true
	}
	cr := clientRect(a.hwnd)
	a.frameMu.Lock()
	r := fitRect(int(cr.Right), int(cr.Bottom), a.aspect)
	a.frameMu.Unlock()
	if r.W <= 0 {
		return
	}
	v := float32(x-r.X)/float32(r.W)*2 - 1
	if v < -1 {
		v = -1
	}
	if v > 1 {
		v = 1
	}
	s.mouseX.Store(math.Float32bits(v))
}

// ---------------- drawing ----------------

// present is called on the game's thread with each new picture.
func (a *app) present(pix []uint32, w, h int) {
	a.frameMu.Lock()
	defer a.frameMu.Unlock()
	if len(a.frame) != len(pix) {
		a.frame = make([]uint32, len(pix))
	}
	copy(a.frame, pix)
	a.fw, a.fh = w, h
	hdc := call(pGetDC, a.hwnd)
	if hdc != 0 {
		a.drawLocked(hdc)
		call(pReleaseDC, a.hwnd, hdc)
	}
}

func (a *app) paint(hdc uintptr) {
	a.frameMu.Lock()
	defer a.frameMu.Unlock()
	if a.frame == nil {
		if !a.mamePaint(hdc) && !a.supermodelPaint(hdc) && !a.pixeluPaint(hdc) && !a.dolPaint(hdc) {
			a.drawIdle(hdc)
		}
		return
	}
	a.drawLocked(hdc)
}

// drawLocked draws the picture, keeping its shape, with black bars round it.
// Holds frameMu.
func (a *app) drawLocked(hdc uintptr) {
	cr := clientRect(a.hwnd)
	cw, ch := int(cr.Right), int(cr.Bottom)
	if cw <= 0 || ch <= 0 || a.fw <= 0 || a.fh <= 0 {
		return
	}
	r := fitRect(cw, ch, a.aspect)
	bar := func(x, y, w, h int) {
		if w > 0 && h > 0 {
			call(pPatBlt, hdc, uintptr(x), uintptr(y), uintptr(w), uintptr(h), blackness)
		}
	}
	bar(0, 0, cw, r.Y)
	bar(0, r.Y+r.H, cw, ch-r.Y-r.H)
	bar(0, r.Y, r.X, r.H)
	bar(r.X+r.W, r.Y, cw-r.X-r.W, r.H)
	mode := uintptr(colorOnColor)
	if a.smooth {
		mode = halftone
	}
	call(pSetStretchBltMode, hdc, mode)
	if a.smooth {
		call(pSetBrushOrgEx, hdc, 0, 0, 0)
	}
	a.bmi = bitmapInfoHeader{Width: int32(a.fw), Height: -int32(a.fh), Planes: 1, BitCount: 32}
	a.bmi.Size = uint32(unsafe.Sizeof(a.bmi))
	call(pStretchDIBits, hdc, uintptr(r.X), uintptr(r.Y), uintptr(r.W), uintptr(r.H), 0, 0, uintptr(a.fw), uintptr(a.fh),
		uintptr(unsafe.Pointer(&a.frame[0])), uintptr(unsafe.Pointer(&a.bmi)), dibRGBColors, srcCopy)
}

// drawIdle: the start screen (ui_home_windows.go).
func (a *app) drawIdle(hdc uintptr) { a.drawHome(hdc) }

// ---------------- window size and fullscreen ----------------

// resizeToScale makes the picture area N times the game's own size.
func (a *app) resizeToScale() {
	if a.fullscreen {
		return
	}
	style := call(pGetWindowLongPtrW, a.hwnd, gwlStyleIdx)
	if style&0x01000000 != 0 { // maximised: leave it
		return
	}
	a.frameMu.Lock()
	h, aspect := a.fh, a.aspect // the picture last shown (the game's thread owns the core)
	a.frameMu.Unlock()
	if h <= 0 {
		h = 240
	}
	bw, bh := baseSize(h, aspect)
	w, hh := bw*a.settings.Scale, bh*a.settings.Scale
	for pass := 0; pass < 2; pass++ {
		wr, cr := windowRect(a.hwnd), clientRect(a.hwnd)
		fw := int(wr.Right-wr.Left) - int(cr.Right) + w
		fh := int(wr.Bottom-wr.Top) - int(cr.Bottom) + hh
		call(pSetWindowPos, a.hwnd, 0, 0, 0, uintptr(fw), uintptr(fh), swpNoMove|swpNoZOrder|swpNoActivate)
		cr = clientRect(a.hwnd)
		if int(cr.Right) == w && int(cr.Bottom) == hh {
			break
		}
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// toggleFullscreen: a borderless window covering the screen (OBS can still
// capture it, and Alt+Tab is instant).
func (a *app) toggleFullscreen() {
	gwl := gwlStyleIdx
	if !a.fullscreen {
		a.savedStyle = call(pGetWindowLongPtrW, a.hwnd, gwl)
		a.savedRect = windowRect(a.hwnd)
		mon := call(pMonitorFromWindow, a.hwnd, monitorDefaultToNearest)
		mi := monitorInfo{}
		mi.Size = uint32(unsafe.Sizeof(mi))
		call(pGetMonitorInfoW, mon, uintptr(unsafe.Pointer(&mi)))
		call(pSetMenu, a.hwnd, 0)
		call(pSetWindowLongPtrW, a.hwnd, gwl, (a.savedStyle&^wsOverlappedWindow)|wsPopup|wsVisible)
		r := mi.Monitor
		call(pSetWindowPos, a.hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top),
			swpFrameChanged|swpNoOwnerZOrder)
		a.fullscreen = true
	} else {
		call(pSetWindowLongPtrW, a.hwnd, gwl, a.savedStyle)
		call(pSetMenu, a.hwnd, a.menu)
		r := a.savedRect
		call(pSetWindowPos, a.hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top),
			swpFrameChanged|swpNoOwnerZOrder|swpNoZOrder)
		a.fullscreen = false
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// ---------------- title ----------------

func (a *app) baseTitle() string { return "RetroPixels Native " + version }

func (a *app) showMessage(text string) {
	a.flash, a.flashUntil = text, time.Now().Add(2500*time.Millisecond)
	a.updateTitle()
}

// updateTitle: the version, the game, and what's going on, in the title bar
// (not on the picture, so OBS and recordings show only the game).
func (a *app) updateTitle() {
	t := a.baseTitle()
	if s := a.sess; s != nil {
		t += " - " + s.game.Name + " (" + systemByID(s.game.System).Label + ")"
		if s.paused.Load() {
			t += " - Paused"
		} else if s.fast.Load() || s.holdFast.Load() {
			t += " - Fast forward"
		}
		if a.recOn {
			if err := s.recError(); err != nil {
				a.recOn = false
				a.finishRecording(s.stopRecording())
			} else {
				d := int(time.Since(a.recStart).Seconds())
				t += fmt.Sprintf(" - ● REC %d:%02d", d/60, d%60)
			}
		}
		t += a.apTitle()
	}
	t += a.dolTitle()
	t += a.mameTitle()
	t += a.pixeluTitle()
	t += a.supermodelTitle()
	t += a.streamTitle()
	if a.recSaving > 0 {
		t += " - Saving the recording…"
	}
	if a.ffDownload != "" {
		t += " - " + a.ffDownload
	}
	if a.flash != "" && time.Now().Before(a.flashUntil) {
		t += " - " + a.flash
	}
	if t != a.lastTitle {
		a.lastTitle = t
		setWindowText(a.hwnd, t)
	}
}

// ---------------- events from other threads ----------------

func (a *app) event(name, detail string) {
	a.evMu.Lock()
	a.events = append(a.events, appEvent{name, detail})
	a.evMu.Unlock()
	call(pPostMessageW, a.hwnd, wmAppEvent, 0, 0)
}

func (a *app) handleEvents() {
	a.evMu.Lock()
	evs := a.events
	a.events = nil
	a.evMu.Unlock()
	for _, e := range evs {
		switch e.name {
		case "failed":
			a.xin.stopRumble()
			messageBox(a.hwnd, "The game stopped: "+e.detail+"\n\nYou can load a save state (F4), reset (Ctrl+R) or open another game.",
				"RetroPixels Native", mbIconError)
		case "open": // a game double-clicked while this window was open
			a.openPath(e.detail)
		case "pad-save":
			a.command(cmdSaveState)
		case "pad-load":
			a.command(cmdLoadState)
		case "pad-record":
			a.command(cmdRecord)
		case "message":
			a.showMessage(e.detail)
		case "rec-done":
			a.recSaving--
			a.showMessage("Recording saved: " + filepath.Base(e.detail))
		case "rec-dropped": // too short to keep
			a.recSaving--
			a.updateTitle()
		case "rec-error":
			a.recSaving--
			a.updateTitle()
			messageBox(a.hwnd, "The recording couldn't be saved: "+e.detail, "RetroPixels Native", mbIconError)
		case "ff-progress":
			a.ffDownload = e.detail
			a.updateTitle()
		case "ff-done":
			a.ffDownload = ""
			if a.sess != nil && !a.recOn && a.settings.autoRecord() {
				a.autoRecordBuiltIn()
				a.showMessage("ffmpeg is ready: recording")
			} else {
				a.showMessage("ffmpeg is ready: press F9 to record")
			}
		case "ff-error":
			a.ffDownload = ""
			a.updateTitle()
			messageBox(a.hwnd, "ffmpeg couldn't be downloaded: "+e.detail, "RetroPixels Native", mbIconError)
		default:
			a.uiEvent(e.name, e.detail)
			a.dolEvent(e.name, e.detail)
			a.mameEvent(e.name, e.detail)
			a.pixeluEvent(e.name, e.detail)
			a.supermodelEvent(e.name, e.detail)
			a.apEvent(e.name, e.detail)
			a.streamEvent(e.name, e.detail)
		}
	}
}

// host interface for the session

func (a *app) pads() [4]PadState { return a.xin.read() }

func (a *app) rumble(pad int, strong, weak float32) {
	if !a.settings.RumbleOff {
		a.xin.rumble(pad, strong, weak)
	}
}

// ---------------- commands ----------------

func (a *app) command(id int) {
	s := a.sess
	switch {
	case id == cmdOpen:
		if p := a.openDialog(); p != "" {
			a.openPath(p)
		}
	case id == cmdFileTypes:
		a.chooseFileTypes()
	case id == cmdExit:
		call(pPostMessageW, a.hwnd, wmClose, 0, 0)
	case id == cmdCloseGame:
		a.closeGame()
		a.uiBackHome()
	case id >= cmdRecent0 && id < cmdRecent0+maxRecent:
		if i := id - cmdRecent0; i < len(a.settings.Recent) {
			a.openPath(a.settings.Recent[i])
		}
	case id == cmdClearRecent:
		a.settings.Recent = nil
		saveSettings(a.settings)
	case id == cmdPause && s != nil:
		p := !s.paused.Load()
		s.paused.Store(p)
		if p {
			s.snd.Clear()
			a.xin.stopRumble()
		}
		a.updateTitle()
	case id == cmdReset && s != nil:
		s.reset()
		a.showMessage("Reset")
	case id == cmdFast && s != nil:
		s.fast.Store(!s.fast.Load())
		a.updateTitle()
	case id == cmdSaveState && s != nil:
		a.saveToSlot()
	case id == cmdLoadState && s != nil:
		a.loadFromSlot()
	case id == cmdUndoLoad && s != nil:
		if err := s.undoLoad(); err != nil {
			a.showMessage("Nothing to undo")
		} else {
			a.showMessage("Back to before the load")
		}
	case id == cmdControls:
		a.openControls()
	case id > cmdVolume0 && id <= cmdVolume0+10:
		a.settings.Volume = (id - cmdVolume0) * 10
		a.settings.Muted = false
		a.applyVolume()
		a.showMessage(fmt.Sprintf("Volume %d%%", a.settings.Volume))
	case id == cmdMute:
		a.settings.Muted = !a.settings.Muted
		a.applyVolume()
		if a.settings.Muted {
			a.showMessage("Muted")
		} else {
			a.showMessage("Sound on")
		}
	case id >= cmdScale1 && id < cmdScale1+5:
		a.settings.Scale = id - cmdScale1 + 1
		saveSettings(a.settings)
		if a.fullscreen {
			a.toggleFullscreen()
		}
		a.resizeToScale()
	case id == cmdFullscreen:
		a.toggleFullscreen()
	case id == cmdSharp || id == cmdSmooth:
		a.settings.Smooth = id == cmdSmooth
		a.frameMu.Lock()
		a.smooth = a.settings.Smooth
		a.frameMu.Unlock()
		saveSettings(a.settings)
		call(pInvalidateRect, a.hwnd, 0, 0)
	case id == cmdMousePaddle:
		a.settings.MousePaddle = !a.settings.MousePaddle
		saveSettings(a.settings)
		if s != nil {
			s.mousePaddle.Store(a.settings.MousePaddle)
			s.mouseX.Store(math.Float32bits(float32(math.NaN())))
			s.mouseFire.Store(false)
		}
	case id == cmdRecord:
		a.toggleRecording()
	case id == cmdRecFolder:
		d := a.settings.recDir()
		os.MkdirAll(d, 0o755)
		shellOpen(d)
	case id >= cmdSeat0 && id < cmdSeat0+4:
		a.pickSeat(id - cmdSeat0)
	case id >= cmdDip0 && id < cmdDipEnd:
		a.dipCommand(id)
	case id >= cmdDolFirst && id <= cmdDolLast:
		a.dolCommand(id)
	case id >= cmdPixelUFirst && id <= cmdPixelULast:
		a.pixeluCommand(id)
	case id >= cmdMameFirst && id <= cmdMameLast:
		a.mameCommand(id)
	case id >= cmdStreamFirst && id <= cmdStreamLast:
		a.streamCommand(id)
	case id >= cmdSMFirst && id <= cmdSMLast:
		a.supermodelCommand(id)
	case id == cmdAPStatus || id == cmdAPHelp:
		a.apCommand(id)
	case id == cmdAbout:
		messageBox(a.hwnd, "RetroPixels Native "+version+"\n\n"+
			"A test build of RetroPixels without a web browser inside, so OBS Studio can capture it "+
			"(Window Capture or Game Capture).\n\n"+
			"Plays "+availableLabels()+" games with RetroPixels' own emulators. "+
			"For the other systems, keep using RetroPixels.exe for now.\n\n"+
			"Settings and saves: "+appDir()+"\nRecordings: "+a.settings.recDir()+"\nScreenshots: "+screenshotsDir(),
			"About RetroPixels Native", mbIconInfo)
	default:
		a.uiCommand(id)
	}
}

func (a *app) applyVolume() {
	saveSettings(a.settings)
	if a.sess != nil {
		a.sess.volume.Store(math.Float32bits(a.volume()))
	}
}

func (a *app) volume() float32 {
	if a.settings.Muted {
		return 0
	}
	return float32(a.settings.Volume) / 100
}

// ---------------- games ----------------

func (a *app) openPath(path string) {
	if a.mameOpenPath(path) {
		return
	}
	if a.supermodelOpenPath(path) { // swtrilgy.zip / swtrilgya.zip (MAME can't play it)
		return
	}
	if a.pixeluOpenPath(path) {
		return
	}
	if a.dolOpenPath(path) {
		return
	}
	if a.apOpenPath(path) {
		return
	}
	g, err := openGame(path)
	if err != nil && a.mameOfferArcade(path, err) {
		return // an arcade game RetroPixels' own emulators don't have: MAME can play it
	}
	if err != nil {
		icon := uint32(mbIconWarning)
		if _, ok := err.(errNotYet); ok {
			icon = mbIconInfo
		}
		messageBox(a.hwnd, err.Error(), "RetroPixels Native", icon)
		return
	}
	a.startGame(g, path)
}

// startGame plays a game opened from path.
func (a *app) startGame(g *loadedGame, path string) {
	a.closeGame()
	g.Dips = a.settings.Dips[g.Name]
	s, err := newSession(g, a, a.snd, a.settings.controlMap(g.System))
	// a cartridge chip whose program the player supplies: ask for the file
	// (once: it is kept in the firmware folder when the game accepts it)
	for err != nil {
		chip, file, msg, ok := needsFirmware(err)
		if !ok {
			break
		}
		if messageBox(a.hwnd, msg, "RetroPixels Native", mbOkCancel|mbIconInfo) != idOk {
			return
		}
		p := a.fileDialog("Choose the "+chip+" chip's program ("+file+")",
			"Chip programs (*.rom *.bin)\x00*.rom;*.bin\x00All files\x00*.*\x00\x00", a.settings.LastFolder)
		if p == "" {
			return
		}
		b, rerr := os.ReadFile(p)
		if rerr != nil {
			messageBox(a.hwnd, rerr.Error(), "RetroPixels Native", mbIconWarning)
			continue
		}
		if g.Firmware == nil {
			g.Firmware = map[string][]byte{}
		}
		g.Firmware[chip] = b
		if s, err = newSession(g, a, a.snd, a.settings.controlMap(g.System)); err == nil {
			if werr := saveFirmware(chip, b); werr != nil {
				messageBox(a.hwnd, "The chip program works, but couldn't be kept for next time: "+werr.Error(), "RetroPixels Native", mbIconWarning)
			}
		}
	}
	if err != nil {
		messageBox(a.hwnd, fmt.Sprintf("Couldn't start %q: %v", filepath.Base(path), err), "RetroPixels Native", mbIconWarning)
		return
	}
	s.volume.Store(math.Float32bits(a.volume()))
	s.mousePaddle.Store(a.settings.MousePaddle)
	a.frameMu.Lock()
	a.aspect = s.aspect
	a.frame = nil
	a.frameMu.Unlock()
	a.sess = s
	a.restoreSeat(s)
	a.settings.addRecent(path)
	a.settings.LastFolder = filepath.Dir(path)
	saveSettings(a.settings)
	s.present() // the first picture, and its size for the window
	a.resizeToScale()
	s.start()
	a.apGameStarted(s)
	a.autoRecordBuiltIn() // Record > Record games automatically
	a.updateTitle()
}

func (a *app) closeGame() {
	s := a.sess
	if s == nil {
		return
	}
	a.uiCloseGame()
	a.apGameClosed()
	a.sess = nil
	rec := s.stop()
	if rec != nil {
		a.recOn = false
		a.finishRecording(rec)
	}
	a.xin.stopRumble()
	a.frameMu.Lock()
	a.frame = nil
	a.frameMu.Unlock()
	call(pInvalidateRect, a.hwnd, 0, 0)
	a.updateTitle()
}

func (a *app) openDialog() string {
	exts := append(availableExts(), dolphinExts()...)
	exts = append(exts, ".ap*") // Archipelago patch files
	pat := "*" + strings.Join(exts, ";*")
	filter := "Games (" + strings.Join(exts, " ") + ")\x00" + pat + "\x00All files\x00*.*\x00\x00"
	return a.fileDialog("Open game", filter, a.settings.LastFolder)
}

// fileDialog: the Open dialog; "" when cancelled.
func (a *app) fileDialog(title, filter, dir string) string {
	buf := make([]uint16, 32768)
	o := openFileName{
		Owner:   a.hwnd,
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

// ---------------- recording ----------------

func (a *app) setEncoder(ff, enc string) {
	a.encMu.Lock()
	a.encoders[ff] = enc
	a.encMu.Unlock()
}

func (a *app) encoder(ff string) string {
	a.encMu.Lock()
	defer a.encMu.Unlock()
	if e, ok := a.encoders[ff]; ok {
		return e
	}
	return "libx264" // still testing the graphics card's encoder: the CPU one always works
}

func (a *app) toggleRecording() {
	if a.dolToggleRecording() || a.pixeluToggleRecording() || a.mameToggleRecording() || a.supermodelToggleRecording() {
		return
	}
	s := a.sess
	if a.recOn {
		a.recOn = false
		if s != nil {
			a.finishRecording(s.stopRecording())
		}
		a.updateTitle()
		return
	}
	if s == nil {
		a.showMessage("Start a game before recording")
		return
	}
	if a.ffDownload != "" {
		a.showMessage("Wait for ffmpeg to finish downloading")
		return
	}
	ff := ffmpeg.Find()
	if ff == "" {
		if messageBox(a.hwnd, "Recording needs ffmpeg, a free video program (about 200 MB, from github.com/BtbN/FFmpeg-Builds; "+
			"RetroPixels.exe uses the same one).\n\nDownload it now?", "RetroPixels Native", mbYesNo|mbIconInfo) == idYes {
			a.downloadFFmpeg()
		}
		return
	}
	if err := s.startRecordingTo(ff, a.encoder(ff), a.settings.recQuality(), a.settings.recDir()); err != nil {
		messageBox(a.hwnd, "Couldn't start recording: "+err.Error(), "RetroPixels Native", mbIconWarning)
		return
	}
	a.recOn, a.recStart = true, time.Now()
	a.updateTitle()
}

// finishRecording saves the .mp4 in the background.
func (a *app) finishRecording(r *recorder) {
	if r == nil {
		return
	}
	a.recSaving++
	a.recWG.Add(1)
	go func() {
		defer a.recWG.Done()
		path, err := r.stop()
		if errors.Is(err, errRecTooShort) {
			a.event("rec-dropped", "")
		} else if err != nil {
			a.event("rec-error", err.Error())
		} else {
			a.event("rec-done", path)
		}
	}()
	a.updateTitle()
}

func (a *app) downloadFFmpeg() {
	a.ffDownload = "Downloading ffmpeg…"
	a.updateTitle()
	go func() {
		ff, err := ffmpeg.Install(ffmpeg.Dirs(), func(got, total int64) {
			if total > 0 {
				a.event("ff-progress", fmt.Sprintf("Downloading ffmpeg %d%%", got*100/total))
			} else {
				a.event("ff-progress", fmt.Sprintf("Downloading ffmpeg (%d MB)", got>>20))
			}
		})
		if err != nil {
			a.event("ff-error", err.Error())
			return
		}
		a.setEncoder(ff, ffmpeg.PickEncoder(ff))
		a.event("ff-done", ff)
	}()
}
