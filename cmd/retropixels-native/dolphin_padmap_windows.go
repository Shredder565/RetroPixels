package main

// The "Map controller buttons" window (GameCube / Wii > Map controller
// buttons…): which button, stick or key works each GameCube controller and
// Wii Remote button, read from and written to PixelU's GCPadNew.ini and
// WiimoteNew.ini (and Dolphin.ini's GameCube ports). Pick a row, click
// "Press to set" (or double-click the row) and press the button, move the
// stick or press the key. The engine must be closed to save: it writes its
// own settings when it closes.

import (
	"fmt"
	"path/filepath"
	"syscall"
	"unsafe"

	"retropixels/internal/dolphin"
)

const (
	idPmPad      = 1430
	idPmPlug     = 1431
	idPmExt      = 1432
	idPmDev      = 1433
	idPmList     = 1434
	idPmExpr     = 1435
	idPmSet      = 1436
	idPmClear    = 1437
	idPmDefault  = 1438
	idPmClearAll = 1439
	idPmSave     = 1440
	idPmCancel   = 1441
	idPmStatus   = 1442
	idPmExtLabel = 1443
	timerPmPad   = 1450

	pmWaitTicks = 200 // 30 ms each: about 6 seconds to press something
)

var (
	padMapClassDone bool
	padMapCallback  = syscall.NewCallback(padMapProc)
)

// The eight controllers in the Controller list.
var pmPads = func() (out [8]struct {
	kind string
	port int
}) {
	for i := 0; i < 4; i++ {
		out[i].kind, out[i].port = dolphin.KindGC, i
		out[4+i].kind, out[4+i].port = dolphin.KindWiimote, i
	}
	return
}()

type dolPadMapDlg struct {
	a      *app
	hwnd   uintptr
	user   string
	exe    string
	locked bool
	ctl    map[int]uintptr

	orig  [8]*dolphin.PadMap // as the files hold them
	maps  [8]*dolphin.PadMap // being edited
	cur   int
	rows  []dolphin.PadControl
	devs  []string         // the Use list's devices
	plugs []dolphin.Option // the Plugged in list's choices
	exts  []dolphin.Option // the Extension list's choices

	waiting  int // the row waiting for a press, or -1
	waitLeft int
	before   [4]PadState
	quiet    bool // the expression box is being filled in, not typed in
}

func (a *app) dolPadMapHwnd() uintptr {
	if a.dol.pdlg != nil {
		return a.dol.pdlg.hwnd
	}
	return 0
}

func (a *app) openDolPadMap() {
	if d := a.dol.pdlg; d != nil {
		call(pSetForegroundWindow, d.hwnd)
		return
	}
	exe := a.dolExe()
	if exe == "" {
		messageBox(a.hwnd, noEngine, "RetroPixels", mbIconInfo)
		return
	}
	cls := u16("RetroPixelsNativeDolphinPadMap")
	if !padMapClassDone {
		wc := wndClassEx{WndProc: padMapCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls}
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		padMapClassDone = true
	}
	a.settings.Dolphin.importOld(exe) // the old Dolphin's set-up first, the first time
	d := &dolPadMapDlg{a: a, exe: exe, user: dolphin.UserDir(exe), ctl: map[int]uintptr{}, waiting: -1}
	d.locked = a.dol.run != nil || dolphin.Running(filepath.Base(exe))
	for i, p := range pmPads {
		d.orig[i] = dolphin.ReadPadMap(d.user, p.kind, p.port)
		d.maps[i] = d.orig[i].Clone()
		if d.maps[i].Device == "" {
			// never set up: start from the Xbox layout RetroPixels sets up
			d.maps[i].Device = dolphin.XInputPad(p.port)
			d.maps[i].Defaults()
		}
	}
	a.dol.pdlg = d
	owner := a.hwnd
	if a.dol.lib != nil {
		owner = a.dol.lib.hwnd
	}
	dpi := a.dpi()
	w, h := 640*dpi/96, 640*dpi/96
	wr := windowRect(owner)
	x := max(int(wr.Left)+(int(wr.Right-wr.Left)-w)/2, 0)
	y := max(int(wr.Top)+(int(wr.Bottom-wr.Top)-h)/2, 0)
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame|wsExControlParent, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("Map controller buttons (GameCube / Wii)"))),
		wsPopup|wsCaption|wsSysMenu|wsVisible|wsClipChildren, uintptr(x), uintptr(y), uintptr(w), uintptr(h), owner, 0, a.hinst, 0)
	d.build()
	d.show(0)
	call(pSetTimer, d.hwnd, timerPmPad, 30, 0)
}

