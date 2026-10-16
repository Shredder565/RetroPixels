// Package games is what the RetroPixels Native programs share to open games:
// the systems and their file types, finding the game inside a .zip or .7z
// (arcade ROM sets too), the emulator cores, each system's controls and
// default control maps, and where battery saves, save states and cartridge
// chip programs are kept. RetroPixels-Native.exe (cmd/retropixels-native)
// and the Android app's engine (cmd/retropixels-android) both use it, so a
// game opens the same way in both. The cores register themselves through
// package allcores.
package games

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"retropixels/core"
	"retropixels/internal/apatch"
	"retropixels/sevenzip"
)

// System is one system as web/index.html lists it.
type System struct {
	ID     string
	Label  string
	Exts   []string
	Aspect float64 // picture shape when the core doesn't say (the web player's value)
}

// Systems: the same table (and order) as SYSTEMS in web/index.html.
var Systems = []System{
	{"nes", "NES", []string{"nes", "fds", "unf", "unif"}, 4.0 / 3},
	{"snes", "Super Nintendo", []string{"sfc", "smc", "fig", "swc"}, 4.0 / 3},
	{"gb", "Game Boy / Color", []string{"gb", "gbc"}, 0},
	{"gba", "Game Boy Advance", []string{"gba", "agb"}, 0},
	{"n64", "Nintendo 64", []string{"z64", "n64", "v64"}, 4.0 / 3},
	{"segaMD", "Sega Genesis", []string{"md", "gen", "smd"}, 4.0 / 3},
	{"sms", "Master System", []string{"sms"}, 4.0 / 3},
	{"atari2600", "Atari 2600", []string{"a26"}, 4.0 / 3},
	{"atari7800", "Atari 7800", []string{"a78"}, 4.0 / 3},
	{"arcade", "Arcade", nil, 0},
}

// Maker makes a core for a game.
type Maker func(rom []byte, opts core.Options) (core.Core, error)

// Makers make a core for each system (filled in by package allcores).
var Makers = map[string]Maker{}

// ThisBuild names the program in messages ("This test build plays ...").
var ThisBuild = "this test build"

func ByID(id string) *System {
	for i := range Systems {
		if Systems[i].ID == id {
			return &Systems[i]
		}
	}
	return nil
}

func ForExt(ext string) string {
	for _, s := range Systems {
		for _, e := range s.Exts {
			if e == ext {
				return s.ID
			}
		}
	}
	return ""
}

func Available(id string) bool { return Makers[id] != nil }

// AvailableLabels: "Atari 2600 and NES", for messages.
func AvailableLabels() string {
	var l []string
	for _, s := range Systems {
		if Available(s.ID) {
			l = append(l, s.Label)
		}
	}
	sort.Strings(l)
	switch len(l) {
	case 0:
		return "no" // a build without the cores: "plays no games"
	case 1:
		return l[0]
	}
	return strings.Join(l[:len(l)-1], ", ") + " and " + l[len(l)-1]
}

// AvailableExts: the file types that open, ".nes" ... ".zip", ".7z".
func AvailableExts() []string {
	var out []string
	for _, s := range Systems {
		if Available(s.ID) {
			for _, e := range s.Exts {
				out = append(out, "."+e)
			}
		}
	}
	if Available("atari2600") {
		out = append(out, ".bin")
	}
	return append(out, ".zip", ".7z")
}

// GuessBin: which system a .bin file is for (guessBin in web/index.html).
func GuessBin(b []byte) string {
	if len(b) > 128 && string(b[1:10]) == "ATARI7800" {
		return "atari7800"
	}
	if len(b) > 0x104 && string(b[0x100:0x104]) == "SEGA" {
		return "segaMD"
	}
	if len(b) >= 0x8000 {
		for _, o := range []int{0x1ff0, 0x3ff0, 0x7ff0} {
			if o+8 <= len(b) && string(b[o:o+8]) == "TMR SEGA" {
				return "sms"
			}
		}
	}
	if len(b) <= 65536 {
		return "atari2600"
	}
	return "segaMD"
}

func ExtOf(name string) string {
	e := filepath.Ext(name)
	return strings.ToLower(strings.TrimPrefix(e, "."))
}

// GameNameOf: the file's name without its extension, as the web page uses.
func GameNameOf(path string) string {
	b := filepath.Base(path)
	return strings.TrimSuffix(b, filepath.Ext(b))
}

