package main

// The window in RetroPixels' dark colours, like the page RetroPixels.exe
// used to show: a dark title bar (Windows 10 2004 and newer), dark menus,
// and a menu bar painted here in the start screen's colours.

import (
	"syscall"
	"unsafe"
)

var (
	dwmapi                  = syscall.NewLazyDLL("dwmapi.dll")
	uxtheme                 = syscall.NewLazyDLL("uxtheme.dll")
	pDwmSetWindowAttribute  = dwmapi.NewProc("DwmSetWindowAttribute")
	pSetMenuInfo            = user32.NewProc("SetMenuInfo")
	pSetMenuItemInfoW       = user32.NewProc("SetMenuItemInfoW")
	pGetMenuItemInfoW       = user32.NewProc("GetMenuItemInfoW")
	pGetMenuBarInfo         = user32.NewProc("GetMenuBarInfo")
	pGetMenu                = user32.NewProc("GetMenu")
	pGetWindowDC            = user32.NewProc("GetWindowDC")
	darkMenuLabels          []string // the menu bar's names, by position
	darkMenuBrush           uintptr
	darkModeReady, darkMode bool
)

const (
	wmDrawItem    = 0x002B
	wmMeasureItem = 0x002C
	wmMenuChar    = 0x0120
	wmNcPaint     = 0x0085
	wmNcActivate  = 0x0086
	odtMenu       = 1
)

type menuItemInfo struct {
	Size, Mask, Type, State, ID uint32
	SubMenu, Checked, Unchecked uintptr
	Data                        uintptr
	TypeData                    *uint16
	Cch                         uint32
	Item                        uintptr
}

type drawItemStruct struct {
	CtlType, CtlID, ItemID, ItemAction, ItemState uint32
	Item, DC                                      uintptr
	Rect                                          winRect
	Data                                          uintptr
}

type measureItemStruct struct {
	CtlType, CtlID, ItemID, Width, Height uint32
	Data                                  uintptr
}

// darkStart: before the window is made, menus are asked to be dark
// (uxtheme's SetPreferredAppMode, ordinal 135, Windows 10 1903 and newer).
func darkStart() {
	if darkModeReady {
		return
	}
	darkModeReady = true
	if uxtheme.Load() != nil {
		return
	}
	setMode, err := syscall.GetProcAddress(syscall.Handle(uxtheme.Handle()), "#135")
	if err != nil || setMode == 0 {
		return
	}
	syscall.SyscallN(setMode, 2) // ForceDark
	if flush, err := syscall.GetProcAddress(syscall.Handle(uxtheme.Handle()), "#136"); err == nil && flush != 0 {
		syscall.SyscallN(flush) // FlushMenuThemes
	}
	darkMode = true
}

// darkWindow: the title bar and the menu bar of a window made dark.
func darkWindow(hwnd, menu uintptr) {
	on := int32(1)
	if pDwmSetWindowAttribute.Find() == nil {
		// DWMWA_USE_IMMERSIVE_DARK_MODE: 20, or 19 before Windows 10 2004
		if call(pDwmSetWindowAttribute, hwnd, 20, uintptr(unsafe.Pointer(&on)), 4) != 0 {
			call(pDwmSetWindowAttribute, hwnd, 19, uintptr(unsafe.Pointer(&on)), 4)
		}
	}
	if menu == 0 {
		return
	}
	if darkMenuBrush == 0 {
		darkMenuBrush = call(pCreateSolidBrush, colBg)
	}
	mi := struct {
		Size, Mask, Style, Max uint32
		Back                   uintptr
		Help                   uint32
		MenuData               uintptr
	}{Mask: 2 /* MIM_BACKGROUND */, Back: darkMenuBrush}
	mi.Size = uint32(unsafe.Sizeof(mi))
	call(pSetMenuInfo, menu, uintptr(unsafe.Pointer(&mi)))
	// each name on the bar painted here (owner-drawn), its menu kept
	n := int(call(pGetMenuItemCount, menu))
	darkMenuLabels = make([]string, n)
	for i := 0; i < n; i++ {
		buf := make([]uint16, 128)
		it := menuItemInfo{Mask: 0x40 /* MIIM_STRING */, TypeData: &buf[0], Cch: uint32(len(buf))}
		it.Size = uint32(unsafe.Sizeof(it))
		call(pGetMenuItemInfoW, menu, uintptr(i), 1, uintptr(unsafe.Pointer(&it)))
		darkMenuLabels[i] = syscall.UTF16ToString(buf)
		set := menuItemInfo{Mask: 0x100 | 0x20 /* MIIM_FTYPE | MIIM_DATA */, Type: 0x100 /* MFT_OWNERDRAW */, Data: uintptr(i + 1)}
		set.Size = uint32(unsafe.Sizeof(set))
		call(pSetMenuItemInfoW, menu, uintptr(i), 1, uintptr(unsafe.Pointer(&set)))
	}
	call(pDrawMenuBar, hwnd)
}

