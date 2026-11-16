package dolphin

// Button mapping for the GameCube controllers and Wii Remotes of PixelU's
// Wii / GameCube engine, kept in its GCPadNew.ini ([GCPad1]..[GCPad4]) and
// WiimoteNew.ini ([Wiimote1]..[Wiimote4]), plus what is plugged into each
// port (Dolphin.ini's [Core] SIDevice0..3 for GameCube ports, WiimoteNew.ini's
// Source and Extension for Wii Remotes). RetroPixels-Native's "Map
// controller buttons" window reads and writes these through PadMap.
//
// Each control's value is an input expression the engine understands: a
// name of an input on the controller's Device (`Button A`, `Left X+`, SPACE),
// or several joined with | (either one) or & (both).

import (
	"fmt"
	"strings"
)

// Pad kinds.
const (
	KindGC      = "gc"
	KindWiimote = "wiimote"
)

// PadControl is one mappable control: its key in the controller's section
// ("Buttons/A", "Nunchuk/Stick/Up") and its name for people.
type PadControl struct{ Key, Label string }

func controls(prefix, group string, names ...string) []PadControl {
	var out []PadControl
	for _, n := range names {
		lbl := group + " " + strings.ToLower(n)
		switch {
		case group == "":
			lbl = n
		case len(n) <= 2 && n != "Up" || n == "Home" || n == "Start" || n == "L-Analog" || n == "R-Analog":
			lbl = group + " " + n
		}
		out = append(out, PadControl{prefix + n, lbl})
	}
	return out
}

func join(lists ...[]PadControl) []PadControl {
	var out []PadControl
	for _, l := range lists {
		out = append(out, l...)
	}
	return out
}

var dirs = []string{"Up", "Down", "Left", "Right"}

// GCControls: a GameCube controller.
var GCControls = join(
	controls("Buttons/", "", "A", "B", "X", "Y", "Z", "Start"),
	controls("Main Stick/", "Control stick", dirs...),
	controls("C-Stick/", "C-stick", dirs...),
	controls("Triggers/", "", "L", "R"),
	[]PadControl{{"Triggers/L-Analog", "L (analog)"}, {"Triggers/R-Analog", "R (analog)"}},
	controls("D-Pad/", "D-pad", dirs...),
	[]PadControl{{"Rumble/Motor", "Rumble"}},
)

// WiimoteControls: the Wii Remote itself.
var WiimoteControls = join(
	controls("Buttons/", "", "A", "B", "1", "2"),
	[]PadControl{{"Buttons/-", "Minus (−)"}, {"Buttons/+", "Plus (+)"}, {"Buttons/Home", "Home"}},
	controls("D-Pad/", "D-pad", dirs...),
	controls("IR/", "Pointer", dirs...),
	controls("Shake/", "Shake", "X", "Y", "Z"),
	controls("Tilt/", "Tilt", "Forward", "Backward", "Left", "Right"),
	controls("Swing/", "Swing", "Up", "Down", "Left", "Right", "Forward", "Backward"),
	[]PadControl{{"Rumble/Motor", "Rumble"}},
)

// NunchukControls: the Nunchuk, when it's the Wii Remote's extension.
var NunchukControls = join(
	controls("Nunchuk/Buttons/", "Nunchuk", "C", "Z"),
	controls("Nunchuk/Stick/", "Nunchuk stick", dirs...),
	controls("Nunchuk/Shake/", "Nunchuk shake", "X", "Y", "Z"),
	controls("Nunchuk/Tilt/", "Nunchuk tilt", "Forward", "Backward", "Left", "Right"),
	controls("Nunchuk/Swing/", "Nunchuk swing", "Up", "Down", "Left", "Right", "Forward", "Backward"),
)

// ClassicControls: the Classic Controller, when it's the extension.
var ClassicControls = join(
	controls("Classic/Buttons/", "Classic", "A", "B", "X", "Y", "ZL", "ZR", "-", "+", "Home"),
	controls("Classic/Left Stick/", "Classic left stick", dirs...),
	controls("Classic/Right Stick/", "Classic right stick", dirs...),
	controls("Classic/Triggers/", "Classic", "L", "R"),
	[]PadControl{{"Classic/Triggers/L-Analog", "Classic L (analog)"}, {"Classic/Triggers/R-Analog", "Classic R (analog)"}},
	controls("Classic/D-Pad/", "Classic d-pad", dirs...),
)

// Extensions a Wii Remote can have here, as WiimoteNew.ini names them.
var Extensions = []Option{{"None", "Nothing"}, {"Nunchuk", "Nunchuk"}, {"Classic", "Classic Controller"}}

