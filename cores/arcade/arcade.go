// Package arcade runs classic arcade boards: a Go port of web/emu/arcade.js.
//
// An arcade game comes as a .zip "ROM set": one file per chip on the game's
// circuit board (program ROMs, graphics ROMs, colour PROMs...). This package
// finds which game a set is (by the files' names, with their sizes as a
// check), builds the right board, turns its picture upright for
// vertical-monitor games, and gives the native app the same core.Core
// interface the consoles use. The boards are registered with Register, by
// this package's own board files and by other packages (the Konami boards).
//
// # Writing a board
//
// A board is a BoardDef (the static fields of a JS board class) whose New
// makes the running Board:
//
//	func init() { arcade.Register(&arcade.BoardDef{ID: "pacman", ..., New: newPacman}) }
//
// New gets the Game that was picked, the set's files (lower-case base names;
// Region and FindFile handle the files' other names) and Opts. Opts.Dips is a
// map the framework changes when the player changes a setting, so a board
// reads it when the game reads its switches (or on reset), as the JS boards
// read their dips object.
//
// The board's Screen is Width*Height pixels before rotation, each 0x00RRGGBB
// (RGB makes them). SetInput hands the board one Pad per controller (at least
// two; three or four for boards whose Players is 3 or 4); the framework keeps
// changing these same maps, so a board may keep them, as the JS boards keep
// the objects. TakeAudio returns mono samples at Opts.SampleRate (Resampler
// helps). A board with battery-backed RAM or NVRAM implements SRAMer.
package arcade

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"sort"
	"strconv"
	"strings"
	"sync"

	"retropixels/core"
)

// ButtonNames are the control names boards may use (RPArcade.BUTTON_NAMES);
// the player's controller and keyboard defaults are set up for these.
var ButtonNames = []string{"Up", "Down", "Left", "Right", "Fire", "Jump", "Button 1", "Button 2", "Button 3", "Button 4",
	"Coin", "Coin 2", "Start (1P)", "Start (2P)", "Service"}

// DefaultButtons are the buttons of a board that doesn't list its own.
var DefaultButtons = []string{"Up", "Down", "Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)"}

// Next as a File's Offset places the file right after the previous file of
// the same role (a JS entry without an offset).
const Next = -1

// File is one chip file of a game's set.
type File struct {
	Name     string   // e.g. "pacman.6e" (lower case)
	Alt      []string // other names for the same chip
	Size     int
	Role     string // what the chip is, in the board's own words ("cpu", "tiles"...)
	Offset   int    // where it goes in its role's region; Next = after the previous one
	Optional bool   // the board can run without it
	Lane     int    // board-specific (the Simpsons' interleaved graphics ROMs)
}

// DipOption is one setting of a DIP switch.
type DipOption struct {
	Label string
	Value int
}

// Dip is one machine setting (DIP switch).
type Dip struct {
	ID      string
	Label   string
	Options []DipOption
	Default int
}

// Game is one game a board runs. The zero values of the override fields mean
// "the board's".
type Game struct {
	ID     string // the set's name, e.g. "pacman"
	Title  string
	Year   int
	Maker  string
	Parent string // the set it's a clone of, for messages
	Files  []File
	Loose  bool // tolerant matching (see LooseFiles)

	Rotate     int // 0 = the board's; 360 = not turned
	Width      int
	Height     int
	FPS        float64
	Aspect     float64
	Dips       []Dip
	Buttons    []string
	Players    int
	Characters []string
}

// Pad is one controller: button name -> held. Missing names are not held.
type Pad map[string]bool

// Opts are what a board is made with.
type Opts struct {
	SampleRate int
	Dips       map[string]int // every setting's value; changed in place by the framework
	SRAM       []byte         // what SRAM() returned last time, or nil
}

// Files are a set's files: lower-case base name -> bytes.
type Files map[string][]byte

// Board is one running board.
type Board interface {
	RunFrame()
	Screen() []uint32 // Width*Height, unrotated, 0x00RRGGBB
	SetInput(pads []Pad)
	TakeAudio() []float32
	Reset()
	SaveState() ([]byte, error)
	LoadState([]byte) error
}

// SRAMer is implemented by boards with battery-backed RAM or NVRAM.
type SRAMer interface {
	SRAM() []byte
}

