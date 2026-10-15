package dolphin

import (
	"fmt"
	"os/exec"
	"path/filepath"
)

// LaunchOptions: how one game is started.
type LaunchOptions struct {
	// Embed: the game screen gets docked into the RetroPixels window, so it
	// must be a plain separate window (not fullscreen, not inside Dolphin's
	// own window), and Dolphin must keep reading the controller while
	// RetroPixels has the focus.
	Embed bool
	// Fullscreen: Dolphin's own fullscreen (only without Embed; when docked,
	// the RetroPixels window goes fullscreen instead).
	Fullscreen bool
	// Record: Dolphin saves the game's picture and sound for the recording.
	Record  bool
	Quality string // "high", "balanced" or "small"
}

// GameArgs is Dolphin's command line for one game, as RetroPixels.exe
// starts it. -C settings only apply to this run; Dolphin's own settings are
// untouched.
func GameArgs(path string, o LaunchOptions) []string {
	// Dolphin's own volume stays at full: the volume setting turns its
	// sound down in the Windows volume mixer instead, so the two don't stack.
	args := []string{"-b", "-e", path, "-C", "Dolphin.DSP.Volume=100"}
	if o.Embed {
		args = append(args,
			"-C", "Dolphin.Display.Fullscreen=False",
			"-C", "Dolphin.Display.RenderToMain=False",
			"-C", "Dolphin.Interface.ConfirmStop=False",
			"-C", "Dolphin.Interface.PauseOnFocusLost=False",
			"-C", "Dolphin.Input.BackgroundInput=True")
	} else if o.Fullscreen {
		args = append(args, "-C", "Dolphin.Display.Fullscreen=True")
	}
	if o.Record {
		args = append(args, RecordArgs(o.Quality)...)
	}
	return args
}

// dumpBitrate: Dolphin's raw video is only an in-between file, so it gets a
// high bitrate; the final, smaller .mp4 is made from it.
func dumpBitrate(quality string) int {
	switch quality {
	case "high":
		return 50000
	case "small":
		return 25000
	}
	return 35000
}

// RecordArgs: Dolphin settings for one run that make it save the game's
// picture and sound.
//
// The picture is saved as MPEG-4 in an .avi at a high bitrate: that's
// Dolphin's own default and is quick to encode, so it doesn't slow the game.
// It's saved at the size the game is shown on screen, which is exactly the
// game picture without the black bars around it.
func RecordArgs(quality string) []string {
	kv := []string{
		"Dolphin.Movie.DumpFrames=True",
		"Dolphin.Movie.DumpFramesSilent=True", // never stop to ask about overwriting a file
		"Dolphin.DSP.DumpAudio=True",
		"Dolphin.DSP.DumpAudioSilent=True",
		"Graphics.Settings.DumpFormat=avi",
		"Graphics.Settings.DumpCodec=mpeg4",
		"Graphics.Settings.DumpEncoder=",
		"Graphics.Settings.UseFFV1=False",
		"Graphics.Settings.DumpFramesAsImages=False",
		"Graphics.Settings.DumpPath=", // a fixed file name would overwrite itself when the picture size changes
		"Graphics.Settings.InternalResolutionFrameDumps=False",
		fmt.Sprintf("Graphics.Settings.BitrateKbps=%d", dumpBitrate(quality)),
	}
	var args []string
	for _, s := range kv {
		args = append(args, "-C", s)
	}
	return args
}

// Command makes the command that runs Dolphin; it starts in Dolphin's own
// folder (a portable Dolphin keeps its settings there).
func Command(exe string, args ...string) *exec.Cmd {
	cmd := exec.Command(exe, args...)
	cmd.Dir = filepath.Dir(exe)
	return cmd
}
