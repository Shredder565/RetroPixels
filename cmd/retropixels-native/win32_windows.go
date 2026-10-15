package main

// The parts of the Windows API the native app uses, called through syscall
// (no cgo).

import (
	"syscall"
	"unicode/utf16"
	"unsafe"
)

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	shell32  = syscall.NewLazyDLL("shell32.dll")
	comdlg32 = syscall.NewLazyDLL("comdlg32.dll")
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
	pSendMessageW             = user32.NewProc("SendMessageW")
	pShowWindow               = user32.NewProc("ShowWindow")
	pUpdateWindow             = user32.NewProc("UpdateWindow")
	pLoadCursorW              = user32.NewProc("LoadCursorW")
	pLoadImageW               = user32.NewProc("LoadImageW")
	pGetClientRect            = user32.NewProc("GetClientRect")
	pGetWindowRect            = user32.NewProc("GetWindowRect")
	pSetWindowPos             = user32.NewProc("SetWindowPos")
	pAdjustWindowRectEx       = user32.NewProc("AdjustWindowRectEx")
	pSetWindowTextW           = user32.NewProc("SetWindowTextW")
	pGetDC                    = user32.NewProc("GetDC")
	pReleaseDC                = user32.NewProc("ReleaseDC")
	pBeginPaint               = user32.NewProc("BeginPaint")
	pEndPaint                 = user32.NewProc("EndPaint")
	pInvalidateRect           = user32.NewProc("InvalidateRect")
	pCreateMenu               = user32.NewProc("CreateMenu")
	pCreatePopupMenu          = user32.NewProc("CreatePopupMenu")
	pAppendMenuW              = user32.NewProc("AppendMenuW")
	pDeleteMenu               = user32.NewProc("DeleteMenu")
	pGetMenuItemCount         = user32.NewProc("GetMenuItemCount")
	pModifyMenuW              = user32.NewProc("ModifyMenuW")
	pSetMenu                  = user32.NewProc("SetMenu")
	pCheckMenuItem            = user32.NewProc("CheckMenuItem")
	pCheckMenuRadioItem       = user32.NewProc("CheckMenuRadioItem")
	pEnableMenuItem           = user32.NewProc("EnableMenuItem")
	pDrawMenuBar              = user32.NewProc("DrawMenuBar")
	pMessageBoxW              = user32.NewProc("MessageBoxW")
	pGetWindowLongPtrW        = user32.NewProc("GetWindowLongPtrW")
	pSetWindowLongPtrW        = user32.NewProc("SetWindowLongPtrW")
	pMonitorFromWindow        = user32.NewProc("MonitorFromWindow")
	pGetMonitorInfoW          = user32.NewProc("GetMonitorInfoW")
	pEnableWindow             = user32.NewProc("EnableWindow")
	pSetForegroundWindow      = user32.NewProc("SetForegroundWindow")
	pSetFocus                 = user32.NewProc("SetFocus")
	pSetTimer                 = user32.NewProc("SetTimer")
	pKillTimer                = user32.NewProc("KillTimer")
	pFillRect                 = user32.NewProc("FillRect")
	pDrawTextW                = user32.NewProc("DrawTextW")
	pSetCursor                = user32.NewProc("SetCursor")
	pGetKeyState              = user32.NewProc("GetKeyState")
	pIsWindow                 = user32.NewProc("IsWindow")
	pIsDialogMessageW         = user32.NewProc("IsDialogMessageW")
	pGetParent                = user32.NewProc("GetParent")
	pGetAncestor              = user32.NewProc("GetAncestor")
	pTrackMouseEvent          = user32.NewProc("TrackMouseEvent")
	pGetDpiForWindow          = user32.NewProc("GetDpiForWindow")
	pSetProcessDpiAwarenessCx = user32.NewProc("SetProcessDpiAwarenessContext")

	pStretchDIBits     = gdi32.NewProc("StretchDIBits")
	pSetStretchBltMode = gdi32.NewProc("SetStretchBltMode")
	pSetBrushOrgEx     = gdi32.NewProc("SetBrushOrgEx")
	pPatBlt            = gdi32.NewProc("PatBlt")
	pGetStockObject    = gdi32.NewProc("GetStockObject")
	pCreateSolidBrush  = gdi32.NewProc("CreateSolidBrush")
	pDeleteObject      = gdi32.NewProc("DeleteObject")
	pSelectObject      = gdi32.NewProc("SelectObject")
	pSetTextColor      = gdi32.NewProc("SetTextColor")
	pSetBkMode         = gdi32.NewProc("SetBkMode")
	pCreateFontW       = gdi32.NewProc("CreateFontW")

	pGetModuleHandleW = kernel32.NewProc("GetModuleHandleW")

	pDragAcceptFiles = shell32.NewProc("DragAcceptFiles")
	pDragQueryFileW  = shell32.NewProc("DragQueryFileW")
	pDragFinish      = shell32.NewProc("DragFinish")
	pShellExecuteW   = shell32.NewProc("ShellExecuteW")

	pGetOpenFileNameW = comdlg32.NewProc("GetOpenFileNameW")

	pTimeBeginPeriod = winmm.NewProc("timeBeginPeriod")
)

