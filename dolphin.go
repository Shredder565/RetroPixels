package main

// GameCube & Wii support through Dolphin.

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
)

// ---------------- disc identification ----------------

type discInfo struct {
	Platform string // "GameCube", "Wii", "WiiWare"
	ID       string // e.g. GMSE01
	Title    string // name stored on the disc
}

var (
	gcMagic  = []byte{0xC2, 0x33, 0x9F, 0x3D} // at 0x1C
	wiiMagic = []byte{0x5D, 0x1C, 0x9E, 0xA3} // at 0x18
)

func parseDiscHeader(h []byte) (discInfo, bool) {
	if len(h) < 0x60 {
		return discInfo{}, false
	}
	var p string
	switch {
	case bytes.Equal(h[0x18:0x1C], wiiMagic):
		p = "Wii"
	case bytes.Equal(h[0x1C:0x20], gcMagic):
		p = "GameCube"
	default:
		return discInfo{}, false
	}
	id := strings.TrimRight(string(h[0:6]), "\x00 ")
	end := 0x20 + 0x40
	if len(h) < end {
		end = len(h)
	}
	name := h[0x20:end]
	if i := bytes.IndexByte(name, 0); i >= 0 {
		name = name[:i]
	}
	return discInfo{Platform: p, ID: id, Title: strings.TrimSpace(latin1(name))}, true
}

func latin1(b []byte) string {
	r := make([]rune, len(b))
	for i, c := range b {
		r[i] = rune(c)
	}
	return string(r)
}

// identifyDisc reads just enough of a file to tell whether it's a GameCube or Wii game.
func identifyDisc(path string) (discInfo, bool) {
	ext := strings.ToLower(filepath.Ext(path))
	f, err := os.Open(path)
	if err != nil {
		return discInfo{}, false
	}
	defer f.Close()
	read := func(off int64, n int) []byte {
		b := make([]byte, n)
		k, _ := f.ReadAt(b, off)
		return b[:k]
	}
	switch ext {
	case ".iso", ".gcm":
		return parseDiscHeader(read(0, 0x60))
	case ".ciso":
		if string(read(0, 4)) == "CISO" {
			return parseDiscHeader(read(0x8000, 0x60))
		}
	case ".wbfs":
		if string(read(0, 4)) == "WBFS" {
			sec := read(8, 1)
			off := int64(0x200)
			if len(sec) == 1 && sec[0] >= 9 && sec[0] <= 16 {
				off = 1 << sec[0]
			}
			return parseDiscHeader(read(off, 0x60))
		}
	case ".rvz", ".wia":
		// WIA/RVZ keep a copy of the disc header at 0x58 of their second header.
		h := read(0, 0x48+0x80)
		if len(h) >= 0x48 && (string(h[0:3]) == "WIA" || string(h[0:3]) == "RVZ") {
			return parseDiscHeader(read(0x48+0x10, 0x80))
		}
	case ".gcz":
		if len(read(0, 4)) == 4 {
			return gczHeader(f)
		}
	case ".wad":
		return wadInfo(f, path)
	}
	return discInfo{}, false
}

// .gcz is zlib-compressed in blocks; the first block holds the disc header.
func gczHeader(f *os.File) (discInfo, bool) {
	b := make([]byte, 32)
	if _, err := f.ReadAt(b, 0); err != nil || binary.LittleEndian.Uint32(b) != 0xB10BC001 {
		return discInfo{}, false
	}
	numBlocks := binary.LittleEndian.Uint32(b[28:])
	dataStart := int64(32) + int64(numBlocks)*12
	first := make([]byte, 8)
	f.ReadAt(first, 32)
	off := binary.LittleEndian.Uint64(first)
	compressed := off&(1<<63) == 0
	blk := make([]byte, 64<<10)
	n, _ := f.ReadAt(blk, dataStart+int64(off&^(1<<63)))
	blk = blk[:n]
	if compressed {
		blk = zlibInflate(blk, 0x60)
	}
	return parseDiscHeader(blk)
}

func wadInfo(f *os.File, path string) (discInfo, bool) {
	h := make([]byte, 0x20)
	if _, err := f.ReadAt(h, 0); err != nil || binary.BigEndian.Uint32(h) != 0x20 || (string(h[4:6]) != "Is" && string(h[4:6]) != "ib") {
		return discInfo{}, false
	}
	align := func(n uint32) int64 { return int64((n + 63) &^ 63) }
	certSize, tikSize := binary.BigEndian.Uint32(h[8:]), binary.BigEndian.Uint32(h[16:])
	tmdOff := align(0x20) + align(certSize) + align(tikSize)
	tid := make([]byte, 8)
	f.ReadAt(tid, tmdOff+0x18C)
	id := strings.TrimRight(string(tid[4:8]), "\x00")
	name := cleanGameName(filepath.Base(path))
	name = strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(name, "WAD"), "-"))
	return discInfo{Platform: "WiiWare", ID: id, Title: name}, true
}

