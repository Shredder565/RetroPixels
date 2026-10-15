package dolphin

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ---------------- Dolphin's own folders ----------------

// UserDir is Dolphin's "User" folder: the portable one next to Dolphin.exe
// (when portable.txt is there), else Documents\Dolphin Emulator, else
// %APPDATA%\Dolphin Emulator. RETRO_DOLPHIN_USER overrides it (tests).
func UserDir(exe string) string {
	if t := os.Getenv("RETRO_DOLPHIN_USER"); t != "" {
		return t
	}
	if exe != "" {
		dir := filepath.Dir(exe)
		if _, err := os.Stat(filepath.Join(dir, "portable.txt")); err == nil {
			return filepath.Join(dir, "User")
		}
	}
	home, _ := os.UserHomeDir()
	docs := filepath.Join(home, "Documents", "Dolphin Emulator")
	if _, err := os.Stat(docs); err == nil {
		return docs
	}
	if ad := os.Getenv("APPDATA"); ad != "" {
		return filepath.Join(ad, "Dolphin Emulator")
	}
	return docs
}

// ConfigFile: one of Dolphin's settings files (Dolphin.ini, GFX.ini, ...).
func ConfigFile(user, name string) string { return filepath.Join(user, "Config", name) }

// DumpDir: where Dolphin saves frame and audio dumps (its "Dump" folder,
// unless a different one is chosen in Dolphin's Paths settings).
func DumpDir(user string) string {
	if p, ok := ReadINI(ConfigFile(user, "Dolphin.ini")).Get("General", "DumpPath"); ok && strings.TrimSpace(p) != "" {
		return filepath.Clean(strings.TrimSpace(p))
	}
	return filepath.Join(user, "Dump")
}

// ---------------- INI files ----------------

// INI is a tiny order-preserving INI editor (Dolphin's files are simple).
type INI struct{ lines []string }

// ReadINI reads a file; a missing one is empty.
func ReadINI(path string) *INI {
	b, _ := os.ReadFile(path)
	b = bytes.TrimPrefix(b, []byte("\xef\xbb\xbf"))
	s := strings.ReplaceAll(string(b), "\r\n", "\n")
	if s == "" {
		return &INI{}
	}
	return &INI{lines: strings.Split(strings.TrimRight(s, "\n"), "\n")}
}

func (f *INI) find(section, key string) (int, int) { // (line of key or -1, end of section or -1)
	in, end := false, -1
	for i, l := range f.lines {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			if in {
				return -1, end
			}
			in = strings.EqualFold(t[1:len(t)-1], section)
			if in {
				end = i + 1
			}
			continue
		}
		if in {
			if t != "" {
				end = i + 1
			}
			if k, _, ok := strings.Cut(t, "="); ok && strings.EqualFold(strings.TrimSpace(k), key) {
				return i, end
			}
		}
	}
	return -1, end
}

// Get returns a value and whether it's there.
func (f *INI) Get(section, key string) (string, bool) {
	if i, _ := f.find(section, key); i >= 0 {
		_, v, _ := strings.Cut(f.lines[i], "=")
		return strings.TrimSpace(v), true
	}
	return "", false
}

// Set changes a value, adding the key (and section) if needed.
func (f *INI) Set(section, key, val string) {
	line := key + " = " + val
	i, end := f.find(section, key)
	switch {
	case i >= 0:
		f.lines[i] = line
	case end >= 0:
		f.lines = append(f.lines[:end], append([]string{line}, f.lines[end:]...)...)
	default:
		if len(f.lines) > 0 && strings.TrimSpace(f.lines[len(f.lines)-1]) != "" {
			f.lines = append(f.lines, "")
		}
		f.lines = append(f.lines, "["+section+"]", line)
	}
}

// hasOriginalBackup: a first-time backup made under either name counts
// (RetroPixels.exe also accepts its old RetroConsole name).
func hasOriginalBackup(path string) bool {
	for _, suffix := range []string{".retropixels-original", ".retroconsole-original"} {
		if _, err := os.Stat(path + suffix); err == nil {
			return true
		}
	}
	return false
}

