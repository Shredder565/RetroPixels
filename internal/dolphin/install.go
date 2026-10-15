package dolphin

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
	"time"

	"retropixels/sevenzip"
)

// One-click Dolphin download, as RetroPixels.exe does it: the newest official
// Windows release from dolphin-emu.org, unpacked into a "Dolphin" folder next
// to the program (or %LOCALAPPDATA%\RetroPixels\Dolphin). Dolphin is free
// software (GPL-2.0+); its licence comes with it and its source code is at
// https://github.com/dolphin-emu/dolphin.
//
// The checks are RetroPixels.exe's: the page and the file come only over
// HTTPS from dolphin-emu.org (redirects anywhere else are refused), the file
// must be the size the server announced and at most 400 MB, and it must hold
// Dolphin.exe. dolphin-emu.org publishes no checksum for these downloads, so
// there is none to compare.

const (
	DownloadPage = "https://dolphin-emu.org/download/"
	DownloadHost = "dl.dolphin-emu.org"
	MaxDownload  = 400 << 20
)

// Stable releases are linked as .../releases/2506/dolphin-2506-x64.7z.
var releaseLink = regexp.MustCompile(`https://dl\.dolphin-emu\.org/releases/(\d+)/dolphin-(\d+)-x64\.7z`)

// PickRelease finds the highest-numbered stable x64 release link on the
// download page.
func PickRelease(page string) (link, version string, err error) {
	return pickRelease(page, releaseLink)
}