// BoardDef is a board family: the static fields of a JS board class.
type BoardDef struct {
	ID         string // "pacman"
	Title      string // "Namco Pac-Man"
	Width      int    // the picture as the board draws it (before rotation)
	Height     int
	Rotate     int     // 0, 90, 180 or 270: clockwise turn that shows it upright
	Aspect     float64 // the monitor's shape for the unrotated picture (0 = 4/3)
	FPS        float64
	Buttons    []string
	Dips       []Dip
	Players    int      // controllers read (0 = 2)
	Characters []string // who players 1-4 play as (4-player cabinets)
	Games      []Game
	New        func(g *Game, files Files, opts Opts) (Board, error)
}

// ---------------------------------------------------------------- registry

// boardOrder is the order web/index.html loads the board scripts in, which is
// the order RPArcade.boards has (it decides ties between games).
var boardOrder = []string{"pacman", "galaxian", "invaders", "dkong", "tmnt", "tmnt2", "simpsons"}

var (
	regMu  sync.Mutex
	boards []*BoardDef
)

// Register adds a board family. Call it from an init function.
func Register(b *BoardDef) {
	regMu.Lock()
	defer regMu.Unlock()
	rank := func(id string) int {
		for i, x := range boardOrder {
			if x == id {
				return i
			}
		}
		return len(boardOrder)
	}
	for i, x := range boards {
		if x.ID == b.ID {
			boards[i] = b
			return
		}
	}
	boards = append(boards, b)
	sort.SliceStable(boards, func(i, j int) bool { return rank(boards[i].ID) < rank(boards[j].ID) })
}

// Boards returns the registered boards.
func Boards() []*BoardDef {
	regMu.Lock()
	defer regMu.Unlock()
	return append([]*BoardDef(nil), boards...)
}

// Entry is one game of one board.
type Entry struct {
	Board *BoardDef
	Game  *Game
}

// AllGames lists every game of every board, in order.
func AllGames() []Entry {
	var out []Entry
	for _, b := range Boards() {
		for i := range b.Games {
			out = append(out, Entry{b, &b.Games[i]})
		}
	}
	return out
}

// SupportedText: "Pac-Man (pacman.zip), Galaxian (galaxian.zip), ...".
func SupportedText() string {
	seen := map[string]bool{}
	var names []string
	for _, e := range AllGames() {
		if !seen[e.Game.ID] {
			seen[e.Game.ID] = true
			names = append(names, fmt.Sprintf("%s (%s.zip)", e.Game.Title, e.Game.ID))
		}
	}
	if len(names) == 0 {
		return "none yet"
	}
	return strings.Join(names, ", ")
}

// ---------------------------------------------------------------- files

// BaseName is a file's name without its folders, in lower case.
func BaseName(n string) string {
	if i := strings.LastIndexAny(n, `\/`); i >= 0 {
		n = n[i+1:]
	}
	return strings.ToLower(n)
}

// fileSet is a set's files in the zip's order: name -> size (and the bytes,
// when read).
type fileSet struct {
	names []string
	size  map[string]int
	data  Files
}

func newFileSet() *fileSet { return &fileSet{size: map[string]int{}, data: Files{}} }

// add keeps the first file of each base name (normFiles in arcade.js).
func (s *fileSet) add(name string, size int, data []byte) {
	b := BaseName(name)
	if b == "" {
		return
	}
	if _, ok := s.size[b]; ok {
		return
	}
	s.names = append(s.names, b)
	s.size[b] = size
	if data != nil {
		s.data[b] = data
	}
}

func (s *fileSet) clone() *fileSet {
	o := &fileSet{names: append([]string(nil), s.names...), size: make(map[string]int, len(s.size)), data: make(Files, len(s.data))}
	for k, v := range s.size {
		o.size[k] = v
	}
	for k, v := range s.data {
		o.data[k] = v
	}
	return o
}

// set puts a file under a name (a new name goes last, as Map.set does).
func (s *fileSet) set(name string, size int, data []byte) {
	if _, ok := s.size[name]; !ok {
		s.names = append(s.names, name)
	}
	s.size[name] = size
	if data != nil {
		s.data[name] = data
	}
}

