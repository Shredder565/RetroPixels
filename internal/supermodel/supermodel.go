// Package supermodel plays Star Wars Trilogy Arcade in Supermodel, the Sega
// Model 3 arcade emulator (https://github.com/trzy/Supermodel, GPL-3.0). It
// finds Supermodel (the build that comes with RetroPixels, in a Supermodel
// folder next to it, made from supermodel/ in this repository), finds and
// checks the player's ROM set, writes Supermodel's settings for the game
// and builds its command line. RetroPixels only starts Supermodel; the
// changes made to Supermodel itself are in supermodel/Src.
package supermodel

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/internal/dolphin"
)

// PatchedMarker is a file next to supermodel.exe in RetroPixels' own build
// (supermodel/build.sh): that build keeps the player's files in
// SUPERMODEL_HOME, follows the docked window's size, and can record.
const PatchedMarker = "RetroPixels.txt"

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// FindExe returns Supermodel's program: RetroPixels' own build, which comes
// in the RetroPixels download (appDir\Supermodel\supermodel.exe), whenever
// it's there, so no separate Supermodel is needed and a Supermodel the
// player has elsewhere is never used instead. Only without it (RetroPixels
// run from outside its download): the one chosen by the player if it still
// exists, else the newest official download unpacked into Downloads
// (home\Downloads\supermodel*\supermodel.exe; on Linux supermodel, in the
// desktop's Downloads folder or ~/Downloads). "" if there is none.
func FindExe(chosen, appDir, home string) string {
	if p := OwnExe(appDir); p != "" {
		return p
	}
	if chosen != "" && isFile(chosen) {
		return chosen
	}
	if home == "" {
		return ""
	}
	var cands []string
	for _, dl := range dolphin.DownloadDirs(home) {
		for _, pat := range []string{filepath.Join(dl, "[Ss]upermodel*", ExeName), filepath.Join(dl, "[Ss]upermodel*", "[Ss]upermodel*", ExeName)} {
			m, _ := filepath.Glob(pat)
			cands = append(cands, m...)
		}
	}
	best, bestT := "", time.Time{}
	for _, p := range cands {
		if st, err := os.Stat(p); err == nil && !st.IsDir() && (best == "" || st.ModTime().After(bestT)) {
			best, bestT = p, st.ModTime()
		}
	}
	return best
}

// OwnExe is the Supermodel that comes with RetroPixels (appDir\Supermodel),
// or "" when it isn't there.
func OwnExe(appDir string) string {
	if appDir == "" {
		return ""
	}
	if p := filepath.Join(appDir, "Supermodel", ExeName); isFile(p) {
		return p
	}
	return ""
}

// Patched tells RetroPixels' own build of Supermodel from another one (such
// as the official download, which also works, with fewer extras).
func Patched(exe string) bool { return isFile(filepath.Join(filepath.Dir(exe), PatchedMarker)) }

// LaunchOptions: how the game is started.
type LaunchOptions struct {
	// Embed: a plain borderless window that RetroPixels docks into its own
	// window (and sizes; RetroPixels' build follows the size). Hidden also
	// starts it off-screen so it doesn't flash up before it's docked.
	Embed, Hidden bool
	// Fullscreen in Supermodel's own window (when not docked), at the
	// desktop's resolution in RetroPixels' build.
	Fullscreen bool
	// WideScreen: the 3D view fills a wide screen (the 2D parts stay 4:3).
	WideScreen bool
	// ForceFeedback runs the force feedback joystick board, which needs its
	// program ROM (Check.DriveBoard). The game then finds its stick ready
	// instead of waiting for it, and Xbox controllers rumble.
	ForceFeedback bool
	// RecordDir and FFmpeg (both or neither, RetroPixels' build only): the
	// game's picture goes to RecordDir\Frames\*.mkv (encoded by that ffmpeg)
	// and its sound to RecordDir\Audio\supermodel_dspdump.wav.
	RecordDir, FFmpeg string
	// Patched: RetroPixels' own build (Patched), which has the extra options.
	Patched bool
}

// FastBootSeconds: how much of the game's start-up RetroPixels' build runs
// at full speed, unseen and unheard (-rp-fast-boot).
const FastBootSeconds = 30

// GameArgs is Supermodel's command line for the game in rom.
func GameArgs(rom string, o LaunchOptions) []string {
	args := []string{rom}
	if o.Embed {
		args = append(args, "-window", "-borderless")
		if o.Hidden {
			args = append(args, "-window-pos=-20000,-20000")
		}
	} else if o.Fullscreen {
		args = append(args, "-fullscreen")
		if o.Patched {
			args = append(args, "-desktop-res")
		} else {
			args = append(args, "-res=1920,1080")
		}
	} else {
		args = append(args, "-window", "-res=992,768") // twice the Model 3's 496x384
	}
	if o.WideScreen {
		args = append(args, "-wide-screen")
	} else {
		args = append(args, "-no-wide-screen")
	}
	if o.ForceFeedback {
		args = append(args, "-force-feedback")
	}
	// Xbox controllers by default (InputSystem: XInput on Windows, SDL's
	// game controller support elsewhere; both number the buttons as INI does).
	args = append(args, "-input-system="+InputSystem)
	if o.Patched {
		// Star Wars' start-up (the feedback lever setup and the Sega logo)
		// runs at full speed behind a black screen: the game opens on its
		// attract mode, past the point where adding coins can crash it.
		args = append(args, fmt.Sprintf("-rp-fast-boot=%d", FastBootSeconds))
	}
	if o.Patched && o.RecordDir != "" && o.FFmpeg != "" {
		args = append(args, "-rp-record="+o.RecordDir, "-rp-ffmpeg="+o.FFmpeg)
	}
	return args
}

