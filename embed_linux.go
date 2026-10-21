//go:build linux

package main

// Docking an engine's game screen into the RetroPixels window on Linux, the
// X11 side of embed_windows.go (on Wayland desktops the app window and the
// engines run under XWayland, see platform_linux.go).
//
// The engine is started as usual. As soon as its game screen appears it is
// taken from the window manager and put inside a window of RetroPixels' own
// (the "box"), sized to the page below RetroPixels' game bar:
//
//   - child mode (PixelU-Wii, MAME): the box is a child window of the
//     browser's app window, so the game moves, stacks and minimizes with it.
//   - overlay mode (PixelU, Supermodel, as on Windows): the box is a
//     frameless top-level window, transient for the app window (kept above
//     it, no task bar entry), moved along with the page.
//
// The engine's window is in our save-set, so if RetroPixels itself ends
// while a game is docked, the X server puts the game screen back on the
// desktop instead of destroying it with the box. Stop game asks the game
// screen to close (WM_DELETE_WINDOW, like the window manager's close button),
// and ends the engine with SIGTERM when it doesn't take that or doesn't react.
//
// RETROPIXELS_DOCK=child or overlay picks the mode for every engine (for
// trying things on a desktop), RETROPIXELS_DOCK=off leaves engines in their
// own windows.
//
// For engines: a window waiting to be docked may stay unmapped if it carries
// the property _RETROPIXELS_EMBED (CARDINAL, 1); otherwise the largest mapped
// top-level window of the engine's process (by _NET_WM_PID) is docked. A
// docked engine can tell it is docked by its parent window carrying
// _RETROPIXELS_DOCK (CARDINAL, RetroPixels' pid).

import (
	"errors"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"retropixels/internal/pixelu"
	"retropixels/internal/x11"
)

const (
	embedMarkProp = "_RETROPIXELS_EMBED" // on an engine's window: dock this one
	dockMarkProp  = "_RETROPIXELS_DOCK"  // on our box: RetroPixels' pid
)

// Timings, variables so the tests can shorten them.
var (
	dockTick      = 100 * time.Millisecond
	dockDeadline  = 90 * time.Second // slow disc / shader compile on first boot
	stopGrace     = 8 * time.Second  // Stop game clicked, engine still there: end it
	lostGrace     = 3 * time.Second  // game screen gone, engine still running: end it
	termGrace     = 3 * time.Second  // SIGTERM sent, still running: SIGKILL
	withdrawGrace = time.Second      // waiting for the window manager to let go of a window
	hostGoneGrace = 10 * time.Second // RetroPixels window closed, engine asked to close: end it
)

var (
	embedMu     sync.Mutex
	embedPID    int
	embedWin    x11.Window // the engine's game screen, while docked
	embedConn   *x11.Conn  // the docking session's connection
	embedTopPx  int        // device pixels the page's game bar takes at the top
	embedStopAt time.Time  // when Stop game was clicked
)

func embeddedWindow(pid int) uintptr {
	embedMu.Lock()
	defer embedMu.Unlock()
	if pid != embedPID || embedWin == 0 {
		return 0
	}
	return uintptr(embedWin)
}

func setEmbedTop(px int) {
	if px < 0 || px > 400 {
		return
	}
	embedMu.Lock()
	embedTopPx = px
	embedMu.Unlock()
}

func embedTop() int {
	embedMu.Lock()
	defer embedMu.Unlock()
	return embedTopPx
}

// dockMode is the RETROPIXELS_DOCK override: "child", "overlay", "off" or "".
func dockMode() string {
	return strings.ToLower(strings.TrimSpace(os.Getenv("RETROPIXELS_DOCK")))
}

func dialX() (*x11.Conn, error) {
	if os.Getenv("DISPLAY") == "" {
		return nil, errors.New("no X display")
	}
	return x11.Dial("")
}

// ---------------- finding windows ----------------

