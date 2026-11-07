// Package boxart finds the cover art for the games in RetroPixels' libraries
// (GameCube, Wii, Wii U, 3DS / DS and arcade), so a game's tile shows its box
// instead of its initials.
//
// Where it looks, in order:
//
//  1. a picture next to the game: "<game file name>.png" (or .jpg, .jpeg,
//     .webp, .gif), "cover", "boxart" or "folder" pictures in an unpacked
//     game's folder, and the same names in boxart\, covers\, images\,
//     media\boxart\ ... folders beside the game;
//  2. GameTDB (art.gametdb.com), by the game's ID read from the file (the
//     disc ID, the DS / 3DS game code, the Wii U product code), or by its
//     name in GameTDB's list of titles;
//  3. libretro-thumbnails (the box art RetroArch shows), by the game's name,
//     first as it is and then matched against the system's list of covers;
//  4. arcade games: the Arcade Database (adb.arcadeitalia.net) by MAME name;
//  5. an unpacked Wii U game's own icon (meta\iconTex.tga).
//
// Downloaded art is shrunk and kept in the user's cache folder
// (%LOCALAPPDATA%\RetroPixels\boxart), so each game is fetched once. A game
// with no art anywhere is remembered for a few days, then looked for again.
//
// RetroPixels.exe (the /api/art route) and RetroPixels Native both use it.
package boxart

import (
	"bytes"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/draw"
	"image/jpeg"
	_ "image/png" // decoding PNG covers
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	_ "image/gif" // decoding local .gif covers
)

// Systems.
const (
	GameCube = "gamecube"
	Wii      = "wii"
	WiiU     = "wiiu"
	DS       = "ds"
	N3DS     = "3ds"
	Arcade   = "arcade"
)

// Game is what's known about one game.
type Game struct {
	System string // one of the systems above
	Path   string // the game file (or, for an unpacked Wii U game, its code\*.rpx)
	Name   string // the name shown in the library
	ID     string // the disc ID / game code, when the library knows it (else it's read from the file)
	Short  string // arcade: MAME's name for the set (dkong)
}

// MaxSide: downloaded covers are shrunk to fit this many pixels (tall).
const MaxSide = 360

var (
	// Dir is where art is kept; empty means the user's cache folder.
	Dir string
	// UserAgent is sent with each download.
	UserAgent = "RetroPixels"
	// Offline turns off downloading (tests).
	Offline bool

	client = &http.Client{Timeout: 20 * time.Second}

	// gametdbBase, libretroBases and adbBase are where art comes from (tests point them elsewhere).
	gametdbBase   = "https://art.gametdb.com"
	gametdbTitles = "https://www.gametdb.com"
	libretroBases = []string{"https://thumbnails.libretro.com", "https://raw.githubusercontent.com/libretro-thumbnails"}
	githubAPI     = "https://api.github.com/repos/libretro-thumbnails"
	adbBase       = "https://adb.arcadeitalia.net/media/mame.current"

	inflight = map[string]*call{}
	flightMu sync.Mutex
	slots    = make(chan struct{}, 4) // downloads at once
)

type call struct {
	done chan struct{}
	file string
	err  error
}

// ErrNone: no art was found for the game.
var ErrNone = errors.New("no art found")

func cacheDir() string {
	if Dir != "" {
		return Dir
	}
	base, err := os.UserCacheDir()
	if err != nil {
		base = os.TempDir()
	}
	return filepath.Join(base, "RetroPixels", "boxart")
}

func (g Game) key() string {
	h := sha1.Sum([]byte(strings.ToLower(g.System + "|" + g.Path + "|" + g.Name + "|" + g.ID + "|" + g.Short)))
	return hex.EncodeToString(h[:10])
}

