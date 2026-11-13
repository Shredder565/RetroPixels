package mame

// Any ROM set, whichever MAME it was made for. MAME looks for a game's chip
// files in a zip named after the game, and a set only "works" in the MAME
// versions whose list of chips it matches. People's sets come renamed,
// merged (clones inside the parent's zip), split (a clone needing its
// parent's zip), with the BIOS in another zip, or made for an older MAME.
//
// RetroPixels recognises a set by its files' CRCs, with the history of every
// arcade machine's chips in every MAME version since 0.171 (internal/romdb).
// From that it knows which game the zip is, which MAME versions accept it,
// and which files it needs from your other zips (the parent, the BIOS).
// When the set as it is won't do, it puts together a copy that will, named
// and laid out as that MAME version wants, in a cache folder (your own zips
// are never changed), and starts the game from there. When no MAME on this
// PC accepts the set but an official release does, it says which one to
// download (RetroPixels.exe and RetroPixels Native download it by
// themselves); when none does, it names the files that are missing.

import (
	"archive/zip"
	"encoding/xml"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/internal/romdb"
	"retropixels/sevenzip"
)

// SetFile is one file in a ROM set archive.
type SetFile struct {
	Archive string // the .zip or .7z it's in
	Name    string // its name in there
	Size    int64
	CRC     uint32
}

// ReadSetFiles lists an archive's files with their CRCs, from the archive's
// directory (nothing is unpacked).
func ReadSetFiles(path string) ([]SetFile, error) {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".zip":
		z, err := zip.OpenReader(path)
		if err != nil {
			return nil, err
		}
		defer z.Close()
		var out []SetFile
		for _, f := range z.File {
			if f.FileInfo().IsDir() {
				continue
			}
			out = append(out, SetFile{Archive: path, Name: f.Name, Size: int64(f.UncompressedSize64), CRC: f.CRC32})
		}
		return out, nil
	case ".7z":
		l, err := sevenzip.List(path)
		if err != nil {
			return nil, err
		}
		var out []SetFile
		for _, f := range l {
			if f.IsDir {
				continue
			}
			c, ok := f.CRC()
			if !ok && f.Size > 0 {
				continue
			}
			out = append(out, SetFile{Archive: path, Name: f.Name, Size: int64(f.Size), CRC: c})
		}
		return out, nil
	}
	return nil, errors.New("not a .zip or .7z file")
}

// ---------------- the pool: every file in your ROM folders, by CRC

type poolEntry struct {
	mod   time.Time
	size  int64
	files []SetFile
}

var (
	poolMu    sync.Mutex
	poolCache = map[string]poolEntry{}
)

// filesOf is ReadSetFiles, remembered until the archive changes.
func filesOf(path string) []SetFile {
	st, err := os.Stat(path)
	if err != nil {
		return nil
	}
	poolMu.Lock()
	e, ok := poolCache[path]
	poolMu.Unlock()
	if ok && e.mod.Equal(st.ModTime()) && e.size == st.Size() {
		return e.files
	}
	files, _ := ReadSetFiles(path)
	poolMu.Lock()
	poolCache[path] = poolEntry{st.ModTime(), st.Size(), files}
	poolMu.Unlock()
	return files
}

// Pool is the files of every ROM set in some folders, by CRC.
type Pool map[uint32][]SetFile

// NewPool reads the archives in the folders (two levels deep, like the
// library) and the extra archives.
func NewPool(folders []string, extra ...string) Pool {
	p := Pool{}
	seen := map[string]bool{}
	add := func(path string) {
		k := strings.ToLower(filepath.Clean(path))
		if seen[k] {
			return
		}
		seen[k] = true
		for _, f := range filesOf(path) {
			p[f.CRC] = append(p[f.CRC], f)
		}
	}
	for _, x := range extra {
		add(x)
	}
	for _, dir := range folders {
		walk(dir, 2, func(path string, d fs.DirEntry) {
			if !d.IsDir() && IsRomSet(path) {
				add(path)
			}
		})
	}
	return p
}

// ---------------- what a machine needs

// need is the files a machine needs in one MAME version: its own and those
// of a BIOS it runs on, with the BIOS choice that's complete.
type need struct {
	roms []romdb.Rom
	bios string // a BIOS choice to ask MAME for (-bios), "" = its default
}

