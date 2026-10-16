package mame

import (
	"bufio"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ---------------- game names ----------------

// ParseListFull reads MAME's -listfull output: each machine's short name and
// its full name ("tmnt" → "Teenage Mutant Ninja Turtles (World 4 Players)").
func ParseListFull(out string) map[string]string {
	names := map[string]string{}
	sc := bufio.NewScanner(strings.NewReader(out))
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		i := strings.IndexByte(l, '"')
		if i <= 0 {
			continue
		}
		short := strings.TrimSpace(l[:i])
		full := strings.TrimSuffix(strings.TrimSpace(l[i+1:]), `"`)
		if short == "" || strings.ContainsAny(short, " \t") || full == "" {
			continue
		}
		names[strings.ToLower(short)] = full
	}
	return names
}

var (
	namesMu    sync.Mutex
	namesCache = map[string]map[string]string{} // exe|size|mtime
)

// namesDir keeps each version's list, so MAME is only asked once.
func namesDir() string { return filepath.Join(filepath.Dir(ConfigPath()), "MAME names") }

// NamesOf returns the machines one MAME knows. The list is saved the first
// time (asking MAME takes a few seconds), so call it in the background.
func NamesOf(in Install) map[string]string { return namesOf(in, true) }

// KnownNames is NamesOf without asking MAME: only a list saved before.
func KnownNames(in Install) map[string]string { return namesOf(in, false) }

func namesOf(in Install, ask bool) map[string]string {
	st, err := os.Stat(in.Exe)
	if err != nil {
		return nil
	}
	key := in.Exe + "|" + st.ModTime().UTC().Format(time.RFC3339) + "|" + itoa(st.Size())
	namesMu.Lock()
	n, ok := namesCache[key]
	namesMu.Unlock()
	if ok {
		return n
	}
	file := filepath.Join(namesDir(), safeName(in.Version+"-"+itoa(st.Size())+"-"+itoa(st.ModTime().Unix()))+".txt")
	var out string
	if b, err := os.ReadFile(file); err == nil {
		out = string(b)
	} else if !ask {
		return nil
	} else {
		out, _ = run(in.Exe, 2*time.Minute, "-listfull")
		if n := ParseListFull(out); len(n) > 0 {
			os.MkdirAll(namesDir(), 0o755)
			os.WriteFile(file, []byte(out), 0o644)
		}
	}
	n = ParseListFull(out)
	namesMu.Lock()
	namesCache[key] = n
	namesMu.Unlock()
	return n
}

// AllNames merges the names every install knows; newer versions win.
func AllNames(installs []Install) map[string]string {
	all := map[string]string{}
	list := append([]Install(nil), installs...)
	sort.SliceStable(list, func(a, b int) bool { return CompareVersions(list[a].Version, list[b].Version) < 0 })
	for _, in := range list {
		for k, v := range NamesOf(in) {
			all[k] = v
		}
	}
	return all
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }

func safeName(s string) string {
	return strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '.' || r == '-' {
			return r
		}
		return '_'
	}, s)
}

// ---------------- the library ----------------

// Game is one ROM set in your ROM folders.
type Game struct {
	Short  string    `json:"short"` // MAME's name for it: tmnt
	Name   string    `json:"name"`  // its full name, when a MAME knows it
	Path   string    `json:"path"`
	Size   int64     `json:"size"`
	Played time.Time `json:"played"`
}

// IsRomSet: MAME reads ROM sets from .zip and .7z files.
func IsRomSet(path string) bool {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".zip", ".7z":
		return true
	}
	return false
}

// ShortName is the set's MAME name: the file name without .zip.
func ShortName(path string) string {
	b := filepath.Base(path)
	return strings.ToLower(strings.TrimSuffix(b, filepath.Ext(b)))
}

// Library lists the ROM sets in the folders (two levels deep), sorted by name.
// When names is set, only sets a MAME knows are listed (other .zip files,
// such as console games, are left out).
func Library(folders []string, names map[string]string, played map[string]time.Time) []Game {
	var out []Game
	seen := map[string]bool{}
	for _, dir := range folders {
		walk(dir, 2, func(p string, d fs.DirEntry) {
			if d.IsDir() || !IsRomSet(p) {
				return
			}
			short := ShortName(p)
			if seen[short] {
				return
			}
			full, known := names[short]
			if len(names) > 0 && !known {
				return
			}
			seen[short] = true
			g := Game{Short: short, Name: full, Path: p, Played: played[short]}
			if g.Name == "" {
				g.Name = short
			}
			if info, err := d.Info(); err == nil {
				g.Size = info.Size()
			}
			out = append(out, g)
		})
	}
	sort.Slice(out, func(a, b int) bool {
		if c := strings.Compare(strings.ToLower(out[a].Name), strings.ToLower(out[b].Name)); c != 0 {
			return c < 0
		}
		return out[a].Short < out[b].Short
	})
	return out
}

func walk(dir string, depth int, fn func(string, fs.DirEntry)) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	for _, e := range entries {
		p := filepath.Join(dir, e.Name())
		fn(p, e)
		if e.IsDir() && depth > 1 {
			walk(p, depth-1, fn)
		}
	}
}

// Filter narrows the library by a search ("" = all) and orders it: "az"
// (by name) or "played" (most recently played first).
func Filter(games []Game, query, order string) []Game {
	q := strings.ToLower(strings.TrimSpace(query))
	var out []Game
	for _, g := range games {
		if q == "" || strings.Contains(strings.ToLower(g.Name), q) || strings.Contains(g.Short, q) {
			out = append(out, g)
		}
	}
	if order == "played" {
		sort.SliceStable(out, func(a, b int) bool { return out[a].Played.After(out[b].Played) })
	}
	return out
}
