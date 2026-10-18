//go:build windows

package main

// Docking Dolphin's game screen inside the RetroPixels window.
//
// Dolphin is started as usual (its own install, batch mode, game screen in a
// separate window). As soon as that window appears it is turned into a child
// window of the RetroPixels app window, without a frame or taskbar button,
// and kept sized to the page below RetroPixels' game bar. Closing it (Stop
// game, or closing RetroPixels) ends the game the same way Dolphin's own
// close button does.

import (
	"errors"
	"os"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

var (
	pEnumChildWindows   = modUser32.NewProc("EnumChildWindows")
	pGetWindowLongPtrW  = modUser32.NewProc("GetWindowLongPtrW")
	pSetWindowLongPtrW  = modUser32.NewProc("SetWindowLongPtrW")
	pSetParent          = modUser32.NewProc("SetParent")
	pGetParent          = modUser32.NewProc("GetParent")
	pGetWindow          = modUser32.NewProc("GetWindow")
	pSetWindowPos       = modUser32.NewProc("SetWindowPos")
	pIsWindow           = modUser32.NewProc("IsWindow")
	pPostMessageW       = modUser32.NewProc("PostMessageW")
	pGetWindowRect      = modUser32.NewProc("GetWindowRect")
	pMapWindowPoints    = modUser32.NewProc("MapWindowPoints")
	pGetForeground      = modUser32.NewProc("GetForegroundWindow")
	pSetForeground      = modUser32.NewProc("SetForegroundWindow")
	pAttachThreadInput  = modUser32.NewProc("AttachThreadInput")
	pSetFocus           = modUser32.NewProc("SetFocus")
	pGetCurrentThreadId = modKernel.NewProc("GetCurrentThreadId")
)

const (
	gwlStyle   = ^uintptr(15) // GWL_STYLE (-16)
	gwlExStyle = ^uintptr(19) // GWL_EXSTYLE (-20)

	wsChild        = 0x40000000
	wsPopup        = 0x80000000
	wsVisible      = 0x10000000
	wsClipSiblings = 0x04000000
	wsMaximize     = 0x01000000
	wsCaption      = 0x00C00000
	wsSysMenu      = 0x00080000
	wsThickFrame   = 0x00040000
	wsMinimizeBox  = 0x00020000
	wsMaximizeBox  = 0x00010000

	wsExDlgModalFrame = 0x00000001
	wsExTopmost       = 0x00000008
	wsExWindowEdge    = 0x00000100
	wsExClientEdge    = 0x00000200
	wsExStaticEdge    = 0x00020000
	wsExAppWindow     = 0x00040000
	wsExToolWindow    = 0x00000080

	gwlpHwndParent = ^uintptr(7) // GWLP_HWNDPARENT (-8): a top-level window's owner
	gwOwner        = 4

	swpNoSize        = 0x0001
	swpNoMove        = 0x0002
	swpNoZOrder      = 0x0004
	swpNoOwnerZOrder = 0x0200
	swpNoActivate    = 0x0010
	swpFrameChanged  = 0x0020
	swpShowWindow    = 0x0040
	gwChild          = 5
	wmClose          = 0x0010
	chromeFrameClass = "Chrome_WidgetWin_1"
	chromePageClass  = "Chrome_RenderWidgetHostHWND"
)

var (
	embedMu     sync.Mutex
	embedPID    int
	embedHWND   uintptr   // Dolphin's game screen, while docked
	embedTopPx  int       // screen pixels the page's game bar takes at the top
	embedStopAt time.Time // when Stop game was clicked
)

func embeddedWindow(pid int) uintptr {
	embedMu.Lock()
	defer embedMu.Unlock()
	if pid != embedPID || embedHWND == 0 {
		return 0
	}
	if ok, _, _ := pIsWindow.Call(embedHWND); ok == 0 {
		return 0
	}
	return embedHWND
}

func setEmbedTop(px int) {
	if px < 0 || px > 400 {
		return
	}
	embedMu.Lock()
	embedTopPx = px
	embedMu.Unlock()
}

func embedTop() int {
	embedMu.Lock()
	defer embedMu.Unlock()
	return embedTopPx
}

// ---------------- finding windows ----------------

var (
	hostMu    sync.Mutex
	hostFound []uintptr
	hostCB    = syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		if v, _, _ := pIsWindowVisible.Call(hwnd); v != 0 &&
			windowText(pGetClassNameW, hwnd) == chromeFrameClass &&
			windowText(pGetWindowTextW, hwnd) == appName {
			hostFound = append(hostFound, hwnd)
		}
		return 1
	})
	pageMu   sync.Mutex
	pageBest uintptr
	pageArea int32
	pageCB   = syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		if v, _, _ := pIsWindowVisible.Call(hwnd); v != 0 && windowText(pGetClassNameW, hwnd) == chromePageClass {
			var r winRECT
			pGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&r)))
			if a := (r.R - r.L) * (r.B - r.T); a > pageArea {
				pageBest, pageArea = hwnd, a
			}
		}
		return 1
	})
)