// Find returns a picture file with the game's art: one next to the game, or
// one downloaded into the cache. It waits for the download; calls for the
// same game share it. ErrNone means there is none.
func Find(g Game) (string, error) {
	if p := Local(g); p != "" {
		return p, nil
	}
	k := g.key()
	flightMu.Lock()
	if c, ok := inflight[k]; ok {
		flightMu.Unlock()
		<-c.done
		return c.file, c.err
	}
	c := &call{done: make(chan struct{})}
	inflight[k] = c
	flightMu.Unlock()
	c.file, c.err = find(g, k)
	close(c.done)
	flightMu.Lock()
	delete(inflight, k)
	flightMu.Unlock()
	return c.file, c.err
}

// Cached returns the game's art if it's already at hand (next to the game or
// in the cache), without downloading anything.
func Cached(g Game) string {
	if p := Local(g); p != "" {
		return p
	}
	dir := filepath.Join(cacheDir(), g.System)
	for _, ext := range []string{".jpg", ".png", ".webp", ".gif"} {
		if p := filepath.Join(dir, g.key()+ext); fileExists(p) {
			return p
		}
	}
	return ""
}

// noneFor: how long "no art anywhere" is remembered.
const noneFor = 3 * 24 * time.Hour

func find(g Game, k string) (string, error) {
	dir := filepath.Join(cacheDir(), g.System)
	if p := Cached(g); p != "" {
		return p, nil
	}
	none := filepath.Join(dir, k+".none")
	if st, err := os.Stat(none); err == nil && time.Since(st.ModTime()) < noneFor {
		return "", ErrNone
	}
	if Offline {
		return "", ErrNone
	}
	if g.ID == "" {
		g.ID = ReadID(g.System, g.Path)
	}
	img, netErr := fetch(g)
	if img == nil {
		// The game's own icon, when it has one.
		if b := ownIcon(g); b != nil {
			img = b
		}
	}
	if img == nil {
		if netErr == nil { // every source answered: there's no art (don't remember it when offline)
			_ = os.MkdirAll(dir, 0o755)
			_ = os.WriteFile(none, nil, 0o644)
		}
		return "", ErrNone
	}
	b, ext := shrink(img)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	p := filepath.Join(dir, k+ext)
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return "", err
	}
	if err := os.Rename(tmp, p); err != nil {
		return "", err
	}
	_ = os.Remove(none)
	return p, nil
}

// Forget drops what's remembered about games with no art, so they're looked
// for again (the library's Rescan).
func Forget() {
	_ = filepath.Walk(cacheDir(), func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() && strings.HasSuffix(p, ".none") {
			_ = os.Remove(p)
		}
		return nil
	})
}

// ---------------- pictures next to the game ----------------

var imageExts = []string{".png", ".jpg", ".jpeg", ".webp", ".gif"}

// IsImage: a file name with a picture's extension.
func IsImage(name string) bool {
	e := strings.ToLower(filepath.Ext(name))
	for _, x := range imageExts {
		if e == x {
			return true
		}
	}
	return false
}

var artFolders = []string{"boxart", "boxarts", "box art", "covers", "cover", "images", "art", "artwork", "thumbnails",
	filepath.Join("media", "boxart"), filepath.Join("media", "covers"), filepath.Join("media", "box2dfront"),
	filepath.Join("media", "images"), filepath.Join("Named_Boxarts")}

// Local finds a picture of the game next to it.
func Local(g Game) string {
	if g.Path == "" {
		return ""
	}
	dir := filepath.Dir(g.Path)
	stem := strings.TrimSuffix(filepath.Base(g.Path), filepath.Ext(g.Path))
	names := []string{stem}
	if g.Short != "" && !strings.EqualFold(g.Short, stem) {
		names = append(names, g.Short)
	}
	if g.System == WiiU && strings.EqualFold(filepath.Base(dir), "code") {
		// An unpacked game: its title folder holds it (and names it).
		title := filepath.Dir(dir)
		for _, n := range []string{"cover", "boxart", "folder", "front"} {
			if p := pictureIn(title, n); p != "" {
				return p
			}
		}
		dir = filepath.Dir(title)
		names = []string{filepath.Base(title)}
	}
	if g.Name != "" {
		names = append(names, g.Name)
	}
	for _, n := range names {
		if p := pictureIn(dir, n); p != "" {
			return p
		}
	}
	for _, f := range artFolders {
		for _, n := range names {
			if p := pictureIn(filepath.Join(dir, f), n); p != "" {
				return p
			}
		}
	}
	return ""
}

