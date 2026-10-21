package main

// The Linux side of platform_windows.go: the app window (Chrome, Chromium,
// Edge or Brave in app mode, else the default browser), file and folder
// dialogs (zenity or kdialog), opening folders (xdg-open) and the running
// programs (/proc). (.7z files are unpacked in internal/dolphin.)

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"retropixels/internal/dolphin"
)

// dataDir is where RetroPixels keeps what Windows keeps in
// %LOCALAPPDATA%\RetroPixels: $XDG_DATA_HOME/RetroPixels, else
// ~/.local/share/RetroPixels.
func dataDir() string {
	if d := os.Getenv("XDG_DATA_HOME"); filepath.IsAbs(d) {
		return filepath.Join(d, appName)
	}
	if home, err := os.UserHomeDir(); err == nil {
		return filepath.Join(home, ".local", "share", appName)
	}
	return filepath.Join(configDir(), "data")
}

func showError(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	switch dialogTool() {
	case "zenity":
		_ = exec.Command("zenity", "--error", "--no-wrap", "--title="+appName, "--text="+msg).Run()
	case "kdialog":
		_ = exec.Command("kdialog", "--title", appName, "--error", msg).Run()
	}
}

// browsers are the Chromium-based browsers that can open a page as an app
// window (--app), by their program names on PATH.
var browsers = []string{
	"google-chrome", "google-chrome-stable", "chromium", "chromium-browser",
	"microsoft-edge", "microsoft-edge-stable", "brave-browser", "brave",
}

func findBrowser() string {
	if b := os.Getenv("RETROPIXELS_BROWSER"); b != "" {
		if p, err := exec.LookPath(b); err == nil {
			return p
		}
	}
	for _, b := range browsers {
		if p, err := exec.LookPath(b); err == nil {
			return p
		}
	}
	return ""
}

// openAppWindow opens the page in its own app window. When wait is true it
// blocks until the window has closed and the page has stopped checking in.
func openAppWindow(url string, wait bool) error {
	browser := findBrowser()
	if browser == "" {
		// No Chromium-based browser: the default browser, and keep running
		// until the page has closed (it checks in while it's open).
		if err := exec.Command("xdg-open", url).Start(); err != nil {
			fmt.Println(appName, "is at", url)
		}
		if wait {
			waitForPageToClose()
		}
		return nil
	}

	profile := filepath.Join(dataDir(), "BrowserProfile")
	_ = os.MkdirAll(profile, 0o755)
	cmd := exec.Command(browser, appWindowArgs(url, profile)...)
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("couldn't open the %s window: %v", appName, err)
	}
	if !wait {
		go cmd.Wait()
		return nil
	}
	_ = cmd.Wait()
	waitForPageToClose()
	return nil
}

// appWindowArgs is the browser's command line for the app window.
func appWindowArgs(url, profile string) []string {
	return append(browserPlatformArgs(),
		"--app="+url,
		"--user-data-dir="+profile,
		"--class="+appName, // the window's WM_CLASS, so the desktop file's icon is used; embed_linux.go finds the window by it
		"--no-first-run",
		"--no-default-browser-check",
		"--window-size=1280,900",
		"--autoplay-policy=no-user-gesture-required",
		"--disable-features=Translate",
	)
}

// On a Wayland desktop the app window and the engines run under XWayland
// (the desktop's X server), so engine windows can be docked into the app
// window: Wayland has no way to put one program's window inside another's.
// RETROPIXELS_WAYLAND=native leaves every program on Wayland (no docking).
func useXWayland() bool {
	return os.Getenv("WAYLAND_DISPLAY") != "" && os.Getenv("DISPLAY") != "" &&
		!strings.EqualFold(os.Getenv("RETROPIXELS_WAYLAND"), "native")
}

// xwaylandEnv makes the engines' toolkits pick X11: Qt (PixelU-Wii), GTK
// (PixelU's wxWidgets UI, zenity), SDL 2 (Supermodel, MAME) and SDL 3.
var xwaylandEnv = [][2]string{
	{"QT_QPA_PLATFORM", "xcb"},
	{"GDK_BACKEND", "x11"},
	{"SDL_VIDEODRIVER", "x11"},
	{"SDL_VIDEO_DRIVER", "x11"},
}

func init() {
	if useXWayland() {
		// Every program RetroPixels starts inherits these.
		for _, kv := range xwaylandEnv {
			_ = os.Setenv(kv[0], kv[1])
		}
	}
}

// browserPlatformArgs: Chrome and the others pick Wayland by themselves on
// a Wayland desktop; under XWayland the app window is an X11 window the
// engines can be docked into.
func browserPlatformArgs() []string {
	if useXWayland() {
		return []string{"--ozone-platform=x11"}
	}
	return nil
}