// GCPortDevices: what can be in a GameCube port (Dolphin.ini's SIDevice).
var GCPortDevices = []Option{{"6", "Standard controller"}, {"0", "Nothing"}, {"12", "GameCube adapter for Wii U"}}

// WiimoteSources: a Wii Remote slot's Source.
var WiimoteSources = []Option{{"1", "Emulated Wii Remote"}, {"0", "Nothing"}}

// Input devices the window can listen to.
const KeyboardDevice = "DInput/0/Keyboard Mouse"

// XInputPad is the n-th (0..3) Xbox controller's device name.
func XInputPad(n int) string { return fmt.Sprintf("XInput/%d/Gamepad", n) }

// IsXInput: an Xbox controller device; n is its number.
func IsXInput(dev string) (n int, ok bool) {
	var k int
	if _, err := fmt.Sscanf(dev, "XInput/%d/Gamepad", &k); err == nil && XInputPad(k) == dev {
		return k, true
	}
	return 0, false
}

// DeviceLabel: a device for people.
func DeviceLabel(dev string) string {
	if n, ok := IsXInput(dev); ok {
		return fmt.Sprintf("Xbox controller %d", n+1)
	}
	switch dev {
	case KeyboardDevice:
		return "Keyboard and mouse"
	case "":
		return "No device"
	}
	return dev
}

// OtherDeviceInput: an input on another device than the controller's
// Device ("`DInput/0/Keyboard Mouse:SPACE`").
func OtherDeviceInput(dev, name string) string { return "`" + dev + ":" + name + "`" }

// QuoteInput writes an input's name the way the engine reads it: in
// backticks unless it's a plain word.
func QuoteInput(name string) string {
	for _, r := range name {
		if !(r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '_') {
			return "`" + name + "`"
		}
	}
	return name
}

// unquote: the input's name if expr is one input alone, else "".
func unquote(expr string) string {
	e := strings.TrimSpace(expr)
	if len(e) >= 2 && e[0] == '`' && e[len(e)-1] == '`' && !strings.Contains(e[1:len(e)-1], "`") {
		return e[1 : len(e)-1]
	}
	for _, r := range e {
		if !(r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '_') {
			return ""
		}
	}
	return e
}

var xinputNames = map[string]string{
	"Button A": "A", "Button B": "B", "Button X": "X", "Button Y": "Y",
	"Pad N": "D-pad up", "Pad S": "D-pad down", "Pad W": "D-pad left", "Pad E": "D-pad right",
	"Start": "Menu", "Back": "View", "Guide": "Guide",
	"Shoulder L": "LB", "Shoulder R": "RB", "Thumb L": "L3 (press left stick)", "Thumb R": "R3 (press right stick)",
	"Trigger L": "LT", "Trigger R": "RT",
	"Left X-": "Left stick left", "Left X+": "Left stick right", "Left Y+": "Left stick up", "Left Y-": "Left stick down",
	"Right X-": "Right stick left", "Right X+": "Right stick right", "Right Y+": "Right stick up", "Right Y-": "Right stick down",
	"Motor L": "Left motor", "Motor R": "Right motor",
}

var keyNames = map[string]string{
	"Click 0": "Left mouse button", "Click 1": "Right mouse button", "Click 2": "Middle mouse button",
	"Cursor X-": "Mouse left", "Cursor X+": "Mouse right", "Cursor Y-": "Mouse up", "Cursor Y+": "Mouse down",
	"RETURN": "Enter", "SPACE": "Space", "BACK": "Backspace", "ESCAPE": "Esc", "TAB": "Tab", "CAPITAL": "Caps Lock",
	"LSHIFT": "Left Shift", "RSHIFT": "Right Shift", "LCONTROL": "Left Ctrl", "RCONTROL": "Right Ctrl",
	"LMENU": "Left Alt", "RMENU": "Right Alt", "UP": "Up arrow", "DOWN": "Down arrow", "LEFT": "Left arrow", "RIGHT": "Right arrow",
	"PRIOR": "Page Up", "NEXT": "Page Down", "HOME": "Home", "END": "End", "INSERT": "Insert", "DELETE": "Delete",
	"NUMPADENTER": "Numpad Enter", "ADD": "Numpad +", "SUBTRACT": "Numpad -", "MULTIPLY": "Numpad *", "DIVIDE": "Numpad /",
	"DECIMAL": "Numpad .", "MINUS": "-", "EQUALS": "=", "LBRACKET": "[", "RBRACKET": "]", "SEMICOLON": ";",
	"APOSTROPHE": "'", "GRAVE": "`", "BACKSLASH": "\\", "COMMA": ",", "PERIOD": ".", "SLASH": "/",
}

