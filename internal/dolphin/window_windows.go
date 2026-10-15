package dolphin

// Docking Dolphin's game screen inside another window, as RetroPixels.exe
// does (embed_windows.go): Dolphin is started as usual (batch mode, game
// screen in a separate window), and as soon as that window appears it's
// turned into a child window of the host, without a frame or taskbar button.
// Closing it ends the game the same way Dolphin's own close button does.

import (
	"os"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

var (
	modUser32                 = syscall.NewLazyDLL("user32.dll")
	pEnumWindows              = modUser32.NewProc("EnumWindows")
	pGetWindowThreadProcessId = modUser32.NewProc("GetWindowThreadProcessId")
	pIsWindowVisible          = modUser32.NewProc("IsWindowVisible")
	pIsIconic                 = modUser32.NewProc("IsIconic")
	pGetClassNameW            = modUser32.NewProc("GetClassNameW")
	pGetWindowRect            = modUser32.NewProc("GetWindowRect")
	pGetWindowLongPtrW        = modUser32.NewProc("GetWindowLongPtrW")
	pSetWindowLongPtrW        = modUser32.NewProc("SetWindowLongPtrW")
	pSetParent                = modUser32.NewProc("SetParent")
	pGetParent                = modUser32.NewProc("GetParent")
	pSetWindowPos             = modUser32.NewProc("SetWindowPos")
	pIsWindow                 = modUser32.NewProc("IsWindow")
	pPostMessageW             = modUser32.NewProc("PostMessageW")
	pMapWindowPoints          = modUser32.NewProc("MapWindowPoints")
	pAttachThreadInput        = modUser32.NewProc("AttachThreadInput")
	pSetFocus                 = modUser32.NewProc("SetFocus")
	pGetCurrentThreadId       = modKernel32.NewProc("GetCurrentThreadId")
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

	swpNoActivate      = 0x0010
	swpFrameChanged    = 0x0020
	swpShowWindow      = 0x0040
	swpAsyncWindowPos  = 0x4000
	wmClose            = 0x0010
	consoleWindowClass = "ConsoleWindowClass"
)

type rect struct{ L, T, R, B int32 }

var (
	enumMu    sync.Mutex
	enumPID   uint32
	enumAll   bool
	enumFound []uintptr
	enumCB    = syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		var pid uint32
		pGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
		if pid == enumPID {
			if v, _, _ := pIsWindowVisible.Call(hwnd); v != 0 || enumAll {
				enumFound = append(enumFound, hwnd)
			}
		}
		return 1
	})
)

// windowsOf lists a process's top-level windows (only the visible ones unless all).
func windowsOf(pid int, all bool) []uintptr {
	enumMu.Lock()
	defer enumMu.Unlock()
	enumPID, enumAll, enumFound = uint32(pid), all, nil
	pEnumWindows.Call(enumCB, 0)
	return enumFound
}

func className(hwnd uintptr) string {
	buf := make([]uint16, 256)
	n, _, _ := pGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return syscall.UTF16ToString(buf[:n])
}

// GameWindow returns Dolphin's largest visible top-level window. In batch
// mode with "render to main window" off, that's the game screen.
func GameWindow(pid int) uintptr {
	var best uintptr
	var bestArea int32
	for _, h := range windowsOf(pid, false) {
		if ic, _, _ := pIsIconic.Call(h); ic != 0 {
			continue
		}
		if className(h) == consoleWindowClass {
			continue
		}
		var r rect
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

// Dock turns child into a frameless child window of host.
func Dock(child, host uintptr) {
	style, _, _ := pGetWindowLongPtrW.Call(child, gwlStyle)
	style &^= wsPopup | wsCaption | wsThickFrame | wsSysMenu | wsMinimizeBox | wsMaximizeBox | wsMaximize
	style |= wsChild | wsVisible | wsClipSiblings
	pSetWindowLongPtrW.Call(child, gwlStyle, style)
	ex, _, _ := pGetWindowLongPtrW.Call(child, gwlExStyle)
	ex &^= wsExAppWindow | wsExTopmost | wsExDlgModalFrame | wsExWindowEdge | wsExClientEdge | wsExStaticEdge
	pSetWindowLongPtrW.Call(child, gwlExStyle, ex)
	pSetParent.Call(child, host)
}

// Docked tells whether child is still a live window docked in host.
func Docked(child, host uintptr) bool {
	if child == 0 {
		return false
	}
	alive, _, _ := pIsWindow.Call(child)
	parent, _, _ := pGetParent.Call(child)
	return alive != 0 && parent == host
}

// Place puts a docked window at x, y (in host's client area), w × h. Nothing
// happens when it's already there, unless force (which also makes Windows
// redraw its removed frame). It doesn't wait for Dolphin to move it, so a
// busy Dolphin can't hold up the caller.
func Place(child, host uintptr, x, y, w, h int32, force bool) {
	want := rect{x, y, x + w, y + h}
	var cur rect
	pGetWindowRect.Call(child, uintptr(unsafe.Pointer(&cur)))
	pMapWindowPoints.Call(0, host, uintptr(unsafe.Pointer(&cur)), 2)
	if !force && cur == want {
		return
	}
	flags := uintptr(swpNoActivate | swpShowWindow | swpAsyncWindowPos)
	if force {
		flags |= swpFrameChanged
	}
	pSetWindowPos.Call(child, 0 /* HWND_TOP */, uintptr(x), uintptr(y), uintptr(w), uintptr(h), flags)
}

// Focus gives the docked game screen the keyboard. (Controllers work either
// way: Dolphin is started with background input on.)
func Focus(child uintptr) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	me, _, _ := pGetCurrentThreadId.Call()
	them, _, _ := pGetWindowThreadProcessId.Call(child, 0)
	if r, _, _ := pAttachThreadInput.Call(me, them, 1); r != 0 {
		pSetFocus.Call(child)
		pAttachThreadInput.Call(me, them, 0)
	}
}

// Close asks one window to close (for the game screen: Stop game).
func Close(hwnd uintptr) {
	if hwnd != 0 {
		pPostMessageW.Call(hwnd, wmClose, 0, 0)
	}
}

// Shutdown asks every Dolphin window (its hidden main window too) to close,
// and ends the process if it's still there after wait.
func Shutdown(pid int, exited <-chan struct{}, wait time.Duration) {
	for _, h := range windowsOf(pid, true) {
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
