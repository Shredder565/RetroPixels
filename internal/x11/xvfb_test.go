//go:build linux

package x11

import (
	"bufio"
	"crypto/rand"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// These tests start their own Xvfb (with cookie authentication) and use
// real windows. Without Xvfb they are skipped, unless RETRO_X11_REQUIRE=1
// (CI sets it so a missing Xvfb can't pass silently).

type xserver struct {
	display string
	cookie  []byte
}

func startXvfb(t *testing.T) xserver {
	t.Helper()
	bin, err := exec.LookPath("Xvfb")
	if err != nil {
		if os.Getenv("RETRO_X11_REQUIRE") == "1" {
			t.Fatal("Xvfb not found and RETRO_X11_REQUIRE=1")
		}
		t.Skip("Xvfb not installed")
	}
	dir := t.TempDir()
	cookie := make([]byte, 16)
	rand.Read(cookie)
	host, _ := os.Hostname()
	auth := filepath.Join(dir, "Xauthority")
	f, err := os.Create(auth)
	if err != nil {
		t.Fatal(err)
	}
	// Xvfb reads the cookie from this file; Dial finds it through
	// $XAUTHORITY like any X program. Number "" matches whatever display
	// Xvfb picks.
	if err := WriteAuthority(f, []AuthEntry{{Family: FamilyLocal, Address: host, Name: "MIT-MAGIC-COOKIE-1", Data: cookie}}); err != nil {
		t.Fatal(err)
	}
	f.Close()

	pr, pw, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(bin, "-displayfd", "3", "-auth", auth, "-nolisten", "tcp", "-screen", "0", "1024x768x24")
	cmd.ExtraFiles = []*os.File{pw}
	var stderr strings.Builder
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	pw.Close()
	t.Cleanup(func() {
		cmd.Process.Kill()
		cmd.Wait()
	})
	got := make(chan string, 1)
	go func() {
		line, _ := bufio.NewReader(pr).ReadString('\n')
		got <- strings.TrimSpace(line)
		pr.Close()
	}()
	select {
	case n := <-got:
		if _, err := strconv.Atoi(n); err != nil {
			t.Fatalf("Xvfb did not start (%q): %s", n, stderr.String())
		}
		t.Setenv("XAUTHORITY", auth)
		t.Setenv("DISPLAY", ":"+n)
		return xserver{display: ":" + n, cookie: cookie}
	case <-time.After(20 * time.Second):
		t.Fatalf("Xvfb did not start in time: %s", stderr.String())
	}
	return xserver{}
}

func dialT(t *testing.T) *Conn {
	t.Helper()
	c, err := Dial("")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

// waitFor waits for the first event that match accepts.
func waitFor[T Event](t *testing.T, c *Conn, what string, match func(T) bool) T {
	t.Helper()
	e := c.WaitEvent(5*time.Second, func(e Event) bool {
		v, ok := e.(T)
		return ok && (match == nil || match(v))
	})
	if e == nil {
		var zero T
		t.Fatalf("no %s event", what)
		return zero
	}
	return e.(T)
}

func TestXvfbConnectAndAuth(t *testing.T) {
	xs := startXvfb(t)
	c := dialT(t)
	if c.Setup.ProtocolMajor != 11 || len(c.Setup.Screens) == 0 || c.Root() == None {
		t.Fatalf("setup = %+v", c.Setup)
	}
	if s := c.DefaultScreen(); s.Width != 1024 || s.Height != 768 || s.RootDepth != 24 {
		t.Errorf("screen = %+v", s)
	}
	if c.IsXWayland() {
		t.Error("Xvfb taken for XWayland")
	}
	if ext, err := c.QueryExtension("NO-SUCH-EXTENSION"); err != nil || ext.Present {
		t.Errorf("QueryExtension = %+v, %v", ext, err)
	}
	d, _ := ParseDisplay(xs.display)
	if _, err := DialDisplay(d, "", nil); err == nil || !strings.Contains(err.Error(), "refused") {
		t.Errorf("no cookie: %v", err)
	}
	wrong := append([]byte(nil), xs.cookie...)
	wrong[0] ^= 0xff
	if _, err := DialDisplay(d, "MIT-MAGIC-COOKIE-1", wrong); err == nil {
		t.Error("wrong cookie accepted")
	}
	c2, err := DialDisplay(d, "MIT-MAGIC-COOKIE-1", xs.cookie)
	if err != nil {
		t.Fatal(err)
	}
	c2.Close()
	if _, err := c2.GetInputFocus(); err == nil {
		t.Error("request on a closed connection worked")
	}
}

func TestXvfbWindows(t *testing.T) {
	startXvfb(t)
	c := dialT(t)
	root := c.Root()
	mask := uint32(StructureNotifyMask | SubstructureNotifyMask)
	host, err := c.CreateWindow(root, 50, 60, 800, 600, 0, &WindowAttributes{
		Mask: CWBackPixel | CWEventMask, BackPixel: c.DefaultScreen().WhitePixel, EventMask: mask})
	must(t, err)
	kid, err := c.CreateWindow(root, 0, 0, 320, 240, 0, &WindowAttributes{Mask: CWEventMask, EventMask: StructureNotifyMask})
	must(t, err)
	must(t, c.Sync())

	// Map / unmap.
	a, err := c.GetWindowAttributes(kid)
	must(t, err)
	if a.MapState != IsUnmapped || a.YourEventMask != StructureNotifyMask {
		t.Errorf("new window attributes = %+v", a)
	}
	must(t, c.MapWindow(host))
	must(t, c.MapWindow(kid))
	waitFor(t, c, "MapNotify", func(e *MapNotifyEvent) bool { return e.Window == kid })
	if a, _ := c.GetWindowAttributes(kid); a.MapState != IsViewable {
		t.Errorf("mapped window state %d", a.MapState)
	}

	// Reparent into host: the tree, the event and the place.
	must(t, c.ReparentWindow(kid, host, 10, 20))
	rn := waitFor(t, c, "ReparentNotify", func(e *ReparentNotifyEvent) bool { return e.Window == kid })
	if rn.Parent != host || rn.X != 10 || rn.Y != 20 {
		t.Errorf("ReparentNotify = %+v", rn)
	}
	r, parent, kids, err := c.QueryTree(kid)
	must(t, err)
	if r != root || parent != host {
		t.Errorf("QueryTree(kid) root %x parent %x", r, parent)
	}
	_, _, kids, err = c.QueryTree(host)
	must(t, err)
	if len(kids) != 1 || kids[0] != kid {
		t.Errorf("host children = %v", kids)
	}
	// A reparented mapped window is mapped again by the server.
	waitFor(t, c, "MapNotify after reparent", func(e *MapNotifyEvent) bool { return e.Window == kid })

	// Configure.
	must(t, c.MoveResizeWindow(kid, 5, 7, 640, 480))
	cn := waitFor(t, c, "ConfigureNotify", func(e *ConfigureNotifyEvent) bool { return e.Window == kid && e.Width == 640 })
	if cn.X != 5 || cn.Y != 7 || cn.Height != 480 {
		t.Errorf("ConfigureNotify = %+v", cn)
	}
	g, err := c.GetGeometry(kid)
	must(t, err)
	if g.X != 5 || g.Y != 7 || g.Width != 640 || g.Height != 480 || g.Root != root || g.Depth != 24 {
		t.Errorf("geometry = %+v", g)
	}
	x, y, child, err := c.TranslateCoordinates(kid, root, 0, 0)
	must(t, err)
	if x != 55 || y != 67 {
		t.Errorf("kid's origin on root = %d,%d", x, y)
	}
	_, _, child, err = c.TranslateCoordinates(root, host, 50+100, 60+100)
	must(t, err)
	if child != kid {
		t.Errorf("child at point = %x, want %x", child, kid)
	}
	// Stacking: a second child raised above, then the first raised back.
	k2, err := c.CreateWindow(host, 0, 0, 10, 10, 0, nil)
	must(t, err)
	must(t, c.ConfigureWindow(kid, WindowChanges{Mask: ConfigSibling | ConfigStackMode, Sibling: k2, StackMode: Above}))
	_, _, kids, err = c.QueryTree(host)
	must(t, err)
	if len(kids) != 2 || kids[1] != kid {
		t.Errorf("stacking after Above = %v", kids)
	}

	// Focus.
	must(t, c.SetInputFocus(kid, RevertToParent, CurrentTime))
	if f, err := c.GetInputFocus(); err != nil || f != kid {
		t.Errorf("focus = %x, %v", f, err)
	}

	// Unmap, back to the root, destroy.
	must(t, c.UnmapWindow(kid))
	waitFor(t, c, "UnmapNotify", func(e *UnmapNotifyEvent) bool { return e.Window == kid })
	if a, _ := c.GetWindowAttributes(kid); a.MapState != IsUnmapped {
		t.Errorf("unmapped window state %d", a.MapState)
	}
	must(t, c.ReparentWindow(kid, root, 0, 0))
	must(t, c.DestroyWindow(kid))
	waitFor(t, c, "DestroyNotify", func(e *DestroyNotifyEvent) bool { return e.Window == kid })
	if _, err := c.GetGeometry(kid); !WindowGone(err) {
		t.Errorf("GetGeometry of a destroyed window: %v", err)
	}
	if _, err := c.GetWindowAttributes(kid); !IsError(err, BadWindow) {
		t.Errorf("GetWindowAttributes of a destroyed window: %v", err)
	}
	// A request without a reply reports its error at the next Sync.
	must(t, c.MapWindow(kid))
	if err := c.Sync(); !IsError(err, BadWindow) {
		t.Errorf("Sync after a bad MapWindow: %v", err)
	}
	must(t, c.Sync())
}

func TestXvfbProperties(t *testing.T) {
	startXvfb(t)
	c := dialT(t)
	w, err := c.CreateWindow(c.Root(), 0, 0, 10, 10, 0, &WindowAttributes{Mask: CWEventMask, EventMask: PropertyChangeMask})
	must(t, err)

	at, err := c.Atoms("RETRO_TEST_A", "RETRO_TEST_B", "UTF8_STRING", "RETRO_TEST_A")
	must(t, err)
	if at[0] == None || at[0] == at[1] || at[0] != at[3] {
		t.Fatalf("atoms = %v", at)
	}
	if n, err := c.AtomName(at[1]); err != nil || n != "RETRO_TEST_B" {
		t.Errorf("AtomName = %q, %v", n, err)
	}
	if n, err := c.AtomName(AtomWMClass); err != nil || n != "WM_CLASS" {
		t.Errorf("AtomName(WM_CLASS) = %q, %v", n, err)
	}
	if a, err := c.InternAtom("RETRO_NEVER_MADE_"+strconv.Itoa(os.Getpid()), true); err != nil || a != None {
		t.Errorf("onlyIfExists atom = %d, %v", a, err)
	}

	must(t, c.SetPropertyString(w, at[0], at[2], "héllo"))
	pn := waitFor(t, c, "PropertyNotify", func(e *PropertyNotifyEvent) bool { return e.Atom == at[0] })
	if pn.Window != w || pn.State != 0 {
		t.Errorf("PropertyNotify = %+v", pn)
	}
	if s, err := c.PropertyString(w, at[0]); err != nil || s != "héllo" {
		t.Errorf("string property = %q, %v", s, err)
	}
	p, err := c.GetProperty(w, at[0], AtomCardinal)
	must(t, err)
	if p.Type != at[2] || len(p.Value) != 0 {
		t.Errorf("property asked with the wrong type = %+v", p)
	}
	must(t, c.ChangeProperty(w, at[0], at[2], 8, PropModeAppend, []byte("!")))
	if s, _ := c.PropertyString(w, at[0]); s != "héllo!" {
		t.Errorf("appended = %q", s)
	}

	must(t, c.SetPropertyUint32s(w, at[1], AtomCardinal, 1, 0xffffffff, 3))
	if v, err := c.PropertyUint32s(w, at[1]); err != nil || len(v) != 3 || v[1] != 0xffffffff {
		t.Errorf("uint32 property = %v, %v", v, err)
	}

	// A property larger than one GetProperty reply and near the request
	// size limit.
	big := make([]byte, 200000)
	for i := range big {
		big[i] = byte(i * 7)
	}
	must(t, c.ChangeProperty(w, at[1], AtomString, 8, PropModeReplace, big))
	p, err = c.GetProperty(w, at[1], AtomAny)
	must(t, err)
	if p.Format != 8 || p.Type != AtomString || string(p.Value) != string(big) {
		t.Errorf("big property: %d bytes, format %d", len(p.Value), p.Format)
	}
	if err := c.ChangeProperty(w, at[1], AtomString, 8, PropModeReplace, make([]byte, 300000)); err == nil {
		t.Error("request over the server's limit was sent")
	}
	if err := c.ChangeProperty(w, at[1], AtomCardinal, 32, PropModeReplace, []byte{1, 2}); err == nil {
		t.Error("half an item accepted")
	}

	must(t, c.DeleteProperty(w, at[1]))
	waitFor(t, c, "PropertyNotify (deleted)", func(e *PropertyNotifyEvent) bool { return e.Atom == at[1] && e.State == 1 })
	p, err = c.GetProperty(w, at[1], AtomAny)
	if err != nil || p.Type != None || len(p.Value) != 0 {
		t.Errorf("deleted property = %+v, %v", p, err)
	}

	// ICCCM/EWMH helpers.
	must(t, c.SetWMName(w, "RetroPixels — test"))
	must(t, c.SetWMClass(w, "retropixels", "RetroPixels"))
	must(t, c.SetWMPID(w, 4242))
	must(t, c.SetWMProtocols(w, "WM_DELETE_WINDOW", "WM_TAKE_FOCUS"))
	owner, _ := c.CreateWindow(c.Root(), 0, 0, 10, 10, 0, nil)
	must(t, c.SetTransientFor(w, owner))
	must(t, c.SetWMState(w, "_NET_WM_STATE_SKIP_TASKBAR"))
	if n, err := c.WMName(w); err != nil || n != "RetroPixels — test" {
		t.Errorf("WMName = %q, %v", n, err)
	}
	if i, cl, err := c.WMClass(w); err != nil || i != "retropixels" || cl != "RetroPixels" {
		t.Errorf("WMClass = %q %q %v", i, cl, err)
	}
	if pid, err := c.WMPID(w); err != nil || pid != 4242 {
		t.Errorf("WMPID = %d, %v", pid, err)
	}
	if ok, err := c.HasWMProtocol(w, "WM_DELETE_WINDOW"); err != nil || !ok {
		t.Errorf("HasWMProtocol = %v, %v", ok, err)
	}
	if ok, _ := c.HasWMProtocol(w, "_NET_WM_PING"); ok {
		t.Error("_NET_WM_PING listed")
	}
	if o, err := c.TransientFor(w); err != nil || o != owner {
		t.Errorf("TransientFor = %x, %v", o, err)
	}
	must(t, c.SetTransientFor(w, None))
	if o, _ := c.TransientFor(w); o != None {
		t.Errorf("TransientFor after clearing = %x", o)
	}
	if ws, err := c.WindowsByPID(4242); err != nil || len(ws) != 1 || ws[0] != w {
		t.Errorf("WindowsByPID = %v, %v", ws, err)
	}
	if a, err := c.ActiveWindow(); err != nil || a != None {
		t.Errorf("ActiveWindow without a window manager = %x, %v", a, err)
	}
	must(t, c.Sync())

	// Cross-check with xprop when it is installed.
	if _, err := exec.LookPath("xprop"); err == nil {
		out, err := exec.Command("xprop", "-id", FormatWindow(w), "WM_CLASS", "_NET_WM_PID", "WM_PROTOCOLS").CombinedOutput()
		if err != nil {
			t.Fatalf("xprop: %v %s", err, out)
		}
		s := string(out)
		for _, want := range []string{`WM_CLASS(STRING) = "retropixels", "RetroPixels"`, "_NET_WM_PID(CARDINAL) = 4242", "WM_DELETE_WINDOW, WM_TAKE_FOCUS"} {
			if !strings.Contains(s, want) {
				t.Errorf("xprop output lacks %q:\n%s", want, s)
			}
		}
	}
}

func TestXvfbClientMessages(t *testing.T) {
	startXvfb(t)
	a := dialT(t)
	b := dialT(t)
	w, err := a.CreateWindow(a.Root(), 0, 0, 10, 10, 0, nil)
	must(t, err)
	must(t, a.Sync())
	typ, err := b.Atom("RETRO_SCREEN")
	must(t, err)

	// Mask 0: delivered to the window's creator, whatever it selects.
	must(t, b.SendClientMessage(w, w, typ, NoEventMask, [5]uint32{2, 7, 0, 0, 9}))
	must(t, b.Sync())
	m := waitFor(t, a, "ClientMessage", func(e *ClientMessageEvent) bool { return e.Type == typ })
	if !m.Synthetic || m.Window != w || m.Format != 32 || m.Data32() != [5]uint32{2, 7, 0, 0, 9} {
		t.Errorf("ClientMessage = %+v", m)
	}

	// To the root with the substructure masks, as for the window manager.
	must(t, a.SelectInput(a.Root(), SubstructureNotifyMask))
	must(t, a.Sync())
	must(t, b.ActivateWindow(w))
	act, _ := a.Atom("_NET_ACTIVE_WINDOW")
	m = waitFor(t, a, "_NET_ACTIVE_WINDOW", func(e *ClientMessageEvent) bool { return e.Type == act })
	if m.Window != w || m.Data32()[0] != 2 {
		t.Errorf("_NET_ACTIVE_WINDOW = %+v", m)
	}

	// WM_DELETE_WINDOW.
	if err := b.CloseWindow(w); !errors.Is(err, ErrNoDeleteProtocol) {
		t.Errorf("CloseWindow without the protocol: %v", err)
	}
	must(t, a.SetWMProtocols(w, "WM_DELETE_WINDOW"))
	must(t, a.Sync())
	must(t, b.CloseWindow(w))
	e := a.WaitEvent(5*time.Second, a.IsDeleteWindow)
	if e == nil {
		t.Fatal("no WM_DELETE_WINDOW")
	}
	if !b.IsDeleteWindow(e) {
		t.Error("the other connection does not see it as WM_DELETE_WINDOW")
	}
	if _, ok := e.(*ClientMessageEvent); !ok {
		t.Errorf("event %T", e)
	}
}

// TestXvfbSaveSet: a window docked into one of ours goes back to the root,
// alive, when our connection closes.
func TestXvfbSaveSet(t *testing.T) {
	startXvfb(t)
	engine := dialT(t)
	w, err := engine.CreateWindow(engine.Root(), 0, 0, 100, 100, 0, nil)
	must(t, err)
	must(t, engine.MapWindow(w))
	must(t, engine.Sync())

	dock, err := Dial("")
	must(t, err)
	host, err := dock.CreateWindow(dock.Root(), 0, 0, 200, 200, 0, nil)
	must(t, err)
	must(t, dock.ReparentWindow(w, host, 3, 4))
	must(t, dock.ChangeSaveSet(SetModeInsert, w))
	must(t, dock.Sync())
	if _, p, _, _ := engine.QueryTree(w); p != host {
		t.Fatalf("parent = %x, want %x", p, host)
	}
	dock.Close()
	deadline := time.Now().Add(5 * time.Second)
	for {
		_, p, _, err := engine.QueryTree(w)
		must(t, err)
		if p == engine.Root() {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("window not back on the root (parent %x)", p)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// TestXvfbSequenceWrap sends more than 65536 requests, so the server's
// 16-bit sequence numbers wrap, and checks replies and errors still match.
func TestXvfbSequenceWrap(t *testing.T) {
	startXvfb(t)
	c := dialT(t)
	w, err := c.CreateWindow(c.Root(), 0, 0, 10, 10, 0, nil)
	must(t, err)
	for i := 0; i < 70000; i++ {
		if err := c.MapWindow(w); err != nil {
			t.Fatal(err)
		}
		if i == 40000 {
			if g, err := c.GetGeometry(w); err != nil || g.Width != 10 {
				t.Fatalf("GetGeometry mid-way: %+v %v", g, err)
			}
		}
	}
	if c.seq.Load() < 70000 {
		t.Fatalf("seq = %d", c.seq.Load())
	}
	if g, err := c.GetGeometry(w); err != nil || g.Width != 10 {
		t.Fatalf("GetGeometry after wrap: %+v %v", g, err)
	}
	must(t, c.Sync())
	must(t, c.DestroyWindow(Window(c.Setup.ResourceIDBase|0xffff)))
	if err := c.Sync(); !IsError(err, BadWindow) {
		t.Errorf("error after wrap: %v", err)
	}
}

func TestXvfbConcurrent(t *testing.T) {
	startXvfb(t)
	c := dialT(t)
	var wg sync.WaitGroup
	errs := make(chan error, 16)
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 200; i++ {
				w, err := c.CreateWindow(c.Root(), int16(i), int16(g), uint16(10+g), uint16(10+i), 0, nil)
				if err != nil {
					errs <- err
					return
				}
				name := fmt.Sprintf("RETRO_C_%d_%d", g, i%10)
				a, err := c.Atom(name)
				if err == nil {
					err = c.SetPropertyUint32s(w, a, AtomCardinal, uint32(g*1000+i))
				}
				var v []uint32
				if err == nil {
					v, err = c.PropertyUint32s(w, a)
				}
				if err == nil && (len(v) != 1 || v[0] != uint32(g*1000+i)) {
					err = fmt.Errorf("goroutine %d: property %v", g, v)
				}
				if err == nil {
					var geo Geometry
					geo, err = c.GetGeometry(w)
					if err == nil && (geo.Width != uint16(10+g) || geo.Height != uint16(10+i)) {
						err = fmt.Errorf("goroutine %d: geometry %+v", g, geo)
					}
				}
				if err == nil {
					err = c.DestroyWindow(w)
				}
				if err != nil {
					errs <- err
					return
				}
			}
		}(g)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
	must(t, c.Sync())
}

// TestXvfbTestwin runs the testwin program the way L2b/L4 tests will:
// find its window by pid, dock it, message it, close it.
func TestXvfbTestwin(t *testing.T) {
	startXvfb(t)
	if testing.Short() {
		t.Skip("builds a program")
	}
	exe := filepath.Join(t.TempDir(), "testwin")
	if out, err := exec.Command("go", "build", "-o", exe, "./testwin").CombinedOutput(); err != nil {
		t.Fatalf("build testwin: %v\n%s", err, out)
	}
	c := dialT(t)

	run := func(args ...string) (*exec.Cmd, *bufio.Scanner, Window) {
		cmd := exec.Command(exe, args...)
		out, err := cmd.StdoutPipe()
		must(t, err)
		cmd.Stderr = os.Stderr
		must(t, cmd.Start())
		t.Cleanup(func() { cmd.Process.Kill(); cmd.Wait() })
		sc := bufio.NewScanner(out)
		if !sc.Scan() || !strings.HasPrefix(sc.Text(), "window 0x") {
			t.Fatalf("testwin said %q", sc.Text())
		}
		id, err := strconv.ParseUint(strings.TrimPrefix(sc.Text(), "window 0x"), 16, 32)
		must(t, err)
		return cmd, sc, Window(id)
	}
	expect := func(sc *bufio.Scanner, prefix string) string {
		t.Helper()
		lines := make(chan string, 1)
		go func() {
			for sc.Scan() {
				if strings.HasPrefix(sc.Text(), prefix) {
					lines <- sc.Text()
					return
				}
			}
			close(lines)
		}()
		select {
		case l, ok := <-lines:
			if !ok {
				t.Fatalf("testwin ended before %q", prefix)
			}
			return l
		case <-time.After(5 * time.Second):
			t.Fatalf("testwin did not print %q", prefix)
		}
		return ""
	}

	cmd, sc, w := run("-name", "Engine", "-class", "engine,Engine")
	expect(sc, "map")
	ws, err := c.WindowsByPID(cmd.Process.Pid)
	must(t, err)
	if len(ws) != 1 || ws[0] != w {
		t.Fatalf("WindowsByPID = %v, want [%x]", ws, w)
	}
	if n, _ := c.WMName(w); n != "Engine" {
		t.Errorf("name %q", n)
	}

	host, err := c.CreateWindow(c.Root(), 0, 0, 900, 700, 0, nil)
	must(t, err)
	must(t, c.MapWindow(host))
	must(t, c.ChangeSaveSet(SetModeInsert, w))
	must(t, c.ReparentWindow(w, host, 0, 40))
	if l := expect(sc, "reparent"); l != fmt.Sprintf("reparent parent=%s x=0 y=40", FormatWindow(host)) {
		t.Errorf("testwin: %q", l)
	}
	must(t, c.MoveResizeWindow(w, 0, 40, 900, 660))
	if l := expect(sc, "configure"); l != "configure x=0 y=40 w=900 h=660" {
		t.Errorf("testwin: %q", l)
	}
	if _, err := exec.LookPath("xwininfo"); err == nil {
		out, err := exec.Command("xwininfo", "-tree", "-id", FormatWindow(host)).CombinedOutput()
		if err != nil || !strings.Contains(string(out), FormatWindow(w)+` "Engine"`) {
			t.Errorf("xwininfo -tree does not show the docked window: %v\n%s", err, out)
		}
	}

	typ, _ := c.Atom("RETRO_TEST_MESSAGE")
	must(t, c.SendClientMessage(w, w, typ, NoEventMask, [5]uint32{1, 2, 3, 4, 5}))
	if l := expect(sc, "client"); l != "client type=RETRO_TEST_MESSAGE data=1,2,3,4,5" {
		t.Errorf("testwin: %q", l)
	}

	must(t, c.CloseWindow(w))
	expect(sc, "delete")
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("testwin exit: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("testwin did not exit on WM_DELETE_WINDOW")
	}

	// A window without WM_DELETE_WINDOW: CloseWindow says so, so the caller
	// falls back to SIGTERM.
	cmd2, sc2, w2 := run("-no-delete", "-unmapped")
	if a, err := c.GetWindowAttributes(w2); err != nil || a.MapState != IsUnmapped {
		t.Errorf("-unmapped window: %+v %v", a, err)
	}
	if err := c.CloseWindow(w2); !errors.Is(err, ErrNoDeleteProtocol) {
		t.Errorf("CloseWindow: %v", err)
	}
	cmd2.Process.Signal(os.Interrupt)
	expect(sc2, "sigterm")
}
