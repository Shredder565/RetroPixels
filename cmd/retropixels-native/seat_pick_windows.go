package main

// The "Choose your player" window: when an arcade game for 3 or 4 players
// starts (TMNT, Turtles in Time, The Simpsons, Double Dragon 3, X-Men), a small
// window over the game shows one tile per player, like the web page's
// "Choose your player" buttons. Click a tile, press 1-4, or move with the
// arrow keys / the controller's D-pad or stick and press Enter / A. Esc or
// B keeps the player picked last time. The game waits while it is open.
// The Player menu, F7 and the right stick still change it later.

import (
	"fmt"
	"syscall"
	"unsafe"
)

const (
	timerSeatPad = 31
	spTileW      = 150
	spTileH      = 96
	spGap        = 12
	spPad        = 18
	spTop        = 58
	spHintH      = 40

	vkLeftArrow, vkUpArrow, vkRightArrow, vkDownArrow = 0x25, 0x26, 0x27, 0x28
)

var (
	seatPickClassDone bool
	seatPickCallback  = syscall.NewCallback(seatPickProc)
)

type seatPicker struct {
	a     *app
	s     *session
	hwnd  uintptr
	n     int // players
	sel   int // the highlighted tile
	names []string
	prev  PadState
	ready bool // the controller's buttons were all up once (A held from the menu doesn't pick)
}

// showSeatPicker opens the window for a game with seats (startGame).
func (a *app) showSeatPicker(s *session) {
	n := s.seats()
	if n == 0 || a.ui.seatPick != nil {
		return
	}
	cls := u16("RetroPixelsNativeSeat")
	if !seatPickClassDone {
		wc := wndClassEx{WndProc: seatPickCallback, Instance: a.hinst, Cursor: call(pLoadCursorW, 0, idcArrow),
			ClassName: cls}
		wc.Size = uint32(unsafe.Sizeof(wc))
		call(pRegisterClassExW, uintptr(unsafe.Pointer(&wc)))
		seatPickClassDone = true
	}
	d := &seatPicker{a: a, s: s, n: n, sel: int(s.seat.Load())}
	if c, ok := s.core.(characterCore); ok {
		d.names = c.Characters()
	}
	a.ui.seatPick = d
	s.modal.Store(true)
	s.snd.Clear()
	s.clearKeys()
	cw := 2*spPad + n*spTileW + (n-1)*spGap
	ch := spTop + spTileH + 14 + spHintH
	r := winRect{0, 0, a.sc(cw), a.sc(ch)}
	style := uintptr(wsPopup | wsCaption | wsSysMenu | wsVisible)
	call(pAdjustWindowRectEx, uintptr(unsafe.Pointer(&r)), style, 0, wsExDlgModalFrame)
	w, h := int(r.Right-r.Left), int(r.Bottom-r.Top)
	wr := windowRect(a.hwnd)
	x := int(wr.Left) + (int(wr.Right-wr.Left)-w)/2
	y := int(wr.Top) + (int(wr.Bottom-wr.Top)-h)/2
	d.hwnd = call(pCreateWindowExW, wsExDlgModalFrame, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("Choose your player - "+s.game.Name))),
		style, uintptr(max(x, 0)), uintptr(max(y, 0)), uintptr(w), uintptr(h), a.hwnd, 0, a.hinst, 0)
	call(pEnableWindow, a.hwnd, 0)
	call(pSetFocus, d.hwnd)
	call(pSetTimer, d.hwnd, timerSeatPad, 16, 0)
}

func (d *seatPicker) label(i int) string {
	if i < len(d.names) && d.names[i] != "" {
		return d.names[i]
	}
	return fmt.Sprintf("Player %d", i+1)
}

func (d *seatPicker) tileRect(i int) winRect {
	a := d.a
	x := spPad + i*(spTileW+spGap)
	return winRect{a.sc(x), a.sc(spTop), a.sc(x + spTileW), a.sc(spTop + spTileH)}
}

func (d *seatPicker) paint(hdc uintptr) {
	a := d.a
	var cr winRect
	call(pGetClientRect, d.hwnd, uintptr(unsafe.Pointer(&cr)))
	fillColor(hdc, cr, colBg)
	call(pSetBkMode, hdc, transparent)
	paintText(hdc, a.ui.bold, "Choose your player", winRect{a.sc(spPad), a.sc(12), cr.Right - a.sc(spPad), a.sc(36)}, colText, dtLeft|dtVCenter|dtSingleLine)
	paintText(hdc, a.ui.small, "The keyboard and the first controller play as the player you pick.",
		winRect{a.sc(spPad), a.sc(34), cr.Right - a.sc(spPad), a.sc(54)}, colMuted, dtLeft|dtVCenter|dtSingleLine)
	cur := int(d.s.seat.Load())
	for i := 0; i < d.n; i++ {
		r := d.tileRect(i)
		fill, border := uint32(colPanel), int64(colLine)
		if i == d.sel {
			fill, border = colAccent, int64(colAccent2)
		}
		roundBox(hdc, r, a.sc(10), fill, border, int32(max(1, a.sc(2))))
		paintText(hdc, a.ui.med, fmt.Sprint(i+1), winRect{r.Left, r.Top + a.sc(10), r.Right, r.Top + a.sc(46)}, colText, dtCenter|dtVCenter|dtSingleLine)
		paintText(hdc, a.ui.bold, d.label(i), winRect{r.Left + a.sc(6), r.Top + a.sc(48), r.Right - a.sc(6), r.Top + a.sc(72)}, colText, dtCenter|dtVCenter|dtSingleLine)
		if i == cur {
			paintText(hdc, a.ui.small, "last time", winRect{r.Left, r.Top + a.sc(70), r.Right, r.Bottom - a.sc(4)}, colMuted, dtCenter|dtVCenter|dtSingleLine)
		}
	}
	hint := "Click a player, press 1-" + fmt.Sprint(d.n) + ", or move with the arrows / D-pad and press Enter / A. " +
		"Change it later: Player menu, F7 or the right stick up."
	paintText(hdc, a.ui.small, hint, winRect{a.sc(spPad), a.sc(spTop + spTileH + 10), cr.Right - a.sc(spPad), cr.Bottom - a.sc(4)}, colMuted, dtLeft|dtWordBreak)
}