func (d *dolPadMapDlg) scale(v int) uintptr { return uintptr(v * d.a.dpi() / 96) }

func (d *dolPadMapDlg) child(class, text string, style uint32, exStyle uintptr, x, y, w, h, id int) uintptr {
	c := call(pCreateWindowExW, exStyle, uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), d.scale(x), d.scale(y), d.scale(w), d.scale(h), d.hwnd, uintptr(id), d.a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, d.a.font, 1)
	if id != 0 {
		d.ctl[id] = c
	}
	return c
}

func (d *dolPadMapDlg) build() {
	y := 14
	d.child("STATIC", "Controller:", ssNoPrefix, 0, 16, y+4, 90, 20, 0)
	pad := d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, 110, y, 220, 300, idPmPad)
	for _, p := range d.maps {
		call(pSendMessageW, pad, cbAddString, 0, uintptr(unsafe.Pointer(u16(p.Title()))))
	}
	y += 34
	d.child("STATIC", "Plugged in:", ssNoPrefix, 0, 16, y+4, 90, 20, 0)
	d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, 110, y, 220, 200, idPmPlug)
	d.child("STATIC", "Extension:", ssNoPrefix, 0, 346, y+4, 80, 20, idPmExtLabel)
	d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, 426, y, 180, 200, idPmExt)
	y += 34
	d.child("STATIC", "Play it with:", ssNoPrefix, 0, 16, y+4, 90, 20, 0)
	d.child("COMBOBOX", "", cbsDropList|wsTabStop|wsVScroll, 0, 110, y, 300, 200, idPmDev)
	y += 38
	lv := d.child("SysListView32", "", lvsReport|lvsSingleSel|lvsShowSelAlways|wsTabStop, 0x200, 16, y, 590, 300, idPmList)
	call(pSendMessageW, lv, lvmSetExStyle, 0x20|0x10000, 0x20|0x10000) // full-row select, double-buffered
	for i, c := range []struct {
		name string
		w    int
	}{{"Button", 200}, {"Mapped to", 360}} {
		col := lvColumn{Mask: 1 | 2 | 4 | 8, Cx: int32(d.scale(c.w)), Text: u16(c.name), SubItem: int32(i)}
		call(pSendMessageW, lv, lvmInsertColumnW, uintptr(i), uintptr(unsafe.Pointer(&col)))
	}
	y += 310
	d.child("STATIC", "Mapped to:", ssNoPrefix, 0, 16, y+4, 90, 20, 0)
	d.child("EDIT", "", esAutoHScroll|wsTabStop, 0x200, 110, y, 260, 24, idPmExpr)
	d.child("BUTTON", "Press to set", bsPushButton|wsTabStop, 0, 378, y-1, 120, 28, idPmSet)
	d.child("BUTTON", "Clear", bsPushButton|wsTabStop, 0, 506, y-1, 100, 28, idPmClear)
	y += 36
	d.child("STATIC", "", ssNoPrefix, 0, 16, y, 590, 40, idPmStatus)
	y += 46
	d.child("BUTTON", "Default layout", bsPushButton|wsTabStop, 0, 16, y, 120, 28, idPmDefault)
	d.child("BUTTON", "Clear all", bsPushButton|wsTabStop, 0, 144, y, 96, 28, idPmClearAll)
	save := d.child("BUTTON", "Save", bsPushButton|wsTabStop, 0, 406, y, 96, 28, idPmSave)
	d.child("BUTTON", "Cancel", bsPushButton|wsTabStop, 0, 510, y, 96, 28, idPmCancel)
	if d.locked {
		call(pEnableWindow, save, 0)
	}
	wr, cr := windowRect(d.hwnd), clientRect(d.hwnd)
	extraH := int(wr.Bottom-wr.Top) - int(cr.Bottom)
	call(pSetWindowPos, d.hwnd, 0, 0, 0, uintptr(wr.Right-wr.Left), uintptr(extraH)+d.scale(y+44), swpNoMove|swpNoZOrder|swpNoActivate)
}