// PictureAspect: the core's own value, else the web player's for the system,
// else the picture's width / height.
func PictureAspect(coreAspect float64, sys string, w, h int) float64 {
	if coreAspect > 0 {
		return coreAspect
	}
	if s := ByID(sys); s != nil && s.Aspect > 0 {
		return s.Aspect
	}
	if w > 0 && h > 0 {
		return float64(w) / float64(h)
	}
	return 4.0 / 3
}

// NotYetError: a system this build doesn't play; What is "GameCube and Wii
// games are" and so on. Each program words the rest of the message.
type NotYetError struct{ What string }

func (e NotYetError) Error() string { return e.What + " not played by " + ThisBuild }

// ErrAPPatch: the file is an Archipelago patch (made into the game with its
// original ROM by the program, not by Open).
var ErrAPPatch = errors.New("that's an Archipelago patch file")

// MaxROM: no game comes close; stops a stray huge file.
const MaxROM = 64 << 20

// Game is a game file opened and recognised.
type Game struct {
	System string
	ROM    []byte // an arcade game's is its ROM set as a .zip
	Name   string // for saves and recordings: the file's name without extension
	Path   string
}

// ArcadeHooks: what the archive code asks the arcade core (allcores fills
// them in).
type ArcadeHooks struct {
	Identify  func(names []string, sizes []int, zipName string) bool // RPArcade.identify
	Supported func() string                                          // RPArcade.supportedText
}

var Arcade ArcadeHooks

// ArcadeMaxFile: bigger files in an arcade set aren't read (as the web page).
const ArcadeMaxFile = 4 << 20

// Entry is one file inside a .zip or .7z.
type Entry struct {
	Name string
	Size int64
	Read func() ([]byte, error)
}

// PickEntry chooses the game inside an archive like the web page does: the
// biggest file with a console extension (or .bin). arcade=true when the
// archive looks like an arcade ROM set instead.
func PickEntry(entries []Entry) (best *Entry, arcade bool) {
	consoleFile := false
	for i := range entries {
		x := ExtOf(entries[i].Name)
		if ForExt(x) != "" {
			consoleFile = true
		}
		if ForExt(x) != "" || x == "bin" {
			if best == nil || entries[i].Size > best.Size {
				best = &entries[i]
			}
		}
	}
	// No console file: the web page asks its arcade code first. Arcade sets
	// hold many chip files; one or two .bin files are a console game.
	if !consoleFile && (best == nil || len(entries) > 2) {
		return nil, true
	}
	return best, false
}

// OpenOptions change how Open decides.
type OpenOptions struct {
	// ForBin picks the system for a .bin file's contents (nil: GuessBin).
	ForBin func(rom []byte) string
}

func capFirst(s string) string {
	if s == "" {
		return s
	}
	return strings.ToUpper(s[:1]) + s[1:]
}

