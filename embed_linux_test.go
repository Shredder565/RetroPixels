//go:build linux

package main

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"retropixels/internal/x11"
)

// The docking tests start their own Xvfb (and openbox, when it's installed,
// for the runs with a window manager) and dock internal/x11/testwin windows
// that stand in for the RetroPixels app window and the engines. Without Xvfb
// they are skipped, unless RETRO_X11_REQUIRE=1 (CI sets it).

func TestBrowserPlatformArgs(t *testing.T) {
	t.Setenv("RETROPIXELS_WAYLAND", "")
	t.Setenv("WAYLAND_DISPLAY", "")
	t.Setenv("DISPLAY", ":0")
	if a := browserPlatformArgs(); a != nil {
		t.Errorf("X11 desktop: %q", a)
	}
	t.Setenv("WAYLAND_DISPLAY", "wayland-0")
	if a := browserPlatformArgs(); !slices.Equal(a, []string{"--ozone-platform=x11"}) {
		t.Errorf("Wayland desktop: %q", a)
	}
	args := appWindowArgs("http://127.0.0.1:47821/", "/p")
	if args[0] != "--ozone-platform=x11" || !slices.Contains(args, "--class=RetroPixels") || !slices.Contains(args, "--app=http://127.0.0.1:47821/") {
		t.Errorf("app window args %q", args)
	}
	t.Setenv("RETROPIXELS_WAYLAND", "native")
	if useXWayland() {
		t.Error("RETROPIXELS_WAYLAND=native ignored")
	}
	t.Setenv("RETROPIXELS_WAYLAND", "")
	t.Setenv("DISPLAY", "")
	if useXWayland() {
		t.Error("XWayland without DISPLAY")
	}
}

func TestEmbedProcessTree(t *testing.T) {
	cmd := exec.Command("sh", "-c", "sleep 30 & wait")
	if err := cmd.Start(); err != nil {
		t.Skip(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if len(processTree("/proc", cmd.Process.Pid)) >= 2 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("processTree(%d) = %v", cmd.Process.Pid, processTree("/proc", cmd.Process.Pid))
		}
		time.Sleep(20 * time.Millisecond)
	}
	if p := processTree("/proc", os.Getpid()); !p[os.Getpid()] || !p[cmd.Process.Pid] {
		t.Errorf("our own tree %v lacks us or the shell", p)
	}
}

// ---------------- the X server, the window manager and testwin ----------------

func startTestX(t *testing.T, withWM bool) *x11.Conn {
	t.Helper()
	bin, err := exec.LookPath("Xvfb")
	if err != nil {
		if os.Getenv("RETRO_X11_REQUIRE") == "1" {
			t.Fatal("Xvfb not found and RETRO_X11_REQUIRE=1")
		}
		t.Skip("Xvfb not installed")
	}
	wm := ""
	if withWM {
		if wm, err = exec.LookPath("openbox"); err != nil {
			t.Skip("openbox not installed")
		}
	}
	pr, pw, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	xs := exec.Command(bin, "-displayfd", "3", "-nolisten", "tcp", "-screen", "0", "1280x1024x24")
	xs.ExtraFiles = []*os.File{pw}
	if err := xs.Start(); err != nil {
		t.Fatal(err)
	}
	pw.Close()
	t.Cleanup(func() { xs.Process.Kill(); xs.Wait() })
	got := make(chan string, 1)
	go func() {
		line, _ := bufio.NewReader(pr).ReadString('\n')
		got <- strings.TrimSpace(line)
		pr.Close()
	}()
	select {
	case n := <-got:
		if _, err := strconv.Atoi(n); err != nil {
			t.Fatalf("Xvfb did not start (%q)", n)
		}
		t.Setenv("DISPLAY", ":"+n)
	case <-time.After(20 * time.Second):
		t.Fatal("Xvfb did not start in time")
	}
	t.Setenv("XAUTHORITY", filepath.Join(t.TempDir(), "none")) // Xvfb without -auth takes anyone local
	t.Setenv("RETROPIXELS_DOCK", "")
	c, err := x11.Dial("")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })

	if withWM {
		home := t.TempDir() // openbox's settings and cache stay out of ~
		cmd := exec.Command(wm, "--sm-disable")
		cmd.Env = append(os.Environ(), "HOME="+home, "XDG_CONFIG_HOME="+home, "XDG_CACHE_HOME="+home, "XDG_DATA_HOME="+home)
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { cmd.Process.Kill(); cmd.Wait() })
		check, _ := c.Atom("_NET_SUPPORTING_WM_CHECK")
		waitUntil(t, "openbox to start", func() bool {
			v, _ := c.PropertyUint32s(c.Root(), check)
			return len(v) > 0
		})
		// It says it's there before it takes new windows: wait until it
		// frames one (a window mapped during its start can stay unmapped).
		probe, err := c.CreateWindow(c.Root(), 0, 0, 50, 50, 0, nil)
		if err != nil {
			t.Fatal(err)
		}
		deadline := time.Now().Add(10 * time.Second)
		for parentOf(c, probe) == c.Root() {
			if time.Now().After(deadline) {
				t.Fatal("openbox does not take windows")
			}
			_ = c.MapWindow(probe)
			time.Sleep(50 * time.Millisecond)
		}
		_ = c.DestroyWindow(probe)
	}
	// Short timings, so Stop and the fallbacks don't take seconds each.
	for _, v := range []struct {
		p *time.Duration
		d time.Duration
	}{{&dockTick, 20 * time.Millisecond}, {&stopGrace, time.Second}, {&lostGrace, 500 * time.Millisecond},
		{&termGrace, time.Second}, {&hostGoneGrace, time.Second}, {&dockDeadline, 20 * time.Second}} {
		old := *v.p
		*v.p = v.d
		t.Cleanup(func() { *v.p = old })
	}
	setEmbedTop(0)
	return c
}

