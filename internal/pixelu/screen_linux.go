//go:build linux

package pixelu

import (
	"errors"
	"time"

	"retropixels/internal/x11"
)

// ScreenAtomName names both PixelU's screen-switch ClientMessage and the
// property its game window carries (CARDINAL[2]: the number of the last
// request it answered, then 1 while the GamePad screen is shown). The
// property also tells PixelU's game window apart from its other windows.
// See pixelu/src/gui/wxgui/RPEmbedX11.h for PixelU's side.
const ScreenAtomName = "PixelU.Screen"

// IsGameWindow reports whether w is a PixelU game window (it carries the
// "PixelU.Screen" property).
func IsGameWindow(c *x11.Conn, w x11.Window) bool {
	a, err := c.Atom(ScreenAtomName)
	if err != nil {
		return false
	}
	v, err := c.PropertyUint32s(w, a)
	return err == nil && len(v) >= 2
}

// SetScreen picks what PixelU's game window (its X11 window id) shows: the
// TV picture, the GamePad's screen, or the other one of the two. It
// returns whether the GamePad screen is shown now. Any other mode only
// asks. With the GamePad screen shown, a click or a touch on the picture is
// a touch on the GamePad's touch screen.
func SetScreen(window uintptr, mode int) (bool, error) {
	if window == 0 || window > 0xFFFFFFFF {
		return false, errors.New("couldn't reach PixelU")
	}
	c, err := x11.Dial("")
	if err != nil {
		return false, errors.New("couldn't reach PixelU: " + err.Error())
	}
	defer c.Close()
	return setScreen(c, x11.Window(window), mode, 2*time.Second)
}

func setScreen(c *x11.Conn, w x11.Window, mode int, timeout time.Duration) (bool, error) {
	a, err := c.Atom(ScreenAtomName)
	if err != nil {
		return false, err
	}
	v, err := c.PropertyUint32s(w, a)
	if err != nil {
		if x11.WindowGone(err) {
			return false, errors.New("PixelU's window is gone")
		}
		return false, err
	}
	if len(v) < 2 {
		return false, errors.New("PixelU didn't answer (an older PixelU build can't show the GamePad screen here)")
	}
	req := v[0] + 1
	if req == 0 {
		req = 1
	}
	// Event mask 0: the message goes to the client that created the
	// window, PixelU.
	if err := c.SendClientMessage(w, w, a, 0, [5]uint32{uint32(int32(mode)), req}); err != nil {
		return false, err
	}
	deadline := time.Now().Add(timeout)
	for {
		v, err := c.PropertyUint32s(w, a)
		if err != nil {
			return false, err
		}
		if len(v) >= 2 && v[0] == req {
			return v[1] == 1, nil
		}
		if time.Now().After(deadline) {
			return false, errors.New("PixelU didn't answer")
		}
		time.Sleep(10 * time.Millisecond)
	}
}
