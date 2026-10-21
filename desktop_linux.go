package main

// Desktop integration on Linux: the applications menu entry, the icon, and
// game files opened from the file manager.
//
// "Add RetroPixels to the applications menu" (a button in the page) writes,
// per user and without root:
//
//   - $XDG_DATA_HOME/applications/retropixels.desktop, with this program's
//     path, the game file types (MimeType) and StartupWMClass=RetroPixels
//     (the app window's --class, so the menu's icon is used for the window);
//   - the app icon (web/icon-256.png and web/icon-512.png, built in) in
//     $XDG_DATA_HOME/icons/hicolor/<size>/apps/retropixels.png;
//   - $XDG_DATA_HOME/mime/packages/retropixels.xml, the file types that have
//     no standard MIME type (.wbfs, .rvz, .wua, Archipelago patches...),
//
// then runs update-desktop-database and update-mime-database when they are
// installed. "Remove from the applications menu" deletes those files again.
// Like the Windows build's file types (cmd/retropixels-native/assoc_windows.go)
// this never makes RetroPixels the default for a file type: it is offered in
// "Open With", and the player picks it there once. A new build folder that is
// started takes the entry over (its path is written again).
//
// A game opened from the file manager starts RetroPixels with its path. The
// first copy opens its window as /?open=<id>; a second copy hands the path to
// the first over a Unix socket in $XDG_RUNTIME_DIR (only this user can reach
// it) and opens a window with the id it got back. The page then asks for the
// game: GameCube, Wii and Wii U games start in their engines by path, every
// other file is read by the page as if it had been dropped on it.

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"retropixels/internal/apatch"
	"retropixels/internal/dolphin"
)

const (
	desktopFileName = "retropixels.desktop"
	desktopIconName = "retropixels"
	desktopMimeFile = "retropixels.xml"
	// desktopMarker is in every entry RetroPixels writes, so it only ever
	// rewrites or removes its own file.
	desktopMarker = "X-RetroPixels-Version"
)

// desktopIconSizes: the built-in icons copied into the hicolor theme.
var desktopIconSizes = []int{256, 512}

// desktopStdMime are the standard MIME types (freedesktop.org's
// shared-mime-info) of the game files RetroPixels opens. Every desktop has
// them, so they need no file of ours. GameCube and Wii .iso files are told
// apart from other .iso files by their contents.
var desktopStdMime = []string{
	"application/x-nes-rom",             // .nes .unf .unif
	"application/x-fds-disk",            // .fds
	"application/vnd.nintendo.snes.rom", // .sfc .smc
	"application/x-snes-rom",            // its older name
	"application/x-gameboy-rom",         // .gb
	"application/x-gameboy-color-rom",   // .gbc
	"application/x-gba-rom",             // .gba .agb
	"application/x-n64-rom",             // .n64 .z64 .v64
	"application/x-genesis-rom",         // .gen .smd
	"application/x-sms-rom",             // .sms
	"application/x-atari-2600-rom",      // .a26
	"application/x-atari-7800-rom",      // .a78
	"application/x-gamecube-rom",        // GameCube .iso
	"application/x-wii-rom",             // Wii .iso
	"application/x-wii-wad",             // .wad
}

// desktopOwnType is a file type RetroPixels describes itself (in
// retropixels.xml) because shared-mime-info has none for it.
type desktopOwnType struct {
	Type, Comment string
	Globs         []string // "*.wbfs"
}

// desktopOwnTypes: game files without a standard MIME type. Not here on
// purpose: .md (Genesis, but every desktop takes .md for Markdown), .bin,
// .zip, .7z and .elf, which are far more often something else; those still
// open from the page and by drag and drop.
func desktopOwnTypes() []desktopOwnType {
	var ap []string
	for e := range apatch.SystemForPatch {
		ap = append(ap, "*"+e)
	}
	sort.Strings(ap)
	return []desktopOwnType{
		{"application/x-retropixels-snes-rom", "Super NES ROM (copier format)", []string{"*.fig", "*.swc"}},
		{"application/x-retropixels-gamecube-wii-disc", "GameCube or Wii disc image", []string{"*.ciso", "*.gcm", "*.gcz", "*.rvz", "*.wbfs", "*.wia"}},
		{"application/x-retropixels-wiiu-game", "Wii U game", []string{"*.rpx", "*.wua", "*.wud", "*.wux"}},
		{"application/x-retropixels-archipelago-patch", "Archipelago patch", ap},
	}
}