// findHostWindow returns the RetroPixels app window (the one in front, if
// more than one is open).
func findHostWindow() uintptr {
	hostMu.Lock()
	hostFound = nil
	pEnumWindows.Call(hostCB, 0)
	found := hostFound
	hostMu.Unlock()
	if len(found) == 0 {
		return 0
	}
	fg, _, _ := pGetForeground.Call()
	for _, h := range found {
		if h == fg {
			return h
		}
	}
	return found[0]
}

// pageRect returns where the web page is drawn inside the host window, in
// the host's client coordinates.
func pageRect(host uintptr) (winRECT, bool) {
	pageMu.Lock()
	pageBest, pageArea = 0, 0
	pEnumChildWindows.Call(host, pageCB, 0)
	page := pageBest
	pageMu.Unlock()
	var r winRECT
	if page == 0 {
		pGetClientRect.Call(host, uintptr(unsafe.Pointer(&r)))
		return r, r.R > 0 && r.B > 0
	}
	pGetWindowRect.Call(page, uintptr(unsafe.Pointer(&r)))
	pMapWindowPoints.Call(0, host, uintptr(unsafe.Pointer(&r)), 2)
	return r, r.R > r.L && r.B > r.T
}

// dolphinGameScreen returns Dolphin's largest visible top-level window. In
// batch mode with "render to main window" off that is the game screen.
func dolphinGameScreen(pid int) uintptr {
	var best uintptr
	var bestArea int32
	for _, h := range topWindows(pid) {
		if ic, _, _ := pIsIconic.Call(h); ic != 0 {
			continue
		}
		if windowText(pGetClassNameW, h) == "ConsoleWindowClass" {
			continue
		}
		var r winRECT
		pGetWindowRect.Call(h, uintptr(unsafe.Pointer(&r)))
		if a := (r.R - r.L) * (r.B - r.T); a > bestArea {
			best, bestArea = h, a
		}
	}
	if bestArea < 100*100 {
		return 0
	}
	return best
}

// ---------------- docking ----------------

func dock(child, host uintptr) {
	style, _, _ := pGetWindowLongPtrW.Call(child, gwlStyle)
	style &^= wsPopup | wsCaption | wsThickFrame | wsSysMenu | wsMinimizeBox | wsMaximizeBox | wsMaximize
	style |= wsChild | wsVisible | wsClipSiblings
	pSetWindowLongPtrW.Call(child, gwlStyle, style)
	ex, _, _ := pGetWindowLongPtrW.Call(child, gwlExStyle)
	ex &^= wsExAppWindow | wsExTopmost | wsExDlgModalFrame | wsExWindowEdge | wsExClientEdge | wsExStaticEdge
	pSetWindowLongPtrW.Call(child, gwlExStyle, ex)
	pSetParent.Call(child, host)
	place(child, host, true)
	pSetForeground.Call(host)
	focusChild(child)
}

// place sizes the game screen to the page area below the game bar and keeps
// it in front of the page. force also makes Windows redraw its (removed) frame.
func place(child, host uintptr, force bool) {
	pr, ok := pageRect(host)
	if !ok {
		return
	}
	top := pr.T + int32(embedTop())
	if top > pr.B-50 {
		top = pr.B - 50
	}
	want := winRECT{pr.L, top, pr.R, pr.B}
	var cur winRECT
	pGetWindowRect.Call(child, uintptr(unsafe.Pointer(&cur)))
	pMapWindowPoints.Call(0, host, uintptr(unsafe.Pointer(&cur)), 2)
	onTop, _, _ := pGetWindow.Call(host, gwChild)
	if !force && cur == want && onTop == child {
		return
	}
	flags := uintptr(swpNoActivate | swpShowWindow)
	if force {
		flags |= swpFrameChanged
	}
	pSetWindowPos.Call(child, 0 /* HWND_TOP */, uintptr(want.L), uintptr(want.T),
		uintptr(want.R-want.L), uintptr(want.B-want.T), flags)
}