func waitUntil(t *testing.T, what string, ok func() bool) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for !ok() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

var (
	testwinOnce sync.Once
	testwinExe  string
	testwinErr  error
)

func buildTestwin(t *testing.T) string {
	t.Helper()
	if testing.Short() {
		t.Skip("builds a program")
	}
	testwinOnce.Do(func() {
		dir, err := os.MkdirTemp("", "retropixels-testwin-")
		if err != nil {
			testwinErr = err
			return
		}
		testwinExe = filepath.Join(dir, "testwin")
		if out, err := exec.Command("go", "build", "-o", testwinExe, "./internal/x11/testwin").CombinedOutput(); err != nil {
			testwinErr = fmt.Errorf("build testwin: %v\n%s", err, out)
		}
	})
	if testwinErr != nil {
		t.Fatal(testwinErr)
	}
	return testwinExe
}

func TestMain(m *testing.M) {
	code := m.Run()
	if testwinExe != "" {
		os.RemoveAll(filepath.Dir(testwinExe))
	}
	os.Exit(code)
}

// twin is one running testwin.
type twin struct {
	cmd   *exec.Cmd
	win   x11.Window
	lines chan string
	done  chan struct{} // closed once it has exited
	err   error         // its exit status, once done
}

func runTestwin(t *testing.T, args ...string) *twin {
	t.Helper()
	cmd := exec.Command(buildTestwin(t), args...)
	out, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	tw := &twin{cmd: cmd, lines: make(chan string, 1000), done: make(chan struct{})}
	t.Cleanup(func() {
		cmd.Process.Kill()
		<-tw.done
	})
	sc := bufio.NewScanner(out)
	if !sc.Scan() || !strings.HasPrefix(sc.Text(), "window 0x") {
		t.Fatalf("testwin said %q", sc.Text())
	}
	id, err := strconv.ParseUint(strings.TrimPrefix(sc.Text(), "window 0x"), 16, 32)
	if err != nil {
		t.Fatal(err)
	}
	tw.win = x11.Window(id)
	go func() {
		for sc.Scan() {
			tw.lines <- sc.Text()
		}
		tw.err = cmd.Wait()
		close(tw.done)
	}()
	return tw
}

func (tw *twin) pid() int { return tw.cmd.Process.Pid }

// expect waits for a line starting with prefix (skipping others).
func (tw *twin) expect(t *testing.T, prefix string) string {
	t.Helper()
	timeout := time.After(10 * time.Second)
	var seen []string
	for {
		select {
		case l := <-tw.lines:
			if strings.HasPrefix(l, prefix) {
				return l
			}
			seen = append(seen, l)
		case <-tw.done:
			select {
			case l := <-tw.lines:
				if strings.HasPrefix(l, prefix) {
					return l
				}
				seen = append(seen, l)
				continue
			default:
			}
			t.Fatalf("testwin ended (%v) before %q; it said %q", tw.err, prefix, seen)
		case <-timeout:
			t.Fatalf("testwin did not print %q; it said %q", prefix, seen)
		}
	}
}

