package main

// PixelDS's window on Windows: the two screens stacked, the keyboard, Xbox
// controllers, the mouse as the stylus, and the sound, through the Windows
// API (syscall, no cgo).

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"retropixels/cores/pixelds"
)

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	winmm    = syscall.NewLazyDLL("winmm.dll")

	pRegisterClassExW         = user32.NewProc("RegisterClassExW")
	pCreateWindowExW          = user32.NewProc("CreateWindowExW")
	pDefWindowProcW           = user32.NewProc("DefWindowProcW")
	pDestroyWindow            = user32.NewProc("DestroyWindow")
	pGetMessageW              = user32.NewProc("GetMessageW")
	pTranslateMessage         = user32.NewProc("TranslateMessage")
	pDispatchMessageW         = user32.NewProc("DispatchMessageW")
	pPostQuitMessage          = user32.NewProc("PostQuitMessage")
	pPostMessageW             = user32.NewProc("PostMessageW")
	pShowWindow               = user32.NewProc("ShowWindow")
	pLoadCursorW              = user32.NewProc("LoadCursorW")
	pLoadIconW                = user32.NewProc("LoadIconW")
	pGetClientRect            = user32.NewProc("GetClientRect")
	pGetWindowRect            = user32.NewProc("GetWindowRect")
	pSetWindowPos             = user32.NewProc("SetWindowPos")
	pAdjustWindowRectEx       = user32.NewProc("AdjustWindowRectEx")
	pSetWindowTextW           = user32.NewProc("SetWindowTextW")
	pBeginPaint               = user32.NewProc("BeginPaint")
	pEndPaint                 = user32.NewProc("EndPaint")
	pInvalidateRect           = user32.NewProc("InvalidateRect")
	pMessageBoxW              = user32.NewProc("MessageBoxW")
	pGetWindowLongPtrW        = user32.NewProc("GetWindowLongPtrW")
	pSetWindowLongPtrW        = user32.NewProc("SetWindowLongPtrW")
	pMonitorFromWindow        = user32.NewProc("MonitorFromWindow")
	pGetMonitorInfoW          = user32.NewProc("GetMonitorInfoW")
	pSetForegroundWindow      = user32.NewProc("SetForegroundWindow")
	pSetCapture               = user32.NewProc("SetCapture")
	pReleaseCapture           = user32.NewProc("ReleaseCapture")
	pGetKeyState              = user32.NewProc("GetKeyState")
	pSetProcessDpiAwarenessCx = user32.NewProc("SetProcessDpiAwarenessContext")
	pCreateMenu               = user32.NewProc("CreateMenu")
	pCreatePopupMenu          = user32.NewProc("CreatePopupMenu")
	pAppendMenuW              = user32.NewProc("AppendMenuW")
	pSetMenu                  = user32.NewProc("SetMenu")
	pCheckMenuItem            = user32.NewProc("CheckMenuItem")
	pModifyMenuW              = user32.NewProc("ModifyMenuW")
	pStretchDIBits            = gdi32.NewProc("StretchDIBits")
	pSetStretchBltMode        = gdi32.NewProc("SetStretchBltMode")
	pPatBlt                   = gdi32.NewProc("PatBlt")
	pGetModuleHandleW         = kernel32.NewProc("GetModuleHandleW")
	pTimeBeginPeriod          = winmm.NewProc("timeBeginPeriod")
	pWaveOutOpen              = winmm.NewProc("waveOutOpen")
	pWaveOutClose             = winmm.NewProc("waveOutClose")
	pWaveOutPrepareHeader     = winmm.NewProc("waveOutPrepareHeader")
	pWaveOutUnprepareHeader   = winmm.NewProc("waveOutUnprepareHeader")
	pWaveOutWrite             = winmm.NewProc("waveOutWrite")
	pWaveOutReset             = winmm.NewProc("waveOutReset")
)

