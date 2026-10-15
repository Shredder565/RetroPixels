package main

// GameCube / Wii through Dolphin (internal/dolphin): the settings kept in
// settings.json and the parts of the GameCube / Wii menu and library that
// aren't Windows calls (dolphin_windows.go has those).

import (
	"archive/zip"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"retropixels/internal/dolphin"
	"retropixels/sevenzip"
)

// DolphinSettings: settings.json's "dolphin" part. Unset switches take
// RetroPixels.exe's defaults (fullscreen, inside the window, recorded,
// volume 100%).
type DolphinSettings struct {
	Exe        string               `json:"exe,omitempty"`
	Folders    []string             `json:"folders,omitempty"`
	Played     map[string]time.Time `json:"played,omitempty"` // by lower-cased path
	Fullscreen *bool                `json:"fullscreen,omitempty"`
	Embed      *bool                `json:"playInWindow,omitempty"`
	AutoRecord *bool                `json:"autoRecord,omitempty"`
	Volume     *int                 `json:"volume,omitempty"` // 0..100
	Muted      bool                 `json:"muted,omitempty"`
	Detected   bool                 `json:"detected,omitempty"` // looked for Dolphin and games once
	Filter     string               `json:"libraryFilter,omitempty"`
	Sort       string               `json:"librarySort,omitempty"`
}

func boolOr(p *bool, def bool) bool {
	if p == nil {
		return def
	}
	return *p
}

func (d *DolphinSettings) fullscreen() bool { return boolOr(d.Fullscreen, true) }
func (d *DolphinSettings) embed() bool      { return boolOr(d.Embed, true) }
func (d *DolphinSettings) autoRecord() bool { return boolOr(d.AutoRecord, true) }

func (d *DolphinSettings) volume() int {
	if d.Volume == nil || *d.Volume < 0 || *d.Volume > 100 {
		return 100
	}
	return *d.Volume
}

func (d *DolphinSettings) setVolume(v int) {
	if v < 0 {
		v = 0
	}
	if v > 100 {
		v = 100
	}
	d.Volume = &v
}

func (d *DolphinSettings) markPlayed(path string, t time.Time) {
	if d.Played == nil {
		d.Played = map[string]time.Time{}
	}
	d.Played[strings.ToLower(path)] = t
	// keep the 200 most recent
	if len(d.Played) > 200 {
		type kv struct {
			k string
			t time.Time
		}
		var all []kv
		for k, v := range d.Played {
			all = append(all, kv{k, v})
		}
		sort.Slice(all, func(i, j int) bool { return all[i].t.After(all[j].t) })
		for _, x := range all[200:] {
			delete(d.Played, x.k)
		}
	}
}

// exe: Dolphin.exe if it's still there, else one RetroPixels downloaded, else "".
func (d *DolphinSettings) exe() string {
	if d.Exe != "" {
		if st, err := os.Stat(d.Exe); err == nil && !st.IsDir() {
			return d.Exe
		}
	}
	return dolphin.Installed()
}

// addFolder adds a games folder unless it's already listed. (A folder inside
// a listed one is still added: the library only looks four levels deep.)
func (d *DolphinSettings) addFolder(dir string) bool {
	if dir == "" || dolphin.ContainsFold(d.Folders, dir) {
		return false
	}
	d.Folders = append(d.Folders, dir)
	return true
}

func (d *DolphinSettings) removeFolder(dir string) {
	out := d.Folders[:0]
	for _, f := range d.Folders {
		if !strings.EqualFold(filepath.Clean(f), filepath.Clean(dir)) {
			out = append(out, f)
		}
	}
	d.Folders = out
}

// appDolphin is what RetroPixels.exe knows about Dolphin (its config.json),
// so the native app starts with the same Dolphin and games folders.
type appDolphin struct {
	Exe     string   `json:"dolphinExe"`
	Folders []string `json:"dolphinFolders"`
	Recent  []struct {
		Path       string    `json:"path"`
		LastPlayed time.Time `json:"lastPlayed"`
	} `json:"dolphinRecent"`
}

// appConfigPath: RetroPixels.exe's config.json (%APPDATA%\RetroPixels).
func appConfigPath() string {
	if d := os.Getenv("RETRO_APP_CONFIG"); d != "" {
		return d
	}
	d, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(d, dolphin.AppFolder, "config.json")
}

func readAppDolphin(path string) appDolphin {
	var a appDolphin
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &a)
	}
	return a
}

