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
	w("InputAnalogJoyX", "JOY1_XAXIS")
	w("InputAnalogJoyY", y)
	w("InputAnalogJoyTrigger", "KEY_LEFTCTRL,KEY_A,JOY1_BUTTON1,JOY1_RZAXIS_POS")
	w("InputAnalogJoyEvent", "KEY_LEFTALT,KEY_S,JOY1_BUTTON2,JOY1_BUTTON6")
	w("InputAnalogJoyTrigger2", "KEY_D,JOY1_BUTTON3")
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
