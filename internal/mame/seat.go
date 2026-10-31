package mame

// Choose your player: the player's keyboard and first controller (the Xbox
// controller) play as player 2, 3 or 4 instead of player 1, for one MAME
// game or all of them. MAME is given a controller file (-ctrlr) that swaps
// that player's controls with player 1's; nothing in MAME's own settings
// changes.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// MaxSeats: players 1 to 4 can be chosen.
const MaxSeats = 4

// seatKeys: MAME's default keys for players 1 to 4, in the order
// up, down, left, right, button 1, 2, 3, 4, 5, 6, start, coin ("" = none).
var seatKeys = [MaxSeats][12]string{
	{"KEYCODE_UP", "KEYCODE_DOWN", "KEYCODE_LEFT", "KEYCODE_RIGHT", "KEYCODE_LCONTROL", "KEYCODE_LALT", "KEYCODE_SPACE", "KEYCODE_LSHIFT", "KEYCODE_Z", "KEYCODE_X", "KEYCODE_1", "KEYCODE_5"},
	{"KEYCODE_R", "KEYCODE_F", "KEYCODE_D", "KEYCODE_G", "KEYCODE_A", "KEYCODE_S", "KEYCODE_Q", "KEYCODE_W", "KEYCODE_E", "", "KEYCODE_2", "KEYCODE_6"},
	{"KEYCODE_I", "KEYCODE_K", "KEYCODE_J", "KEYCODE_L", "KEYCODE_RCONTROL", "KEYCODE_RSHIFT", "KEYCODE_ENTER", "", "", "", "KEYCODE_3", "KEYCODE_7"},
	{"KEYCODE_8_PAD", "KEYCODE_2_PAD", "KEYCODE_4_PAD", "KEYCODE_6_PAD", "KEYCODE_0_PAD", "KEYCODE_DEL_PAD", "KEYCODE_ENTER_PAD", "", "", "", "KEYCODE_4", "KEYCODE_8"},
}

// seatControls: what each control is called for player n (1-based) in MAME,
// and the controller's input for it on joystick j.
func seatPorts(n int) [12]string {
	p := fmt.Sprintf("P%d_", n)
	return [12]string{p + "JOYSTICK_UP", p + "JOYSTICK_DOWN", p + "JOYSTICK_LEFT", p + "JOYSTICK_RIGHT",
		p + "BUTTON1", p + "BUTTON2", p + "BUTTON3", p + "BUTTON4", p + "BUTTON5", p + "BUTTON6",
		fmt.Sprintf("START%d", n), fmt.Sprintf("COIN%d", n)}
}

func joyCodes(j int) [12]string {
	c := fmt.Sprintf("JOYCODE_%d_", j)
	return [12]string{
		c + "YAXIS_UP_SWITCH OR " + c + "HAT1UP", c + "YAXIS_DOWN_SWITCH OR " + c + "HAT1DOWN",
		c + "XAXIS_LEFT_SWITCH OR " + c + "HAT1LEFT", c + "XAXIS_RIGHT_SWITCH OR " + c + "HAT1RIGHT",
		c + "BUTTON1", c + "BUTTON2", c + "BUTTON3", c + "BUTTON4", c + "BUTTON5", c + "BUTTON6",
		c + "START", c + "SELECT"}
}

// controls is what player `as` (1-based) is played with: keyboard keys of
// player `keys` and controller number `keys`.
func controls(keys int) [12]string {
	var out [12]string
	j := joyCodes(keys)
	for i := range out {
		if k := seatKeys[keys-1][i]; k != "" {
			out[i] = k + " OR " + j[i]
		} else {
			out[i] = j[i]
		}
	}
	return out
}

// SeatConfig is the MAME controller file that makes player seat (1-based,
// 2 to 4) play with player 1's keys and the first controller, and player 1
// with that player's keys and controller.
func SeatConfig(seat int) string {
	var b strings.Builder
	b.WriteString("<?xml version=\"1.0\"?>\n<!-- RetroPixels: you play as player " + fmt.Sprint(seat) + " -->\n")
	b.WriteString("<mameconfig version=\"10\">\n    <system name=\"default\">\n        <input>\n")
	write := func(ports, seqs [12]string) {
		for i, port := range ports {
			fmt.Fprintf(&b, "            <port type=\"%s\">\n                <newseq type=\"standard\">%s</newseq>\n            </port>\n", port, seqs[i])
		}
	}
	write(seatPorts(seat), controls(1))
	write(seatPorts(1), controls(seat))
	b.WriteString("        </input>\n    </system>\n</mameconfig>\n")
	return b.String()
}

// SeatArgs writes the controller file for seat (1-based) into dir and
// returns MAME's options for it. Player 1 (or a MAME without -ctrlr) needs
// none.
func SeatArgs(seat int, dir string, opts Options) []string {
	if seat < 2 || seat > MaxSeats || dir == "" {
		return nil
	}
	if opts != nil && (!opts["ctrlr"] || !opts["ctrlrpath"]) {
		return nil
	}
	name := fmt.Sprintf("retropixels-player%d", seat)
	if os.MkdirAll(dir, 0o755) != nil || os.WriteFile(filepath.Join(dir, name+".cfg"), []byte(SeatConfig(seat)), 0o644) != nil {
		return nil
	}
	return []string{"-ctrlrpath", dir, "-ctrlr", name}
}

// SeatDir is where the choose-your-player controller files are kept: next to
// the MAME settings (%APPDATA%\RetroPixels\MAME-controls).
func SeatDir() string { return filepath.Join(filepath.Dir(ConfigPath()), "MAME-controls") }
