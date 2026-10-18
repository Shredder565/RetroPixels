// Package pixelu finds PixelU (RetroPixels' Wii U emulator, based on Cemu),
// lists Wii U games in the player's folders and builds PixelU's command line.
// RetroPixels.exe and RetroPixels Native both use it. Game files are only
// read where they are (a few bytes, to tell what they are), never copied.
package pixelu

import (
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"retropixels/internal/dolphin"
)

// ExeName is PixelU's program file (it ships in a "PixelU" folder with its
// resources, gameProfiles and portable folders).
const ExeName = "PixelU.exe"

// FindExe returns PixelU.exe: the one chosen by the player if it still
// exists, else a PixelU folder next to RetroPixels (appDir\PixelU\PixelU.exe),
// else the newest copy unpacked into Downloads
// (home\Downloads\PixelU*\PixelU\PixelU.exe). "" if there is none.
func FindExe(chosen, appDir, home string) string {
	isFile := func(p string) bool {
		st, err := os.Stat(p)
		return err == nil && !st.IsDir()
	}
	if chosen != "" && isFile(chosen) {
		return chosen
	}
	if appDir != "" {
		if p := filepath.Join(appDir, "PixelU", ExeName); isFile(p) {
			return p
		}
	}
	if home == "" {
		return ""
	}
	var cands []string
	dl := filepath.Join(home, "Downloads")
	for _, pat := range []string{filepath.Join(dl, "PixelU*", "PixelU", ExeName), filepath.Join(dl, "PixelU*", ExeName)} {
		m, _ := filepath.Glob(pat)
		cands = append(cands, m...)
	}
	best, bestT := "", time.Time{}
	for _, p := range cands {
		if st, err := os.Stat(p); err == nil && !st.IsDir() && (best == "" || st.ModTime().After(bestT)) {
			best, bestT = p, st.ModTime()
		}
	}
	return best
}

// ---------------- games ----------------

// Game is one Wii U game in the library.
type Game struct {
	Name   string    `json:"name"`
	File   string    `json:"file"` // file name, or the title folder's name for an unpacked game
	Path   string    `json:"path"` // what PixelU opens (for an unpacked game, its code\*.rpx)
	Kind   string    `json:"kind"` // "WUA", "WUD", "WUX", "ISO" or "Folder"
	Size   int64     `json:"size"`
	Added  time.Time `json:"added"`
	Played time.Time `json:"played"`
}

// fileKinds: game files listed in the library (.iso only when it is a Wii U disc).
var fileKinds = map[string]string{".wua": "WUA", ".wud": "WUD", ".wux": "WUX", ".iso": "ISO"}

// IsWiiUDisc tells a Wii U disc image saved as .iso: it starts with the
// product code, e.g. "WUP-P-ARDE".
func IsWiiUDisc(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	b := make([]byte, 4)
	n, _ := f.ReadAt(b, 0)
	return n == 4 && string(b) == "WUP-"
}

var rePart = regexp.MustCompile(`(?i)[ _.-]part(\d+)$`)

// fileGame: a game file, or false if it isn't one PixelU should list.
func fileGame(p string, info fs.FileInfo) (Game, bool) {
	ext := strings.ToLower(filepath.Ext(p))
	kind, ok := fileKinds[ext]
	if !ok {
		return Game{}, false
	}
	if ext == ".iso" && !IsWiiUDisc(p) {
		return Game{}, false // a GameCube, Wii, PC or PlayStation disc
	}
	stem := strings.TrimSuffix(filepath.Base(p), filepath.Ext(p))
	if m := rePart.FindStringSubmatch(stem); m != nil {
		if n, _ := strconv.Atoi(m[1]); n > 1 {
			return Game{}, false // a split .wud: PixelU opens it from part 1
		}
		stem = stem[:len(stem)-len(m[0])]
	}
	return Game{Name: DisplayName(stem), File: filepath.Base(p), Path: p, Kind: kind, Size: info.Size(), Added: info.ModTime()}, true
}

var (
	reTitleID = regexp.MustCompile(`(?is)<title_id[^>]*>\s*([0-9a-f]{16})\s*<`)
	reArgStr  = regexp.MustCompile(`(?is)<argstr[^>]*>\s*([^<]+?\.rpx)\s*<`)
)

// titleGame: an unpacked game (a title folder with code\*.rpx), given its
// code folder. Updates and DLC (also unpacked like this) aren't games by
// themselves, so they're left out.
func titleGame(code string) (Game, bool) {
	if b, err := os.ReadFile(filepath.Join(code, "app.xml")); err == nil {
		if m := reTitleID.FindSubmatch(b); m != nil {
			switch strings.ToUpper(string(m[1][:8])) {
			case "0005000E", "0005000C": // update, DLC
				return Game{}, false
			}
		}
	}
	entries, err := os.ReadDir(code)
	if err != nil {
		return Game{}, false
	}
	var rpx []string
	for _, e := range entries {
		if !e.IsDir() && strings.EqualFold(filepath.Ext(e.Name()), ".rpx") {
			rpx = append(rpx, e.Name())
		}
	}
	if len(rpx) == 0 {
		return Game{}, false
	}
	sort.Strings(rpx)
	main := rpx[0]
	if b, err := os.ReadFile(filepath.Join(code, "cos.xml")); err == nil && len(rpx) > 1 {
		if m := reArgStr.FindSubmatch(b); m != nil {
			for _, r := range rpx {
				if strings.EqualFold(r, filepath.Base(string(m[1]))) {
					main = r
				}
			}
		}
	}
	p := filepath.Join(code, main)
	st, err := os.Stat(p)
	if err != nil {
		return Game{}, false
	}
	title := filepath.Base(filepath.Dir(code))
	return Game{Name: DisplayName(title), File: title, Path: p, Kind: "Folder", Size: st.Size(), Added: st.ModTime()}, true
}

