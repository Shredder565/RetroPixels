package main

// The "PixelU settings (Wii/GameCube)" window: the settings RetroPixels.exe
// offers (graphics API, resolution, aspect ratio...), read from and written
// to the engine's own files (Dolphin's, in PixelU\wii\User\Config). The
// engine must be closed to change them: it writes its own settings when it
// closes.

import (
	"path/filepath"
	"syscall"
	"unsafe"

	"retropixels/internal/dolphin"
)

const (
	idDsSave    = 1380
	idDsCancel  = 1381
	idDsCtl0    = 1382 // + setting
	bsAutoCheck = 0x3
	bmGetCheck  = 0x00F0
	bmSetCheck  = 0x00F1
)

var (
	dolSetClassDone bool
	dolSetCallback  = syscall.NewCallback(dolSetProc)
)

type dolSettingsDlg struct {
	a       *app
	hwnd    uintptr
	user    string
	values  map[string]string
	extra   map[string]string // a value set in PixelU itself that isn't one of the choices
	exeName string            // the engine's file name, to tell whether it's open
	ctls    []uintptr
	locked  bool
}

func (a *app) dolSettingsHwnd() uintptr {
	if a.dol.sdlg != nil {
		return a.dol.sdlg.hwnd
	}
	return 0
}

func (a *app) openDolSettings() {
	if d := a.dol.sdlg; d != nil {
		call(pSetForegroundWindow, d.hwnd)
		return
	}
	cls := u16("RetroPixelsNativeDolphinSettings")
	if !dolSetClassDone {
		wc := wndClassEx{WndProc: dolSetCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls}
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		dolSetClassDone = true
	}
	exe := a.dolExe()
	if exe == "" {
		messageBox(a.hwnd, noEngine, "RetroPixels Native", mbIconInfo)
		return
	}
	a.settings.Dolphin.importOld(exe) // the old Dolphin's settings first, the first time
	d := &dolSettingsDlg{a: a, user: dolphin.UserDir(exe), extra: map[string]string{}}
	d.values = dolphin.ReadSettings(d.user)
	d.exeName = filepath.Base(exe)
	d.locked = a.dol.run != nil || dolphin.Running(d.exeName)
	a.dol.sdlg = d
	owner := a.hwnd
	if a.dol.lib != nil {
		owner = a.dol.lib.hwnd
	}
	dpi := a.dpi()
	w, h := 600*dpi/96, 560*dpi/96
	wr := windowRect(owner)
	x := int(wr.Left) + (int(wr.Right-wr.Left)-w)/2
	y := int(wr.Top) + (int(wr.Bottom-wr.Top)-h)/2
	if x < 0 {
		x = 0
	}
	if y < 0 {
		y = 0
	}
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame|wsExControlParent, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("PixelU settings (Wii/GameCube)"))),
		wsPopup|wsCaption|wsSysMenu|wsVisible|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), owner, 0, a.hinst, 0)
	d.build()
}

func (d *dolSettingsDlg) scale(v int) uintptr { return uintptr(v * d.a.dpi() / 96) }

func (d *dolSettingsDlg) child(class, text string, style uint32, x, y, w, h, id int) uintptr {
	c := call(pCreateWindowExW, 0, uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), d.scale(x), d.scale(y), d.scale(w), d.scale(h), d.hwnd, uintptr(id), d.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, d.a.font, 1)
	return c
}