const (
	wsOverlappedWindow = 0x00CF0000
	wsPopup            = 0x80000000
	wsVisible          = 0x10000000
	cwUseDefault       = 0x80000000

	wmDestroy     = 0x0002
	wmSize        = 0x0005
	wmPaint       = 0x000F
	wmClose       = 0x0010
	wmEraseBkgnd  = 0x0014
	wmKillFocus   = 0x0008
	wmKeyDown     = 0x0100
	wmKeyUp       = 0x0101
	wmSysKeyDown  = 0x0104
	wmSysKeyUp    = 0x0105
	wmMouseMove   = 0x0200
	wmLButtonDown = 0x0201
	wmLButtonUp   = 0x0202
	wmApp         = 0x8000
	wmCommand     = 0x0111

	mfString    = 0x0000
	mfPopup     = 0x0010
	mfSeparator = 0x0800
	mfChecked   = 0x0008
	mfByCommand = 0x0000

	swShow                  = 5
	swpNoZOrder             = 0x0004
	swpFrameChanged         = 0x0020
	swpNoOwnerZOrder        = 0x0200
	monitorDefaultToNearest = 2
	colorOnColor            = 3
	dibRGBColors            = 0
	srcCopy                 = 0x00CC0020
	blackness               = 0x00000042
	idcArrow                = 32512
	mbIconError             = 0x10
)

const gwlStyle = ^uintptr(15) // GWL_STYLE (-16)

type point struct{ X, Y int32 }
type winRect struct{ Left, Top, Right, Bottom int32 }
type msg struct {
	Hwnd    uintptr
	Message uint32
	WParam  uintptr
	LParam  uintptr
	Time    uint32
	Pt      point
	private uint32
}
type wndClassEx struct {
	Size       uint32
	Style      uint32
	WndProc    uintptr
	ClsExtra   int32
	WndExtra   int32
	Instance   uintptr
	Icon       uintptr
	Cursor     uintptr
	Background uintptr
	MenuName   *uint16
	ClassName  *uint16
	IconSm     uintptr
}
type paintStruct struct {
	Hdc       uintptr
	Erase     int32
	RcPaint   winRect
	Restore   int32
	IncUpdate int32
	Reserved  [32]byte
}
type bitmapInfoHeader struct {
	Size          uint32
	Width         int32
	Height        int32
	Planes        uint16
	BitCount      uint16
	Compression   uint32
	SizeImage     uint32
	XPelsPerMeter int32
	YPelsPerMeter int32
	ClrUsed       uint32
	ClrImportant  uint32
}
type monitorInfo struct {
	Size    uint32
	Monitor winRect
	Work    winRect
	Flags   uint32
}

func u16(s string) *uint16 {
	p, _ := syscall.UTF16PtrFromString(s)
	return p
}

func call(p *syscall.LazyProc, args ...uintptr) uintptr {
	r, _, _ := p.Call(args...)
	return r
}

func messageBox(hwnd uintptr, text, title string, flags uint32) {
	call(pMessageBoxW, hwnd, uintptr(unsafe.Pointer(u16(text))), uintptr(unsafe.Pointer(u16(title))), uintptr(flags))
}

// ---------------- sound ----------------

type waveFormatEx struct {
	FormatTag      uint16
	Channels       uint16
	SamplesPerSec  uint32
	AvgBytesPerSec uint32
	BlockAlign     uint16
	BitsPerSample  uint16
	CbSize         uint16
}

type waveHdr struct {
	Data          uintptr
	BufferLength  uint32
	BytesRecorded uint32
	User          uintptr
	Flags         uint32
	Loops         uint32
	Next          uintptr
	Reserved      uintptr
}

type waveBlock struct {
	hdr  waveHdr
	data []int16
}

type waveOut struct {
	h       uintptr
	playing []*waveBlock
}

func openWaveOut(rate int) *waveOut {
	f := waveFormatEx{FormatTag: 1, Channels: 2, SamplesPerSec: uint32(rate), AvgBytesPerSec: uint32(rate * 4), BlockAlign: 4, BitsPerSample: 16}
	var h uintptr
	if r, _, _ := pWaveOutOpen.Call(uintptr(unsafe.Pointer(&h)), 0xFFFFFFFF, uintptr(unsafe.Pointer(&f)), 0, 0, 0); r != 0 || h == 0 {
		return nil
	}
	return &waveOut{h: h}
}

// queued: stereo sample pairs not played yet.
func (w *waveOut) queued() int {
	keep := w.playing[:0]
	n := 0
	for _, b := range w.playing {
		if atomic.LoadUint32(&b.hdr.Flags)&1 != 0 {
			pWaveOutUnprepareHeader.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr))
			continue
		}
		keep = append(keep, b)
		n += len(b.data) / 2
	}
	w.playing = keep
	return n
}

