package main

// The Save states window (Game > Save states, F6): the web page's Save
// states card. Four slots, each with a picture of the game and the time it
// was saved, Save, Load, Export (the web page's Download) and Delete; Undo
// last load and Import state file. Clicking a slot's picture makes it the
// slot F2 and F4 use. The game waits while it is open, as with Controls.

import (
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

const (
	idStSave0   = 2000 // + slot*4 + 0 save, 1 load, 2 export, 3 delete
	idStUndo    = 2100
	idStImport  = 2101
	idStClose   = 2102
	stSlotW     = 172
	stThumbH    = 129
	stPad       = 14
	stGap       = 12
	stTop       = 52
	stFooterTop = stTop + stThumbH + 30 + 34 + 28 + 14
)

var (
	statesClassDone bool
	statesCallback  = syscall.NewCallback(statesProc)
)

type statesThumb struct {
	pix  []uint32
	w, h int
}

type statesDialog struct {
	a       *app
	hwnd    uintptr
	slots   [slotCount]slotInfo
	thumbs  [slotCount]statesThumb
	buttons [slotCount][4]uintptr
	undo    uintptr
}

func (a *app) openStates() {
	s := a.sess
	if s == nil {
		return
	}
	if a.ui.states != nil {
		call(pSetForegroundWindow, a.ui.states.hwnd)
		return
	}
	cls := u16("RetroPixelsNativeStates")
	if !statesClassDone {
		wc := wndClassEx{WndProc: statesCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			Background: 16, ClassName: cls} // COLOR_BTNFACE + 1
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		statesClassDone = true
	}
	d := &statesDialog{a: a}
	a.ui.states = d
	s.modal.Store(true)
	s.snd.Clear()
	s.clearKeys()
	cw := 2*stPad + slotCount*stSlotW + (slotCount-1)*stGap
	ch := stFooterTop + 64 + 10
	// the window's size for that client area
	r := winRect{0, 0, a.sc(cw), a.sc(ch)}
	style := uintptr(wsPopup | wsCaption | wsSysMenu | wsVisible | wsClipChildren)
	call(pAdjustWindowRectEx, uintptr(unsafe.Pointer(&r)), style, 0, wsExDlgModalFrame)
	w, h := int(r.Right-r.Left), int(r.Bottom-r.Top)
	wr := windowRect(a.hwnd)
	x := int(wr.Left) + (int(wr.Right-wr.Left)-w)/2
	y := int(wr.Top) + (int(wr.Bottom-wr.Top)-h)/2
	title := "Save states - " + s.game.Name
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16(title))),
		style, uintptr(max(x, 0)), uintptr(max(y, 0)), uintptr(w), uintptr(h), a.hwnd, 0, a.hinst, 0)
	call(pEnableWindow, a.hwnd, 0)
	d.build()
	d.refresh()
}

func (d *statesDialog) child(class, text string, style uint32, x, y, w, h int, id int) uintptr {
	a := d.a
	c := call(pCreateWindowExW, 0, uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(wsChild|wsVisible|style), uintptr(a.sc(x)), uintptr(a.sc(y)), uintptr(a.sc(w)), uintptr(a.sc(h)), d.hwnd, uintptr(id), a.hinst, 0)
	call(pSendMessageW, c, wmSetFont, a.font, 1)
	return c
}

func slotX(i int) int { return stPad + i*(stSlotW+stGap) }

func (d *statesDialog) build() {
	cw := 2*stPad + slotCount*stSlotW + (slotCount-1)*stGap
	d.child("BUTTON", "Import state file…", bsPushButton|wsTabStop, cw-stPad-150, 10, 150, 28, idStImport)
	d.undo = d.child("BUTTON", "Undo last load", bsPushButton|wsTabStop, cw-stPad-150-8-130, 10, 130, 28, idStUndo)
	d.child("STATIC", "Click a picture to pick the slot F2 and F4 use.", ssNoPrefix, stPad, 16, cw-2*stPad-300, 22, 0)
	for i := 0; i < slotCount; i++ {
		x := slotX(i)
		y := stTop + stThumbH + 30
		d.buttons[i][0] = d.child("BUTTON", "Save", bsPushButton|wsTabStop, x, y, 82, 28, idStSave0+i*4)
		d.buttons[i][1] = d.child("BUTTON", "Load", bsPushButton|wsTabStop, x+90, y, 82, 28, idStSave0+i*4+1)
		d.buttons[i][2] = d.child("BUTTON", "Export…", bsPushButton|wsTabStop, x, y+34, 82, 28, idStSave0+i*4+2)
		d.buttons[i][3] = d.child("BUTTON", "Delete", bsPushButton|wsTabStop, x+90, y+34, 82, 28, idStSave0+i*4+3)
	}
	note := "F2 saves to the chosen slot, F4 loads it, F3 picks the next slot. Controller: click the right stick to save, " +
		"the left stick to load, flick the right stick left or right to change slot. The game's own battery saves keep working as normal."
	d.child("STATIC", note, ssNoPrefix, stPad, stFooterTop, cw-2*stPad-110, 64, 0)
	d.child("BUTTON", "Close", bsPushButton|wsTabStop, cw-stPad-90, stFooterTop+30, 90, 28, idStClose)
}

// refresh re-reads the slots and their pictures.
func (d *statesDialog) refresh() {
	s := d.a.sess
	if s == nil {
		return
	}
	d.slots = readSlots(s.game.System, s.game.Name)
	for i := range d.thumbs {
		d.thumbs[i] = statesThumb{}
		if d.slots[i].Used {
			pix, w, h := readThumb(slotThumbPath(s.game.System, s.game.Name, i))
			d.thumbs[i] = statesThumb{pix, w, h}
		}
		for k := 1; k < 4; k++ {
			call(pEnableWindow, d.buttons[i][k], boolArg(d.slots[i].Used))
		}
	}
	call(pEnableWindow, d.undo, boolArg(s.canUndo()))
	call(pInvalidateRect, d.hwnd, 0, 1)
}

