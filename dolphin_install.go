package main

// One-click Dolphin download: the latest official Windows release from
// dolphin-emu.org, unpacked into a "Dolphin" folder next to RetroPixels (or in
// the user's app data). Dolphin is free software (GPL-2.0+); its licence comes
// with it and its source code is at https://github.com/dolphin-emu/dolphin.
// Games still play inside the RetroPixels window as before.

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/sevenzip"
)

const (
	dolDownloadPage = "https://dolphin-emu.org/download/"
	dolDownloadHost = "dl.dolphin-emu.org"
	dolMaxDownload  = 400 << 20
)

// Stable releases are linked as .../releases/2506/dolphin-2506-x64.7z.
var reDolRelease = regexp.MustCompile(`https://dl\.dolphin-emu\.org/releases/(\d+)/dolphin-(\d+)-x64\.7z`)

var (
	dolInstMu    sync.Mutex
	dolInstState = "idle" // idle | finding | downloading | installing | done | error
	dolInstGot   int64
	dolInstTotal int64
	dolInstErr   string
	dolInstVer   string
	dolInstallF  = installDolphin
)

// dolphinInstallDirs: next to the app if possible, otherwise in the user's app data.
func dolphinInstallDirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Join(filepath.Dir(exe), "Dolphin"))
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		dirs = append(dirs, filepath.Join(la, appName, "Dolphin"))
	}
	return dirs
}

func dolInstallStatus() map[string]any {
	dolInstMu.Lock()
	defer dolInstMu.Unlock()
	return map[string]any{"state": dolInstState, "got": dolInstGot, "total": dolInstTotal, "error": dolInstErr, "version": dolInstVer}
}

func setDolInst(state string, err error) {
	dolInstMu.Lock()
	dolInstState = state
	if err != nil {
		dolInstErr = err.Error()
	} else if state != "error" {
		dolInstErr = ""
	}
	dolInstMu.Unlock()
}

func dolInstBusy() bool {
	dolInstMu.Lock()
	defer dolInstMu.Unlock()
	return dolInstState == "finding" || dolInstState == "downloading" || dolInstState == "installing"
}

type dolCounter struct{ w io.Writer }

func (c dolCounter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	dolInstMu.Lock()
	dolInstGot += int64(n)
	dolInstMu.Unlock()
	return n, err
}

func startDolphinInstall() error {
	if dolphinRunning() {
		return errors.New("close Dolphin first")
	}
	dolInstMu.Lock()
	if dolInstState == "finding" || dolInstState == "downloading" || dolInstState == "installing" {
		dolInstMu.Unlock()
		return nil
	}
	dolInstState, dolInstGot, dolInstTotal, dolInstErr = "finding", 0, 0, ""
	dolInstMu.Unlock()
	go func() {
		exe, err := dolInstallF()
		if err != nil {
			setDolInst("error", err)
			return
		}
		cfgMu.Lock()
		cfg.DolphinExe = exe
		_ = saveConfigLocked()
		cfgMu.Unlock()
		setDolInst("done", nil)
	}()
	return nil
}

// latestDolphin reads the download page for the newest stable release.
func latestDolphin(client *http.Client) (link, version string, err error) {
	resp, err := client.Get(dolDownloadPage)
	if err != nil {
		return "", "", fmt.Errorf("couldn't reach dolphin-emu.org: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", "", fmt.Errorf("dolphin-emu.org answered %s", resp.Status)
	}
	page, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return "", "", fmt.Errorf("couldn't read dolphin-emu.org: %v", err)
	}
	return pickDolphinRelease(string(page))
}

// pickDolphinRelease finds the highest-numbered stable x64 release link.
func pickDolphinRelease(page string) (string, string, error) {
	var found [][]string
	for _, m := range reDolRelease.FindAllStringSubmatch(page, -1) {
		if m[1] == m[2] {
			found = append(found, m)
		}
	}
	if len(found) == 0 {
		return "", "", errors.New("couldn't find the Windows download on dolphin-emu.org")
	}
	sort.Slice(found, func(i, j int) bool {
		a, _ := strconv.Atoi(found[i][1])
		b, _ := strconv.Atoi(found[j][1])
		return a > b
	})
	return found[0][0], found[0][1], nil
}

