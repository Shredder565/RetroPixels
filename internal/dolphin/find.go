package dolphin

import (
	"bufio"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
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
			if IsAddOn(filepath.Base(p), name) {
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
