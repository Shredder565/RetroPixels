package main

import (
	"errors"

	"retropixels/internal/apatch"
	"retropixels/internal/games"
)

// Opening games: the systems, archives and arcade sets are internal/games's
// (shared with the Android app's engine); these are the names this program
// has always used for them.

// version is set at build time: -ldflags "-X main.version=2.9.0".
var version = "dev"

// A system as web/index.html lists it.
type system struct {
	ID     string
	Label  string
	Exts   []string
	Aspect float64 // picture shape when the core doesn't say (the web player's value)
}

// systems: the same table (and order) as SYSTEMS in web/index.html
// (games.Systems). Tests add to it; syncSystems hands it to internal/games
// before each use.
var systems = func() []system {
	out := make([]system, len(games.Systems))
	for i, s := range games.Systems {
		out[i] = system(s)
	}
	return out
}()

func syncSystems() {
	out := make([]games.System, len(systems))
	for i, s := range systems {
		out[i] = games.System(s)
	}
	games.Systems = out
}

// coreMakers make a core for each system (the same map as games.Makers,
// filled in by internal/games/allcores, and fakecore.go in test builds).
var coreMakers = games.Makers

func systemByID(id string) *system {
	for i := range systems {
		if systems[i].ID == id {
			return &systems[i]
		}
	}
	return nil
}

func systemForExt(ext string) string { syncSystems(); return games.ForExt(ext) }

func available(id string) bool { return games.Available(id) }

// availableLabels: "Atari 2600 and NES", for messages.
func availableLabels() string { syncSystems(); return games.AvailableLabels() }

// guessBin: which system a .bin file is for (guessBin in web/index.html).
func guessBin(b []byte) string { return games.GuessBin(b) }

func extOf(name string) string { return games.ExtOf(name) }

// gameNameOf: the file's name without its extension, as the web page uses.
func gameNameOf(path string) string { return games.GameNameOf(path) }

// errNotYet is a friendly "this system isn't in the test build" message.
type errNotYet struct{ what string }

func (e errNotYet) Error() string {
	return e.what + " coming in a later test build; use RetroPixels.exe for now.\n\n" +
		"This test build plays " + availableLabels() + " games."
}

// loadedGame is a game file opened and recognised.
type loadedGame struct {
	System string
	ROM    []byte // an arcade game's is its ROM set as a .zip
	Name   string // for saves and recordings: the file's name without extension
	Path   string
	Dips   map[string]int // arcade machine settings the player chose
	AP     *apatch.Patch  // the Archipelago patch the game was made from (archipelago.go), or nil
	// chip programs chosen for this game but not yet kept (see firmwareFor)
	Firmware map[string][]byte
}

// archiveEntry is one file inside a .zip or .7z.
type archiveEntry struct {
	name string
	size int64
	read func() ([]byte, error)
}

// openGame reads a game file (or an archive holding one) and finds its system.
func openGame(path string) (*loadedGame, error) {
	syncSystems()
	g, err := games.Open(path, games.OpenOptions{ForBin: systemForBin})
	var ny games.NotYetError
	switch {
	case errors.As(err, &ny):
		return nil, errNotYet{ny.What}
	case errors.Is(err, games.ErrAPPatch):
		return nil, errAPPatchFile // the window makes the game (openAPPatch)
	case err != nil:
		return nil, err
	}
	return &loadedGame{System: g.System, ROM: g.ROM, Name: g.Name, Path: g.Path}, nil
}

// rezip packs an archive's files (up to 4 MB each, as the web page reads
// them) into a .zip for the arcade core.
func rezip(entries []archiveEntry) ([]byte, error) {
	es := make([]games.Entry, len(entries))
	for i, e := range entries {
		es[i] = games.Entry{Name: e.name, Size: e.size, Read: e.read}
	}
	return games.Rezip(es)
}

// commaNum writes 4096 as "4,096".
func commaNum(n int64) string { return games.CommaNum(n) }

func availableExts() []string { syncSystems(); return games.AvailableExts() }
