//go:build linux

package pixelu

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"retropixels/internal/x11"
)

// These tests start their own Xvfb. Without Xvfb they are skipped, unless
// RETRO_X11_REQUIRE=1.
//
// TestPixelUDocking* run a real PixelU (or anything that behaves like it)
// when RETRO_PIXELU_BIN names it; RETRO_PIXELU_ARGS adds arguments (CI
// passes "-g <demo.rpx>"). They check RPEmbed's X11 contract
// (pixelu/src/gui/wxgui/RPEmbedX11.h): hidden until docked, the screen
// switch, docking by WM_TRANSIENT_FOR and by --rp-parent, closing with the
// host window, WM_DELETE_WINDOW and SIGTERM.

func startXvfb(t *testing.T) {
	t.Helper()
	bin, err := exec.LookPath("Xvfb")
	if err != nil {
		if os.Getenv("RETRO_X11_REQUIRE") == "1" {
			t.Fatal("Xvfb not found and RETRO_X11_REQUIRE=1")
		}
		t.Skip("Xvfb not installed")
	}
	pr, pw, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(bin, "-displayfd", "3", "-nolisten", "tcp", "-screen", "0", "1280x1024x24")
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
		t.Setenv("DISPLAY", ":"+n)
		t.Setenv("XAUTHORITY", filepath.Join(t.TempDir(), "none"))
	case <-time.After(20 * time.Second):
		t.Fatalf("Xvfb did not start in time: %s", stderr.String())
	}
}

