package supermodel

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// iniMarker starts the Supermodel.ini RetroPixels writes. While the file
// still starts with it, RetroPixels writes it again before each game (so
// its settings take effect); once Supermodel itself rewrites the file (its
// -config-inputs screen) or the player edits the marker away, it's left alone.
const iniMarker = "; RetroPixels Supermodel settings v1"

// Controls are Star Wars Trilogy's controls in Supermodel.
type Controls struct {
	// InvertY: pushing the stick up dives, like an aircraft's yoke (the
	// arcade cabinet's flight stick works this way).
	InvertY bool
	// FlightStick: the thumbstick is shaped like a flight stick by
	// RetroPixels' build (FlightStickEnv); Supermodel's own dead zone on
	// the stick is then off, because the shaping has a round one.
	FlightStick bool
}

// Flight stick feel (RetroPixels' build of Supermodel, patch 0002): a round
// dead zone of FlightStickDead %, the round gate stretched to a square,
// a cubic response curve of FlightStickCurve % for fine aim near the
// centre with full deflection at the edge, and the aim smoothed over time
// like a yoke's weight and spring (Smoothness).
const (
	FlightStickCurve = 60
	FlightStickDead  = 5
)

// Smoothness: how much the aim is smoothed, as the filter's time constant.
var Smoothness = map[string]int{"off": 0, "light": 35, "medium": 60, "heavy": 100}

// DefaultSmoothness is used when none is chosen (or an unknown one is).
const DefaultSmoothness = "medium"

// SmoothnessMS is the filter's time constant for a smoothness name.
func SmoothnessMS(name string) int {
	if ms, ok := Smoothness[name]; ok {
		return ms
	}
	return Smoothness[DefaultSmoothness]
}

// FlightStickEnv is the environment variable that turns it on or off.
func FlightStickEnv(on bool, smoothness string) string {
	if !on {
		return "RP_FLIGHTSTICK=off"
	}
	return fmt.Sprintf("RP_FLIGHTSTICK=curve=%d,dead=%d,smooth=%d", FlightStickCurve, FlightStickDead, SmoothnessMS(smoothness))
}

// INI is Supermodel.ini for Star Wars Trilogy Arcade, played from RetroPixels.
//
// Xbox-style controllers (with SDL's game controller support, so every pad
// has the same layout: JOY1_BUTTON1 = A, 2 = B, 3 = X, 4 = Y, 5 = LB,
// 6 = RB, 7 = Back/View, 8 = Start/Menu, 9/10 = stick clicks; ZAXIS and
// RZAXIS are the left and right triggers):
//
//	left stick     flight yoke (aim the crosshair / steer)
//	RT or A        trigger (fire)
//	RB or B        event button (the yoke's thumb button: force / lock-on)
//	Start          start, Back (View) inserts a coin
//	Back + Start   leave the game
//
// The mouse works at the same time as the controller, with nothing to
// switch on: where the pointer is in the game's window is where the yoke
// points (the middle is centre; "Flight-style up / down" doesn't flip
// the mouse, so the aim always follows the pointer), the left button fires, the right button is
// the event button and the middle button is the second trigger. Whichever
// moved last steers.
//
// The keyboard, as in RetroPixels' MAME tab: 5 coin, 1 start, arrow keys
// for the yoke, Ctrl or A to fire, Alt or S for the event button, 9
// service, F2 test menu, Esc leaves.
func INI(c Controls) string {
	y := "JOY1_YAXIS"
	if c.InvertY {
		y = "JOY1_YAXIS_INV"
	}
	var b strings.Builder
	w := func(k, v string) { fmt.Fprintf(&b, "%s = %q\n", k, v) }
	b.WriteString(iniMarker + "\n")
	b.WriteString("; Written by RetroPixels for Star Wars Trilogy Arcade. RetroPixels writes it again\n")
	b.WriteString("; before each game while the first line is there; delete the first line to keep\n")
	b.WriteString("; your own changes (or delete the file to get these settings back).\n\n")
	b.WriteString("[ Global ]\n\n")
	w("InputSystem", InputSystem)
	b.WriteString("\n; Common\n")
	w("InputStart1", "KEY_1,JOY1_BUTTON8")
	w("InputCoin1", "KEY_5,KEY_3,JOY1_BUTTON7")
	w("InputStart2", "KEY_2")
	w("InputCoin2", "KEY_6")
	w("InputServiceA", "KEY_9")
	w("InputTestA", "KEY_F2")
	w("InputServiceB", "NONE")
	w("InputTestB", "NONE")
	b.WriteString("\n; Star Wars Trilogy's flight yoke\n")
	w("InputAnalogJoyLeft", "KEY_LEFT")
	w("InputAnalogJoyRight", "KEY_RIGHT")
	w("InputAnalogJoyUp", "KEY_UP")
	w("InputAnalogJoyDown", "KEY_DOWN")
	w("InputAnalogJoyX", "JOY1_XAXIS,MOUSE_XAXIS")
	w("InputAnalogJoyY", y+",MOUSE_YAXIS")
	if c.FlightStick {
		b.WriteString("; The stick's dead zone is RetroPixels' flight stick shaping's own (round).\n")
		w("InputJoy1XDeadZone", "0")
		w("InputJoy1YDeadZone", "0")
	}
	w("InputAnalogJoyTrigger", "KEY_LEFTCTRL,KEY_A,JOY1_BUTTON1,JOY1_RZAXIS_POS,MOUSE_LEFT_BUTTON")
	w("InputAnalogJoyEvent", "KEY_LEFTALT,KEY_S,JOY1_BUTTON2,JOY1_BUTTON6,MOUSE_RIGHT_BUTTON")
	w("InputAnalogJoyTrigger2", "KEY_D,JOY1_BUTTON3,MOUSE_MIDDLE_BUTTON")
	w("InputAnalogJoyEvent2", "KEY_F,JOY1_BUTTON5")
	b.WriteString("\n; Supermodel's own keys (Esc, or Back + Start on the controller, leaves the game)\n")
	w("InputUIExit", "KEY_ESCAPE,JOY1_BUTTON7+JOY1_BUTTON8")
	w("InputUIReset", "KEY_ALT+KEY_R")
	w("InputUIPause", "KEY_ALT+KEY_P,KEY_PAUSE")
	w("InputUISaveState", "KEY_F5")
	w("InputUIChangeSlot", "KEY_F6")
	w("InputUILoadState", "KEY_F7")
	w("InputUIClearNVRAM", "KEY_ALT+KEY_N")
	b.WriteString("\n; Star Wars Trilogy's music board is louder than its sound effects (Supermodel's README)\n")
	for _, s := range RomSets {
		fmt.Fprintf(&b, "\n[ %s ]\n", s.Name)
		w("MusicVolume", "70")
		w("SoundVolume", "100")
	}
	return b.String()
}

// WriteINI writes Supermodel.ini at path when it isn't there yet or is still
// RetroPixels' own. It says whether it wrote it.
func WriteINI(path string, c Controls) (bool, error) {
	if b, err := os.ReadFile(path); err == nil {
		if !bytes.HasPrefix(b, []byte(iniMarker)) {
			return false, nil // the player's own settings
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return false, err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(strings.ReplaceAll(INI(c), "\n", "\r\n")), 0o644); err != nil {
		return false, err
	}
	return true, os.Rename(tmp, path)
}

// Ours: the Supermodel.ini at path is still the one RetroPixels writes.
func Ours(path string) bool {
	b, err := os.ReadFile(path)
	return err != nil || bytes.HasPrefix(b, []byte(iniMarker))
}