func (d *dolPadMapDlg) status(s string) { setWindowText(d.ctl[idPmStatus], s) }

func (d *dolPadMapDlg) idleStatus() {
	switch {
	case d.locked:
		d.status("PixelU (Wii/GameCube) is open. Close it to save changes: it saves its own settings when it closes.")
	default:
		d.status("Pick a button, then click \"Press to set\" (or double-click it) and press what should work it. " +
			"You can also type in the Mapped to box: A | B means either one.")
	}
}

func fillCombo(c uintptr, labels []string, sel int) {
	call(pSendMessageW, c, cbResetContent, 0, 0)
	for _, l := range labels {
		call(pSendMessageW, c, cbAddString, 0, uintptr(unsafe.Pointer(u16(l))))
	}
	call(pSendMessageW, c, cbSetCurSel, uintptr(sel), 0)
}

// choices: the options, plus the current value if it isn't one of them.
func choices(opts []dolphin.Option, cur string) ([]dolphin.Option, int) {
	out := append([]dolphin.Option(nil), opts...)
	for i, o := range out {
		if o.Value == cur {
			return out, i
		}
	}
	return append(out, dolphin.Option{Value: cur, Label: cur + " (set in PixelU's files)"}), len(out)
}

func labels(opts []dolphin.Option) []string {
	var out []string
	for _, o := range opts {
		out = append(out, o.Label)
	}
	return out
}

// show switches to controller i.
func (d *dolPadMapDlg) show(i int) {
	d.stopWait("")
	d.cur = i
	p := d.maps[i]
	call(pSendMessageW, d.ctl[idPmPad], cbSetCurSel, uintptr(i), 0)
	var sel int
	if p.Kind == dolphin.KindGC {
		d.plugs, sel = choices(dolphin.GCPortDevices, p.Plugged)
	} else {
		d.plugs, sel = choices(dolphin.WiimoteSources, p.Plugged)
	}
	fillCombo(d.ctl[idPmPlug], labels(d.plugs), sel)
	wii := p.Kind == dolphin.KindWiimote
	for _, id := range []int{idPmExt, idPmExtLabel} {
		call(pShowWindow, d.ctl[id], uintptr(boolArg(wii)*swShow))
	}
	if wii {
		d.exts, sel = choices(dolphin.Extensions, p.Ext)
		fillCombo(d.ctl[idPmExt], labels(d.exts), sel)
	}
	d.devs = nil
	for n := 0; n < 4; n++ {
		d.devs = append(d.devs, dolphin.XInputPad(n))
	}
	d.devs = append(d.devs, dolphin.KeyboardDevice)
	sel = -1
	var devLabels []string
	for k, dev := range d.devs {
		devLabels = append(devLabels, dolphin.DeviceLabel(dev))
		if dev == p.Device {
			sel = k
		}
	}
	if sel < 0 {
		d.devs = append(d.devs, p.Device)
		devLabels = append(devLabels, p.Device+" (set in PixelU's files)")
		sel = len(d.devs) - 1
	}
	fillCombo(d.ctl[idPmDev], devLabels, sel)
	d.fillList(0)
	d.idleStatus()
}