// biosChoices: the machine's own files for each BIOS choice (index -1 = the
// files every choice has).
func choiceFiles(m *romdb.Machine, v int) (common []romdb.Rom, byChoice map[int][]romdb.Rom) {
	byChoice = map[int][]romdb.Rom{}
	for _, r := range m.Roms {
		if !r.In(v) {
			continue
		}
		if r.Bios < 0 {
			common = append(common, r)
		} else {
			byChoice[r.Bios] = append(byChoice[r.Bios], r)
		}
	}
	return
}

func defaultChoice(m *romdb.Machine, byChoice map[int][]romdb.Rom) int {
	if m.DefaultBios != "" {
		for i, b := range m.Bios {
			if b == m.DefaultBios {
				return i
			}
		}
	}
	best := -1
	for k := range byChoice {
		if best < 0 || k < best {
			best = k
		}
	}
	return best
}

// needsOf works out what a machine needs in version v, given what's there
// (have reports whether a CRC can be found). A BIOS choice other than the
// default is taken when only that one is complete.
func needsOf(db *romdb.DB, m *romdb.Machine, v int, have func(uint32) bool) need {
	var n need
	add := func(x *romdb.Machine, pickChoice bool) {
		common, byChoice := choiceFiles(x, v)
		n.roms = append(n.roms, common...)
		if len(byChoice) == 0 {
			return
		}
		def := defaultChoice(x, byChoice)
		pick := def
		complete := func(l []romdb.Rom) bool {
			for _, r := range l {
				if !have(r.CRC) {
					return false
				}
			}
			return true
		}
		if pickChoice && !complete(byChoice[def]) {
			var keys []int
			for k := range byChoice {
				keys = append(keys, k)
			}
			sort.Ints(keys)
			for _, k := range keys {
				if complete(byChoice[k]) {
					pick = k
					break
				}
			}
		}
		n.roms = append(n.roms, byChoice[pick]...)
		if pick != def && pick < len(x.Bios) {
			n.bios = x.Bios[pick]
		}
	}
	add(m, true)
	// the BIOS it runs on: the first BIOS set up its parents
	seen := map[string]bool{m.Name: true}
	for p := db.Machine(m.Parent); p != nil && !seen[p.Name]; p = db.Machine(p.Parent) {
		seen[p.Name] = true
		if p.BiosRoot && p.In(v) {
			add(p, n.bios == "")
			break
		}
	}
	return n
}

func missingOf(roms []romdb.Rom, have func(uint32) bool) []romdb.Rom {
	var out []romdb.Rom
	seen := map[uint32]bool{}
	for _, r := range roms {
		if !have(r.CRC) && !seen[r.CRC] {
			seen[r.CRC] = true
			out = append(out, r)
		}
	}
	return out
}

// ---------------- recognising a set

// Identify says which machine an archive is, from its files' CRCs: the
// machine the most of whose files (in its newest version) are in the
// archive; on a tie the one the archive is named after, then the one the
// most of the archive's files belong to, then a parent before its clones.
// nil when no machine has its files.
func Identify(db *romdb.DB, files []SetFile, archiveName string) *romdb.Machine {
	inZip := map[uint32]bool{}
	for _, f := range files {
		inZip[f.CRC] = true
	}
	short := ShortName(archiveName)
	cands := map[*romdb.Machine]bool{}
	for c := range inZip {
		for _, m := range db.WithCRC(c) {
			cands[m] = true
		}
	}
	type scored struct {
		m        *romdb.Machine
		cover    float64 // share of the machine's files that are in the zip
		explains int     // the zip's files that belong to the machine
	}
	var list []scored
	for m := range cands {
		if m.BiosRoot {
			continue
		}
		v := m.To
		var mine []uint32
		seen := map[uint32]bool{}
		for _, r := range m.Roms {
			if r.In(v) && r.Bios < 0 && !seen[r.CRC] {
				seen[r.CRC] = true
				mine = append(mine, r.CRC)
			}
		}
		if len(mine) == 0 {
			continue
		}
		in := 0
		for _, c := range mine {
			if inZip[c] {
				in++
			}
		}
		// files of the machine in any version count for explaining the zip
		all := map[uint32]bool{}
		for _, r := range m.Roms {
			all[r.CRC] = true
		}
		ex := 0
		for c := range inZip {
			if all[c] {
				ex++
			}
		}
		list = append(list, scored{m, float64(in) / float64(len(mine)), ex})
	}
	if len(list) == 0 {
		return nil
	}
	sort.Slice(list, func(a, b int) bool {
		x, y := list[a], list[b]
		if x.cover != y.cover {
			return x.cover > y.cover
		}
		// the machine the archive is named after (a merged set holds its
		// clones' files too)
		if (x.m.Name == short) != (y.m.Name == short) {
			return x.m.Name == short
		}
		if x.explains != y.explains {
			return x.explains > y.explains
		}
		// a parent before its clones
		if (x.m.Parent == "") != (y.m.Parent == "") {
			return x.m.Parent == ""
		}
		return x.m.Name < y.m.Name
	})
	return list[0].m
}

