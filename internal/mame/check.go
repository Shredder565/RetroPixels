package mame

import (
	"bufio"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// Checking ROM sets: a set only plays in the MAME versions whose list of
// chips it matches (MAME adds chips it needs, and fixes bad dumps, over the
// years). Each version is asked once per set with -verifyroms, and the
// answer is kept, so the library can say which games play in which version,
// and a game whose default version can't play it starts in one that can.

// Check results.
const (
	Good = "good" // every file is there and right (or the best dump there is)
	Bad  = "bad"  // files are missing or wrong for this version
)

var reVerify = regexp.MustCompile(`(?i)^romset\s+(\S+)(?:\s+\[[^\]]*\])?\s+(?:is\s+)?(good|best available|bad|correct|incorrect|not found)\b`)

// ParseVerify reads -verifyroms output: each set's result. Sets MAME
// doesn't know aren't listed.
func ParseVerify(out string) map[string]string {
	res := map[string]string{}
	sc := bufio.NewScanner(strings.NewReader(out))
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	for sc.Scan() {
		m := reVerify.FindStringSubmatch(strings.TrimSpace(sc.Text()))
		if m == nil {
			continue
		}
		switch strings.ToLower(m[2]) {
		case "good", "best available", "correct":
			res[strings.ToLower(m[1])] = Good
		default:
			res[strings.ToLower(m[1])] = Bad
		}
	}
	return res
}

var (
	checkMu    sync.Mutex
	checkCache map[string]string // exeKey|setKey → Good/Bad
)

func checksFile() string { return filepath.Join(filepath.Dir(ConfigPath()), "MAME checks.json") }

func exeKey(exe string) string {
	st, err := os.Stat(exe)
	if err != nil {
		return ""
	}
	return strings.ToLower(exe) + "|" + itoa(st.Size()) + "|" + itoa(st.ModTime().Unix())
}

func setKey(g Game) string {
	st, err := os.Stat(g.Path)
	if err != nil {
		return ""
	}
	return strings.ToLower(g.Path) + "|" + itoa(st.Size()) + "|" + itoa(st.ModTime().Unix())
}

func loadChecks() {
	if checkCache != nil {
		return
	}
	checkCache = map[string]string{}
	if b, err := os.ReadFile(checksFile()); err == nil {
		_ = json.Unmarshal(b, &checkCache)
	}
}

func saveChecks() {
	b, _ := json.Marshal(checkCache)
	os.MkdirAll(filepath.Dir(checksFile()), 0o755)
	tmp := checksFile() + ".tmp"
	if os.WriteFile(tmp, b, 0o644) == nil {
		os.Rename(tmp, checksFile())
	}
}

// Checked is what's known of a set in one MAME, without asking it: Good,
// Bad or "" (not checked yet).
func Checked(in Install, g Game) string {
	ek, sk := exeKey(in.Exe), setKey(g)
	if ek == "" || sk == "" {
		return ""
	}
	checkMu.Lock()
	defer checkMu.Unlock()
	loadChecks()
	return checkCache[ek+"|"+sk]
}

// CheckGames asks each MAME about the sets it hasn't checked yet (a few
// seconds per version the first time) and returns, for each game, its
// result in each version by exe. Call it in the background.
func CheckGames(c Config, games []Game) map[string]map[string]string {
	out := map[string]map[string]string{}
	for _, in := range c.Installs {
		ek := exeKey(in.Exe)
		if ek == "" {
			continue
		}
		var todo []Game
		for _, g := range games {
			if r := Checked(in, g); r != "" {
				put(out, g.Short, in.Exe, r)
			} else if setKey(g) != "" {
				todo = append(todo, g)
			}
		}
		for len(todo) > 0 {
			n := min(len(todo), 50) // keep the command line short
			batch := todo[:n]
			todo = todo[n:]
			var names []string
			dirs := append([]string(nil), c.Folders...)
			for _, g := range batch {
				names = append(names, g.Short)
				dirs = append(dirs, filepath.Dir(g.Path))
			}
			args := append([]string{"-rompath", RomPath(in.Exe, "", dirs), "-verifyroms"}, names...)
			o, _ := run(in.Exe, 2*time.Minute, args...) // bad sets end it with an error code
			res := ParseVerify(o)
			if len(res) == 0 {
				continue // this version can't check (or isn't MAME): leave it unknown
			}
			checkMu.Lock()
			loadChecks()
			for _, g := range batch {
				r, ok := res[g.Short]
				if !ok {
					if !strings.Contains(strings.ToLower(o), "'"+g.Short+"'") {
						continue // no answer for it: ask again next time
					}
					r = Bad // "No matching systems found for 'x'": this version doesn't have it
				}
				checkCache[ek+"|"+setKey(g)] = r
				put(out, g.Short, in.Exe, r)
			}
			saveChecks()
			checkMu.Unlock()
		}
	}
	return out
}

func put(m map[string]map[string]string, short, exe, r string) {
	if m[short] == nil {
		m[short] = map[string]string{}
	}
	m[short][exe] = r
}

// Best is the MAME a game starts in: its own pick; else the default, unless
// the default is known not to play it and another version is known to.
func (c Config) Best(g Game) (Install, bool) {
	if exe, ok := c.Games[strings.ToLower(g.Short)]; ok {
		if i, ok := c.Find(exe); ok {
			return i, true
		}
	}
	def, ok := c.DefaultInstall()
	if !ok || Checked(def, g) != Bad {
		return def, ok
	}
	for _, in := range c.Installs { // newest first
		if exists(in.Exe) && Checked(in, g) == Good {
			return in, true
		}
	}
	return def, true
}

// GameStatus is what the library shows about one game.
type GameStatus struct {
	Exe     string   `json:"exe"`               // the MAME it starts in (see Best)
	Label   string   `json:"label"`             // that MAME's name
	Result  string   `json:"result"`            // Good, Bad or "" (not checked)
	PlaysIn []string `json:"playsIn,omitempty"` // every version known to play it
}

// Status says which MAME a game starts in and whether it's known to play.
func (c Config) Status(g Game) GameStatus {
	var s GameStatus
	if in, ok := c.Best(g); ok {
		s = GameStatus{Exe: in.Exe, Label: in.Label(), Result: Checked(in, g)}
	}
	for _, in := range c.Installs {
		if exists(in.Exe) && Checked(in, g) == Good {
			s.PlaysIn = append(s.PlaysIn, in.Label())
		}
	}
	return s
}

// Text is the status in words: "Plays in MAME 0.281", or why it won't.
func (s GameStatus) Text() string {
	switch {
	case s.Label == "":
		return ""
	case s.Result == Good:
		return "Plays in " + s.Label
	case s.Result == Bad && len(s.PlaysIn) > 0:
		return "Won't start in " + s.Label + ": plays in " + strings.Join(s.PlaysIn, ", ")
	case s.Result == Bad:
		return "Won't start in " + s.Label + ": files missing, or the set is for another MAME version"
	}
	return ""
}