// fillList lists the current controller's buttons and selects row sel.
func (d *dolPadMapDlg) fillList(sel int) {
	p := d.maps[d.cur]
	d.rows = p.Controls()
	lv := d.ctl[idPmList]
	call(pSendMessageW, lv, wmSetRedraw, 0, 0)
	call(pSendMessageW, lv, lvmDeleteAll, 0, 0)
	for i, c := range d.rows {
		it := lvItem{Mask: 1, Item: int32(i), Text: u16(c.Label)}
		call(pSendMessageW, lv, lvmInsertItemW, 0, uintptr(unsafe.Pointer(&it)))
		d.setRowText(i)
	}
	call(pSendMessageW, lv, wmSetRedraw, 1, 0)
	call(pInvalidateRect, lv, 0, 1)
	if sel >= len(d.rows) {
		sel = len(d.rows) - 1
	}
	if sel >= 0 {
		d.selectRow(sel)
	}
	d.showExpr()
}

func (d *dolPadMapDlg) setRowText(i int) {
	p := d.maps[d.cur]
	text := dolphin.DescribeInput(p.Device, p.Values[d.rows[i].Key])
	if text == "" {
		text = "—"
	}
	if i == d.waiting {
		text = "Press…"
	}
	sub := lvItem{Mask: 1, Item: int32(i), SubItem: 1, Text: u16(text)}
	call(pSendMessageW, d.ctl[idPmList], lvmSetItemTextW, uintptr(i), uintptr(unsafe.Pointer(&sub)))
}

func (d *dolPadMapDlg) selected() int {
	i := int(int32(call(pSendMessageW, d.ctl[idPmList], lvmGetNextItem, ^uintptr(0), 2))) // LVNI_SELECTED
	if i >= len(d.rows) {
		return -1
	}
	return i
}

func (d *dolPadMapDlg) selectRow(i int) {
	lv := d.ctl[idPmList]
	clear := lvItem{StateMask: 3}
	call(pSendMessageW, lv, lvmSetItemState, ^uintptr(0), uintptr(unsafe.Pointer(&clear)))
	it := lvItem{State: 3, StateMask: 3}
	call(pSendMessageW, lv, lvmSetItemState, uintptr(i), uintptr(unsafe.Pointer(&it)))
	call(pSendMessageW, lv, lvmEnsureVisible, uintptr(i), 0)
}

// showExpr puts the selected row's expression in the Mapped to box.
func (d *dolPadMapDlg) showExpr() {
	v := ""
	if i := d.selected(); i >= 0 {
		v = d.maps[d.cur].Values[d.rows[i].Key]
	}
	d.quiet = true
	setWindowText(d.ctl[idPmExpr], v)
	d.quiet = false
}

// typed: the Mapped to box changed.
func (d *dolPadMapDlg) typed() {
	if d.quiet {
		return
	}
	if i := d.selected(); i >= 0 {
		d.maps[d.cur].Values[d.rows[i].Key] = windowString(d.ctl[idPmExpr])
		d.setRowText(i)
	}
}

func (d *dolPadMapDlg) set(i int, expr string) {
	d.maps[d.cur].Values[d.rows[i].Key] = expr
	d.setRowText(i)
	if i == d.selected() {
		d.showExpr()
	}
}

// startWait: row i takes the next button, stick or key pressed.
func (d *dolPadMapDlg) startWait(i int) {
	if i < 0 {
		return
	}
	d.stopWait("")
	d.waiting, d.waitLeft = i, pmWaitTicks
	d.before = d.a.xin.read()
	d.setRowText(i)
	setWindowText(d.ctl[idPmSet], "Waiting…")
	p := d.maps[d.cur]
	what := "a button or move a stick on any Xbox controller, or press a key"
	if p.Device == dolphin.KeyboardDevice {
		what = "a key, click a mouse button in this window or press a button on an Xbox controller"
	}
	d.status(fmt.Sprintf("%s: press %s. Esc cancels.", d.rows[i].Label, what))
	call(pSetFocus, d.ctl[idPmList])
}

func (d *dolPadMapDlg) stopWait(msg string) {
	if d.waiting < 0 {
		return
	}
	i := d.waiting
	d.waiting = -1
	setWindowText(d.ctl[idPmSet], "Press to set")
	if i < len(d.rows) {
		d.setRowText(i)
	}
	if msg != "" {
		d.status(msg)
	} else {
		d.idleStatus()
	}
}