// topLevels are the windows a window manager manages (_NET_CLIENT_LIST),
// plus the root's children and grandchildren for a window manager without
// it (or none: then the root's children are the programs' windows).
func topLevels(c *x11.Conn) []x11.Window {
	seen := map[x11.Window]bool{}
	var out []x11.Window
	add := func(w x11.Window) {
		if w != 0 && !seen[w] {
			seen[w] = true
			out = append(out, w)
		}
	}
	if a, err := c.Atom("_NET_CLIENT_LIST"); err == nil {
		if v, err := c.PropertyUint32s(c.Root(), a); err == nil {
			for _, w := range v {
				add(x11.Window(w))
			}
		}
	}
	_, _, kids, err := c.QueryTree(c.Root())
	if err != nil {
		return out
	}
	for _, k := range kids {
		add(k)
		if _, _, gk, err := c.QueryTree(k); err == nil {
			for _, g := range gk {
				add(g)
			}
		}
	}
	return out
}

// findHost returns the RetroPixels app window: the browser's --app window
// (WM_CLASS class "RetroPixels", see openAppWindow), the active one if
// several are open.
func findHost(c *x11.Conn) x11.Window {
	mark, _ := c.Atom(dockMarkProp)
	me := os.Getpid()
	var found []x11.Window
	named := map[x11.Window]bool{}
	for _, w := range topLevels(c) {
		_, class, err := c.WMClass(w)
		if err != nil || !strings.EqualFold(class, appName) {
			continue
		}
		if a, err := c.GetWindowAttributes(w); err != nil || a.MapState != x11.IsViewable || a.Class != x11.InputOutput {
			continue
		}
		if pid, _ := c.WMPID(w); pid == me {
			continue // one of our boxes
		}
		if v, _ := c.PropertyUint32s(w, mark); len(v) > 0 {
			continue
		}
		if n, _ := c.WMName(w); n == appName {
			named[w] = true
		}
		found = append(found, w)
	}
	if len(found) == 0 {
		return 0
	}
	if act, err := c.ActiveWindow(); err == nil {
		for _, w := range found {
			if w == act {
				return w
			}
		}
	}
	for _, w := range found {
		if named[w] {
			return w
		}
	}
	return found[0]
}

// engineWindows are the top-level windows of pid's process tree.
func engineWindows(c *x11.Conn, pid int) []x11.Window {
	pids := processTree("/proc", pid)
	var out []x11.Window
	for p := range pids {
		ws, err := c.WindowsByPID(p)
		if err != nil {
			return out
		}
		out = append(out, ws...)
	}
	return out
}

// gameScreen returns the engine's window to dock: one marked with
// _RETROPIXELS_EMBED (mapped or not), else its largest mapped window.
func gameScreen(c *x11.Conn, pid int) x11.Window {
	mark, _ := c.Atom(embedMarkProp)
	var best, bestMarked x11.Window
	var bestArea, markedArea int
	for _, w := range engineWindows(c, pid) {
		a, err := c.GetWindowAttributes(w)
		if err != nil || a.OverrideRedirect || a.Class != x11.InputOutput {
			continue
		}
		g, err := c.GetGeometry(w)
		if err != nil {
			continue
		}
		area := int(g.Width) * int(g.Height)
		if v, _ := c.PropertyUint32s(w, mark); len(v) > 0 && v[0] != 0 {
			if area > markedArea || bestMarked == 0 {
				bestMarked, markedArea = w, area
			}
			continue
		}
		if a.MapState == x11.IsViewable && area > bestArea {
			best, bestArea = w, area
		}
	}
	if bestMarked != 0 {
		return bestMarked
	}
	if bestArea < 100*100 {
		return 0
	}
	return best
}

// ---------------- docking ----------------

type rect struct{ X, Y, W, H int }

// dockSession is one docked game screen.
type dockSession struct {
	c       *x11.Conn
	host    x11.Window
	overlay bool
	box     x11.Window    // our window that holds the game screen
	boxID   atomic.Uint32 // box, for the events goroutine
	child   x11.Window    // the engine's game screen
	shown   bool          // overlay: the box is mapped
	last    rect          // where the box was put last
}