func (w *waveOut) write(pcm []int16) {
	if len(pcm) < 2 || w.queued() > sampleRate/2 {
		return
	}
	b := &waveBlock{data: append([]int16(nil), pcm...)}
	b.hdr = waveHdr{Data: uintptr(unsafe.Pointer(&b.data[0])), BufferLength: uint32(len(b.data) * 2)}
	if r, _, _ := pWaveOutPrepareHeader.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr)); r != 0 {
		return
	}
	if r, _, _ := pWaveOutWrite.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr)); r != 0 {
		pWaveOutUnprepareHeader.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr))
		return
	}
	w.playing = append(w.playing, b)
}

func (w *waveOut) close() {
	pWaveOutReset.Call(w.h)
	w.queued()
	pWaveOutClose.Call(w.h)
}

// ---------------- Xbox controllers ----------------

type xinputState struct {
	Packet  uint32
	Buttons uint16
	LT, RT  uint8
	LX, LY  int16
	RX, RY  int16
}

type pads struct {
	get     *syscall.LazyProc
	lastTry [4]time.Time
	on      [4]bool
}

func newPads() *pads {
	for _, n := range []string{"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"} {
		d := syscall.NewLazyDLL(n)
		if d.Load() == nil {
			if g := d.NewProc("XInputGetState"); g.Find() == nil {
				return &pads{get: g}
			}
		}
	}
	return &pads{}
}

// read: the first connected controller.
func (p *pads) read() (xinputState, bool) {
	if p.get == nil {
		return xinputState{}, false
	}
	now := time.Now()
	for i := 0; i < 4; i++ {
		if !p.on[i] && now.Sub(p.lastTry[i]) < 2*time.Second {
			continue
		}
		var st xinputState
		if r, _, _ := p.get.Call(uintptr(i), uintptr(unsafe.Pointer(&st))); r != 0 {
			p.on[i] = false
			p.lastTry[i] = now
			continue
		}
		p.on[i] = true
		return st, true
	}
	return xinputState{}, false
}

// ---------------- the window ----------------

type app struct {
	hwnd uintptr
	path string

	mu      sync.Mutex
	emu     *pixelds.Emulator
	frame   []uint32
	fw, fh  int
	bmi     bitmapInfoHeader
	title   string
	flash   string
	flashAt time.Time
	pending string

	keys       atomic.Uint32
	mouseDown  atomic.Bool
	mouseX     atomic.Int32
	mouseY     atomic.Int32
	cmds       chan func(e *pixelds.Emulator)
	quit       chan struct{}
	done       chan struct{}
	fullscreen bool
	savedStyle uintptr
	savedRect  winRect
	slot       int
	menu       uintptr
	paused     atomic.Bool
}

// Menu commands.
const (
	cmdPause = 100 + iota
	cmdRestart
	cmdFullscreen
	cmdExit
	cmdSaveState
	cmdLoadState
	cmdSlot0 // cmdSlot0 + n: state slot n
)

const stateSlots = 10

// makeMenu: the menu bar, with save states and pause where they can be seen.
func (a *app) makeMenu() {
	bar := call(pCreateMenu)
	add := func(m uintptr, id int, text string) {
		call(pAppendMenuW, m, mfString, uintptr(id), uintptr(unsafe.Pointer(u16(text))))
	}
	sep := func(m uintptr) { call(pAppendMenuW, m, mfSeparator, 0, 0) }
	game := call(pCreatePopupMenu)
	add(game, cmdPause, "&Pause\tP")
	add(game, cmdRestart, "&Restart\tCtrl+R")
	sep(game)
	add(game, cmdFullscreen, "&Fullscreen\tF11")
	sep(game)
	add(game, cmdExit, "E&xit")
	st := call(pCreatePopupMenu)
	add(st, cmdSaveState, "&Save state\tF5")
	add(st, cmdLoadState, "&Load state\tF8")
	sep(st)
	for i := 0; i < stateSlots; i++ {
		add(st, cmdSlot0+i, fmt.Sprintf("Slot &%d", (i+1)%10))
	}
	call(pAppendMenuW, bar, mfPopup, game, uintptr(unsafe.Pointer(u16("&Game"))))
	call(pAppendMenuW, bar, mfPopup, st, uintptr(unsafe.Pointer(u16("&Save states"))))
	a.menu = bar
	call(pSetMenu, a.hwnd, bar)
	a.updateMenu()
}