func (d *dolSettingsDlg) build() {
	y := 14
	top := "PixelU's Wii/GameCube settings (engine based on Dolphin), used for every game. Memory cards, cheats and per-game settings " +
		"are in PixelU itself (GameCube / Wii > Open PixelU).\nSaved in " + d.user + "\\Config"
	if d.locked {
		top = "PixelU (Wii/GameCube) is open. Close it first: it saves its own settings when it closes."
	}
	d.child("STATIC", top, ssNoPrefix, 16, y, 556, 72, 0)
	y += 80
	for i, s := range dolphin.Settings {
		cur := d.values[s.ID]
		var c uintptr
		if s.Bool() {
			c = d.child("BUTTON", s.Label, bsAutoCheck|wsTabStop, 16, y, 400, 22, idDsCtl0+i)
			if cur == "True" {
				call(pSendMessageW, c, bmSetCheck, 1, 0)
			}
		} else {
			d.child("STATIC", s.Label+":", ssNoPrefix, 16, y+4, 170, 20, 0)
			c = d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 190, y, 260, 260, idDsCtl0+i)
			sel := -1
			for k, o := range s.Options {
				call(pSendMessageW, c, cbAddString, 0, uintptr(unsafe.Pointer(u16(o.Label))))
				if o.Value == cur {
					sel = k
				}
			}
			if sel < 0 {
				d.extra[s.ID] = cur
				call(pSendMessageW, c, cbAddString, 0, uintptr(unsafe.Pointer(u16(cur+" (set in PixelU)"))))
				sel = len(s.Options)
			}
			call(pSendMessageW, c, cbSetCurSel, uintptr(sel), 0)
		}
		if d.locked {
			call(pEnableWindow, c, 0)
		}
		d.ctls = append(d.ctls, c)
		y += 30
		if s.Help != "" {
			d.child("STATIC", s.Help, ssNoPrefix, 36, y-2, 520, 20, 0)
			y += 22
		}
	}
	y += 10
	save := d.child("BUTTON", "Save", bsDefPushButton|wsTabStop, 372, y, 96, 28, idDsSave)
	if d.locked {
		call(pEnableWindow, save, 0)
	}
	d.child("BUTTON", "Cancel", bsPushButton|wsTabStop, 476, y, 96, 28, idDsCancel)
	wr, cr := windowRect(d.hwnd), clientRect(d.hwnd)
	extraH := int(wr.Bottom-wr.Top) - int(cr.Bottom)
	call(pSetWindowPos, d.hwnd, 0, 0, 0, uintptr(wr.Right-wr.Left), uintptr(extraH)+d.scale(y+44), swpNoMove|swpNoZOrder|swpNoActivate)
}

// save writes what changed.
func (d *dolSettingsDlg) save() bool {
	if d.a.dol.run != nil || dolphin.Running(d.exeName) {
		messageBox(d.hwnd, "PixelU (Wii/GameCube) is open. Close it first: it saves its own settings when it closes.", "PixelU settings", mbIconInfo)
		return false
	}
	changes := map[string]string{}
	for i, s := range dolphin.Settings {
		c := d.ctls[i]
		v := ""
		if s.Bool() {
			v = "False"
			if call(pSendMessageW, c, bmGetCheck, 0, 0) == 1 {
				v = "True"
			}
		} else {
			k := int(int32(call(pSendMessageW, c, cbGetCurSel, 0, 0)))
			switch {
			case k >= 0 && k < len(s.Options):
				v = s.Options[k].Value
			default:
				v = d.extra[s.ID]
			}
		}
		if v != d.values[s.ID] {
			changes[s.ID] = v
		}
	}
	if len(changes) == 0 {
		return true
	}
	if err := dolphin.SaveSettings(d.user, changes); err != nil {
		messageBox(d.hwnd, "Couldn't save PixelU's settings: "+err.Error(), "PixelU settings", mbIconWarning)
		return false
	}
	d.a.showMessage("PixelU settings saved")
	return true
}

func (d *dolSettingsDlg) close() {
	a := d.a
	owner := call(pGetParent, d.hwnd)
	call(pDestroyWindow, d.hwnd)
	a.dol.sdlg = nil
	if owner != 0 {
		call(pSetForegroundWindow, owner)
	}
}

func dolSetProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.dol.sdlg == nil || a.dol.sdlg.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.dol.sdlg
	switch uint32(m) {
	case wmCommand:
		switch loword(wp) & 0xffff {
		case idDsSave, idOK:
			if d.locked || d.save() {
				d.close()
			}
		case idDsCancel, 2: // IDCANCEL (Esc)
			d.close()
		}
		return 0
	case wmClose:
		d.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}