// find: the name in the set for one entry (its own name or another of its names).
func (s *fileSet) find(f *File) (string, bool) {
	if _, ok := s.size[strings.ToLower(f.Name)]; ok {
		return strings.ToLower(f.Name), true
	}
	for _, n := range f.Alt {
		if _, ok := s.size[strings.ToLower(n)]; ok {
			return strings.ToLower(n), true
		}
	}
	return "", false
}

// FindFile returns the file for one entry of a game's list (its own name or
// one of its other names), or nil.
func FindFile(f *File, files Files) []byte {
	if d := files[strings.ToLower(f.Name)]; d != nil {
		return d
	}
	for _, n := range f.Alt {
		if d := files[strings.ToLower(n)]; d != nil {
			return d
		}
	}
	return nil
}

type wrongSize struct {
	name         string
	size, actual int
}

type checkResult struct {
	missing []string
	wrong   []wrongSize
	found   int
	ok      bool
}

// check: what's missing or the wrong size for one game.
func check(g *Game, s *fileSet) checkResult {
	var c checkResult
	for i := range g.Files {
		f := &g.Files[i]
		n, ok := s.find(f)
		if !ok {
			if !f.Optional {
				c.missing = append(c.missing, f.Name)
			}
			continue
		}
		if s.size[n] != f.Size {
			if !f.Optional {
				c.wrong = append(c.wrong, wrongSize{f.Name, f.Size, s.size[n]})
			}
			continue
		}
		c.found++
	}
	c.ok = len(c.missing) == 0 && len(c.wrong) == 0
	return c
}

// nameParts: the parts of a file name that tolerant matching compares.
func nameParts(n string) map[string]bool {
	out := map[string]bool{}
	isAZ := func(c byte) bool { return c >= 'a' && c <= 'z' }
	isNum := func(c byte) bool { return c >= '0' && c <= '9' }
	for _, t := range strings.FieldsFunc(strings.ToLower(n), func(r rune) bool { return !(r < 128 && (isAZ(byte(r)) || isNum(byte(r)))) }) {
		out[t] = true
		// the leftmost letters-then-digits ending: "963a28" -> "a28"
		for i := 0; i < len(t); i++ {
			j := i
			for j < len(t) && isAZ(t[j]) {
				j++
			}
			if j == i || j == len(t) {
				continue
			}
			k := j
			for k < len(t) && isNum(t[k]) {
				k++
			}
			if k == len(t) {
				out[t[i:]] = true
				break
			}
		}
		if len(t) > 3 {
			out[t[len(t)-3:]] = true
		}
	}
	return out
}

// looseFiles: tolerant matching for games marked Loose (RPArcade.looseFiles):
// a new set where every listed entry that could be paired is under its listed
// name, or nil if nothing changed.
func looseFiles(g *Game, files *fileSet) *fileSet {
	out := files.clone()
	used := map[string]bool{}
	var todo []*File
	for i := range g.Files {
		f := &g.Files[i]
		hit := ""
		for _, n := range append([]string{f.Name}, f.Alt...) {
			ln := strings.ToLower(n)
			if sz, ok := files.size[ln]; ok && sz == f.Size {
				hit = ln
				break
			}
		}
		if hit != "" {
			used[hit] = true
		} else {
			todo = append(todo, f)
		}
	}
	if len(todo) == 0 {
		return nil
	}
	free := func() []string {
		var l []string
		for _, k := range files.names {
			if !used[k] {
				l = append(l, k)
			}
		}
		sort.Strings(l)
		return l
	}
	// 1) the closest name of the right size
	for _, f := range append([]*File(nil), todo...) {
		want := nameParts(f.Name)
		best, bestScore, tie := "", 0, false
		for _, k := range free() {
			if files.size[k] != f.Size {
				continue
			}
			score := 0
			for p := range nameParts(k) {
				if want[p] {
					score++
				}
			}
			if score > bestScore {
				best, bestScore, tie = k, score, false
			} else if score == bestScore {
				tie = true
			}
		}
		if best != "" && !tie {
			out.set(strings.ToLower(f.Name), files.size[best], files.data[best])
			used[best] = true
			for i, x := range todo {
				if x == f {
					todo = append(todo[:i], todo[i+1:]...)
					break
				}
			}
		}
	}
	// 2) by size and order
	var sizes []int
	seen := map[int]bool{}
	for _, f := range todo {
		if !seen[f.Size] {
			seen[f.Size] = true
			sizes = append(sizes, f.Size)
		}
	}
	for _, size := range sizes {
		var want []*File
		for _, f := range todo {
			if f.Size == size {
				want = append(want, f)
			}
		}
		var have []string
		for _, k := range free() {
			if files.size[k] == size {
				have = append(have, k)
			}
		}
		if len(have) < len(want) {
			continue
		}
		for i, f := range want {
			out.set(strings.ToLower(f.Name), files.size[have[i]], files.data[have[i]])
			used[have[i]] = true
		}
	}
	return out
}