// updateMenu ticks the pause item and the chosen slot, and names the slot
// in the save and load items.
func (a *app) updateMenu() {
	if a.menu == 0 {
		return
	}
	check := func(id int, on bool) {
		f := uintptr(mfByCommand)
		if on {
			f |= mfChecked
		}
		call(pCheckMenuItem, a.menu, uintptr(id), f)
	}
	check(cmdPause, a.paused.Load())
	for i := 0; i < stateSlots; i++ {
		check(cmdSlot0+i, i == a.slot)
	}
	call(pModifyMenuW, a.menu, cmdSaveState, mfByCommand|mfString, cmdSaveState,
		uintptr(unsafe.Pointer(u16(fmt.Sprintf("&Save state (slot %d)\tF5", a.slot+1)))))
	call(pModifyMenuW, a.menu, cmdLoadState, mfByCommand|mfString, cmdLoadState,
		uintptr(unsafe.Pointer(u16(fmt.Sprintf("&Load state (slot %d)\tF8", a.slot+1)))))
}

func (a *app) setSlot(n int) {
	a.slot = (n%stateSlots + stateSlots) % stateSlots
	a.updateMenu()
	a.say(fmt.Sprintf("state slot %d", a.slot+1))
}

// togglePause pauses or resumes the game: while paused nothing runs, so
// there is no sound and the recording leaves the pause out.
func (a *app) togglePause() {
	p := !a.paused.Load()
	a.paused.Store(p)
	a.keys.Store(0)
	a.updateMenu()
	if p {
		a.say("paused (P to resume)")
	} else {
		a.say("resumed")
	}
}

var theApp *app

var keyMap = map[uintptr]uint32{
	0x25: pixelds.BtnLeft, 0x26: pixelds.BtnUp, 0x27: pixelds.BtnRight, 0x28: pixelds.BtnDown,
	'X': pixelds.BtnA, 'Z': pixelds.BtnB, 'S': pixelds.BtnX, 'A': pixelds.BtnY,
	'Q': pixelds.BtnL, 'W': pixelds.BtnR, 0x0D: pixelds.BtnStart, 0x10: pixelds.BtnSelect, 0x08: pixelds.BtnSelect,
	'E': pixelds.BtnZL, 'R': pixelds.BtnZR,
	'I': dpadUp, 'K': dpadDown, 'J': dpadLeft, 'L': dpadRight,
}

// On the 3DS the arrow keys move the circle pad and I, J, K, L press the
// D-pad (on the DS they are the D-pad too).
const (
	dpadUp = 1 << (20 + iota)
	dpadDown
	dpadLeft
	dpadRight
)

// split3DS turns the keyboard's bits into the buttons and the circle pad.
func split3DS(bits uint32) (uint32, float64, float64) {
	var x, y float64
	if bits&pixelds.BtnLeft != 0 {
		x--
	}
	if bits&pixelds.BtnRight != 0 {
		x++
	}
	if bits&pixelds.BtnUp != 0 {
		y++
	}
	if bits&pixelds.BtnDown != 0 {
		y--
	}
	return bits &^ (pixelds.BtnLeft | pixelds.BtnRight | pixelds.BtnUp | pixelds.BtnDown), x, y
}

// dpadKeys: I, J, K, L as the D-pad.
func dpadKeys(bits uint32) uint32 {
	out := bits &^ (dpadUp | dpadDown | dpadLeft | dpadRight)
	for _, p := range [][2]uint32{{dpadUp, pixelds.BtnUp}, {dpadDown, pixelds.BtnDown}, {dpadLeft, pixelds.BtnLeft}, {dpadRight, pixelds.BtnRight}} {
		if bits&p[0] != 0 {
			out |= p[1]
		}
	}
	return out
}

