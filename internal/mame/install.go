package mame

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash"
	"hash/crc32"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"retropixels/internal/dolphin"
)

// Downloading official MAME releases. The MAME team publishes every Windows
// release on GitHub (github.com/mamedev/mame/releases) as a self-extracting
// 7-Zip file, mame0270b_x64.exe (mame0250b_64bit.exe for older ones).
// RetroPixels unpacks it itself, without running it, into a folder named for
// the version, so any number of versions sit side by side.
//
// The checks: the list and the file come only over HTTPS from github.com and
// GitHub's download servers (redirects anywhere else are refused), the file
// must be the size GitHub lists and at most 400 MB, it must match the SHA-256
// GitHub lists for it (when it lists one), and it must hold mame.exe.

const (
	ReleasesAPI   = "https://api.github.com/repos/mamedev/mame/releases"
	ReleasesPage  = "https://github.com/mamedev/mame/releases"
	MaxDownload   = 400 << 20
	releasePrefix = "/mamedev/mame/releases/download/"
)

// Release is one official MAME release for Windows.
type Release struct {
	Version   string    `json:"version"` // "0.270"
	Tag       string    `json:"tag"`     // "mame0270"
	URL       string    `json:"url"`
	Size      int64     `json:"size"`
	SHA256    string    `json:"sha256,omitempty"`
	Published time.Time `json:"published"`
}

var (
	reTag   = regexp.MustCompile(`^mame0?(\d{2,4})$`)
	reAsset = regexp.MustCompile(`(?i)^mame0?(\d{2,4})b_(64bit|x64)\.exe$`)
)

// TagVersion turns a release tag (mame0270) into a version (0.270).
func TagVersion(tag string) string {
	m := reTag.FindStringSubmatch(strings.ToLower(tag))
	if m == nil {
		return ""
	}
	n, _ := strconv.Atoi(m[1])
	return "0." + strconv.Itoa(n)
}

type ghRelease struct {
	Tag        string    `json:"tag_name"`
	Draft      bool      `json:"draft"`
	Prerelease bool      `json:"prerelease"`
	Published  time.Time `json:"published_at"`
	Assets     []struct {
		Name   string `json:"name"`
		URL    string `json:"browser_download_url"`
		Size   int64  `json:"size"`
		Digest string `json:"digest"` // "sha256:…"
	} `json:"assets"`
}

// ParseReleases picks the 64-bit Windows download of each release from
// GitHub's release list, newest first.
func ParseReleases(body []byte) ([]Release, error) { return parseReleases(body, releaseURLOK) }

func parseReleases(body []byte, urlOK func(string) bool) ([]Release, error) {
	var list []ghRelease
	if err := json.Unmarshal(body, &list); err != nil {
		return nil, errors.New("GitHub's list of MAME releases couldn't be read")
	}
	var out []Release
	for _, r := range list {
		v := TagVersion(r.Tag)
		if v == "" || r.Draft || r.Prerelease {
			continue
		}
		for _, a := range r.Assets {
			if !reAsset.MatchString(a.Name) || !urlOK(a.URL) {
				continue
			}
			rel := Release{Version: v, Tag: r.Tag, URL: a.URL, Size: a.Size, Published: r.Published}
			if d, ok := strings.CutPrefix(a.Digest, "sha256:"); ok && len(d) == 64 {
				rel.SHA256 = strings.ToLower(d)
			}
			out = append(out, rel)
			break
		}
	}
	sortReleases(out)
	return out, nil
}

func sortReleases(r []Release) {
	sort.SliceStable(r, func(a, b int) bool { return CompareVersions(r[a].Version, r[b].Version) > 0 })
}

func releaseURLOK(link string) bool {
	u, err := url.Parse(link)
	return err == nil && u.Scheme == "https" && u.Host == "github.com" && strings.HasPrefix(u.Path, releasePrefix)
}

// Progress is what a download is doing: "finding", "downloading",
// "installing", then "done" or "error".
type Progress struct {
	State   string `json:"state"`
	Got     int64  `json:"got"`
	Total   int64  `json:"total"`
	Version string `json:"version"`
	Error   string `json:"error,omitempty"`
}

// Installer lists and downloads official MAME releases. NewInstaller sets
// it up for GitHub; tests point it at a local server.
type Installer struct {
	API        string                // the release list
	URLOK      func(string) bool     // what a download link must look like
	RedirectOK func(u *url.URL) bool // where requests may be redirected
	Client     *http.Client          // nil: one with a 30-minute limit
	Dirs       []string              // where versions may go, best first
	Unpack     func(sfx, dest string) error
	Progress   func(Progress) // called often while downloading
}

