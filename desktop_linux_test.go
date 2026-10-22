package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

const desktopTemplate = "tools/linux/RetroPixels.desktop"

// desktopTemplateHeader comes before the template's [Desktop Entry].
const desktopTemplateHeader = `# RetroPixels' applications menu entry, as RetroPixels writes it.
# Use "Add RetroPixels to the applications menu" in RetroPixels (at the top of
# its window) instead of copying this file: that writes it with the folder
# RetroPixels is in, adds the icon and the game file types, and can remove
# them again. This copy finds RetroPixels only when it is on your PATH.
`

func TestDesktopExecArg(t *testing.T) {
	for _, c := range []struct{ in, want string }{
		{"/opt/RetroPixels/RetroPixels", "/opt/RetroPixels/RetroPixels"},
		{"/home/a b/RetroPixels", `"/home/a b/RetroPixels"`},
		{`/home/x"y/$HOME/a\b/RP`, `"/home/x\"y/\$HOME/a\\b/RP"`},
		{"/g/100%/RetroPixels", "/g/100%%/RetroPixels"},
		{"/g/(new)/R`x`", "\"/g/(new)/R\\`x\\`\""},
	} {
		if got := desktopExecArg(c.in); got != c.want {
			t.Errorf("desktopExecArg(%q) = %q, want %q", c.in, got, c.want)
		}
	}
	// in the file, the string value's own escaping doubles each backslash
	e, err := desktopEntry(`/home/a\b c/RetroPixels`)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(e, "\nExec=\"/home/a\\\\\\\\b c/RetroPixels\" %f\n") || !strings.Contains(e, "\nTryExec=/home/a\\\\b c/RetroPixels\n") {
		t.Errorf("entry:\n%s", e)
	}
	if _, err := desktopEntry("relative/RetroPixels"); err == nil {
		t.Error("a relative path was accepted")
	}
	if _, err := desktopEntry("/a\nb/RetroPixels"); err == nil {
		t.Error("a path with a newline was accepted")
	}
}