// DescribeInput: a control's expression for people, on device dev ("" =
// not mapped). Anything more than one input is shown as it is.
func DescribeInput(dev, expr string) string {
	e := strings.TrimSpace(expr)
	if e == "" {
		return ""
	}
	if n := unquote(e); strings.Contains(n, ":") {
		// another device's input: `XInput/1/Gamepad:Button A`
		other, in, _ := strings.Cut(n, ":")
		if d := DescribeInput(other, QuoteInput(in)); d != QuoteInput(in) || other == KeyboardDevice {
			return d + " (" + DeviceLabel(other) + ")"
		}
		return e
	}
	if _, ok := IsXInput(dev); ok {
		if e == "`Motor L`|`Motor R`" || e == "`Motor R`|`Motor L`" {
			return "Both motors"
		}
		if n, ok := xinputNames[unquote(e)]; ok {
			return n
		}
	}
	if dev == KeyboardDevice {
		n := unquote(e)
		if l, ok := keyNames[n]; ok {
			return l
		}
		if strings.HasPrefix(n, "NUMPAD") && len(n) == 7 {
			return "Numpad " + n[6:]
		}
		if n != "" {
			return n
		}
	}
	return e
}

// PadMap is one controller's mapping, as the files hold it.
type PadMap struct {
	Kind    string            // KindGC or KindWiimote
	Port    int               // 0..3
	Device  string            // the input device ("XInput/0/Gamepad", ...)
	Plugged string            // GC: SIDevice value; Wii Remote: Source
	Ext     string            // Wii Remote: Extension
	Values  map[string]string // by PadControl.Key
}

// Section: the controller's section in its file.
func (p *PadMap) Section() string {
	if p.Kind == KindGC {
		return fmt.Sprintf("GCPad%d", p.Port+1)
	}
	return fmt.Sprintf("Wiimote%d", p.Port+1)
}

// File: GCPadNew.ini or WiimoteNew.ini.
func (p *PadMap) File() string {
	if p.Kind == KindGC {
		return "GCPadNew.ini"
	}
	return "WiimoteNew.ini"
}

// Title: "GameCube controller 1", "Wii Remote 2".
func (p *PadMap) Title() string {
	if p.Kind == KindGC {
		return fmt.Sprintf("GameCube controller %d", p.Port+1)
	}
	return fmt.Sprintf("Wii Remote %d", p.Port+1)
}

// AllControls: every control this kind of controller has (the Wii Remote's
// with both extensions').
func (p *PadMap) AllControls() []PadControl {
	if p.Kind == KindGC {
		return GCControls
	}
	return join(WiimoteControls, NunchukControls, ClassicControls)
}

// Controls: the ones that count now (the Wii Remote's and its extension's).
func (p *PadMap) Controls() []PadControl {
	if p.Kind == KindGC {
		return GCControls
	}
	switch p.Ext {
	case "Nunchuk":
		return join(WiimoteControls, NunchukControls)
	case "Classic":
		return join(WiimoteControls, ClassicControls)
	}
	return WiimoteControls
}

// ReadPadMap reads controller port (0..3) of kind from the engine's files
// in user. A controller that was never set up has no Device; Plugged and Ext
// then take the engine's defaults.
func ReadPadMap(user, kind string, port int) *PadMap {
	p := &PadMap{Kind: kind, Port: port, Values: map[string]string{}}
	f := ReadINI(ConfigFile(user, p.File()))
	sec := p.Section()
	p.Device, _ = f.Get(sec, "Device")
	for _, c := range p.AllControls() {
		if v, ok := f.Get(sec, c.Key); ok {
			p.Values[c.Key] = v
		}
	}
	if kind == KindGC {
		p.Plugged = "0"
		if port == 0 {
			p.Plugged = "6" // the engine's default: a controller in port 1 only
		}
		if v, ok := ReadINI(ConfigFile(user, "Dolphin.ini")).Get("Core", fmt.Sprintf("SIDevice%d", port)); ok {
			p.Plugged = v
		}
		return p
	}
	p.Plugged = "0"
	if port == 0 {
		p.Plugged = "1"
	}
	if v, ok := f.Get(sec, "Source"); ok {
		p.Plugged = v
	}
	p.Ext = "None"
	if v, ok := f.Get(sec, "Extension"); ok && v != "" {
		p.Ext = v
	}
	return p
}

