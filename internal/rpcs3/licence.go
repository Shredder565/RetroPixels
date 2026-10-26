package rpcs3

// Licences. A game bought on PlayStation Network has its EBOOT.BIN locked
// to a licence: RPCS3 looks for it in its PS3 user's exdata folder
// (dev_hdd0\home\00000001\exdata), named after the game's content ID
// (XX0000-XXXX00000_00-XXXXXXXXXXXXXXXX.rap or .edat). The content ID and
// whether a licence is needed are in the EBOOT's NPDRM block. RetroPixels
// only copies a licence file the player has into that folder, as RPCS3's
// own File > Install Packages/Raps/Edats does.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// DefaultUser is RPCS3's first PS3 user, the one games start as.
const DefaultUser = "00000001"

// LicenceCheck: what a game's EBOOT.BIN needs.
type LicenceCheck struct {
	ContentID string `json:"contentId,omitempty"` // "" when the EBOOT has no NPDRM block (disc games, homebrew)
	Needed    bool   `json:"needed"`              // a .rap or .edat licence has to be in exdata
	Found     string `json:"found,omitempty"`     // the licence file RPCS3 will find
}

// OK: the game can start as far as its licence goes.
func (l LicenceCheck) OK() bool { return !l.Needed || l.Found != "" }

// Message says what the player has to add ("" when nothing).
func (l LicenceCheck) Message() string {
	if l.OK() {
		return ""
	}
	return "This game needs its licence file, " + l.ContentID + ".rap (or " + l.ContentID + ".edat), which isn't in RPCS3 yet. " +
		"Use \"Add licence file…\" and choose your own licence file for it; RetroPixels copies it into RPCS3's exdata folder."
}

// NPDRM licence kinds (the NPD block's licence field).
const (
	licenceNetwork = 1
	licenceLocal   = 2
	licenceFree    = 3
)

// ReadNPD finds the NPDRM block in an EBOOT.BIN (a signed PS3 program) and
// returns its content ID and licence kind. ok is false when there's none
// (a plain or disc program).
func ReadNPD(eboot string) (contentID string, licence uint32, ok bool) {
	f, err := os.Open(eboot)
	if err != nil {
		return "", 0, false
	}
	defer f.Close()
	b, _ := io.ReadAll(io.LimitReader(f, 64<<10))
	if len(b) < 4 || string(b[:4]) != "SCE\x00" {
		return "", 0, false // not a signed program: nothing to unlock
	}
	i := bytes.Index(b, []byte("NPD\x00"))
	if i < 0 || i+16+0x30 > len(b) {
		return "", 0, false
	}
	licence = binary.BigEndian.Uint32(b[i+8:])
	id := b[i+16 : i+16+0x30]
	if z := bytes.IndexByte(id, 0); z >= 0 {
		id = id[:z]
	}
	return string(id), licence, len(id) > 0
}

// reContentID: XX0000-XXXX00000_00-XXXXXXXXXXXXXXXX.
var reContentID = regexp.MustCompile(`^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9_]{16}$`)

// IsContentID tells whether s is a PSN content ID.
func IsContentID(s string) bool { return reContentID.MatchString(s) }

// ExdataDir is where RPCS3 looks for a user's licences.
func ExdataDir(root, user string) string {
	return filepath.Join(root, "dev_hdd0", "home", user, "exdata")
}

// FindLicence returns the licence file for contentID in any of RPCS3's PS3
// users' exdata folders ("" when there's none).
func FindLicence(root, contentID string) string {
	homes, _ := os.ReadDir(filepath.Join(root, "dev_hdd0", "home"))
	users := []string{DefaultUser}
	for _, h := range homes {
		if h.IsDir() && h.Name() != DefaultUser {
			users = append(users, h.Name())
		}
	}
	for _, u := range users {
		entries, err := os.ReadDir(ExdataDir(root, u))
		if err != nil {
			continue
		}
		for _, e := range entries {
			n := e.Name()
			ext := strings.ToLower(filepath.Ext(n))
			if (ext == ".rap" || ext == ".edat") && strings.EqualFold(strings.TrimSuffix(n, filepath.Ext(n)), contentID) {
				return filepath.Join(ExdataDir(root, u), n)
			}
		}
	}
	return ""
}

// CheckLicence tells whether the game whose EBOOT.BIN is eboot needs a
// licence and whether RPCS3 (in root) has it.
func CheckLicence(root, eboot string) LicenceCheck {
	id, kind, ok := ReadNPD(eboot)
	if !ok {
		return LicenceCheck{}
	}
	c := LicenceCheck{ContentID: id, Needed: kind == licenceNetwork || kind == licenceLocal}
	if c.Needed {
		c.Found = FindLicence(root, id)
	}
	return c
}

// AddLicence copies the player's licence file src (.rap or .edat, named
// after its game's content ID) into RPCS3's exdata folder for its first
// user, and returns where it went. Nothing is made or changed: the file is
// copied as it is.
func AddLicence(root, src string) (string, error) {
	name := filepath.Base(src)
	ext := strings.ToLower(filepath.Ext(name))
	if ext != ".rap" && ext != ".edat" {
		return "", errors.New("a licence file is a .rap or .edat file")
	}
	id := strings.TrimSuffix(name, filepath.Ext(name))
	if !IsContentID(strings.ToUpper(id)) {
		return "", errors.New(name + " isn't named like a licence file: RPCS3 finds a licence by its game's content ID " +
			"(XX0000-XXXX00000_00-XXXXXXXXXXXXXXXX" + ext + ")")
	}
	st, err := os.Stat(src)
	if err != nil {
		return "", err
	}
	switch {
	case ext == ".rap" && st.Size() != 16:
		return "", errors.New(name + " isn't a .rap licence (those are 16 bytes)")
	case ext == ".edat":
		f, err := os.Open(src)
		if err != nil {
			return "", err
		}
		head := make([]byte, 4)
		_, err = io.ReadFull(f, head)
		f.Close()
		if err != nil || string(head) != "NPD\x00" {
			return "", errors.New(name + " isn't an .edat licence")
		}
	}
	if !isFile(filepath.Join(root, ExeName)) {
		return "", errors.New("RPCS3 isn't set up yet")
	}
	dir := ExdataDir(root, DefaultUser)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	dst := filepath.Join(dir, strings.ToUpper(id)+ext)
	if sameFile(src, dst) {
		return dst, nil
	}
	b, err := os.ReadFile(src)
	if err != nil {
		return "", err
	}
	tmp := dst + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return "", err
	}
	return dst, os.Rename(tmp, dst)
}

func sameFile(a, b string) bool {
	sa, err1 := os.Stat(a)
	sb, err2 := os.Stat(b)
	return err1 == nil && err2 == nil && os.SameFile(sa, sb)
}
