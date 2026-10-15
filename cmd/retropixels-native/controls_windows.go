package main

import (
	"syscall"
	"unsafe"
)

// The Controls window: change which key and which controller button work
// each of the system's buttons (showControls in host.js). The game waits
// while it is open.

const (
	idChange0    = 1000 // + row
	idReset      = 900
	idDone       = 901
	idSystem     = 902
	timerPad     = 2
	cbsDropList  = 0x0003
	cbAddString  = 0x0143
	cbSetCurSel  = 0x014E
	cbGetCurSel  = 0x0147
	cbnSelChange = 1
	wsVScroll    = 0x00200000
)

var (
	controlsClassDone bool
	controlsCallback  = syscall.NewCallback(controlsProc)
)

type controlsDialog struct {
	a       *app
	hwnd    uintptr
	sys     string
	systems []string // in the System list
	cmap    ControlMap
	names   []string
	rows    [][3]uintptr // the pad text, keys text and Change button of each row
	other   []uintptr    // everything else made for the current system
	waiting string
	before  [4]PadState
}

func (a *app) openControls() {
	if a.controls != nil {
		call(pSetForegroundWindow, a.controls.hwnd)
		return
	}
	d := &controlsDialog{a: a}
	for _, s := range systems {
		if available(s.ID) {
			d.systems = append(d.systems, s.ID)
		}
	}
	if len(d.systems) == 0 {
		messageBox(a.hwnd, "This build has no emulators.", "Controls", mbIconInfo)
		return
	}
	d.sys = d.systems[0]
	if a.sess != nil {
		d.sys = a.sess.game.System
	}
	cls := u16("RetroPixelsNativeControls")
	if !controlsClassDone {
		wc := wndClassEx{WndProc: controlsCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls} // COLOR_BTNFACE + 1
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		controlsClassDone = true
	}
	a.controls = d
	if a.sess != nil {
		a.sess.modal.Store(true)
		a.sess.snd.Clear()
		a.sess.clearKeys()
	}
	dpi := a.dpi()
	w, h := 560*dpi/96, 520*dpi/96
	wr := windowRect(a.hwnd)
	x := int(wr.Left) + (int(wr.Right-wr.Left)-w)/2
	y := int(wr.Top) + (int(wr.Bottom-wr.Top)-h)/2
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("Controls"))),
		wsPopup|wsCaption|wsSysMenu|wsVisible|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), a.hwnd, 0, a.hinst, 0)
	call(pEnableWindow, a.hwnd, 0)
	d.build()
	call(pSetTimer, d.hwnd, timerPad, 30, 0)
}

func (d *controlsDialog) scale(v int) uintptr { return uintptr(v * d.a.dpi() / 96) }

func (d *controlsDialog) child(class, text string, style uint32, x, y, w, h int, id int) uintptr {
	c := call(pCreateWindowExW, 0, uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), d.scale(x), d.scale(y), d.scale(w), d.scale(h), d.hwnd, uintptr(id), d.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, d.a.font, 1)
	return c
}

// build makes the rows for the current system.
func (d *controlsDialog) build() {
	for _, r := range d.rows {
		for _, c := range r {
			call(pDestroyWindow, c)
		}
	}
	for _, c := range d.other {
		call(pDestroyWindow, c)
	}
	d.rows, d.other = nil, nil
	d.waiting = ""
	d.cmap = d.a.settings.controlMap(d.sys)
	d.names = systemButtons[d.sys]
	if s := d.a.sess; s != nil && s.game.System == d.sys {
		d.names = s.info.Buttons
	}

	add := func(c uintptr) { d.other = append(d.other, c) }
	add(d.child("STATIC", "System:", ssLeft|ssNoPrefix, 16, 16, 70, 22, 0))
	combo := d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 90, 12, 200, 200, idSystem)
	add(combo)
	for i, id := range d.systems {
		call(pSendMessageW, combo, cbAddString, 0, uintptr(unsafe.Pointer(u16(systemByID(id).Label))))
		if id == d.sys {
			call(pSendMessageW, combo, cbSetCurSel, uintptr(i), 0)
		}
	}
	y := 52
	add(d.child("STATIC", "Button", ssNoPrefix, 16, y, 130, 20, 0))
	add(d.child("STATIC", "Controller", ssNoPrefix, 150, y, 130, 20, 0))
	add(d.child("STATIC", "Keyboard", ssNoPrefix, 290, y, 140, 20, 0))
	y += 24
	for i, n := range d.names {
		bd := d.cmap[n]
		pad, keys := bindingText(bd)
		label := n
		if bd.Toggle {
			label += " (switch)"
		}
		add(d.child("STATIC", label, ssNoPrefix, 16, y+4, 130, 20, 0))
		row := [3]uintptr{
			d.child("STATIC", pad, ssNoPrefix, 150, y+4, 135, 20, 0),
			d.child("STATIC", keys, ssNoPrefix, 290, y+4, 150, 20, 0),
			d.child("BUTTON", "Change", bsPushButton|wsTabStop, 450, y, 80, 26, idChange0+i),
		}
		d.rows = append(d.rows, row)
		y += 30
	}
	note := "Click Change, then press a controller button or a key. Esc cancels. " +
		"The left stick moves too. Clicking the sticks (L3 / R3) loads and saves states; " +
		"hold the right stick down for a second to start or stop recording."
	if s := d.a.sess; s != nil && s.game.System == d.sys && s.note != "" {
		note = "Click Change, then press a controller button or a key. Esc cancels. " + s.note
	} else if n := systemNotes[d.sys]; n != "" {
		note = "Click Change, then press a controller button or a key. Esc cancels. " + n
	}
	add(d.child("STATIC", note, ssNoPrefix, 16, y+6, 514, 60, 0))
	y += 72
	add(d.child("BUTTON", "Reset to default", bsPushButton|wsTabStop, 300, y, 130, 28, idReset))
	add(d.child("BUTTON", "Done", bsPushButton|wsTabStop, 440, y, 90, 28, idDone))
	// fit the window to the rows
	wr, cr := windowRect(d.hwnd), clientRect(d.hwnd)
	extraH := int(wr.Bottom-wr.Top) - int(cr.Bottom)
	call(pSetWindowPos, d.hwnd, 0, 0, 0, uintptr(wr.Right-wr.Left), uintptr(extraH)+d.scale(y+44), swpNoMove|swpNoZOrder|swpNoActivate)
}