func pictureIn(dir, stem string) string {
	if stem == "" {
		return ""
	}
	for _, e := range imageExts {
		p := filepath.Join(dir, stem+e)
		if fileExists(p) {
			return p
		}
		if p = filepath.Join(dir, stem+strings.ToUpper(e)); fileExists(p) {
			return p
		}
	}
	return ""
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir() && st.Size() > 0
}

// ---------------- downloading ----------------

// fetch tries each source in turn. The error is set when a source couldn't
// be reached (so "no art" isn't remembered while offline).
func fetch(g Game) ([]byte, error) {
	var netErr error
	try := func(u string) []byte {
		b, err := get(u)
		if err != nil && !errors.Is(err, errNotFound) {
			netErr = err
		}
		if b != nil && sniff(b) != "" {
			return b
		}
		return nil
	}

	// GameTDB, by ID.
	for _, id := range gametdbIDs(g, &netErr) {
		for _, u := range gametdbURLs(g.System, id) {
			if b := try(u); b != nil {
				return b, nil
			}
		}
	}

	// libretro-thumbnails, by name.
	if sys := libretroSystem(g.System); sys != "" {
		names := nameCandidates(g)
		for _, kind := range libretroKinds(g.System) {
			for _, n := range names {
				if b := tryLibretro(try, sys, kind, n); b != nil {
					return b, nil
				}
			}
		}
		if g.System != Arcade { // the arcade list is too long to fetch; MAME's names are exact anyway
			for _, kind := range libretroKinds(g.System) {
				list, err := libretroList(sys, kind)
				if err != nil {
					netErr = err
				}
				if n := bestMatch(list, g); n != "" {
					if b := tryLibretro(try, sys, kind, n); b != nil {
						return b, nil
					}
				}
				if list == nil {
					// No list to match against: the name with the usual region tags.
					for _, n := range regionGuesses(g) {
						if b := tryLibretro(try, sys, kind, n); b != nil {
							return b, nil
						}
					}
				}
			}
		}
	}

	// Arcade Database, by MAME name.
	if g.System == Arcade && g.Short != "" {
		for _, kind := range []string{"flyers", "titles", "ingames"} {
			if b := try(adbBase + "/" + kind + "/" + url.PathEscape(g.Short) + ".png"); b != nil {
				return b, nil
			}
		}
	}
	return nil, netErr
}

func tryLibretro(try func(string) []byte, sys, kind, name string) []byte {
	file := url.PathEscape(libretroFileName(name) + ".png")
	for i, base := range libretroBases {
		var u string
		if i == 0 {
			u = base + "/" + url.PathEscape(sys) + "/" + kind + "/" + file
		} else {
			u = base + "/" + strings.ReplaceAll(sys, " ", "_") + "/master/" + kind + "/" + file
		}
		if b := try(u); b != nil {
			return b
		}
	}
	return nil
}

var errNotFound = errors.New("not found")

func get(u string) ([]byte, error) {
	slots <- struct{}{}
	defer func() { <-slots }()
	req, err := http.NewRequest("GET", u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", UserAgent)
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == 404 || resp.StatusCode == 410 || resp.StatusCode == 403 {
		return nil, errNotFound
	}
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("%s: %s", u, resp.Status)
	}
	return io.ReadAll(io.LimitReader(resp.Body, 24<<20))
}

// sniff names a picture's format from its first bytes ("" if it isn't one).
func sniff(b []byte) string {
	switch {
	case bytes.HasPrefix(b, []byte("\x89PNG")):
		return ".png"
	case bytes.HasPrefix(b, []byte{0xFF, 0xD8, 0xFF}):
		return ".jpg"
	case len(b) > 12 && string(b[:4]) == "RIFF" && string(b[8:12]) == "WEBP":
		return ".webp"
	case bytes.HasPrefix(b, []byte("GIF8")):
		return ".gif"
	}
	return ""
}