// dialogTool is the dialog program that's installed: zenity (GNOME and most
// others) or kdialog (KDE), "" when neither is.
func dialogTool() string {
	if os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "" {
		return ""
	}
	order := []string{"zenity", "kdialog"}
	if strings.Contains(strings.ToUpper(os.Getenv("XDG_CURRENT_DESKTOP")), "KDE") {
		order = []string{"kdialog", "zenity"}
	}
	for _, t := range order {
		if _, err := exec.LookPath(t); err == nil {
			return t
		}
	}
	return ""
}

var errNoDialog = errors.New("there's no file dialog on this system: install zenity (or kdialog on KDE), then try again")

// winFilter splits a Windows dialog filter ("Name|*.zip;*.7z|Other|*.exe")
// into its names and their patterns.
func winFilter(filter string) (names []string, patterns [][]string) {
	parts := strings.Split(filter, "|")
	for i := 0; i+1 < len(parts); i += 2 {
		var ps []string
		for _, p := range strings.Split(parts[i+1], ";") {
			if p = strings.TrimSpace(p); p != "" {
				ps = append(ps, p)
			}
		}
		if len(ps) > 0 {
			names = append(names, strings.TrimSpace(parts[i]))
			patterns = append(patterns, ps)
		}
	}
	return names, patterns
}

// pickArgs is the command line that shows an "Open" dialog with tool.
// Every dialog also offers all files, since Linux programs have no .exe.
func pickArgs(tool, title, filter, startDir string) []string {
	names, patterns := winFilter(filter)
	start := ""
	if st, err := os.Stat(startDir); startDir != "" && err == nil && st.IsDir() {
		start = startDir
	}
	switch tool {
	case "zenity":
		args := []string{"--file-selection", "--title=" + title}
		if start != "" {
			args = append(args, "--filename="+strings.TrimSuffix(start, "/")+"/")
		}
		for i, n := range names {
			args = append(args, "--file-filter="+n+" | "+strings.Join(patterns[i], " "))
		}
		return append(args, "--file-filter=All files | *")
	case "kdialog":
		var fs []string
		for i, n := range names {
			fs = append(fs, strings.Join(patterns[i], " ")+"|"+n)
		}
		fs = append(fs, "*|All files")
		if start == "" {
			start = "."
			if home, err := os.UserHomeDir(); err == nil {
				start = home
			}
		}
		return []string{"--title", title, "--getopenfilename", start, strings.Join(fs, "\n")}
	}
	return nil
}

func folderArgs(tool, title, startDir string) []string {
	start := ""
	if st, err := os.Stat(startDir); startDir != "" && err == nil && st.IsDir() {
		start = startDir
	}
	switch tool {
	case "zenity":
		args := []string{"--file-selection", "--directory", "--title=" + title}
		if start != "" {
			args = append(args, "--filename="+strings.TrimSuffix(start, "/")+"/")
		}
		return args
	case "kdialog":
		if start == "" {
			start = "."
			if home, err := os.UserHomeDir(); err == nil {
				start = home
			}
		}
		return []string{"--title", title, "--getexistingdirectory", start}
	}
	return nil
}

// runDialog runs a dialog and returns the chosen path, "" if it was cancelled
// (both tools exit with 1 then).
func runDialog(tool string, args []string) (string, error) {
	out, err := exec.Command(tool, args...).Output()
	if err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) && ee.ExitCode() == 1 {
			return "", nil
		}
		if errors.As(err, &ee) {
			return "", fmt.Errorf("the file dialog failed: %s", strings.TrimSpace(string(ee.Stderr)))
		}
		return "", fmt.Errorf("couldn't show the file dialog: %v", err)
	}
	return strings.TrimRight(string(out), "\r\n"), nil
}

// pickFile shows an "Open" dialog and returns the chosen path, or "" if the
// user cancelled. RETRO_TEST_PICK, when set, is the answer (headless tests).
func pickFile(title, filter, startDir string) (string, error) {
	if p, ok := os.LookupEnv("RETRO_TEST_PICK"); ok {
		return p, nil
	}
	tool := dialogTool()
	if tool == "" {
		return "", errNoDialog
	}
	return runDialog(tool, pickArgs(tool, title, filter, startDir))
}

// pickFolder shows a folder picker. RETRO_TEST_PICK_DIR, when set, is the answer.
func pickFolder(title, startDir string) (string, error) {
	if p, ok := os.LookupEnv("RETRO_TEST_PICK_DIR"); ok {
		return p, nil
	}
	tool := dialogTool()
	if tool == "" {
		return "", errNoDialog
	}
	return runDialog(tool, folderArgs(tool, title, startDir))
}

func execCommand(name string, args ...string) *exec.Cmd { return exec.Command(name, args...) }

// processRunning tells whether a program is running, by file name (a
// Linux build without ".exe" counts too).
func processRunning(image string) bool { return dolphin.Running(image) }

// openFolder shows a folder (or opens a file) in the file manager.
func openFolder(p string) error {
	if _, err := os.Stat(p); err != nil {
		return fmt.Errorf("can't find %s", p)
	}
	if err := exec.Command("xdg-open", p).Start(); err != nil {
		return fmt.Errorf("couldn't open %s: %v", p, err)
	}
	return nil
}
