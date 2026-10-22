package x11

import (
	"errors"
	"fmt"
	"strings"
)

// Predefined atoms of the core protocol.
const (
	AtomAny            Atom = 0
	AtomPrimary        Atom = 1
	AtomAtom           Atom = 4
	AtomCardinal       Atom = 6
	AtomInteger        Atom = 19
	AtomString         Atom = 31
	AtomWindow         Atom = 33
	AtomWMCommand      Atom = 34
	AtomWMHints        Atom = 35
	AtomWMIconName     Atom = 37
	AtomWMName         Atom = 39
	AtomWMNormalHints  Atom = 40
	AtomWMSizeHints    Atom = 41
	AtomWMClass        Atom = 67
	AtomWMTransientFor Atom = 68
)

// SetPropertyString sets a format-8 property (typ is usually AtomString
// or UTF8_STRING).
func (c *Conn) SetPropertyString(w Window, prop, typ Atom, s string) error {
	return c.ChangeProperty(w, prop, typ, 8, PropModeReplace, []byte(s))
}

// SetPropertyUint32s sets a format-32 property (CARDINAL, WINDOW, ATOM).
func (c *Conn) SetPropertyUint32s(w Window, prop, typ Atom, v ...uint32) error {
	b := make([]byte, 0, 4*len(v))
	for _, x := range v {
		b = le.AppendUint32(b, x)
	}
	return c.ChangeProperty(w, prop, typ, 32, PropModeReplace, b)
}

// PropertyUint32s reads a format-32 property; nil when w lacks it.
func (c *Conn) PropertyUint32s(w Window, prop Atom) ([]uint32, error) {
	p, err := c.GetProperty(w, prop, AtomAny)
	if err != nil {
		return nil, err
	}
	return p.Uint32s(), nil
}

// PropertyString reads a format-8 property; "" when w lacks it.
func (c *Conn) PropertyString(w Window, prop Atom) (string, error) {
	p, err := c.GetProperty(w, prop, AtomAny)
	if err != nil || p.Format != 8 {
		return "", err
	}
	return string(p.Value), nil
}

// WMName is the window's title: _NET_WM_NAME, else WM_NAME.
func (c *Conn) WMName(w Window) (string, error) {
	a, err := c.Atom("_NET_WM_NAME")
	if err != nil {
		return "", err
	}
	if s, err := c.PropertyString(w, a); err != nil || s != "" {
		return s, err
	}
	return c.PropertyString(w, AtomWMName)
}

// SetWMName sets _NET_WM_NAME (UTF-8) and WM_NAME.
func (c *Conn) SetWMName(w Window, name string) error {
	at, err := c.Atoms("_NET_WM_NAME", "UTF8_STRING")
	if err != nil {
		return err
	}
	if err := c.SetPropertyString(w, at[0], at[1], name); err != nil {
		return err
	}
	return c.SetPropertyString(w, AtomWMName, at[1], name)
}

// WMClass returns WM_CLASS's instance and class names.
func (c *Conn) WMClass(w Window) (instance, class string, err error) {
	s, err := c.PropertyString(w, AtomWMClass)
	if err != nil {
		return "", "", err
	}
	parts := strings.SplitN(strings.TrimRight(s, "\x00"), "\x00", 2)
	if len(parts) == 2 {
		return parts[0], parts[1], nil
	}
	return parts[0], "", nil
}

// SetWMClass sets WM_CLASS.
func (c *Conn) SetWMClass(w Window, instance, class string) error {
	return c.SetPropertyString(w, AtomWMClass, AtomString, instance+"\x00"+class+"\x00")
}

// WMPID returns _NET_WM_PID, or 0 when the window has none.
func (c *Conn) WMPID(w Window) (int, error) {
	a, err := c.Atom("_NET_WM_PID")
	if err != nil {
		return 0, err
	}
	v, err := c.PropertyUint32s(w, a)
	if err != nil || len(v) == 0 {
		return 0, err
	}
	return int(v[0]), nil
}

