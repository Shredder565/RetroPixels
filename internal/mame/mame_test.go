package mame

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"retropixels/sevenzip"
)

func TestParseVersionOutput(t *testing.T) {
	for in, want := range map[string]string{
		"0.270 (mame0270)\n":                   "0.270",
		"0.106u12 (Mar 1 2006)":                "0.106u12",
		"M.A.M.E. v0.37b16 (Jul 31 2001)\n...": "0.37b16",
		"MAME v0.139 (Sep 2010)":               "0.139",
		"Unknown option: -version":             "",
	} {
		if got := ParseVersionOutput(in); got != want {
			t.Errorf("ParseVersionOutput(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestVersionFromPath(t *testing.T) {
	for in, want := range map[string]string{
		filepath.Join("C:", "Emus", "mame0270", "mame.exe"):    "0.270",
		filepath.Join("D:", "MAME 0.250", "mame.exe"):          "0.250",
		filepath.Join("D:", "mame0106u12", "mame.exe"):         "0.106u12",
		filepath.Join("D:", "MAME64", "mame64.exe"):            "",
		filepath.Join("D:", "Emulators", "Arcade", "mame.exe"): "",
	} {
		if got := VersionFromPath(in); got != want {
			t.Errorf("VersionFromPath(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestCompareVersions(t *testing.T) {
	order := []string{"", "0.37b16", "0.37", "0.106", "0.106u2", "0.106u12", "0.139", "0.270", "1.0"}
	for i := range order {
		for j := range order {
			want := 0
			if i < j {
				want = -1
			} else if i > j {
				want = 1
			}
			if got := CompareVersions(order[i], order[j]); got != want {
				t.Errorf("CompareVersions(%q, %q) = %d, want %d", order[i], order[j], got, want)
			}
		}
	}
}

const showConfig = `#
# CORE CONFIGURATION OPTIONS
#
readconfig                1
writeconfig               0

#
# CORE SEARCH PATH OPTIONS
#
homepath                  .
rompath                   roms
hashpath                  hash
samplepath                samples
artpath                   artwork
skip_gameinfo             0
joystick                  1
keyboardprovider          auto
maximize                  1
window                    0
`

func TestParseShowConfig(t *testing.T) {
	o := ParseShowConfig(showConfig)
	for _, k := range []string{"rompath", "skip_gameinfo", "joystick", "keyboardprovider", "maximize", "window"} {
		if !o[k] {
			t.Errorf("%s missing", k)
		}
	}
	if ParseShowConfig("Unknown option: -showconfig\n") != nil {
		t.Error("an error message isn't a list of options")
	}
}

func TestGameArgs(t *testing.T) {
	opts := ParseShowConfig(showConfig)
	got := GameArgs("tmnt", `D:\roms;C:\mame\roms`, opts, LaunchOptions{Embed: true})
	want := []string{"tmnt", "-rompath", `D:\roms;C:\mame\roms`, "-window", "-nomaximize", "-keyboardprovider", "win32", "-skip_gameinfo", "-joystick"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("embedded:\n got %q\nwant %q", got, want)
	}
	got = GameArgs("tmnt", "roms", opts, LaunchOptions{})
	want = []string{"tmnt", "-rompath", "roms", "-skip_gameinfo", "-joystick"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("own window:\n got %q\nwant %q", got, want)
	}
	// an old MAME that didn't list its options gets only what every version has
	got = GameArgs("pacman", "roms", nil, LaunchOptions{Embed: true})
	want = []string{"pacman", "-rompath", "roms", "-window"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("old MAME:\n got %q\nwant %q", got, want)
	}
}

func TestRomPath(t *testing.T) {
	exe := filepath.Join("C", "mame", "mame.exe")
	got := RomPath(exe, filepath.Join("D", "roms"), []string{filepath.Join("D", "roms"), filepath.Join("E", "arcade")})
	want := strings.Join([]string{filepath.Join("D", "roms"), filepath.Join("E", "arcade"), filepath.Join("C", "mame", "roms")}, ";")
	if got != want {
		t.Errorf("RomPath = %q, want %q", got, want)
	}
}

func TestProblem(t *testing.T) {
	out := "tmnt-a.k22 NOT FOUND (tried in tmnt)\r\ntmnt-a.k23 NOT FOUND (tried in tmnt)\r\n" +
		"Fatal error: Required files are missing, the machine cannot be run.\r\n"
	if p := Problem(out); !strings.Contains(p, "missing") || !strings.Contains(p, "tmnt-a.k22") || !strings.Contains(p, "another version") {
		t.Errorf("missing files: %q", p)
	}
	if p := Problem("Unknown system 'tmnt9'\n"); !strings.Contains(p, "doesn't know that game") {
		t.Errorf("unknown game: %q", p)
	}
	if p := Problem(""); p != "" {
		t.Errorf("nothing printed: %q", p)
	}
}

func TestParseListFull(t *testing.T) {
	out := "Name:             Description:\n" +
		"tmnt              \"Teenage Mutant Ninja Turtles (World 4 Players, version X)\"\n" +
		"pacman            \"Pac-Man (Midway)\"\n" +
		"garbage line\n"
	n := ParseListFull(out)
	if len(n) != 2 || n["tmnt"] != "Teenage Mutant Ninja Turtles (World 4 Players, version X)" || n["pacman"] != "Pac-Man (Midway)" {
		t.Errorf("ParseListFull = %v", n)
	}
}

func TestLibrary(t *testing.T) {
	d := t.TempDir()
	write := func(p string) {
		p = filepath.Join(d, p)
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("x"), 0o644)
	}
	write("tmnt.zip")
	write("pacman.7z")
	write("sub/simpsons.zip")
	write("sub/too/deep/galaga.zip")
	write("mario.zip") // a console game in the same folder
	write("readme.txt")
	names := map[string]string{"tmnt": "Teenage Mutant Ninja Turtles", "pacman": "Pac-Man", "simpsons": "The Simpsons", "galaga": "Galaga"}
	played := map[string]time.Time{"pacman": time.Unix(100, 0)}
	games := Library([]string{d}, names, played)
	var got []string
	for _, g := range games {
		got = append(got, g.Short)
	}
	if want := []string{"pacman", "tmnt", "simpsons"}; !reflect.DeepEqual(got, want) {
		t.Errorf("library = %v, want %v", got, want)
	}
	if !games[0].Played.Equal(time.Unix(100, 0)) {
		t.Error("last played lost")
	}
	// without names (no MAME asked yet) every set is listed
	if n := len(Library([]string{d}, nil, nil)); n != 4 {
		t.Errorf("without names: %d games", n)
	}
	if f := Filter(games, "turt", "az"); len(f) != 1 || f[0].Short != "tmnt" {
		t.Errorf("Filter = %v", f)
	}
}

func TestConfig(t *testing.T) {
	t.Setenv("RETRO_MAME_CONFIG", filepath.Join(t.TempDir(), "mame.json"))
	_, err := Update(func(c *Config) {
		c.Add(Install{Exe: `C:\mame0250\mame.exe`, Version: "0.250"})
		c.Add(Install{Exe: `C:\mame0270\mame.exe`, Version: "0.270"})
		c.Add(Install{Exe: `C:\mame0139\mame.exe`, Version: "0.139"})
		c.SetGame("TMNT", `C:\mame0139\mame.exe`)
		c.AddFolder(`D:\roms`)
		c.AddFolder(`d:\ROMS\`)
	})
	if err != nil {
		t.Fatal(err)
	}
	c := Load()
	if c.Installs[0].Version != "0.270" || c.Installs[2].Version != "0.139" {
		t.Errorf("installs not newest first: %v", c.Installs)
	}
	if def, _ := c.DefaultInstall(); def.Version != "0.250" {
		t.Errorf("the first one added stays the default, got %v", def)
	}
	if in, _ := c.For("tmnt"); in.Version != "0.139" {
		t.Errorf("tmnt's own version lost: %v", in)
	}
	if in, _ := c.For("pacman"); in.Version != "0.250" {
		t.Errorf("pacman should use the default: %v", in)
	}
	if len(c.Folders) != 1 {
		t.Errorf("the same folder twice: %v", c.Folders)
	}
	c, _ = Update(func(c *Config) { c.Remove(`C:\mame0139\mame.exe`); c.Remove(`C:\mame0250\mame.exe`) })
	if in, _ := c.For("tmnt"); in.Version != "0.270" || c.Default != `C:\mame0270\mame.exe` {
		t.Errorf("after removing: %v, default %q", in, c.Default)
	}
	if !c.EmbedOn() {
		t.Error("playing inside the window is the default")
	}
}

func TestIsMameExe(t *testing.T) {
	for name, want := range map[string]bool{"mame.exe": true, "MAME64.EXE": true, "mameui64.exe": true, "mame0270b_x64.exe": false,
		"mame0250b_64bit.exe": false, "chdman.exe": false, "mame.ini": false} {
		if IsMameExe(name) != want {
			t.Errorf("IsMameExe(%q) = %v", name, !want)
		}
	}
}

func releasesJSON(base string) string {
	return `[
 {"tag_name":"mame0271","draft":false,"prerelease":false,"published_at":"2024-10-30T00:00:00Z","assets":[
   {"name":"mame0271b_arm64.exe","browser_download_url":"` + base + `/mamedev/mame/releases/download/mame0271/mame0271b_arm64.exe","size":5},
   {"name":"mame0271b_x64.exe","browser_download_url":"` + base + `/mamedev/mame/releases/download/mame0271/mame0271b_x64.exe","size":11,"digest":"sha256:%s"},
   {"name":"mame0271s.exe","browser_download_url":"` + base + `/mamedev/mame/releases/download/mame0271/mame0271s.exe","size":7}]},
 {"tag_name":"mame0250","published_at":"2022-12-31T00:00:00Z","assets":[
   {"name":"mame0250b_64bit.exe","browser_download_url":"` + base + `/mamedev/mame/releases/download/mame0250/mame0250b_64bit.exe","size":11}]},
 {"tag_name":"mame0260","prerelease":true,"assets":[]},
 {"tag_name":"something-else","assets":[]}
]`
}

func TestParseReleases(t *testing.T) {
	r, err := ParseReleases([]byte(fmt.Sprintf(releasesJSON("https://github.com"), strings.Repeat("ab", 32))))
	if err != nil {
		t.Fatal(err)
	}
	if len(r) != 2 || r[0].Version != "0.271" || r[1].Version != "0.250" {
		t.Fatalf("releases = %+v", r)
	}
	if !strings.HasSuffix(r[0].URL, "mame0271b_x64.exe") || r[0].SHA256 != strings.Repeat("ab", 32) || r[1].SHA256 != "" {
		t.Errorf("picked %+v", r[0])
	}
	// links that aren't MAME's own releases are left out
	r, _ = ParseReleases([]byte(fmt.Sprintf(releasesJSON("https://example.com"), "")))
	if len(r) != 0 {
		t.Errorf("foreign links used: %+v", r)
	}
}

func TestInstaller(t *testing.T) {
	payload := []byte("SFX+archive")
	sum := sha256.Sum256(payload)
	var srv *httptest.Server
	srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/releases":
			if r.URL.Query().Get("page") != "1" {
				w.Write([]byte("[]"))
				return
			}
			w.Write([]byte(fmt.Sprintf(releasesJSON(srv.URL), hex.EncodeToString(sum[:]))))
		case strings.HasSuffix(r.URL.Path, ".exe"):
			w.Write(payload)
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()
	dir := t.TempDir()
	var states []string
	in := &Installer{
		API:        srv.URL + "/releases",
		URLOK:      func(s string) bool { return strings.HasPrefix(s, srv.URL+releasePrefix) },
		RedirectOK: func(*url.URL) bool { return true },
		Dirs:       []string{dir},
		Unpack: func(sfx, dest string) error {
			b, _ := os.ReadFile(sfx)
			if string(b) != string(payload) {
				return fmt.Errorf("got %q", b)
			}
			os.MkdirAll(filepath.Join(dest, "mame"), 0o755)
			return os.WriteFile(filepath.Join(dest, "mame", "mame.exe"), []byte("MZ"), 0o644)
		},
		Progress: func(p Progress) { states = append(states, p.State) },
	}
	rel, err := in.Releases()
	if err != nil || len(rel) != 2 {
		t.Fatalf("Releases: %v %v", rel, err)
	}
	exe, err := in.Install(rel[0])
	if err != nil {
		t.Fatal(err)
	}
	if exe != filepath.Join(dir, "MAME 0.271", "mame.exe") {
		t.Errorf("installed at %s", exe)
	}
	if _, err := os.Stat(filepath.Join(dir, "MAME 0.271", "ABOUT-THIS-FOLDER.txt")); err != nil {
		t.Error("no notice next to it")
	}
	if states[len(states)-1] != "installing" {
		t.Errorf("progress %v", states)
	}
	// a wrong checksum is refused
	bad := rel[1]
	bad.SHA256 = strings.Repeat("00", 32)
	if _, err := in.Install(bad); err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Errorf("bad checksum: %v", err)
	}
	// and so is a download link somewhere else
	bad.URL = "https://example.com/mame.exe"
	if _, err := in.Install(bad); err == nil {
		t.Error("foreign link used")
	}
}

func TestFindArchive(t *testing.T) {
	arc, err := os.ReadFile(filepath.Join("..", "..", "sevenzip", "testdata", "lzma2.7z"))
	if err != nil {
		t.Skip(err)
	}
	// an extractor's code ahead of the archive, holding the signature itself
	stub := append([]byte("MZ stub code "), sig7z...)
	stub = append(stub, make([]byte, 3<<20)...) // across more than one read
	sfx := filepath.Join(t.TempDir(), "mame0270b_x64.exe")
	os.WriteFile(sfx, append(stub, arc...), 0o644)
	off, err := FindArchive(sfx)
	if err != nil || off != int64(len(stub)) {
		t.Fatalf("FindArchive = %d, %v; want %d", off, err, len(stub))
	}
	tail := sfx + ".7z"
	if err := copyFrom(sfx, tail, off); err != nil {
		t.Fatal(err)
	}
	if _, err := sevenzip.List(tail); err != nil {
		t.Errorf("the archive found doesn't open: %v", err)
	}
}
