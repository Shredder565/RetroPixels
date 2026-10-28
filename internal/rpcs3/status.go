package rpcs3

import (
	"errors"
	"os"
	"strings"
)

// Status is what both programs show about RPCS3 and its games.
type Status struct {
	Exe             string `json:"exe"`               // "" = not found
	Root            string `json:"root"`              // RPCS3's folder
	Source          string `json:"source,omitempty"`  // SourceChosen, SourceYours or SourceBundled
	Version         string `json:"version,omitempty"` // the bundled RPCS3's build ("" for the player's own)
	InUse           string `json:"inUse,omitempty"`   // which RPCS3 is in use, in words (Describe)
	Firmware        bool   `json:"firmware"`
	FirmwareVersion string `json:"firmwareVersion,omitempty"`
	Pad             string `json:"pad"` // player 1's controller handler ("" = no settings yet)
	Games           []Game `json:"games"`
}

// ExeFor is the RPCS3 these settings use (FindExe), without searching drives.
func (c Config) ExeFor(appDir string) string { return FindExe(c.Folder, c.Exe, appDir) }

// NeedsSearch: the drives haven't been searched for the player's own RPCS3
// yet and nothing of theirs is in use (none found, or only the bundled one),
// so the program should Search once.
func (c Config) NeedsSearch(st Status) bool {
	return c.Folder == "" && (st.Exe == "" || (st.Source == SourceBundled && !c.Detected))
}

// Describe says which RPCS3 is in use, for the PS3 tab and menu.
func (s Status) Describe() string {
	switch {
	case s.Exe == "":
		return "RPCS3 not found"
	case s.Source == SourceBundled:
		v := s.Version
		if v == "" {
			v = "RPCS3"
		}
		return v + ", the one that comes with RetroPixels (" + s.Root + ")"
	case s.Source == SourceChosen:
		return "your chosen RPCS3 (" + s.Root + ")"
	}
	return "your own RPCS3 (" + s.Root + ")"
}

// Look reads RPCS3's state and its games for these settings.
func Look(c Config, appDir string) Status {
	var st Status
	st.Exe, st.Source = WhichExe(c.Folder, c.Exe, appDir)
	if st.Exe == "" {
		st.InUse = st.Describe()
		return st
	}
	st.Root = Root(st.Exe)
	if st.Source == SourceBundled {
		st.Version = BundledVersion(st.Root)
	}
	st.InUse = st.Describe()
	st.Firmware, st.FirmwareVersion = Firmware(st.Root)
	st.Pad = Player1Handler(st.Root)
	st.Games = Games(st.Root)
	for i := range st.Games {
		st.Games[i].Played = c.Played[strings.ToUpper(st.Games[i].TitleID)]
	}
	return st
}

// Find picks a game by title ID.
func (s Status) Find(titleID string) (Game, bool) {
	for _, g := range s.Games {
		if strings.EqualFold(g.TitleID, titleID) {
			return g, true
		}
	}
	return Game{}, false
}

// Ready says why a game can't start yet (nil when it can): the system
// software or its licence is missing.
func Ready(root string, g Game) error {
	if ok, _ := Firmware(root); !ok {
		return errors.New(FirmwareMessage)
	}
	if _, err := os.Stat(g.Eboot); err != nil {
		return errors.New("the game's EBOOT.BIN isn't there any more (" + g.Eboot + ")")
	}
	if l := CheckLicence(root, g.Eboot); !l.OK() {
		return errors.New(l.Message())
	}
	return nil
}

// NotFoundMessage: RPCS3 wasn't found.
const NotFoundMessage = "RetroPixels didn't find RPCS3 (" + ExeName + "), and this RetroPixels has no RPCS3 folder of its own. Use \"Choose RPCS3 folder…\" and pick the folder " + ExeName + " is in."