// SetWMPID sets _NET_WM_PID.
func (c *Conn) SetWMPID(w Window, pid int) error {
	a, err := c.Atom("_NET_WM_PID")
	if err != nil {
		return err
	}
	return c.SetPropertyUint32s(w, a, AtomCardinal, uint32(pid))
}

// WMProtocols returns the WM_PROTOCOLS the window takes part in.
func (c *Conn) WMProtocols(w Window) ([]Atom, error) {
	a, err := c.Atom("WM_PROTOCOLS")
	if err != nil {
		return nil, err
	}
	v, err := c.PropertyUint32s(w, a)
	out := make([]Atom, len(v))
	for i, x := range v {
		out[i] = Atom(x)
	}
	return out, err
}

// SetWMProtocols sets WM_PROTOCOLS to the named protocols
// ("WM_DELETE_WINDOW", "WM_TAKE_FOCUS", "_NET_WM_PING").
func (c *Conn) SetWMProtocols(w Window, names ...string) error {
	at, err := c.Atoms(append([]string{"WM_PROTOCOLS"}, names...)...)
	if err != nil {
		return err
	}
	v := make([]uint32, len(names))
	for i := range names {
		v[i] = uint32(at[i+1])
	}
	return c.SetPropertyUint32s(w, at[0], AtomAtom, v...)
}

// HasWMProtocol reports whether w lists the named protocol.
func (c *Conn) HasWMProtocol(w Window, name string) (bool, error) {
	a, err := c.Atom(name)
	if err != nil {
		return false, err
	}
	ps, err := c.WMProtocols(w)
	for _, p := range ps {
		if p == a {
			return true, nil
		}
	}
	return false, err
}

// ErrNoDeleteProtocol is returned by CloseWindow for a window that does
// not take WM_DELETE_WINDOW.
var ErrNoDeleteProtocol = errors.New("x11: window does not take WM_DELETE_WINDOW")

// CloseWindow asks w's program to close it, the way a window manager's
// close button does: a WM_PROTOCOLS ClientMessage with WM_DELETE_WINDOW.
// It returns ErrNoDeleteProtocol when w does not list that protocol.
func (c *Conn) CloseWindow(w Window) error {
	ok, err := c.HasWMProtocol(w, "WM_DELETE_WINDOW")
	if err != nil {
		return err
	}
	if !ok {
		return ErrNoDeleteProtocol
	}
	return c.SendDeleteWindow(w)
}

// SendDeleteWindow sends the WM_DELETE_WINDOW message to w's program
// without checking WM_PROTOCOLS first.
func (c *Conn) SendDeleteWindow(w Window) error {
	at, err := c.Atoms("WM_PROTOCOLS", "WM_DELETE_WINDOW")
	if err != nil {
		return err
	}
	return c.SendClientMessage(w, w, at[0], NoEventMask, [5]uint32{uint32(at[1]), CurrentTime})
}

// IsDeleteWindow reports whether e is a WM_DELETE_WINDOW message.
func (c *Conn) IsDeleteWindow(e Event) bool {
	m, ok := e.(*ClientMessageEvent)
	if !ok || m.Format != 32 {
		return false
	}
	at, err := c.Atoms("WM_PROTOCOLS", "WM_DELETE_WINDOW")
	return err == nil && m.Type == at[0] && Atom(m.Data32()[0]) == at[1]
}

// SetTransientFor marks w as a dialog/overlay of owner (WM_TRANSIENT_FOR):
// window managers keep it above owner and out of the task bar.
func (c *Conn) SetTransientFor(w, owner Window) error {
	if owner == None {
		return c.DeleteProperty(w, AtomWMTransientFor)
	}
	return c.SetPropertyUint32s(w, AtomWMTransientFor, AtomWindow, uint32(owner))
}