// LooseFiles is RPArcade.looseFiles for a set of files: every listed entry of
// the game that could be paired is under its listed name, or nil if nothing
// changed. (Names are taken in sorted order; the set's own order only matters
// for messages.)
func LooseFiles(g *Game, files Files) Files {
	s := newFileSet()
	var names []string
	for k := range files {
		names = append(names, k)
	}
	sort.Strings(names)
	for _, k := range names {
		s.add(k, len(files[k]), files[k])
	}
	out := looseFiles(g, s)
	if out == nil {
		return nil
	}
	return out.data
}

// Region gathers all files of one role into one array at their offsets (in
// list order when the offset is Next); the rest is 0xff.
func Region(g *Game, files Files, role string, size int) []byte {
	type part struct {
		f  *File
		at int
	}
	var parts []part
	end, pos := 0, 0
	for i := range g.Files {
		f := &g.Files[i]
		if f.Role != role {
			continue
		}
		at := pos
		if f.Offset != Next {
			at = f.Offset
		}
		pos = at + f.Size
		end = max(end, pos)
		parts = append(parts, part{f, at})
	}
	out := make([]byte, max(size, end))
	for i := range out {
		out[i] = 0xff
	}
	for _, p := range parts {
		if d := FindFile(p.f, files); d != nil {
			copy(out[p.at:], d)
		}
	}
	return out
}

// ---------------------------------------------------------------- identify

var copyNumber = struct{ paren, dash func(string) string }{
	// "tmnt (1)", "tmnt [2]"
	paren: func(s string) string {
		if len(s) < 3 {
			return s
		}
		cl := s[len(s)-1]
		if cl != ')' && cl != ']' {
			return s
		}
		i := len(s) - 2
		for i >= 0 && s[i] >= '0' && s[i] <= '9' {
			i--
		}
		if i == len(s)-2 || i < 0 || (s[i] != '(' && s[i] != '[') {
			return s
		}
		j := i
		for j > 0 && isSpace(s[j-1]) {
			j--
		}
		return s[:j]
	},
	// "tmnt-2", "tmnt_2", "tmnt 2"
	dash: func(s string) string {
		n := len(s)
		if n >= 2 && s[n-1] >= '0' && s[n-1] <= '9' && (s[n-2] == '-' || s[n-2] == '_' || s[n-2] == ' ') {
			return s[:n-2]
		}
		return s
	},
}

func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'
}

func zipBase(zipName string) string {
	z := BaseName(zipName)
	if i := strings.LastIndexByte(z, '.'); i >= 0 {
		z = z[:i]
	}
	return z
}

func identify(files *fileSet, zipName string) *Entry {
	zip := zipBase(zipName)
	games := AllGames()
	pick := func(list []Entry, needOk bool) *Entry {
		var best *Entry
		bestScore := -1
		for i := range list {
			c := check(list[i].Game, files)
			if needOk && !c.ok {
				continue
			}
			score := c.found
			if c.ok {
				score += 10000
			}
			if score > bestScore {
				best, bestScore = &list[i], score
			}
		}
		return best
	}
	var ids []string
	if zip != "" {
		ids = []string{zip, strings.TrimSpace(copyNumber.dash(copyNumber.paren(zip)))}
	}
	for _, id := range ids {
		var named []Entry
		for _, g := range games {
			if g.Game.ID == id {
				named = append(named, g)
			}
		}
		if len(named) > 0 {
			return pick(named, false)
		}
	}
	if full := pick(games, true); full != nil {
		return full
	}
	// The zip has some other name: take the game most of its files belong to,
	// by name, or, for games marked loose, by size.
	var best *Entry
	bestScore := 0.0
	for i := range games {
		g := games[i].Game
		var need []*File
		for j := range g.Files {
			if !g.Files[j].Optional {
				need = append(need, &g.Files[j])
			}
		}
		if len(need) == 0 {
			continue
		}
		byName := float64(check(g, files).found) / float64(len(need))
		bySize := 0.0
		if g.Loose {
			var have []int
			for _, k := range files.names {
				have = append(have, files.size[k])
			}
			n := 0
			for _, f := range need {
				for x, sz := range have {
					if sz == f.Size {
						have = append(have[:x], have[x+1:]...)
						n++
						break
					}
				}
			}
			bySize = float64(n) / float64(len(need))
		}
		score := 0.0
		if byName >= 0.3 {
			score = byName
		}
		if bySize >= 0.8 && bySize > score {
			score = bySize
		}
		if score > bestScore {
			best, bestScore = &games[i], score
		}
	}
	return best
}