// shrink makes a cover small enough for the library (MaxSide tall, a JPEG);
// pictures Go can't read are kept as they are.
func shrink(b []byte) ([]byte, string) {
	src, _, err := image.Decode(bytes.NewReader(b))
	if err != nil {
		return b, sniff(b)
	}
	r := src.Bounds()
	w, h := r.Dx(), r.Dy()
	if w <= 0 || h <= 0 {
		return b, sniff(b)
	}
	scale := float64(MaxSide) / float64(max(w, h))
	if scale > 1 {
		scale = 1
	}
	nw, nh := max(1, int(float64(w)*scale+0.5)), max(1, int(float64(h)*scale+0.5))
	dst := image.NewRGBA(image.Rect(0, 0, nw, nh))
	// The library's tile colour behind see-through covers.
	draw.Draw(dst, dst.Bounds(), image.NewUniform(bg), image.Point{}, draw.Src)
	resize(dst, src)
	var out bytes.Buffer
	if err := jpeg.Encode(&out, dst, &jpeg.Options{Quality: 88}); err != nil {
		return b, sniff(b)
	}
	return out.Bytes(), ".jpg"
}

// ---------------- GameTDB ----------------

// gametdbSystem: GameTDB's name for the system (GameCube covers are with Wii's).
func gametdbSystem(sys string) string {
	switch sys {
	case GameCube, Wii:
		return "wii"
	case WiiU:
		return "wiiu"
	case DS:
		return "ds"
	case N3DS:
		return "3ds"
	}
	return ""
}

// gametdbIDs: the game's ID, completed from GameTDB's list of titles when
// only part of it is known (Wii U product codes have 4 of its 6 letters),
// or looked up there by name when the file gives none.
func gametdbIDs(g Game, netErr *error) []string {
	ts := gametdbSystem(g.System)
	if ts == "" {
		return nil
	}
	id := strings.ToUpper(strings.TrimSpace(g.ID))
	if id != "" && (ts != "wiiu" || len(id) == 6) {
		return []string{id}
	}
	titles, err := gametdbTitleList(ts)
	if err != nil {
		*netErr = err
	}
	var out []string
	if id != "" {
		for _, t := range titles {
			if strings.HasPrefix(t.id, id) {
				out = append(out, t.id)
			}
		}
		if len(out) > 0 {
			return out[:min(len(out), 3)]
		}
	}
	base := baseTitle(g.Name)
	if base == "" {
		return nil
	}
	want := regionWanted(g)
	var match []title
	for _, t := range titles {
		if t.base == base {
			match = append(match, t)
		}
	}
	sort.SliceStable(match, func(i, j int) bool { return idRegionRank(match[i].id, want) < idRegionRank(match[j].id, want) })
	for _, t := range match {
		// GameCube and Wii share GameTDB's list: GameCube IDs start with G, D or P (and a few others).
		if g.System == GameCube && !strings.ContainsRune("GDPU", rune(t.id[0])) {
			continue
		}
		if g.System == Wii && strings.ContainsRune("GD", rune(t.id[0])) {
			continue
		}
		out = append(out, t.id)
		if len(out) == 2 {
			break
		}
	}
	return out
}

