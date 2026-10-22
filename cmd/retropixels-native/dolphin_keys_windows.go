package main

// The "keys file" step before a Wii game. PixelU's Wii engine has no
// Nintendo keys of its own: it needs keys.bin (or otp.bin) from the
// player's own Wii, made with BootMii's NAND backup. When it's missing this
// window can look for it on the PC, take one the player picks, or explain
// where it comes from. Nothing is ever downloaded.

import (
	"context"
	"fmt"
	"path/filepath"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
)

const (
	idWkFind   = 1390
	idWkChoose = 1391
	idWkHow    = 1392
	idWkCancel = 1393
	idWkList   = 1394
	idWkUse    = 1395
	idWkStatus = 1396

	wsHScroll              = 0x00100000
	lbSetCurSel            = 0x0186
	lbSetHorizontalExtent  = 0x0194
	lbnDblClk              = 2
	lbnSelChange           = 1
	bootMiiURL             = "https://wiibrew.org/wiki/BootMii"
	wiiKeysWhereFrom       = "The keys file comes from your own Wii: BootMii's NAND backup saves keys.bin next to nand.bin on the console's SD card."
	wiiKeysTitle           = "Wii keys file"
	wiiKeysFileFilter      = "Wii keys (keys.bin, otp.bin, nand.bin)\x00*.bin\x00All files\x00*.*"
	wiiKeysScanEventPrefix = "dol-keys-"
)

var (
	wiiKeysClassDone bool
	wiiKeysCallback  uintptr // made with the class (wiiKeysProc ends up starting games, which opens this window)
)

type wiiKeysDlg struct {
	a    *app
	hwnd uintptr
	game dolphin.Game // started once the keys are in place
	user string
	ctl  map[int]uintptr

	cancel   context.CancelFunc // the running search, if any
	mu       sync.Mutex         // shared with the search
	scanDir  string
	scanN    int
	scanned  time.Time
	results  []*dolphin.WiiKeysFile
	shown    int // results in the list
	scanning bool
}

// dolNeedsWiiKeys: a Wii game without the owner's keys file. Shows the
// keys window (and starts the game from there once they're in place).
func (a *app) dolNeedsWiiKeys(g dolphin.Game, user string) bool {
	if g.Platform != "Wii" && g.Platform != "WiiWare" {
		return false // GameCube games need no keys
	}
	if dolphin.HaveWiiKeys(user) {
		return false
	}
	a.openWiiKeys(g, user)
	return true
}

func (a *app) openWiiKeys(g dolphin.Game, user string) {
	if d := a.dol.kdlg; d != nil {
		d.game = g
		call(pSetForegroundWindow, d.hwnd)
		return
	}
	cls := u16("RetroPixelsNativeWiiKeys")
	if !wiiKeysClassDone {
		wiiKeysCallback = syscall.NewCallback(wiiKeysProc)
		wc := wndClassEx{WndProc: wiiKeysCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls}
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		wiiKeysClassDone = true
	}
	d := &wiiKeysDlg{a: a, game: g, user: user, ctl: map[int]uintptr{}}
	a.dol.kdlg = d
	owner := a.hwnd
	if a.dol.lib != nil {
		owner = a.dol.lib.hwnd
	}
	dpi := a.dpi()
	w, h := 620*dpi/96, 420*dpi/96
	wr := windowRect(owner)
	x := max(int(wr.Left)+(int(wr.Right-wr.Left)-w)/2, 0)
	y := max(int(wr.Top)+(int(wr.Bottom-wr.Top)-h)/2, 0)
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame|wsExControlParent, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16(wiiKeysTitle))),
		wsPopup|wsCaption|wsSysMenu|wsVisible|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), owner, 0, a.hinst, 0)
	d.build()
}

func (d *wiiKeysDlg) scale(v int) uintptr { return uintptr(v * d.a.dpi() / 96) }

