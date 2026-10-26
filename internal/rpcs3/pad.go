package rpcs3

// The Xbox controller as player 1. RPCS3 keeps its controller settings in
// config\input_configs\global\Default.yml. When there's none yet,
// RetroPixels writes one with player 1 on the first Xbox controller
// (RPCS3's XInput handler, the PS3 buttons where they sit on an Xbox pad,
// rumble on). A file the player already has is left alone unless they ask
// (UseXboxPad with replace), and then only player 1's part changes; the
// file before is kept as Default.yml.bak.

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
)

// InputConfig is RPCS3's global controller settings file.
func InputConfig(root string) string {
	newer := filepath.Join(root, "config", "input_configs", "global", "Default.yml")
	older := filepath.Join(root, "input_configs", "global", "Default.yml")
	if !isFile(newer) && isFile(older) {
		return older
	}
	return newer
}

// xboxPlayer1 is player 1 on the first Xbox controller. Settings it doesn't
// name keep RPCS3's own defaults.
const xboxPlayer1 = `Player 1 Input:
  Handler: XInput
  Device: XInput Pad #0
  Profile: Default
  Config:
    Left Stick Left: LS X-
    Left Stick Down: LS Y-
    Left Stick Right: LS X+
    Left Stick Up: LS Y+
    Right Stick Left: RS X-
    Right Stick Down: RS Y-
    Right Stick Right: RS X+
    Right Stick Up: RS Y+
    Start: Start
    Select: Back
    PS Button: Guide
    Square: X
    Cross: A
    Circle: B
    Triangle: Y
    Left: Left
    Down: Down
    Right: Right
    Up: Up
    R1: RB
    R2: RT
    R3: RS
    L1: LB
    L2: LT
    L3: LS
    Enable Large Vibration Motor: true
    Enable Small Vibration Motor: true
    Switch Vibration Motors: false
`

// Player1Handler is the controller handler player 1 uses in RPCS3's
// settings ("" when there's no settings file yet).
func Player1Handler(root string) string {
	b, err := os.ReadFile(InputConfig(root))
	if err != nil {
		return ""
	}
	_, block := splitPlayer1(string(b))
	for _, l := range strings.Split(block, "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "Handler:"); ok {
			return strings.Trim(strings.TrimSpace(v), `"'`)
		}
	}
	return ""
}

// splitPlayer1 cuts player 1's part out of the file: the rest (with a mark
// where it was) and the part itself.
func splitPlayer1(s string) (rest, block string) {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	lines := strings.Split(s, "\n")
	start, end := -1, len(lines)
	for i, l := range lines {
		if start < 0 {
			if strings.HasPrefix(l, "Player 1 Input:") {
				start = i
			}
			continue
		}
		if l != "" && l[0] != ' ' && l[0] != '\t' {
			end = i
			break
		}
	}
	if start < 0 {
		return s, ""
	}
	block = strings.Join(lines[start:end], "\n")
	rest = strings.Join(append(append(append([]string(nil), lines[:start]...), "\x00"), lines[end:]...), "\n")
	return rest, block
}

// UseXboxPad sets the Xbox controller up as player 1. Without replace it
// only writes a settings file when there's none (or player 1 has no
// controller at all); with replace it also changes the player's own choice
// for player 1. changed tells whether the file was written.
func UseXboxPad(root string, replace bool) (changed bool, err error) {
	if !isFile(filepath.Join(root, ExeName)) {
		return false, errors.New("RPCS3 isn't set up yet")
	}
	p := InputConfig(root)
	b, err := os.ReadFile(p)
	if errors.Is(err, os.ErrNotExist) {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return false, err
		}
		return true, writeFile(p, []byte(xboxPlayer1))
	}
	if err != nil {
		return false, err
	}
	h := Player1Handler(root)
	if strings.EqualFold(h, "XInput") {
		return false, nil
	}
	if !replace && h != "" && !strings.EqualFold(h, "Null") {
		return false, nil // the player's own choice
	}
	rest, block := splitPlayer1(string(b))
	var out string
	if block == "" {
		out = strings.TrimRight(xboxPlayer1+rest, "\n") + "\n"
	} else {
		out = strings.Replace(rest, "\x00", strings.TrimRight(xboxPlayer1, "\n"), 1)
	}
	if err := writeFile(p+".bak", b); err != nil {
		return false, err
	}
	return true, writeFile(p, []byte(out))
}

func writeFile(p string, b []byte) error {
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}
