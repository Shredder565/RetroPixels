package main

// More of the Windows API, for the start screen, the Save states window and
// the file and folder dialogs (ui_*_windows.go).

import (
	"syscall"
	"unsafe"
)

var (
	ole32 = syscall.NewLazyDLL("ole32.dll")

	pGetSaveFileNameW     = comdlg32.NewProc("GetSaveFileNameW")
	pSHBrowseForFolderW   = shell32.NewProc("SHBrowseForFolderW")
	pSHGetPathFromIDListW = shell32.NewProc("SHGetPathFromIDListW")
	pCoInitializeEx       = ole32.NewProc("CoInitializeEx")
	pCoTaskMemFree        = ole32.NewProc("CoTaskMemFree")

	pCreateCompatibleDC     = gdi32.NewProc("CreateCompatibleDC")
	pCreateCompatibleBitmap = gdi32.NewProc("CreateCompatibleBitmap")
	pBitBlt                 = gdi32.NewProc("BitBlt")
	pDeleteDC               = gdi32.NewProc("DeleteDC")
	pCreatePen              = gdi32.NewProc("CreatePen")
	pRoundRect              = gdi32.NewProc("RoundRect")
	pEllipse                = gdi32.NewProc("Ellipse")
	pGetTextExtentPoint32W  = gdi32.NewProc("GetTextExtentPoint32W")

	pGetForegroundWindow = user32.NewProc("GetForegroundWindow")
)

const (
	ofnOverwritePrompt = 0x2

	bifReturnOnlyFSDirs = 0x1
	bifNewDialogStyle   = 0x40
	bffmInitialized     = 1
	bffmSetSelectionW   = 0x400 + 103

	psSolid   = 0
	nullPen   = 8
	nullBrush = 5

	dtLeft        = 0x0
	dtRight       = 0x2
	dtSingleLine  = 0x20
	dtEndEllipsis = 0x8000

	idcHand = 32649

	wmLButtonDblClk = 0x0203
)

type browseInfo struct {
	Owner       uintptr
	Root        uintptr
	DisplayName *uint16
	Title       *uint16
	Flags       uint32
	Fn          uintptr
	LParam      uintptr
	Image       int32
}

type textSize struct{ CX, CY int32 }

// ---------------- file and folder dialogs ----------------

// fileDialog shows the Open (save=false) or Save As dialog. filter is
// "Name\x00*.ext\x00..." without the final \x00\x00.
func fileDialog(owner uintptr, save bool, title, filter, dir, name, defExt string) string {
	buf := make([]uint16, 32768)
	if name != "" {
		n, _ := syscall.UTF16FromString(name)
		copy(buf[:len(buf)-1], n)
	}
	o := openFileName{
		Owner:   owner,
		Filter:  u16z(filter + "\x00\x00"),
		File:    &buf[0],
		MaxFile: uint32(len(buf)),
		Title:   u16(title),
		Flags:   ofnPathMustExist | ofnHideReadOnly | ofnExplorer | ofnNoChangeDir,
	}
	if dir != "" {
		o.InitialDir = u16(dir)
	}
	if defExt != "" {
		o.DefExt = u16(defExt)
	}
	o.StructSize = uint32(unsafe.Sizeof(o))
	proc := pGetOpenFileNameW
	if save {
		o.Flags |= ofnOverwritePrompt
		proc = pGetSaveFileNameW
	} else {
		o.Flags |= ofnFileMustExist
	}
	if call(proc, uintptr(unsafe.Pointer(&o))) == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}

var (
	folderStart    string
	folderCallback = syscall.NewCallback(func(hwnd, m, lp, data uintptr) uintptr {
		if uint32(m) == bffmInitialized && folderStart != "" {
			call(pSendMessageW, hwnd, bffmSetSelectionW, 1, uintptr(unsafe.Pointer(u16(folderStart))))
		}
		return 0
	})
)

// pickFolder shows Windows' folder picker, starting at start ("" if cancelled).
func pickFolder(owner uintptr, title, start string) string {
	folderStart = start
	bi := browseInfo{Owner: owner, Title: u16(title), Flags: bifReturnOnlyFSDirs | bifNewDialogStyle, Fn: folderCallback}
	pidl := call(pSHBrowseForFolderW, uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return ""
	}
	defer call(pCoTaskMemFree, pidl)
	buf := make([]uint16, 32768) // (MAX_PATH is enough for the old call; the rest is room to spare)
	if call(pSHGetPathFromIDListW, pidl, uintptr(unsafe.Pointer(&buf[0]))) == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}