// The template in tools/linux is desktopEntry("") with its comment, and the
// icon shipped next to it is the app's own icon.
func TestDesktopTemplate(t *testing.T) {
	want, err := desktopEntry("")
	if err != nil {
		t.Fatal(err)
	}
	want = desktopTemplateHeader + want
	if os.Getenv("RETRO_WRITE_DESKTOP") == "1" {
		if err := os.WriteFile(desktopTemplate, []byte(want), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	got, err := os.ReadFile(desktopTemplate)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != want {
		t.Errorf("%s is out of date (RETRO_WRITE_DESKTOP=1 go test -run TestDesktopTemplate writes it):\n%s", desktopTemplate, want)
	}
	icon, err := os.ReadFile("tools/linux/retropixels.png")
	if err != nil {
		t.Fatal(err)
	}
	app, _ := os.ReadFile("web/icon-512.png")
	if !bytes.Equal(icon, app) {
		t.Error("tools/linux/retropixels.png isn't web/icon-512.png")
	}
	if strings.Contains(want, desktopMarker) {
		t.Error("the template has the marker of an entry RetroPixels wrote")
	}
}

// Every file type the page's "Choose game file" offers has a MIME type in
// the entry, apart from the ones left out on purpose.
func TestDesktopMimeCoversPage(t *testing.T) {
	page, err := os.ReadFile("web/index.html")
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`id="fileInput"[^>]*accept="([^"]+)"`).FindSubmatch(page)
	if m == nil {
		t.Fatal("no fileInput accept list in the page")
	}
	exts := strings.Split(string(m[1]), ",")
	exts = append(exts, ".iso", ".gcm", ".ciso", ".wbfs", ".rvz", ".wia", ".gcz", ".wad", ".wua", ".wud", ".wux", ".rpx")
	// standard types, by the globs freedesktop.org's shared-mime-info gives them
	std := map[string]string{
		".nes": "application/x-nes-rom", ".unf": "application/x-nes-rom", ".unif": "application/x-nes-rom",
		".fds": "application/x-fds-disk", ".sfc": "application/vnd.nintendo.snes.rom", ".smc": "application/vnd.nintendo.snes.rom",
		".gb": "application/x-gameboy-rom", ".gbc": "application/x-gameboy-color-rom", ".gba": "application/x-gba-rom", ".agb": "application/x-gba-rom",
		".z64": "application/x-n64-rom", ".n64": "application/x-n64-rom", ".v64": "application/x-n64-rom",
		".gen": "application/x-genesis-rom", ".smd": "application/x-genesis-rom", ".sms": "application/x-sms-rom",
		".a26": "application/x-atari-2600-rom", ".a78": "application/x-atari-7800-rom",
		".iso": "application/x-gamecube-rom", ".wad": "application/x-wii-wad",
	}
	skip := map[string]bool{".md": true, ".bin": true, ".zip": true}
	listed := map[string]bool{}
	for _, t := range desktopMimeTypes() {
		listed[t] = true
	}
	own := map[string]string{}
	for _, o := range desktopOwnTypes() {
		for _, g := range o.Globs {
			own[strings.TrimPrefix(g, "*")] = o.Type
		}
	}
	for _, e := range exts {
		e = strings.TrimSpace(e)
		switch {
		case skip[e]:
		case std[e] != "":
			if !listed[std[e]] {
				t.Errorf("%s: %s isn't in MimeType", e, std[e])
			}
			if own[e] != "" {
				t.Errorf("%s has a standard type and one of RetroPixels' own", e)
			}
		case own[e] != "":
		default:
			t.Errorf("%s has no MIME type", e)
		}
	}
	if os.Getenv("RETRO_SYSTEM_MIME") == "" {
		return
	}
	// RETRO_SYSTEM_MIME=1: check the standard globs against this system's database
	db, err := os.ReadFile("/usr/share/mime/packages/freedesktop.org.xml")
	if err != nil {
		t.Skip(err)
	}
	for e, typ := range std {
		re := regexp.MustCompile(`(?s)<mime-type type="` + regexp.QuoteMeta(typ) + `">(.*?)</mime-type>`)
		b := re.FindSubmatch(db)
		if b == nil || (e != ".iso" && !bytes.Contains(b[1], []byte(`<glob pattern="*`+e+`"`))) {
			t.Errorf("%s isn't %s in this system's shared-mime-info", e, typ)
		}
	}
}

func TestDesktopMimeXML(t *testing.T) {
	var doc struct {
		XMLName xml.Name `xml:"mime-info"`
		Types   []struct {
			Type    string `xml:"type,attr"`
			Comment string `xml:"comment"`
			Globs   []struct {
				Pattern string `xml:"pattern,attr"`
			} `xml:"glob"`
		} `xml:"mime-type"`
	}
	if err := xml.Unmarshal([]byte(desktopMimeXML()), &doc); err != nil {
		t.Fatal(err)
	}
	if doc.XMLName.Space != "http://www.freedesktop.org/standards/shared-mime-info" || len(doc.Types) != len(desktopOwnTypes()) {
		t.Fatalf("%+v", doc)
	}
	seen := map[string]bool{}
	for _, ty := range doc.Types {
		if ty.Comment == "" || len(ty.Globs) == 0 {
			t.Errorf("%s: no comment or globs", ty.Type)
		}
		for _, g := range ty.Globs {
			if seen[g.Pattern] {
				t.Errorf("%s twice", g.Pattern)
			}
			seen[g.Pattern] = true
		}
	}
	if !seen["*.apsm"] || !seen["*.wbfs"] || !seen["*.wua"] {
		t.Error("types missing")
	}
}

func TestGameArgument(t *testing.T) {
	d := t.TempDir()
	g := filepath.Join(d, "a game.sfc")
	if err := os.WriteFile(g, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		args []string
		want string
	}{
		{nil, ""},
		{[]string{"--flag", g}, g},
		{[]string{"file://" + g}, g},
		{[]string{d}, ""},                      // a folder
		{[]string{filepath.Join(d, "no")}, ""}, // not there
	} {
		if got := gameArgument(c.args); got != c.want {
			t.Errorf("gameArgument(%q) = %q, want %q", c.args, got, c.want)
		}
	}
	for p, want := range map[string]string{"/g/x.ISO": "dolphin", "/g/x.wbfs": "dolphin", "/g/code/x.rpx": "pixelu", "/g/x.wua": "pixelu", "/g/x.sfc": "page", "/g/x.apsm": "page"} {
		if k, _ := openKind(p); k != want {
			t.Errorf("openKind(%s) = %s, want %s", p, k, want)
		}
	}
	if _, wii := openKind("/g/x.wad"); !wii {
		t.Error(".wad isn't Wii")
	}
}

// desktopTestHome points the XDG folders at a temporary one.
func desktopTestHome(t *testing.T) string {
	d := t.TempDir()
	t.Setenv("XDG_DATA_HOME", filepath.Join(d, "share"))
	return filepath.Join(d, "share")
}

func TestInstallDesktopMenu(t *testing.T) {
	data := desktopTestHome(t)
	exe := "/opt/Retro Pixels/RetroPixels"
	if err := installDesktopMenu(exe); err != nil {
		t.Fatal(err)
	}
	entry := filepath.Join(data, "applications", desktopFileName)
	files := []string{
		entry,
		filepath.Join(data, "mime", "packages", desktopMimeFile),
		filepath.Join(data, "icons", "hicolor", "256x256", "apps", "retropixels.png"),
		filepath.Join(data, "icons", "hicolor", "512x512", "apps", "retropixels.png"),
	}
	for _, f := range files {
		if _, err := os.Stat(f); err != nil {
			t.Error(err)
		}
	}
	icon, _ := os.ReadFile(files[2])
	app, _ := os.ReadFile("web/icon-256.png")
	if !bytes.Equal(icon, app) {
		t.Error("256 icon isn't web/icon-256.png")
	}
	if got, ours := desktopInstalledExec(); !ours || got != exe {
		t.Errorf("installed exec = %q, %v", got, ours)
	}
	if p, err := exec.LookPath("desktop-file-validate"); err == nil {
		if out, err := exec.Command(p, entry).CombinedOutput(); err != nil || len(bytes.TrimSpace(out)) > 0 {
			t.Errorf("desktop-file-validate: %v\n%s", err, out)
		}
	}
	if _, err := exec.LookPath("update-desktop-database"); err == nil {
		cache, err := os.ReadFile(filepath.Join(data, "applications", "mimeinfo.cache"))
		if err != nil || !bytes.Contains(cache, []byte("application/x-retropixels-wiiu-game="+desktopFileName)) {
			t.Errorf("mimeinfo.cache: %v\n%s", err, cache)
		}
	}
	if _, err := exec.LookPath("update-mime-database"); err == nil {
		globs, err := os.ReadFile(filepath.Join(data, "mime", "globs2"))
		if err != nil || !bytes.Contains(globs, []byte("application/x-retropixels-gamecube-wii-disc:*.rvz")) {
			t.Errorf("globs2: %v\n%s", err, globs)
		}
	}

	if err := removeDesktopMenu(); err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		if _, err := os.Stat(f); !os.IsNotExist(err) {
			t.Errorf("%s still there", f)
		}
	}
	if _, ours := desktopInstalledExec(); ours {
		t.Error("still installed")
	}
	if _, err := exec.LookPath("update-mime-database"); err == nil {
		globs, _ := os.ReadFile(filepath.Join(data, "mime", "globs2"))
		if bytes.Contains(globs, []byte("retropixels")) {
			t.Error("the file types are still known")
		}
	}

	// someone else's retropixels.desktop is never removed
	_ = os.MkdirAll(filepath.Dir(entry), 0o755)
	_ = os.WriteFile(entry, []byte("[Desktop Entry]\nType=Application\nName=Mine\nExec=mine\n"), 0o644)
	if err := removeDesktopMenu(); err == nil {
		t.Error("removed an entry RetroPixels didn't write")
	}
	if _, err := os.Stat(entry); err != nil {
		t.Error(err)
	}
	refreshDesktopMenu() // not ours: left alone
	if b, _ := os.ReadFile(entry); !strings.Contains(string(b), "Name=Mine") {
		t.Error("refresh rewrote someone else's entry")
	}
}