// gametdbURLs: the covers to try for an ID, in the game's own region first.
func gametdbURLs(sys, id string) []string {
	ts := gametdbSystem(sys)
	var langs []string
	if len(id) >= 4 {
		switch id[3] {
		case 'E', 'N':
			langs = []string{"US"}
		case 'J':
			langs = []string{"JA"}
		case 'K', 'Q', 'T':
			langs = []string{"KO"}
		case 'W':
			langs = []string{"ZH", "ZHTW"}
		case 'D':
			langs = []string{"DE", "EN"}
		case 'F':
			langs = []string{"FR", "EN"}
		case 'S':
			langs = []string{"ES", "EN"}
		case 'I':
			langs = []string{"IT", "EN"}
		case 'H':
			langs = []string{"NL", "EN"}
		case 'U':
			langs = []string{"AU", "EN"}
		case 'P', 'X', 'Y', 'Z', 'V', 'L', 'M':
			langs = []string{"EN"}
		}
	}
	for _, l := range []string{"US", "EN", "JA"} {
		if !contains(langs, l) {
			langs = append(langs, l)
		}
	}
	kinds := []string{"cover"}
	exts := []string{".png", ".jpg"}
	switch ts {
	case "wiiu":
		exts = []string{".jpg", ".png"}
	case "ds", "3ds":
		kinds = []string{"cover", "coverHQ", "box"}
		exts = []string{".jpg", ".png"}
	}
	var out []string
	for _, l := range langs[:min(len(langs), 3)] {
		for _, k := range kinds {
			for _, e := range exts {
				out = append(out, fmt.Sprintf("%s/%s/%s/%s/%s%s", gametdbBase, ts, k, l, url.PathEscape(id), e))
			}
		}
	}
	return out
}

type title struct{ id, base string }

var (
	titleMu    sync.Mutex
	titleLists = map[string][]title{}
)

// gametdbTitleList: GameTDB's list of a system's games ("RMCE01 = Mario Kart
// Wii"), kept for a month.
func gametdbTitleList(ts string) ([]title, error) {
	titleMu.Lock()
	defer titleMu.Unlock()
	if l, ok := titleLists[ts]; ok {
		return l, nil
	}
	b, err := cachedText(filepath.Join(cacheDir(), "lists", "gametdb-"+ts+".txt"), func() ([]byte, error) {
		return get(gametdbTitles + "/" + ts + "tdb.txt?LANG=EN")
	})
	if err != nil && b == nil {
		return nil, err
	}
	var l []title
	for _, line := range strings.Split(string(b), "\n") {
		id, name, ok := strings.Cut(line, " = ")
		id = strings.TrimSpace(id)
		if !ok || id == "TITLES" || len(id) < 4 {
			continue
		}
		l = append(l, title{id: id, base: baseTitle(strings.TrimSpace(name))})
	}
	titleLists[ts] = l
	return l, nil
}

// cachedText keeps a downloaded list for a month (an older copy is used when
// a new one can't be had).
func cachedText(p string, download func() ([]byte, error)) ([]byte, error) {
	st, err := os.Stat(p)
	if err == nil && time.Since(st.ModTime()) < 30*24*time.Hour {
		return os.ReadFile(p)
	}
	if Offline {
		b, _ := os.ReadFile(p)
		return b, nil
	}
	b, derr := download()
	if derr != nil || len(b) == 0 {
		old, _ := os.ReadFile(p)
		if old != nil {
			return old, nil
		}
		if derr == nil || errors.Is(derr, errNotFound) {
			return nil, nil
		}
		return nil, derr
	}
	_ = os.MkdirAll(filepath.Dir(p), 0o755)
	_ = os.WriteFile(p, b, 0o644)
	return b, nil
}

// ---------------- libretro-thumbnails ----------------

func libretroSystem(sys string) string {
	switch sys {
	case GameCube:
		return "Nintendo - GameCube"
	case Wii:
		return "Nintendo - Wii"
	case WiiU:
		return "Nintendo - Wii U"
	case DS:
		return "Nintendo - Nintendo DS"
	case N3DS:
		return "Nintendo - Nintendo 3DS"
	case Arcade:
		return "MAME"
	}
	return ""
}

func libretroKinds(sys string) []string {
	if sys == Arcade {
		return []string{"Named_Boxarts", "Named_Titles", "Named_Snaps"}
	}
	return []string{"Named_Boxarts"}
}

// libretroFileName: libretro-thumbnails' file names have these characters as "_".
func libretroFileName(n string) string {
	return strings.Map(func(r rune) rune {
		if strings.ContainsRune(`&*/:`+"`"+`<>?\|"`, r) {
			return '_'
		}
		return r
	}, n)
}