// Library lists the Wii U games in folders (a few levels deep), A to Z.
// played gives when each game (lower-case path) was last played.
func Library(folders []string, played map[string]time.Time) []Game {
	var out []Game
	seen := map[string]bool{}
	add := func(g Game) {
		k := strings.ToLower(g.Path)
		if seen[k] {
			return
		}
		seen[k] = true
		g.Played = played[k]
		out = append(out, g)
	}
	for _, dir := range folders {
		dolphin.WalkLimited(dir, 4, func(p string, d fs.DirEntry) bool {
			if d.IsDir() {
				if strings.EqualFold(d.Name(), "code") {
					if g, ok := titleGame(p); ok {
						add(g)
					}
				}
				return true
			}
			if _, ok := fileKinds[strings.ToLower(filepath.Ext(p))]; !ok {
				return true
			}
			info, err := d.Info()
			if err != nil {
				return true
			}
			if g, ok := fileGame(p, info); ok {
				add(g)
			}
			return true
		})
	}
	sort.SliceStable(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

// IsGameFile: a file PixelU plays when it's opened directly (a Wii U .iso
// is checked; .rpx and .elf are Wii U programs).
func IsGameFile(path string) bool {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".wua", ".wud", ".wux", ".rpx", ".elf":
		return true
	case ".iso":
		return IsWiiUDisc(path)
	}
	return false
}

// GameFor describes one game file opened directly.
func GameFor(path string) (Game, bool) {
	st, err := os.Stat(path)
	if err != nil || st.IsDir() || !IsGameFile(path) {
		return Game{}, false
	}
	if g, ok := fileGame(path, st); ok {
		return g, true
	}
	name := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	if dir := filepath.Dir(path); strings.EqualFold(filepath.Base(dir), "code") {
		name = filepath.Base(filepath.Dir(dir)) // an unpacked game's code\*.rpx: its title folder names it
	}
	return Game{Name: DisplayName(name), File: filepath.Base(path), Path: path, Kind: strings.ToUpper(strings.TrimPrefix(filepath.Ext(path), ".")),
		Size: st.Size(), Added: st.ModTime()}, true
}

var (
	reWiiUPrefix   = regexp.MustCompile(`(?i)^\s*(wii\s*u|wiiu|wup)\s*[-–:]+\s*`)
	reNumberPrefix = regexp.MustCompile(`^\s*\d{1,4}\s*[-–.]\s+`)
	reDashColon    = regexp.MustCompile(`\s*-\s+`)
)

// DisplayName tidies a file or folder name (without its extension) the way
// GameCube and Wii names are: "WII  U -Hyrule Warriors (US)" -> "Hyrule
// Warriors". Dolphin's own name tidier isn't used whole, because it drops a
// leading "Wii", which many Wii U games start with ("Wii Party U").
func DisplayName(stem string) string {
	n := reWiiUPrefix.ReplaceAllString(stem, "")
	n = dolphin.CleanName(n + ".") // the "." stands in for an extension, so a dot in the name ("v1.0") stays
	n = reNumberPrefix.ReplaceAllString(n, "")
	n = reDashColon.ReplaceAllString(n, ": ")
	if n = strings.Trim(strings.TrimSpace(n), "-–: "); n == "" {
		return strings.TrimSpace(stem)
	}
	return n
}

// ---------------- launching ----------------

// LaunchOptions: how one game is started.
type LaunchOptions struct {
	// Fullscreen: PixelU's own fullscreen (-f). Leave it off when the game
	// window is docked into RetroPixels: that window goes fullscreen instead.
	Fullscreen bool
	// RecordDir and FFmpeg (both or neither): PixelU saves the game's TV
	// picture to RecordDir\Frames\pixelu_framedump<N>.mkv and its sound to
	// RecordDir\Audio\pixelu_dspdump.wav, encoding with that ffmpeg.exe.
	RecordDir, FFmpeg string
}

// GameArgs is PixelU's command line for one game. PixelU is Cemu renamed,
// so these are Cemu's: -g opens a game, -f is fullscreen; the --rp-*
// options are PixelU's own.
func GameArgs(path string, o LaunchOptions) []string {
	args := []string{"-g", path}
	if o.Fullscreen {
		args = append(args, "-f")
	}
	if o.RecordDir != "" && o.FFmpeg != "" {
		args = append(args, "--rp-record", o.RecordDir, "--rp-ffmpeg", o.FFmpeg)
	}
	return args
}

// Command runs PixelU from its own folder (it keeps its settings, saves
// and shader caches in the portable folder next to it).
func Command(exe string, args ...string) *exec.Cmd {
	cmd := exec.Command(exe, args...)
	cmd.Dir = filepath.Dir(exe)
	return cmd
}
