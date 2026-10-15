package dolphin

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
)

func TestPickRelease(t *testing.T) {
	page := `<a href="https://dl.dolphin-emu.org/releases/2503/dolphin-2503-x64.7z">old</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2506-x64.7z">new</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2506-universal.dmg">mac</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2506-arm64.7z">arm</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2505-x64.7z">odd</a>`
	link, ver, err := PickRelease(page)
	if err != nil || ver != "2506" || link != "https://dl.dolphin-emu.org/releases/2506/dolphin-2506-x64.7z" {
		t.Fatalf("got %q %q %v", link, ver, err)
	}
	if _, _, err := PickRelease("nothing here"); err == nil {
		t.Fatal("expected an error")
	}
	in := NewInstaller()
	for u, ok := range map[string]bool{
		"https://dolphin-emu.org/download/": true, "https://dl.dolphin-emu.org/x.7z": true,
		"http://dl.dolphin-emu.org/x.7z": false, "https://evil-dolphin-emu.org/x": false, "https://example.com/": false,
	} {
		p, _ := url.Parse(u)
		if in.RedirectOK(p) != ok {
			t.Errorf("RedirectOK(%s) = %v", u, !ok)
		}
	}
}

// fakeSite is a local HTTPS stand-in for dolphin-emu.org.
type fakeSite struct {
	srv     *httptest.Server
	archive []byte
	mu      sync.Mutex
	short   bool   // send less than the announced size
	page    string // overrides the download page
	hits    []string
}

func newFakeSite(t *testing.T) *fakeSite {
	data, err := os.ReadFile(filepath.Join("testdata", "fake-dolphin-x64.7z"))
	if err != nil {
		t.Fatal(err)
	}
	s := &fakeSite{archive: data}
	s.srv = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.hits = append(s.hits, r.URL.Path)
		short, page := s.short, s.page
		s.mu.Unlock()
		switch {
		case r.URL.Path == "/download/":
			if page == "" {
				page = fmt.Sprintf(`<a href="%[1]s/releases/2503/dolphin-2503-x64.7z">old</a> <a href="%[1]s/releases/2506/dolphin-2506-x64.7z">new</a>`, s.srv.URL)
			}
			fmt.Fprint(w, page)
		case r.URL.Path == "/elsewhere":
			http.Redirect(w, r, "https://example.com/dolphin.7z", http.StatusFound)
		case strings.HasSuffix(r.URL.Path, "dolphin-2506-x64.7z"):
			body := s.archive
			w.Header().Set("Content-Length", strconv.Itoa(len(body)))
			if short {
				body = body[:len(body)/2]
				if hj, ok := w.(http.Hijacker); ok {
					w.WriteHeader(200)
					w.Write(body)
					w.(http.Flusher).Flush()
					c, _, _ := hj.Hijack()
					c.Close()
					return
				}
			}
			w.Write(body)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(s.srv.Close)
	return s
}

func (s *fakeSite) installer(dirs ...string) *Installer {
	u, _ := url.Parse(s.srv.URL)
	in := NewInstaller()
	in.Page = s.srv.URL + "/download/"
	in.Link = regexp.MustCompile(regexp.QuoteMeta(s.srv.URL) + `/releases/(\d+)/dolphin-(\d+)-x64\.7z`)
	in.LinkHost = u.Host
	in.RedirectOK = func(r *url.URL) bool { return r.Scheme == "https" && r.Host == u.Host }
	in.Client = s.srv.Client()
	in.Dirs = dirs
	return in
}

// The whole download: newest release, unpacked with RetroPixels' own 7z
// reader into the Dolphin folder, replacing an older one, with the notice.
func TestInstall(t *testing.T) {
	site := newFakeSite(t)
	parent := t.TempDir()
	dest := filepath.Join(parent, "Dolphin")
	os.MkdirAll(dest, 0o755)
	os.WriteFile(filepath.Join(dest, "old.txt"), []byte("old Dolphin"), 0o644)

	blocked := filepath.Join(t.TempDir(), "a-file")
	os.WriteFile(blocked, nil, 0o644)
	in := site.installer(filepath.Join(blocked, "Dolphin"), dest) // the first folder can't be made: the next is used
	var states []string
	var last Progress
	in.Progress = func(p Progress) {
		if len(states) == 0 || states[len(states)-1] != p.State {
			states = append(states, p.State)
		}
		last = p
	}
	exe, ver, err := in.Run()
	if err != nil {
		t.Fatal(err)
	}
	if exe != filepath.Join(dest, "Dolphin.exe") || ver != "2506" {
		t.Fatalf("got %q %q", exe, ver)
	}
	if b, _ := os.ReadFile(exe); !strings.Contains(string(b), "stand-in") {
		t.Fatalf("Dolphin.exe = %q", b)
	}
	if _, err := os.Stat(filepath.Join(dest, "old.txt")); err == nil {
		t.Error("the old Dolphin folder wasn't replaced")
	}
	if n, _ := os.ReadFile(filepath.Join(dest, "ABOUT-THIS-FOLDER.txt")); !strings.Contains(string(n), "Dolphin 2506") {
		t.Errorf("notice: %q", n)
	}
	if Version(exe) != "Version 2506 (test stand-in)" {
		t.Errorf("version %q", Version(exe))
	}
	if strings.Join(states, ",") != "finding,downloading,installing" || last.Got != int64(len(site.archive)) || last.Total != last.Got {
		t.Errorf("progress %v %+v", states, last)
	}
	left, _ := os.ReadDir(parent)
	for _, e := range left {
		if e.Name() != "Dolphin" {
			t.Errorf("left behind: %s", e.Name())
		}
	}
}

func TestInstallRefuses(t *testing.T) {
	site := newFakeSite(t)
	try := func(what string, setup func(in *Installer), want string) {
		t.Helper()
		parent := t.TempDir()
		in := site.installer(filepath.Join(parent, "Dolphin"))
		if setup != nil {
			setup(in)
		}
		_, _, err := in.Run()
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: err = %v, want %q", what, err, want)
		}
		if _, err := os.Stat(filepath.Join(parent, "Dolphin")); err == nil {
			t.Errorf("%s: installed anyway", what)
		}
		if left, _ := os.ReadDir(parent); len(left) != 0 {
			t.Errorf("%s: left %d files behind", what, len(left))
		}
	}
	site.short = true
	try("cut-off download", nil, "download")
	site.short = false

	try("no Dolphin.exe inside", func(in *Installer) {
		in.Unpack = func(a, d string) error { return os.WriteFile(filepath.Join(d, "readme.txt"), nil, 0o644) }
	}, "Dolphin.exe wasn't in the download")
	try("broken archive", func(in *Installer) {
		in.Unpack = func(a, d string) error { return fmt.Errorf("bad data") }
	}, "couldn't unpack")
	try("page without a download", func(in *Installer) { site.page = "nothing" }, "couldn't find the Windows download")
	site.page = `<a href="https://dl.dolphin-emu.org.example.net/releases/2506/dolphin-2506-x64.7z">`
	try("link elsewhere", func(in *Installer) {
		in.Link = regexp.MustCompile(`https://dl\.dolphin-emu\.org\.example\.net/releases/(\d+)/dolphin-(\d+)-x64\.7z`)
	}, "looks wrong")
	site.page = ""
	try("redirected elsewhere", func(in *Installer) { in.Page = site.srv.URL + "/elsewhere" }, "redirected somewhere unexpected")
}