var discExts = map[string]bool{".iso": true, ".gcm": true, ".ciso": true, ".wbfs": true, ".rvz": true, ".wia": true, ".gcz": true, ".wad": true}

// ---------------- Dolphin location & folders ----------------

func dolphinExe() string {
	cfgMu.Lock()
	p := cfg.DolphinExe
	cfgMu.Unlock()
	if p != "" {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

// findDolphin looks near RetroPixels and your games folders, in Program Files,
// and in the top folders of each drive.
func findDolphin() string {
	var roots []string
	cfgMu.Lock()
	for _, p := range cfg.DolphinFolders {
		roots = append(roots, p, filepath.Dir(p))
	}
	cfgMu.Unlock()
	if exe, err := os.Executable(); err == nil {
		roots = append(roots, filepath.Dir(exe), filepath.Dir(filepath.Dir(exe)))
	}
	for _, env := range []string{"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"} {
		if d := os.Getenv(env); d != "" {
			roots = append(roots, d)
		}
	}
	for d := 'C'; d <= 'Z'; d++ {
		roots = append(roots, string(d)+`:\`)
	}
	found := ""
	for _, r := range roots {
		if _, err := os.Stat(r); err != nil {
			continue
		}
		depth := 4
		if len(r) <= 3 && !strings.EqualFold(r, `C:\`) {
			depth = 5 // game drives often nest emulators a level deeper
		}
		walkLimited(r, depth, func(p string, d fs.DirEntry) bool {
			if !d.IsDir() && strings.EqualFold(d.Name(), "Dolphin.exe") {
				found = p
				return false
			}
			return true
		})
		if found != "" {
			return found
		}
	}
	return ""
}

var skipDirs = map[string]bool{"windows": true, "$recycle.bin": true, "system volume information": true, "programdata": true,
	"node_modules": true, ".git": true, "appdata": true, "steamapps": true, "shadercache": true, "cache": true}

// walkLimited walks a folder tree to a set depth. fn returns false to stop.
func walkLimited(root string, depth int, fn func(string, fs.DirEntry) bool) {
	var walk func(dir string, d int) bool
	walk = func(dir string, d int) bool {
		entries, err := os.ReadDir(dir)
		if err != nil {
			return true
		}
		for _, e := range entries {
			p := filepath.Join(dir, e.Name())
			if !fn(p, e) {
				return false
			}
			if e.IsDir() && d < depth && !skipDirs[strings.ToLower(e.Name())] {
				if !walk(p, d+1) {
					return false
				}
			}
		}
		return true
	}
	walk(root, 1)
}

func dolphinFolders() []string {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	return append([]string(nil), cfg.DolphinFolders...)
}

// guessDolphinFolders finds folders holding GameCube/Wii games near your other games.
func guessDolphinFolders() []string {
	var roots []string
	cfgMu.Lock()
	if p := cfg.DolphinExe; p != "" {
		// Widest first: the folder holding all your game collections, then narrower.
		d := filepath.Dir(p)
		var chain []string
		for i := 0; i < 4; i++ {
			chain = append([]string{d}, chain...)
			d = filepath.Dir(d)
		}
		roots = append(roots, chain...)
	}
	cfgMu.Unlock()
	seen := map[string]bool{}
	var out []string
	for _, r := range roots {
		r = filepath.Clean(r)
		if seen[strings.ToLower(r)] || len(r) <= 3 { // never scan a whole drive
			continue
		}
		seen[strings.ToLower(r)] = true
		if len(out) > 0 && isUnder(r, out) {
			continue
		}
		dirs := map[string]bool{}
		walkLimited(r, 4, func(p string, d fs.DirEntry) bool {
			if !d.IsDir() && discExts[strings.ToLower(filepath.Ext(p))] {
				if _, ok := identifyDisc(p); ok {
					dirs[filepath.Dir(p)] = true
				}
			}
			return true
		})
		for d := range dirs {
			if !isUnder(d, out) {
				out = append(out, d)
			}
		}
		if len(out) > 0 {
			break
		}
	}
	sort.Strings(out)
	return out
}

func containsFold(list []string, p string) bool {
	lp := strings.ToLower(filepath.Clean(p))
	for _, r := range list {
		if strings.ToLower(filepath.Clean(r)) == lp {
			return true
		}
	}
	return false
}

func isUnder(p string, roots []string) bool {
	lp := strings.ToLower(filepath.Clean(p))
	for _, r := range roots {
		lr := strings.ToLower(filepath.Clean(r))
		if lp == lr || strings.HasPrefix(lp, lr+string(os.PathSeparator)) {
			return true
		}
	}
	return false
}

func autoDetectDolphin() {
	cfgMu.Lock()
	done := cfg.DolphinDetected
	cfgMu.Unlock()
	if done {
		return
	}
	exe := findDolphin()
	cfgMu.Lock()
	if cfg.DolphinExe == "" {
		cfg.DolphinExe = exe
	}
	cfgMu.Unlock()
	folders := guessDolphinFolders()
	cfgMu.Lock()
	if len(cfg.DolphinFolders) == 0 {
		cfg.DolphinFolders = folders
	}
	cfg.DolphinDetected = true
	_ = saveConfigLocked()
	cfgMu.Unlock()
}

// ---------------- library ----------------

type dolGame struct {
	Name     string    `json:"name"`
	File     string    `json:"file"`
	Path     string    `json:"path"`
	Platform string    `json:"platform"`
	ID       string    `json:"id"`
	Size     int64     `json:"size"`
	Added    time.Time `json:"added"`
	Played   time.Time `json:"played"`
}

var (
	discCacheMu sync.Mutex
	discCache   = map[string]*discInfo{} // path|size|mtime -> info (nil = not a game)
)

func cachedIdentify(p string, info fs.FileInfo) (discInfo, bool) {
	key := fmt.Sprintf("%s|%d|%d", p, info.Size(), info.ModTime().Unix())
	discCacheMu.Lock()
	c, ok := discCache[key]
	discCacheMu.Unlock()
	if ok {
		if c == nil {
			return discInfo{}, false
		}
		return *c, true
	}
	d, good := identifyDisc(p)
	discCacheMu.Lock()
	if good {
		discCache[key] = &d
	} else {
		discCache[key] = nil
	}
	discCacheMu.Unlock()
	return d, good
}

var (
	rePlatformPrefix = regexp.MustCompile(`(?i)^\s*(wad|wii\s*ware|wii|gc|ngc|game\s*cube)\b\s*[-–:]*\s*`)
	reNumberPrefix   = regexp.MustCompile(`^\s*\d{1,4}\s*[-–.]\s+`)
	reGameCode       = regexp.MustCompile(`^[A-Z0-9]{4,8}$`)
	reDashColon      = regexp.MustCompile(`\s*-\s+`)
	reTrailingID     = regexp.MustCompile(`([a-z.!?])([A-Z0-9]{6})$`)
)

// dolDisplayName prefers a tidied file name ("WII   - Wii Sports [RSPE01].wbfs" ->
// "Wii Sports") and falls back to the name stored on the disc.
func dolDisplayName(file string, di discInfo) string {
	n := cleanGameName(file)
	n = rePlatformPrefix.ReplaceAllString(n, "")
	n = reNumberPrefix.ReplaceAllString(n, "")
	if di.ID != "" {
		n = strings.TrimSuffix(n, di.ID)
	}
	n = reTrailingID.ReplaceAllString(n, "$1")
	n = reDashColon.ReplaceAllString(n, ": ")
	n = strings.Trim(strings.TrimSpace(n), "-–: ")
	letters := 0
	for _, r := range n {
		if unicode.IsLetter(r) {
			letters++
		}
	}
	// File names like "GMSE01" or "hi-temunitu" (scene release names) say nothing; use the disc's name.
	looksLikeCode := reGameCode.MatchString(n) || (di.ID != "" && strings.EqualFold(n, di.ID)) ||
		!strings.ContainsAny(strings.ToLower(n), "aeiouy") || (!strings.Contains(n, " ") && strings.ToLower(n) == n)
	if (letters >= 3 && !looksLikeCode) || di.Title == "" {
		return n
	}
	return niceTitle(di.Title)
}

// niceTitle makes the on-disc name readable: "SONIC HEROES" -> "Sonic Heroes".
func niceTitle(t string) string {
	if t == "" || strings.ToUpper(t) != t {
		return t
	}
	words := strings.Fields(strings.ToLower(t))
	small := map[string]bool{"of": true, "the": true, "and": true, "a": true, "in": true, "on": true}
	for i, w := range words {
		if i > 0 && small[w] {
			continue
		}
		if len(w) <= 3 && strings.ContainsAny(w, "0123456789") || w == "ii" || w == "iii" || w == "dx" || w == "tmnt" {
			words[i] = strings.ToUpper(w)
			continue
		}
		parts := strings.Split(w, "-")
		for j, p := range parts {
			if p != "" {
				parts[j] = strings.ToUpper(p[:1]) + p[1:]
			}
		}
		words[i] = strings.Join(parts, "-")
	}
	return strings.Join(words, " ")
}

func dolphinLibrary() []dolGame {
	cfgMu.Lock()
	played := map[string]time.Time{}
	for _, r := range cfg.DolphinRecent {
		played[strings.ToLower(r.Path)] = r.LastPlayed
	}
	cfgMu.Unlock()
	var out []dolGame
	seen := map[string]bool{}
	for _, dir := range dolphinFolders() {
		walkLimited(dir, 4, func(p string, d fs.DirEntry) bool {
			if d.IsDir() || !discExts[strings.ToLower(filepath.Ext(p))] || seen[strings.ToLower(p)] {
				return true
			}
			info, err := d.Info()
			if err != nil {
				return true
			}
			di, ok := cachedIdentify(p, info)
			if !ok {
				return true
			}
			seen[strings.ToLower(p)] = true
			name := dolDisplayName(filepath.Base(p), di)
			if strings.HasSuffix(strings.ToLower(name), "(dlc)") || strings.Contains(strings.ToLower(filepath.Base(p)), "(dlc)") {
				return true // add-on content isn't playable by itself
			}
			out = append(out, dolGame{Name: name, File: filepath.Base(p), Path: p, Platform: di.Platform, ID: di.ID,
				Size: info.Size(), Added: info.ModTime(), Played: played[strings.ToLower(p)]})
			return true
		})
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

// ---------------- Dolphin settings files ----------------

// dolphinUserDir: portable "User" folder next to Dolphin.exe, else the usual Windows locations.
func dolphinUserDir() string {
	if t := os.Getenv("RETRO_DOLPHIN_USER"); t != "" {
		return t
	}
	if exe := dolphinExe(); exe != "" {
		dir := filepath.Dir(exe)
		if _, err := os.Stat(filepath.Join(dir, "portable.txt")); err == nil {
			return filepath.Join(dir, "User")
		}
	}
	home, _ := os.UserHomeDir()
	docs := filepath.Join(home, "Documents", "Dolphin Emulator")
	if _, err := os.Stat(docs); err == nil {
		return docs
	}
	if ad := os.Getenv("APPDATA"); ad != "" {
		return filepath.Join(ad, "Dolphin Emulator")
	}
	return docs
}

func dolphinIni(name string) string { return filepath.Join(dolphinUserDir(), "Config", name) }

// ini is a tiny order-preserving INI editor (Dolphin's files are simple).
type ini struct{ lines []string }

func readIni(path string) *ini {
	b, _ := os.ReadFile(path)
	b = bytes.TrimPrefix(b, []byte("\xef\xbb\xbf"))
	s := strings.ReplaceAll(string(b), "\r\n", "\n")
	if s == "" {
		return &ini{}
	}
	return &ini{lines: strings.Split(strings.TrimRight(s, "\n"), "\n")}
}

func (f *ini) find(section, key string) (int, int) { // (line of key or -1, end of section or -1)
	in, end := false, -1
	for i, l := range f.lines {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			if in {
				return -1, end
			}
			in = strings.EqualFold(t[1:len(t)-1], section)
			if in {
				end = i + 1
			}
			continue
		}
		if in {
			if t != "" {
				end = i + 1
			}
			if k, _, ok := strings.Cut(t, "="); ok && strings.EqualFold(strings.TrimSpace(k), key) {
				return i, end
			}
		}
	}
	return -1, end
}

func (f *ini) Get(section, key string) (string, bool) {
	if i, _ := f.find(section, key); i >= 0 {
		_, v, _ := strings.Cut(f.lines[i], "=")
		return strings.TrimSpace(v), true
	}
	return "", false
}

func (f *ini) Set(section, key, val string) {
	line := key + " = " + val
	i, end := f.find(section, key)
	switch {
	case i >= 0:
		f.lines[i] = line
	case end >= 0:
		f.lines = append(f.lines[:end], append([]string{line}, f.lines[end:]...)...)
	default:
		if len(f.lines) > 0 && strings.TrimSpace(f.lines[len(f.lines)-1]) != "" {
			f.lines = append(f.lines, "")
		}
		f.lines = append(f.lines, "["+section+"]", line)
	}
}

func (f *ini) HasSection(section string) bool {
	for _, l := range f.lines {
		if t := strings.TrimSpace(l); strings.EqualFold(t, "["+section+"]") {
			return true
		}
	}
	return false
}

func (f *ini) Save(path string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	if orig, err := os.ReadFile(path); err == nil {
		bk := path + ".retropixels-original"
		if !hasOriginalBackup(path) {
			os.WriteFile(bk, orig, 0o644)
		}
	}
	tmp := path + ".retropixels-tmp"
	if err := os.WriteFile(tmp, []byte(strings.Join(f.lines, "\r\n")+"\r\n"), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// ---------------- Xbox controller set-up for Dolphin ----------------

const xi = "XInput/0/Gamepad"

var gcPadMap = [][2]string{
	{"Device", xi},
	{"Buttons/A", "`Button A`"}, {"Buttons/B", "`Button X`"}, {"Buttons/X", "`Button B`"}, {"Buttons/Y", "`Button Y`"},
	{"Buttons/Z", "`Shoulder R`"}, {"Buttons/Start", "Start"},
	{"Main Stick/Up", "`Left Y+`"}, {"Main Stick/Down", "`Left Y-`"}, {"Main Stick/Left", "`Left X-`"}, {"Main Stick/Right", "`Left X+`"},
	{"C-Stick/Up", "`Right Y+`"}, {"C-Stick/Down", "`Right Y-`"}, {"C-Stick/Left", "`Right X-`"}, {"C-Stick/Right", "`Right X+`"},
	{"Triggers/L", "`Trigger L`"}, {"Triggers/R", "`Trigger R`"}, {"Triggers/L-Analog", "`Trigger L`"}, {"Triggers/R-Analog", "`Trigger R`"},
	{"D-Pad/Up", "`Pad N`"}, {"D-Pad/Down", "`Pad S`"}, {"D-Pad/Left", "`Pad W`"}, {"D-Pad/Right", "`Pad E`"},
	{"Rumble/Motor", "`Motor L`|`Motor R`"},
}

var wiimoteMap = [][2]string{
	{"Device", xi}, {"Source", "1"}, {"Extension", "Nunchuk"},
	{"Buttons/A", "`Button A`"}, {"Buttons/B", "`Trigger R`"}, {"Buttons/1", "`Button X`"}, {"Buttons/2", "`Button Y`"},
	{"Buttons/-", "Back"}, {"Buttons/+", "Start"}, {"Buttons/Home", "`Thumb R`"},
	{"D-Pad/Up", "`Pad N`"}, {"D-Pad/Down", "`Pad S`"}, {"D-Pad/Left", "`Pad W`"}, {"D-Pad/Right", "`Pad E`"},
	{"IR/Up", "`Right Y+`"}, {"IR/Down", "`Right Y-`"}, {"IR/Left", "`Right X-`"}, {"IR/Right", "`Right X+`"},
	{"Shake/X", "`Shoulder R`"}, {"Shake/Y", "`Shoulder R`"}, {"Shake/Z", "`Shoulder R`"},
	{"Nunchuk/Buttons/C", "`Shoulder L`"}, {"Nunchuk/Buttons/Z", "`Trigger L`"},
	{"Nunchuk/Stick/Up", "`Left Y+`"}, {"Nunchuk/Stick/Down", "`Left Y-`"}, {"Nunchuk/Stick/Left", "`Left X-`"}, {"Nunchuk/Stick/Right", "`Left X+`"},
	{"Nunchuk/Shake/X", "`Thumb L`"}, {"Nunchuk/Shake/Y", "`Thumb L`"}, {"Nunchuk/Shake/Z", "`Thumb L`"},
	{"Rumble/Motor", "`Motor L`|`Motor R`"},
}

// padState: "xbox" (our set-up), "custom" (something else is mapped), or "none".
func padState(file, section string) string {
	f := readIni(dolphinIni(file))
	dev, ok := f.Get(section, "Device")
	if !ok {
		return "none"
	}
	if strings.EqualFold(dev, xi) {
		return "xbox"
	}
	return "custom"
}

func setupXboxPads(force bool) error {
	gc := readIni(dolphinIni("GCPadNew.ini"))
	if force || padState("GCPadNew.ini", "GCPad1") == "none" {
		for _, kv := range gcPadMap {
			gc.Set("GCPad1", kv[0], kv[1])
		}
		if err := gc.Save(dolphinIni("GCPadNew.ini")); err != nil {
			return err
		}
	}
	wm := readIni(dolphinIni("WiimoteNew.ini"))
	if force || padState("WiimoteNew.ini", "Wiimote1") == "none" {
		for _, kv := range wiimoteMap {
			wm.Set("Wiimote1", kv[0], kv[1])
		}
		if err := wm.Save(dolphinIni("WiimoteNew.ini")); err != nil {
			return err
		}
	}
	// Port 1 = standard GameCube controller.
	d := readIni(dolphinIni("Dolphin.ini"))
	if _, ok := d.Get("Core", "SIDevice0"); !ok || force {
		d.Set("Core", "SIDevice0", "6")
		return d.Save(dolphinIni("Dolphin.ini"))
	}
	return nil
}

// ---------------- Dolphin graphics settings ----------------

type dolSetting struct {
	File, Section, Key string
	setting
}

var dolSchema = []dolSetting{
	{"Dolphin.ini", "Core", "GFXBackend", setting{Key: "backend", Label: "Graphics API", Type: "select",
		Options: sel("Vulkan", "Vulkan (recommended)", "D3D12", "Direct3D 12", "D3D", "Direct3D 11", "OGL", "OpenGL"), Value: "Vulkan"}},
	{"GFX.ini", "Settings", "InternalResolution", setting{Key: "res", Label: "Resolution", Type: "select",
		Options: sel(1, "Native (480p)", 2, "2× (720p)", 3, "3× (1080p)", 4, "4× (1440p)", 5, "5×", 6, "6× (4K)", 8, "8× (5K)"), Value: int64(1),
		Help: "Higher is sharper; 3× suits a 1080p screen, 6× a 4K one."}},
	{"GFX.ini", "Settings", "AspectRatio", setting{Key: "aspect", Label: "Aspect ratio", Type: "select",
		Options: sel(0, "Auto (what the game uses)", 1, "Force 16:9", 2, "Force 4:3", 3, "Stretch to window"), Value: int64(0)}},
	{"GFX.ini", "Settings", "wideScreenHack", setting{Key: "widehack", Label: "Widescreen hack", Type: "bool", Value: false,
		Help: "Shows more of the scene in 16:9. Can cause glitches in some games."}},
	{"GFX.ini", "Hardware", "VSync", setting{Key: "vsync", Label: "VSync", Type: "bool", Value: false}},
	{"GFX.ini", "Settings", "MSAA", setting{Key: "aa", Label: "Anti-aliasing", Type: "select",
		Options: sel(1, "Off", 2, "2× MSAA", 4, "4× MSAA", 8, "8× MSAA"), Value: int64(1)}},
	{"GFX.ini", "Enhancements", "MaxAnisotropy", setting{Key: "aniso", Label: "Texture filtering", Type: "select",
		Options: sel(0, "1× (default)", 1, "2×", 2, "4×", 3, "8×", 4, "16×"), Value: int64(0)}},
	{"GFX.ini", "Settings", "ShowFPS", setting{Key: "fps", Label: "Show FPS counter", Type: "bool", Value: false}},
	{"Dolphin.ini", "Core", "EnableCheats", setting{Key: "cheats", Label: "Allow cheats", Type: "bool", Value: false}},
	{"Dolphin.ini", "Interface", "ConfirmStop", setting{Key: "confirmstop", Label: "Ask before closing a game", Type: "bool", Value: true}},
}

func iniToValue(s setting, raw string) any {
	switch s.Type {
	case "bool":
		return strings.EqualFold(raw, "true")
	case "select":
		if _, isInt := s.Value.(int64); isInt {
			n, err := strconv.ParseInt(raw, 10, 64)
			if err == nil {
				return n
			}
			return s.Value
		}
		return raw
	}
	return raw
}

func dolSettingsPayload() map[string]any {
	files := map[string]*ini{}
	get := func(name string) *ini {
		if files[name] == nil {
			files[name] = readIni(dolphinIni(name))
		}
		return files[name]
	}
	var out []setting
	for _, d := range dolSchema {
		s := d.setting
		if raw, ok := get(d.File).Get(d.Section, d.Key); ok {
			s.Value = iniToValue(s, raw)
		}
		s.Present = true
		out = append(out, s)
	}
	return map[string]any{"settings": out, "locked": dolphinRunning(),
		"pads":    map[string]string{"gc": padState("GCPadNew.ini", "GCPad1"), "wii": padState("WiimoteNew.ini", "Wiimote1")},
		"userDir": dolphinUserDir()}
}

func saveDolSettings(changes map[string]any) error {
	if dolphinRunning() {
		return errors.New("Dolphin is open. Close it first — it saves its own settings when it closes")
	}
	files := map[string]*ini{}
	for k, raw := range changes {
		var d *dolSetting
		for i := range dolSchema {
			if dolSchema[i].setting.Key == k {
				d = &dolSchema[i]
			}
		}
		if d == nil {
			return fmt.Errorf("unknown setting %q", k)
		}
		v, err := coerce(d.setting, raw, d.Value)
		if err != nil {
			return err
		}
		if files[d.File] == nil {
			files[d.File] = readIni(dolphinIni(d.File))
		}
		str := fmt.Sprint(v)
		if b, ok := v.(bool); ok {
			str = map[bool]string{true: "True", false: "False"}[b]
		}
		files[d.File].Set(d.Section, d.Key, str)
	}
	for name, f := range files {
		if err := f.Save(dolphinIni(name)); err != nil {
			return err
		}
	}
	return nil
}

func dolphinRunning() bool {
	runMu.Lock()
	r := running != nil && running.Kind == "dolphin"
	runMu.Unlock()
	return r || processRunning("Dolphin.exe")
}

// dolphinDumpDir: where Dolphin saves frame and audio dumps (its "Dump"
// folder, unless a different one is chosen in Dolphin's Paths settings).
func dolphinDumpDir() string {
	if p, ok := readIni(dolphinIni("Dolphin.ini")).Get("General", "DumpPath"); ok && strings.TrimSpace(p) != "" {
		return filepath.Clean(strings.TrimSpace(p))
	}
	return filepath.Join(dolphinUserDir(), "Dump")
}

// ---------------- launching ----------------

func launchDolphinGame(path string) error {
	exe := dolphinExe()
	if exe == "" {
		return errors.New("Dolphin isn't set up yet — choose Dolphin.exe in the GameCube or Wii tab")
	}
	var g *dolGame
	for _, x := range dolphinLibrary() {
		if strings.EqualFold(x.Path, path) {
			x := x
			g = &x
		}
	}
	if g == nil {
		return errors.New("that game isn't in your GameCube / Wii games folders")
	}
	if dolphinRunning() {
		return errors.New("Dolphin is already open. Close it first, then start the game here")
	}
	_ = setupXboxPads(false) // only fills in controllers that aren't set up yet
	cfgMu.Lock()
	fs := cfg.DolphinFullscreen == nil || *cfg.DolphinFullscreen
	embed := dolphinEmbedOn()
	cfgMu.Unlock()
	// Dolphin's own volume stays at full: the volume setting here turns its
	// sound down in the Windows volume mixer instead, so the two don't stack.
	args := []string{"-b", "-e", path, "-C", "Dolphin.DSP.Volume=100"}
	onStart := followDolphinVolume
	if embed {
		// Its game screen gets docked into the RetroPixels window, so it must
		// be a plain separate window (not fullscreen, not inside Dolphin's own
		// window), and keep reading the controller while RetroPixels has focus.
		// -C settings only apply to this run; Dolphin's own settings are untouched.
		args = append(args,
			"-C", "Dolphin.Display.Fullscreen=False",
			"-C", "Dolphin.Display.RenderToMain=False",
			"-C", "Dolphin.Interface.ConfirmStop=False",
			"-C", "Dolphin.Interface.PauseOnFocusLost=False",
			"-C", "Dolphin.Input.BackgroundInput=True")
		onStart = func(pid int, exited <-chan struct{}) {
			go followDolphinVolume(pid, exited)
			embedDolphin(pid, exited)
		}
	} else if fs {
		args = append(args, "-C", "Dolphin.Display.Fullscreen=True")
	}
	// Recording: Dolphin saves the game's own picture and sound, and
	// RetroPixels makes the video from them when the game ends.
	var rec *recSession
	if recordingWanted() {
		rec = newRecSession(g.Name, dolphinDumpDir())
		args = append(args, dolphinRecordArgs(recQuality())...)
	}
	if err := startEmulator(exe, args, path, g.Name, "dolphin", "Dolphin", rec, onStart); err != nil {
		return err
	}
	clearManualWanted()
	cfgMu.Lock()
	defer cfgMu.Unlock()
	list := []recentGame{{Path: path, Name: g.Name, LastPlayed: time.Now()}}
	for _, r := range cfg.DolphinRecent {
		if !strings.EqualFold(r.Path, path) {
			list = append(list, r)
		}
	}
	if len(list) > 50 {
		list = list[:50]
	}
	cfg.DolphinRecent = list
	return saveConfigLocked()
}

// dolphinEmbedOn: games play inside the RetroPixels window (the default).
// Call with cfgMu held.
func dolphinEmbedOn() bool { return cfg.DolphinEmbed == nil || *cfg.DolphinEmbed }

func openDolphinUI() error {
	exe := dolphinExe()
	if exe == "" {
		return errors.New("Dolphin isn't set up yet")
	}
	if dolphinRunning() {
		return errors.New("Dolphin is already open")
	}
	_ = setupXboxPads(false)
	cmd := exec.Command(exe)
	cmd.Dir = filepath.Dir(exe)
	return cmd.Start()
}

// ---------------- routes ----------------

func dolStatus() map[string]any {
	exe := dolphinExe()
	version := ""
	if exe != "" {
		if f, err := os.Open(filepath.Join(filepath.Dir(exe), "build_info.txt")); err == nil {
			sc := bufio.NewScanner(io.LimitReader(f, 4096))
			for sc.Scan() {
				if l := strings.TrimSpace(sc.Text()); strings.HasPrefix(strings.ToLower(l), "version") || strings.Contains(l, "Dolphin") {
					version = l
					break
				}
			}
			f.Close()
		}
	}
	cfgMu.Lock()
	fs := cfg.DolphinFullscreen == nil || *cfg.DolphinFullscreen
	embed := dolphinEmbedOn()
	cfgMu.Unlock()
	vol, muted := dolphinVolume()
	return map[string]any{"exe": exe, "version": version, "folders": dolphinFolders(), "fullscreen": fs, "embed": embed,
		"volume": vol, "muted": muted, "install": dolInstallStatus()}
}

func registerDolphinRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/dol/library", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"games": dolphinLibrary(), "status": dolStatus()})
	}))
	mux.HandleFunc("/api/dol/play", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Path string `json:"path"`
		}
		if !readBody(w, r, &b) {
			return
		}
		if err := launchDolphinGame(b.Path); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	// The page reports how much of the window its game bar takes, so the docked
	// game sits right below it.
	mux.HandleFunc("/api/dol/embed", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Top float64 `json:"top"` // CSS pixels from the top of the page
			DPR float64 `json:"dpr"` // window.devicePixelRatio
		}
		if !readBody(w, r, &b) {
			return
		}
		if b.DPR <= 0 {
			b.DPR = 1
		}
		setEmbedTop(int(b.Top*b.DPR + 0.5))
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	registerDolphinVolumeRoute(mux)
	mux.HandleFunc("/api/dol/settings", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			var b struct {
				Changes map[string]any `json:"changes"`
			}
			if !readBody(w, r, &b) {
				return
			}
			if err := saveDolSettings(b.Changes); err != nil {
				fail(w, err)
				return
			}
		}
		writeJSON(w, 200, dolSettingsPayload())
	}))
	mux.HandleFunc("/api/dol/action", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
			Folder string `json:"folder"`
			On     bool   `json:"on"`
		}
		if !readBody(w, r, &b) {
			return
		}
		var err error
		switch b.Action {
		case "choose-exe":
			var p string
			p, err = pickFile("Choose Dolphin.exe", "Dolphin (Dolphin.exe)|Dolphin.exe|Programs (*.exe)|*.exe", filepath.Dir(dolphinExe()))
			if err == nil && p != "" {
				if !strings.EqualFold(filepath.Ext(p), ".exe") {
					err = errors.New("that isn't a program (.exe)")
				} else {
					cfgMu.Lock()
					cfg.DolphinExe = p
					_ = saveConfigLocked()
					cfgMu.Unlock()
				}
			}
		case "add-folder":
			var d string
			d, err = pickFolder("Add a folder with GameCube or Wii games", "")
			if err == nil && d != "" {
				cfgMu.Lock()
				// Add it unless it's already listed; a folder inside a listed one is
				// still added, because the library only looks a few levels deep.
				if !containsFold(cfg.DolphinFolders, d) {
					cfg.DolphinFolders = append(cfg.DolphinFolders, d)
				}
				_ = saveConfigLocked()
				cfgMu.Unlock()
			}
		case "remove-folder":
			cfgMu.Lock()
			out := cfg.DolphinFolders[:0]
			for _, f := range cfg.DolphinFolders {
				if !strings.EqualFold(f, b.Folder) {
					out = append(out, f)
				}
			}
			cfg.DolphinFolders = out
			_ = saveConfigLocked()
			cfgMu.Unlock()
		case "rescan":
			cfgMu.Lock()
			cfg.DolphinFolders = nil
			cfgMu.Unlock()
			f := guessDolphinFolders()
			cfgMu.Lock()
			cfg.DolphinFolders = f
			_ = saveConfigLocked()
			cfgMu.Unlock()
		case "fullscreen":
			cfgMu.Lock()
			v := b.On
			cfg.DolphinFullscreen = &v
			_ = saveConfigLocked()
			cfgMu.Unlock()
		case "embed":
			cfgMu.Lock()
			v := b.On
			cfg.DolphinEmbed = &v
			_ = saveConfigLocked()
			cfgMu.Unlock()
		case "stop-game":
			err = stopEmbeddedGame()
		case "setup-pads":
			if dolphinRunning() {
				err = errors.New("close Dolphin first")
			} else {
				err = setupXboxPads(true)
			}
		case "open-dolphin":
			err = openDolphinUI()
		case "open-config":
			p := filepath.Join(dolphinUserDir(), "Config")
			os.MkdirAll(p, 0o755)
			err = openFolder(p)
		default:
			err = errors.New("unknown action")
		}
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true, "games": dolphinLibrary(), "status": dolStatus()})
	}))
}

// zlibInflate decompresses up to n bytes of a zlib stream.
func zlibInflate(b []byte, n int) []byte {
	r, err := zlib.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil
	}
	defer r.Close()
	out := make([]byte, n)
	k, _ := io.ReadFull(r, out)
	return out[:k]
}