const (
	wsOverlappedWindow = 0x00CF0000
	wsPopup            = 0x80000000
	wsVisible          = 0x10000000
	wsChild            = 0x40000000
	wsCaption          = 0x00C00000
	wsSysMenu          = 0x00080000
	wsTabStop          = 0x00010000
	wsClipChildren     = 0x02000000
	wsExAcceptFiles    = 0x00000010
	wsExDlgModalFrame  = 0x00000001

	cwUseDefault = 0x80000000

	wmCreate         = 0x0001
	wmDestroy        = 0x0002
	wmSize           = 0x0005
	wmActivate       = 0x0006
	wmSetFocus       = 0x0007
	wmKillFocus      = 0x0008
	wmPaint          = 0x000F
	wmClose          = 0x0010
	wmEraseBkgnd     = 0x0014
	wmActivateApp    = 0x001C
	wmSetCursor      = 0x0020
	wmGetMinMaxInfo  = 0x0024
	wmSetFont        = 0x0030
	wmKeyDown        = 0x0100
	wmKeyUp          = 0x0101
	wmSysKeyDown     = 0x0104
	wmSysKeyUp       = 0x0105
	wmCommand        = 0x0111
	wmSysCommand     = 0x0112
	wmTimer          = 0x0113
	wmInitMenuPopup  = 0x0117
	wmEnterMenuLoop  = 0x0211
	wmMouseMove      = 0x0200
	wmLButtonDown    = 0x0201
	wmLButtonUp      = 0x0202
	wmMouseLeave     = 0x02A3
	wmDropFiles      = 0x0233
	wmDpiChanged     = 0x02E0
	wmApp            = 0x8000
	wmCtlColorStatic = 0x0138

	swShow     = 5
	swShowNorm = 1

	mfString     = 0x0000
	mfGrayed     = 0x0001
	mfChecked    = 0x0008
	mfPopup      = 0x0010
	mfSeparator  = 0x0800
	mfByCommand  = 0x0000
	mfByPosition = 0x0400
	mfEnabled    = 0x0000
	mfUnchecked  = 0x0000

	mbOK          = 0x0
	mbOKCancel    = 0x1
	mbYesNo       = 0x4
	mbOkCancel    = 0x1
	mbIconError   = 0x10
	mbIconWarning = 0x30
	mbIconInfo    = 0x40
	idYes         = 6
	idOk          = 1
	idOK          = 1

	swpNoZOrder      = 0x0004
	swpNoMove        = 0x0002
	swpNoActivate    = 0x0010
	swpFrameChanged  = 0x0020
	swpNoOwnerZOrder = 0x0200

	monitorDefaultToNearest = 2

	colorOnColor = 3
	halftone     = 4
	dibRGBColors = 0
	srcCopy      = 0x00CC0020
	blackness    = 0x00000042

	blackBrush     = 4
	defaultGUIFont = 17

	idcArrow = 32512

	vkTab     = 0x09
	vkReturn  = 0x0D
	vkShift   = 0x10
	vkControl = 0x11
	vkMenu    = 0x12
	vkEscape  = 0x1B
	vkSpace   = 0x20
	vkF2      = 0x71
	vkF4      = 0x73
	vkF9      = 0x78
	vkF10     = 0x79
	vkF11     = 0x7A

	bsPushButton  = 0x0
	bnClicked     = 0
	ssLeft        = 0x0
	ssCenterImage = 0x200
	ssNoPrefix    = 0x80

	dtCenter    = 0x1
	dtVCenter   = 0x4
	dtWordBreak = 0x10
	dtNoPrefix  = 0x800

	transparent = 1

	ofnFileMustExist = 0x1000
	ofnPathMustExist = 0x800
	ofnHideReadOnly  = 0x4
	ofnExplorer      = 0x80000
	ofnNoChangeDir   = 0x8

	scKeyMenu = 0xF100
)

