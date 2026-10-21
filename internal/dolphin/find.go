package dolphin

import (
	"bufio"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

// AppFolder is the folder name RetroPixels.exe uses in %APPDATA% (its
// config.json).
const AppFolder = "RetroPixels"

var skipDirs = map[string]bool{"windows": true, "$recycle.bin": true, "system volume information": true, "programdata": true,
	"node_modules": true, ".git": true, "appdata": true, "steamapps": true, "shadercache": true, "cache": true}

// WalkLimited walks a folder tree to a set depth. fn returns false to stop.
func WalkLimited(root string, depth int, fn func(string, fs.DirEntry) bool) {
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

// GuessFolders finds folders holding GameCube / Wii games near an emulator
// (people often keep emulators next to their games), widest first.
func GuessFolders(exe string) []string {
	var roots []string
	if exe != "" {
		d := filepath.Dir(exe)
		var chain []string
		for i := 0; i < 4; i++ {
			chain = append([]string{d}, chain...)
			d = filepath.Dir(d)
		}
		roots = append(roots, chain...)
	}
	seen := map[string]bool{}
	var out []string
	for _, r := range roots {
		r = filepath.Clean(r)
		if seen[strings.ToLower(r)] || len(r) <= 3 { // never scan a whole drive
			continue
		}
		seen[strings.ToLower(r)] = true
		if len(out) > 0 && IsUnder(r, out) {
			continue
		}
		dirs := map[string]bool{}
		WalkLimited(r, 4, func(p string, d fs.DirEntry) bool {
			if !d.IsDir() && IsGameFile(p) {
				if _, ok := Identify(p); ok {
					dirs[filepath.Dir(p)] = true
				}
			}
			return true
		})
		for d := range dirs {
			if !IsUnder(d, out) {
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

// ContainsFold: whether list holds p (ignoring case and trailing separators).
func ContainsFold(list []string, p string) bool {
	lp := strings.ToLower(filepath.Clean(p))
	for _, r := range list {
		if strings.ToLower(filepath.Clean(r)) == lp {
			return true
		}
	}
	return false
}

// IsUnder: whether p is one of roots or inside one.
func IsUnder(p string, roots []string) bool {
	lp := strings.ToLower(filepath.Clean(p))
	for _, r := range roots {
		lr := strings.ToLower(filepath.Clean(r))
		if lp == lr || strings.HasPrefix(lp, lr+string(os.PathSeparator)) {
			return true
		}
	}
	return false
}

// Version reads the engine's version from the build_info.txt next to it
// (Dolphin builds have one).
func Version(exe string) string {
	if exe == "" {
		return ""
	}
	f, err := os.Open(filepath.Join(filepath.Dir(exe), "build_info.txt"))
	if err != nil {
		return ""
	}
	defer f.Close()
	sc := bufio.NewScanner(io.LimitReader(f, 4096))
	for sc.Scan() {
		if l := strings.TrimSpace(sc.Text()); strings.HasPrefix(strings.ToLower(l), "version") || strings.Contains(l, "Dolphin") {
			return l
		}
	}
	return ""
}

// ---------------- library ----------------

// Game is one game in the library.
type Game struct {
	Name     string
	File     string
	Path     string
	Platform string
	ID       string
	Size     int64
	Added    time.Time
	Played   time.Time
}

var (
	cacheMu sync.Mutex
	cache   = map[string]*Disc{} // path|size|mtime -> disc (nil = not a game)
)

func cachedIdentify(p string, info fs.FileInfo) (Disc, bool) {
	key := fmt.Sprintf("%s|%d|%d", p, info.Size(), info.ModTime().Unix())
	cacheMu.Lock()
	c, ok := cache[key]
	cacheMu.Unlock()
	if ok {
		if c == nil {
			return Disc{}, false
		}
		return *c, true
	}
	d, good := Identify(p)
	cacheMu.Lock()
	if good {
		cache[key] = &d
	} else {
		cache[key] = nil
	}
	cacheMu.Unlock()
	return d, good
}

// Library lists the games in the folders (four levels deep), sorted by name.
// played gives when each game (by lower-cased path) was last played.
func Library(folders []string, played map[string]time.Time) []Game {
	var out []Game
	seen := map[string]bool{}
	for _, dir := range folders {
		WalkLimited(dir, 4, func(p string, d fs.DirEntry) bool {
			if d.IsDir() || !IsGameFile(p) || seen[strings.ToLower(p)] {
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
			name := DisplayName(filepath.Base(p), di)
			if di.DLC || IsAddOn(filepath.Base(p), name) {
				return true
			}
			out = append(out, Game{Name: name, File: filepath.Base(p), Path: p, Platform: di.Platform, ID: di.ID,
				Size: info.Size(), Added: info.ModTime(), Played: played[strings.ToLower(p)]})
			return true
		})
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

// IsDLC tells whether path is a WiiWare add-on (.wad) that can only be
// installed, not started.
func IsDLC(path string) bool {
	st, err := os.Stat(path)
	if err != nil || st.IsDir() {
		return false
	}
	di, ok := cachedIdentify(path, st)
	return ok && (di.DLC || IsAddOn(filepath.Base(path), ""))
}

// GameFor identifies one game file (for a game opened directly, not from the library).
func GameFor(path string) (Game, bool) {
	st, err := os.Stat(path)
	if err != nil || st.IsDir() {
		return Game{}, false
	}
	di, ok := cachedIdentify(path, st)
	if !ok {
		return Game{}, false
	}
	return Game{Name: DisplayName(filepath.Base(path), di), File: filepath.Base(path), Path: path, Platform: di.Platform,
		ID: di.ID, Size: st.Size(), Added: st.ModTime()}, true
}

// ---------------- the player's own folders ----------------

// xdgUserDirKeys: the user-dirs.dirs names of the player's folders.
var xdgUserDirKeys = map[string]string{
	"Downloads": "XDG_DOWNLOAD_DIR", "Desktop": "XDG_DESKTOP_DIR", "Documents": "XDG_DOCUMENTS_DIR",
	"Videos": "XDG_VIDEOS_DIR", "Music": "XDG_MUSIC_DIR", "Pictures": "XDG_PICTURES_DIR",
}

// xdgConfigHome is $XDG_CONFIG_HOME, or home/.config when it's unset or not
// an absolute path (as the XDG spec says).
func xdgConfigHome(home string) string {
	if d := os.Getenv("XDG_CONFIG_HOME"); filepath.IsAbs(d) {
		return d
	}
	return filepath.Join(home, ".config")
}

// xdgDataHome is $XDG_DATA_HOME, or home/.local/share.
func xdgDataHome(home string) string {
	if d := os.Getenv("XDG_DATA_HOME"); filepath.IsAbs(d) {
		return d
	}
	return filepath.Join(home, ".local", "share")
}

// UserFolder is one of the player's own folders in home ("Downloads",
// "Desktop", "Documents", "Videos", "Music" or "Pictures"). On Linux the
// desktop can name it differently (in the player's language, or moved
// somewhere else) in ~/.config/user-dirs.dirs; that one is used when it's
// set there. On Windows it is always home\<name>.
func UserFolder(home, name string) string {
	return userFolderFor(runtime.GOOS, home, name)
}

func userFolderFor(goos, home, name string) string {
	def := filepath.Join(home, name)
	key, ok := xdgUserDirKeys[name]
	if goos == "windows" || !ok || home == "" {
		return def
	}
	f, err := os.Open(filepath.Join(xdgConfigHome(home), "user-dirs.dirs"))
	if err != nil {
		return def
	}
	defer f.Close()
	sc := bufio.NewScanner(io.LimitReader(f, 64<<10))
	for sc.Scan() {
		k, v, ok := strings.Cut(strings.TrimSpace(sc.Text()), "=")
		if !ok || strings.TrimSpace(k) != key {
			continue
		}
		v = strings.TrimSpace(v)
		if len(v) < 2 || v[0] != '"' || v[len(v)-1] != '"' {
			continue
		}
		v = v[1 : len(v)-1]
		switch {
		case v == "$HOME" || v == "$HOME/":
			return home
		case strings.HasPrefix(v, "$HOME/"):
			return filepath.Join(home, filepath.FromSlash(v[len("$HOME/"):]))
		case filepath.IsAbs(v):
			return filepath.Clean(v)
		}
	}
	return def
}

// DownloadDirs: where the player's downloads land: the desktop's Downloads
// folder (UserFolder) first, then home/Downloads when that's another one.
func DownloadDirs(home string) []string {
	if home == "" {
		return nil
	}
	out := []string{UserFolder(home, "Downloads")}
	if d := filepath.Join(home, "Downloads"); filepath.Clean(d) != filepath.Clean(out[0]) {
		out = append(out, d)
	}
	return out
}