func TestRefreshDesktopMenu(t *testing.T) {
	desktopTestHome(t)
	if err := installDesktopMenu("/old/build/RetroPixels"); err != nil {
		t.Fatal(err)
	}
	refreshDesktopMenu()
	self, _ := selfExe()
	if got, _ := desktopInstalledExec(); got != self {
		t.Errorf("after refresh: %q, want %q", got, self)
	}
}

func TestDesktopMenuRoute(t *testing.T) {
	desktopTestHome(t)
	token = "tok"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	call := func(method, body string) map[string]any {
		req, _ := http.NewRequest(method, srv.URL+"/api/desktop/menu", strings.NewReader(body))
		req.Host = fmt.Sprintf("127.0.0.1:%d", port)
		req.Header.Set("X-Retro-Token", token)
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer res.Body.Close()
		var v map[string]any
		_ = json.NewDecoder(res.Body).Decode(&v)
		return v
	}
	if v := call("GET", ""); v["installed"] != false {
		t.Errorf("%v", v)
	}
	if v := call("POST", `{"action":"add"}`); v["installed"] != true {
		t.Errorf("%v", v)
	}
	if v := call("POST", `{"action":"remove"}`); v["installed"] != false {
		t.Errorf("%v", v)
	}
	if v := call("POST", `{"action":"x"}`); v["error"] == nil {
		t.Errorf("%v", v)
	}
}

