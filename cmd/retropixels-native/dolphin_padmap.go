package main

// The parts of the "Map controller buttons" window (GameCube / Wii) that
// aren't Windows calls: which engine input a pressed controller button,
// moved stick or pressed key is, as GCPadNew.ini / WiimoteNew.ini name it
// (dolphin_padmap_windows.go has the window).

import "fmt"

// dolPadInputs: the Xbox controller's buttons as the engine's XInput
// device names them, by the app's pad button number.
var dolPadInputs = map[int]string{
	padA: "Button A", padB: "Button B", padX: "Button X", padY: "Button Y",
	padLB: "Shoulder L", padRB: "Shoulder R", padLT: "Trigger L", padRT: "Trigger R",
	padView: "Back", padMenu: "Start", padL3: "Thumb L", padR3: "Thumb R",
	padUp: "Pad N", padDown: "Pad S", padLeft: "Pad W", padRight: "Pad E",
}

// dolStickPush: how far a stick has to move to count as pressed.
const dolStickPush = 0.6

// dolStickInputs: the stick directions, by axis (PadState.Axes: left X,
// left Y, right X, right Y; Y is down = +) and sign.
var dolStickInputs = [4][2]string{
	{"Left X-", "Left X+"}, {"Left Y+", "Left Y-"}, {"Right X-", "Right X+"}, {"Right Y+", "Right Y-"},
}

// dolPadPressed: the engine's name of an input pressed on now that wasn't
// on before, or "".
func dolPadPressed(before, now PadState) string {
	if !now.Connected {
		return ""
	}
	for b := 0; b < padButtons; b++ {
		if now.Buttons[b] && !before.Buttons[b] {
			if n, ok := dolPadInputs[b]; ok {
				return n
			}
		}
	}
	for i, ax := range now.Axes {
		was := before.Axes[i]
		switch {
		case ax <= -dolStickPush && was > -dolStickPush:
			return dolStickInputs[i][0]
		case ax >= dolStickPush && was < dolStickPush:
			return dolStickInputs[i][1]
		}
	}
	return ""
}

// dolVKKeys: Windows virtual keys as the engine's keyboard names them (its
// DirectInput key names). Letters and digits are their own names.
var dolVKKeys = map[int]string{
	0x08: "BACK", 0x09: "TAB", 0x0D: "RETURN", 0x13: "PAUSE", 0x14: "CAPITAL", 0x1B: "ESCAPE", 0x20: "SPACE",
	0x21: "PRIOR", 0x22: "NEXT", 0x23: "END", 0x24: "HOME",
	0x25: "LEFT", 0x26: "UP", 0x27: "RIGHT", 0x28: "DOWN",
	0x2C: "SYSRQ", 0x2D: "INSERT", 0x2E: "DELETE",
	0x5B: "LWIN", 0x5C: "RWIN", 0x5D: "APPS",
	0x6A: "MULTIPLY", 0x6B: "ADD", 0x6D: "SUBTRACT", 0x6E: "DECIMAL", 0x6F: "DIVIDE",
	0x90: "NUMLOCK", 0x91: "SCROLL",
	0xA0: "LSHIFT", 0xA1: "RSHIFT", 0xA2: "LCONTROL", 0xA3: "RCONTROL", 0xA4: "LMENU", 0xA5: "RMENU",
	0xBA: "SEMICOLON", 0xBB: "EQUALS", 0xBC: "COMMA", 0xBD: "MINUS", 0xBE: "PERIOD", 0xBF: "SLASH",
	0xC0: "GRAVE", 0xDB: "LBRACKET", 0xDC: "BACKSLASH", 0xDD: "RBRACKET", 0xDE: "APOSTROPHE", 0xE2: "OEM_102",
}

// dolKeyInput: a key press (its virtual key and WM_KEYDOWN's lParam) as the
// engine's keyboard input name, or "". Shift, Ctrl and Alt come as their
// left / right keys.
func dolKeyInput(vk int, lp uintptr) string {
	extended := lp&(1<<24) != 0
	scan := (lp >> 16) & 0xff
	switch {
	case vk >= 'A' && vk <= 'Z', vk >= '0' && vk <= '9':
		return string(rune(vk))
	case vk >= 0x60 && vk <= 0x69: // numpad digits
		return fmt.Sprintf("NUMPAD%d", vk-0x60)
	case vk >= 0x70 && vk <= 0x7E: // F1..F15
		return fmt.Sprintf("F%d", vk-0x6F)
	case vk == 0x10: // Shift: the right one has scan code 0x36
		if scan == 0x36 {
			return "RSHIFT"
		}
		return "LSHIFT"
	case vk == 0x11:
		if extended {
			return "RCONTROL"
		}
		return "LCONTROL"
	case vk == 0x12:
		if extended {
			return "RMENU"
		}
		return "LMENU"
	case vk == 0x0D && extended:
		return "NUMPADENTER"
	}
	return dolVKKeys[vk]
}

// dolMouseInputs: mouse buttons (WM_LBUTTONDOWN, RBUTTON, MBUTTON) as the
// engine names them.
var dolMouseInputs = map[uint32]string{0x0201: "Click 0", 0x0204: "Click 1", 0x0207: "Click 2"}
