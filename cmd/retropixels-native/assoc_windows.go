package main

// Game files opened from File Explorer.
//
// Windows only offers a program for a file type once the program has said it
// opens that type. Each start writes that, per user (HKCU, no admin rights),
// with this exe's path, so a newly unzipped build takes over from the old one.
// It never makes RetroPixels the default by itself (Windows doesn't allow
// that); the player picks it once in "Open with" or Options > Open game files
// with RetroPixels Native.
//
// A game double-clicked while RetroPixels Native is already open is handed to
// that window instead of starting a second copy.

import (
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"syscall"
	"unsafe"
)

const (
	assocProgID = "RetroPixels.Native.Game"
	mainClass   = "RetroPixelsNative"
	wmCopyData  = 0x004a
	copyOpen    = 0x52504f50 // "RPOP": a game path to open
)

var (
	advapi32          = syscall.NewLazyDLL("advapi32.dll")
	pRegCreateKeyExW  = advapi32.NewProc("RegCreateKeyExW")
	pRegSetValueExW   = advapi32.NewProc("RegSetValueExW")
	pRegQueryValueExW = advapi32.NewProc("RegQueryValueExW")
	pRegCloseKey      = advapi32.NewProc("RegCloseKey")
	pSHChangeNotify   = shell32.NewProc("SHChangeNotify")
	pFindWindowW      = user32.NewProc("FindWindowW")
	pIsIconic         = user32.NewProc("IsIconic")
)

const (
	hkeyCurrentUser = 0x80000001
	keyAllAccess    = 0xf003f
	regNone         = 0
	regSz           = 1
)

type copyDataStruct struct {
	Data  uintptr
	Bytes uint32
	Ptr   uintptr
}

// assocExts: every file ending RetroPixels Native opens directly.
func assocExts() []string {
	seen := map[string]bool{}
	var out []string
	add := func(e string) {
		e = strings.ToLower(e)
		if !strings.HasPrefix(e, ".") || strings.ContainsAny(e, "*?") || seen[e] {
			return
		}
		seen[e] = true
		out = append(out, e)
	}
	for _, e := range availableExts() {
		add(e)
	}
	for _, e := range dolphinExts() {
		add(e)
	}
	for _, e := range []string{".wua", ".wud", ".wux", ".rpx"} { // Wii U (PixelU)
		add(e)
	}
	sort.Strings(out)
	return out
}

func regKey(path string) (uintptr, bool) {
	var k uintptr
	r := call(pRegCreateKeyExW, hkeyCurrentUser, uintptr(unsafe.Pointer(u16(path))), 0, 0, 0, keyAllAccess, 0,
		uintptr(unsafe.Pointer(&k)), 0)
	return k, r == 0
}

func regSetString(path, name, value string) {
	k, ok := regKey(path)
	if !ok {
		return
	}
	defer call(pRegCloseKey, k)
	v, _ := syscall.UTF16FromString(value)
	var np uintptr
	if name != "" {
		np = uintptr(unsafe.Pointer(u16(name)))
	}
	call(pRegSetValueExW, k, np, 0, regSz, uintptr(unsafe.Pointer(&v[0])), uintptr(len(v)*2))
}

func regSetEmpty(path, name string) {
	k, ok := regKey(path)
	if !ok {
		return
	}
	defer call(pRegCloseKey, k)
	call(pRegSetValueExW, k, uintptr(unsafe.Pointer(u16(name))), 0, regNone, 0, 0)
}

func regGetString(path string) string {
	k, ok := regKey(path)
	if !ok {
		return ""
	}
	defer call(pRegCloseKey, k)
	buf := make([]uint16, 1024)
	n := uint32(len(buf) * 2)
	var typ uint32
	if call(pRegQueryValueExW, k, 0, 0, uintptr(unsafe.Pointer(&typ)), uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(&n))) != 0 || typ != regSz {
		return ""
	}
	return syscall.UTF16ToString(buf)
}