// Open reads a game file (or an archive holding one) and finds its system.
func Open(path string, o OpenOptions) (*Game, error) {
	name := filepath.Base(path)
	ext := ExtOf(name)
	switch ext {
	case "iso", "gcm", "ciso", "wbfs", "rvz", "wia", "gcz", "wad":
		return nil, NotYetError{"GameCube and Wii games are"}
	}
	if apatch.IsPatchName(name) {
		return nil, ErrAPPatch
	}
	st, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if st.Size() > MaxROM && ext != "zip" && ext != "7z" {
		return nil, fmt.Errorf("%q is too big to be a game for %s", name, ThisBuild)
	}

	g := &Game{Name: GameNameOf(path), Path: path}
	var entries []Entry
	switch ext {
	case "zip":
		entries, err = ZipEntries(path)
	case "7z":
		var cleanup func()
		entries, cleanup, err = SevenZipEntries(path)
		if cleanup != nil {
			defer cleanup()
		}
	}
	if err != nil {
		return nil, fmt.Errorf("couldn't open %q: %v", name, err)
	}
	if ext == "zip" || ext == "7z" {
		// No console file inside: the web page asks its arcade code first.
		if Arcade.Identify != nil && !hasConsoleFile(entries) {
			var names []string
			var sizes []int
			for _, e := range entries {
				names = append(names, e.Name)
				sizes = append(sizes, int(e.Size))
			}
			if Arcade.Identify(names, sizes, name) {
				g.System = "arcade"
				if ext == "zip" {
					g.ROM, err = os.ReadFile(path)
				} else {
					g.ROM, err = Rezip(entries)
				}
				if err != nil {
					return nil, fmt.Errorf("couldn't unpack %q: %v", name, err)
				}
				return g, nil
			}
		}
		e, arcade := PickEntry(entries)
		if arcade {
			if Arcade.Identify != nil {
				var held []string
				for i, x := range entries {
					if i == 30 {
						held = append(held, "…")
						break
					}
					held = append(held, fmt.Sprintf("%s (%s bytes)", x.Name, CommaNum(x.Size)))
				}
				h := strings.Join(held, ", ")
				if h == "" {
					h = "nothing"
				}
				return nil, fmt.Errorf("%q isn't an arcade game RetroPixels knows yet. The games it can play are: %s. "+
					"Load the game's .zip ROM set (keep it zipped), named like the set (for example pacman.zip). This zip holds: %s.",
					name, Arcade.Supported(), h)
			}
			return nil, NotYetError{"Arcade games are"}
		}
		if e == nil {
			return nil, fmt.Errorf("there's no game file inside %q", name)
		}
		if e.Size > MaxROM {
			return nil, fmt.Errorf("the file inside %q is too big to be a game for %s", name, ThisBuild)
		}
		if g.ROM, err = e.Read(); err != nil {
			return nil, fmt.Errorf("couldn't unpack %q: %v", name, err)
		}
		ext = ExtOf(e.Name)
	} else {
		if g.ROM, err = os.ReadFile(path); err != nil {
			return nil, err
		}
	}

	g.System = ForExt(ext)
	if g.System == "" && ext == "bin" {
		if o.ForBin != nil {
			g.System = o.ForBin(g.ROM)
		} else {
			g.System = GuessBin(g.ROM)
		}
	}
	if g.System == "" {
		return nil, fmt.Errorf("couldn't tell which system %q is for.\n\n%s plays %s games (%s).",
			name, capFirst(ThisBuild), AvailableLabels(), strings.Join(AvailableExts(), " "))
	}
	if !Available(g.System) {
		return nil, NotYetError{ByID(g.System).Label + " games are"}
	}
	return g, nil
}

func hasConsoleFile(entries []Entry) bool {
	for _, e := range entries {
		if ForExt(ExtOf(e.Name)) != "" {
			return true
		}
	}
	return false
}

// Rezip packs an archive's files (up to 4 MB each, as the web page reads
// them) into a .zip for the arcade core.
func Rezip(entries []Entry) ([]byte, error) {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, e := range entries {
		if e.Size > ArcadeMaxFile {
			continue
		}
		data, err := e.Read()
		if err != nil {
			return nil, err
		}
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.Name, Method: zip.Store})
		if err != nil {
			return nil, err
		}
		w.Write(data)
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// CommaNum writes 4096 as "4,096".
func CommaNum(n int64) string {
	s := fmt.Sprint(n)
	for i := len(s) - 3; i > 0; i -= 3 {
		s = s[:i] + "," + s[i:]
	}
	return s
}

// ZipEntries lists the files in a .zip.
func ZipEntries(path string) ([]Entry, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil, errors.New("that .zip file looks damaged")
	}
	var out []Entry
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		f := f
		out = append(out, Entry{Name: f.Name, Size: int64(f.UncompressedSize64), Read: func() ([]byte, error) {
			rc, err := f.Open()
			if err != nil {
				return nil, err
			}
			defer rc.Close()
			return io.ReadAll(io.LimitReader(rc, MaxROM+1))
		}})
	}
	return out, nil
}

// SevenZipEntries unpacks the archive into a temporary folder (the 7z reader
// works on whole archives); cleanup removes it.
func SevenZipEntries(path string) ([]Entry, func(), error) {
	list, err := sevenzip.List(path)
	if err != nil {
		return nil, nil, err
	}
	var total uint64
	for _, f := range list {
		total += f.Size
	}
	if total > 1<<30 {
		return nil, nil, errors.New("it's too big to unpack")
	}
	dir, err := os.MkdirTemp("", "rpnative-7z-")
	if err != nil {
		return nil, nil, err
	}
	cleanup := func() { os.RemoveAll(dir) }
	if err := sevenzip.ExtractAll(path, dir, nil); err != nil {
		return nil, cleanup, err
	}
	var out []Entry
	for _, f := range list {
		if f.IsDir {
			continue
		}
		p := filepath.Join(dir, filepath.FromSlash(f.Name))
		out = append(out, Entry{Name: f.Name, Size: int64(f.Size), Read: func() ([]byte, error) { return os.ReadFile(p) }})
	}
	return out, cleanup, nil
}