// GWL_STYLE (-16) as an argument
const gwlStyleIdx = ^uintptr(15)

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

type openFileName struct {
	StructSize    uint32
	Owner         uintptr
	Instance      uintptr
	Filter        *uint16
	CustomFilter  *uint16
	MaxCustFilter uint32
	FilterIndex   uint32
	File          *uint16
	MaxFile       uint32
	FileTitle     *uint16
	MaxFileTitle  uint32
	InitialDir    *uint16
	Title         *uint16
	Flags         uint32
	FileOffset    uint16
	FileExtension uint16
	DefExt        *uint16
	CustData      uintptr
	FnHook        uintptr
	TemplateName  *uint16
	PvReserved    uintptr
	DwReserved    uint32
	FlagsEx       uint32
}

type minMaxInfo struct {
	Reserved     point
	MaxSize      point
	MaxPosition  point
	MinTrackSize point
	MaxTrackSize point
}

type trackMouseEvent struct {
	Size      uint32
	Flags     uint32
	Track     uintptr
	HoverTime uint32
}

func u16(s string) *uint16 {
	p, _ := syscall.UTF16PtrFromString(s)
	return p
}

// u16z: a string that may hold \0 separators (file dialog filters).
func u16z(s string) *uint16 {
	r := utf16.Encode([]rune(s + "\x00"))
	return &r[0]
}

func call(p *syscall.LazyProc, args ...uintptr) uintptr {
	r, _, _ := p.Call(args...)
	return r
}

func messageBox(hwnd uintptr, text, title string, flags uint32) int {
	return int(call(pMessageBoxW, hwnd, uintptr(unsafe.Pointer(u16(text))), uintptr(unsafe.Pointer(u16(title))), uintptr(flags)))
}

func clientRect(hwnd uintptr) winRect {
	var r winRect
	call(pGetClientRect, hwnd, uintptr(unsafe.Pointer(&r)))
	return r
}

func windowRect(hwnd uintptr) winRect {
	var r winRect
	call(pGetWindowRect, hwnd, uintptr(unsafe.Pointer(&r)))
	return r
}

func loword(v uintptr) int { return int(int16(v & 0xffff)) }
func hiword(v uintptr) int { return int(int16((v >> 16) & 0xffff)) }

func setWindowText(hwnd uintptr, s string) {
	call(pSetWindowTextW, hwnd, uintptr(unsafe.Pointer(u16(s))))
}

func appendMenu(m uintptr, flags uint32, id uintptr, text string) {
	var p uintptr
	if text != "" {
		p = uintptr(unsafe.Pointer(u16(text)))
	}
	call(pAppendMenuW, m, uintptr(flags), id, p)
}

func checkItem(m uintptr, id int, on bool) {
	f := uintptr(mfByCommand | mfUnchecked)
	if on {
		f = mfByCommand | mfChecked
	}
	call(pCheckMenuItem, m, uintptr(id), f)
}

func enableItem(m uintptr, id int, on bool) {
	f := uintptr(mfByCommand | mfEnabled)
	if !on {
		f = mfByCommand | mfGrayed
	}
	call(pEnableMenuItem, m, uintptr(id), f)
}

func renameItem(m uintptr, id int, text string) {
	call(pModifyMenuW, m, uintptr(id), mfByCommand|mfString, uintptr(id), uintptr(unsafe.Pointer(u16(text))))
}

func keyDown(vk int) bool { return int16(call(pGetKeyState, uintptr(vk))) < 0 }

func shellOpen(path string) {
	call(pShellExecuteW, 0, uintptr(unsafe.Pointer(u16("open"))), uintptr(unsafe.Pointer(u16(path))), 0, 0, swShowNorm)
}