func boolArg(b bool) uintptr {
	if b {
		return 1
	}
	return 0
}

func (d *statesDialog) thumbRect(i int) winRect {
	a := d.a
	x := slotX(i)
	return winRect{a.sc(x), a.sc(stTop), a.sc(x + stSlotW), a.sc(stTop + stThumbH)}
}

func (d *statesDialog) paint(hdc uintptr) {
	a := d.a
	call(pSetBkMode, hdc, transparent)
	cur := a.curSlot()
	now := time.Now()
	for i := 0; i < slotCount; i++ {
		r := d.thumbRect(i)
		bw := a.sc(1)
		border := uint32(0x808080)
		if i == cur {
			bw, border = max(2, a.sc(3)), colAccent
		}
		fillColor(hdc, winRect{r.Left - bw, r.Top - bw, r.Right + bw, r.Bottom + bw}, border)
		fillColor(hdc, r, 0x0e0b0a)
		t := d.thumbs[i]
		if t.pix != nil {
			fr := fitRect(int(r.Right-r.Left), int(r.Bottom-r.Top), float64(t.w)/float64(t.h))
			drawPixels(hdc, t.pix, t.w, t.h, winRect{r.Left + int32(fr.X), r.Top + int32(fr.Y), r.Left + int32(fr.X+fr.W), r.Top + int32(fr.Y+fr.H)})
		} else {
			label := "Empty"
			if d.slots[i].Used {
				label = "Saved"
			}
			paintText(hdc, a.font, label, r, colMuted, dtCenter|dtVCenter|dtSingleLine)
		}
		lr := winRect{r.Left, r.Bottom + a.sc(4), r.Right, r.Bottom + a.sc(26)}
		paintText(hdc, a.ui.bold, fmt.Sprintf("Slot %d", i+1), lr, 0, dtLeft|dtVCenter|dtSingleLine)
		paintText(hdc, a.font, slotTime(d.slots[i].Time, now), lr, 0x505050, dtRight|dtVCenter|dtSingleLine)
	}
}

func (d *statesDialog) close() {
	a := d.a
	call(pEnableWindow, a.hwnd, 1)
	call(pDestroyWindow, d.hwnd)
	a.ui.states = nil
	call(pSetForegroundWindow, a.hwnd)
	if a.sess != nil {
		a.sess.modal.Store(false)
	}
}

// uiCloseGame: the game is closing (closeGame): the Save states window goes too.
func (a *app) uiCloseGame() {
	if a.ui != nil && a.ui.states != nil {
		a.ui.states.close()
	}
}

func (d *statesDialog) command(id int) {
	a := d.a
	s := a.sess
	if s == nil {
		d.close()
		return
	}
	switch {
	case id == idStClose:
		d.close()
	case id == idStUndo:
		if err := s.undoLoad(); err != nil {
			a.showMessage("Nothing to undo")
		} else {
			a.showMessage("Back to before the last load")
		}
		d.refresh()
	case id == idStImport:
		a.importStateFile(d.hwnd, "")
		d.refresh()
	case id >= idStSave0 && id < idStSave0+slotCount*4:
		i, k := (id-idStSave0)/4, (id-idStSave0)%4
		switch k {
		case 0:
			a.ui.slot = i
			a.saveToSlot()
		case 1:
			a.ui.slot = i
			a.loadFromSlot()
		case 2:
			d.export(i)
		case 3:
			if messageBox(d.hwnd, fmt.Sprintf("Delete the save state in slot %d?", i+1), "Save states", mbYesNo|mbIconWarning) == idYes {
				if err := deleteSlot(s.game.System, s.game.Name, i); err != nil {
					messageBox(d.hwnd, "Couldn't delete it: "+err.Error(), "Save states", mbIconWarning)
				} else {
					a.showMessage(fmt.Sprintf("Slot %d cleared", i+1))
				}
			}
		}
		d.refresh()
	}
}

// export copies a slot's save state to a file (the web page's Download).
func (d *statesDialog) export(i int) {
	a := d.a
	s := a.sess
	data, err := os.ReadFile(slotPath(s.game.System, s.game.Name, i))
	if err != nil {
		messageBox(d.hwnd, "Couldn't read the save state: "+err.Error(), "Save states", mbIconWarning)
		return
	}
	name := fmt.Sprintf("%s - slot %d.state", safeName(s.game.Name), i+1)
	p := fileDialog(d.hwnd, true, "Export save state", "Save states (*.state)\x00*.state\x00All files\x00*.*", a.settings.LastFolder, name, "state")
	if p == "" {
		return
	}
	if err := writeFileAtomic(p, data); err != nil {
		messageBox(d.hwnd, "Couldn't save the file: "+err.Error(), "Save states", mbIconWarning)
		return
	}
	a.showMessage("Save state exported: " + p)
}

func statesProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.ui == nil || a.ui.states == nil || a.ui.states.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.ui.states
	switch uint32(m) {
	case wmPaint:
		var ps paintStruct
		hdc := call(pBeginPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		d.paint(hdc)
		call(pEndPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		return 0
	case wmCommand:
		if hiword(wp)&0xffff == bnClicked {
			d.command(loword(wp) & 0xffff)
		}
		return 0
	case wmLButtonDown:
		x, y := loword(lp), hiword(lp)
		for i := 0; i < slotCount; i++ {
			if d.thumbRect(i).contains(x, y) {
				a.setSlot(i)
			}
		}
		return 0
	case wmClose:
		d.close()
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}