func (d *wiiKeysDlg) child(class, text string, style uint32, x, y, w, h, id int) uintptr {
	c := call(pCreateWindowExW, 0, uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), d.scale(x), d.scale(y), d.scale(w), d.scale(h), d.hwnd, uintptr(id), d.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, d.a.font, 1)
	if id != 0 {
		d.ctl[id] = c
	}
	return c
}

func (d *wiiKeysDlg) build() {
	root := dolphin.WiiRoot(d.user)
	intro := fmt.Sprintf("%s needs a keys file from your own Wii to play. RetroPixels doesn't include Nintendo's keys "+
		"and never downloads them.\n\n%s\n\nIt goes in: %s", d.game.Name, wiiKeysWhereFrom, root)
	d.child("STATIC", intro, ssNoPrefix, 16, 14, 572, 100, 0)
	y := 122
	d.child("BUTTON", "Find keys on this PC", bsDefPushButton|wsTabStop, 16, y, 160, 28, idWkFind)
	d.child("BUTTON", "Choose file...", bsPushButton|wsTabStop, 184, y, 120, 28, idWkChoose)
	d.child("BUTTON", "How to get yours", bsPushButton|wsTabStop, 312, y, 140, 28, idWkHow)
	y += 38
	d.child("STATIC", "", ssNoPrefix, 16, y, 572, 20, idWkStatus)
	y += 24
	d.child("LISTBOX", "", lbsNotify|lbsNoIntegral|wsVScroll|wsHScroll|wsBorder|wsTabStop, 16, y, 572, 140, idWkList)
	call(pSendMessageW, d.ctl[idWkList], lbSetHorizontalExtent, d.scale(1400), 0)
	y += 150
	use := d.child("BUTTON", "Use this file", bsPushButton|wsTabStop, 384, y, 100, 28, idWkUse)
	call(pEnableWindow, use, 0)
	d.child("BUTTON", "Cancel", bsPushButton|wsTabStop, 492, y, 96, 28, idWkCancel)
	wr, cr := windowRect(d.hwnd), clientRect(d.hwnd)
	extraH := int(wr.Bottom-wr.Top) - int(cr.Bottom)
	call(pSetWindowPos, d.hwnd, 0, 0, 0, uintptr(wr.Right-wr.Left), uintptr(extraH)+d.scale(y+44), swpNoMove|swpNoZOrder|swpNoActivate)
}

func (d *wiiKeysDlg) status(s string) { setWindowText(d.ctl[idWkStatus], s) }

