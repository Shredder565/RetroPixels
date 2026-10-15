package main

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
	"retropixels/sevenzip"
)

// version is set at build time: -ldflags "-X main.version=2.9.0".
var version = "dev"

// A system as web/index.html lists it.
type system struct {
	ID     string
	Label  string
	Exts   []string
	Aspect float64 // picture shape when the core doesn't say (the web player's value)
}

// systems: the same table (and order) as SYSTEMS in web/index.html.
var systems = []system{
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

// coreMakers make a core for each system; filled in by the files that bring the
// cores (cores_stage1.go, and fakecore.go in test builds).
var coreMakers = map[string]func(rom []byte, opts core.Options) (core.Core, error){}

func systemByID(id string) *system {
	for i := range systems {
		if systems[i].ID == id {
			return &systems[i]
		}
	}
	return nil
}

func systemForExt(ext string) string {
	for _, s := range systems {
		for _, e := range s.Exts {
			if e == ext {
				return s.ID
			}
		}
	}
	return ""
}

func available(id string) bool { return coreMakers[id] != nil }

// availableLabels: "Atari 2600 and NES", for messages.
func availableLabels() string {
	var l []string
	for _, s := range systems {
		if available(s.ID) {
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

// guessBin: which system a .bin file is for (guessBin in web/index.html).
func guessBin(b []byte) string {
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

func extOf(name string) string {
	e := filepath.Ext(name)
	return strings.ToLower(strings.TrimPrefix(e, "."))
}

// gameNameOf: the file's name without its extension, as the web page uses.
func gameNameOf(path string) string {
	b := filepath.Base(path)
	return strings.TrimSuffix(b, filepath.Ext(b))
}

// errNotYet is a friendly "this system isn't in the test build" message.
type errNotYet struct{ what string }

func (e errNotYet) Error() string {
	return e.what + " coming in a later test build; use RetroPixels.exe for now.\n\n" +
		"This test build plays " + availableLabels() + " games."
}

const maxROM = 64 << 20 // no stage-1 game comes close; stops a stray huge file

// loadedGame is a game file opened and recognised.
type loadedGame struct {
	System string
	ROM    []byte
	Name   string // for saves and recordings: the file's name without extension
	Path   string
}

// archiveEntry is one file inside a .zip or .7z.
type archiveEntry struct {
	name string
	size int64
	read func() ([]byte, error)
}

// pickEntry chooses the game inside an archive like the web page does: the
// biggest file with a console extension (or .bin). arcade=true when the
// archive looks like an arcade ROM set instead.
func pickEntry(entries []archiveEntry) (best *archiveEntry, arcade bool) {
	consoleFile := false
	for i := range entries {
		x := extOf(entries[i].name)
		if systemForExt(x) != "" {
			consoleFile = true
		}
		if systemForExt(x) != "" || x == "bin" {
			if best == nil || entries[i].size > best.size {
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

// openGame reads a game file (or an archive holding one) and finds its system.
func openGame(path string) (*loadedGame, error) {
	name := filepath.Base(path)
	ext := extOf(name)
	switch ext {
	case "iso", "gcm", "ciso", "wbfs", "rvz", "wia", "gcz", "wad":
		return nil, errNotYet{"GameCube and Wii games are"}
	}
	if strings.HasPrefix(ext, "ap") && len(ext) > 2 && systemForExt(ext) == "" {
		return nil, errNotYet{"Archipelago patch files are"}
	}
	st, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if st.Size() > maxROM && ext != "zip" && ext != "7z" {
		return nil, fmt.Errorf("%q is too big to be a game for this test build", name)
	}

	g := &loadedGame{Name: gameNameOf(path), Path: path}
	var entries []archiveEntry
	switch ext {
	case "zip":
		entries, err = zipEntries(path)
	case "7z":
		var cleanup func()
		entries, cleanup, err = sevenZipEntries(path)
		if cleanup != nil {
			defer cleanup()
		}
	}
	if err != nil {
		return nil, fmt.Errorf("couldn't open %q: %v", name, err)
	}
	if ext == "zip" || ext == "7z" {
		e, arcade := pickEntry(entries)
		if arcade {
			return nil, errNotYet{"Arcade games are"}
		}
		if e == nil {
			return nil, fmt.Errorf("there's no game file inside %q", name)
		}
		if e.size > maxROM {
			return nil, fmt.Errorf("the file inside %q is too big to be a game for this test build", name)
		}
		if g.ROM, err = e.read(); err != nil {
			return nil, fmt.Errorf("couldn't unpack %q: %v", name, err)
		}
		ext = extOf(e.name)
	} else {
		if g.ROM, err = os.ReadFile(path); err != nil {
			return nil, err
		}
	}

	g.System = systemForExt(ext)
	if g.System == "" && ext == "bin" {
		g.System = guessBin(g.ROM)
	}
	if g.System == "" {
		return nil, fmt.Errorf("couldn't tell which system %q is for.\n\nThis test build plays %s games (%s).",
			name, availableLabels(), strings.Join(availableExts(), " "))
	}
	if !available(g.System) {
		return nil, errNotYet{systemByID(g.System).Label + " games are"}
	}
	return g, nil
}

func availableExts() []string {
	var out []string
	for _, s := range systems {
		if available(s.ID) {
			for _, e := range s.Exts {
				out = append(out, "."+e)
			}
		}
	}
	if available("atari2600") {
		out = append(out, ".bin")
	}
	return append(out, ".zip", ".7z")
}

func zipEntries(path string) ([]archiveEntry, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil, errors.New("that .zip file looks damaged")
	}
	var out []archiveEntry
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		f := f
		out = append(out, archiveEntry{name: f.Name, size: int64(f.UncompressedSize64), read: func() ([]byte, error) {
			rc, err := f.Open()
			if err != nil {
				return nil, err
			}
			defer rc.Close()
			return io.ReadAll(io.LimitReader(rc, maxROM+1))
		}})
	}
	return out, nil
}

// sevenZipEntries unpacks the archive into a temporary folder (the 7z reader
// works on whole archives); cleanup removes it.
func sevenZipEntries(path string) ([]archiveEntry, func(), error) {
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
	var out []archiveEntry
	for _, f := range list {
		if f.IsDir {
			continue
		}
		p := filepath.Join(dir, filepath.FromSlash(f.Name))
		out = append(out, archiveEntry{name: f.Name, size: int64(f.Size), read: func() ([]byte, error) { return os.ReadFile(p) }})
	}
	return out, cleanup, nil
}