// pressed: input name on device dev was pressed while a row waits.
func (d *dolPadMapDlg) pressed(dev, name string) {
	i := d.waiting
	p := d.maps[d.cur]
	expr := dolphin.QuoteInput(name)
	_, padDev := dolphin.IsXInput(dev)
	_, padMap := dolphin.IsXInput(p.Device)
	if dev != p.Device && !(padDev && padMap) {
		expr = dolphin.OtherDeviceInput(dev, name) // a key for an Xbox controller's button, or the other way round
	}
	d.stopWait("")
	d.set(i, expr)
	d.status(d.rows[i].Label + " = " + dolphin.DescribeInput(p.Device, expr) + ". Save keeps it.")
	if i+1 < len(d.rows) {
		d.selectRow(i + 1)
		d.showExpr()
	}
}

// capturing takes key presses and mouse clicks while a row waits (from the
// message loop, before the focused control can use them).
func (d *dolPadMapDlg) capturing(m *msg) bool {
	if d.waiting < 0 || (m.Hwnd != d.hwnd && call(pGetAncestor, m.Hwnd, 2 /* GA_ROOT */) != d.hwnd) {
		return false
	}
	switch m.Message {
	case wmKeyDown, wmSysKeyDown:
		vk := int(m.WParam)
		if vk == vkEscape {
			d.stopWait("")
			return true
		}
		if n := dolKeyInput(vk, m.LParam); n != "" {
			d.pressed(dolphin.KeyboardDevice, n)
		}
		return true
	case 0x0101, 0x0105, 0x0102, 0x0106: // key ups and characters
		return true
	}
	if n, ok := dolMouseInputs[m.Message]; ok && d.maps[d.cur].Device == dolphin.KeyboardDevice &&
		m.Hwnd != d.ctl[idPmSet] && m.Hwnd != d.ctl[idPmCancel] { // those two stop waiting
		d.pressed(dolphin.KeyboardDevice, n)
		return true
	}
	return false
}

func (d *dolPadMapDlg) poll() {
	if d.waiting < 0 {
		return
	}
	now := d.a.xin.read()
	for n := range now {
		if name := dolPadPressed(d.before[n], now[n]); name != "" {
			d.pressed(dolphin.XInputPad(n), name)
			return
		}
	}
	d.before = now
	if d.waitLeft--; d.waitLeft <= 0 {
		d.stopWait("Nothing was pressed. Click \"Press to set\" to try again.")
	}
}

// deviceChanged: the Play it with list changed.
func (d *dolPadMapDlg) deviceChanged() {
	k := int(int32(call(pSendMessageW, d.ctl[idPmDev], cbGetCurSel, 0, 0)))
	if k < 0 || k >= len(d.devs) {
		return
	}
	p := d.maps[d.cur]
	dev := d.devs[k]
	if dev == p.Device {
		return
	}
	_, wasPad := dolphin.IsXInput(p.Device)
	_, isPad := dolphin.IsXInput(dev)
	p.Device = dev
	if wasPad && isPad {
		d.fillList(d.selected()) // same buttons, another controller
		d.status(p.Title() + " now plays with " + dolphin.DeviceLabel(dev) + ".")
		return
	}
	p.Defaults()
	d.fillList(0)
	d.status("Filled in the default layout for " + dolphin.DeviceLabel(dev) + ". Change any button you like, then Save.")
}

func (d *dolPadMapDlg) save() bool {
	if d.a.dol.run != nil || dolphin.Running(filepath.Base(d.exe)) {
		messageBox(d.hwnd, "PixelU (Wii/GameCube) is open. Close it first: it saves its own settings when it closes.", "Map controller buttons", mbIconInfo)
		return false
	}
	var changed []*dolphin.PadMap
	for i, p := range d.maps {
		if !p.Equal(d.orig[i]) { // (one never set up is saved with the layout shown)
			changed = append(changed, p)
		}
	}
	if len(changed) == 0 {
		return true
	}
	if err := dolphin.SavePadMaps(d.user, changed); err != nil {
		messageBox(d.hwnd, "Couldn't save the controller set-up: "+err.Error(), "Map controller buttons", mbIconWarning)
		return false
	}
	d.a.showMessage("Controller buttons saved")
	return true
}

