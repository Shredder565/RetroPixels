package main

import (
	"strconv"
	"strings"

	"retropixels/internal/games"
)

// Controls, ported from web/emu/host.js.
//
// Controller buttons use the browser's standard gamepad numbering (what an
// Xbox controller reports there), so saved maps look the same as the web
// version's: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 View, 9 Menu,
// 10 L3, 11 R3, 12-15 D-pad up/down/left/right. L3 / R3 are kept for save
// states. Keys use the browser's lower-cased key names ("x", "enter",
// "shift", " ", "arrowup").

var padNames = []string{"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "L3", "R3", "D-pad ↑", "D-pad ↓", "D-pad ←", "D-pad →"}

const (
	padA, padB, padX, padY = games.PadA, games.PadB, games.PadX, games.PadY
	padLB, padRB           = games.PadLB, games.PadRB
	padLT, padRT           = games.PadLT, games.PadRT
	padView, padMenu       = games.PadView, games.PadMenu
	padL3, padR3           = games.PadL3, games.PadR3
	padUp, padDown         = games.PadUp, games.PadDown
	padLeft, padRight      = games.PadLeft, games.PadRight
	padButtons             = games.PadButtons
)

// The control maps, the systems' buttons and the default controls are
// internal/games's (shared with the Android app's engine; allcores fills in
// each core's).

// Binding is what makes one emulated button go: controller buttons and keys.
// Toggle buttons flip on each press (the Atari's console switches).
type Binding = games.Binding

type ControlMap = games.ControlMap

// defaultMaps: DEFAULT_MAPS in host.js for the systems this build has.
var defaultMaps = games.DefaultMaps

// systemButtons: each system's buttons in the core's bit order (Info().Buttons),
// for the Controls window when no game is running.
var systemButtons = games.SystemButtons

func init() { systemButtons["fake"] = testButtons }

// systemNotes: the Controls window's note for systems whose controls work
// differently.
var systemNotes = games.SystemNotes

// loadMap: the defaults with the saved changes on top (loadMap in host.js).
func loadMap(sys string, saved ControlMap) ControlMap { return games.LoadMap(sys, saved) }

// PadState is one controller, in the standard gamepad layout.
type PadState = games.PadState

// inputState turns keys and controllers into the core's button bits.
type inputState struct{ g *games.InputState }

func newInputState() *inputState { return &inputState{games.NewInputState()} }

// bits for one player (readInput in host.js). pad may be nil; keys count for
// player 0 only. analogStick: the core reads the stick itself.
func (s *inputState) bits(player int, buttons []string, m ControlMap, keys map[string]bool, pad *PadState, analogStick bool) uint32 {
	return s.g.Bits(player, buttons, m, keys, pad, analogStick, 0)
}

// keyName: a Windows virtual-key code as the browser's lower-cased key name.
func keyName(vk int) string {
	switch {
	case vk >= 'A' && vk <= 'Z':
		return string(rune(vk + 32))
	case vk >= '0' && vk <= '9':
		return string(rune(vk))
	case vk >= 0x60 && vk <= 0x69: // number pad
		return string(rune('0' + vk - 0x60))
	case vk >= 0x70 && vk <= 0x7b:
		return "f" + strconv.Itoa(vk-0x6f)
	}
	if n, ok := vkNames[vk]; ok {
		return n
	}
	return ""
}

var vkNames = map[int]string{
	0x08: "backspace", 0x09: "tab", 0x0d: "enter", 0x10: "shift", 0x11: "control", 0x12: "alt",
	0x14: "capslock", 0x1b: "escape", 0x20: " ", 0x21: "pageup", 0x22: "pagedown", 0x23: "end", 0x24: "home",
	0x25: "arrowleft", 0x26: "arrowup", 0x27: "arrowright", 0x28: "arrowdown", 0x2d: "insert", 0x2e: "delete",
	0x6a: "*", 0x6b: "+", 0x6d: "-", 0x6e: ".", 0x6f: "/",
	0xa0: "shift", 0xa1: "shift", 0xa2: "control", 0xa3: "control", 0xa4: "alt", 0xa5: "alt",
	0xba: ";", 0xbb: "=", 0xbc: ",", 0xbd: "-", 0xbe: ".", 0xbf: "/", 0xc0: "`", 0xdb: "[", 0xdc: "\\", 0xdd: "]", 0xde: "'",
}

// keyLabel: how a key name is shown in the Controls window.
func keyLabel(k string) string {
	switch k {
	case " ":
		return "Space"
	case "":
		return "—"
	}
	k = strings.TrimPrefix(k, "arrow")
	return strings.ToUpper(k[:1]) + k[1:]
}

func bindingText(bd Binding) (pad, keys string) {
	var p, k []string
	for _, x := range bd.Pad {
		if x >= 0 && x < len(padNames) {
			p = append(p, padNames[x])
		}
	}
	for _, x := range bd.Keys {
		k = append(k, keyLabel(x))
	}
	pad, keys = strings.Join(p, ", "), strings.Join(k, ", ")
	if pad == "" {
		pad = "—"
	}
	if keys == "" {
		keys = "—"
	}
	return
}
