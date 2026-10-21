// Command testwin is a stand-in engine window for X11 tests (docking,
// screen switching, Stop). It creates a top-level window with
// _NET_WM_PID, WM_NAME, WM_CLASS and WM_PROTOCOLS (WM_DELETE_WINDOW),
// maps it, and prints one line per thing that happens to it on stdout:
//
//	window 0x400001
//	map | unmap
//	reparent parent=0x200005 x=0 y=0
//	configure x=0 y=0 w=640 h=480
//	client type=NAME data=1,2,3,4,5
//	delete            (WM_DELETE_WINDOW; it exits 0 unless -ignore-delete)
//	sigterm           (exits 0)
//
// Build it with `go build ./internal/x11/testwin` and run it with DISPLAY
// set.
package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"retropixels/internal/x11"
)

func main() {
	display := flag.String("display", "", "X display (default $DISPLAY)")
	name := flag.String("name", "testwin", "window title")
	class := flag.String("class", "testwin,TestWin", "WM_CLASS instance,class")
	width := flag.Int("w", 320, "width")
	height := flag.Int("h", 240, "height")
	noPID := flag.Bool("no-pid", false, "do not set _NET_WM_PID")
	noDelete := flag.Bool("no-delete", false, "do not take WM_DELETE_WINDOW")
	ignoreDelete := flag.Bool("ignore-delete", false, "print WM_DELETE_WINDOW but keep running")
	unmapped := flag.Bool("unmapped", false, "do not map the window (like an engine waiting to be docked)")
	flag.Parse()
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)

	c, err := x11.Dial(*display)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer c.Close()
	scr := c.DefaultScreen()
	w, err := c.CreateWindow(c.Root(), 0, 0, uint16(*width), uint16(*height), 0, &x11.WindowAttributes{
		Mask:      x11.CWBackPixel | x11.CWEventMask,
		BackPixel: scr.BlackPixel,
		EventMask: x11.StructureNotifyMask | x11.PropertyChangeMask | x11.FocusChangeMask,
	})
	if err == nil {
		err = c.SetWMName(w, *name)
	}
	if err == nil {
		inst, cls, _ := strings.Cut(*class, ",")
		err = c.SetWMClass(w, inst, cls)
	}
	if err == nil && !*noPID {
		err = c.SetWMPID(w, os.Getpid())
	}
	if err == nil && !*noDelete {
		err = c.SetWMProtocols(w, "WM_DELETE_WINDOW")
	}
	if err == nil && !*unmapped {
		err = c.MapWindow(w)
	}
	if err == nil {
		err = c.Sync()
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("window", x11.FormatWindow(w))

	for {
		select {
		case <-sig:
			fmt.Println("sigterm")
			return
		case e, ok := <-c.Events():
			if !ok {
				fmt.Fprintln(os.Stderr, "connection closed")
				os.Exit(1)
			}
			switch e := e.(type) {
			case *x11.MapNotifyEvent:
				if e.Window == w {
					fmt.Println("map")
				}
			case *x11.UnmapNotifyEvent:
				if e.Window == w {
					fmt.Println("unmap")
				}
			case *x11.ReparentNotifyEvent:
				if e.Window == w {
					fmt.Printf("reparent parent=%s x=%d y=%d\n", x11.FormatWindow(e.Parent), e.X, e.Y)
				}
			case *x11.ConfigureNotifyEvent:
				if e.Window == w {
					fmt.Printf("configure x=%d y=%d w=%d h=%d\n", e.X, e.Y, e.Width, e.Height)
				}
			case *x11.ClientMessageEvent:
				if c.IsDeleteWindow(e) {
					fmt.Println("delete")
					if !*ignoreDelete {
						return
					}
					continue
				}
				n, _ := c.AtomName(e.Type)
				d := e.Data32()
				fmt.Printf("client type=%s data=%d,%d,%d,%d,%d\n", n, d[0], d[1], d[2], d[3], d[4])
			}
		}
	}
}