// Recognise says which game a set named some other way is (for the
// library): the machine most of whose files are in it, or nil.
func Recognise(path string) *romdb.Machine {
	db := History()
	if db == nil {
		return nil
	}
	files := filesOf(path)
	if len(files) == 0 {
		return nil
	}
	m := Identify(db, files, path)
	if m == nil {
		return nil
	}
	in := map[uint32]bool{}
	for _, f := range files {
		in[f.CRC] = true
	}
	mine, found := 0, 0
	seen := map[uint32]bool{}
	for _, r := range m.Roms {
		if r.In(m.To) && r.Bios < 0 && !seen[r.CRC] {
			seen[r.CRC] = true
			mine++
			if in[r.CRC] {
				found++
			}
		}
	}
	if mine == 0 || found*2 < mine {
		return nil
	}
	return m
}

// ---------------- resolving: which game, which MAME, which files

// Resolution is what RetroPixels worked out about a set.
type Resolution struct {
	Machine *romdb.Machine
	// the MAME on this PC to play it in, or nil (see Download / Missing)
	Install *Install
	Version string      // that MAME's version, or the version to download
	Bios    string      // -bios choice, "" = the default
	Files   []SetFile   // where each file the game needs is, when it's complete
	Roms    []romdb.Rom // the files, in the same order as Files
	// set when the archive, under its own name, already is the game's set
	// for that version (nothing to put together)
	AsIs bool
	// no MAME here accepts the set, but this official version does
	Download string
	// no version accepts the set: the files missing in the version that
	// comes closest (Version)
	Missing []romdb.Rom

	full bool // the archive holds all of the game's own files
}

// versionIndex maps a MAME version to the history's version index: a version
// newer than the history's newest counts as the newest (its listings are
// assumed unchanged), older than the oldest is -1.
func versionIndex(db *romdb.DB, ver string) int {
	if ver == "" {
		return -1
	}
	if i := db.Version(ver); i >= 0 {
		return i
	}
	if CompareVersions(ver, db.Versions[db.Newest()]) > 0 {
		return db.Newest()
	}
	if CompareVersions(ver, db.Versions[0]) < 0 {
		return -1
	}
	// a version between two the history has (not expected): the one before
	best := -1
	for i, x := range db.Versions {
		if CompareVersions(x, ver) <= 0 {
			best = i
		}
	}
	return best
}

// Resolve recognises a set and works out how to play it. preferred is the
// MAME to use when it accepts the set (the game's pick or the default); the
// other installs are tried newest first. ok is false when the history
// doesn't know the set (then it's played the old way, by its file name).
func Resolve(db *romdb.DB, path string, installs []Install, preferred string, pool Pool, downloadable func(version string) bool) (Resolution, bool) {
	return ResolveLazy(db, path, installs, preferred, func() Pool { return pool }, downloadable)
}

// ResolveLazy is Resolve reading the other ROM sets (pool) only when the
// archive alone doesn't play in a MAME here.
func ResolveLazy(db *romdb.DB, path string, installs []Install, preferred string, pool func() Pool, downloadable func(version string) bool) (Resolution, bool) {
	r, ok := resolve(db, path, installs, preferred, Pool{}, downloadable)
	if !ok || (r.Install != nil && r.full) {
		return r, ok // the archive alone holds the whole game, for a MAME here
	}
	return resolve(db, path, installs, preferred, pool(), downloadable)
}

