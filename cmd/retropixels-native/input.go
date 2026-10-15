package main

import (
	"strconv"
	"strings"
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
	padA, padB, padX, padY = 0, 1, 2, 3
	padLB, padRB           = 4, 5
	padLT, padRT           = 6, 7
	padView, padMenu       = 8, 9
	padL3, padR3           = 10, 11
	padUp, padDown         = 12, 13
	padLeft, padRight      = 14, 15
	padButtons             = 16
)

// Binding is what makes one emulated button go: controller buttons and keys.
// Toggle buttons flip on each press (the Atari's console switches).
type Binding struct {
	Pad    []int    `json:"pad"`
	Keys   []string `json:"keys"`
	Toggle bool     `json:"toggle,omitempty"`
}

type ControlMap map[string]Binding

func b(pad []int, keys ...string) Binding { return Binding{Pad: pad, Keys: keys} }
func tg(pad []int, keys ...string) Binding {
	return Binding{Pad: pad, Keys: keys, Toggle: true}
}

// defaultMaps: DEFAULT_MAPS in host.js for the systems this build has.
var defaultMaps = map[string]ControlMap{
	// Nintendo-style: Xbox A (bottom) = B, Xbox B (right) = A, like the original pads.
	"nes": {"A": b([]int{1}, "x"), "B": b([]int{0}, "z"), "Select": b([]int{8}, "shift"), "Start": b([]int{9}, "enter")},
	"atari2600": {"Fire": b([]int{0, 1}, "z", "x", " "), "Reset": b([]int{9}, "enter"), "Select": b([]int{8}, "shift"),
		"Left difficulty": tg([]int{4}, "1"), "Right difficulty": tg([]int{5}, "2"), "Color / B&W": tg([]int{3}, "3")},
}

// systemButtons: each system's buttons in the core's bit order (Info().Buttons),
// for the Controls window when no game is running.
var systemButtons = map[string][]string{
	"nes":       {"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"},
	"atari2600": {"Up", "Down", "Left", "Right", "Fire", "Select", "Reset", "Left difficulty", "Right difficulty", "Color / B&W"},
	"fake":      testButtons,
}

// systemNotes: the Controls window's note for systems whose controls work
// differently (set by the files that bring the cores).
var systemNotes = map[string]string{}

var arrowKeys = map[string]string{"Up": "arrowup", "Down": "arrowdown", "Left": "arrowleft", "Right": "arrowright"}

// dirs: the D-pad buttons, their controller button and the stick axis/direction (DIRS in host.js).
var dirs = map[string]struct {
	pad, axis int
	sign      float32
}{"Up": {padUp, 1, -1}, "Down": {padDown, 1, 1}, "Left": {padLeft, 0, -1}, "Right": {padRight, 0, 1}}

func cloneMap(m ControlMap) ControlMap {
	out := ControlMap{}
	for k, v := range m {
		out[k] = Binding{Pad: append([]int{}, v.Pad...), Keys: append([]string{}, v.Keys...), Toggle: v.Toggle}
	}
	return out
}

// loadMap: the defaults with the saved changes on top (loadMap in host.js).
func loadMap(sys string, saved ControlMap) ControlMap {
	m := cloneMap(defaultMaps[sys])
	for d, k := range arrowKeys {
		if _, ok := m[d]; !ok {
			m[d] = Binding{Pad: []int{}, Keys: []string{k}}
		}
	}
	for _, n := range systemButtons[sys] {
		if _, ok := m[n]; !ok {
			m[n] = Binding{Pad: []int{}, Keys: []string{}}
		}
	}
	for k, v := range saved {
		if def, ok := m[k]; ok {
			v.Toggle = def.Toggle // whether a switch latches is the system's, not the player's
			if v.Pad == nil {
				v.Pad = []int{}
			}
			if v.Keys == nil {
				v.Keys = []string{}
			}
			m[k] = v
		}
	}
	return m
}

// PadState is one controller, in the standard gamepad layout.
type PadState struct {
	Connected bool
	Buttons   [padButtons]bool
	Axes      [4]float32 // left X, left Y (down = +), right X, right Y (down = +)
}

// inputState turns keys and controllers into the core's button bits.
type inputState struct {
	toggles map[string]bool
	prev    map[string]bool
}

func newInputState() *inputState {
	return &inputState{toggles: map[string]bool{}, prev: map[string]bool{}}
}

// bits for one player (readInput in host.js). pad may be nil; keys count for
// player 0 only. analogStick: the core reads the stick itself.
func (s *inputState) bits(player int, buttons []string, m ControlMap, keys map[string]bool, pad *PadState, analogStick bool) uint32 {
	var bits uint32
	for i, name := range buttons {
		bd := m[name]
		on := false
		if pad != nil {
			for _, p := range bd.Pad {
				if p >= 0 && p < padButtons && pad.Buttons[p] {
					on = true
				}
			}
		}
		if player == 0 {
			for _, k := range bd.Keys {
				if keys[k] {
					on = true
				}
			}
		}
		if d, ok := dirs[name]; ok && pad != nil {
			if pad.Buttons[d.pad] {
				on = true
			}
			ax := pad.Axes[d.axis]
			if !analogStick && ((d.sign < 0 && ax < -0.5) || (d.sign > 0 && ax > 0.5)) {
				on = true
			}
		}
		if bd.Toggle {
			key := string(rune('0'+player)) + ":" + name
			if on && !s.prev[key] {
				s.toggles[key] = !s.toggles[key]
			}
			s.prev[key] = on
			on = s.toggles[key]
		}
		if on && i < 32 {
			bits |= 1 << i
		}
	}
	return bits
}

func (m ControlMap) usesKey(k string) bool {
	for _, bd := range m {
		for _, x := range bd.Keys {
			if x == k {
				return true
			}
		}
	}
	return false
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