// ---------------- drawing helpers ----------------

func fillColor(dc uintptr, r winRect, color uint32) {
	br := call(pCreateSolidBrush, uintptr(color))
	call(pFillRect, dc, uintptr(unsafe.Pointer(&r)), br)
	call(pDeleteObject, br)
}

// roundBox: a rounded rectangle, filled, with a border unless border < 0.
func roundBox(dc uintptr, r winRect, radius int32, fill uint32, border int64, width int32) {
	var pen uintptr
	if border < 0 {
		pen = call(pGetStockObject, nullPen)
	} else {
		pen = call(pCreatePen, psSolid, uintptr(width), uintptr(border))
	}
	br := call(pCreateSolidBrush, uintptr(fill))
	op := call(pSelectObject, dc, pen)
	ob := call(pSelectObject, dc, br)
	call(pRoundRect, dc, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right), uintptr(r.Bottom), uintptr(radius), uintptr(radius))
	call(pSelectObject, dc, op)
	call(pSelectObject, dc, ob)
	call(pDeleteObject, br)
	if border >= 0 {
		call(pDeleteObject, pen)
	}
}

func paintDot(dc uintptr, x, y, d int32, color uint32) {
	pen := call(pGetStockObject, nullPen)
	br := call(pCreateSolidBrush, uintptr(color))
	op := call(pSelectObject, dc, pen)
	ob := call(pSelectObject, dc, br)
	call(pEllipse, dc, uintptr(x), uintptr(y), uintptr(x+d), uintptr(y+d))
	call(pSelectObject, dc, op)
	call(pSelectObject, dc, ob)
	call(pDeleteObject, br)
}

// paintText draws s in r with the given font, colour and DrawText flags.
func paintText(dc, font uintptr, s string, r winRect, color uint32, flags uint32) {
	old := call(pSelectObject, dc, font)
	call(pSetTextColor, dc, uintptr(color))
	call(pDrawTextW, dc, uintptr(unsafe.Pointer(u16(s))), ^uintptr(0), uintptr(unsafe.Pointer(&r)), uintptr(flags|dtNoPrefix))
	call(pSelectObject, dc, old)
}

// textHeight: the height s needs when wrapped to width w.
func textHeight(dc, font uintptr, s string, w int32) int32 {
	r := winRect{0, 0, w, 0}
	old := call(pSelectObject, dc, font)
	call(pDrawTextW, dc, uintptr(unsafe.Pointer(u16(s))), ^uintptr(0), uintptr(unsafe.Pointer(&r)), dtWordBreak|dtNoPrefix|0x400) // DT_CALCRECT
	call(pSelectObject, dc, old)
	return r.Bottom
}

func textWidth(dc, font uintptr, s string) int32 {
	var sz textSize
	p, _ := syscall.UTF16FromString(s)
	old := call(pSelectObject, dc, font)
	call(pGetTextExtentPoint32W, dc, uintptr(unsafe.Pointer(&p[0])), uintptr(len(p)-1), uintptr(unsafe.Pointer(&sz)))
	call(pSelectObject, dc, old)
	return sz.CX
}

func (r winRect) contains(x, y int) bool {
	return x >= int(r.Left) && x < int(r.Right) && y >= int(r.Top) && y < int(r.Bottom)
}

// drawPixels draws a 0x00RRGGBB picture stretched into r.
func drawPixels(dc uintptr, pix []uint32, w, h int, r winRect) {
	if len(pix) < w*h || w <= 0 || h <= 0 {
		return
	}
	bmi := bitmapInfoHeader{Width: int32(w), Height: -int32(h), Planes: 1, BitCount: 32}
	bmi.Size = uint32(unsafe.Sizeof(bmi))
	call(pSetStretchBltMode, dc, colorOnColor)
	call(pStretchDIBits, dc, uintptr(r.Left), uintptr(r.Top), uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top), 0, 0, uintptr(w), uintptr(h),
		uintptr(unsafe.Pointer(&pix[0])), uintptr(unsafe.Pointer(&bmi)), dibRGBColors, srcCopy)
}