func runWindow(path string) bool {
	runtime.LockOSThread()
	if pSetProcessDpiAwarenessCx.Find() == nil {
		call(pSetProcessDpiAwarenessCx, ^uintptr(3))
	}
	call(pTimeBeginPeriod, 1)
	if path == "" {
		messageBox(0, "PixelDS plays DS games, and decrypted 3DS games and homebrew as an experiment.\n\nStart a game from the 3DS / DS tab in RetroPixels, or drop a game file onto PixelDS.exe.", "PixelDS "+pixelds.Version, 0x40)
		return true
	}
	emu, err := open(path)
	if err != nil {
		messageBox(0, err.Error(), "PixelDS "+pixelds.Version, mbIconError)
		os.Exit(1)
	}
	a := &app{path: path, emu: emu, cmds: make(chan func(*pixelds.Emulator), 16), quit: make(chan struct{}), done: make(chan struct{})}
	a.fw, a.fh = emu.Width(), emu.Height()
	a.frame = make([]uint32, a.fw*a.fh)
	a.bmi = bitmapInfoHeader{Size: 40, Width: int32(a.fw), Height: -int32(a.fh), Planes: 1, BitCount: 32}
	a.title = emu.Title()
	a.mouseX.Store(-1)
	theApp = a

	inst := call(pGetModuleHandleW, 0)
	cls := wndClassEx{Size: uint32(unsafe.Sizeof(wndClassEx{})), WndProc: syscall.NewCallback(wndProc), Instance: inst,
		Cursor: call(pLoadCursorW, 0, idcArrow), Icon: call(pLoadIconW, inst, 1), ClassName: u16("PixelDS")}
	call(pRegisterClassExW, uintptr(unsafe.Pointer(&cls)))
	// the window: the screens at twice their size
	r := winRect{0, 0, int32(a.fw * 2), int32(a.fh * 2)}
	call(pAdjustWindowRectEx, uintptr(unsafe.Pointer(&r)), wsOverlappedWindow, 1, 0) // with the menu bar
	a.hwnd = call(pCreateWindowExW, 0, uintptr(unsafe.Pointer(u16("PixelDS"))), uintptr(unsafe.Pointer(u16("PixelDS"))),
		wsOverlappedWindow|wsVisible, cwUseDefault, cwUseDefault, uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top), 0, 0, inst, 0)
	a.makeMenu()
	call(pShowWindow, a.hwnd, swShow)
	call(pSetForegroundWindow, a.hwnd)
	if *fullscreen && !*windowed {
		a.toggleFullscreen()
	}
	a.updateTitle()
	go a.run()

	var m msg
	for {
		if r := call(pGetMessageW, uintptr(unsafe.Pointer(&m)), 0, 0, 0); r == 0 || int32(r) == -1 {
			break
		}
		call(pTranslateMessage, uintptr(unsafe.Pointer(&m)))
		call(pDispatchMessageW, uintptr(unsafe.Pointer(&m)))
	}
	return true
}

func (a *app) updateTitle() {
	t := "PixelDS " + pixelds.Version + " - " + a.title
	if a.paused.Load() {
		t += " - PAUSED"
	}
	if a.flash != "" && time.Since(a.flashAt) < 3*time.Second {
		t += " - " + a.flash
	} else {
		t += fmt.Sprintf(" - P pause, F5 save state, F8 load (slot %d, F6/F7 to change), F11 fullscreen", a.slot+1)
	}
	call(pSetWindowTextW, a.hwnd, uintptr(unsafe.Pointer(u16(t))))
}

func (a *app) say(s string) {
	a.flash, a.flashAt = s, time.Now()
	a.updateTitle()
}

// picture: where the two screens are drawn in the window.
func (a *app) picture() (x, y, w, h int) {
	var cr winRect
	call(pGetClientRect, a.hwnd, uintptr(unsafe.Pointer(&cr)))
	cw, ch := int(cr.Right), int(cr.Bottom)
	w, h = cw, cw*a.fh/a.fw
	if h > ch {
		h, w = ch, ch*a.fw/a.fh
	}
	return (cw - w) / 2, (ch - h) / 2, w, h
}

func (a *app) paint(hdc uintptr) {
	var cr winRect
	call(pGetClientRect, a.hwnd, uintptr(unsafe.Pointer(&cr)))
	x, y, w, h := a.picture()
	cw, ch := int(cr.Right), int(cr.Bottom)
	bar := func(x, y, w, h int) {
		if w > 0 && h > 0 {
			call(pPatBlt, hdc, uintptr(x), uintptr(y), uintptr(w), uintptr(h), blackness)
		}
	}
	bar(0, 0, cw, y)
	bar(0, y+h, cw, ch-y-h)
	bar(0, y, x, h)
	bar(x+w, y, cw-x-w, h)
	call(pSetStretchBltMode, hdc, colorOnColor)
	a.mu.Lock()
	call(pStretchDIBits, hdc, uintptr(x), uintptr(y), uintptr(w), uintptr(h), 0, 0, uintptr(a.fw), uintptr(a.fh),
		uintptr(unsafe.Pointer(&a.frame[0])), uintptr(unsafe.Pointer(&a.bmi)), dibRGBColors, srcCopy)
	a.mu.Unlock()
}

