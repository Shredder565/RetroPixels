//go:build windows

package main

// The RetroPixels picture on the app window (title bar, taskbar button and
// Alt+Tab). The picture itself is stored in RetroPixels.exe (winres/), which
// is also what Explorer and the desktop show for the program.
//
// The window belongs to Edge, and the taskbar groups windows by their app
// ID: Edge gives its app windows its own, so the taskbar button showed the
// Edge icon whatever icon the window had. RetroPixels gives the window an
// app ID of its own, with RetroPixels.exe as its icon and relaunch command,
// so the taskbar shows RetroPixels.

import (
	"os"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

var (
	modShell32                   = syscall.NewLazyDLL("shell32.dll")
	pExtractIconExW              = modShell32.NewProc("ExtractIconExW")
	pSHGetPropertyStoreForWindow = modShell32.NewProc("SHGetPropertyStoreForWindow")
	pSendMessageW                = modUser32.NewProc("SendMessageW")
)

// appUserModelID is the taskbar's name for RetroPixels' windows.
const appUserModelID = "Shredder565.RetroPixels"

var (
	iidIPropertyStore = mkGUID(0x886D8EEB, 0x8CF2, 0x4446, 0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99)
	fmtidAppUserModel = mkGUID(0x9F4C2855, 0x9F79, 0x4B39, 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3)
)

type propertyKey struct {
	fmtid guid
	pid   uint32
}

// propVariant is a PROPVARIANT holding a string (VT_LPWSTR).
type propVariant struct {
	vt       uint16
	_        [3]uint16
	val      uintptr
	reserved uintptr
}

// setWindowAppID gives a window RetroPixels' app ID, icon and relaunch
// command (IPropertyStore on the window, as Windows' taskbar reads them).
func setWindowAppID(hwnd uintptr, exe string) bool {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if hr, _, _ := pCoInitializeEx.Call(0, 2); !failed(hr) { // COINIT_APARTMENTTHREADED
		defer pCoUninitialize.Call()
	}
	var store uintptr
	if hr, _, _ := pSHGetPropertyStoreForWindow.Call(hwnd, uintptr(unsafe.Pointer(&iidIPropertyStore)), uintptr(unsafe.Pointer(&store))); failed(hr) || store == 0 {
		return false
	}
	defer comRelease(store)
	set := func(pid uint32, v string) bool {
		p, err := syscall.UTF16PtrFromString(v)
		if err != nil {
			return false
		}
		key := propertyKey{fmtidAppUserModel, pid}
		pv := propVariant{vt: 31, val: uintptr(unsafe.Pointer(p))}                           // VT_LPWSTR
		hr := comCall(store, 6, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&pv))) // SetValue
		runtime.KeepAlive(p)
		return !failed(hr)
	}
	// The relaunch details go first: the taskbar reads them when the ID is set.
	set(2, `"`+exe+`"`)          // PKEY_AppUserModel_RelaunchCommand
	set(3, exe+",0")             // PKEY_AppUserModel_RelaunchIconResource
	set(4, appName)              // PKEY_AppUserModel_RelaunchDisplayNameResource
	ok := set(5, appUserModelID) // PKEY_AppUserModel_ID
	comCall(store, 7)            // Commit
	return ok
}

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
		var named uintptr
		for end := time.Now().Add(20 * time.Second); time.Now().Before(end); time.Sleep(time.Second) {
			if h := findHostWindow(); h != 0 {
				if h != named && setWindowAppID(h, exe) {
					named = h
				}
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
