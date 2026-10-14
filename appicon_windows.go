//go:build windows

package main

// The RetroPixels picture on the app window (title bar, taskbar button and
// Alt+Tab). The picture itself is stored in RetroPixels.exe (winres/), which
// is also what Explorer and the desktop show for the program.

import (
	"os"
	"syscall"
	"time"
	"unsafe"
)

var (
	modShell32      = syscall.NewLazyDLL("shell32.dll")
	pExtractIconExW = modShell32.NewProc("ExtractIconExW")
	pSendMessageW   = modUser32.NewProc("SendMessageW")
)

const (
	wmSetIcon = 0x0080
	iconSmall = 0
	iconBig   = 1
)

// setAppWindowIcon gives the RetroPixels window the program's own icon once
// it appears. The page's favicon is the same picture; this covers the moment
// before the page has loaded and browsers that ignore it.
func setAppWindowIcon() {
	go func() {
		exe, err := os.Executable()
		if err != nil {
			return
		}
		p, err := syscall.UTF16PtrFromString(exe)
		if err != nil {
			return
		}
		var big, small uintptr
		if n, _, _ := pExtractIconExW.Call(uintptr(unsafe.Pointer(p)), 0,
			uintptr(unsafe.Pointer(&big)), uintptr(unsafe.Pointer(&small)), 1); n == 0 {
			return
		}
		// Keep it for the first while: the browser sets its own icon as the window opens.
		for end := time.Now().Add(20 * time.Second); time.Now().Before(end); time.Sleep(time.Second) {
			if h := findHostWindow(); h != 0 {
				if big != 0 {
					pSendMessageW.Call(h, wmSetIcon, iconBig, big)
				}
				if small != 0 {
					pSendMessageW.Call(h, wmSetIcon, iconSmall, small)
				}
			}
		}
	}()
}