var (
	reLeadNumber = regexp.MustCompile(`^\s*\d{1,4}\s*[-–.]\s+`)
	reTags       = regexp.MustCompile(`\s*[\(\[][^\)\]]*[\)\]]`)
)

// nameCandidates: the names to try as they are: the file's name (often a
// No-Intro or Redump name, "New Super Mario Bros. (USA)") and the library's.
func nameCandidates(g Game) []string {
	var out []string
	add := func(n string) {
		n = strings.TrimSpace(n)
		if n != "" && !contains(out, n) {
			out = append(out, n)
		}
	}
	if g.Path != "" && g.System != Arcade {
		stem := strings.TrimSuffix(filepath.Base(g.Path), filepath.Ext(g.Path))
		if g.System == WiiU && strings.EqualFold(filepath.Base(filepath.Dir(g.Path)), "code") {
			stem = filepath.Base(filepath.Dir(filepath.Dir(g.Path)))
		}
		add(stem)
		add(reLeadNumber.ReplaceAllString(stem, ""))
	}
	add(g.Name)
	return out
}

// regionGuesses: the game's name with the region tags most covers have
// ("Super Smash Bros. Melee (USA)"), for when the cover list can't be had.
func regionGuesses(g Game) []string {
	var out []string
	for _, n := range nameCandidates(g) {
		if reTags.MatchString(n) {
			continue
		}
		for _, r := range []string{" (USA)", " (USA, Europe)", " (World)", " (Europe)"} {
			if !contains(out, n+r) {
				out = append(out, n+r)
			}
		}
	}
	return out
}

var (
	listMu    sync.Mutex
	listCache = map[string][]string{}
)

// libretroList: the names of the covers libretro-thumbnails has for a
// system, kept for a month.
func libretroList(sys, kind string) ([]string, error) {
	listMu.Lock()
	defer listMu.Unlock()
	k := sys + "/" + kind
	if l, ok := listCache[k]; ok {
		return l, nil
	}
	p := filepath.Join(cacheDir(), "lists", "libretro-"+strings.ReplaceAll(sys, " ", "_")+"-"+kind+".txt")
	b, err := cachedText(p, func() ([]byte, error) {
		names, err := downloadLibretroList(sys, kind)
		if err != nil {
			return nil, err
		}
		return []byte(strings.Join(names, "\n")), nil
	})
	if err != nil && b == nil {
		return nil, err
	}
	var l []string
	for _, n := range strings.Split(string(b), "\n") {
		if n = strings.TrimSpace(n); n != "" {
			l = append(l, n)
		}
	}
	listCache[k] = l
	return l, nil
}

var reHref = regexp.MustCompile(`href="([^"?/]+)\.png"`)

func downloadLibretroList(sys, kind string) ([]string, error) {
	// The thumbnail server's folder listing.
	b, err := get(libretroBases[0] + "/" + url.PathEscape(sys) + "/" + kind + "/")
	if err == nil {
		var names []string
		for _, m := range reHref.FindAllStringSubmatch(string(b), -1) {
			if n, err := url.PathUnescape(m[1]); err == nil {
				names = append(names, n)
			}
		}
		if len(names) > 0 {
			return names, nil
		}
	}
	// GitHub's list of the folder (two calls: the repository's top, then the folder).
	repo := githubAPI + "/" + strings.ReplaceAll(sys, " ", "_") + "/git/trees/"
	var top struct {
		Tree []struct{ Path, Type, Sha string }
	}
	if err := getJSON(repo+"master", &top); err != nil {
		return nil, err
	}
	for _, t := range top.Tree {
		if t.Path != kind || t.Type != "tree" {
			continue
		}
		var sub struct {
			Tree []struct{ Path, Type string }
		}
		if err := getJSON(repo+t.Sha, &sub); err != nil {
			return nil, err
		}
		var names []string
		for _, f := range sub.Tree {
			if strings.HasSuffix(f.Path, ".png") {
				names = append(names, strings.TrimSuffix(f.Path, ".png"))
			}
		}
		return names, nil
	}
	return nil, nil
}