// pageArea is where the game goes: the app window's inside below the game
// bar, in the host's coordinates. (Chrome's --app window has the window
// manager's frame, so its whole X window is the page.)
func (s *dockSession) pageArea() (rect, bool) {
	g, err := s.c.GetGeometry(s.host)
	if err != nil || g.Width < 50 || g.Height < 50 {
		return rect{}, false
	}
	top := embedTop()
	if top > int(g.Height)-50 {
		top = int(g.Height) - 50
	}
	return rect{0, top, int(g.Width), int(g.Height) - top}, true
}

// want is where the box belongs: in the host for a child box, on the root
// for an overlay.
func (s *dockSession) want() (rect, bool) {
	r, ok := s.pageArea()
	if !ok || !s.overlay {
		return r, ok
	}
	x, y, _, err := s.c.TranslateCoordinates(s.host, s.c.Root(), 0, 0)
	if err != nil {
		return rect{}, false
	}
	r.X += int(x)
	r.Y += int(y)
	return r, true
}

// makeBox creates our window that will hold the game screen.
func (s *dockSession) makeBox() error {
	c := s.c
	r, ok := s.want()
	if !ok {
		return errors.New("the RetroPixels window has no room")
	}
	parent := s.host
	mask := uint32(0)
	if s.overlay {
		parent = c.Root()
		mask = x11.StructureNotifyMask | x11.FocusChangeMask
	}
	box, err := c.CreateWindow(parent, int16(r.X), int16(r.Y), uint16(r.W), uint16(r.H), 0, &x11.WindowAttributes{
		Mask:      x11.CWBackPixel | x11.CWEventMask,
		BackPixel: c.DefaultScreen().BlackPixel,
		EventMask: mask,
	})
	if err != nil {
		return err
	}
	s.box, s.last = box, r
	s.boxID.Store(uint32(box))
	mark, err := c.Atom(dockMarkProp)
	if err != nil {
		return err
	}
	_ = c.SetPropertyUint32s(box, mark, x11.AtomCardinal, uint32(os.Getpid()))
	_ = c.SetWMName(box, appName+" game")
	_ = c.SetWMClass(box, "retropixels-game", appName)
	if !s.overlay {
		return c.MapWindow(box)
	}
	// A frameless window kept above the app window, out of the task bar,
	// placed exactly where it's asked to be.
	_ = c.SetWMPID(box, os.Getpid())
	_ = c.SetTransientFor(box, s.host)
	_ = c.SetWMState(box, "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER")
	_ = c.SetWMProtocols(box, "WM_DELETE_WINDOW")
	if at, err := c.Atoms("_MOTIF_WM_HINTS", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL"); err == nil {
		_ = c.SetPropertyUint32s(box, at[0], at[0], 2 /* decorations */, 0, 0, 0, 0)
		_ = c.SetPropertyUint32s(box, at[1], x11.AtomAtom, uint32(at[2]))
	}
	_ = c.SetPropertyUint32s(box, x11.AtomWMHints, x11.AtomWMHints, 1 /* InputHint */, 1, 0, 0, 0, 0, 0, 0, 0)
	s.setSizeHints(r)
	return nil
}

// setSizeHints: USPosition|USSize|PPosition|PSize|PWinGravity, StaticGravity,
// so a window manager puts the box's inside where we say.
func (s *dockSession) setSizeHints(r rect) {
	v := make([]uint32, 18)
	v[0] = 1 | 2 | 4 | 8 | 512
	v[1], v[2], v[3], v[4] = uint32(int32(r.X)), uint32(int32(r.Y)), uint32(r.W), uint32(r.H)
	v[17] = 10
	_ = s.c.SetPropertyUint32s(s.box, x11.AtomWMNormalHints, x11.AtomWMSizeHints, v...)
}

