//go:build windows

package main

import (
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode/utf16"
	"unsafe"
)

const createNoWindow = 0x08000000

var (
	user32         = syscall.NewLazyDLL("user32.dll")
	procMessageBox = user32.NewProc("MessageBoxW")
)

func messageBox(text, title string, flags uintptr) {
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString(title)
	procMessageBox.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), flags)
}

func showError(msg string) { messageBox(msg, appName, 0x10|0x40000) } // MB_ICONERROR | MB_TOPMOST

// findBrowser returns Edge (built into Windows 10/11) or Chrome.
func findBrowser() string {
	var cands []string
	for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"} {
		if d := os.Getenv(env); d != "" {
			cands = append(cands,
				filepath.Join(d, "Microsoft", "Edge", "Application", "msedge.exe"),
				filepath.Join(d, "Google", "Chrome", "Application", "chrome.exe"))
		}
	}
	for _, c := range cands {
		if st, err := os.Stat(c); err == nil && !st.IsDir() {
			return c
		}
	}
	return ""
}

// openAppWindow opens the page in its own app window. When wait is true it
// blocks until the window has closed and the page has stopped checking in.
func openAppWindow(url string, wait bool) error {
	browser := findBrowser()
	if browser == "" {
		// No Edge/Chrome: use the default browser and keep running until the user quits.
		_ = exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
		if wait {
			messageBox(appName+" is open in your web browser.\n\nKeep this box open while you play. Click OK to quit "+appName+".",
				appName, 0x40|0x40000)
		}
		return nil
	}

	profile := filepath.Join(os.Getenv("LOCALAPPDATA"), appName, "BrowserProfile")
	_ = os.MkdirAll(profile, 0o755)
	cmd := exec.Command(browser,
		"--app="+url,
		"--user-data-dir="+profile,
		"--no-first-run",
		"--no-default-browser-check",
		"--window-size=1280,900",
		"--autoplay-policy=no-user-gesture-required",
		"--disable-features=Translate,msEdgeSidebarV2,msUndersideButton",
	)
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

// pickFile shows a standard Windows "Open" dialog and returns the chosen path,
// or "" if the user cancelled.
func pickFile(title, filter, startDir string) (string, error) {
	q := func(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }
	script := `
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Windows.Forms
$owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }
$d = New-Object System.Windows.Forms.OpenFileDialog
$d.Title = ` + q(title) + `
$d.Filter = ` + q(filter) + `
$d.CheckFileExists = $true
$start = ` + q(startDir) + `
if ($start -and (Test-Path -LiteralPath $start)) { $d.InitialDirectory = $start }
if ($d.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { [Console]::Out.Write($d.FileName) }
$owner.Dispose()
`
	return runPowerShell(script)
}

// pickFolder shows the Windows folder picker.
func pickFolder(title, startDir string) (string, error) {
	q := func(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }
	script := `
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Windows.Forms
$owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }
$d = New-Object System.Windows.Forms.FolderBrowserDialog
$d.Description = ` + q(title) + `
$start = ` + q(startDir) + `
if ($start -and (Test-Path -LiteralPath $start)) { $d.SelectedPath = $start }
if ($d.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { [Console]::Out.Write($d.SelectedPath) }
$owner.Dispose()
`
	return runPowerShell(script)
}

func runPowerShell(script string) (string, error) {
	u := utf16.Encode([]rune(script))
	b := make([]byte, len(u)*2)
	for i, v := range u {
		b[i*2] = byte(v)
		b[i*2+1] = byte(v >> 8)
	}
	enc := base64.StdEncoding.EncodeToString(b)

	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-STA",
		"-ExecutionPolicy", "Bypass", "-EncodedCommand", enc)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	out, err := cmd.Output()
	if err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) {
			return "", fmt.Errorf("the file dialog failed: %s", strings.TrimSpace(string(ee.Stderr)))
		}
		return "", fmt.Errorf("couldn't show the file dialog: %v", err)
	}
	return strings.TrimSpace(string(out)), nil
}

func execCommand(name string, args ...string) *exec.Cmd { return exec.Command(name, args...) }

// processRunning checks the Windows task list for a program by file name.
// Results are cached briefly because the app polls.
func processRunning(image string) bool {
	procMu.Lock()
	defer procMu.Unlock()
	if c, ok := procCache[image]; ok && time.Since(c.at) < 2*time.Second {
		return c.running
	}
	cmd := exec.Command("tasklist", "/FI", "IMAGENAME eq "+image, "/NH", "/FO", "CSV")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	out, err := cmd.Output()
	r := err == nil && strings.Contains(strings.ToLower(string(out)), strings.ToLower(image))
	procCache[image] = procEntry{r, time.Now()}
	return r
}

type procEntry struct {
	running bool
	at      time.Time
}

var (
	procMu    sync.Mutex
	procCache = map[string]procEntry{}
)

// openFolder shows a folder (or opens a file) in Windows Explorer.
func openFolder(p string) error {
	if _, err := os.Stat(p); err != nil {
		return fmt.Errorf("can't find %s", p)
	}
	return exec.Command("explorer.exe", p).Start()
}
