package dolphin

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// ---------------- the kept GameCube / Wii resolution ----------------
//
// The engine keeps its settings in PixelU\wii\User, next to its program, so
// a new RetroPixels download (a new folder) would start again at the native
// 480p. RetroPixels.exe and RetroPixels Native therefore keep the chosen
// resolution themselves, in %APPDATA%\RetroPixels (shared by both), and
// write it into the engine's GFX.ini before every game.

// DefaultResolution is 3× native (1920×1584, for a 1080p "1K" screen).
const DefaultResolution = "3"

// ResolutionOptions: the resolutions offered (the engine's InternalResolution).
var ResolutionOptions = opts("1", "Native (480p)", "2", "2× (720p)", "3", "3× (1K / 1080p)", "4", "4× (1440p)", "5", "5×", "6", "6× (4K)", "8", "8× (5K)")

// ResolutionHelp is shown under the setting.
const ResolutionHelp = "Kept for every game, also after updating RetroPixels. 3× (1K) suits a 1080p screen, 6× a 4K one."

// ResolutionFile: where the chosen resolution is kept. RETRO_GC_RES_FILE
// overrides it (tests).
func ResolutionFile() string {
	if p := os.Getenv("RETRO_GC_RES_FILE"); p != "" {
		return p
	}
	d, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(d, AppFolder, "gamecube-resolution.txt")
}

func validResolution(v string) bool {
	n, err := strconv.Atoi(v)
	return err == nil && n >= 1 && n <= 16
}

// KeptResolution: the resolution games play at. The one chosen in
// RetroPixels; before one is chosen, a higher one already set in the
// engine's GFX.ini; else DefaultResolution.
func KeptResolution(user string) string {
	if p := ResolutionFile(); p != "" {
		if b, err := os.ReadFile(p); err == nil {
			if v := strings.TrimSpace(string(b)); validResolution(v) {
				return v
			}
		}
	}
	if v, ok := ReadINI(ConfigFile(user, "GFX.ini")).Get("Settings", "InternalResolution"); ok && validResolution(v) && v != "1" {
		return v
	}
	return DefaultResolution
}

// KeepResolution remembers the chosen resolution for every later game,
// in this and later versions of RetroPixels.
func KeepResolution(v string) error {
	p := ResolutionFile()
	if p == "" || !validResolution(v) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	return os.WriteFile(p, []byte(v+"\r\n"), 0o644)
}

// ApplyResolution writes the kept resolution into the engine's GFX.ini (the
// engine must be closed), so the game starts at it.
func ApplyResolution(user string) error {
	v := KeptResolution(user)
	path := ConfigFile(user, "GFX.ini")
	f := ReadINI(path)
	if cur, ok := f.Get("Settings", "InternalResolution"); ok && cur == v {
		return nil
	}
	f.Set("Settings", "InternalResolution", v)
	return f.Save(path)
}