// withdraw takes w from the window manager: unmap it (with the synthetic
// UnmapNotify ICCCM asks for) and wait until the manager has put it back on
// the root.
func withdraw(c *x11.Conn, w x11.Window) {
	_, parent, _, err := c.QueryTree(w)
	if err != nil || parent == c.Root() {
		if a, err := c.GetWindowAttributes(w); err == nil && a.MapState != x11.IsUnmapped {
			_ = c.UnmapWindow(w)
		}
		return
	}
	_ = c.UnmapWindow(w)
	var ev [32]byte
	ev[0] = x11.UnmapNotify
	putU32(ev[4:], uint32(c.Root()))
	putU32(ev[8:], uint32(w))
	_ = c.SendEvent(c.Root(), false, x11.SubstructureRedirectMask|x11.SubstructureNotifyMask, ev)
	deadline := time.Now().Add(withdrawGrace)
	for time.Now().Before(deadline) {
		if _, p, _, err := c.QueryTree(w); err != nil || p == c.Root() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func putU32(b []byte, v uint32) {
	b[0], b[1], b[2], b[3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
}

// dock puts the engine's window w into the box (making the box first).
func (s *dockSession) dock(w x11.Window) error {
	c := s.c
	if s.box == 0 {
		if err := s.makeBox(); err != nil {
			return err
		}
	}
	withdraw(c, w)
	if err := c.ChangeSaveSet(x11.SetModeInsert, w); err != nil {
		return err
	}
	if err := c.ReparentWindow(w, s.box, 0, 0); err != nil {
		return err
	}
	_ = c.ConfigureWindow(w, x11.WindowChanges{Mask: x11.ConfigX | x11.ConfigY | x11.ConfigWidth | x11.ConfigHeight | x11.ConfigBorderWidth,
		Width: uint32(max(s.last.W, 1)), Height: uint32(max(s.last.H, 1))})
	_ = c.MapWindow(w)
	if s.overlay {
		_ = c.MapWindow(s.box)
		s.shown = true
	} else {
		_ = c.RaiseWindow(s.box)
	}
	if err := c.Sync(); err != nil {
		return err
	}
	s.child = w
	s.focus()
	return nil
}

// focus gives the game screen the keyboard: the app window (or the overlay)
// is activated through the window manager, then the focus goes to the game.
func (s *dockSession) focus() {
	c := s.c
	top := s.host
	if s.overlay {
		top = s.box
	}
	_ = c.ActivateWindow(top)
	go func(child x11.Window) {
		// After the window manager has done its focusing.
		time.Sleep(200 * time.Millisecond)
		if a, err := c.GetWindowAttributes(child); err == nil && a.MapState == x11.IsViewable {
			_ = c.SetInputFocus(child, x11.RevertToParent, x11.CurrentTime)
		}
	}(s.child)
}

// isDocked: the game screen still exists and is still in our box.
func (s *dockSession) isDocked() bool {
	if s.child == 0 {
		return false
	}
	_, p, _, err := s.c.QueryTree(s.child)
	return err == nil && p == s.box
}

// hostAlive: the app window still exists; viewable says it is on screen
// (not minimized).
func (s *dockSession) hostAlive() (alive, viewable bool) {
	a, err := s.c.GetWindowAttributes(s.host)
	if err != nil {
		return !x11.WindowGone(err), false
	}
	return true, a.MapState == x11.IsViewable
}

// place keeps the box over the page area below the game bar and the game
// screen the size of the box; force puts both there even if nothing moved.
func (s *dockSession) place(force bool) {
	c := s.c
	if s.overlay {
		_, viewable := s.hostAlive()
		if !viewable {
			if s.shown {
				_ = c.UnmapWindow(s.box) // the app window was minimized
				s.shown = false
			}
			return
		}
		if !s.shown {
			_ = c.MapWindow(s.box)
			s.shown = true
			force = true
		}
	}
	r, ok := s.want()
	if !ok {
		return
	}
	if force || r != s.last {
		if s.overlay {
			s.setSizeHints(r)
		}
		_ = c.MoveResizeWindow(s.box, r.X, r.Y, r.W, r.H)
		s.last = r
	}
	if g, err := c.GetGeometry(s.child); err == nil &&
		(force || g.X != 0 || g.Y != 0 || int(g.Width) != r.W || int(g.Height) != r.H || g.BorderWidth != 0) {
		_ = c.ConfigureWindow(s.child, x11.WindowChanges{Mask: x11.ConfigX | x11.ConfigY | x11.ConfigWidth | x11.ConfigHeight | x11.ConfigBorderWidth,
			Width: uint32(max(r.W, 1)), Height: uint32(max(r.H, 1))})
	}
	if !s.overlay {
		// Keep it above the browser's own child windows (its GPU surface).
		if _, _, kids, err := c.QueryTree(s.host); err == nil && len(kids) > 0 && kids[len(kids)-1] != s.box {
			_ = c.RaiseWindow(s.box)
		}
	}
}

// events handles what the box gets: an overlay that the window manager
// focuses passes the keyboard on to the game screen; a close request on it
// (Alt+F4) is Stop game.
func (s *dockSession) events() {
	c := s.c
	for e := range c.Events() {
		switch e := e.(type) {
		case *x11.FocusEvent:
			// Not when the focus comes back from the game screen (NotifyInferior)
			// or for the pointer (NotifyPointer and after).
			if s.overlay && e.Code() == x11.FocusIn && e.Window == x11.Window(s.boxID.Load()) && e.Detail != 2 && e.Detail < 5 {
				embedMu.Lock()
				child := embedWin
				embedMu.Unlock()
				if child != 0 {
					_ = c.SetInputFocus(child, x11.RevertToParent, x11.CurrentTime)
				}
			}
		case *x11.ClientMessageEvent:
			if e.Window == x11.Window(s.boxID.Load()) && c.IsDeleteWindow(e) {
				go stopEmbeddedGame()
			}
		}
	}
}

// embedPixelU follows one PixelU (Wii U) run, its game screen over the page
// as an overlay (as on Windows).
func embedPixelU(pid int, exited <-chan struct{}) { embedWindow(pid, exited, true) }

// embedSupermodel follows one Supermodel run like embedPixelU.
func embedSupermodel(pid int, exited <-chan struct{}) { embedWindow(pid, exited, true) }

// embedDolphin follows one PixelU-Wii (or MAME) run: docks its game screen
// into the RetroPixels window as a child window, keeps it there, and makes
// sure the engine closes completely once the game is stopped.
func embedDolphin(pid int, exited <-chan struct{}) { embedWindow(pid, exited, false) }

// hostWindowPresent: the RetroPixels app window is open, so a game started
// now gets docked into it.
func hostWindowPresent() bool {
	if dockMode() == "off" {
		return false
	}
	c, err := dialX()
	if err != nil {
		return false
	}
	defer c.Close()
	return findHost(c) != 0
}

func embedWindow(pid int, exited <-chan struct{}, overlay bool) {
	switch dockMode() {
	case "off":
		return
	case "child":
		overlay = false
	case "overlay":
		overlay = true
	}
	c, err := dialX()
	if err != nil {
		return
	}
	defer c.Close() // our box goes; a game screen still in it goes back to the desktop (save-set)
	host := findHost(c)
	if host == 0 {
		return // no RetroPixels window (e.g. opened in a normal browser): the engine keeps its own window
	}
	s := &dockSession{c: c, host: host, overlay: overlay}
	go s.events()

	embedMu.Lock()
	embedPID, embedWin, embedConn, embedStopAt = pid, 0, c, time.Time{}
	embedMu.Unlock()
	defer func() {
		embedMu.Lock()
		if embedPID == pid {
			embedPID, embedWin, embedConn, embedStopAt = 0, 0, nil, time.Time{}
		}
		embedMu.Unlock()
	}()

	tick := time.NewTicker(dockTick)
	defer tick.Stop()
	deadline := time.Now().Add(dockDeadline)
	var lostAt time.Time // when the docked game screen went away
	failed := map[x11.Window]int{}
	for {
		select {
		case <-exited:
			return
		case <-tick.C:
		}
		if err := c.Sync(); errors.Is(err, x11.ErrClosed) {
			return // the X server went away
		}
		if alive, _ := s.hostAlive(); !alive {
			// RetroPixels closed: end the game too.
			if s.child != 0 {
				_ = c.CloseWindow(s.child)
			}
			shutdownEngine(c, pid, exited, hostGoneGrace)
			return
		}
		if s.child != 0 {
			if s.isDocked() {
				embedMu.Lock()
				stopAt := embedStopAt
				embedMu.Unlock()
				if !stopAt.IsZero() && time.Since(stopAt) > stopGrace {
					// Stop game was clicked but the engine didn't react.
					shutdownEngine(c, pid, exited, termGrace)
					return
				}
				s.place(false)
				continue
			}
			// The game screen went away: the game was stopped, or the engine
			// made a new one (e.g. it toggled fullscreen), which gets docked below.
			s.child, lostAt = 0, time.Now()
			embedMu.Lock()
			embedWin = 0
			embedMu.Unlock()
		}
		w := gameScreen(c, pid)
		if failed[w] >= 3 {
			w = 0 // it can't be docked: it stays the engine's own window
		}
		if w == 0 {
			if !lostAt.IsZero() && time.Since(lostAt) > lostGrace {
				// The game ended but the engine is still running in the
				// background: close it so a new game can start.
				shutdownEngine(c, pid, exited, 5*time.Second)
				return
			}
			if lostAt.IsZero() && time.Now().After(deadline) {
				return // never showed a game screen: leave the engine as it is
			}
			continue
		}
		if err := s.dock(w); err != nil {
			// Give it back to the desktop, mapped, as it was.
			s.child = 0
			failed[w]++
			_ = c.ReparentWindow(w, c.Root(), 0, 0)
			_ = c.ChangeSaveSet(x11.SetModeDelete, w)
			_ = c.MapWindow(w)
			continue
		}
		lostAt = time.Time{}
		embedMu.Lock()
		embedWin = w
		embedMu.Unlock()
		runMu.Lock()
		if running != nil { // one game runs at a time
			running.Embedded = true
		}
		runMu.Unlock()
		s.place(true)
	}
}

// stopEmbeddedGame asks the docked game screen to close, which ends the game
// the same way the engine's own close button does (memory cards are saved).
// An engine that doesn't take WM_DELETE_WINDOW gets SIGTERM. embedWindow
// then makes sure the engine itself closes.
func stopEmbeddedGame() error {
	embedMu.Lock()
	w, c, pid := embedWin, embedConn, embedPID
	if w != 0 {
		embedStopAt = time.Now()
	}
	embedMu.Unlock()
	if w == 0 || c == nil {
		return errors.New("no game is playing inside RetroPixels")
	}
	// (The engine may close at once and the docking session with it, which
	// closes c: errors after the message is sent don't matter.)
	err := c.CloseWindow(w)
	if errors.Is(err, x11.ErrNoDeleteProtocol) || x11.WindowGone(err) {
		_ = syscall.Kill(pid, syscall.SIGTERM)
		return nil
	}
	if err != nil {
		embedMu.Lock()
		ended := embedPID != pid
		embedMu.Unlock()
		if ended {
			return nil // the game ended meanwhile
		}
		return err
	}
	return nil
}

// shutdownEngine asks every window of the engine (its hidden main window
// too) to close, then ends the process with SIGTERM if it's still there
// after wait, and with SIGKILL if that didn't do it either.
func shutdownEngine(c *x11.Conn, pid int, exited <-chan struct{}, wait time.Duration) {
	for _, w := range engineWindows(c, pid) {
		_ = c.CloseWindow(w)
	}
	_ = c.Sync()
	select {
	case <-exited:
		return
	case <-time.After(wait):
	}
	_ = syscall.Kill(pid, syscall.SIGTERM)
	select {
	case <-exited:
	case <-time.After(termGrace):
		_ = syscall.Kill(pid, syscall.SIGKILL)
	}
}

// pixeluScreen picks what the docked PixelU game shows (pixelu.SetScreen,
// a ClientMessage to its X11 window on Linux).
func pixeluScreen(mode int) (bool, error) {
	embedMu.Lock()
	w := embedWin
	embedMu.Unlock()
	runMu.Lock()
	isPixelU := running != nil && running.Kind == "pixelu"
	runMu.Unlock()
	if w == 0 || !isPixelU {
		return false, errors.New("no Wii U game is playing inside RetroPixels")
	}
	return pixelu.SetScreen(uintptr(w), mode)
}