// focusChild gives the game screen the keyboard. (Controllers work either
// way: Dolphin is started with background input on.)
func focusChild(child uintptr) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	me, _, _ := pGetCurrentThreadId.Call()
	them, _, _ := pGetWindowThreadProcessId.Call(child, 0)
	if r, _, _ := pAttachThreadInput.Call(me, them, 1); r != 0 {
		pSetFocus.Call(child)
		pAttachThreadInput.Call(me, them, 0)
	}
}

// ---------------- docking as an owned window (PixelU's Wii U picture) ----------------
//
// PixelU draws the Wii U picture with Vulkan. The RetroPixels window is an
// Edge/Chrome app window that draws itself with DirectComposition and has no
// redirection surface (WS_EX_NOREDIRECTIONBITMAP), and a Vulkan picture in a
// child window of it never shows: the page shows through instead. So PixelU's
// window isn't made a child: it stays a frameless top-level window, owned by
// the RetroPixels window (so it stays in front of it, hides when it's
// minimized and has no taskbar button), and is kept over the page area.

func dockOverlay(child, host uintptr) {
	style, _, _ := pGetWindowLongPtrW.Call(child, gwlStyle)
	style &^= wsChild | wsCaption | wsThickFrame | wsSysMenu | wsMinimizeBox | wsMaximizeBox | wsMaximize
	style |= wsPopup | wsVisible | wsClipSiblings
	pSetWindowLongPtrW.Call(child, gwlStyle, style)
	ex, _, _ := pGetWindowLongPtrW.Call(child, gwlExStyle)
	ex &^= wsExAppWindow | wsExTopmost | wsExDlgModalFrame | wsExWindowEdge | wsExClientEdge | wsExStaticEdge
	ex |= wsExToolWindow
	pSetWindowLongPtrW.Call(child, gwlExStyle, ex)
	pSetWindowLongPtrW.Call(child, gwlpHwndParent, host) // owner, not parent
	placeOverlay(child, host, true)
	focusOverlay(child)
}

// placeOverlay keeps the game screen over the page area below the game bar,
// in screen coordinates (it follows the RetroPixels window when it moves).
func placeOverlay(child, host uintptr, force bool) {
	if ic, _, _ := pIsIconic.Call(host); ic != 0 {
		return // minimized: Windows hides owned windows with it
	}
	pr, ok := pageRect(host)
	if !ok {
		return
	}
	top := pr.T + int32(embedTop())
	if top > pr.B-50 {
		top = pr.B - 50
	}
	want := winRECT{pr.L, top, pr.R, pr.B}
	pMapWindowPoints.Call(host, 0, uintptr(unsafe.Pointer(&want)), 2)
	var cur winRECT
	pGetWindowRect.Call(child, uintptr(unsafe.Pointer(&cur)))
	if !force && cur == want {
		return
	}
	flags := uintptr(swpNoActivate | swpNoZOrder | swpNoOwnerZOrder)
	if force {
		flags |= swpFrameChanged | swpShowWindow
	}
	pSetWindowPos.Call(child, 0, uintptr(want.L), uintptr(want.T),
		uintptr(want.R-want.L), uintptr(want.B-want.T), flags)
}

// focusOverlay brings the game screen to the front with the keyboard. The
// RetroPixels window is in front when a game is started, so its thread is
// joined for a moment to be allowed to do that.
func focusOverlay(child uintptr) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	me, _, _ := pGetCurrentThreadId.Call()
	fg, _, _ := pGetForeground.Call()
	if fg == 0 {
		pSetForeground.Call(child)
		return
	}
	them, _, _ := pGetWindowThreadProcessId.Call(fg, 0)
	attached := false
	if them != me {
		r, _, _ := pAttachThreadInput.Call(me, them, 1)
		attached = r != 0
	}
	pSetForeground.Call(child)
	if attached {
		pAttachThreadInput.Call(me, them, 0)
	}
}

// isDocked says whether child is still docked to host (as a child window, or
// as an owned window).
func isDocked(child, host uintptr, overlay bool) bool {
	if alive, _, _ := pIsWindow.Call(child); alive == 0 {
		return false
	}
	if overlay {
		owner, _, _ := pGetWindow.Call(child, gwOwner)
		style, _, _ := pGetWindowLongPtrW.Call(child, gwlStyle)
		return owner == host && style&wsChild == 0
	}
	parent, _, _ := pGetParent.Call(child)
	return parent == host
}

