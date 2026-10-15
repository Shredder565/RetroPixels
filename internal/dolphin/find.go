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

// AppFolder is the folder name RetroPixels.exe uses in %LOCALAPPDATA%, so
// both programs share one Dolphin download.
const AppFolder = "RetroPixels"

// InstallDirs: where the one-click download puts Dolphin, as RetroPixels.exe
// does: a "Dolphin" folder next to the program if it can write there,
// otherwise %LOCALAPPDATA%\RetroPixels\Dolphin.
func InstallDirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Join(filepath.Dir(exe), "Dolphin"))
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		dirs = append(dirs, filepath.Join(la, AppFolder, "Dolphin"))
	}
	return dirs
}

// Installed returns Dolphin.exe in one of InstallDirs, or "".
func Installed() string {
	for _, d := range InstallDirs() {
		p := filepath.Join(d, "Dolphin.exe")
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	return ""
}

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

// FindExe looks for Dolphin.exe where RetroPixels.exe looks: the download
// folders, near the games folders and the program, in Program Files, and in
// the top folders of each drive. It can take a while (run it in the background).
func FindExe(gameFolders []string) string {
	if p := Installed(); p != "" {
		return p
	}
	var roots []string
	for _, p := range gameFolders {
		roots = append(roots, p, filepath.Dir(p))
	}
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
		WalkLimited(r, depth, func(p string, d fs.DirEntry) bool {
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

// GuessFolders finds folders holding GameCube / Wii games near Dolphin
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

// Version reads Dolphin's version from the build_info.txt next to it.
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