// Identify tells which game a set is from its files' names and sizes (and the
// zip's name: a zip named after a game picks that game even if files are
// missing, so the error can say which). ok is false when it's no game this
// package knows.
func Identify(names []string, sizes []int, zipName string) (e Entry, ok bool) {
	s := newFileSet()
	for i, n := range names {
		s.add(n, sizes[i], nil)
	}
	if hit := identify(s, zipName); hit != nil {
		return *hit, true
	}
	return Entry{}, false
}

// ---------------------------------------------------------------- messages

// commas writes 4096 as "4,096" (toLocaleString in an English browser).
func commas(n int) string {
	s := strconv.Itoa(n)
	neg := strings.HasPrefix(s, "-")
	if neg {
		s = s[1:]
	}
	var b strings.Builder
	for i, c := range s {
		if i > 0 && (len(s)-i)%3 == 0 {
			b.WriteByte(',')
		}
		b.WriteRune(c)
	}
	if neg {
		return "-" + b.String()
	}
	return b.String()
}

// setError is the friendly error when a set can't be used.
func setError(g *Game, files *fileSet, zipName string) error {
	c := check(g, files)
	zip := g.ID + ".zip"
	if zipName != "" {
		zip = BaseName(zipName)
	}
	var parts []string
	if len(c.missing) > 0 {
		what := "these chip files"
		if len(c.missing) == 1 {
			what = "this chip file"
		}
		parts = append(parts, fmt.Sprintf("%s is missing %s for %s: %s.", zip, what, g.Title, strings.Join(c.missing, ", ")))
	}
	if len(c.wrong) > 0 {
		what := "These files are"
		if len(c.wrong) == 1 {
			what = "This file is"
		}
		var l []string
		for _, w := range c.wrong {
			l = append(l, fmt.Sprintf("%s (%s bytes, should be %s)", w.name, commas(w.actual), commas(w.size)))
		}
		parts = append(parts, what+" the wrong size: "+strings.Join(l, ", ")+".")
	}
	if g.Parent != "" && len(c.missing) > 0 {
		parts = append(parts, fmt.Sprintf("%s is a version of the game in %s.zip, so some of its chips may be in that set: use a full (non-merged) ROM set, or add the missing files into %s.", g.Title, g.Parent, zip))
	} else if len(c.missing) > 0 {
		parts = append(parts, "Use a complete ROM set for this game.")
	} else {
		parts = append(parts, "The set may be a different version of the game, or damaged.")
	}
	if g.Loose {
		var exp []string
		for _, f := range g.Files {
			if !f.Optional {
				exp = append(exp, fmt.Sprintf("%s (%s bytes)", f.Name, commas(f.Size)))
			}
		}
		parts = append(parts, "RetroPixels expected: "+strings.Join(exp, ", ")+".")
		var have []string
		for _, k := range files.names {
			have = append(have, fmt.Sprintf("%s (%s bytes)", k, commas(files.size[k])))
		}
		h := strings.Join(have, ", ")
		if h == "" {
			h = "nothing"
		}
		parts = append(parts, "The zip holds: "+h+".")
	}
	return errors.New(strings.Join(parts, " "))
}

// ---------------------------------------------------------------- helpers

// RGB makes a pixel (RPArcade.rgb, in the native 0x00RRGGBB form).
func RGB(r, g, b int) uint32 { return uint32(r&255)<<16 | uint32(g&255)<<8 | uint32(b&255) }