// desktopMimeTypes: the .desktop file's MimeType list.
func desktopMimeTypes() []string {
	out := append([]string(nil), desktopStdMime...)
	for _, t := range desktopOwnTypes() {
		out = append(out, t.Type)
	}
	return out
}

// desktopMimeXML is retropixels.xml, a shared-mime-info package.
func desktopMimeXML() string {
	var b strings.Builder
	b.WriteString(`<?xml version="1.0" encoding="UTF-8"?>` + "\n")
	b.WriteString(`<!-- Written by RetroPixels ("Add RetroPixels to the applications menu"); removed again by "Remove from the applications menu". -->` + "\n")
	b.WriteString(`<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">` + "\n")
	for _, t := range desktopOwnTypes() {
		fmt.Fprintf(&b, "  <mime-type type=%q>\n    <comment>%s</comment>\n", t.Type, t.Comment)
		for _, g := range t.Globs {
			fmt.Fprintf(&b, "    <glob pattern=%q/>\n", g)
		}
		b.WriteString("  </mime-type>\n")
	}
	b.WriteString("</mime-info>\n")
	return b.String()
}

// desktopExecArg quotes one argument of a .desktop Exec line (Desktop Entry
// Specification, "The Exec key"): an argument with a reserved character is
// put in double quotes, with ", `, $ and \ escaped by a backslash; a literal
// % is written %%.
func desktopExecArg(s string) string {
	s = strings.ReplaceAll(s, "%", "%%")
	if !strings.ContainsAny(s, " \t\"'\\><~|&;$*?#()`") {
		return s
	}
	r := strings.NewReplacer(`"`, `\"`, "`", "\\`", `$`, `\$`, `\`, `\\`)
	return `"` + r.Replace(s) + `"`
}

// desktopString escapes a value of the string type (backslash is the escape
// character in .desktop values, so it is written twice).
func desktopString(s string) string { return strings.ReplaceAll(s, `\`, `\\`) }

// desktopEntry is retropixels.desktop for the program at exe. With exe ""
// it is the template shipped in the download (tools/linux/RetroPixels.desktop),
// which names the program without a folder.
func desktopEntry(exe string) (string, error) {
	execArg, tryExec, ver := "RetroPixels", "RetroPixels", ""
	if exe != "" {
		if !filepath.IsAbs(exe) || strings.ContainsAny(exe, "\n\r") {
			return "", fmt.Errorf("RetroPixels can't add itself to the menu from %q", exe)
		}
		execArg, tryExec = desktopString(desktopExecArg(exe)), desktopString(exe)
		ver = desktopMarker + "=" + appVersion + "\n"
	}
	return "[Desktop Entry]\n" +
		"Type=Application\n" +
		"Name=RetroPixels\n" +
		"GenericName=Game Emulator\n" +
		"Comment=Play classic console, GameCube, Wii, Wii U and arcade games\n" +
		"Icon=" + desktopIconName + "\n" +
		"TryExec=" + tryExec + "\n" +
		"Exec=" + execArg + " %f\n" +
		"Terminal=false\n" +
		"Categories=Game;Emulator;\n" +
		"Keywords=emulator;retro;NES;SNES;Game Boy;N64;Genesis;GameCube;Wii;Wii U;arcade;MAME;\n" +
		"MimeType=" + strings.Join(desktopMimeTypes(), ";") + ";\n" +
		"StartupWMClass=" + appName + "\n" +
		ver, nil
}

// xdgDataHome is $XDG_DATA_HOME, else ~/.local/share.
func xdgDataHome() (string, error) {
	if d := os.Getenv("XDG_DATA_HOME"); filepath.IsAbs(d) {
		return d, nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", errors.New("RetroPixels can't find your home folder")
	}
	return filepath.Join(home, ".local", "share"), nil
}

type desktopFiles struct {
	data, entry, mime string
	icons             map[int]string
}

func desktopPaths() (desktopFiles, error) {
	d, err := xdgDataHome()
	if err != nil {
		return desktopFiles{}, err
	}
	f := desktopFiles{
		data:  d,
		entry: filepath.Join(d, "applications", desktopFileName),
		mime:  filepath.Join(d, "mime", "packages", desktopMimeFile),
		icons: map[int]string{},
	}
	for _, s := range desktopIconSizes {
		f.icons[s] = filepath.Join(d, "icons", "hicolor", fmt.Sprintf("%dx%d", s, s), "apps", desktopIconName+".png")
	}
	return f, nil
}

// desktopInstalledExec: the program path of RetroPixels' own menu entry,
// and whether there is one.
func desktopInstalledExec() (string, bool) {
	f, err := desktopPaths()
	if err != nil {
		return "", false
	}
	b, err := os.ReadFile(f.entry)
	if err != nil || !strings.Contains(string(b), "\n"+desktopMarker+"=") {
		return "", false
	}
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, "TryExec="); ok {
			return strings.ReplaceAll(v, `\\`, `\`), true
		}
	}
	return "", true
}

func writeFileAtomic(p string, b []byte, mode os.FileMode) error {
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, b, mode); err != nil {
		return err
	}
	if err := os.Rename(tmp, p); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// desktopRun runs a desktop database tool when it is installed; without it
// the desktop finds the files on its own a little later (or at next login).
func desktopRun(tool string, args ...string) {
	p, err := exec.LookPath(tool)
	if err != nil {
		return
	}
	cmd := exec.Command(p, args...)
	done := make(chan struct{})
	go func() { _ = cmd.Run(); close(done) }()
	select {
	case <-done:
	case <-time.After(30 * time.Second):
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
		}
	}
}

func desktopUpdateDatabases(f desktopFiles) {
	desktopRun("update-desktop-database", "-q", filepath.Dir(f.entry))
	desktopRun("update-mime-database", filepath.Join(f.data, "mime"))
	// as xdg-icon-resource does: a user icon folder only gets a cache when
	// it is a full theme (an old cache would hide the new icon)
	theme := filepath.Join(f.data, "icons", "hicolor")
	if _, err := os.Stat(filepath.Join(theme, "index.theme")); err == nil {
		desktopRun("gtk-update-icon-cache", "-q", "-t", "-f", theme)
	}
}

// selfExe: this program's path, symlinks resolved.
func selfExe() (string, error) {
	exe, err := os.Executable()
	if err != nil {
		return "", err
	}
	if p, err := filepath.EvalSymlinks(exe); err == nil {
		exe = p
	}
	return exe, nil
}

// installDesktopMenu adds RetroPixels (the program at exe) to the
// applications menu, with its icon and file types.
func installDesktopMenu(exe string) error {
	f, err := desktopPaths()
	if err != nil {
		return err
	}
	entry, err := desktopEntry(exe)
	if err != nil {
		return err
	}
	sub, _ := fs.Sub(webFS, "web")
	for size, p := range f.icons {
		b, err := fs.ReadFile(sub, fmt.Sprintf("icon-%d.png", size))
		if err != nil {
			return err
		}
		if err := writeFileAtomic(p, b, 0o644); err != nil {
			return fmt.Errorf("couldn't save the icon: %v", err)
		}
	}
	if err := writeFileAtomic(f.mime, []byte(desktopMimeXML()), 0o644); err != nil {
		return fmt.Errorf("couldn't save the game file types: %v", err)
	}
	if err := writeFileAtomic(f.entry, []byte(entry), 0o644); err != nil {
		return fmt.Errorf("couldn't add RetroPixels to the menu: %v", err)
	}
	desktopUpdateDatabases(f)
	return nil
}

// removeDesktopMenu takes out everything installDesktopMenu wrote. A menu
// entry someone else wrote under the same name is left alone.
func removeDesktopMenu() error {
	f, err := desktopPaths()
	if err != nil {
		return err
	}
	if _, ours := desktopInstalledExec(); !ours {
		if _, err := os.Stat(f.entry); err == nil {
			return fmt.Errorf("%s wasn't written by RetroPixels, so it was left as it is", f.entry)
		}
	}
	var errs []string
	for _, p := range append([]string{f.entry, f.mime}, iconPaths(f)...) {
		if err := os.Remove(p); err != nil && !errors.Is(err, fs.ErrNotExist) {
			errs = append(errs, err.Error())
		}
	}
	desktopUpdateDatabases(f)
	if len(errs) > 0 {
		return errors.New("couldn't remove everything: " + strings.Join(errs, "; "))
	}
	return nil
}

func iconPaths(f desktopFiles) []string {
	var out []string
	for _, s := range desktopIconSizes {
		out = append(out, f.icons[s])
	}
	return out
}

// refreshDesktopMenu: once RetroPixels is in the menu, a build started from
// another folder (a newer download) writes its own path there, as the
// Windows build does with its file types.
func refreshDesktopMenu() {
	old, ours := desktopInstalledExec()
	if !ours {
		return
	}
	exe, err := selfExe()
	if err != nil || exe == old {
		return
	}
	_ = installDesktopMenu(exe)
}

// ---------------- games opened from the file manager ----------------

type openRequest struct {
	Path string
	at   time.Time
}

var (
	openMu      sync.Mutex
	openPending = map[string]*openRequest{}
	openTTL     = 10 * time.Minute
)

// gameArgument is the game file the program was started with (Exec's %f),
// as an absolute path, or "".
func gameArgument(args []string) string {
	for _, a := range args {
		if a == "" || strings.HasPrefix(a, "-") {
			continue
		}
		a = strings.TrimPrefix(a, "file://") // a launcher that passes %u anyway
		if abs, err := filepath.Abs(a); err == nil {
			a = abs
		}
		if st, err := os.Stat(a); err == nil && st.Mode().IsRegular() {
			return a
		}
	}
	return ""
}

// queueOpen keeps a game for the page to ask for and returns its id.
func queueOpen(path string) string {
	b := make([]byte, 12)
	_, _ = rand.Read(b)
	id := hex.EncodeToString(b)
	openMu.Lock()
	defer openMu.Unlock()
	for k, r := range openPending {
		if time.Since(r.at) > openTTL {
			delete(openPending, k)
		}
	}
	openPending[id] = &openRequest{Path: path, at: time.Now()}
	return id
}

func pendingOpen(id string, take bool) (string, bool) {
	openMu.Lock()
	defer openMu.Unlock()
	r := openPending[id]
	if r == nil || time.Since(r.at) > openTTL {
		return "", false
	}
	if take {
		delete(openPending, id)
	}
	return r.Path, true
}

func openQuery(id string) string {
	if id == "" {
		return ""
	}
	return "?open=" + id
}

// openSocketPath is where the first copy listens for games from later ones:
// $XDG_RUNTIME_DIR, which only this user can open.
func openSocketPath() string {
	d := os.Getenv("XDG_RUNTIME_DIR")
	if !filepath.IsAbs(d) {
		return ""
	}
	if st, err := os.Stat(d); err != nil || !st.IsDir() || st.Mode().Perm()&0o077 != 0 {
		return ""
	}
	return filepath.Join(d, fmt.Sprintf("retropixels-%d.sock", port))
}

func listenOpenSocket() {
	p := openSocketPath()
	if p == "" {
		return
	}
	_ = os.Remove(p) // a socket left by a copy that crashed (this copy has the port, so it's the only one)
	ln, err := net.Listen("unix", p)
	if err != nil {
		return
	}
	_ = os.Chmod(p, 0o600)
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go serveOpenConn(c)
		}
	}()
}

// serveOpenConn: "open <path>\n" is answered "ok <id>\n".
func serveOpenConn(c net.Conn) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(5 * time.Second))
	line, err := bufio.NewReader(c).ReadString('\n')
	if err != nil {
		return
	}
	p, ok := strings.CutPrefix(strings.TrimSuffix(line, "\n"), "open ")
	if !ok || !filepath.IsAbs(p) {
		fmt.Fprint(c, "error\n")
		return
	}
	if st, err := os.Stat(p); err != nil || !st.Mode().IsRegular() {
		fmt.Fprint(c, "error\n")
		return
	}
	fmt.Fprintf(c, "ok %s\n", queueOpen(p))
}

// handOpenToRunningCopy gives the game to the copy that is already running
// and returns the id it keeps it under ("" when that didn't work).
func handOpenToRunningCopy(path string) string {
	p := openSocketPath()
	if p == "" || path == "" {
		return ""
	}
	c, err := net.DialTimeout("unix", p, 2*time.Second)
	if err != nil {
		return ""
	}
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := fmt.Fprintf(c, "open %s\n", path); err != nil {
		return ""
	}
	line, _ := bufio.NewReader(c).ReadString('\n')
	id, ok := strings.CutPrefix(strings.TrimSpace(line), "ok ")
	if !ok || len(id) != 24 {
		return ""
	}
	return id
}

// desktopHandOff runs in a second copy (one is already running): the URL
// query for its window, which opens the game it was started with.
func desktopHandOff(args []string) string {
	return openQuery(handOpenToRunningCopy(gameArgument(args)))
}

// desktopStart runs in the first copy once its server is up: it listens for
// games from later copies, keeps the menu entry current and returns the URL
// query for its window.
func desktopStart(args []string) string {
	listenOpenSocket()
	go refreshDesktopMenu()
	if g := gameArgument(args); g != "" {
		return openQuery(queueOpen(g))
	}
	return ""
}

// openKind: where a game opened from the file manager plays. "dolphin"
// (GameCube and Wii, wii true for the Wii-only types) and "pixelu" (Wii U)
// start their engines by path; "page" files are read by the page.
func openKind(p string) (kind string, wii bool) {
	ext := strings.ToLower(filepath.Ext(p))
	switch {
	case dolphin.Exts[ext]:
		return "dolphin", ext == ".wbfs" || ext == ".wad"
	case ext == ".wua" || ext == ".wud" || ext == ".wux" || ext == ".rpx":
		return "pixelu", false
	}
	return "page", false
}

func registerDesktopRoutes(mux *http.ServeMux) {
	// GET: is RetroPixels in the menu. POST {action: "add"|"remove"}.
	mux.HandleFunc("/api/desktop/menu", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			var b struct {
				Action string `json:"action"`
			}
			if !readBody(w, r, &b) {
				return
			}
			var err error
			switch b.Action {
			case "add":
				var exe string
				if exe, err = selfExe(); err == nil {
					err = installDesktopMenu(exe)
				}
			case "remove":
				err = removeDesktopMenu()
			default:
				err = errors.New("unknown action")
			}
			if err != nil {
				writeJSON(w, 500, map[string]any{"error": err.Error()})
				return
			}
		}
		_, ours := desktopInstalledExec()
		writeJSON(w, 200, map[string]any{"installed": ours})
	}))

	// The game a window was opened with (?open=<id>).
	mux.HandleFunc("/api/desktop/open", guard(func(w http.ResponseWriter, r *http.Request) {
		id := r.URL.Query().Get("id")
		p, ok := pendingOpen(id, false)
		if !ok {
			writeJSON(w, 404, map[string]any{"error": "That game was opened a while ago — open it again"})
			return
		}
		kind, wii := openKind(p)
		if kind != "page" {
			pendingOpen(id, true)
		}
		writeJSON(w, 200, map[string]any{"name": filepath.Base(p), "path": p, "kind": kind, "wii": wii})
	}))

	// The file itself, once, for a game the page plays.
	mux.HandleFunc("/api/desktop/file", guard(func(w http.ResponseWriter, r *http.Request) {
		p, ok := pendingOpen(r.URL.Query().Get("id"), true)
		if !ok {
			http.NotFound(w, r)
			return
		}
		if k, _ := openKind(p); k != "page" {
			http.NotFound(w, r)
			return
		}
		f, err := os.Open(p)
		if err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		defer f.Close()
		st, err := f.Stat()
		if err != nil || !st.Mode().IsRegular() {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Cache-Control", "no-store")
		http.ServeContent(w, r, "", st.ModTime(), f)
	}))
}