func pickRelease(page string, re *regexp.Regexp) (string, string, error) {
	var found [][]string
	for _, m := range re.FindAllStringSubmatch(page, -1) {
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

// Progress is what the download is doing: "finding", "downloading",
// "installing", then "done" or "error".
type Progress struct {
	State      string
	Got, Total int64
	Version    string
}

// Installer downloads and unpacks Dolphin. NewInstaller sets it up for
// dolphin-emu.org; tests point it at a local server.
type Installer struct {
	Page       string                // the download page
	Link       *regexp.Regexp        // release links on it (folder number, file number)
	LinkHost   string                // the host the file must come from
	RedirectOK func(u *url.URL) bool // where the requests may be redirected
	Client     *http.Client          // nil: one with a 30-minute limit
	Dirs       []string              // where Dolphin may go, best first
	Unpack     func(archive, dest string) error
	Progress   func(Progress) // called often while downloading
}

// NewInstaller: dolphin-emu.org into InstallDirs().
func NewInstaller() *Installer {
	return &Installer{
		Page:     DownloadPage,
		Link:     releaseLink,
		LinkHost: DownloadHost,
		RedirectOK: func(u *url.URL) bool {
			h := u.Hostname()
			return u.Scheme == "https" && (h == "dolphin-emu.org" || strings.HasSuffix(h, ".dolphin-emu.org"))
		},
		Dirs:   InstallDirs(),
		Unpack: Unpack7z,
	}
}

func (in *Installer) report(p Progress) {
	if in.Progress != nil {
		in.Progress(p)
	}
}

type counter struct {
	w    io.Writer
	got  int64
	tick time.Time
	fn   func(int64)
}

func (c *counter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	c.got += int64(n)
	if time.Since(c.tick) > 100*time.Millisecond {
		c.tick = time.Now()
		c.fn(c.got)
	}
	return n, err
}

// Run downloads the newest Dolphin and returns its Dolphin.exe and version.
// A Dolphin already in the folder is replaced only once the new one is
// unpacked.
func (in *Installer) Run() (exe, version string, err error) {
	client := in.Client
	if client == nil {
		client = &http.Client{Timeout: 30 * time.Minute}
	}
	c := *client
	c.CheckRedirect = func(r *http.Request, via []*http.Request) error {
		if !in.RedirectOK(r.URL) || len(via) > 5 {
			return errors.New("the download was redirected somewhere unexpected")
		}
		return nil
	}
	in.report(Progress{State: "finding"})
	resp, err := c.Get(in.Page)
	if err != nil {
		return "", "", fmt.Errorf("couldn't reach dolphin-emu.org: %v", err)
	}
	page, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", "", fmt.Errorf("dolphin-emu.org answered %s", resp.Status)
	}
	if err != nil {
		return "", "", fmt.Errorf("couldn't read dolphin-emu.org: %v", err)
	}
	link, version, err := pickRelease(string(page), in.Link)
	if err != nil {
		return "", "", err
	}
	if u, err := url.Parse(link); err != nil || u.Host != in.LinkHost || !in.RedirectOK(u) {
		return "", "", errors.New("the Dolphin download link looks wrong")
	}

	// A folder we can write to.
	dest := ""
	for _, d := range in.Dirs {
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
		return "", "", errors.New("no folder RetroPixels can put Dolphin in")
	}
	parent := filepath.Dir(dest)

	// Download.
	in.report(Progress{State: "downloading", Version: version})
	archive := filepath.Join(parent, "dolphin-download.7z.tmp")
	defer os.Remove(archive)
	resp, err = c.Get(link)
	if err != nil {
		return "", "", fmt.Errorf("download failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", "", fmt.Errorf("download failed: %s", resp.Status)
	}
	total := resp.ContentLength
	if total > MaxDownload {
		return "", "", errors.New("the download is much bigger than Dolphin should be, so it wasn't used")
	}
	out, err := os.Create(archive)
	if err != nil {
		return "", "", err
	}
	cw := &counter{w: out, fn: func(got int64) { in.report(Progress{State: "downloading", Got: got, Total: total, Version: version}) }}
	n, err := io.Copy(cw, io.LimitReader(resp.Body, MaxDownload+1))
	out.Close()
	if err != nil {
		return "", "", fmt.Errorf("download interrupted: %v", err)
	}
	in.report(Progress{State: "downloading", Got: n, Total: total, Version: version})
	if n > MaxDownload || (total > 0 && n != total) {
		return "", "", errors.New("the download was incomplete — try again")
	}

	// Unpack next to the final folder, then swap it in.
	in.report(Progress{State: "installing", Got: n, Total: total, Version: version})
	tmp := filepath.Join(parent, "dolphin-unpack.tmp")
	os.RemoveAll(tmp)
	defer os.RemoveAll(tmp)
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return "", "", err
	}
	if err := in.Unpack(archive, tmp); err != nil {
		return "", "", fmt.Errorf("couldn't unpack Dolphin: %v", err)
	}
	root := ""
	WalkLimited(tmp, 3, func(p string, d os.DirEntry) bool {
		if !d.IsDir() && strings.EqualFold(d.Name(), "Dolphin.exe") {
			root = filepath.Dir(p)
			return false
		}
		return true
	})
	if root == "" {
		return "", "", errors.New("Dolphin.exe wasn't in the download")
	}
	old := dest + ".old"
	os.RemoveAll(old)
	if _, err := os.Stat(dest); err == nil {
		if err := os.Rename(dest, old); err != nil {
			return "", "", fmt.Errorf("couldn't replace the old Dolphin folder (is Dolphin open?): %v", err)
		}
	}
	if err := os.Rename(root, dest); err != nil {
		os.Rename(old, dest)
		return "", "", err
	}
	os.RemoveAll(old)
	_ = os.WriteFile(filepath.Join(dest, "ABOUT-THIS-FOLDER.txt"), []byte(Notice(version)), 0o644)
	return filepath.Join(dest, "Dolphin.exe"), version, nil
}

// Unpack7z uses Windows' own tar.exe when it can read .7z (Windows 11), and
// RetroPixels' own extractor otherwise.
func Unpack7z(archive, dest string) error {
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

// Notice is the ABOUT-THIS-FOLDER.txt put next to the downloaded Dolphin
// (the same text RetroPixels.exe writes).
func Notice(version string) string {
	return "This folder holds Dolphin " + version + ", the free GameCube and Wii emulator,\r\n" +
		"downloaded unchanged by RetroPixels from " + DownloadPage + "\r\n\r\n" +
		"Dolphin is made by the Dolphin Emulator Project and is licensed under the\r\n" +
		"GNU General Public License, version 2 or later: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html\r\n" +
		"Its source code: https://github.com/dolphin-emu/dolphin\r\n"
}