// RGBf is RGB for colours worked out as fractions, as the JS boards pass
// them: each value is cut to an integer and to 8 bits (JS's & 255).
func RGBf(r, g, b float64) uint32 { return RGB(jsInt(r), jsInt(g), jsInt(b)) }

// jsInt is JavaScript's ToInt32 for the values boards make (in range).
func jsInt(v float64) int { return int(int32(int64(v))) }

// RotateInto copies a w*h picture into dst turned clockwise by rot degrees.
func RotateInto(src []uint32, w, h, rot int, dst []uint32) {
	switch rot {
	case 90:
		for y := 0; y < h; y++ {
			row, col := y*w, h-1-y
			for x := 0; x < w; x++ {
				dst[x*h+col] = src[row+x]
			}
		}
	case 270:
		for y := 0; y < h; y++ {
			row := y * w
			for x := 0; x < w; x++ {
				dst[(w-1-x)*h+y] = src[row+x]
			}
		}
	case 180:
		n := w * h
		for i := 0; i < n; i++ {
			dst[n-1-i] = src[i]
		}
	default:
		copy(dst, src[:w*h])
	}
}

// Resampler turns samples at the board's own rate into the player's rate,
// averaged down (or stretched up), with the steady (DC) part removed
// (RPArcade.Resampler).
type Resampler struct {
	ratio      float64
	buf        []float32
	acc        float64
	cnt        int
	pos, last  float64
	hp, hpPrev float64
}

// NewResampler makes a Resampler from inRate to outRate.
func NewResampler(inRate, outRate float64) *Resampler {
	return &Resampler{ratio: inRate / outRate, buf: make([]float32, 0, 4096)}
}

func (r *Resampler) push(v float64) {
	r.hp = 0.9985 * (r.hp + v - r.hpPrev)
	r.hpPrev = v
	r.buf = append(r.buf, float32(r.hp))
}

// Add is one sample at the board's rate.
func (r *Resampler) Add(v float64) {
	if r.ratio >= 1 {
		r.acc += v
		r.cnt++
		r.pos++
		if r.pos >= r.ratio {
			r.push(r.acc / float64(r.cnt))
			r.pos -= r.ratio
			r.acc, r.cnt = 0, 0
		}
		return
	}
	for r.pos < 1 {
		r.push(r.last + (v-r.last)*r.pos)
		r.pos += r.ratio
	}
	r.pos--
	r.last = v
}

// Take returns the samples made since the last call.
func (r *Resampler) Take() []float32 {
	out := append([]float32(nil), r.buf...)
	r.buf = r.buf[:0]
	return out
}

// Clear drops the samples waiting.
func (r *Resampler) Clear() { r.buf = r.buf[:0] }

// AppendState / LoadState keep the resampler's running values (not its
// waiting samples, which JS's saveState doesn't keep either).
func (r *Resampler) AppendState(w *StateW) {
	w.F64(r.acc)
	w.Int(r.cnt)
	w.F64(r.pos)
	w.F64(r.last)
	w.F64(r.hp)
	w.F64(r.hpPrev)
}

// LoadState reads what AppendState wrote.
func (r *Resampler) LoadState(s *StateR) {
	r.acc = s.F64()
	r.cnt = s.Int()
	r.pos = s.F64()
	r.last = s.F64()
	r.hp = s.F64()
	r.hpPrev = s.F64()
}

// ---------------------------------------------------------------- zip

// maxFile: files bigger than this aren't read (as the web page skips them).
const maxFile = 4 * 1024 * 1024

// ReadZip reads a .zip ROM set: the files in the zip's order (names as
// stored) with their sizes, and the bytes of every file up to 4 MB.
func ReadZip(data []byte) (names []string, sizes []int, contents [][]byte, err error) {
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil, nil, nil, errors.New("that .zip file looks damaged")
	}
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		size := int(f.UncompressedSize64)
		var b []byte
		if size <= maxFile {
			rc, err := f.Open()
			if err != nil {
				return nil, nil, nil, fmt.Errorf("couldn't unpack %s: %v", f.Name, err)
			}
			b, err = io.ReadAll(io.LimitReader(rc, maxFile+1))
			rc.Close()
			if err != nil {
				return nil, nil, nil, fmt.Errorf("couldn't unpack %s: %v", f.Name, err)
			}
			size = len(b)
		}
		names = append(names, f.Name)
		sizes = append(sizes, size)
		contents = append(contents, b)
	}
	return names, sizes, contents, nil
}