// Clone copies a mapping (the window edits copies until Save).
func (p *PadMap) Clone() *PadMap {
	c := *p
	c.Values = make(map[string]string, len(p.Values))
	for k, v := range p.Values {
		c.Values[k] = v
	}
	return &c
}

// Equal: nothing to save.
func (p *PadMap) Equal(o *PadMap) bool {
	if p.Device != o.Device || p.Plugged != o.Plugged || p.Ext != o.Ext {
		return false
	}
	for _, c := range p.AllControls() {
		if strings.TrimSpace(p.Values[c.Key]) != strings.TrimSpace(o.Values[c.Key]) {
			return false
		}
	}
	return true
}

// SavePadMaps writes the mappings into the engine's files (the engine must
// be closed: it writes its own settings when it closes). Only the keys the
// window knows are touched; the rest (dead zones, calibration...) stays.
func SavePadMaps(user string, maps []*PadMap) error {
	files := map[string]*INI{}
	get := func(name string) *INI {
		if files[name] == nil {
			files[name] = ReadINI(ConfigFile(user, name))
		}
		return files[name]
	}
	for _, p := range maps {
		f, sec := get(p.File()), p.Section()
		f.Set(sec, "Device", p.Device)
		for _, c := range p.AllControls() {
			v, ok := p.Values[c.Key]
			if _, had := f.Get(sec, c.Key); !ok && !had {
				continue
			}
			f.Set(sec, c.Key, strings.TrimSpace(v))
		}
		if p.Kind == KindGC {
			get("Dolphin.ini").Set("Core", fmt.Sprintf("SIDevice%d", p.Port), p.Plugged)
		} else {
			f.Set(sec, "Source", p.Plugged)
			f.Set(sec, "Extension", p.Ext)
		}
	}
	for _, name := range []string{"GCPadNew.ini", "WiimoteNew.ini", "Dolphin.ini"} {
		if f := files[name]; f != nil {
			if err := f.Save(ConfigFile(user, name)); err != nil {
				return err
			}
		}
	}
	return nil
}

// ---------------- default layouts ----------------

// GCKeyboardMap: a GameCube controller on the keyboard.
var GCKeyboardMap = [][2]string{
	{"Buttons/A", "X"}, {"Buttons/B", "Z"}, {"Buttons/X", "C"}, {"Buttons/Y", "S"}, {"Buttons/Z", "D"}, {"Buttons/Start", "RETURN"},
	{"Main Stick/Up", "UP"}, {"Main Stick/Down", "DOWN"}, {"Main Stick/Left", "LEFT"}, {"Main Stick/Right", "RIGHT"},
	{"C-Stick/Up", "I"}, {"C-Stick/Down", "K"}, {"C-Stick/Left", "J"}, {"C-Stick/Right", "L"},
	{"Triggers/L", "Q"}, {"Triggers/R", "W"}, {"Triggers/L-Analog", ""}, {"Triggers/R-Analog", ""},
	{"D-Pad/Up", "T"}, {"D-Pad/Down", "G"}, {"D-Pad/Left", "F"}, {"D-Pad/Right", "H"},
	{"Rumble/Motor", ""},
}

