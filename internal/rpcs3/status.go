package rpcs3

import (
	"errors"
	"os"
	"strings"
)

// Status is what both programs show about RPCS3 and its games.
type Status struct {
	Exe             string `json:"exe"`  // "" = not found
	Root            string `json:"root"` // RPCS3's folder
	Firmware        bool   `json:"firmware"`
	FirmwareVersion string `json:"firmwareVersion,omitempty"`
	Pad             string `json:"pad"` // player 1's controller handler ("" = no settings yet)
	Games           []Game `json:"games"`
}

// ExeFor is the RPCS3 these settings use (FindExe), without searching drives.
func (c Config) ExeFor(appDir string) string { return FindExe(c.Folder, c.Exe, appDir) }

// Look reads RPCS3's state and its games for these settings.
func Look(c Config, appDir string) Status {
	st := Status{Exe: c.ExeFor(appDir)}
	if st.Exe == "" {
		return st
	}
	st.Root = Root(st.Exe)
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
const NotFoundMessage = "RetroPixels didn't find RPCS3 (" + ExeName + "). Use \"Choose RPCS3 folder…\" and pick the folder " + ExeName + " is in."