// ---------------------------------------------------------------- the core

// Core is a running arcade game (RPCores.arcade).
type Core struct {
	def     *BoardDef
	game    *Game
	board   Board
	bw, bh  int
	rot     int
	width   int
	height  int
	aspect  float64
	fps     float64
	buttons []string
	players int
	chars   []string
	inputs  []Pad
	dipList []Dip
	dips    map[string]int
	screen  []uint32
}

// New opens a ROM set: rom is the zip's bytes; opts.Name is the zip's name
// without its extension.
func New(rom []byte, opts core.Options) (core.Core, error) {
	names, sizes, contents, err := ReadZip(rom)
	if err != nil {
		return nil, err
	}
	zipName := ""
	if opts.Name != "" {
		zipName = opts.Name + ".zip"
	}
	return NewFromFiles(names, sizes, contents, zipName, opts)
}

// NewFromFiles opens a ROM set given as files (contents[i] nil for a file
// that wasn't read; it then counts as absent, as on the web page).
func NewFromFiles(names []string, sizes []int, contents [][]byte, zipName string, opts core.Options) (*Core, error) {
	files := newFileSet()
	for i, n := range names {
		if contents[i] != nil {
			files.add(n, len(contents[i]), contents[i])
		}
	}
	hit := identify(files, zipName)
	if hit == nil {
		return nil, fmt.Errorf("RetroPixels doesn't know this arcade game yet. The games it can play are: %s. Load the game's .zip ROM set (keep it zipped).", SupportedText())
	}
	def, game := hit.Board, hit.Game
	use := files
	if !check(game, files).ok && game.Loose {
		if lf := looseFiles(game, files); lf != nil && check(game, lf).ok {
			use = lf
		}
	}
	if !check(game, use).ok {
		return nil, setError(game, files, zipName)
	}
	c := &Core{def: def, game: game}

	// DIP switches: defaults, then the player's own choices
	c.dipList = game.Dips
	if c.dipList == nil {
		c.dipList = def.Dips
	}
	c.dips = map[string]int{}
	for _, d := range c.dipList {
		v := d.Default
		if want, ok := opts.Dips[d.ID]; ok && d.has(want) {
			v = want
		}
		c.dips[d.ID] = v
	}
	rate := opts.SampleRate
	if rate == 0 {
		rate = 48000
	}
	var sram []byte
	if len(opts.SRAM) > 0 {
		sram = opts.SRAM
	}
	b, err := def.New(game, use.data, Opts{SampleRate: rate, Dips: c.dips, SRAM: sram})
	if err != nil {
		return nil, err
	}
	c.board = b
	c.bw, c.bh = pick(game.Width, def.Width), pick(game.Height, def.Height)
	r := def.Rotate
	if game.Rotate != 0 {
		r = game.Rotate
	}
	c.rot = ((r % 360) + 360) % 360
	side := c.rot == 90 || c.rot == 270
	c.width, c.height = c.bw, c.bh
	if side {
		c.width, c.height = c.bh, c.bw
	}
	a := game.Aspect
	if a == 0 {
		a = def.Aspect
	}
	if a == 0 {
		a = 4.0 / 3
	}
	c.aspect = a
	if side {
		c.aspect = 1 / a
	}
	c.fps = game.FPS
	if c.fps == 0 {
		c.fps = def.FPS
	}
	if c.fps == 0 {
		c.fps = 60
	}
	c.buttons = game.Buttons
	if c.buttons == nil {
		c.buttons = def.Buttons
	}
	if c.buttons == nil {
		c.buttons = DefaultButtons
	}
	c.buttons = append([]string(nil), c.buttons...)
	p := pick(game.Players, def.Players)
	if p == 0 {
		p = 2
	}
	c.players = max(1, min(4, p))
	if c.players >= 3 {
		c.chars = game.Characters
		if c.chars == nil {
			c.chars = def.Characters
		}
	}
	for i := 0; i < max(2, c.players); i++ {
		pad := Pad{}
		for _, n := range c.buttons {
			pad[n] = false
		}
		c.inputs = append(c.inputs, pad)
	}
	if c.rot != 0 {
		c.screen = make([]uint32, c.width*c.height)
	}
	c.updateScreen()
	return c, nil
}