func (a *app) toggleFullscreen() {
	if !a.fullscreen {
		a.savedStyle = call(pGetWindowLongPtrW, a.hwnd, gwlStyle)
		call(pGetWindowRect, a.hwnd, uintptr(unsafe.Pointer(&a.savedRect)))
		mon := call(pMonitorFromWindow, a.hwnd, monitorDefaultToNearest)
		mi := monitorInfo{Size: uint32(unsafe.Sizeof(monitorInfo{}))}
		call(pGetMonitorInfoW, mon, uintptr(unsafe.Pointer(&mi)))
		call(pSetMenu, a.hwnd, 0)
		call(pSetWindowLongPtrW, a.hwnd, gwlStyle, a.savedStyle&^wsOverlappedWindow|wsPopup|wsVisible)
		r := mi.Monitor
		call(pSetWindowPos, a.hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top), swpFrameChanged|swpNoOwnerZOrder)
		a.fullscreen = true
	} else {
		call(pSetWindowLongPtrW, a.hwnd, gwlStyle, a.savedStyle)
		call(pSetMenu, a.hwnd, a.menu)
		r := a.savedRect
		call(pSetWindowPos, a.hwnd, 0, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top), swpFrameChanged|swpNoOwnerZOrder|swpNoZOrder)
		a.fullscreen = false
	}
	call(pInvalidateRect, a.hwnd, 0, 0)
}

// mouse: the stylus, on the bottom screen.
func (a *app) mouse(lp uintptr, down bool) {
	mx, my := int(int16(lp&0xffff)), int(int16(lp>>16&0xffff))
	x, y, w, h := a.picture()
	if w <= 0 || h <= 0 {
		return
	}
	sx := (mx-x)*a.fw/w - a.emu.BottomX()
	sy := (my-y)*a.fh/h - a.fh/2
	if down && (sy < 0 || sx < 0 || sx >= a.emu.BottomWidth()) && !a.mouseDown.Load() {
		return
	}
	a.mouseX.Store(int32(sx))
	a.mouseY.Store(int32(sy))
	a.mouseDown.Store(down)
}

func (a *app) stateCmd(save bool) {
	slot := a.slot
	p := statePath(a.path, a.title, slot)
	a.cmds <- func(e *pixelds.Emulator) {
		var msgText string
		if save {
			b, err := e.SaveState()
			if err == nil {
				os.MkdirAll(dirOf(p), 0o755)
				err = writeFileAtomic(p, b)
			}
			if err != nil {
				msgText = "couldn't save the state: " + err.Error()
			} else {
				msgText = fmt.Sprintf("saved state %d", slot+1)
			}
		} else {
			b, err := os.ReadFile(p)
			if err == nil {
				err = e.LoadState(b)
			}
			if err != nil {
				msgText = fmt.Sprintf("no state %d to load", slot+1)
			} else {
				msgText = fmt.Sprintf("loaded state %d", slot+1)
			}
		}
		a.mu.Lock()
		a.pending = msgText
		copy(a.frame, e.Screen()) // shows the loaded state even while paused
		a.mu.Unlock()
		call(pInvalidateRect, a.hwnd, 0, 0)
		call(pPostMessageW, a.hwnd, wmApp, 0, 1)
	}
}

func dirOf(p string) string {
	for i := len(p) - 1; i >= 0; i-- {
		if p[i] == '\\' || p[i] == '/' {
			return p[:i]
		}
	}
	return "."
}

func wndProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	switch m {
	case wmPaint:
		var ps paintStruct
		hdc := call(pBeginPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		if a != nil && a.hwnd == hwnd {
			a.paint(hdc)
		}
		call(pEndPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		return 0
	case wmEraseBkgnd:
		return 1
	case wmSize:
		call(pInvalidateRect, hwnd, 0, 0)
	case wmKeyDown, wmSysKeyDown:
		ctrl := int16(call(pGetKeyState, 0x11)) < 0
		switch {
		case wp == 0x7A || m == wmSysKeyDown && wp == 0x0D: // F11, Alt+Enter
			a.toggleFullscreen()
			return 0
		case wp == 0x1B: // Esc
			if a.fullscreen {
				a.toggleFullscreen()
			}
			return 0
		case wp == 0x74: // F5
			a.stateCmd(true)
			return 0
		case wp == 0x77: // F8
			a.stateCmd(false)
			return 0
		case wp == 0x75: // F6
			a.setSlot(a.slot - 1)
			return 0
		case wp == 0x76: // F7
			a.setSlot(a.slot + 1)
			return 0
		case wp == 'P' || wp == 0x13: // P, Pause
			a.togglePause()
			return 0
		case ctrl && wp == 'R':
			a.cmds <- nil // restart
			return 0
		}
		if b, ok := keyMap[wp]; ok {
			a.keys.Store(a.keys.Load() | b)
			return 0
		}
	case wmKeyUp, wmSysKeyUp:
		if b, ok := keyMap[wp]; ok {
			a.keys.Store(a.keys.Load() &^ b)
			return 0
		}
	case wmKillFocus:
		if a != nil {
			a.keys.Store(0)
			a.mouseDown.Store(false)
		}
	case wmLButtonDown:
		call(pSetCapture, hwnd)
		a.mouse(lp, true)
		return 0
	case wmMouseMove:
		if a.mouseDown.Load() {
			a.mouse(lp, true)
		}
		return 0
	case wmLButtonUp:
		call(pReleaseCapture)
		a.mouseDown.Store(false)
		return 0
	case wmCommand:
		switch id := int(wp & 0xffff); {
		case id == cmdPause:
			a.togglePause()
		case id == cmdRestart:
			a.cmds <- nil
		case id == cmdFullscreen:
			a.toggleFullscreen()
		case id == cmdExit:
			call(pPostMessageW, hwnd, wmClose, 0, 0)
		case id == cmdSaveState:
			a.stateCmd(true)
		case id == cmdLoadState:
			a.stateCmd(false)
		case id >= cmdSlot0 && id < cmdSlot0+stateSlots:
			a.setSlot(id - cmdSlot0)
		}
		return 0
	case wmApp:
		a.mu.Lock()
		p := a.pending
		a.pending = ""
		a.mu.Unlock()
		if p != "" {
			a.say(p)
		} else {
			a.updateTitle()
		}
		return 0
	case wmClose:
		close(a.quit)
		<-a.done
		call(pDestroyWindow, hwnd)
		return 0
	case wmDestroy:
		call(pPostQuitMessage, 0)
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}

// run is the game's thread: input, one frame, sound, picture, paced to
// the DS's 59.83 frames a second.
func (a *app) run() {
	defer close(a.done)
	runtime.LockOSThread()
	snd := openWaveOut(sampleRate)
	pd := newPads()
	e := a.emu
	frameDur := time.Duration(float64(time.Second) / e.FPS())
	next := time.Now()
	lastSave := time.Now()
	saveDirty := false
	cx, cy := float64(e.BottomWidth()/2), float64(e.TopHeight()/2) // the controller's stylus pointer
	errShown := false
	l3Held := false
	pcm := make([]int16, 0, 4096)
	rec := newRecorder(e)
	defer rec.stop(sampleRate)
	titleTick := time.Now()
	flush := func() {
		if saveDirty {
			if err := writeSave(a.path, e.Save()); err == nil {
				saveDirty = false
			}
			lastSave = time.Now()
		}
	}
	defer func() {
		flush()
		if snd != nil {
			snd.close()
		}
	}()
	for {
		select {
		case <-a.quit:
			return
		case f := <-a.cmds:
			if f == nil { // restart
				flush()
				if ne, err := open(a.path); err == nil {
					e = ne
					a.emu = ne
					errShown = false
				}
			} else {
				f(e)
			}
			continue
		default:
		}
		if a.paused.Load() {
			// paused: the game, its sound and the recording all wait; the
			// controller's left-stick click resumes
			if st, ok := pd.read(); ok {
				l3 := st.Buttons&0x0040 != 0
				if l3 && !l3Held {
					call(pPostMessageW, a.hwnd, wmCommand, cmdPause, 0)
				}
				l3Held = l3
			}
			time.Sleep(15 * time.Millisecond)
			next = time.Now()
			continue
		}
		bits := a.keys.Load()
		three := e.HasCirclePad()
		var padX, padY float64
		if three {
			bits, padX, padY = split3DS(bits)
		}
		bits = dpadKeys(bits)
		touchDown, tx, ty := a.mouseDown.Load(), int(a.mouseX.Load()), int(a.mouseY.Load())
		if st, ok := pd.read(); ok {
			b := st.Buttons
			// the left-stick click pauses
			l3 := b&0x0040 != 0
			if l3 && !l3Held {
				call(pPostMessageW, a.hwnd, wmCommand, cmdPause, 0)
			}
			l3Held = l3
			m := map[uint16]uint32{0x1000: pixelds.BtnB, 0x2000: pixelds.BtnA, 0x4000: pixelds.BtnY, 0x8000: pixelds.BtnX,
				0x0100: pixelds.BtnL, 0x0200: pixelds.BtnR, 0x0010: pixelds.BtnStart, 0x0020: pixelds.BtnSelect,
				0x0001: pixelds.BtnUp, 0x0002: pixelds.BtnDown, 0x0004: pixelds.BtnLeft, 0x0008: pixelds.BtnRight}
			for k, v := range m {
				if b&k != 0 {
					bits |= v
				}
			}
			const dead = 12000
			if three {
				// the left stick is the circle pad; the triggers ZL and ZR
				if math.Abs(float64(st.LX)) > 4000 || math.Abs(float64(st.LY)) > 4000 {
					padX, padY = float64(st.LX)/32767, float64(st.LY)/32767
				}
				if st.LT > 100 {
					bits |= pixelds.BtnZL
				}
				if st.RT > 100 {
					bits |= pixelds.BtnZR
				}
			} else {
				// DS: the left stick is the D-pad too, and the triggers L and R
				if st.LX < -dead {
					bits |= pixelds.BtnLeft
				}
				if st.LX > dead {
					bits |= pixelds.BtnRight
				}
				if st.LY > dead {
					bits |= pixelds.BtnUp
				}
				if st.LY < -dead {
					bits |= pixelds.BtnDown
				}
				if st.LT > 100 {
					bits |= pixelds.BtnL
				}
			}
			// the right stick moves a stylus pointer; RT or R3 touches
			rx, ry := float64(st.RX)/32767, -float64(st.RY)/32767
			if math.Abs(rx) > 0.25 {
				cx += rx * 4
			}
			if math.Abs(ry) > 0.25 {
				cy += ry * 4
			}
			cx, cy = math.Max(0, math.Min(float64(e.BottomWidth()-1), cx)), math.Max(0, math.Min(float64(e.TopHeight()-1), cy))
			if (!three && st.RT > 100) || b&0x0080 != 0 {
				touchDown, tx, ty = true, int(cx), int(cy)
			}
		}
		e.SetButtons(bits)
		e.SetCirclePad(padX, padY)
		e.Touch(tx, ty, touchDown)
		e.RunFrame()
		if err := e.Err(); err != nil && !errShown {
			errShown = true
			go messageBox(a.hwnd, "The game stopped: "+err.Error()+"\n\nPixelDS's 3DS side is still being built, so some programs need parts it doesn't have yet.", "PixelDS "+pixelds.Version, mbIconError)
		}
		au := e.TakeAudio()
		rec.frame(e.Screen(), au, e.FPS(), sampleRate)
		if snd != nil {
			pcm = pcm[:0]
			for _, s := range au {
				pcm = append(pcm, int16(s*32767))
			}
			snd.write(pcm)
		}
		a.mu.Lock()
		copy(a.frame, e.Screen())
		a.mu.Unlock()
		call(pInvalidateRect, a.hwnd, 0, 0)
		if e.SaveChanged() {
			saveDirty = true
		}
		if saveDirty && time.Since(lastSave) > 2*time.Second {
			flush()
		}
		if time.Since(titleTick) > time.Second {
			titleTick = time.Now()
			call(pPostMessageW, a.hwnd, wmApp, 0, 0)
		}
		// pacing: the sound queue when there's sound, else the clock (a 3DS
		// game makes no sound until it starts its sound processor)
		next = next.Add(frameDur)
		if snd != nil && len(au) > 0 {
			for snd.queued() > sampleRate/12 {
				time.Sleep(2 * time.Millisecond)
			}
			next = time.Now()
		} else if d := time.Until(next); d > 0 {
			time.Sleep(d)
		} else if d < -250*time.Millisecond {
			next = time.Now()
		}
	}
}
