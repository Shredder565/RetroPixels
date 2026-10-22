// Package dolphin plays GameCube and Wii games through Dolphin for
// RetroPixels Native. It follows the RetroPixels app (dolphin.go,
// dolphin_install.go, dolvolume*.go, embed_windows.go and the Dolphin parts
// of recorder.go at the repository root): the same disc detection, the same
// download from dolphin-emu.org into the same folder, the same controller
// set-up and settings written to Dolphin's own files, the same command line,
// and the same recording made from Dolphin's frame and audio dumps.
//
// The app at the root keeps its own copy: its code is tied to its web page's
// routes and its own settings file, and this package is free of both.
package dolphin

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"unicode"
)

// Disc is what a game file says about itself.
type Disc struct {
	Platform string // "GameCube", "Wii" or "WiiWare"
	ID       string // e.g. GMSE01
	Title    string // the name stored on the disc
	DLC      bool   // a WiiWare add-on (.wad) that can't be started by itself
}

// Exts: the file types Dolphin plays, as the app lists them.
var Exts = map[string]bool{".iso": true, ".gcm": true, ".ciso": true, ".wbfs": true, ".rvz": true, ".wia": true, ".gcz": true, ".wad": true}

// IsGameFile tells whether a file's extension is a GameCube / Wii one.
func IsGameFile(path string) bool { return Exts[strings.ToLower(filepath.Ext(path))] }

var (
	gcMagic  = []byte{0xC2, 0x33, 0x9F, 0x3D} // at 0x1C
	wiiMagic = []byte{0x5D, 0x1C, 0x9E, 0xA3} // at 0x18
)

// ParseHeader reads a disc header (the first 0x60 bytes of a disc).
func ParseHeader(h []byte) (Disc, bool) {
	if len(h) < 0x60 {
		return Disc{}, false
	}
	var p string
	switch {
	case bytes.Equal(h[0x18:0x1C], wiiMagic):
		p = "Wii"
	case bytes.Equal(h[0x1C:0x20], gcMagic):
		p = "GameCube"
	default:
		return Disc{}, false
	}
	id := strings.TrimRight(string(h[0:6]), "\x00 ")
	end := 0x20 + 0x40
	if len(h) < end {
		end = len(h)
	}
	name := h[0x20:end]
	if i := bytes.IndexByte(name, 0); i >= 0 {
		name = name[:i]
	}
	return Disc{Platform: p, ID: id, Title: strings.TrimSpace(latin1(name))}, true
}

func latin1(b []byte) string {
	r := make([]rune, len(b))
	for i, c := range b {
		r[i] = rune(c)
	}
	return string(r)
}

// Identify reads just enough of a file to tell whether it's a GameCube or
// Wii game, and which.
func Identify(path string) (Disc, bool) {
	ext := strings.ToLower(filepath.Ext(path))
	f, err := os.Open(path)
	if err != nil {
		return Disc{}, false
	}
	defer f.Close()
	read := func(off int64, n int) []byte {
		b := make([]byte, n)
		k, _ := f.ReadAt(b, off)
		return b[:k]
	}
	switch ext {
	case ".iso", ".gcm":
		return ParseHeader(read(0, 0x60))
	case ".ciso":
		if string(read(0, 4)) == "CISO" {
			return ParseHeader(read(0x8000, 0x60))
		}
	case ".wbfs":
		if string(read(0, 4)) == "WBFS" {
			sec := read(8, 1)
			off := int64(0x200)
			if len(sec) == 1 && sec[0] >= 9 && sec[0] <= 16 {
				off = 1 << sec[0]
			}
			return ParseHeader(read(off, 0x60))
		}
	case ".rvz", ".wia":
		// WIA / RVZ keep a copy of the disc header at 0x58 of their second header.
		h := read(0, 0x48+0x80)
		if len(h) >= 0x48 && (string(h[0:3]) == "WIA" || string(h[0:3]) == "RVZ") {
			return ParseHeader(read(0x48+0x10, 0x80))
		}
	case ".gcz":
		if len(read(0, 4)) == 4 {
			return gczHeader(f)
		}
	case ".wad":
		return wadInfo(f, path)
	}
	return Disc{}, false
}

// .gcz is zlib-compressed in blocks; the first block holds the disc header.
func gczHeader(f *os.File) (Disc, bool) {
	b := make([]byte, 32)
	if _, err := f.ReadAt(b, 0); err != nil || binary.LittleEndian.Uint32(b) != 0xB10BC001 {
		return Disc{}, false
	}
	numBlocks := binary.LittleEndian.Uint32(b[28:])
	dataStart := int64(32) + int64(numBlocks)*12
	first := make([]byte, 8)
	f.ReadAt(first, 32)
	off := binary.LittleEndian.Uint64(first)
	compressed := off&(1<<63) == 0
	blk := make([]byte, 64<<10)
	n, _ := f.ReadAt(blk, dataStart+int64(off&^(1<<63)))
	blk = blk[:n]
	if compressed {
		blk = zlibInflate(blk, 0x60)
	}
	return ParseHeader(blk)
}

// zlibInflate decompresses up to n bytes of a zlib stream.
func zlibInflate(b []byte, n int) []byte {
	r, err := zlib.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil
	}
	defer r.Close()
	out := make([]byte, n)
	k, _ := io.ReadFull(r, out)
	return out[:k]
}

