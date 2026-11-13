// Package handheld is RetroPixels' 3DS / DS tab: one library for Nintendo 3DS
// and Nintendo DS games, all played in PixelDS (PixelDS\PixelDS.exe),
// RetroPixels' own DS and 3DS emulator (cores/pixelds, cmd/pixelds), which
// comes in the RetroPixels download next to the programs. No other DS or
// 3DS emulator is used.
//
// RetroPixels.exe and RetroPixels Native both use this package.
// Game files are only read where they are, never copied.
package handheld

import (
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"retropixels/internal/dolphin"
)

// The two systems.
const (
	System3DS = "3ds"
	SystemDS  = "ds"
)

// Engine is the emulator that plays the games.
type Engine struct {
	ID      string   // "pixelds"
	Name    string   // "PixelDS"
	Dir     string   // its folder next to RetroPixels
	Exes    []string // its program's names
	System  string   // the systems it plays
	Licence string
}

// PixelDS is RetroPixels' own DS and 3DS emulator.
var PixelDS = Engine{ID: "pixelds", Name: "PixelDS", Dir: "PixelDS", Exes: exeNames("PixelDS"),
	System: SystemDS, Licence: "RetroPixels' own"}

// EngineFor is the emulator that plays a system's games: PixelDS.
func EngineFor(system string) Engine { return PixelDS }

// EngineChoice is the emulator for a system's games. Older settings may
// still name another emulator; PixelDS plays everything now.
func EngineChoice(system, choice string) Engine { return PixelDS }

// EngineByID: "pixelds".
func EngineByID(id string) (Engine, bool) {
	if strings.EqualFold(id, PixelDS.ID) {
		return PixelDS, true
	}
	return Engine{}, false
}

// FindExe returns the engine's program: the one chosen by the player if it
// still exists, else its folder next to RetroPixels (appDir\PixelDS\
// PixelDS.exe), else the newest copy unpacked
// into Downloads (a folder starting with the engine's name, the program in
// it or one folder further down). "" if there is none.
func (e Engine) FindExe(chosen, appDir, home string) string {
	if chosen != "" && isFile(chosen) {
		return chosen
	}
	if appDir != "" {
		for _, n := range e.Exes {
			if p := filepath.Join(appDir, e.Dir, n); isFile(p) {
				return p
			}
		}
	}
	if home == "" {
		return ""
	}
	var cands []string
	for _, dl := range dolphin.DownloadDirs(home) {
		for _, n := range e.Exes {
			for _, pat := range []string{filepath.Join(dl, "*"+e.Dir+"*", n), filepath.Join(dl, "*"+e.Dir+"*", "*", n),
				filepath.Join(dl, strings.ToLower(e.Dir)+"*", n), filepath.Join(dl, strings.ToLower(e.Dir)+"*", "*", n)} {
				m, _ := filepath.Glob(pat)
				cands = append(cands, m...)
			}
		}
	}
	best, bestT := "", time.Time{}
	for _, p := range cands {
		if st, err := os.Stat(p); err == nil && !st.IsDir() && (best == "" || st.ModTime().After(bestT)) {
			best, bestT = p, st.ModTime()
		}
	}
	return best
}

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// ---------------- games ----------------

// Game is one 3DS or DS game in the library.
type Game struct {
	Name    string    `json:"name"`
	File    string    `json:"file"`
	Path    string    `json:"path"`
	System  string    `json:"system"`  // System3DS or SystemDS
	Kind    string    `json:"kind"`    // the file type: "3DS", "CIA", "NDS", ...
	Install bool      `json:"install"` // a .cia (PixelDS plays it like any other game)
	Size    int64     `json:"size"`
	Added   time.Time `json:"added"`
	Played  time.Time `json:"played"`
}

type fileKind struct{ system, kind string }