// WiimoteKeyboardMap: a Wii Remote (with Nunchuk and Classic Controller
// keys) on the keyboard and mouse: the mouse points.
var WiimoteKeyboardMap = [][2]string{
	{"Buttons/A", "`Click 0`"}, {"Buttons/B", "`Click 1`"}, {"Buttons/1", "1"}, {"Buttons/2", "2"},
	{"Buttons/-", "Q"}, {"Buttons/+", "E"}, {"Buttons/Home", "RETURN"},
	{"D-Pad/Up", "UP"}, {"D-Pad/Down", "DOWN"}, {"D-Pad/Left", "LEFT"}, {"D-Pad/Right", "RIGHT"},
	{"IR/Up", "`Cursor Y-`"}, {"IR/Down", "`Cursor Y+`"}, {"IR/Left", "`Cursor X-`"}, {"IR/Right", "`Cursor X+`"},
	{"Shake/X", "`Click 2`"}, {"Shake/Y", "`Click 2`"}, {"Shake/Z", "`Click 2`"},
	{"Tilt/Forward", "I"}, {"Tilt/Backward", "K"}, {"Tilt/Left", "J"}, {"Tilt/Right", "L"},
	{"Nunchuk/Buttons/C", "LCONTROL"}, {"Nunchuk/Buttons/Z", "LSHIFT"},
	{"Nunchuk/Stick/Up", "W"}, {"Nunchuk/Stick/Down", "S"}, {"Nunchuk/Stick/Left", "A"}, {"Nunchuk/Stick/Right", "D"},
	{"Nunchuk/Shake/X", "F"}, {"Nunchuk/Shake/Y", "F"}, {"Nunchuk/Shake/Z", "F"},
	{"Classic/Buttons/A", "X"}, {"Classic/Buttons/B", "Z"}, {"Classic/Buttons/X", "C"}, {"Classic/Buttons/Y", "S"},
	{"Classic/Buttons/ZL", "Q"}, {"Classic/Buttons/ZR", "E"}, {"Classic/Buttons/-", "MINUS"}, {"Classic/Buttons/+", "EQUALS"},
	{"Classic/Buttons/Home", "RETURN"},
	{"Classic/Left Stick/Up", "W"}, {"Classic/Left Stick/Down", "S"}, {"Classic/Left Stick/Left", "A"}, {"Classic/Left Stick/Right", "D"},
	{"Classic/Right Stick/Up", "I"}, {"Classic/Right Stick/Down", "K"}, {"Classic/Right Stick/Left", "J"}, {"Classic/Right Stick/Right", "L"},
	{"Classic/Triggers/L", "1"}, {"Classic/Triggers/R", "2"},
	{"Classic/D-Pad/Up", "UP"}, {"Classic/D-Pad/Down", "DOWN"}, {"Classic/D-Pad/Left", "LEFT"}, {"Classic/D-Pad/Right", "RIGHT"},
}

// WiimoteClassicXboxMap: the Classic Controller on an Xbox controller
// (laid out as the Classic Controller Pro).
var WiimoteClassicXboxMap = [][2]string{
	{"Classic/Buttons/A", "`Button B`"}, {"Classic/Buttons/B", "`Button A`"}, {"Classic/Buttons/X", "`Button Y`"}, {"Classic/Buttons/Y", "`Button X`"},
	{"Classic/Buttons/ZL", "`Trigger L`"}, {"Classic/Buttons/ZR", "`Trigger R`"},
	{"Classic/Buttons/-", "Back"}, {"Classic/Buttons/+", "Start"}, {"Classic/Buttons/Home", "Guide"},
	{"Classic/Left Stick/Up", "`Left Y+`"}, {"Classic/Left Stick/Down", "`Left Y-`"}, {"Classic/Left Stick/Left", "`Left X-`"}, {"Classic/Left Stick/Right", "`Left X+`"},
	{"Classic/Right Stick/Up", "`Right Y+`"}, {"Classic/Right Stick/Down", "`Right Y-`"}, {"Classic/Right Stick/Left", "`Right X-`"}, {"Classic/Right Stick/Right", "`Right X+`"},
	{"Classic/Triggers/L", "`Shoulder L`"}, {"Classic/Triggers/R", "`Shoulder R`"},
	{"Classic/D-Pad/Up", "`Pad N`"}, {"Classic/D-Pad/Down", "`Pad S`"}, {"Classic/D-Pad/Left", "`Pad W`"}, {"Classic/D-Pad/Right", "`Pad E`"},
}

// Defaults fills in the default layout for the mapping's device: the Xbox
// layout (GCPadMap, WiimoteMap) on an Xbox controller, the keyboard one on
// the keyboard. Other devices only lose their mappings. Device, Plugged and
// Ext stay.
func (p *PadMap) Defaults() {
	p.Values = map[string]string{}
	for _, c := range p.AllControls() {
		p.Values[c.Key] = ""
	}
	var layouts [][][2]string
	_, xbox := IsXInput(p.Device)
	switch {
	case xbox && p.Kind == KindGC:
		layouts = [][][2]string{GCPadMap}
	case xbox:
		layouts = [][][2]string{WiimoteMap, WiimoteClassicXboxMap}
	case p.Device == KeyboardDevice && p.Kind == KindGC:
		layouts = [][][2]string{GCKeyboardMap}
	case p.Device == KeyboardDevice:
		layouts = [][][2]string{WiimoteKeyboardMap}
	}
	for _, l := range layouts {
		for _, kv := range l {
			if _, ok := p.Values[kv[0]]; ok {
				p.Values[kv[0]] = kv[1]
			}
		}
	}
}

// Clear unmaps every control.
func (p *PadMap) Clear() {
	for _, c := range p.AllControls() {
		p.Values[c.Key] = ""
	}
}