// find starts (or stops) the search in the background.
func (d *wiiKeysDlg) find() {
	if d.cancel != nil {
		d.cancel()
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	d.cancel = cancel
	d.mu.Lock()
	d.results, d.scanN, d.scanDir, d.scanning = nil, 0, "", true
	d.scanned = time.Now()
	d.mu.Unlock()
	d.shown = 0
	call(pSendMessageW, d.ctl[idWkList], lbResetContent, 0, 0)
	call(pEnableWindow, d.ctl[idWkUse], 0)
	setWindowText(d.ctl[idWkFind], "Stop looking")
	d.status("Looking for keys.bin and otp.bin on this PC...")
	a := d.a
	go func() {
		found := dolphin.ScanWiiKeys(ctx, dolphin.WiiKeysScan{Progress: func(dir string, n int) {
			d.mu.Lock()
			d.scanDir, d.scanN = dir, n
			d.mu.Unlock()
			a.event(wiiKeysScanEventPrefix+"progress", "")
		}})
		d.mu.Lock()
		d.results, d.scanN, d.scanning = found, len(found), false
		d.mu.Unlock()
		a.event(wiiKeysScanEventPrefix+"done", "")
	}()
}

// wiiKeysEvent: news from the search (UI thread).
func (a *app) wiiKeysEvent(name string) {
	d := a.dol.kdlg
	if d == nil {
		return
	}
	d.mu.Lock()
	dir, n, results, scanning, started := d.scanDir, d.scanN, d.results, d.scanning, d.scanned
	d.mu.Unlock()
	if name == wiiKeysScanEventPrefix+"progress" {
		if scanning {
			d.status(fmt.Sprintf("Looking (%d s, %d found): %s", int(time.Since(started).Seconds()), n, dir))
		}
		return
	}
	if d.cancel != nil {
		d.cancel()
		d.cancel = nil
	}
	setWindowText(d.ctl[idWkFind], "Find keys on this PC")
	lb := d.ctl[idWkList]
	for _, f := range results[d.shown:] {
		call(pSendMessageW, lb, lbAddString, 0, uintptr(unsafe.Pointer(u16(f.Describe()))))
	}
	d.shown = len(results)
	switch len(results) {
	case 0:
		d.status("No keys file found. Use \"Choose file...\" if it's somewhere else, or \"How to get yours\".")
	default:
		call(pSendMessageW, lb, lbSetCurSel, 0, 0)
		call(pEnableWindow, d.ctl[idWkUse], 1)
		d.status(fmt.Sprintf("Found %d. Choose the one from your Wii, then \"Use this file\".", len(results)))
	}
}

// useSelected installs the chosen search result.
func (d *wiiKeysDlg) useSelected() {
	i := int(int32(call(pSendMessageW, d.ctl[idWkList], lbGetCurSel, 0, 0)))
	d.mu.Lock()
	var f *dolphin.WiiKeysFile
	if i >= 0 && i < len(d.results) {
		f = d.results[i]
	}
	d.mu.Unlock()
	if f != nil {
		d.install(f)
	}
}

func (d *wiiKeysDlg) choose() {
	p := fileDialog(d.hwnd, false, "Choose your Wii's keys.bin or otp.bin", wiiKeysFileFilter, "", "", "")
	if p == "" {
		return
	}
	f, err := dolphin.ReadWiiKeysFile(p)
	if err != nil {
		messageBox(d.hwnd, filepath.Base(p)+": "+err.Error()+".\n\nUse the keys.bin that BootMii saved with your NAND backup, "+
			"or your console's otp.bin (128 bytes from a Wii, 1 KiB from a Wii U).", wiiKeysTitle, mbIconWarning)
		return
	}
	d.install(f)
}

// install puts the keys in place and starts the game.
func (d *wiiKeysDlg) install(f *dolphin.WiiKeysFile) {
	dst, err := dolphin.InstallWiiKeys(d.user, f)
	if err != nil {
		messageBox(d.hwnd, "Couldn't copy the keys file: "+err.Error(), wiiKeysTitle, mbIconWarning)
		return
	}
	if !dolphin.HaveWiiKeys(d.user) {
		messageBox(d.hwnd, "The keys file was copied to "+dst+" but can't be read back.", wiiKeysTitle, mbIconWarning)
		return
	}
	a, g := d.a, d.game
	d.close()
	a.showMessage("Wii keys file saved")
	a.dolPlay(g)
}

func (d *wiiKeysDlg) close() {
	a := d.a
	if d.cancel != nil {
		d.cancel()
		d.cancel = nil
	}
	owner := call(pGetParent, d.hwnd)
	call(pDestroyWindow, d.hwnd)
	a.dol.kdlg = nil
	if owner != 0 {
		call(pSetForegroundWindow, owner)
	}
}

func wiiKeysProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.dol.kdlg == nil || a.dol.kdlg.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.dol.kdlg
	switch uint32(m) {
	case wmCommand:
		id, code := loword(wp)&0xffff, hiword(wp)&0xffff
		switch id {
		case idWkFind, idOK:
			d.find()
		case idWkChoose:
			d.choose()
		case idWkHow:
			d.status(wiiKeysWhereFrom)
			shellOpen(bootMiiURL)
		case idWkUse:
			d.useSelected()
		case idWkList:
			switch code {
			case lbnDblClk:
				d.useSelected()
			case lbnSelChange:
				call(pEnableWindow, d.ctl[idWkUse], 1)
			}
		case idWkCancel, 2: // IDCANCEL (Esc)
			d.close()
		}
		return 0
	case wmClose:
		d.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}