func pick(a, b int) int {
	if a != 0 {
		return a
	}
	return b
}

func (d *Dip) has(v int) bool {
	for _, o := range d.Options {
		if o.Value == v {
			return true
		}
	}
	return false
}

func (c *Core) updateScreen() {
	if c.rot != 0 {
		RotateInto(c.board.Screen(), c.bw, c.bh, c.rot, c.screen)
	}
}

// Info: the game's buttons and players, and its picture's shape.
func (c *Core) Info() core.Info {
	return core.Info{System: "arcade", Buttons: c.buttons, Players: max(2, c.players), Aspect: c.aspect}
}

func (c *Core) FPS() float64 { return c.fps }
func (c *Core) Width() int   { return c.width }
func (c *Core) Height() int  { return c.height }

// Screen is the picture, turned upright.
func (c *Core) Screen() []uint32 {
	if c.rot == 0 {
		return c.board.Screen()
	}
	return c.screen
}

func (c *Core) SetInput(p int, bits uint32) {
	if p < 0 || p >= len(c.inputs) {
		return
	}
	st := c.inputs[p]
	for i, b := range c.buttons {
		st[b] = bits&(1<<i) != 0
	}
	c.board.SetInput(c.inputs)
}

func (c *Core) RunFrame() {
	c.board.RunFrame()
	c.updateScreen()
}

func (c *Core) TakeAudio() []float32 { return c.board.TakeAudio() }
func (c *Core) Reset()               { c.board.Reset() }

func (c *Core) SRAM() []byte {
	if s, ok := c.board.(SRAMer); ok {
		return s.SRAM()
	}
	return nil
}

const stateMagic = "RPARC\x01"

// SaveState: the game's id, then the board's state.
func (c *Core) SaveState() ([]byte, error) {
	b, err := c.board.SaveState()
	if err != nil {
		return nil, err
	}
	w := &StateW{}
	w.Bytes([]byte(stateMagic))
	w.Str(c.game.ID)
	w.Bytes(b)
	return w.B, nil
}

func (c *Core) LoadState(b []byte) error {
	if !bytes.HasPrefix(b, []byte(stateMagic)) {
		return errors.New("that isn't an arcade save state")
	}
	r := &StateR{B: b[len(stateMagic):]}
	if id := r.Str(); r.Err != nil || id != c.game.ID {
		return errors.New("that save state is for a different arcade game")
	}
	if err := c.board.LoadState(r.B); err != nil {
		return err
	}
	c.updateScreen()
	return nil
}

// GameID is the set's name ("pacman"); GameTitle the game's ("Pac-Man").
func (c *Core) GameID() string    { return c.game.ID }
func (c *Core) GameTitle() string { return c.game.Title }

// Players is how many controllers the game reads (1-4).
func (c *Core) Players() int { return c.players }

// Characters are who players 1-4 play as on 3- and 4-player cabinets
// (for the player picker), or nil.
func (c *Core) Characters() []string { return c.chars }

// ControlsNote is the Controls window's note.
func (c *Core) ControlsNote() string { return ControlsNote }

// ControlsNote is the Controls window's note for arcade games (controlsNote in arcade.js).
const ControlsNote = "Put a coin in with Coin (View button, or 5 on the keyboard), then press Start (Menu button, or 1). " +
	"The left stick moves too. Clicking the sticks (L3/R3) and the right stick are used for save states."

// DipSetting is a machine setting with its current value.
type DipSetting struct {
	Dip
	Value int
}

// Dips lists the machine's settings (DIP switches) with their values.
func (c *Core) Dips() []DipSetting {
	out := make([]DipSetting, len(c.dipList))
	for i, d := range c.dipList {
		out[i] = DipSetting{d, c.dips[d.ID]}
	}
	return out
}

// SetDip changes a setting; false if there's no such setting or value.
// Most games read their switches when they start: reset after changing one.
func (c *Core) SetDip(id string, v int) bool {
	for i := range c.dipList {
		if d := &c.dipList[i]; d.ID == id && d.has(v) {
			c.dips[id] = v
			return true
		}
	}
	return false
}

// Board is the running board (for tests).
func (c *Core) Board() Board { return c.board }
