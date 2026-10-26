package rpcs3

// The games RPCS3 has: those installed on its PS3 hard drive
// (dev_hdd0\game\<title ID>, PSN games and other installed packages) and
// the disc games it was told about (games.yml), each with its PARAM.SFO.

import (
	"bufio"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Game is one PS3 game RPCS3 can start.
type Game struct {
	Title    string    `json:"title"`
	TitleID  string    `json:"titleId"`  // NPEB00149, BLUS30443…
	Category string    `json:"category"` // HG (installed), DG (disc)…
	Version  string    `json:"version,omitempty"`
	Dir      string    `json:"dir"`   // the game's folder
	Eboot    string    `json:"eboot"` // what RPCS3 is given: …\USRDIR\EBOOT.BIN
	Disc     bool      `json:"disc,omitempty"`
	Played   time.Time `json:"played,omitempty"`
	// Licence: what the game's EBOOT.BIN needs (see CheckLicence).
	Licence LicenceCheck `json:"licence"`
}

// playable: the kinds of PARAM.SFO that are games RPCS3 boots. Others in
// dev_hdd0\game are game data: disc games' installed parts and updates
// (GD), save tools, themes and so on.
var playable = map[string]bool{
	"HG": true, // installed game (PSN, and disc games installed to the hard drive)
	"DG": true, // disc game
	"1P": true, // PlayStation classic
	"2P": true, // PlayStation 2 classic
	"MN": true, // PSP minis
	"PE": true, // PSP remaster
}

// gameAt reads one game folder (where PARAM.SFO and USRDIR are). ok is
// false when it isn't a game RPCS3 starts.
func gameAt(dir string, disc bool) (Game, bool) {
	sfo, err := ReadSFO(filepath.Join(dir, "PARAM.SFO"))
	if err != nil {
		return Game{}, false
	}
	cat := sfo.String("CATEGORY")
	if !playable[cat] {
		return Game{}, false
	}
	eboot := filepath.Join(dir, "USRDIR", "EBOOT.BIN")
	if !isFile(eboot) {
		return Game{}, false
	}
	g := Game{Title: sfo.String("TITLE"), TitleID: sfo.String("TITLE_ID"), Category: cat,
		Version: sfo.String("APP_VER"), Dir: dir, Eboot: eboot, Disc: disc}
	if g.TitleID == "" {
		g.TitleID = filepath.Base(dir)
	}
	if g.Title == "" {
		g.Title = g.TitleID
	}
	return g, true
}

// Games lists the games in the RPCS3 folder root: installed ones first,
// then disc games from games.yml, each title ID once, sorted by name.
func Games(root string) []Game {
	var out []Game
	seen := map[string]bool{}
	add := func(g Game) {
		k := strings.ToUpper(g.TitleID)
		if seen[k] {
			return
		}
		seen[k] = true
		g.Licence = CheckLicence(root, g.Eboot)
		out = append(out, g)
	}
	if entries, err := os.ReadDir(filepath.Join(root, "dev_hdd0", "game")); err == nil {
		for _, e := range entries {
			if e.IsDir() {
				if g, ok := gameAt(filepath.Join(root, "dev_hdd0", "game", e.Name()), false); ok {
					add(g)
				}
			}
		}
	}
	for _, d := range DiscGames(root) {
		if g, ok := gameAt(filepath.Join(d, "PS3_GAME"), true); ok {
			add(g)
		}
	}
	sort.SliceStable(out, func(a, b int) bool { return strings.ToLower(out[a].Title) < strings.ToLower(out[b].Title) })
	return out
}

// GamesYML is the list of disc game folders RPCS3 keeps ("BLUS30443:
// D:/PS3/God of War III/"): in its config folder in newer builds, next to
// the program in older ones.
func GamesYML(root string) string {
	for _, p := range []string{filepath.Join(root, "config", "games.yml"), filepath.Join(root, "games.yml")} {
		if isFile(p) {
			return p
		}
	}
	return filepath.Join(root, "config", "games.yml")
}

// DiscGames reads games.yml: the folder of each disc game (the folder that
// has PS3_GAME in it).
func DiscGames(root string) []string {
	f, err := os.Open(GamesYML(root))
	if err != nil {
		return nil
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		_, v, ok := strings.Cut(sc.Text(), ":")
		if !ok {
			continue
		}
		v = strings.TrimSpace(v)
		v = strings.Trim(v, `"'`)
		if v == "" {
			continue
		}
		v = filepath.FromSlash(v)
		if !filepath.IsAbs(v) {
			v = filepath.Join(root, v)
		}
		out = append(out, filepath.Clean(v))
	}
	return out
}

// ---------------- the PS3 system software ----------------

// Firmware tells whether the PS3 system software is installed in RPCS3
// (RPCS3's File > Install Firmware with the player's PS3UPDAT.PUP), and its
// version ("4.89", "" when unknown).
func Firmware(root string) (bool, string) {
	if !isFile(filepath.Join(root, "dev_flash", "vsh", "module", "vsh.self")) {
		return false, ""
	}
	b, err := os.ReadFile(filepath.Join(root, "dev_flash", "vsh", "etc", "version.txt"))
	if err != nil {
		return true, ""
	}
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "release:"); ok {
			v, _, _ = strings.Cut(v, ":")
			v = strings.TrimLeft(v, "0")
			if strings.HasPrefix(v, ".") {
				v = "0" + v
			}
			// "4.8900" → "4.89"
			if i := strings.IndexByte(v, '.'); i >= 0 {
				v = strings.TrimRight(v, "0")
				if strings.HasSuffix(v, ".") {
					v += "0"
				}
			}
			return true, v
		}
	}
	return true, ""
}

// FirmwareMessage says what the player has to do when the system software
// isn't installed.
const FirmwareMessage = "RPCS3 needs the PlayStation 3 system software, which isn't installed in it yet. " +
	"Get PS3UPDAT.PUP from PlayStation's own system software page (or your PS3's update file), " +
	"then use \"Install PS3 system software…\" here (or File > Install Firmware in RPCS3). " +
	"RetroPixels doesn't download or include it."