// NewInstaller: MAME's GitHub releases into InstallDirs().
func NewInstaller() *Installer {
	api := ReleasesAPI
	if t := os.Getenv("RETRO_TEST_MAME_RELEASES"); t != "" {
		api = t // tests: a local copy of the list
	}
	return &Installer{
		API:   api,
		URLOK: releaseURLOK,
		RedirectOK: func(u *url.URL) bool {
			h := u.Hostname()
			return u.Scheme == "https" && (h == "github.com" || h == "api.github.com" || strings.HasSuffix(h, ".githubusercontent.com"))
		},
		Dirs:   InstallDirs(),
		Unpack: unpackSFX,
	}
}

func (in *Installer) report(p Progress) {
	if in.Progress != nil {
		in.Progress(p)
	}
}

func (in *Installer) client() *http.Client {
	c := http.Client{Timeout: 30 * time.Minute}
	if in.Client != nil {
		c = *in.Client
	}
	c.CheckRedirect = func(r *http.Request, via []*http.Request) error {
		if !in.RedirectOK(r.URL) || len(via) > 5 {
			return errors.New("the download was redirected somewhere unexpected")
		}
		return nil
	}
	return &c
}

// Releases lists the official releases, newest first (a few pages of
// GitHub's list: every release GitHub has, back to MAME 0.171 or so).
func (in *Installer) Releases() ([]Release, error) {
	c := in.client()
	var all []Release
	for page := 1; page <= 4; page++ {
		req, _ := http.NewRequest("GET", fmt.Sprintf("%s?per_page=100&page=%d", in.API, page), nil)
		req.Header.Set("Accept", "application/vnd.github+json")
		req.Header.Set("User-Agent", "RetroPixels")
		resp, err := c.Do(req)
		if err != nil {
			if page > 1 && len(all) > 0 {
				break
			}
			return nil, fmt.Errorf("couldn't reach GitHub for MAME's releases: %v", err)
		}
		body, err := io.ReadAll(io.LimitReader(resp.Body, 16<<20))
		resp.Body.Close()
		if resp.StatusCode != 200 {
			if page > 1 && len(all) > 0 {
				break
			}
			if resp.StatusCode == 403 || resp.StatusCode == 429 {
				return nil, errors.New("GitHub is limiting requests right now: try again in a while, or download MAME from " + ReleasesPage + " and use Add MAME…")
			}
			return nil, fmt.Errorf("GitHub answered %s", resp.Status)
		}
		if err != nil {
			return nil, fmt.Errorf("couldn't read GitHub's list: %v", err)
		}
		rel, err := parseReleases(body, in.URLOK)
		if err != nil {
			return nil, err
		}
		all = append(all, rel...)
		var raw []json.RawMessage
		if json.Unmarshal(body, &raw) == nil && len(raw) < 100 {
			break
		}
	}
	if len(all) == 0 {
		return nil, errors.New("couldn't find MAME's Windows downloads on GitHub")
	}
	sortReleases(all)
	return all, nil
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

// Install downloads one release and returns its mame.exe. A version that's
// already downloaded is used as it is.
func (in *Installer) Install(r Release) (string, error) {
	if r.Version == "" || !in.URLOK(r.URL) {
		return "", errors.New("that MAME download link looks wrong")
	}
	// A folder we can write to.
	base := ""
	for _, d := range in.Dirs {
		if os.MkdirAll(d, 0o755) != nil {
			continue
		}
		if f, err := os.CreateTemp(d, "dl-*.tmp"); err == nil {
			f.Close()
			os.Remove(f.Name())
			base = d
			break
		}
	}
	if base == "" {
		return "", errors.New("no folder RetroPixels can put MAME in")
	}
	dest := filepath.Join(base, "MAME "+r.Version)
	if exe := findExe(dest, 1); exe != "" {
		return exe, nil
	}

	c := in.client()
	in.report(Progress{State: "downloading", Version: r.Version, Total: r.Size})
	archive := filepath.Join(base, "mame-download-"+r.Version+".tmp")
	defer os.Remove(archive)
	req, _ := http.NewRequest("GET", r.URL, nil)
	req.Header.Set("User-Agent", "RetroPixels")
	resp, err := c.Do(req)
	if err != nil {
		return "", fmt.Errorf("download failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("download failed: %s", resp.Status)
	}
	total := resp.ContentLength
	if total <= 0 {
		total = r.Size
	}
	if total > MaxDownload {
		return "", errors.New("the download is much bigger than MAME should be, so it wasn't used")
	}
	out, err := os.Create(archive)
	if err != nil {
		return "", err
	}
	var sum hash.Hash = sha256.New()
	cw := &counter{w: io.MultiWriter(out, sum), fn: func(got int64) {
		in.report(Progress{State: "downloading", Got: got, Total: total, Version: r.Version})
	}}
	n, err := io.Copy(cw, io.LimitReader(resp.Body, MaxDownload+1))
	out.Close()
	if err != nil {
		return "", fmt.Errorf("download interrupted: %v", err)
	}
	in.report(Progress{State: "downloading", Got: n, Total: total, Version: r.Version})
	if n > MaxDownload || (total > 0 && n != total) || (r.Size > 0 && n != r.Size) {
		return "", errors.New("the download was incomplete — try again")
	}
	if r.SHA256 != "" && hex.EncodeToString(sum.Sum(nil)) != r.SHA256 {
		return "", errors.New("the download doesn't match the checksum GitHub lists for it, so it wasn't used — try again")
	}

	// Unpack next to the final folder, then move it in.
	in.report(Progress{State: "installing", Got: n, Total: total, Version: r.Version})
	tmp := filepath.Join(base, "mame-unpack-"+r.Version+".tmp")
	os.RemoveAll(tmp)
	defer os.RemoveAll(tmp)
	if err := in.Unpack(archive, tmp); err != nil {
		return "", fmt.Errorf("couldn't unpack MAME %s: %v", r.Version, err)
	}
	exe := findExe(tmp, 3)
	if exe == "" {
		return "", errors.New("mame.exe wasn't in the download")
	}
	root := filepath.Dir(exe)
	os.RemoveAll(dest)
	if err := os.Rename(root, dest); err != nil {
		return "", err
	}
	_ = os.WriteFile(filepath.Join(dest, "ABOUT-THIS-FOLDER.txt"), []byte(Notice(r.Version)), 0o644)
	return filepath.Join(dest, filepath.Base(exe)), nil
}

// findExe finds MAME's program in a folder (depth levels deep).
func findExe(dir string, depth int) string {
	found := ""
	limitedWalk(dir, depth, func(p string, d os.DirEntry) {
		if found == "" && !d.IsDir() && IsMameExe(d.Name()) {
			found = p
		}
	})
	return found
}

// ---------------- unpacking ----------------

var sig7z = []byte{'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}

// FindArchive finds where the 7z archive starts inside a self-extracting
// .exe: the first 7z signature whose start header checks out (the
// extractor's own code can hold the signature bytes too).
func FindArchive(path string) (int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	r := bufio.NewReaderSize(f, 1<<20)
	var pos int64
	window := make([]byte, 0, 2<<20)
	chunk := make([]byte, 1<<20)
	for {
		n, err := r.Read(chunk)
		window = append(window, chunk[:n]...)
		for i := 0; i+32 <= len(window); i++ {
			if window[i] != '7' || !bytes.Equal(window[i:i+6], sig7z) {
				continue
			}
			h := window[i : i+32]
			if binary.LittleEndian.Uint32(h[8:12]) == crc32.ChecksumIEEE(h[12:32]) && binary.LittleEndian.Uint64(h[12:20]) > 0 {
				return pos + int64(i), nil
			}
		}
		if err != nil {
			break
		}
		// keep the last 31 bytes: a header can straddle two chunks
		keep := 31
		if len(window) < keep {
			keep = len(window)
		}
		pos += int64(len(window) - keep)
		window = append(window[:0], window[len(window)-keep:]...)
		if pos > MaxDownload {
			break
		}
	}
	return 0, errors.New("no 7-Zip archive inside the download")
}

// unpackSFX unpacks MAME's self-extracting download without running it: the
// archive inside is copied out and unpacked like Dolphin's (Windows' own
// tar.exe, else RetroPixels' own 7-Zip reader). Only if both fail is the
// official extractor itself run, silently, into the folder.
func unpackSFX(sfx, dest string) error {
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	off, err := FindArchive(sfx)
	if err == nil {
		tail := sfx + ".7z"
		defer os.Remove(tail)
		if err = copyFrom(sfx, tail, off); err == nil {
			if err = dolphin.Unpack7z(tail, dest); err == nil {
				return nil
			}
		}
	}
	os.RemoveAll(dest)
	if os.PathSeparator != '\\' {
		return err
	}
	if e := os.MkdirAll(dest, 0o755); e != nil {
		return e
	}
	exe := sfx + ".exe" // it must be named .exe to run
	if e := os.Rename(sfx, exe); e != nil {
		return err
	}
	defer os.Rename(exe, sfx)
	cmd := exec.Command(exe, "-o"+dest, "-y")
	hideProbe(cmd)
	if e := cmd.Run(); e != nil {
		return fmt.Errorf("%v (and the extractor: %v)", err, e)
	}
	return nil
}

func copyFrom(src, dst string, off int64) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if _, err := in.Seek(off, io.SeekStart); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// Notice is the ABOUT-THIS-FOLDER.txt put next to a downloaded MAME.
func Notice(version string) string {
	return "This folder holds MAME " + version + ", the free multi-purpose emulator,\r\n" +
		"downloaded unchanged by RetroPixels from the MAME team's official releases:\r\n" + ReleasesPage + "\r\n\r\n" +
		"MAME is made by the MAME team and is licensed under the GNU General Public\r\n" +
		"License, version 2 or later: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html\r\n" +
		"Its source code: https://github.com/mamedev/mame\r\n" +
		"MAME's own site: https://www.mamedev.org\r\n"
}
