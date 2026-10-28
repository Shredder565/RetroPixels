package rpcs3

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// padsMark is the line in settingsMark saying the controllers were set up.
const padsMark = "input/global/Default"

// padConfig: players 1 to 4 on Xbox controllers 1 to 4 (XInput), with the
// usual PS3 layout (A cross, B circle, X square, Y triangle, LB/RB L1/R1,
// LT/RT L2/R2, Back select, Guide PS). Without it RPCS3 gives player 1 the
// keyboard only.
func padConfig() string {
	var b strings.Builder
	for p := 1; p <= 7; p++ {
		fmt.Fprintf(&b, "Player %d Input:\n", p)
		if p > 4 {
			b.WriteString("  Handler: \"Null\"\n  Device: \"Null\"\n")
			continue
		}
		fmt.Fprintf(&b, "  Handler: XInput\n  Device: \"XInput Pad #%d\"\n  Config:\n", p)
		for _, kv := range [][2]string{
			{"Left Stick Left", "LS X-"}, {"Left Stick Down", "LS Y-"}, {"Left Stick Right", "LS X+"}, {"Left Stick Up", "LS Y+"},
			{"Right Stick Left", "RS X-"}, {"Right Stick Down", "RS Y-"}, {"Right Stick Right", "RS X+"}, {"Right Stick Up", "RS Y+"},
			{"Start", "Start"}, {"Select", "Back"}, {"PS Button", "Guide"},
			{"Square", "X"}, {"Cross", "A"}, {"Circle", "B"}, {"Triangle", "Y"},
			{"Left", "Left"}, {"Down", "Down"}, {"Right", "Right"}, {"Up", "Up"},
			{"R1", "RB"}, {"R2", "RT"}, {"R3", "RS"}, {"L1", "LB"}, {"L2", "LT"}, {"L3", "LS"},
		} {
			fmt.Fprintf(&b, "    %s: %s\n", kv[0], kv[1])
		}
		b.WriteString("    Left Stick Deadzone: 7849\n    Right Stick Deadzone: 8689\n")
		b.WriteString("    Left Trigger Threshold: 30\n    Right Trigger Threshold: 30\n")
	}
	return b.String()
}

// preparePads sets up Xbox controllers for every game in RPCS3's folder
// dir, once (a later change made in RPCS3 itself is kept).
func preparePads(dir string) error {
	markPath := filepath.Join(dir, settingsMark)
	mark, _ := os.ReadFile(markPath)
	for _, l := range strings.Split(strings.ReplaceAll(string(mark), "\r\n", "\n"), "\n") {
		if l == padsMark {
			return nil
		}
	}
	p := filepath.Join(dir, "config", "input_configs", "global", "Default.yml")
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(p, []byte(padConfig()), 0o644); err != nil {
		return err
	}
	return os.WriteFile(markPath, append(mark, padsMark+"\r\n"...), 0o644)
}