// embedPixelU follows one PixelU (Wii U) run like embedDolphin, keeping its
// game screen over the page as an owned window (see dockOverlay).
func embedPixelU(pid int, exited <-chan struct{}) { embedWindow(pid, exited, true) }

// embedDolphin follows one Dolphin run: docks its game screen into the
// RetroPixels window, keeps it there, and makes sure Dolphin closes
// completely once the game is stopped so the next game can start.
func embedDolphin(pid int, exited <-chan struct{}) { embedWindow(pid, exited, false) }

func embedWindow(pid int, exited <-chan struct{}, overlay bool) {
	host := findHostWindow()
	if host == 0 {
		return // no RetroPixels window (e.g. opened in a normal browser): Dolphin stays in its own window
	}
	embedMu.Lock()
	embedPID, embedHWND, embedStopAt = pid, 0, time.Time{}
	embedMu.Unlock()
	defer func() {
		embedMu.Lock()
		if embedPID == pid {
			embedPID, embedHWND, embedStopAt = 0, 0, time.Time{}
		}
		embedMu.Unlock()
	}()

	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	deadline := time.Now().Add(90 * time.Second) // slow disc / shader compile on first boot
	var child uintptr
	var lostAt time.Time // when the docked game screen went away
	for {
		select {
		case <-exited:
			return
		case <-tick.C:
		}
		if ok, _, _ := pIsWindow.Call(host); ok == 0 {
			// RetroPixels closed: end the game too.
			if child != 0 {
				pPostMessageW.Call(child, wmClose, 0, 0)
			}
			shutdownDolphin(pid, exited, 10*time.Second)
			return
		}
		if child != 0 {
			if isDocked(child, host, overlay) {
				embedMu.Lock()
				stopAt := embedStopAt
				embedMu.Unlock()
				if !stopAt.IsZero() && time.Since(stopAt) > 8*time.Second {
					// Stop game was clicked but Dolphin didn't react.
					shutdownDolphin(pid, exited, 3*time.Second)
					return
				}
				if overlay {
					placeOverlay(child, host, false)
				} else {
					place(child, host, false)
				}
				continue
			}
			// The game screen went away: the game was stopped, or Dolphin
			// made a new one (e.g. it toggled fullscreen) which gets docked below.
			child, lostAt = 0, time.Now()
		}
		h := dolphinGameScreen(pid)
		if h == 0 {
			if !lostAt.IsZero() && time.Since(lostAt) > 3*time.Second {
				// The game ended but Dolphin is still running in the
				// background: close it so a new game can start.
				shutdownDolphin(pid, exited, 5*time.Second)
				return
			}
			if lostAt.IsZero() && time.Now().After(deadline) {
				return // never showed a game screen: leave Dolphin as it is
			}
			continue
		}
		if overlay {
			dockOverlay(h, host)
		} else {
			dock(h, host)
		}
		child, lostAt = h, time.Time{}
		embedMu.Lock()
		embedHWND = h
		embedMu.Unlock()
		runMu.Lock()
		if running != nil { // Dolphin or MAME: one game runs at a time
			running.Embedded = true
		}
		runMu.Unlock()
	}
}

// stopEmbeddedGame closes the docked game screen, which ends the game the
// same way Dolphin's close button does (memory cards are saved). embedDolphin
// then makes sure Dolphin itself closes.
func stopEmbeddedGame() error {
	embedMu.Lock()
	h := embedHWND
	if h != 0 {
		embedStopAt = time.Now()
	}
	embedMu.Unlock()
	if h == 0 {
		return errors.New("no game is playing inside RetroPixels")
	}
	pPostMessageW.Call(h, wmClose, 0, 0)
	return nil
}

var (
	allMu    sync.Mutex
	allPID   uint32
	allFound []uintptr
	allCB    = syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		var pid uint32
		pGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
		if pid == allPID {
			allFound = append(allFound, hwnd)
		}
		return 1
	})
)

// shutdownDolphin asks every Dolphin window (its hidden main window too) to
// close, and ends the process if it's still there after wait.
func shutdownDolphin(pid int, exited <-chan struct{}, wait time.Duration) {
	allMu.Lock()
	allPID, allFound = uint32(pid), nil
	pEnumWindows.Call(allCB, 0)
	found := allFound
	allMu.Unlock()
	for _, h := range found {
		pPostMessageW.Call(h, wmClose, 0, 0)
	}
	select {
	case <-exited:
	case <-time.After(wait):
		if p, err := os.FindProcess(pid); err == nil {
			_ = p.Kill()
		}
	}
}