func (tw *twin) waitExit(t *testing.T) {
	t.Helper()
	select {
	case <-tw.done:
		if tw.err != nil {
			t.Errorf("testwin exit: %v", tw.err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("testwin did not exit")
	}
}

// startHost opens the stand-in for the RetroPixels app window.
func startHost(t *testing.T, c *x11.Conn) *twin {
	t.Helper()
	h := runTestwin(t, "-name", appName, "-class", "retropixels,"+appName, "-w", "900", "-h", "700")
	h.expect(t, "map")
	waitUntil(t, "the host window", func() bool { return findHost(c) == h.win })
	return h
}

// session runs one docking session like startEmulator does.
type session struct{ done chan struct{} }

func startSession(e *twin, run func(int, <-chan struct{})) *session {
	s := &session{done: make(chan struct{})}
	go func() {
		run(e.pid(), e.done)
		close(s.done)
	}()
	return s
}

func (s *session) wait(t *testing.T) {
	t.Helper()
	select {
	case <-s.done:
	case <-time.After(15 * time.Second):
		t.Fatal("the docking session did not end")
	}
}

func parentOf(c *x11.Conn, w x11.Window) x11.Window {
	_, p, _, err := c.QueryTree(w)
	if err != nil {
		return 0
	}
	return p
}

func isBox(c *x11.Conn, w x11.Window) bool {
	a, _ := c.Atom(dockMarkProp)
	v, _ := c.PropertyUint32s(w, a)
	return len(v) == 1 && int(v[0]) == os.Getpid()
}

// rootRect is w's inside on the root.
func rootRect(t *testing.T, c *x11.Conn, w x11.Window) rect {
	t.Helper()
	g, err := c.GetGeometry(w)
	if err != nil {
		t.Fatal(err)
	}
	x, y, _, err := c.TranslateCoordinates(w, c.Root(), 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	return rect{int(x), int(y), int(g.Width), int(g.Height)}
}

func viewable(c *x11.Conn, w x11.Window) bool {
	a, err := c.GetWindowAttributes(w)
	return err == nil && a.MapState == x11.IsViewable
}

// moveHost moves and sizes the host (through the window manager, when
// there is one, like a user would).
func moveHost(c *x11.Conn, host x11.Window, x, y, w, h int) error {
	return c.MoveResizeWindow(host, x, y, w, h)
}

func bothWMs(t *testing.T, f func(t *testing.T, withWM bool)) {
	t.Run("bare", func(t *testing.T) { f(t, false) })
	t.Run("openbox", func(t *testing.T) { f(t, true) })
}

// ---------------- the tests ----------------

// TestDockChild: a PixelU-Wii/MAME stand-in goes inside the app window,
// below the game bar, follows its size, and Stop closes it.
func TestDockChild(t *testing.T) {
	bothWMs(t, func(t *testing.T, withWM bool) {
		c := startTestX(t, withWM)
		host := startHost(t, c)
		if !hostWindowPresent() {
			t.Fatal("hostWindowPresent = false")
		}
		setEmbedTop(40)
		eng := runTestwin(t, "-name", "Engine", "-w", "320", "-h", "240")
		eng.expect(t, "map")
		s := startSession(eng, embedDolphin)

		var box x11.Window
		waitUntil(t, "the engine in a box in the host", func() bool {
			box = parentOf(c, eng.win)
			return box != 0 && isBox(c, box) && parentOf(c, box) == host.win
		})
		waitUntil(t, "the box placed", func() bool {
			g, _ := c.GetGeometry(box)
			e, _ := c.GetGeometry(eng.win)
			return g.X == 0 && g.Y == 40 && g.Width == 900 && g.Height == 660 &&
				e.X == 0 && e.Y == 0 && e.Width == 900 && e.Height == 660 && viewable(c, eng.win)
		})
		if got := embeddedWindow(eng.pid()); got != uintptr(eng.win) {
			t.Errorf("embeddedWindow = %x, want %x", got, eng.win)
		}
		if _, err := exec.LookPath("xwininfo"); err == nil {
			out, err := exec.Command("xwininfo", "-tree", "-id", x11.FormatWindow(host.win)).CombinedOutput()
			if err != nil || !strings.Contains(string(out), x11.FormatWindow(eng.win)+` "Engine"`) {
				t.Errorf("xwininfo -tree does not show the docked engine: %v\n%s", err, out)
			}
		}
		if findHost(c) != host.win {
			t.Error("the box is taken for the host")
		}
		if withWM {
			// The window manager let go of it (no empty frame left behind).
			list, _ := c.Atom("_NET_CLIENT_LIST")
			waitUntil(t, "the engine out of the window manager's list", func() bool {
				v, _ := c.PropertyUint32s(c.Root(), list)
				return !slices.Contains(v, uint32(eng.win)) && slices.Contains(v, uint32(host.win))
			})
		}

		// The app window gets bigger, the game bar taller.
		if err := moveHost(c, host.win, 10, 10, 1000, 800); err != nil {
			t.Fatal(err)
		}
		setEmbedTop(100)
		waitUntil(t, "the game to follow", func() bool {
			e, _ := c.GetGeometry(eng.win)
			g, _ := c.GetGeometry(box)
			return e.Width == 1000 && e.Height == 700 && g.Y == 100
		})
		// The engine resizes itself: put back.
		if err := c.MoveResizeWindow(eng.win, 5, 5, 200, 200); err != nil {
			t.Fatal(err)
		}
		waitUntil(t, "the game put back", func() bool {
			e, _ := c.GetGeometry(eng.win)
			return e.X == 0 && e.Y == 0 && e.Width == 1000 && e.Height == 700
		})

		if err := stopEmbeddedGame(); err != nil {
			t.Fatal(err)
		}
		eng.expect(t, "delete")
		eng.waitExit(t)
		s.wait(t)
		if err := stopEmbeddedGame(); err == nil {
			t.Error("Stop game with nothing docked worked")
		}
		if embeddedWindow(eng.pid()) != 0 {
			t.Error("embeddedWindow after the game")
		}
	})
}

// TestDockOverlay: a PixelU/Supermodel stand-in in a frameless transient
// window over the page, following the app window, hidden while it is.
func TestDockOverlay(t *testing.T) {
	bothWMs(t, func(t *testing.T, withWM bool) {
		c := startTestX(t, withWM)
		host := startHost(t, c)
		setEmbedTop(30)
		eng := runTestwin(t, "-name", "Engine", "-ignore-delete")
		eng.expect(t, "map")
		s := startSession(eng, embedPixelU)

		var box x11.Window
		waitUntil(t, "the engine in an overlay box", func() bool {
			box = parentOf(c, eng.win)
			return box != 0 && isBox(c, box)
		})
		if p := parentOf(c, box); p == host.win {
			t.Fatal("the overlay box is inside the host")
		} else if !withWM && p != c.Root() {
			t.Errorf("box parent %x, want the root", p)
		}
		if tf, _ := c.TransientFor(box); tf != host.win {
			t.Errorf("WM_TRANSIENT_FOR = %x, want %x", tf, host.win)
		}
		follows := func(what string) {
			t.Helper()
			waitUntil(t, what, func() bool {
				if !viewable(c, box) || !viewable(c, eng.win) {
					return false
				}
				h := rootRect(t, c, host.win)
				return rootRect(t, c, box) == rect{h.X, h.Y + 30, h.W, h.H - 30} &&
					rootRect(t, c, eng.win) == rect{h.X, h.Y + 30, h.W, h.H - 30}
			})
		}
		follows("the overlay over the page")
		if err := moveHost(c, host.win, 200, 150, 800, 600); err != nil {
			t.Fatal(err)
		}
		waitUntil(t, "the host moved", func() bool { r := rootRect(t, c, host.win); return r.W == 800 && r.H == 600 })
		follows("the overlay to follow")

		// Minimized (unmapped): the overlay goes too, and comes back.
		if err := c.UnmapWindow(host.win); err != nil {
			t.Fatal(err)
		}
		waitUntil(t, "the overlay hidden", func() bool { return !viewable(c, box) })
		if err := c.MapWindow(host.win); err != nil {
			t.Fatal(err)
		}
		follows("the overlay back")

		if withWM {
			// The window manager focuses the overlay; the game gets the keyboard.
			waitUntil(t, "the game focused", func() bool {
				f, _ := c.GetInputFocus()
				return f == eng.win
			})
		}

		// Stop: the engine ignores WM_DELETE_WINDOW, so it gets SIGTERM.
		if err := stopEmbeddedGame(); err != nil {
			t.Fatal(err)
		}
		eng.expect(t, "delete")
		eng.expect(t, "sigterm")
		eng.waitExit(t)
		s.wait(t)
	})
}

// TestDockNoDelete: an engine window without WM_DELETE_WINDOW is stopped
// with SIGTERM at once.
func TestDockNoDelete(t *testing.T) {
	c := startTestX(t, false)
	host := startHost(t, c)
	eng := runTestwin(t, "-no-delete")
	s := startSession(eng, embedDolphin)
	waitUntil(t, "docked", func() bool { return parentOf(c, parentOf(c, eng.win)) == host.win })
	waitUntil(t, "embeddedWindow", func() bool { return embeddedWindow(eng.pid()) != 0 })
	start := time.Now()
	if err := stopEmbeddedGame(); err != nil {
		t.Fatal(err)
	}
	eng.expect(t, "sigterm")
	eng.waitExit(t)
	if time.Since(start) > stopGrace {
		t.Errorf("SIGTERM after %v", time.Since(start))
	}
	s.wait(t)
}

// TestDockMarkedUnmapped: an engine that waits unmapped, marked with
// _RETROPIXELS_EMBED, is docked and mapped; its unmapped helper windows
// (Dolphin's hidden main window) are not.
func TestDockMarkedUnmapped(t *testing.T) {
	c := startTestX(t, false)
	startHost(t, c)
	eng := runTestwin(t, "-unmapped", "-w", "50", "-h", "50")
	mark, _ := c.Atom(embedMarkProp)
	if gameScreen(c, eng.pid()) != 0 {
		t.Fatal("an unmarked unmapped window taken for the game screen")
	}
	if err := c.SetPropertyUint32s(eng.win, mark, x11.AtomCardinal, 1); err != nil {
		t.Fatal(err)
	}
	if w := gameScreen(c, eng.pid()); w != eng.win {
		t.Fatalf("gameScreen = %x, want the marked %x", w, eng.win)
	}
	s := startSession(eng, embedSupermodel)
	eng.expect(t, "reparent")
	eng.expect(t, "map")
	waitUntil(t, "mapped in the box", func() bool { return viewable(c, eng.win) && isBox(c, parentOf(c, eng.win)) })
	if err := stopEmbeddedGame(); err != nil {
		t.Fatal(err)
	}
	eng.waitExit(t)
	s.wait(t)
}

// TestDockHostCloses: closing the RetroPixels window ends the game, the
// polite way when the game screen is still there (overlay), else by SIGTERM
// (child: it went with the app window).
func TestDockHostCloses(t *testing.T) {
	for _, overlay := range []bool{true, false} {
		t.Run(map[bool]string{true: "overlay", false: "child"}[overlay], func(t *testing.T) {
			c := startTestX(t, false)
			host := startHost(t, c)
			eng := runTestwin(t)
			s := startSession(eng, func(pid int, ex <-chan struct{}) { embedWindow(pid, ex, overlay) })
			waitUntil(t, "docked", func() bool { return embeddedWindow(eng.pid()) != 0 })
			host.cmd.Process.Kill()
			<-host.done
			if overlay {
				eng.expect(t, "delete")
			} else {
				eng.expect(t, "sigterm")
			}
			eng.waitExit(t)
			s.wait(t)
		})
	}
}

// TestDockSaveSet: if RetroPixels ends while a game is docked, the game
// screen goes back to the desktop instead of being destroyed with our box.
func TestDockSaveSet(t *testing.T) {
	c := startTestX(t, false)
	startHost(t, c)
	eng := runTestwin(t)
	s := startSession(eng, embedPixelU)
	waitUntil(t, "docked", func() bool { return embeddedWindow(eng.pid()) != 0 })
	embedMu.Lock()
	conn := embedConn
	embedMu.Unlock()
	conn.Close() // what RetroPixels ending does to its X connection
	s.wait(t)
	waitUntil(t, "the game back on the root", func() bool {
		return parentOf(c, eng.win) == c.Root() && viewable(c, eng.win)
	})
	select {
	case <-eng.done:
		t.Fatal("the engine ended with RetroPixels' connection")
	default:
	}
}

// TestDockNoHost: without the RetroPixels window nothing is touched.
func TestDockNoHost(t *testing.T) {
	c := startTestX(t, false)
	if hostWindowPresent() {
		t.Fatal("hostWindowPresent with no host")
	}
	eng := runTestwin(t)
	eng.expect(t, "map")
	done := make(chan struct{})
	go func() { embedDolphin(eng.pid(), eng.done); close(done) }()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("embedDolphin kept waiting without a host")
	}
	if parentOf(c, eng.win) != c.Root() {
		t.Error("engine window moved")
	}
	t.Setenv("RETROPIXELS_DOCK", "off")
	startHost(t, c)
	if hostWindowPresent() {
		t.Error("RETROPIXELS_DOCK=off: hostWindowPresent")
	}
}
