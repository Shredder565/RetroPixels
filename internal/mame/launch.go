package mame

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

// LaunchOptions: how one game is started.
type LaunchOptions struct {
	// Embed: MAME's window gets docked into the RetroPixels window, so it
	// must be a plain window (not fullscreen or maximized), skip the info
	// screens, and take the keyboard from window messages (which reach a
	// docked window) rather than raw input (which goes to the window in front).
	Embed bool
}

// RomPath is MAME's ROM search path for a game: the game's own folder, your
// ROM folders, and MAME's own "roms" folder (for the distribution's MAME on
// Linux, the ROM folders its mame.ini names: ~/mame/roms and the shared ones).
func RomPath(exe, gameFolder string, folders []string) string {
	var parts []string
	seen := map[string]bool{}
	add := func(p string) {
		if p == "" {
			return
		}
		k := strings.ToLower(filepath.Clean(p))
		if !seen[k] {
			seen[k] = true
			parts = append(parts, p)
		}
	}
	add(gameFolder)
	for _, f := range folders {
		add(f)
	}
	add(filepath.Join(filepath.Dir(exe), "roms"))
	if isSystemExe(exe) {
		for _, d := range systemRomDirs() {
			add(d)
		}
	}
	return strings.Join(parts, ";")
}

// GameArgs is MAME's command line for one game. Options this MAME doesn't
// know (opts, from OptionsOf) are left out, so old versions start too; nil
// opts passes only the options every Windows MAME has had.
func GameArgs(short, rompath string, opts Options, o LaunchOptions) []string {
	args := []string{short, "-rompath", rompath}
	has := func(name string) bool { return opts != nil && opts[name] }
	if o.Embed {
		args = append(args, "-window")
		if has("maximize") {
			args = append(args, "-nomaximize")
		}
		if has("keyboardprovider") && embedKeyboard != "" {
			args = append(args, "-keyboardprovider", embedKeyboard)
		}
	}
	if has("skip_gameinfo") {
		args = append(args, "-skip_gameinfo")
	}
	if has("joystick") {
		args = append(args, "-joystick") // Xbox controllers
	}
	return args
}

// embedKeyboard is the keyboard provider a docked MAME uses: Windows'
// messages on Windows; on Linux MAME's own default (SDL, which gets the keys
// of the docked X11 window it has the focus of).
var embedKeyboard = func() string {
	if runtime.GOOS == "windows" {
		return "win32"
	}
	return ""
}()

// Command makes the command that runs MAME, in its own folder (where it keeps
// mame.ini, its saves and its settings; see WorkDir). MAME's messages are kept (see
// Output) so a game that won't start can say why.
func Command(exe string, args ...string) (*exec.Cmd, *Output) {
	cmd := exec.Command(exe, args...)
	cmd.Dir = WorkDir(exe)
	hideConsole(cmd)
	out := &Output{}
	cmd.Stdout = out
	cmd.Stderr = out
	return cmd, out
}

// Output keeps the last few KB MAME printed.
type Output struct {
	mu  sync.Mutex
	buf []byte
}

const outputKeep = 8 << 10

func (o *Output) Write(p []byte) (int, error) {
	o.mu.Lock()
	o.buf = append(o.buf, p...)
	if len(o.buf) > outputKeep {
		o.buf = append([]byte(nil), o.buf[len(o.buf)-outputKeep:]...)
	}
	o.mu.Unlock()
	return len(p), nil
}

func (o *Output) String() string {
	o.mu.Lock()
	defer o.mu.Unlock()
	return string(o.buf)
}

// Problem turns what MAME printed before it quit into a short reason for
// people ("" when there's nothing useful).
func Problem(out string) string {
	var lines []string
	for _, l := range strings.Split(strings.ReplaceAll(out, "\r", ""), "\n") {
		l = strings.TrimSpace(l)
		if l == "" {
			continue
		}
		lines = append(lines, l)
	}
	if len(lines) == 0 {
		return ""
	}
	// Missing files: MAME lists each one, then sums up.
	var missing []string
	for _, l := range lines {
		low := strings.ToLower(l)
		if strings.Contains(low, "not found") || strings.Contains(low, "incorrect checksum") || strings.Contains(low, "incorrect length") {
			missing = append(missing, l)
		}
	}
	for _, l := range lines {
		low := strings.ToLower(l)
		switch {
		case strings.Contains(low, "unknown system"), strings.Contains(low, "unknown driver"), strings.Contains(low, "unknown game"):
			return "This MAME doesn't know that game (" + l + "). It may need a newer or older MAME, or the file may be named differently."
		case strings.Contains(low, "required files are missing"):
			msg := "Some of the game's ROM files are missing or don't match this MAME version"
			if len(missing) > 0 {
				shown := missing
				if len(shown) > 4 {
					shown = shown[:4]
				}
				msg += ": " + strings.Join(shown, "; ")
				if len(missing) > 4 {
					msg += " …"
				}
			}
			return msg + ". ROM sets have to match the MAME version: try another version for this game."
		case strings.Contains(low, "unknown option"), strings.Contains(low, "error:"):
			return l
		}
	}
	if len(lines) > 3 {
		lines = lines[len(lines)-3:]
	}
	return strings.Join(lines, " ")
}