func resolve(db *romdb.DB, path string, installs []Install, preferred string, pool Pool, downloadable func(version string) bool) (Resolution, bool) {
	files := filesOf(path)
	if len(files) == 0 {
		return Resolution{}, false
	}
	inZip := map[uint32]SetFile{}
	for _, f := range files {
		if _, ok := inZip[f.CRC]; !ok {
			inZip[f.CRC] = f
		}
	}
	find := func(c uint32) (SetFile, bool) {
		if f, ok := inZip[c]; ok {
			return f, true
		}
		if l := pool[c]; len(l) > 0 {
			return l[0], true
		}
		return SetFile{}, false
	}
	have := func(c uint32) bool { _, ok := find(c); return ok }
	short := ShortName(path)

	// the MAMEs on this PC by the history's version: the preferred one
	// first, then newest first
	list := append([]Install(nil), installs...)
	sort.SliceStable(list, func(a, b int) bool {
		pa, pb := strings.EqualFold(list[a].Exe, preferred), strings.EqualFold(list[b].Exe, preferred)
		if pa != pb {
			return pa
		}
		return CompareVersions(list[a].Version, list[b].Version) > 0
	})
	installedAt := map[int][]Install{}
	for _, in := range list {
		if v := versionIndex(db, in.Version); v >= 0 && exists(in.Exe) {
			installedAt[v] = append(installedAt[v], in)
		}
	}

	// every machine with a file in the archive, in every version that has
	// it: the ones the files make complete, scored by how well the archive
	// matches them
	cands := map[*romdb.Machine]bool{}
	for c := range inZip {
		for _, m := range db.WithCRC(c) {
			if !m.BiosRoot || m.Name == short {
				cands[m] = true
			}
		}
	}
	if len(cands) == 0 {
		return Resolution{}, false
	}
	type pick struct {
		m        *romdb.Machine
		v        int
		cover    float64 // share of the machine's own files that are in the archive
		explains int     // the archive's files that are the machine's
		in       *Install
		named    bool
	}
	var best *pick
	better := func(x, y *pick) bool { // x better than y
		if y == nil {
			return true
		}
		if x.cover != y.cover {
			return x.cover > y.cover
		}
		// a MAME here before one to download
		if (x.in != nil) != (y.in != nil) {
			return x.in != nil
		}
		if x.named != y.named {
			return x.named
		}
		if x.explains != y.explains {
			return x.explains > y.explains
		}
		if x.in != nil && y.in != nil {
			xp, yp := strings.EqualFold(x.in.Exe, preferred), strings.EqualFold(y.in.Exe, preferred)
			if xp != yp {
				return xp
			}
		}
		if x.v != y.v {
			return x.v > y.v
		}
		return x.m.Name < y.m.Name
	}
	for m := range cands {
		for v := m.From; v <= m.To && v <= db.Newest(); v++ {
			var in *Install
			if l := installedAt[v]; len(l) > 0 {
				in = &l[0]
			}
			if in == nil && (downloadable == nil || !downloadable(db.Versions[v])) {
				continue
			}
			if !m.In(v) {
				continue
			}
			own, found, explains := 0, 0, 0
			seen := map[uint32]bool{}
			for _, r := range m.Roms {
				if !r.In(v) || seen[r.CRC] {
					continue
				}
				seen[r.CRC] = true
				if _, ok := inZip[r.CRC]; ok {
					explains++
				}
				if r.Bios < 0 {
					own++
					if _, ok := inZip[r.CRC]; ok {
						found++
					}
				}
			}
			if own == 0 || explains == 0 {
				continue
			}
			p := &pick{m: m, v: v, cover: float64(found) / float64(own), explains: explains, in: in, named: m.Name == short}
			if !better(p, best) {
				continue
			}
			if len(missingOf(needsOf(db, m, v, have).roms, have)) > 0 {
				continue
			}
			best = p
		}
	}
	if best != nil {
		m, v := best.m, best.v
		n := needsOf(db, m, v, have)
		r := Resolution{Machine: m, Install: best.in, Version: db.Versions[v], Bios: n.bios}
		if best.in != nil {
			r.Version = best.in.Version
		} else {
			r.Download = db.Versions[v]
		}
		asIs := short == m.Name
		seen := map[uint32]bool{}
		for _, rom := range n.roms {
			if seen[rom.CRC] {
				continue
			}
			seen[rom.CRC] = true
			f, _ := find(rom.CRC)
			r.Files = append(r.Files, f)
			r.Roms = append(r.Roms, rom)
			if _, ok := inZip[rom.CRC]; !ok {
				asIs = false
			}
		}
		r.AsIs = asIs && r.Bios == ""
		r.full = best.cover == 1
		return r, true
	}
	// nothing accepts it: the game it looks most like, its closest version
	// and what's missing there
	m := Identify(db, files, path)
	if m == nil {
		return Resolution{}, false
	}
	bestV, bestMissing := -1, []romdb.Rom(nil)
	for v := m.To; v >= m.From; v-- {
		miss := missingOf(needsOf(db, m, v, have).roms, have)
		if bestV < 0 || len(miss) < len(bestMissing) {
			bestV, bestMissing = v, miss
		}
	}
	r := Resolution{Machine: m, Missing: bestMissing}
	if bestV >= 0 {
		r.Version = db.Versions[bestV]
	}
	return r, true
}

