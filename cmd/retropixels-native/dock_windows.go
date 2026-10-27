package main

// Games in an external emulator (Dolphin, MAME, PixelU) whose window is
// docked inside the RetroPixels Native window, as RetroPixels.exe's
// embedDolphin does for all of them. The window calls themselves are in
// internal/dolphin (GameWindow, Dock, Place, Close, Shutdown).

import (
	"sync/atomic"
	"time"

	"retropixels/internal/dolphin"
)

// extRun is what docking needs of one game in an external emulator; each
// emulator's run (dolRun, mameRun, pixeluRun) embeds one.
type extRun struct {
	pid    int
	exited chan struct{}  // closed once the emulator has ended
	child  atomic.Uintptr // the emulator's window while it's docked here
	stopAt atomic.Int64   // when Stop game was asked (Unix ns)
}

func newExtRun(pid int) extRun { return extRun{pid: pid, exited: make(chan struct{})} }

// stoppingFor tells whether Stop game was asked more than d ago.
func (r *extRun) stoppingFor(d time.Duration) bool {
	at := r.stopAt.Load()
	return at != 0 && time.Since(time.Unix(0, at)) > d
}

// stop ends the game the way the emulator's own close button does (it saves
// what it saves then); dockWatch closes the emulator if it doesn't react.
// Not docked: every window of it is asked to close, then it's ended.
func (r *extRun) stop() {
	r.stopAt.Store(time.Now().UnixNano())
	if c := r.child.Load(); c != 0 {
		dolphin.Close(c)
	} else {
		go dolphin.Shutdown(r.pid, r.exited, 10*time.Second)
	}
}

// dockWatch docks the emulator's largest visible window into this window and
// keeps it there, and makes sure the emulator closes completely once the
// game is stopped. It sends ev+"-docked" each time a window is docked, and
// ev+"-nodock" if none showed up within wait (the emulator is then left as
// it is).
func (a *app) dockWatch(r *extRun, wait time.Duration, ev string) {
	host := a.hwnd
	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	deadline := time.Now().Add(wait)
	var child uintptr
	var lostAt time.Time
	for {
		select {
		case <-r.exited:
			return
		case <-tick.C:
		}
		if child != 0 {
			if dolphin.Docked(child, host) {
				if r.stoppingFor(8 * time.Second) {
					dolphin.Shutdown(r.pid, r.exited, 3*time.Second) // Stop game was asked but the emulator didn't react
					return
				}
				a.dockPlace(child, false)
				continue
			}
			// The window went away: the game was stopped, or the emulator
			// made a new one (e.g. it switched fullscreen), docked below.
			child, lostAt = 0, time.Now()
			r.child.Store(0)
		}
		h := dolphin.GameWindow(r.pid)
		if h == 0 {
			if !lostAt.IsZero() && time.Since(lostAt) > 3*time.Second {
				// The game ended but the emulator is still running in the background.
				dolphin.Shutdown(r.pid, r.exited, 5*time.Second)
				return
			}
			if lostAt.IsZero() && time.Now().After(deadline) {
				a.event(ev+"-nodock", "")
				return
			}
			continue
		}
		dolphin.Dock(h, host)
		a.dockPlace(h, true)
		call(pSetForegroundWindow, host)
		dolphin.Focus(h)
		child, lostAt = h, time.Time{}
		r.child.Store(h)
		a.event(ev+"-docked", "")
	}
}

// dockPlace fits a docked window to this window.
func (a *app) dockPlace(child uintptr, force bool) {
	cr := clientRect(a.hwnd)
	if cr.Right > 0 && cr.Bottom > 0 {
		dolphin.Place(child, a.hwnd, 0, 0, cr.Right, cr.Bottom, force)
	}
}

// dockResize follows the window's size (WM_SIZE): whichever emulator's
// window is docked is fitted to it.
func (a *app) dockResize() {
	for _, r := range []*extRun{a.dolExt(), a.mameExt(), a.pixeluExt(), a.supermodelExt(), a.rpcs3Ext()} {
		if r != nil {
			if c := r.child.Load(); c != 0 {
				a.dockPlace(c, false)
			}
		}
	}
}

func (a *app) dolExt() *extRun {
	if r := a.dol.run; r != nil {
		return &r.extRun
	}
	return nil
}

func (a *app) mameExt() *extRun {
	if r := a.mame.run; r != nil {
		return &r.extRun
	}
	return nil
}

func (a *app) pixeluExt() *extRun {
	if r := a.pu.run; r != nil {
		return &r.extRun
	}
	return nil
}