// TransientFor returns WM_TRANSIENT_FOR, None when not set.
func (c *Conn) TransientFor(w Window) (Window, error) {
	v, err := c.PropertyUint32s(w, AtomWMTransientFor)
	if err != nil || len(v) == 0 {
		return None, err
	}
	return Window(v[0]), nil
}

// ActivateWindow asks the window manager to raise and focus w
// (_NET_ACTIVE_WINDOW, source "pager" so it is not refused as focus
// stealing). Without a window manager nothing happens.
func (c *Conn) ActivateWindow(w Window) error {
	a, err := c.Atom("_NET_ACTIVE_WINDOW")
	if err != nil {
		return err
	}
	return c.SendClientMessage(c.Root(), w, a, SubstructureNotifyMask|SubstructureRedirectMask,
		[5]uint32{2, CurrentTime, 0})
}

// ActiveWindow is the window manager's _NET_ACTIVE_WINDOW, None without
// one.
func (c *Conn) ActiveWindow() (Window, error) {
	a, err := c.Atom("_NET_ACTIVE_WINDOW")
	if err != nil {
		return None, err
	}
	v, err := c.PropertyUint32s(c.Root(), a)
	if err != nil || len(v) == 0 {
		return None, err
	}
	return Window(v[0]), nil
}

// SetWMState sets _NET_WM_STATE to the named states (e.g.
// "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_ABOVE") before a window is
// mapped.
func (c *Conn) SetWMState(w Window, states ...string) error {
	at, err := c.Atoms(append([]string{"_NET_WM_STATE"}, states...)...)
	if err != nil {
		return err
	}
	v := make([]uint32, len(states))
	for i := range states {
		v[i] = uint32(at[i+1])
	}
	return c.SetPropertyUint32s(w, at[0], AtomAtom, v...)
}

// Tree returns every window under top (not top itself), parents before
// children. Windows that vanish while it walks are left out.
func (c *Conn) Tree(top Window) ([]Window, error) {
	var out []Window
	queue := []Window{top}
	for len(queue) > 0 {
		w := queue[0]
		queue = queue[1:]
		_, _, kids, err := c.QueryTree(w)
		if err != nil {
			if w == top {
				return nil, err
			}
			continue
		}
		out = append(out, kids...)
		queue = append(queue, kids...)
	}
	return out, nil
}

// WindowsByPID returns the windows anywhere under the root whose
// _NET_WM_PID is pid, parents before children. A window manager's frame
// carries no pid, so these are the program's own windows.
func (c *Conn) WindowsByPID(pid int) ([]Window, error) {
	a, err := c.Atom("_NET_WM_PID")
	if err != nil {
		return nil, err
	}
	all, err := c.Tree(c.Root())
	if err != nil {
		return nil, err
	}
	// Ask for every window's pid at once, then collect the replies.
	chans := make([]chan reply, len(all))
	for i, w := range all {
		_, ch, err := c.send(newRequest(opGetProperty, 0).u32(uint32(w)).u32(uint32(a)).
			u32(uint32(AtomCardinal)).u32(0).u32(1), true)
		if err != nil {
			return nil, err
		}
		chans[i] = ch
	}
	var out []Window
	for i, ch := range chans {
		b, err := c.wait(ch)
		if err != nil || b[1] != 32 || len(b) < 36 || le.Uint32(b[16:]) == 0 {
			continue
		}
		if int(le.Uint32(b[32:])) == pid {
			out = append(out, all[i])
		}
	}
	return out, nil
}

// IsXWayland reports whether the server is XWayland (the X server of a
// Wayland desktop).
func (c *Conn) IsXWayland() bool {
	if e, err := c.QueryExtension("XWAYLAND"); err == nil && e.Present {
		return true
	}
	return strings.Contains(c.Setup.Vendor, "XWayland")
}

// FormatWindow is a window id in the 0x… form xwininfo and xprop print.
func FormatWindow(w Window) string { return fmt.Sprintf("0x%x", uint32(w)) }