func installDolphin() (string, error) {
	client := &http.Client{
		Timeout: 30 * time.Minute,
		// Only follow redirects that stay on Dolphin's own HTTPS servers.
		CheckRedirect: func(r *http.Request, via []*http.Request) error {
			if r.URL.Scheme != "https" || !strings.HasSuffix(r.URL.Hostname(), "dolphin-emu.org") || len(via) > 5 {
				return errors.New("the download was redirected somewhere unexpected")
			}
			return nil
		},
	}
	link, version, err := latestDolphin(client)
	if err != nil {
		return "", err
	}
	if u, err := url.Parse(link); err != nil || u.Host != dolDownloadHost {
		return "", errors.New("the Dolphin download link looks wrong")
	}
	dolInstMu.Lock()
	dolInstVer = version
	dolInstMu.Unlock()

	// A folder we can write to.
	dest := ""
	for _, d := range dolphinInstallDirs() {
		if os.MkdirAll(filepath.Dir(d), 0o755) != nil {
			continue
		}
		if f, err := os.CreateTemp(filepath.Dir(d), "dl-*.tmp"); err == nil {
			f.Close()
			os.Remove(f.Name())
			dest = d
			break
		}
	}
	if dest == "" {
		return "", errors.New("no folder RetroPixels can put Dolphin in")
	}
	parent := filepath.Dir(dest)

	// Download.
	setDolInst("downloading", nil)
	archive := filepath.Join(parent, "dolphin-download.7z.tmp")
	defer os.Remove(archive)
	resp, err := client.Get(link)
	if err != nil {
		return "", fmt.Errorf("download failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("download failed: %s", resp.Status)
	}
	dolInstMu.Lock()
	dolInstTotal = resp.ContentLength
	dolInstMu.Unlock()
	out, err := os.Create(archive)
	if err != nil {
		return "", err
	}
	n, err := io.Copy(dolCounter{out}, io.LimitReader(resp.Body, dolMaxDownload+1))
	out.Close()
	if err != nil {
		return "", fmt.Errorf("download interrupted: %v", err)
	}
	if n > dolMaxDownload || (resp.ContentLength > 0 && n != resp.ContentLength) {
		return "", errors.New("the download was incomplete — try again")
	}

	// Unpack next to the final folder, then swap it in.
	setDolInst("installing", nil)
	tmp := filepath.Join(parent, "dolphin-unpack.tmp")
	os.RemoveAll(tmp)
	defer os.RemoveAll(tmp)
	if err := unpack7z(archive, tmp); err != nil {
		return "", fmt.Errorf("couldn't unpack Dolphin: %v", err)
	}
	root := ""
	walkLimited(tmp, 3, func(p string, d os.DirEntry) bool {
		if !d.IsDir() && strings.EqualFold(d.Name(), "Dolphin.exe") {
			root = filepath.Dir(p)
			return false
		}
		return true
	})
	if root == "" {
		return "", errors.New("Dolphin.exe wasn't in the download")
	}
	old := dest + ".old"
	os.RemoveAll(old)
	if _, err := os.Stat(dest); err == nil {
		if err := os.Rename(dest, old); err != nil {
			return "", fmt.Errorf("couldn't replace the old Dolphin folder (is Dolphin open?): %v", err)
		}
	}
	if err := os.Rename(root, dest); err != nil {
		os.Rename(old, dest)
		return "", err
	}
	os.RemoveAll(old)
	_ = os.WriteFile(filepath.Join(dest, "ABOUT-THIS-FOLDER.txt"), []byte(dolphinNotice(version)), 0o644)
	return filepath.Join(dest, "Dolphin.exe"), nil
}

// unpack7z uses Windows' own tar.exe when it can read .7z (Windows 11), and
// RetroPixels' own extractor otherwise.
func unpack7z(archive, dest string) error {
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	if err := systemUnpack7z(archive, dest); err == nil {
		return nil
	}
	os.RemoveAll(dest)
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	return sevenzip.ExtractAll(archive, dest, nil)
}

func registerDolphinInstallRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/dol/install", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			if err := startDolphinInstall(); err != nil {
				fail(w, err)
				return
			}
		}
		writeJSON(w, 200, map[string]any{"install": dolInstallStatus(), "exe": dolphinExe()})
	}))
}

func dolphinNotice(version string) string {
	return "This folder holds Dolphin " + version + ", the free GameCube and Wii emulator,\r\n" +
		"downloaded unchanged by RetroPixels from " + dolDownloadPage + "\r\n\r\n" +
		"Dolphin is made by the Dolphin Emulator Project and is licensed under the\r\n" +
		"GNU General Public License, version 2 or later: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html\r\n" +
		"Its source code: https://github.com/dolphin-emu/dolphin\r\n"
}