// bestMatch picks the cover whose title is the game's, in the game's region
// if there's a choice (else USA, World, Europe, Japan, then any).
func bestMatch(list []string, g Game) string {
	bases := map[string]bool{}
	for _, n := range nameCandidates(g) {
		if b := baseTitle(n); b != "" {
			bases[b] = true
		}
	}
	if len(bases) == 0 {
		return ""
	}
	want := regionWanted(g)
	best, bestRank := "", 1<<30
	for _, n := range list {
		if !bases[baseTitle(n)] {
			continue
		}
		r := nameRegionRank(n, want)
		if r < bestRank || (r == bestRank && len(n) < len(best)) {
			best, bestRank = n, r
		}
	}
	return best
}

var reNonWord = regexp.MustCompile(`[^\p{L}\p{N}]+`)

// baseTitle: a name without its tags, punctuation, a leading number or
// "The", for matching: "Legend of Zelda, The - Spirit Tracks (USA) (En,Fr,Es)"
// -> "legend of zelda spirit tracks".
func baseTitle(n string) string {
	n = reLeadNumber.ReplaceAllString(n, "")
	n = reTags.ReplaceAllString(n, "")
	n = strings.ToLower(n)
	n = strings.ReplaceAll(n, "&", " and ")
	n = strings.ReplaceAll(n, "'", "")
	n = strings.ReplaceAll(n, "’", "")
	n = strings.ReplaceAll(n, ", the", " ")
	n = strings.TrimSpace(reNonWord.ReplaceAllString(n, " "))
	n = strings.TrimPrefix(n, "the ")
	for _, s := range []string{" the", " a"} {
		n = strings.TrimSuffix(n, s)
	}
	return strings.Join(strings.Fields(n), " ")
}

// regionWanted: the region named in the game's file name ("(Europe)"), if any.
func regionWanted(g Game) string {
	s := strings.ToLower(filepath.Base(g.Path) + " " + g.Name)
	for _, r := range []struct{ tag, region string }{
		{"(usa", "usa"}, {"(us)", "usa"}, {"(u)", "usa"}, {"(ntsc-u", "usa"}, {"(world", "world"},
		{"(europe", "europe"}, {"(eur", "europe"}, {"(e)", "europe"}, {"(pal", "europe"}, {"(uk", "europe"},
		{"(germany", "europe"}, {"(france", "europe"}, {"(spain", "europe"}, {"(italy", "europe"},
		{"(japan", "japan"}, {"(jpn", "japan"}, {"(j)", "japan"}, {"(ntsc-j", "japan"}, {"(korea", "korea"},
	} {
		if strings.Contains(s, r.tag) {
			return r.region
		}
	}
	return ""
}

func nameRegionRank(n, want string) int {
	l := strings.ToLower(n)
	order := []string{"usa", "world", "europe", "japan"}
	if want != "" {
		order = append([]string{want}, order...)
	}
	rank := len(order)
	for i, r := range order {
		if strings.Contains(l, "("+r) || strings.Contains(l, ", "+r) {
			rank = i
			break
		}
	}
	// Plain releases before betas, demos and the like.
	for _, t := range []string{"(beta", "(proto", "(demo", "(sample", "(kiosk", "(rev", "(virtual console"} {
		if strings.Contains(l, t) {
			rank += 10
		}
	}
	return rank
}

func idRegionRank(id, want string) int {
	if len(id) < 4 {
		return 9
	}
	c := id[3]
	order := map[string]string{"usa": "E", "europe": "PDFSIXYH", "japan": "J", "korea": "K", "world": "EP"}
	if w, ok := order[want]; ok && strings.IndexByte(w, c) >= 0 {
		return 0
	}
	switch c {
	case 'E':
		return 1
	case 'P':
		return 2
	case 'J':
		return 3
	}
	return 4
}

// ---------------- small helpers ----------------

func contains(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}