// MissingText describes the files a set lacks, for people.
func (r Resolution) MissingText() string {
	title := r.Machine.Desc
	if title == "" {
		title = r.Machine.Name
	}
	var names []string
	for i, f := range r.Missing {
		if i == 8 {
			names = append(names, fmt.Sprintf("and %d more", len(r.Missing)-8))
			break
		}
		names = append(names, fmt.Sprintf("%s (CRC %08x)", f.Name, f.CRC))
	}
	where := ""
	if p := r.Machine.Parent; p != "" {
		where = fmt.Sprintf(" If they're in another set (such as %s.zip), put it in one of your ROM folders.", p)
	}
	return fmt.Sprintf("This set is %s (%s), but %d of its files aren't in it or in your other ROM sets, so no MAME version can play it. The closest is MAME %s, which also needs: %s.%s",
		title, r.Machine.Name, len(r.Missing), r.Version, strings.Join(names, ", "), where)
}

// ---------------- putting a set together

// SetsDir is where put-together sets go: %LOCALAPPDATA%\RetroPixels\MAME
// sets (RETRO_MAME_SETS overrides it, for tests).
func SetsDir() string {
	if p := os.Getenv("RETRO_MAME_SETS"); p != "" {
		return p
	}
	d, err := os.UserCacheDir()
	if err != nil {
		d = filepath.Dir(ConfigPath())
	}
	return filepath.Join(d, AppFolder, "MAME sets")
}

// Build writes the game's set, with every file it needs under MAME's names,
// to dir\<game>.zip, unless an identical one is already there. Files are
// copied as they are packed (zip) or unpacked once (7z).
func (r Resolution) Build(dir string) (string, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	out := filepath.Join(dir, r.Machine.Name+".zip")
	if sameSet(out, r.Roms) {
		return out, nil
	}
	tmp := out + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return "", err
	}
	zw := zip.NewWriter(f)
	used := map[string]bool{}
	open := map[string]*zip.ReadCloser{}
	unpacked := map[string]string{} // .7z -> the folder it's unpacked in
	defer func() {
		for _, z := range open {
			z.Close()
		}
		for _, d := range unpacked {
			os.RemoveAll(d)
		}
	}()
	fail := func(err error) (string, error) {
		zw.Close()
		f.Close()
		os.Remove(tmp)
		return "", err
	}
	for i, src := range r.Files {
		name := r.Roms[i].Name
		if used[name] {
			name = fmt.Sprintf("%08x_%s", r.Roms[i].CRC, name)
		}
		used[name] = true
		if strings.EqualFold(filepath.Ext(src.Archive), ".zip") {
			z := open[src.Archive]
			if z == nil {
				z, err = zip.OpenReader(src.Archive)
				if err != nil {
					return fail(err)
				}
				open[src.Archive] = z
			}
			var zf *zip.File
			for _, x := range z.File {
				if x.Name == src.Name {
					zf = x
					break
				}
			}
			if zf == nil {
				return fail(fmt.Errorf("%s isn't in %s any more", src.Name, src.Archive))
			}
			h := zf.FileHeader
			h.Name = name
			h.Modified = time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
			w, err := zw.CreateRaw(&h)
			if err != nil {
				return fail(err)
			}
			rd, err := zf.OpenRaw()
			if err != nil {
				return fail(err)
			}
			if _, err := io.Copy(w, rd); err != nil {
				return fail(err)
			}
			continue
		}
		data, err := read7z(src, unpacked)
		if err != nil {
			return fail(err)
		}
		w, err := zw.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Deflate, Modified: time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)})
		if err != nil {
			return fail(err)
		}
		if _, err := w.Write(data); err != nil {
			return fail(err)
		}
	}
	if err := zw.Close(); err != nil {
		f.Close()
		os.Remove(tmp)
		return "", err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return "", err
	}
	os.Remove(out)
	if err := os.Rename(tmp, out); err != nil {
		return "", err
	}
	return out, nil
}