func dial(t *testing.T) *x11.Conn {
	t.Helper()
	c, err := x11.Dial("")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

// fakePixelU answers screen requests on its own window like PixelU does.
func fakePixelU(t *testing.T) x11.Window {
	t.Helper()
	c := dial(t)
	w, err := c.CreateWindow(c.Root(), 0, 0, 100, 100, 0, nil)
	if err != nil {
		t.Fatal(err)
	}
	a, err := c.Atom(ScreenAtomName)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.SetPropertyUint32s(w, a, x11.AtomCardinal, 0, 0); err != nil {
		t.Fatal(err)
	}
	if err := c.Sync(); err != nil {
		t.Fatal(err)
	}
	go func() {
		pad := false
		for e := range c.Events() {
			m, ok := e.(*x11.ClientMessageEvent)
			if !ok || m.Type != a {
				continue
			}
			d := m.Data32()
			switch int32(d[0]) {
			case 0:
				pad = false
			case 1:
				pad = true
			case 2:
				pad = !pad
			}
			v := uint32(0)
			if pad {
				v = 1
			}
			c.SetPropertyUint32s(m.Window, a, x11.AtomCardinal, d[1], v)
		}
	}()
	return w
}

func TestSetScreenX11(t *testing.T) {
	startXvfb(t)
	w := fakePixelU(t)
	for _, step := range []struct {
		mode int
		want bool
	}{{ScreenGamePad, true}, {-1, true}, {ScreenTV, false}, {ScreenSwitch, true}, {ScreenSwitch, false}} {
		got, err := SetScreen(uintptr(w), step.mode)
		if err != nil || got != step.want {
			t.Fatalf("SetScreen(%d) = %v, %v; want %v", step.mode, got, err, step.want)
		}
	}

	// a window without the property is no PixelU game window
	c := dial(t)
	other, _ := c.CreateWindow(c.Root(), 0, 0, 10, 10, 0, nil)
	c.Sync()
	if IsGameWindow(c, other) || !IsGameWindow(c, w) {
		t.Error("IsGameWindow")
	}
	if _, err := SetScreen(uintptr(other), ScreenTV); err == nil || !strings.Contains(err.Error(), "older PixelU") {
		t.Errorf("not PixelU: %v", err)
	}
	c.DestroyWindow(other)
	c.Sync()
	if _, err := SetScreen(uintptr(other), ScreenTV); err == nil || !strings.Contains(err.Error(), "gone") {
		t.Errorf("gone: %v", err)
	}
	if _, err := SetScreen(0, ScreenTV); err == nil {
		t.Error("window 0 accepted")
	}
}

// ---- a real PixelU ----

type pixeluProc struct {
	cmd    *exec.Cmd
	exited chan struct{} // closed when it ended, err is then its exit
	err    error
}

func startPixelU(t *testing.T, args ...string) *pixeluProc {
	t.Helper()
	bin := os.Getenv("RETRO_PIXELU_BIN")
	if bin == "" {
		t.Skip("RETRO_PIXELU_BIN not set")
	}
	args = append(args, strings.Fields(os.Getenv("RETRO_PIXELU_ARGS"))...)
	cmd := Command(bin, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	p := &pixeluProc{cmd: cmd, exited: make(chan struct{})}
	go func() {
		p.err = cmd.Wait()
		close(p.exited)
	}()
	t.Cleanup(func() {
		select {
		case <-p.exited:
		default:
			cmd.Process.Kill()
			<-p.exited
		}
	})
	return p
}

// gameWindow waits for PixelU's game window.
func gameWindow(t *testing.T, c *x11.Conn, p *pixeluProc) x11.Window {
	t.Helper()
	deadline := time.Now().Add(90 * time.Second) // the first start builds caches
	for time.Now().Before(deadline) {
		select {
		case <-p.exited:
			t.Fatalf("PixelU ended before showing a window: %v", p.err)
		default:
		}
		ws, err := c.WindowsByPID(p.cmd.Process.Pid)
		if err == nil {
			for _, w := range ws {
				if IsGameWindow(c, w) {
					return w
				}
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("no PixelU game window")
	return 0
}

func mapState(t *testing.T, c *x11.Conn, w x11.Window) uint8 {
	t.Helper()
	a, err := c.GetWindowAttributes(w)
	if err != nil {
		t.Fatal(err)
	}
	return a.MapState
}

func waitUntil(t *testing.T, what string, timeout time.Duration, ok func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for !ok() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func waitExit(t *testing.T, p *pixeluProc, timeout time.Duration) error {
	t.Helper()
	select {
	case <-p.exited:
		return p.err
	case <-time.After(timeout):
		t.Fatalf("PixelU still running after %v", timeout)
		return nil
	}
}

func hostWindow(t *testing.T, c *x11.Conn, w, h uint16) x11.Window {
	t.Helper()
	host, err := c.CreateWindow(c.Root(), 0, 0, w, h, 0, &x11.WindowAttributes{Mask: x11.CWBackPixel, BackPixel: c.DefaultScreen().WhitePixel})
	if err == nil {
		err = c.SetWMPID(host, os.Getpid())
	}
	if err == nil {
		err = c.MapWindow(host)
	}
	if err == nil {
		err = c.Sync()
	}
	if err != nil {
		t.Fatal(err)
	}
	return host
}

func TestPixelUDockingTransient(t *testing.T) {
	if os.Getenv("RETRO_PIXELU_BIN") == "" {
		t.Skip("RETRO_PIXELU_BIN not set")
	}
	startXvfb(t)
	c := dial(t)
	p := startPixelU(t, "--rp-embed")
	w := gameWindow(t, c, p)

	// hidden while it waits to be docked
	time.Sleep(500 * time.Millisecond)
	if s := mapState(t, c, w); s != x11.IsUnmapped {
		t.Fatalf("game window map state %d before docking", s)
	}
	// the screen switch works while hidden too
	for _, step := range []struct {
		mode int
		want bool
	}{{ScreenGamePad, true}, {ScreenSwitch, false}, {-1, false}} {
		if got, err := SetScreen(uintptr(w), step.mode); err != nil || got != step.want {
			t.Fatalf("SetScreen(%d) = %v, %v", step.mode, got, err)
		}
	}

	// dock: place it, then make it transient for our window
	host := hostWindow(t, c, 800, 600)
	if err := c.MoveResizeWindow(w, 40, 30, 640, 360); err != nil {
		t.Fatal(err)
	}
	if err := c.SetTransientFor(w, host); err != nil {
		t.Fatal(err)
	}
	c.Sync()
	waitUntil(t, "the docked window to be shown", 5*time.Second, func() bool { return mapState(t, c, w) == x11.IsViewable })
	time.Sleep(time.Second) // let GTK settle
	if g, err := c.GetGeometry(w); err != nil || g.Width != 640 || g.Height != 360 {
		t.Errorf("docked geometry %+v, %v; want 640x360 where RetroPixels put it", g, err)
	}
	shot(t, w)

	// RetroPixels goes away: PixelU closes
	c.DestroyWindow(host)
	c.Sync()
	if err := waitExit(t, p, 30*time.Second); err != nil {
		t.Errorf("exit after host closed: %v", err)
	}
}

// The way RetroPixels docks it (embed_linux.go): the unmapped window marked
// _RETROPIXELS_EMBED is reparented into a box carrying _RETROPIXELS_DOCK,
// sized, and mapped.
func TestPixelUDockingBox(t *testing.T) {
	if os.Getenv("RETRO_PIXELU_BIN") == "" {
		t.Skip("RETRO_PIXELU_BIN not set")
	}
	startXvfb(t)
	c := dial(t)
	p := startPixelU(t, "--rp-embed")
	w := gameWindow(t, c, p)
	mark, _ := c.Atom("_RETROPIXELS_EMBED")
	if v, err := c.PropertyUint32s(w, mark); err != nil || len(v) != 1 || v[0] != 1 {
		t.Fatalf("_RETROPIXELS_EMBED = %v, %v", v, err)
	}
	if s := mapState(t, c, w); s != x11.IsUnmapped {
		t.Fatalf("game window map state %d before docking", s)
	}
	box := hostWindow(t, c, 960, 540)
	dock, _ := c.Atom("_RETROPIXELS_DOCK")
	c.SetPropertyUint32s(box, dock, x11.AtomCardinal, uint32(os.Getpid()))
	c.ChangeSaveSet(x11.SetModeInsert, w)
	if err := c.ReparentWindow(w, box, 0, 0); err != nil {
		t.Fatal(err)
	}
	// mapped at once, before PixelU looks (GTK must not unmap it again), and sized later
	c.MapWindow(w)
	c.Sync()
	time.Sleep(300 * time.Millisecond)
	c.MoveResizeWindow(w, 0, 0, 960, 540)
	c.Sync()
	waitUntil(t, "the docked window to fill the box", 10*time.Second, func() bool {
		g, err := c.GetGeometry(w)
		return err == nil && mapState(t, c, w) == x11.IsViewable && g.X == 0 && g.Y == 0 && g.Width == 960 && g.Height == 540
	})
	time.Sleep(time.Second)
	if g, _ := c.GetGeometry(w); g.Width != 960 || g.Height != 540 {
		t.Errorf("GTK changed the docked size to %dx%d", g.Width, g.Height)
	}
	// the page below the game bar changes size
	c.MoveResizeWindow(box, 0, 0, 800, 450)
	c.MoveResizeWindow(w, 0, 0, 800, 450)
	c.Sync()
	waitUntil(t, "the window to follow the box", 5*time.Second, func() bool {
		g, err := c.GetGeometry(w)
		return err == nil && g.Width == 800 && g.Height == 450
	})
	if on, err := SetScreen(uintptr(w), ScreenGamePad); err != nil || !on {
		t.Errorf("SetScreen while docked = %v, %v", on, err)
	}
	shot(t, box)
	if err := c.SendDeleteWindow(w); err != nil {
		t.Fatal(err)
	}
	c.Sync()
	if err := waitExit(t, p, 30*time.Second); err != nil {
		t.Errorf("exit after WM_DELETE_WINDOW: %v", err)
	}
}

func TestPixelUDockingParent(t *testing.T) {
	if os.Getenv("RETRO_PIXELU_BIN") == "" {
		t.Skip("RETRO_PIXELU_BIN not set")
	}
	startXvfb(t)
	c := dial(t)
	host := hostWindow(t, c, 800, 450)
	p := startPixelU(t, "--rp-parent", x11.FormatWindow(host))
	w := gameWindow(t, c, p)
	waitUntil(t, "the window inside its parent", 10*time.Second, func() bool {
		_, parent, _, err := c.QueryTree(w)
		return err == nil && parent == host && mapState(t, c, w) == x11.IsViewable
	})
	waitUntil(t, "the window to fill its parent", 5*time.Second, func() bool {
		g, err := c.GetGeometry(w)
		return err == nil && g.X == 0 && g.Y == 0 && g.Width == 800 && g.Height == 450
	})
	// it follows the parent's size
	c.MoveResizeWindow(host, 0, 0, 1024, 576)
	c.Sync()
	waitUntil(t, "the window to follow its parent", 5*time.Second, func() bool {
		g, err := c.GetGeometry(w)
		return err == nil && g.Width == 1024 && g.Height == 576
	})
	shot(t, host)

	// Stop: WM_DELETE_WINDOW
	if err := c.SendDeleteWindow(w); err != nil {
		t.Fatal(err)
	}
	c.Sync()
	if err := waitExit(t, p, 30*time.Second); err != nil {
		t.Errorf("exit after WM_DELETE_WINDOW: %v", err)
	}
}

func TestPixelUStopSignal(t *testing.T) {
	if os.Getenv("RETRO_PIXELU_BIN") == "" {
		t.Skip("RETRO_PIXELU_BIN not set")
	}
	startXvfb(t)
	c := dial(t)
	p := startPixelU(t, "--rp-embed")
	w := gameWindow(t, c, p)
	host := hostWindow(t, c, 640, 360)
	c.SetTransientFor(w, host)
	c.Sync()
	waitUntil(t, "the docked window to be shown", 5*time.Second, func() bool { return mapState(t, c, w) == x11.IsViewable })
	p.cmd.Process.Signal(syscall.SIGTERM)
	if err := waitExit(t, p, 30*time.Second); err != nil {
		t.Errorf("exit after SIGTERM: %v (want a normal close, code 0)", err)
	}
}

// shot saves a screenshot of w to $RETRO_PIXELU_SHOTS (when set and
// ImageMagick's import is there), for CI to check and upload.
func shot(t *testing.T, w x11.Window) {
	dir := os.Getenv("RETRO_PIXELU_SHOTS")
	if dir == "" {
		return
	}
	imp, err := exec.LookPath("import")
	if err != nil {
		return
	}
	time.Sleep(3 * time.Second) // a few frames of the game
	out := filepath.Join(dir, fmt.Sprintf("%s.png", t.Name()))
	if b, err := exec.Command(imp, "-window", x11.FormatWindow(w), out).CombinedOutput(); err != nil {
		t.Logf("screenshot: %v %s", err, b)
	}
}