// detectDolphin finds Dolphin and the games folders the first time (it can
// take a while: run it in the background). It starts from what
// RetroPixels.exe found, then looks around as RetroPixels.exe does.
func detectDolphin(have DolphinSettings) (exe string, folders []string, played map[string]time.Time) {
	app := readAppDolphin(appConfigPath())
	exe = have.exe()
	if exe == "" && app.Exe != "" {
		if st, err := os.Stat(app.Exe); err == nil && !st.IsDir() {
			exe = app.Exe
		}
	}
	folders = have.Folders
	if len(folders) == 0 {
		for _, f := range app.Folders {
			if st, err := os.Stat(f); err == nil && st.IsDir() {
				folders = append(folders, f)
			}
		}
	}
	if exe == "" {
		exe = dolphin.FindExe(folders)
	}
	if len(folders) == 0 && exe != "" {
		folders = dolphin.GuessFolders(exe)
	}
	played = map[string]time.Time{}
	for _, r := range app.Recent {
		played[strings.ToLower(r.Path)] = r.LastPlayed
	}
	return exe, folders, played
}

// dolphinExts: the file types for the Open dialog.
func dolphinExts() []string {
	var out []string
	for e := range dolphin.Exts {
		out = append(out, e)
	}
	sort.Strings(out)
	return out
}

// discInArchive: the GameCube / Wii game inside a .zip or .7z, if there is
// one (only the list of files is read).
func discInArchive(path string) string {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".zip":
		zr, err := zip.OpenReader(path)
		if err != nil {
			return ""
		}
		defer zr.Close()
		for _, f := range zr.File {
			if dolphin.IsGameFile(f.Name) {
				return filepath.Base(f.Name)
			}
		}
	case ".7z":
		list, err := sevenzip.List(path)
		if err != nil {
			return ""
		}
		for _, f := range list {
			if !f.IsDir && dolphin.IsGameFile(f.Name) {
				return filepath.Base(f.Name)
			}
		}
	}
	return ""
}

// ---------------- the library's list ----------------

// Library filters, as the app's GameCube and Wii tabs (and its Wii discs /
// WiiWare buttons).
var libFilters = []struct{ id, label string }{
	{"all", "All games"}, {"GameCube", "GameCube"}, {"Wii", "Wii discs"}, {"WiiWare", "WiiWare"},
}

var libSorts = []struct{ id, label string }{
	{"az", "A – Z"}, {"played", "Recently played"}, {"added", "Recently added"},
}

// filterGames: the games to show, in order.
func filterGames(games []dolphin.Game, filter, query, order string) []dolphin.Game {
	q := strings.ToLower(strings.TrimSpace(query))
	var out []dolphin.Game
	for _, g := range games {
		if filter != "" && filter != "all" && g.Platform != filter {
			continue
		}
		if q != "" && !strings.Contains(strings.ToLower(g.Name), q) && !strings.Contains(strings.ToLower(g.File), q) {
			continue
		}
		out = append(out, g)
	}
	switch order {
	case "played":
		sort.SliceStable(out, func(i, j int) bool {
			if !out[i].Played.Equal(out[j].Played) {
				return out[i].Played.After(out[j].Played)
			}
			return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name)
		})
	case "added":
		sort.SliceStable(out, func(i, j int) bool { return out[i].Added.After(out[j].Added) })
	}
	return out
}

// fmtSize: 1.4 GB, 350 MB, 12 KB (as the app's library).
func fmtSize(b int64) string {
	switch {
	case b >= 1e9:
		return fmt.Sprintf("%.1f GB", float64(b)/1e9)
	case b >= 1e6:
		return fmt.Sprintf("%d MB", (b+5e5)/1e6)
	case b > 0:
		return fmt.Sprintf("%d KB", max(1, (b+500)/1000))
	}
	return "0"
}

// fmtPlayed: when a game was last played, in words.
func fmtPlayed(t, now time.Time) string {
	if t.IsZero() {
		return ""
	}
	y1, m1, d1 := t.Date()
	y2, m2, d2 := now.Date()
	days := int(time.Date(y2, m2, d2, 0, 0, 0, 0, time.UTC).Sub(time.Date(y1, m1, d1, 0, 0, 0, 0, time.UTC)).Hours() / 24)
	switch {
	case days <= 0:
		return "Today"
	case days == 1:
		return "Yesterday"
	case days < 7:
		return fmt.Sprintf("%d days ago", days)
	case y1 == y2:
		return t.Format("2 Jan")
	}
	return t.Format("2 Jan 2006")
}