func (d *controlsDialog) refreshRow(i int) {
	pad, keys := bindingText(d.cmap[d.names[i]])
	setWindowText(d.rows[i][0], pad)
	setWindowText(d.rows[i][1], keys)
	setWindowText(d.rows[i][2], "Change")
}

func (d *controlsDialog) startWait(i int) {
	d.stopWait()
	d.waiting = d.names[i]
	d.before = d.a.xin.read()
	setWindowText(d.rows[i][2], "Press…")
	call(pSetFocus, d.hwnd)
}

func (d *controlsDialog) stopWait() {
	if d.waiting == "" {
		return
	}
	for i, n := range d.names {
		if n == d.waiting {
			d.refreshRow(i)
		}
	}
	d.waiting = ""
}

func (d *controlsDialog) assign(bd Binding) {
	name := d.waiting
	d.cmap[name] = bd
	d.a.settings.setControlMap(d.sys, d.cmap)
	saveSettings(d.a.settings)
	d.stopWait()
}

// capturing takes the key press while a Change is waiting (called from the
// message loop, before the focused button can eat it).
func (d *controlsDialog) capturing(m *msg) bool {
	if d.waiting == "" || (m.Message != wmKeyDown && m.Message != wmSysKeyDown) {
		return false
	}
	vk := int(m.WParam)
	if vk == vkEscape {
		d.stopWait()
		return true
	}
	name := keyName(vk)
	if name == "" {
		return true
	}
	old := d.cmap[d.waiting]
	d.assign(Binding{Pad: old.Pad, Keys: []string{name}, Toggle: old.Toggle})
	return true
}

func (d *controlsDialog) pollPads() {
	if d.waiting == "" {
		return
	}
	now := d.a.xin.read()
	for p := range now {
		for b := 0; b < padButtons; b++ {
			if now[p].Buttons[b] && !d.before[p].Buttons[b] && b != padL3 && b != padR3 {
				old := d.cmap[d.waiting]
				d.assign(Binding{Pad: []int{b}, Keys: old.Keys, Toggle: old.Toggle})
				return
			}
		}
	}
	d.before = now
}

func (d *controlsDialog) close() {
	a := d.a
	call(pKillTimer, d.hwnd, timerPad)
	call(pEnableWindow, a.hwnd, 1)
	call(pDestroyWindow, d.hwnd)
	a.controls = nil
	call(pSetForegroundWindow, a.hwnd)
	if a.sess != nil {
		a.sess.setControlMap(a.settings.controlMap(a.sess.game.System))
		a.sess.modal.Store(false)
	}
}

func controlsProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.controls == nil || a.controls.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.controls
	switch uint32(m) {
	case wmCommand:
		id, code := loword(wp)&0xffff, hiword(wp)&0xffff
		switch {
		case id == idDone:
			d.close()
		case id == idReset:
			if a.settings.Maps != nil {
				delete(a.settings.Maps, d.sys)
			}
			saveSettings(a.settings)
			d.build()
		case id == idSystem && code == cbnSelChange:
			i := int(call(pSendMessageW, lp, cbGetCurSel, 0, 0))
			if i >= 0 && i < len(d.systems) {
				d.sys = d.systems[i]
				d.build()
			}
		case id >= idChange0 && id < idChange0+len(d.rows) && code == bnClicked:
			d.startWait(id - idChange0)
		}
		return 0
	case wmTimer:
		if wp == timerPad {
			d.pollPads()
		}
		return 0
	case wmClose:
		d.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}