// registerFileTypes tells Windows this exe opens game files. It only writes
// when something changed (a new build folder, or new file types); force
// writes anyway.
func registerFileTypes(force bool) {
	exe, err := os.Executable()
	if err != nil {
		return
	}
	if p, err := filepath.EvalSymlinks(exe); err == nil {
		exe = p
	}
	cmd := `"` + exe + `" "%1"`
	exts := assocExts()
	const cls = `Software\Classes\`
	stamp := strings.Join(exts, " ")
	if !force && regGetString(cls+assocProgID+`\shell\open\command`) == cmd && regGetString(cls+assocProgID+`\RetroPixelsTypes`) == stamp {
		return
	}
	icon := `"` + exe + `",0`
	app := cls + `Applications\` + filepath.Base(exe)

	regSetString(cls+assocProgID, "", "RetroPixels game")
	regSetString(cls+assocProgID, "FriendlyTypeName", "RetroPixels game")
	regSetString(cls+assocProgID+`\DefaultIcon`, "", icon)
	regSetString(cls+assocProgID+`\shell\open`, "FriendlyAppName", "RetroPixels Native")
	regSetString(cls+assocProgID+`\shell\open\command`, "", cmd)
	regSetString(cls+assocProgID+`\RetroPixelsTypes`, "", stamp)

	regSetString(app, "FriendlyAppName", "RetroPixels Native")
	regSetString(app+`\DefaultIcon`, "", icon)
	regSetString(app+`\shell\open`, "FriendlyAppName", "RetroPixels Native")
	regSetString(app+`\shell\open\command`, "", cmd)
	for _, e := range exts {
		regSetEmpty(app+`\SupportedTypes`, e)
		regSetEmpty(cls+e+`\OpenWithProgids`, assocProgID)
	}
	call(pSHChangeNotify, 0x08000000, 0, 0, 0) // SHCNE_ASSOCCHANGED
}

// chooseFileTypes: Options > Open game files with RetroPixels Native.
func (a *app) chooseFileTypes() {
	registerFileTypes(true)
	messageBox(a.hwnd, "RetroPixels Native is now offered for game files.\n\n"+
		"To make double-clicking a game open it here: right-click a game in File Explorer, choose Open with > Choose another app, "+
		"pick RetroPixels Native and tick \"Always\". Do this once for each kind of game file (.md, .sfc, .nes and so on).\n\n"+
		"New RetroPixels builds take over automatically when you first start them.",
		"RetroPixels Native", mbIconInfo)
}

// handToRunningCopy: when RetroPixels Native is already open, give it the game
// and report true (this copy then quits).
func handToRunningCopy(path string) bool {
	w := call(pFindWindowW, uintptr(unsafe.Pointer(u16(mainClass))), 0)
	if w == 0 {
		return false
	}
	if abs, err := filepath.Abs(path); err == nil {
		path = abs
	}
	s, _ := syscall.UTF16FromString(path)
	cd := copyDataStruct{Data: copyOpen, Bytes: uint32(len(s) * 2), Ptr: uintptr(unsafe.Pointer(&s[0]))}
	r := call(pSendMessageW, w, wmCopyData, 0, uintptr(unsafe.Pointer(&cd)))
	runtime.KeepAlive(s)
	if r != 1 {
		return false
	}
	// this copy was just started by Explorer, so it may bring the window up
	if call(pIsIconic, w) != 0 {
		call(pShowWindow, w, 9) // SW_RESTORE
	}
	call(pSetForegroundWindow, w)
	return true
}

// copyDataOpen: the WM_COPYDATA handler. The path is opened after the
// message returns, so the sending copy isn't kept waiting on any dialog.
func (a *app) copyDataOpen(lp uintptr) bool {
	cd := *(**copyDataStruct)(unsafe.Pointer(&lp))
	if cd == nil || cd.Data != copyOpen || cd.Bytes < 2 || cd.Bytes > 65536 {
		return false
	}
	s := unsafe.Slice(*(**uint16)(unsafe.Pointer(&cd.Ptr)), cd.Bytes/2)
	a.event("open", syscall.UTF16ToString(s))
	return true
}