func (d *dolPadMapDlg) close() {
	a := d.a
	call(pKillTimer, d.hwnd, timerPmPad)
	owner := call(pGetParent, d.hwnd)
	call(pDestroyWindow, d.hwnd)
	a.dol.pdlg = nil
	if owner != 0 {
		call(pSetForegroundWindow, owner)
	}
}

// cancel closes, asking first when something changed.
func (d *dolPadMapDlg) cancel() {
	for i, p := range d.maps {
		if !p.Equal(d.orig[i]) && d.orig[i].Device != "" {
			if messageBox(d.hwnd, "Close without saving your changes?", "Map controller buttons", mbYesNo|mbIconInfo) != idYes {
				return
			}
			break
		}
	}
	d.close()
}

func padMapProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.dol.pdlg == nil || a.dol.pdlg.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.dol.pdlg
	switch uint32(m) {
	case wmCommand:
		id, code := int(loword(wp)&0xffff), uint32(hiword(wp)&0xffff)
		switch id {
		case idPmPad:
			if code == cbnSelChange {
				if k := int(int32(call(pSendMessageW, d.ctl[idPmPad], cbGetCurSel, 0, 0))); k >= 0 && k < len(d.maps) {
					d.show(k)
				}
			}
		case idPmPlug:
			if code == cbnSelChange {
				if k := int(int32(call(pSendMessageW, d.ctl[idPmPlug], cbGetCurSel, 0, 0))); k >= 0 && k < len(d.plugs) {
					d.maps[d.cur].Plugged = d.plugs[k].Value
				}
			}
		case idPmExt:
			if code == cbnSelChange {
				if k := int(int32(call(pSendMessageW, d.ctl[idPmExt], cbGetCurSel, 0, 0))); k >= 0 && k < len(d.exts) {
					d.stopWait("")
					d.maps[d.cur].Ext = d.exts[k].Value
					d.fillList(d.selected())
				}
			}
		case idPmDev:
			if code == cbnSelChange {
				d.stopWait("")
				d.deviceChanged()
			}
		case idPmExpr:
			if code == enChange {
				d.typed()
			}
		case idPmSet:
			if d.waiting >= 0 {
				d.stopWait("")
			} else {
				d.startWait(d.selected())
			}
		case idPmClear:
			d.stopWait("")
			if i := d.selected(); i >= 0 {
				d.set(i, "")
			}
		case idPmDefault:
			d.stopWait("")
			d.maps[d.cur].Defaults()
			d.fillList(d.selected())
			d.status("Default layout for " + dolphin.DeviceLabel(d.maps[d.cur].Device) + " filled in. Save keeps it.")
		case idPmClearAll:
			d.stopWait("")
			d.maps[d.cur].Clear()
			d.fillList(d.selected())
		case idOK: // Enter: on the list it waits for a press
			if call(pGetFocus) == d.ctl[idPmList] {
				d.startWait(d.selected())
			}
		case idPmSave:
			if d.locked {
				d.close()
			} else if d.save() {
				d.close()
			}
		case idPmCancel, 2: // IDCANCEL (Esc)
			if d.waiting >= 0 {
				d.stopWait("")
			} else {
				d.cancel()
			}
		}
		return 0
	case wmNotify:
		h := *(**nmHdr)(unsafe.Pointer(&lp))
		if h.ID == idPmList {
			switch h.Code {
			case lvnItemChanged:
				if d.waiting < 0 {
					d.showExpr()
				}
			case nmDblClk:
				d.startWait(d.selected())
			}
		}
		return 0
	case wmTimer:
		if wp == timerPmPad {
			d.poll()
		}
		return 0
	case wmClose:
		d.cancel()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}