// Command makes the command that runs Supermodel from its own folder (where
// Games.xml is), with the player's files in home for RetroPixels' build.
// Supermodel is a console program: no console window is shown. What it
// prints is kept (Output) so a game that won't start can say why.
func Command(exe, home string, args ...string) (*exec.Cmd, *Output) {
	cmd := exec.Command(exe, shortArgs(args)...)
	cmd.Dir = filepath.Dir(exe)
	// SDL_WINDOWS_DPI_AWARENESS (an SDL hint, read from the environment):
	// Supermodel sees the screen's real pixels. Without it, on a scaled
	// Windows desktop (a 4K screen at 150% or 200%) fullscreen draws the
	// game in a small square in the top-left corner.
	cmd.Env = append(os.Environ(), "SUPERMODEL_HOME="+home, "SDL_WINDOWS_DPI_AWARENESS=permonitorv2")
	hideConsole(cmd)
	out := &Output{}
	cmd.Stdout = out
	cmd.Stderr = out
	return cmd, out
}

// shortArgs: Supermodel reads its command line in the Windows code page, so
// paths with other characters are passed in their short (8.3) form.
func shortArgs(args []string) []string {
	out := make([]string, len(args))
	for i, a := range args {
		if k := strings.IndexByte(a, '='); k > 0 && strings.HasPrefix(a, "-") {
			out[i] = a[:k+1] + shortPath(a[k+1:])
		} else if !strings.HasPrefix(a, "-") {
			out[i] = shortPath(a)
		} else {
			out[i] = a
		}
	}
	return out
}

// NVRAMFile is where Supermodel keeps the game's settings and high scores
// (in the player's folder, for RetroPixels' build).
func NVRAMFile(home, exe, set string) string {
	if Patched(exe) {
		return filepath.Join(home, "NVRAM", set+".nv")
	}
	return filepath.Join(filepath.Dir(exe), "NVRAM", set+".nv")
}

// ConfigFile is Supermodel.ini: in the player's folder for RetroPixels'
// build, in Supermodel's own Config folder otherwise.
func ConfigFile(home, exe string) string {
	if Patched(exe) {
		return filepath.Join(home, "Config", "Supermodel.ini")
	}
	return filepath.Join(filepath.Dir(exe), "Config", "Supermodel.ini")
}

// LogFile is Supermodel's own log.
func LogFile(home, exe string) string {
	if Patched(exe) {
		return filepath.Join(home, "Log", "Supermodel.log")
	}
	return filepath.Join(filepath.Dir(exe), "Supermodel.log")
}

// Output keeps the last few KB Supermodel printed.
type Output struct {
	mu  sync.Mutex
	buf []byte
}

const outputKeep = 16 << 10

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

// Problem turns what Supermodel printed (or wrote to its log) before it quit
// into a short reason for people ("" when there's nothing useful).
func Problem(out string) string {
	var lines []string
	for _, l := range strings.Split(strings.ReplaceAll(out, "\r", ""), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			lines = append(lines, l)
		}
	}
	has := func(s string) string {
		for _, l := range lines {
			if strings.Contains(strings.ToLower(l), s) {
				return l
			}
		}
		return ""
	}
	switch {
	case has("expected to find parent rom set") != "":
		return "The ROM set is split: Supermodel also needs swtrilgy.zip (the main version) in the same folder as swtrilgya.zip."
	case has("not found in") != "" || has("no complete model 3 games") != "":
		n := 0
		for _, l := range lines {
			if strings.Contains(strings.ToLower(l), "not found in") {
				n++
			}
		}
		msg := "The ROM set doesn't have every file Supermodel needs"
		if n > 0 {
			msg += " (" + strconv.Itoa(n) + " missing or different)"
		}
		return msg + ". It has to be the MAME ROM set swtrilgya.zip or swtrilgy.zip."
	case has("unable to create an opengl") != "" || has("opengl initialization failed") != "" || has("unable to create opengl context") != "":
		return "Supermodel needs OpenGL 4.1, which this PC's graphics driver didn't give it. Updating the graphics driver usually fixes this. (" + has("opengl") + ")"
	case has("audio with sdl") != "" || has("sdl audio") != "":
		return "Supermodel couldn't open the sound device: " + has("audio")
	case has("error:") != "":
		return has("error:")
	}
	if len(lines) > 3 {
		lines = lines[len(lines)-3:]
	}
	return strings.Join(lines, " ")
}
