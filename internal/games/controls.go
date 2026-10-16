package games

// Controls, ported from web/emu/host.js.
//
// Controller buttons use the browser's standard gamepad numbering (what an
// Xbox controller reports there), so saved maps look the same as the web
// version's: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 View, 9 Menu,
// 10 L3, 11 R3, 12-15 D-pad up/down/left/right. Keys use the browser's
// lower-cased key names ("x", "enter", "shift", " ", "arrowup").

const (
	PadA, PadB, PadX, PadY = 0, 1, 2, 3
	PadLB, PadRB           = 4, 5
	PadLT, PadRT           = 6, 7
	PadView, PadMenu       = 8, 9
	PadL3, PadR3           = 10, 11
	PadUp, PadDown         = 12, 13
	PadLeft, PadRight      = 14, 15
	PadButtons             = 16
)

// Binding is what makes one emulated button go: controller buttons and keys.
// Toggle buttons flip on each press (the Atari's console switches).
type Binding struct {
	Pad    []int    `json:"pad"`
	Keys   []string `json:"keys"`
	Toggle bool     `json:"toggle,omitempty"`
}

type ControlMap map[string]Binding

// B and TG make bindings for the default maps (TG: a toggle).
func B(pad []int, keys ...string) Binding { return Binding{Pad: pad, Keys: keys} }
func TG(pad []int, keys ...string) Binding {
	return Binding{Pad: pad, Keys: keys, Toggle: true}
}

// DefaultMaps: DEFAULT_MAPS in host.js (the cores' entries come from allcores).
var DefaultMaps = map[string]ControlMap{
	// Nintendo-style: Xbox A (bottom) = B, Xbox B (right) = A, like the original pads.
	"nes": {"A": B([]int{1}, "x"), "B": B([]int{0}, "z"), "Select": B([]int{8}, "shift"), "Start": B([]int{9}, "enter")},
	"atari2600": {"Fire": B([]int{0, 1}, "z", "x", " "), "Reset": B([]int{9}, "enter"), "Select": B([]int{8}, "shift"),
		"Left difficulty": TG([]int{4}, "1"), "Right difficulty": TG([]int{5}, "2"), "Color / B&W": TG([]int{3}, "3")},
}

// SystemButtons: each system's buttons in the core's bit order (Info().Buttons),
// for the controls screens when no game is running.
var SystemButtons = map[string][]string{
	"nes":       {"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"},
	"atari2600": {"Up", "Down", "Left", "Right", "Fire", "Select", "Reset", "Left difficulty", "Right difficulty", "Color / B&W"},
}

// SystemNotes: the controls screens' note for systems whose controls work
// differently.
var SystemNotes = map[string]string{}

var ArrowKeys = map[string]string{"Up": "arrowup", "Down": "arrowdown", "Left": "arrowleft", "Right": "arrowright"}

// Dirs: the D-pad buttons, their controller button and the stick axis/direction (DIRS in host.js).
var Dirs = map[string]struct {
	Pad, Axis int
	Sign      float32
}{"Up": {PadUp, 1, -1}, "Down": {PadDown, 1, 1}, "Left": {PadLeft, 0, -1}, "Right": {PadRight, 0, 1}}

func CloneMap(m ControlMap) ControlMap {
	out := ControlMap{}
	for k, v := range m {
		out[k] = Binding{Pad: append([]int{}, v.Pad...), Keys: append([]string{}, v.Keys...), Toggle: v.Toggle}
	}
	return out
}

// LoadMap: the defaults with the saved changes on top (loadMap in host.js).
func LoadMap(sys string, saved ControlMap) ControlMap {
	m := CloneMap(DefaultMaps[sys])
	for d, k := range ArrowKeys {
		if _, ok := m[d]; !ok {
			m[d] = Binding{Pad: []int{}, Keys: []string{k}}
		}
	}
	for _, n := range SystemButtons[sys] {
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

// UsesKey: some button is worked by key k.
func (m ControlMap) UsesKey(k string) bool {
	for _, bd := range m {
		for _, x := range bd.Keys {
			if x == k {
				return true
			}
		}
	}
	return false
}

// PadState is one controller, in the standard gamepad layout.
type PadState struct {
	Connected bool
	Buttons   [PadButtons]bool
	Axes      [4]float32 // left X, left Y (down = +), right X, right Y (down = +)
}

// InputState turns keys and controllers into the core's button bits.
type InputState struct {
	toggles map[string]bool
	prev    map[string]bool
}

func NewInputState() *InputState {
	return &InputState{toggles: map[string]bool{}, prev: map[string]bool{}}
}

// Bits for one player (readInput in host.js). pad may be nil; keys count for
// player 0 only. analogStick: the core reads the stick itself. held: buttons
// (by bit) held some other way, such as the Android app's touch controls;
// they work toggles the same way.
func (s *InputState) Bits(player int, buttons []string, m ControlMap, keys map[string]bool, pad *PadState, analogStick bool, held uint32) uint32 {
	var bits uint32
	for i, name := range buttons {
		bd := m[name]
		on := i < 32 && held&(1<<i) != 0
		if pad != nil {
			for _, p := range bd.Pad {
				if p >= 0 && p < PadButtons && pad.Buttons[p] {
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
		if d, ok := Dirs[name]; ok && pad != nil {
			if pad.Buttons[d.Pad] {
				on = true
			}
			ax := pad.Axes[d.Axis]
			if !analogStick && ((d.Sign < 0 && ax < -0.5) || (d.Sign > 0 && ax > 0.5)) {
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