// sameSet: the zip already holds exactly these files.
func sameSet(path string, roms []romdb.Rom) bool {
	z, err := zip.OpenReader(path)
	if err != nil {
		return false
	}
	defer z.Close()
	want := map[uint32]bool{}
	for _, r := range roms {
		want[r.CRC] = true
	}
	got := map[uint32]bool{}
	for _, f := range z.File {
		got[f.CRC32] = true
	}
	if len(got) != len(want) {
		return false
	}
	for c := range want {
		if !got[c] {
			return false
		}
	}
	return true
}

// read7z reads one file of a .7z. 7z packs files together, so each archive
// is unpacked whole once, into a temporary folder kept in unpacked.
func read7z(f SetFile, unpacked map[string]string) ([]byte, error) {
	tmp, ok := unpacked[f.Archive]
	if !ok {
		d, err := os.MkdirTemp("", "rp7z-")
		if err != nil {
			return nil, err
		}
		if err := sevenzip.ExtractAll(f.Archive, d, nil); err != nil {
			os.RemoveAll(d)
			return nil, err
		}
		unpacked[f.Archive] = d
		tmp = d
	}
	data, err := os.ReadFile(filepath.Join(tmp, filepath.FromSlash(f.Name)))
	if err != nil {
		return nil, err
	}
	if crc32.ChecksumIEEE(data) != f.CRC {
		return nil, fmt.Errorf("%s in %s is damaged", f.Name, filepath.Base(f.Archive))
	}
	return data, nil
}

// ---------------- the history

// History is the built-in ROM history (nil if it can't be read).
func History() *romdb.DB {
	db, err := romdb.Load()
	if err != nil {
		return nil
	}
	return db
}

// ---------------- the library's note

var (
	fixMu    sync.Mutex
	fixPool  Pool
	fixAt    time.Time
	fixNotes = map[string]fixNote{}
)

type fixNote struct {
	mod, at time.Time
	text    string
}

// fixText says what RetroPixels does with a set no MAME here plays as it
// is ("" when the history doesn't know it). Remembered per archive.
func (c Config) fixText(g Game) string {
	st, err := os.Stat(g.Path)
	if err != nil {
		return ""
	}
	fixMu.Lock()
	defer fixMu.Unlock()
	if n, ok := fixNotes[g.Path]; ok && n.mod.Equal(st.ModTime()) && time.Since(n.at) < time.Minute {
		return n.text
	}
	db := History()
	if db == nil {
		return ""
	}
	pool := func() Pool {
		if fixPool == nil || time.Since(fixAt) > time.Minute {
			fixPool, fixAt = NewPool(c.Folders), time.Now()
		}
		return fixPool
	}
	def, _ := c.DefaultInstall()
	res, ok := ResolveLazy(db, g.Path, c.Installs, def.Exe, pool, func(v string) bool {
		return DownloadsAvailable && CompareVersions(v, "0.171") >= 0
	})
	text := ""
	switch {
	case !ok:
	case res.Install != nil:
		text = "Plays in " + res.Install.Label() + " (as " + res.Machine.Name + ")"
	case res.Download != "":
		text = "Plays in MAME " + res.Download + ", downloaded when you start it"
	case len(res.Missing) > 0:
		text = fmt.Sprintf("Can't play: %d file(s) missing (%s)", len(res.Missing), res.Missing[0].Name)
	}
	fixNotes[g.Path] = fixNote{st.ModTime(), time.Now(), text}
	return text
}

// ---------------- device files, from the MAME itself