// Save writes the file (CRLF, as Dolphin writes it). The first time
// RetroPixels changes a file, the original is kept as
// <name>.retropixels-original, as RetroPixels.exe does.
func (f *INI) Save(path string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	if orig, err := os.ReadFile(path); err == nil && !hasOriginalBackup(path) {
		os.WriteFile(path+".retropixels-original", orig, 0o644)
	}
	tmp := path + ".retropixels-tmp"
	if err := os.WriteFile(tmp, []byte(strings.Join(f.lines, "\r\n")+"\r\n"), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// ---------------- Xbox controller set-up ----------------

// XInputDevice is the first Xbox controller as Dolphin names it.
const XInputDevice = "XInput/0/Gamepad"

// GCPadMap: GameCube controller in port 1 on an Xbox controller (RetroPixels.exe's layout).
var GCPadMap = [][2]string{
	{"Device", XInputDevice},
	{"Buttons/A", "`Button A`"}, {"Buttons/B", "`Button X`"}, {"Buttons/X", "`Button B`"}, {"Buttons/Y", "`Button Y`"},
	{"Buttons/Z", "`Shoulder R`"}, {"Buttons/Start", "Start"},
	{"Main Stick/Up", "`Left Y+`"}, {"Main Stick/Down", "`Left Y-`"}, {"Main Stick/Left", "`Left X-`"}, {"Main Stick/Right", "`Left X+`"},
	{"C-Stick/Up", "`Right Y+`"}, {"C-Stick/Down", "`Right Y-`"}, {"C-Stick/Left", "`Right X-`"}, {"C-Stick/Right", "`Right X+`"},
	{"Triggers/L", "`Trigger L`"}, {"Triggers/R", "`Trigger R`"}, {"Triggers/L-Analog", "`Trigger L`"}, {"Triggers/R-Analog", "`Trigger R`"},
	{"D-Pad/Up", "`Pad N`"}, {"D-Pad/Down", "`Pad S`"}, {"D-Pad/Left", "`Pad W`"}, {"D-Pad/Right", "`Pad E`"},
	{"Rumble/Motor", "`Motor L`|`Motor R`"},
}

// WiimoteMap: Wii Remote 1 with a Nunchuk on an Xbox controller.
var WiimoteMap = [][2]string{
	{"Device", XInputDevice}, {"Source", "1"}, {"Extension", "Nunchuk"},
	{"Buttons/A", "`Button A`"}, {"Buttons/B", "`Trigger R`"}, {"Buttons/1", "`Button X`"}, {"Buttons/2", "`Button Y`"},
	{"Buttons/-", "Back"}, {"Buttons/+", "Start"}, {"Buttons/Home", "`Thumb R`"},
	{"D-Pad/Up", "`Pad N`"}, {"D-Pad/Down", "`Pad S`"}, {"D-Pad/Left", "`Pad W`"}, {"D-Pad/Right", "`Pad E`"},
	{"IR/Up", "`Right Y+`"}, {"IR/Down", "`Right Y-`"}, {"IR/Left", "`Right X-`"}, {"IR/Right", "`Right X+`"},
	{"Shake/X", "`Shoulder R`"}, {"Shake/Y", "`Shoulder R`"}, {"Shake/Z", "`Shoulder R`"},
	{"Nunchuk/Buttons/C", "`Shoulder L`"}, {"Nunchuk/Buttons/Z", "`Trigger L`"},
	{"Nunchuk/Stick/Up", "`Left Y+`"}, {"Nunchuk/Stick/Down", "`Left Y-`"}, {"Nunchuk/Stick/Left", "`Left X-`"}, {"Nunchuk/Stick/Right", "`Left X+`"},
	{"Nunchuk/Shake/X", "`Thumb L`"}, {"Nunchuk/Shake/Y", "`Thumb L`"}, {"Nunchuk/Shake/Z", "`Thumb L`"},
	{"Rumble/Motor", "`Motor L`|`Motor R`"},
}

// PadLayout describes the layout for people, as the app's "Button layout" list.
const PadLayout = "GameCube: A = A, B = X, X = B, Y = Y, Z = RB, L / R = LT / RT, " +
	"C-stick = right stick, Start = Menu.\n" +
	"Wii Remote + Nunchuk: A = A, B = RT, 1 / 2 = X / Y, pointer = right stick, shake = RB, " +
	"Nunchuk C / Z = LB / LT, − / + / Home = View / Menu / R3."

// PadState: "xbox" (RetroPixels' set-up), "custom" (something else is
// mapped) or "none", for GCPadNew.ini / GCPad1 or WiimoteNew.ini / Wiimote1.
func PadState(user, file, section string) string {
	dev, ok := ReadINI(ConfigFile(user, file)).Get(section, "Device")
	if !ok {
		return "none"
	}
	if strings.EqualFold(dev, XInputDevice) {
		return "xbox"
	}
	return "custom"
}

// SetupXboxPads writes the Xbox controller layout into Dolphin's settings.
// Without force it only fills in controllers that aren't set up yet (what
// happens before each game).
func SetupXboxPads(user string, force bool) error {
	gc := ReadINI(ConfigFile(user, "GCPadNew.ini"))
	if force || PadState(user, "GCPadNew.ini", "GCPad1") == "none" {
		for _, kv := range GCPadMap {
			gc.Set("GCPad1", kv[0], kv[1])
		}
		if err := gc.Save(ConfigFile(user, "GCPadNew.ini")); err != nil {
			return err
		}
	}
	wm := ReadINI(ConfigFile(user, "WiimoteNew.ini"))
	if force || PadState(user, "WiimoteNew.ini", "Wiimote1") == "none" {
		for _, kv := range WiimoteMap {
			wm.Set("Wiimote1", kv[0], kv[1])
		}
		if err := wm.Save(ConfigFile(user, "WiimoteNew.ini")); err != nil {
			return err
		}
	}
	// Port 1 = standard GameCube controller.
	d := ReadINI(ConfigFile(user, "Dolphin.ini"))
	if _, ok := d.Get("Core", "SIDevice0"); !ok || force {
		d.Set("Core", "SIDevice0", "6")
		return d.Save(ConfigFile(user, "Dolphin.ini"))
	}
	return nil
}

// ---------------- Dolphin settings ----------------

// Option is one choice of a list setting.
type Option struct{ Value, Label string }

// Setting is one of the Dolphin settings RetroPixels offers (the app's
// "Dolphin settings" panel), stored in Dolphin's own files.
type Setting struct {
	File, Section, Key string
	ID, Label, Help    string
	Options            []Option // none: an on / off setting
	Default            string   // "True" / "False" for on / off
}

// Bool tells an on / off setting from a list.
func (s Setting) Bool() bool { return len(s.Options) == 0 }

func opts(pairs ...string) []Option {
	var o []Option
	for i := 0; i+1 < len(pairs); i += 2 {
		o = append(o, Option{pairs[i], pairs[i+1]})
	}
	return o
}

// Settings: the same list, in the same order, as RetroPixels.exe.
var Settings = []Setting{
	{"Dolphin.ini", "Core", "GFXBackend", "backend", "Graphics API", "",
		opts("Vulkan", "Vulkan (recommended)", "D3D12", "Direct3D 12", "D3D", "Direct3D 11", "OGL", "OpenGL"), "Vulkan"},
	{"GFX.ini", "Settings", "InternalResolution", "res", "Resolution", "Higher is sharper; 3× suits a 1080p screen, 6× a 4K one.",
		opts("1", "Native (480p)", "2", "2× (720p)", "3", "3× (1080p)", "4", "4× (1440p)", "5", "5×", "6", "6× (4K)", "8", "8× (5K)"), "1"},
	{"GFX.ini", "Settings", "AspectRatio", "aspect", "Aspect ratio", "",
		opts("0", "Auto (what the game uses)", "1", "Force 16:9", "2", "Force 4:3", "3", "Stretch to window"), "0"},
	{"GFX.ini", "Settings", "wideScreenHack", "widehack", "Widescreen hack", "Shows more of the scene in 16:9. Can cause glitches in some games.", nil, "False"},
	{"GFX.ini", "Hardware", "VSync", "vsync", "VSync", "", nil, "False"},
	{"GFX.ini", "Settings", "MSAA", "aa", "Anti-aliasing", "",
		opts("1", "Off", "2", "2× MSAA", "4", "4× MSAA", "8", "8× MSAA"), "1"},
	{"GFX.ini", "Enhancements", "MaxAnisotropy", "aniso", "Texture filtering", "",
		opts("0", "1× (default)", "1", "2×", "2", "4×", "3", "8×", "4", "16×"), "0"},
	{"GFX.ini", "Settings", "ShowFPS", "fps", "Show FPS counter", "", nil, "False"},
	{"Dolphin.ini", "Core", "EnableCheats", "cheats", "Allow cheats", "", nil, "False"},
	{"Dolphin.ini", "Interface", "ConfirmStop", "confirmstop", "Ask before closing a game", "", nil, "True"},
}

func settingByID(id string) *Setting {
	for i := range Settings {
		if Settings[i].ID == id {
			return &Settings[i]
		}
	}
	return nil
}

// ReadSettings returns each setting's value by ID, as Dolphin's files hold
// it (on / off as "True" / "False"); missing ones get Dolphin's default.
func ReadSettings(user string) map[string]string {
	files := map[string]*INI{}
	out := map[string]string{}
	for _, s := range Settings {
		if files[s.File] == nil {
			files[s.File] = ReadINI(ConfigFile(user, s.File))
		}
		v := s.Default
		if raw, ok := files[s.File].Get(s.Section, s.Key); ok {
			v = raw
			if s.Bool() {
				v = "False"
				if strings.EqualFold(raw, "true") {
					v = "True"
				}
			}
		}
		out[s.ID] = v
	}
	return out
}

// SaveSettings writes changed settings (by ID) into Dolphin's files. Dolphin
// must be closed: it writes its own settings when it closes.
func SaveSettings(user string, changes map[string]string) error {
	cur := ReadSettings(user)
	files := map[string]*INI{}
	for id, v := range changes {
		s := settingByID(id)
		if s == nil {
			return fmt.Errorf("unknown setting %q", id)
		}
		if s.Bool() {
			switch strings.ToLower(v) {
			case "true":
				v = "True"
			case "false":
				v = "False"
			default:
				return fmt.Errorf("%s must be on or off", s.Label)
			}
		} else {
			ok := v == cur[id]
			for _, o := range s.Options {
				ok = ok || o.Value == v
			}
			if !ok {
				return fmt.Errorf("%s: %v isn't a valid choice", s.Label, v)
			}
		}
		if files[s.File] == nil {
			files[s.File] = ReadINI(ConfigFile(user, s.File))
		}
		files[s.File].Set(s.Section, s.Key, v)
	}
	for name, f := range files {
		if err := f.Save(ConfigFile(user, name)); err != nil {
			return err
		}
	}
	return nil
}

// ErrNoDolphin: Dolphin hasn't been found or downloaded yet.
var ErrNoDolphin = errors.New("Dolphin isn't set up yet")