func (d *seatPicker) move(dir int) {
	d.sel = (d.sel + dir + d.n) % d.n
	call(pInvalidateRect, d.hwnd, 0, 0)
}

// pick plays as player n (n < 0: keep the current one) and closes.
func (d *seatPicker) pick(n int) {
	a := d.a
	call(pKillTimer, d.hwnd, timerSeatPad)
	call(pEnableWindow, a.hwnd, 1)
	call(pDestroyWindow, d.hwnd)
	a.ui.seatPick = nil
	call(pSetForegroundWindow, a.hwnd)
	if a.sess != d.s {
		return
	}
	d.s.modal.Store(false)
	if n < 0 {
		n = int(d.s.seat.Load())
	}
	a.pickSeat(n)
}

// padTick: the controller moves the highlight and picks.
func (d *seatPicker) padTick() {
	p := firstPad(d.a.pads())
	pressed := func(b int) bool { return p.Buttons[b] && !d.prev.Buttons[b] }
	left := p.Buttons[padLeft] || p.Axes[0] < -0.6
	right := p.Buttons[padRight] || p.Axes[0] > 0.6
	wasLeft := d.prev.Buttons[padLeft] || d.prev.Axes[0] < -0.6
	wasRight := d.prev.Buttons[padRight] || d.prev.Axes[0] > 0.6
	if !d.ready {
		d.ready = !p.Buttons[padA] && !p.Buttons[padB] && !p.Buttons[padMenu]
		d.prev = p
		return
	}
	d.prev = p
	switch {
	case pressed(padA) || pressed(padMenu):
		d.pick(d.sel)
	case pressed(padB):
		d.pick(-1)
	case left && !wasLeft:
		d.move(-1)
	case right && !wasRight:
		d.move(1)
	}
}

func (d *seatPicker) key(vk int) {
	switch {
	case vk >= '1' && vk < '1'+d.n:
		d.pick(vk - '1')
	case vk >= 0x61 && vk < 0x61+d.n: // number pad 1-4
		d.pick(vk - 0x61)
	case vk == vkLeftArrow || vk == vkUpArrow:
		d.move(-1)
	case vk == vkRightArrow || vk == vkDownArrow || vk == vkTab:
		d.move(1)
	case vk == vkReturn || vk == vkSpace:
		d.pick(d.sel)
	case vk == vkEscape:
		d.pick(-1)
	}
}

func seatPickProc(hwnd, m, wp, lp uintptr) uintptr {
	a := theApp
	if a == nil || a.ui == nil || a.ui.seatPick == nil || a.ui.seatPick.hwnd != hwnd {
		return call(pDefWindowProcW, hwnd, m, wp, lp)
	}
	d := a.ui.seatPick
	switch uint32(m) {
	case wmPaint:
		var ps paintStruct
		hdc := call(pBeginPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		d.paint(hdc)
		call(pEndPaint, hwnd, uintptr(unsafe.Pointer(&ps)))
		return 0
	case wmEraseBkgnd:
		return 1
	case wmKeyDown:
		d.key(int(wp))
		return 0
	case wmLButtonDown:
		x, y := loword(lp), hiword(lp)
		for i := 0; i < d.n; i++ {
			if d.tileRect(i).contains(x, y) {
				d.pick(i)
				return 0
			}
		}
		return 0
	case wmMouseMove:
		x, y := loword(lp), hiword(lp)
		for i := 0; i < d.n; i++ {
			if d.tileRect(i).contains(x, y) && i != d.sel {
				d.sel = i
				call(pInvalidateRect, hwnd, 0, 0)
			}
		}
		return 0
	case wmTimer:
		if wp == timerSeatPad {
			d.padTick()
		}
		return 0
	case wmClose:
		d.pick(-1)
		return 0
	}
	return call(pDefWindowProcW, hwnd, m, wp, lp)
}

// closeSeatPicker: the game is closing; the window goes too, quietly.
func (a *app) closeSeatPicker() {
	d := a.ui.seatPick
	if d == nil {
		return
	}
	call(pKillTimer, d.hwnd, timerSeatPad)
	call(pEnableWindow, a.hwnd, 1)
	call(pDestroyWindow, d.hwnd)
	a.ui.seatPick = nil
}
