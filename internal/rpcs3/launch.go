package rpcs3

// RPCS3's command line. What RetroPixels relies on (RPCS3 0.0.21 builds of
// early 2022 have them, as do later ones):
//
//	rpcs3.exe --no-gui [--fullscreen] <EBOOT.BIN>   the game alone, without RPCS3's game list
//	rpcs3.exe --installpkg <file.pkg>               RPCS3's own package installer
//	rpcs3.exe --installfw <PS3UPDAT.PUP>            RPCS3's own system software installer
//
// --fullscreen only applies with --no-gui. In --no-gui mode RPCS3 closes
// when its game window is closed.

import (
	"bufio"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// LaunchOptions: how one game is started.
type LaunchOptions struct {
	// Fullscreen in RPCS3's own window (ignored when Embed).
	Fullscreen bool
	// Embed: RPCS3's game window is docked into the RetroPixels window, so
	// it starts as a plain window.
	Embed bool
}

// GameArgs is RPCS3's command line for the game whose EBOOT.BIN is eboot.
func GameArgs(eboot string, o LaunchOptions) []string {
	args := []string{"--no-gui"}
	if o.Fullscreen && !o.Embed {
		args = append(args, "--fullscreen")
	}
	return append(args, eboot)
}

// InstallPkgArgs: RPCS3 installs a game package (.pkg) with its own installer.
func InstallPkgArgs(pkg string) []string { return []string{"--installpkg", pkg} }

// InstallFirmwareArgs: RPCS3 installs the PS3 system software (PS3UPDAT.PUP).
func InstallFirmwareArgs(pup string) []string { return []string{"--installfw", pup} }

// CheckPkg tells whether a file looks like a PS3 package (it starts with
// "\x7FPKG").
func CheckPkg(p string) error {
	f, err := os.Open(p)
	if err != nil {
		return err
	}
	defer f.Close()
	head := make([]byte, 4)
	if _, err := f.Read(head); err != nil || string(head) != "\x7fPKG" {
		return errors.New(filepath.Base(p) + " isn't a PS3 package (.pkg)")
	}
	return nil
}

// CheckPUP tells whether a file looks like PS3 system software (it starts
// with "SCEUF").
func CheckPUP(p string) error {
	f, err := os.Open(p)
	if err != nil {
		return err
	}
	defer f.Close()
	head := make([]byte, 5)
	if _, err := f.Read(head); err != nil || string(head) != "SCEUF" {
		return errors.New(filepath.Base(p) + " isn't PS3 system software (PS3UPDAT.PUP)")
	}
	return nil
}

// Command makes the command that runs RPCS3 from its own folder. What it
// prints is kept (Output) so a game that won't start can say why.
func Command(exe string, args ...string) (*exec.Cmd, *Output) {
	cmd := exec.Command(exe, args...)
	cmd.Dir = filepath.Dir(exe)
	hideConsole(cmd)
	out := &Output{}
	cmd.Stdout = out
	cmd.Stderr = out
	return cmd, out
}

// Output keeps the last few KB RPCS3 printed.
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

// LogFile is RPCS3's log (RPCS3.log next to the program on Windows).
func LogFile(exe string) string {
	return filepath.Join(filepath.Dir(exe), "RPCS3.log")
}

// LogErrors is the fatal and error lines RPCS3 wrote to its log at path
// since since (its lines start with ·F and ·E), the last 20 at most.
func LogErrors(path string, since time.Time) string {
	st, err := os.Stat(path)
	if err != nil || (!since.IsZero() && st.ModTime().Before(since)) {
		return ""
	}
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	var lines []string
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	for sc.Scan() {
		l := sc.Text()
		if strings.HasPrefix(l, "·F ") || strings.HasPrefix(l, "·E ") {
			lines = append(lines, l)
		}
	}
	if len(lines) > 20 {
		lines = lines[len(lines)-20:]
	}
	return strings.Join(lines, "\n")
}

// Problem turns what RPCS3 printed or logged before it quit into a short
// reason for people ("" when there's nothing useful).
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
	case has(".rap") != "" || has("license") != "" || has("licence") != "":
		return "RPCS3 couldn't unlock the game: its licence file is missing or doesn't match. " +
			"Use \"Add licence file…\" with your own .rap or .edat for this game. (" + firstOf(has(".rap"), has("license"), has("licence")) + ")"
	case has("firmware") != "" || has("dev_flash") != "":
		return FirmwareMessage
	case has("vulkan") != "" && (has("fail") != "" || has("error") != ""):
		return "RPCS3 couldn't start its Vulkan renderer. Updating the graphics driver usually fixes this, or choose OpenGL in RPCS3's settings."
	case has("unknown option") != "":
		return "This RPCS3 is too old for RetroPixels' way of starting games (" + has("unknown option") + "). A newer RPCS3 build works."
	}
	for i := len(lines) - 1; i >= 0; i-- {
		if strings.HasPrefix(lines[i], "·F ") {
			return lines[i]
		}
	}
	if len(lines) > 3 {
		lines = lines[len(lines)-3:]
	}
	return strings.Join(lines, " ")
}

func firstOf(s ...string) string {
	for _, v := range s {
		if v != "" {
			return v
		}
	}
	return ""
}