// DeviceRom is a file one of a game's devices needs (a sound chip's or a
// protection chip's own ROM, kept in a zip named after the device).
type DeviceRom struct {
	Device string
	Name   string
	CRC    uint32
}

type xmlList struct {
	Machines []struct {
		Name     string `xml:"name,attr"`
		IsDevice string `xml:"isdevice,attr"`
		Roms     []struct {
			Name     string `xml:"name,attr"`
			CRC      string `xml:"crc,attr"`
			Status   string `xml:"status,attr"`
			Optional string `xml:"optional,attr"`
		} `xml:"rom"`
	} `xml:"machine"`
}

var (
	devMu    sync.Mutex
	devCache = map[string][]DeviceRom{}
)

// deviceRoms asks a MAME which files the game's devices need (-listxml
// lists the game and each device it uses). Remembered per MAME and game.
func deviceRoms(in Install, game string) ([]DeviceRom, error) {
	key := strings.ToLower(in.Exe) + "|" + game
	devMu.Lock()
	l, ok := devCache[key]
	devMu.Unlock()
	if ok {
		return l, nil
	}
	out, err := run(in.Exe, 30*time.Second, "-listxml", game)
	i := strings.Index(out, "<?xml")
	if i < 0 {
		if err == nil {
			err = errors.New("no list")
		}
		return nil, err
	}
	l, err = parseDeviceRoms(out[i:])
	if err != nil {
		return nil, err
	}
	devMu.Lock()
	devCache[key] = l
	devMu.Unlock()
	return l, nil
}

func parseDeviceRoms(x string) ([]DeviceRom, error) {
	var list xmlList
	if err := xml.Unmarshal([]byte(x), &list); err != nil {
		return nil, err
	}
	var out []DeviceRom
	for _, m := range list.Machines {
		if m.IsDevice != "yes" {
			continue
		}
		for _, r := range m.Roms {
			if r.CRC == "" || r.Status == "nodump" || r.Optional == "yes" {
				continue
			}
			c, err := strconv.ParseUint(r.CRC, 16, 32)
			if err != nil {
				continue
			}
			out = append(out, DeviceRom{Device: m.Name, Name: r.Name, CRC: uint32(c)})
		}
	}
	return out, nil
}

// AddDevices asks the chosen MAME which files the game's devices need and
// adds the ones found in your sets to the files to put together. It returns
// those that are nowhere (the game can't start without them).
func (r *Resolution) AddDevices(pool func() Pool) []DeviceRom {
	if r.Install == nil {
		return nil
	}
	devs, err := deviceRoms(*r.Install, r.Machine.Name)
	if err != nil {
		return nil // the MAME will say what's wrong itself
	}
	have := map[uint32]bool{}
	for _, f := range r.Files {
		have[f.CRC] = true
	}
	var missing []DeviceRom
	for _, d := range devs {
		if have[d.CRC] {
			continue
		}
		l := pool()[d.CRC]
		if len(l) == 0 {
			missing = append(missing, d)
			continue
		}
		have[d.CRC] = true
		// device files already in a zip named after the device are found
		// there by MAME; others go into the game's set
		if strings.EqualFold(ShortName(l[0].Archive), d.Device) {
			continue
		}
		r.Files = append(r.Files, l[0])
		r.Roms = append(r.Roms, romdb.Rom{Name: d.Name, CRC: d.CRC, Bios: -1})
		r.AsIs = false
	}
	return missing
}

// DevicesText describes missing device files, for people.
func DevicesText(game string, l []DeviceRom) string {
	var names []string
	for i, d := range l {
		if i == 6 {
			names = append(names, fmt.Sprintf("and %d more", len(l)-6))
			break
		}
		names = append(names, fmt.Sprintf("%s (CRC %08x, in %s.zip)", d.Name, d.CRC, d.Device))
	}
	if len(l) == 1 {
		return fmt.Sprintf("%s also needs the ROM of a chip on its board, which MAME keeps in a set of its own, and it isn't in your ROM folders: %s. Put that set (the .zip named after the chip) in one of your ROM folders.",
			game, names[0])
	}
	return fmt.Sprintf("%s also needs the ROMs of chips on its board, which MAME keeps in sets of their own, and they aren't in your ROM folders: %s. Put those sets (the .zip files named after the chips) in one of your ROM folders.",
		game, strings.Join(names, ", "))
}