func menuLabel(data uintptr) (string, bool) {
	i := int(data) - 1
	if i < 0 || i >= len(darkMenuLabels) {
		return "", false
	}
	return darkMenuLabels[i], true
}

// the names are shown without their & (Alt and the letter still open them:
// darkMenuChar)
func (a *app) darkMeasure(lp uintptr) bool {
	m := *(**measureItemStruct)(unsafe.Pointer(&lp))
	if m.CtlType != odtMenu {
		return false
	}
	label, ok := menuLabel(m.Data)
	if !ok {
		return false
	}
	dc := call(pGetDC, a.hwnd)
	w := textWidth(dc, a.ui.small, stripAmp(label))
	call(pReleaseDC, a.hwnd, dc)
	m.Width = uint32(w + a.sc(10))
	m.Height = uint32(a.sc(20))
	return true
}

func (a *app) darkDraw(lp uintptr) bool {
	d := *(**drawItemStruct)(unsafe.Pointer(&lp))
	if d.CtlType != odtMenu {
		return false
	}
	label, ok := menuLabel(d.Data)
	if !ok {
		return false
	}
	bg, fg := uint32(colBg), uint32(colText)
	if d.ItemState&(1|0x40) != 0 { // ODS_SELECTED, ODS_HOTLIGHT
		bg = colPanel2
	}
	if d.ItemState&(0x80|4) != 0 { // ODS_INACTIVE, ODS_GRAYED
		fg = colMuted
	}
	fillColor(d.DC, d.Rect, bg)
	call(pSetBkMode, d.DC, transparent)
	paintText(d.DC, a.ui.small, stripAmp(label), d.Rect, fg, dtCenter|dtSingleLine|dtVCenter)
	return true
}

// darkMenuChar: Alt and a menu's letter opens it (owner-drawn names have no
// letter of their own for Windows).
func (a *app) darkMenuChar(wp, lp uintptr) (uintptr, bool) {
	if lp != a.menu {
		return 0, false
	}
	ch := []rune(string(rune(wp & 0xffff)))[0]
	for i, l := range darkMenuLabels {
		r := []rune(l)
		for j := 0; j+1 < len(r); j++ {
			if r[j] == '&' && lower(r[j+1]) == lower(ch) {
				return uintptr(i) | 2<<16, true // MNC_EXECUTE
			}
		}
	}
	return 0, false
}

func lower(r rune) rune {
	if r >= 'A' && r <= 'Z' {
		return r + 32
	}
	return r
}

func stripAmp(s string) string {
	out := []rune{}
	r := []rune(s)
	for i := 0; i < len(r); i++ {
		if r[i] == '&' && i+1 < len(r) {
			i++
		}
		out = append(out, r[i])
	}
	return string(out)
}

// darkMenuLine paints over the light line Windows draws under the menu bar.
func (a *app) darkMenuLine() {
	if call(pGetMenu, a.hwnd) == 0 {
		return
	}
	mbi := struct {
		Size      uint32
		Bar       winRect
		Menu, Wnd uintptr
		Focus     int32
	}{}
	mbi.Size = uint32(unsafe.Sizeof(mbi))
	if call(pGetMenuBarInfo, a.hwnd, ^uintptr(2) /* OBJID_MENU = -3 */, 0, uintptr(unsafe.Pointer(&mbi))) == 0 {
		return
	}
	wr := windowRect(a.hwnd)
	r := mbi.Bar
	r.Left, r.Right = r.Left-wr.Left, r.Right-wr.Left
	r.Top = r.Bottom - wr.Top
	r.Bottom = r.Top + 1
	dc := call(pGetWindowDC, a.hwnd)
	fillColor(dc, r, colBg)
	call(pReleaseDC, a.hwnd, dc)
}
