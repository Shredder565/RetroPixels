package supermodel

import (
	"archive/zip"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// The one game the Supermodel tab plays: Star Wars Trilogy Arcade (Sega,
// LucasArts, 1998), Sega Model 3 Step 2.1. Supermodel's Games.xml knows two
// export versions RetroPixels accepts: swtrilgy (Revision A, the parent set)
// and swtrilgya (the first export version, a clone of swtrilgy that only
// replaces the four main program ROMs). The location test (swtrilgyp) isn't
// offered.

// Title is the game's name as people know it.
const Title = "Star Wars Trilogy Arcade"

// ROMFile is one file of a ROM set: Supermodel finds it by its CRC-32.
type ROMFile struct {
	Name  string
	CRC32 uint32
}

type romRegion struct {
	Name     string
	Required bool // the driveboard_program (force feedback CPU) isn't
	Files    []ROMFile
}

// RomSet is one version of the game, as Supermodel's Games.xml (commit
// 24d2ffc) describes it. Supermodel finds the files in a .zip by CRC-32, not
// by name.
type RomSet struct {
	Name    string // the ROM set's (and .zip's) name, as in MAME
	Parent  string
	Version string
	Regions []romRegion
}

// DriveBoardROM is the force feedback joystick's CPU program. It's optional:
// without it the game asks for the feedback stick to be set up (see
// analysis in docs/supermodel.md), so RetroPixels turns force feedback on,
// which runs this program, only when the ROM set has it.
var DriveBoardROM = ROMFile{"epr-21119.ic8", 0x65082B14}

var swtrilgy = RomSet{Name: "swtrilgy", Version: "Export, Revision A", Regions: []romRegion{
	{Name: "crom", Required: true, Files: []ROMFile{{"epr-21382a.20", 0x69BAF117}, {"epr-21381a.19", 0x2DD34E28}, {"epr-21380a.18", 0x780FB4E7}, {"epr-21379a.17", 0x24DC1555}}},
	{Name: "banked_crom", Required: true, Files: []ROMFile{{"mpr-21342.04", 0x339525CE}, {"mpr-21341.03", 0xB2A269E4}, {"mpr-21340.02", 0xAD36040E}, {"mpr-21339.01", 0xC0CE5037}, {"mpr-21346.08", 0xC8733594}, {"mpr-21345.07", 0x6C183A21}, {"mpr-21344.06", 0x87453D76}, {"mpr-21343.05", 0x12552D07}, {"mpr-21350.12", 0x486195E7}, {"mpr-21349.11", 0x3D39454B}, {"mpr-21348.10", 0x1F7CC5F5}, {"mpr-21347.09", 0xECB6B934}}},
	{Name: "vrom", Required: true, Files: []ROMFile{{"mpr-21359.26", 0x34EF4122}, {"mpr-21360.27", 0x2882B95E}, {"mpr-21361.28", 0x9B61C3C1}, {"mpr-21362.29", 0x01A92169}, {"mpr-21363.30", 0xE7D18FED}, {"mpr-21364.31", 0xCB6A5468}, {"mpr-21365.32", 0xAD5449D8}, {"mpr-21366.33", 0xDEFB6B95}, {"mpr-21367.34", 0xDFD51029}, {"mpr-21368.35", 0xAE90FD21}, {"mpr-21369.36", 0xBF17EEB4}, {"mpr-21370.37", 0x2321592A}, {"mpr-21371.38", 0xA68782FD}, {"mpr-21372.39", 0xFC3F4E8B}, {"mpr-21373.40", 0xB76AD261}, {"mpr-21374.41", 0xAE6C4D28}}},
	{Name: "sound_program", Required: true, Files: []ROMFile{{"epr-21383.21", 0x544D1E28}}},
	{Name: "sound_samples", Required: true, Files: []ROMFile{{"mpr-21355.22", 0xC1B2D326}, {"mpr-21357.24", 0x02703FAB}}},
	{Name: "mpeg_program", Required: true, Files: []ROMFile{{"epr-21384.2", 0x12FA4780}}},
	{Name: "mpeg_music", Required: true, Files: []ROMFile{{"mpr-21375.18", 0x735157A9}, {"mpr-21376.20", 0xE635F81E}, {"mpr-21377.22", 0x720621F8}, {"mpr-21378.24", 0x1FCF715E}}},
	{Name: "driveboard_program", Required: false, Files: []ROMFile{DriveBoardROM}},
}}

var swtrilgya = RomSet{Name: "swtrilgya", Parent: "swtrilgy", Version: "Export", Regions: []romRegion{
	{Name: "crom", Required: true, Files: []ROMFile{{"epr-21382.20", 0x0B9C44A0}, {"epr-21381.19", 0xBB5757BF}, {"epr-21380.18", 0x49B182F2}, {"epr-21379.17", 0x61AD51D9}}},
}}

// RomSets lists the versions RetroPixels plays, the owner's own (swtrilgya)
// first.
var RomSets = []RomSet{swtrilgya, swtrilgy}

// SetByName finds a version by its ROM set name.
func SetByName(name string) (RomSet, bool) {
	for _, s := range RomSets {
		if strings.EqualFold(s.Name, name) {
			return s, true
		}
	}
	return RomSet{}, false
}

// merged is the whole game for a set: a clone's own regions, and its
// parent's for the rest (as Supermodel merges them).
func (s RomSet) merged() []romRegion {
	if s.Parent == "" {
		return s.Regions
	}
	p, _ := SetByName(s.Parent)
	own := map[string]romRegion{}
	for _, r := range s.Regions {
		own[r.Name] = r
	}
	var out []romRegion
	for _, r := range p.Regions {
		if o, ok := own[r.Name]; ok {
			out = append(out, o)
			delete(own, r.Name)
		} else {
			out = append(out, r)
		}
	}
	for _, r := range s.Regions {
		if _, ok := own[r.Name]; ok {
			out = append(out, r)
		}
	}
	return out
}

// SetFiles lists every file a version of the game needs (with the optional
// force feedback program too, if asked), its parent's included for a clone.
func SetFiles(name string, withOptional bool) []ROMFile {
	s, ok := SetByName(name)
	if !ok {
		return nil
	}
	var out []ROMFile
	for _, r := range s.merged() {
		if r.Required || withOptional {
			out = append(out, r.Files...)
		}
	}
	return out
}

// Check is what's in a game file.
type Check struct {
	Path   string `json:"path"`
	Set    string `json:"set"`    // swtrilgy or swtrilgya ("" when it's neither)
	OK     bool   `json:"ok"`     // Supermodel has every file it needs
	Parent string `json:"parent"` // the parent .zip also used, when the set is split
	// Missing names the files that aren't there (at most a few).
	Missing     []string `json:"missing,omitempty"`
	MissingN    int      `json:"missingCount,omitempty"`
	DriveBoard  bool     `json:"driveBoard"` // the force feedback program is there
	Problem     string   `json:"problem,omitempty"`
	NeedsParent bool     `json:"needsParent,omitempty"` // a split clone set whose parent .zip isn't next to it
}

// zipCRCs lists the CRC-32s in a .zip (from its directory: nothing is unpacked).
func zipCRCs(path string) (map[uint32]bool, error) {
	r, err := zip.OpenReader(path)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	out := map[uint32]bool{}
	for _, f := range r.File {
		if !f.FileInfo().IsDir() {
			out[f.CRC32] = true
		}
	}
	return out, nil
}

// CheckROM looks at a .zip and says which version of Star Wars Trilogy it
// is and whether Supermodel can play it, as Supermodel decides it: a merged
// or non-merged set has everything in one .zip; a split clone set (only the
// files that differ from the parent) also needs the parent's .zip in the
// same folder (swtrilgy.zip next to swtrilgya.zip).
func CheckROM(path string) Check {
	c := Check{Path: path}
	crcs, err := zipCRCs(path)
	if err != nil {
		c.Problem = "it can't be read as a .zip file (" + err.Error() + ")"
		return c
	}
	has := func(m map[uint32]bool, files []ROMFile) int {
		n := 0
		for _, f := range files {
			if m[f.CRC32] {
				n++
			}
		}
		return n
	}
	// Which version: the one whose own main program ROMs are there.
	var set *RomSet
	best := 0
	for i := range RomSets {
		n := has(crcs, RomSets[i].Regions[0].Files)
		if n > best {
			set, best = &RomSets[i], n
		}
	}
	if set == nil {
		// Maybe the named set with its program ROMs missing.
		stem := strings.TrimSuffix(strings.ToLower(filepath.Base(path)), strings.ToLower(filepath.Ext(path)))
		if s, ok := SetByName(stem); ok {
			set = &s
		} else {
			c.Problem = "it isn't Star Wars Trilogy Arcade (swtrilgy or swtrilgya)"
			return c
		}
	}
	c.Set = set.Name
	all := crcs
	if set.Parent != "" {
		pp := filepath.Join(filepath.Dir(path), set.Parent+".zip")
		if pc, err := zipCRCs(pp); err == nil {
			all = map[uint32]bool{}
			for k := range crcs {
				all[k] = true
			}
			for k := range pc {
				all[k] = true
			}
			c.Parent = pp
		}
	}
	var missing []string
	for _, r := range set.merged() {
		if !r.Required {
			continue
		}
		for _, f := range r.Files {
			if !all[f.CRC32] {
				missing = append(missing, f.Name)
			}
		}
	}
	c.DriveBoard = all[DriveBoardROM.CRC32]
	c.MissingN = len(missing)
	if len(missing) > 4 {
		missing = missing[:4]
	}
	c.Missing = missing
	c.OK = c.MissingN == 0
	if !c.OK {
		if set.Parent != "" && c.Parent == "" && !hasParentFiles(set, crcs) {
			// Only the clone's own files: a split set.
			c.NeedsParent = true
			c.Problem = fmt.Sprintf("%s holds only the files that differ from the main version, so Supermodel also needs %s.zip (the main version's ROM set) in the same folder",
				filepath.Base(path), set.Parent)
		} else {
			list := strings.Join(c.Missing, ", ")
			if c.MissingN > len(c.Missing) {
				list += ", …"
			}
			c.Problem = fmt.Sprintf("%d of the game's files aren't in it (%s), or don't match the version Supermodel expects", c.MissingN, list)
		}
	} else {
		c.Parent = parentIfUsed(set, crcs, c.Parent)
	}
	return c
}

// hasParentFiles: any of the files a clone takes from its parent are there.
func hasParentFiles(set *RomSet, crcs map[uint32]bool) bool {
	p, _ := SetByName(set.Parent)
	own := map[string]bool{}
	for _, r := range set.Regions {
		own[r.Name] = true
	}
	for _, r := range p.Regions {
		if own[r.Name] || !r.Required {
			continue
		}
		for _, f := range r.Files {
			if crcs[f.CRC32] {
				return true
			}
		}
	}
	return false
}

// parentIfUsed keeps the parent's path only when files came from it.
func parentIfUsed(set *RomSet, own map[uint32]bool, parent string) string {
	if parent == "" {
		return ""
	}
	for _, r := range set.merged() {
		for _, f := range r.Files {
			if r.Required && !own[f.CRC32] {
				return parent
			}
		}
	}
	return ""
}

// IsRomFile: a file name the game could be in (swtrilgya.zip, swtrilgy.zip).
func IsRomFile(name string) bool {
	n := strings.ToLower(name)
	for _, s := range RomSets {
		if n == s.Name+".zip" {
			return true
		}
	}
	return false
}

// FindROM looks for the game in folders (and their sub-folders, a few levels
// down) and returns the best copy: one Supermodel can play, the owner's own
// version first; failing that the first one found (its Check says what's
// wrong); "" when there's none.
func FindROM(folders []string) (string, Check) {
	return findIn(folders, 3)
}

func findIn(folders []string, depth int) (string, Check) {
	var found []string
	seen := map[string]bool{}
	for _, d := range folders {
		if d == "" {
			continue
		}
		walk(d, depth, func(p string, e fs.DirEntry) {
			if e.IsDir() || !IsRomFile(e.Name()) {
				return
			}
			k := strings.ToLower(filepath.Clean(p))
			if !seen[k] {
				seen[k] = true
				found = append(found, p)
			}
		})
	}
	order := func(p string) int {
		n := strings.ToLower(filepath.Base(p))
		for i, s := range RomSets {
			if n == s.Name+".zip" {
				return i
			}
		}
		return len(RomSets)
	}
	sort.SliceStable(found, func(i, j int) bool { return order(found[i]) < order(found[j]) })
	var first Check
	firstPath := ""
	for _, p := range found {
		c := CheckROM(p)
		if c.OK {
			return p, c
		}
		// A parent set found here is used through its clone, if that works.
		if firstPath == "" {
			first, firstPath = c, p
		}
	}
	return firstPath, first
}

// romDirWords: folder names that look like they hold games. FindROMOnDrives
// only goes into folders named with one of these, so a whole drive is
// searched in moments.
var romDirWords = []string{"rom", "arcade", "mame", "game", "emu", "sega", "model", "supermodel", "retro"}

func romishDir(name string) bool {
	n := strings.ToLower(name)
	for _, w := range romDirWords {
		if strings.Contains(n, w) {
			return true
		}
	}
	return false
}

// FindROMOnDrives is FindROM's last resort: from each drive root, it goes up
// to five folders down, but only into folders whose names look like they
// hold games (D:\Roms All Systems\Arcade Roms Mame .289\Arcade roms).
func FindROMOnDrives(roots []string) (string, Check) {
	var dirs []string
	var collect func(dir string, depth int)
	collect = func(dir string, depth int) {
		entries, err := os.ReadDir(dir)
		if err != nil {
			return
		}
		for _, e := range entries {
			if !e.IsDir() || skipDirs[strings.ToLower(e.Name())] || !romishDir(e.Name()) {
				continue
			}
			p := filepath.Join(dir, e.Name())
			dirs = append(dirs, p)
			if depth > 1 {
				collect(p, depth-1)
			}
		}
	}
	for _, r := range roots {
		if r != "" {
			collect(r, 5)
		}
	}
	return findIn(dirs, 1)
}

var skipDirs = map[string]bool{"windows": true, "$recycle.bin": true, "system volume information": true,
	"node_modules": true, ".git": true, "appdata": true, "steamapps": true, "program files": true, "program files (x86)": true}

func walk(dir string, depth int, fn func(string, fs.DirEntry)) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	for _, e := range entries {
		p := filepath.Join(dir, e.Name())
		fn(p, e)
		if e.IsDir() && depth > 1 && !skipDirs[strings.ToLower(e.Name())] {
			walk(p, depth-1, fn)
		}
	}
}
