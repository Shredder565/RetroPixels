// Package handheld is RetroPixels' 3DS / DS tab: one library for Nintendo 3DS
// and Nintendo DS games, played in the emulators that come in the
// RetroPixels download, next to the programs:
//
//   - PixelDS (PixelDS\PixelDS.exe), RetroPixels' own DS and 3DS emulator
//     (cores/pixelds, cmd/pixelds). It plays DS games now; 3DS games come
//     to it stage by stage, and then it replaces the two below.
//   - Azahar (Azahar\azahar.exe, GPL-2.0-or-later) for 3DS games until
//     PixelDS plays them.
//   - melonDS (melonDS\melonDS.exe, GPL-3.0-or-later), which DS games can
//     still be sent to instead of PixelDS while PixelDS is new.
//
// The player sees one tab and one list: each game goes to the emulator for
// its system. RetroPixels.exe and RetroPixels Native both use this package.
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

// Engine is one of the two emulators.
type Engine struct {
	ID      string   // "azahar" or "melonds"
	Name    string   // "Azahar"
	Dir     string   // its folder next to RetroPixels
	Exes    []string // its program's names, newest first
	System  string   // the system it plays
	Licence string
}

var (
	// Azahar plays 3DS games.
	Azahar = Engine{ID: "azahar", Name: "Azahar", Dir: "Azahar", Exes: exeNames("azahar", "azahar-qt", "citra-qt"),
		System: System3DS, Licence: "GPL-2.0-or-later"}
	// MelonDS plays DS (and DSi) games when the player picks it over PixelDS.
	MelonDS = Engine{ID: "melonds", Name: "melonDS", Dir: "melonDS", Exes: exeNames("melonDS"),
		System: SystemDS, Licence: "GPL-3.0-or-later"}
	// PixelDS is RetroPixels' own DS (and, as it grows, 3DS) emulator.
	PixelDS = Engine{ID: "pixelds", Name: "PixelDS", Dir: "PixelDS", Exes: exeNames("PixelDS"),
		System: SystemDS, Licence: "RetroPixels' own"}
)

// EngineFor is the emulator that plays a system's games by default:
// PixelDS for DS games, Azahar for 3DS games.
func EngineFor(system string) Engine {
	if system == SystemDS || system == System3DS {
		return PixelDS
	}
	return Azahar
}

// EngineChoice is the emulator for a system's games given the player's
// choice for that system: "pixelds" or "melonds" for DS games ("" =
// PixelDS), "pixelds" or "azahar" for 3DS games ("" = PixelDS).
func EngineChoice(system, choice string) Engine {
	if system == SystemDS && strings.EqualFold(choice, MelonDS.ID) {
		return MelonDS
	}
	if system == System3DS && strings.EqualFold(choice, Azahar.ID) {
		return Azahar
	}
	return EngineFor(system)
}

// Alternative is the other emulator for a system's games, used when the
// chosen one isn't there.
func Alternative(system string, e Engine) (Engine, bool) {
	switch {
	case system == SystemDS && e.ID == PixelDS.ID:
		return MelonDS, true
	case system == SystemDS && e.ID == MelonDS.ID:
		return PixelDS, true
	case system == System3DS && e.ID == PixelDS.ID:
		return Azahar, true
	case system == System3DS && e.ID == Azahar.ID:
		return PixelDS, true
	}
	return Engine{}, false
}

// EngineByID: "pixelds", "azahar" or "melonds".
func EngineByID(id string) (Engine, bool) {
	switch strings.ToLower(id) {
	case Azahar.ID:
		return Azahar, true
	case MelonDS.ID:
		return MelonDS, true
	case PixelDS.ID:
		return PixelDS, true
	}
	return Engine{}, false
}

// FindExe returns the engine's program: the one chosen by the player if it
// still exists, else its folder next to RetroPixels (appDir\Azahar\
// azahar.exe, appDir\melonDS\melonDS.exe), else the newest copy unpacked
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
	Install bool      `json:"install"` // a .cia: installed into Azahar, then played from there
	Size    int64     `json:"size"`
	Added   time.Time `json:"added"`
	Played  time.Time `json:"played"`
}

type fileKind struct{ system, kind string }

// fileKinds: the game files listed. Azahar also opens .app and .elf, but
// those are too often other things to list them.
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

// GameArgs is the emulator's command line for a game. All three take -f
// for fullscreen and the game file last. In Azahar a .cia is installed
// instead (-i): Azahar then lists it in its own game list; PixelDS plays
// a .cia like any other game.
func GameArgs(e Engine, g Game, fullscreen bool) []string {
	if g.Install && e.ID == Azahar.ID {
		return []string{"-i", g.Path}
	}
	var args []string
	if fullscreen {
		args = append(args, "-f")
	} else if g.System == System3DS {
		args = append(args, "-w") // Azahar remembers fullscreen from its last game otherwise
	}
	return append(args, g.Path)
}

// Command makes the command that runs the emulator in its own folder.
func Command(exe string, args ...string) *exec.Cmd {
	cmd := exec.Command(exe, args...)
	cmd.Dir = filepath.Dir(exe)
	return cmd
}