func wadInfo(f *os.File, path string) (Disc, bool) {
	h := make([]byte, 0x20)
	if _, err := f.ReadAt(h, 0); err != nil || binary.BigEndian.Uint32(h) != 0x20 || (string(h[4:6]) != "Is" && string(h[4:6]) != "ib") {
		return Disc{}, false
	}
	align := func(n uint32) int64 { return int64((n + 63) &^ 63) }
	certSize, tikSize := binary.BigEndian.Uint32(h[8:]), binary.BigEndian.Uint32(h[16:])
	tmdOff := align(0x20) + align(certSize) + align(tikSize)
	tid := make([]byte, 8)
	f.ReadAt(tid, tmdOff+0x18C)
	// Title IDs 00010005-xxxxxxxx are add-on content (DLC) for a game: it can
	// only be installed, not started. Some dumps are named "(DLC)" too.
	dlc := binary.BigEndian.Uint32(tid[0:4]) == 0x00010005 ||
		strings.Contains(strings.ToLower(filepath.Base(path)), "(dlc)")
	id := strings.TrimRight(string(tid[4:8]), "\x00")
	name := CleanName(filepath.Base(path))
	name = strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(name, "WAD"), "-"))
	return Disc{Platform: "WiiWare", ID: id, Title: name, DLC: dlc}, true
}

// ---------------- names ----------------

var (
	reBrackets       = regexp.MustCompile(`\s*[\[\(][^\]\)]*[\]\)]`)
	reSpaces         = regexp.MustCompile(`\s+`)
	rePlatformPrefix = regexp.MustCompile(`(?i)^\s*(wad|wii\s*ware|wii|gc|ngc|game\s*cube)\b\s*[-–:]*\s*`)
	reNumberPrefix   = regexp.MustCompile(`^\s*\d{1,4}\s*[-–.]\s+`)
	reGameCode       = regexp.MustCompile(`^[A-Z0-9]{4,8}$`)
	reDashColon      = regexp.MustCompile(`\s*-\s+`)
	reTrailingID     = regexp.MustCompile(`([a-z.!?])([A-Z0-9]{6})$`)
)

// CleanName turns "Super Mario Sunshine (USA) [GMSE01].iso" into "Super Mario Sunshine".
func CleanName(file string) string {
	n := strings.TrimSuffix(file, filepath.Ext(file))
	n = strings.ReplaceAll(n, "_", " ")
	n = strings.ReplaceAll(n, "  ", " – ") // dumps often turn "A: B" / "A – B" into a double space
	n = reBrackets.ReplaceAllString(n, "")
	n = reSpaces.ReplaceAllString(n, " ")
	n = strings.TrimSpace(strings.Trim(n, " -–+"))
	if n == "" {
		return strings.TrimSuffix(file, filepath.Ext(file))
	}
	return n
}

// DisplayName prefers a tidied file name ("WII   - Wii Sports [RSPE01].wbfs"
// -> "Wii Sports") and falls back to the name stored on the disc.
func DisplayName(file string, d Disc) string {
	n := CleanName(file)
	n = rePlatformPrefix.ReplaceAllString(n, "")
	n = reNumberPrefix.ReplaceAllString(n, "")
	if d.ID != "" {
		n = strings.TrimSuffix(n, d.ID)
	}
	n = reTrailingID.ReplaceAllString(n, "$1")
	n = reDashColon.ReplaceAllString(n, ": ")
	n = strings.Trim(strings.TrimSpace(n), "-–: ")
	letters := 0
	for _, r := range n {
		if unicode.IsLetter(r) {
			letters++
		}
	}
	// File names like "GMSE01" or "hi-temunitu" (scene release names) say nothing; use the disc's name.
	looksLikeCode := reGameCode.MatchString(n) || (d.ID != "" && strings.EqualFold(n, d.ID)) ||
		!strings.ContainsAny(strings.ToLower(n), "aeiouy") || (!strings.Contains(n, " ") && strings.ToLower(n) == n)
	if (letters >= 3 && !looksLikeCode) || d.Title == "" {
		return n
	}
	return NiceTitle(d.Title)
}

// NiceTitle makes the on-disc name readable: "SONIC HEROES" -> "Sonic Heroes".
func NiceTitle(t string) string {
	if t == "" || strings.ToUpper(t) != t {
		return t
	}
	words := strings.Fields(strings.ToLower(t))
	small := map[string]bool{"of": true, "the": true, "and": true, "a": true, "in": true, "on": true}
	for i, w := range words {
		if i > 0 && small[w] {
			continue
		}
		if len(w) <= 3 && strings.ContainsAny(w, "0123456789") || w == "ii" || w == "iii" || w == "dx" || w == "tmnt" {
			words[i] = strings.ToUpper(w)
			continue
		}
		parts := strings.Split(w, "-")
		for j, p := range parts {
			if p != "" {
				parts[j] = strings.ToUpper(p[:1]) + p[1:]
			}
		}
		words[i] = strings.Join(parts, "-")
	}
	return strings.Join(words, " ")
}

// IsAddOn: WiiWare add-on content ("(DLC)") isn't playable by itself.
func IsAddOn(file, name string) bool {
	return strings.HasSuffix(strings.ToLower(name), "(dlc)") || strings.Contains(strings.ToLower(file), "(dlc)")
}