func TestOpenHandOff(t *testing.T) {
	desktopTestHome(t)
	run := t.TempDir()
	_ = os.Chmod(run, 0o700)
	t.Setenv("XDG_RUNTIME_DIR", run)
	listenOpenSocket()
	if fi, err := os.Stat(openSocketPath()); err != nil || fi.Mode().Perm() != 0o600 {
		t.Fatalf("socket: %v %v", fi, err)
	}

	d := t.TempDir()
	rom := filepath.Join(d, "Game (USA).nes")
	_ = os.WriteFile(rom, []byte("NES\x1aROM"), 0o644)
	iso := filepath.Join(d, "Game.iso")
	_ = os.WriteFile(iso, []byte("disc"), 0o644)

	q := desktopHandOff([]string{rom})
	id, ok := strings.CutPrefix(q, "?open=")
	if !ok {
		t.Fatalf("hand-off: %q", q)
	}
	// not a file: refused
	c, err := net.Dial("unix", openSocketPath())
	if err != nil {
		t.Fatal(err)
	}
	fmt.Fprintf(c, "open %s\n", d)
	line, _ := bufio.NewReader(c).ReadString('\n')
	c.Close()
	if line != "error\n" {
		t.Errorf("folder: %q", line)
	}

	token = "tok"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	get := func(p string, tok string) *http.Response {
		req, _ := http.NewRequest("GET", srv.URL+p, nil)
		req.Host = fmt.Sprintf("127.0.0.1:%d", port)
		req.Header.Set("X-Retro-Token", tok)
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		return res
	}
	if res := get("/api/desktop/open?id="+id, "wrong"); res.StatusCode != 403 {
		t.Errorf("without the token: %d", res.StatusCode)
	}
	res := get("/api/desktop/open?id="+id, token)
	var g map[string]any
	_ = json.NewDecoder(res.Body).Decode(&g)
	if g["kind"] != "page" || g["name"] != "Game (USA).nes" {
		t.Errorf("%v", g)
	}
	res = get("/api/desktop/file?id="+id, token)
	b, _ := io.ReadAll(res.Body)
	if string(b) != "NES\x1aROM" {
		t.Errorf("file: %d %q", res.StatusCode, b)
	}
	if res := get("/api/desktop/file?id="+id, token); res.StatusCode != 404 {
		t.Errorf("served twice: %d", res.StatusCode)
	}

	// the first copy: its own window opens the game; engines' games aren't served as files
	q = desktopStart([]string{iso})
	id = strings.TrimPrefix(q, "?open=")
	res = get("/api/desktop/open?id="+id, token)
	g = nil
	_ = json.NewDecoder(res.Body).Decode(&g)
	if g["kind"] != "dolphin" || g["path"] != iso {
		t.Errorf("%v", g)
	}
	if res := get("/api/desktop/file?id="+id, token); res.StatusCode != 404 {
		t.Errorf("disc served: %d", res.StatusCode)
	}
	if desktopStart(nil) != "" || desktopHandOff(nil) != "" {
		t.Error("a query without a game")
	}
}