// fileKinds: the game files listed.
var fileKinds = map[string]fileKind{
	".3ds": {System3DS, "3DS"}, ".cci": {System3DS, "CCI"}, ".cxi": {System3DS, "CXI"}, ".3dsx": {System3DS, "3DSX"},
	".zcci": {System3DS, "ZCCI"}, ".zcxi": {System3DS, "ZCXI"}, ".z3dsx": {System3DS, "Z3DSX"},
	".cia": {System3DS, "CIA"}, ".zcia": {System3DS, "ZCIA"},
	".nds": {SystemDS, "NDS"}, ".dsi": {SystemDS, "DSi"}, ".srl": {SystemDS, "SRL"},
}

// Extensions lists the game file extensions, for file dialogs.
func Extensions() []string {
	out := make([]string, 0, len(fileKinds))
	for e := range fileKinds {
		out = append(out, e)
	}
	sort.Strings(out)
	return out
}

// GameFor tells what a file is: a 3DS or DS game, or false.
func GameFor(path string) (Game, bool) {
	st, err := os.Stat(path)
	if err != nil || st.IsDir() {
		return Game{}, false
	}
	return fileGame(path, st)
}

func fileGame(p string, info fs.FileInfo) (Game, bool) {
	ext := strings.ToLower(filepath.Ext(p))
	k, ok := fileKinds[ext]
	if !ok {
		return Game{}, false
	}
	stem := strings.TrimSuffix(filepath.Base(p), filepath.Ext(p))
	return Game{Name: DisplayName(stem), File: filepath.Base(p), Path: p, System: k.system, Kind: k.kind,
		Install: ext == ".cia" || ext == ".zcia", Size: info.Size(), Added: info.ModTime()}, true
}

var (
	reSysPrefix    = regexp.MustCompile(`(?i)^\s*(3ds|nds|ds|nintendo\s*3?ds)\s*[-–:]+\s*`)
	reNumberPrefix = regexp.MustCompile(`^\s*\d{1,4}\s*[-–.]\s+`)
	reDashColon    = regexp.MustCompile(`\s*-\s+`)
)

// DisplayName tidies a file name without its extension:
// "0123 - Mario Kart 7 (USA) (En,Fr,Es)" -> "Mario Kart 7".
func DisplayName(stem string) string {
	n := reSysPrefix.ReplaceAllString(stem, "")
	n = dolphin.CleanName(n + ".")
	n = reNumberPrefix.ReplaceAllString(n, "")
	n = reDashColon.ReplaceAllString(n, ": ")
	if n = strings.Trim(strings.TrimSpace(n), "-–: "); n == "" {
		return strings.TrimSpace(stem)
	}
	return n
}

// Library lists the 3DS and DS games in folders (a few levels deep), A to Z.
// played gives when each game (lower-case path) was last played.
func Library(folders []string, played map[string]time.Time) []Game {
	var out []Game
	seen := map[string]bool{}
	for _, dir := range folders {
		dolphin.WalkLimited(dir, 4, func(p string, d fs.DirEntry) bool {
			if d.IsDir() {
				return true
			}
			if _, ok := fileKinds[strings.ToLower(filepath.Ext(p))]; !ok {
				return true
			}
			k := strings.ToLower(p)
			if seen[k] {
				return true
			}
			info, err := d.Info()
			if err != nil {
				return true
			}
			if g, ok := fileGame(p, info); ok {
				seen[k] = true
				g.Played = played[k]
				out = append(out, g)
			}
			return true
		})
	}
	sort.SliceStable(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

// ---------------- launching ----------------

// GameArgs is PixelDS's command line for a game: -f for fullscreen or -w
// for a window, then the game file.
func GameArgs(e Engine, g Game, fullscreen bool) []string {
	if fullscreen {
		return []string{"-f", g.Path}
	}
	return []string{"-w", g.Path}
}

// Command makes the command that runs the emulator in its own folder.
func Command(exe string, args ...string) *exec.Cmd {
	cmd := exec.Command(exe, args...)
	cmd.Dir = filepath.Dir(exe)
	return cmd
}
