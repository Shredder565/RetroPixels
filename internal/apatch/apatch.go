// Package apatch reads Archipelago patch files (.apsm, .aplttp, .apred,
// .apemerald, ...) and makes the game from them.
//
// A patch file is a zip holding archipelago.json (who the patch is for, which
// game, which steps make the ROM) and the files those steps use. RetroPixels
// runs the steps that are the same for every game:
//
//	apply_bsdiff4 <file>   a bsdiff4 difference against the ROM so far
//	apply_tokens <file>    a list of small edits (write, copy, fill, and/or/xor)
//	calc_snes_crc          fix the checksum in a LoROM SNES header
//
// plus Super Metroid's write_crc. Patches made before the steps existed
// ("APDeltaPatch") are one apply_bsdiff4 of delta.bsdiff4. Games whose patch
// runs that game's own Python code can't be made here; the person makes the
// ROM with the Archipelago Launcher instead and opens that.
//
// RetroPixels.exe (the web player) and RetroPixels Native both use it.
package apatch

import (
	"archive/zip"
	"bytes"
	"crypto/md5"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"path"
	"strings"
)

const (
	ContainerVersion = 7         // the newest patch container version this reader knows
	MaxPatch         = 64 << 20  // patch files are small; this is generous
	MaxROM           = 128 << 20 // the largest base ROM accepted
)

type step struct {
	Name string
	Args []string
}

// Patch is what a patch file says, and the files it holds.
type Patch struct {
	Game         string   `json:"game"`
	Player       int      `json:"player"`
	PlayerName   string   `json:"playerName"`
	Server       string   `json:"server"`
	PatchEnding  string   `json:"patchEnding"`
	ResultEnding string   `json:"resultEnding"`
	BaseMD5      []string `json:"baseMd5"`
	System       string   `json:"system"`      // RetroPixels' name for the console ("snes", "gb", ...)
	Unsupported  string   `json:"unsupported"` // why RetroPixels can't make this ROM itself ("" = it can)

	steps []step
	files map[string][]byte
}

// SystemForExt: the console each kind of output file plays on.
var SystemForExt = map[string]string{
	".sfc": "snes", ".smc": "snes",
	".gb": "gb", ".gbc": "gb",
	".gba": "gba",
	".nes": "nes",
	".z64": "n64", ".n64": "n64", ".v64": "n64",
	".md": "segaMD", ".gen": "segaMD", ".bin": "segaMD",
	".sms": "sms",
	".a26": "atari2600",
}

// SystemForPatch: patch file endings whose manifest may not say what they make.
var SystemForPatch = map[string]string{
	".aplttp": "snes", ".apsm": "snes", ".apsmz3": "snes", ".apsmw": "snes", ".apkdl3": "snes", ".apyi": "snes",
	".apeb": "snes", ".apl2ac": "snes", ".apsoe": "snes", ".apdkc3": "snes", ".apmq": "snes",
	".apred": "gb", ".apblue": "gb", ".apsml2": "gb", ".apladx": "gb",
	".apemerald": "gba", ".apmlss": "gba", ".apygo06": "gba", ".apbn3": "gba", ".apcvcotm": "gba",
	".apmm2": "nes", ".apmm3": "nes", ".aptloz": "nes", ".apff1": "nes",
	".apz5": "n64", ".apcv64": "n64",
	".apzl": "sms",
}

// Steps that some game replaces with its own code, by game.
var overridden = map[string]map[string]bool{
	"Gauntlet Legends": {"apply_bsdiff4": true},
}

func (p *Patch) supportsStep(s step) bool {
	if overridden[p.Game][s.Name] {
		return false
	}
	switch s.Name {
	case "apply_bsdiff4", "apply_tokens":
		return len(s.Args) == 1
	case "calc_snes_crc":
		return true
	case "write_crc":
		return p.Game == "Super Metroid"
	}
	return false
}

// Read reads a patch file's manifest and files (fileName gives the patch
// ending when the manifest doesn't).
func Read(data []byte, fileName string) (*Patch, error) {
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil, errors.New("that isn't an Archipelago patch file (it should be a zip inside)")
	}
	p := &Patch{files: map[string][]byte{}}
	var manifest []byte
	var total int64
	for _, f := range zr.File {
		if strings.HasSuffix(f.Name, "/") {
			continue
		}
		total += int64(f.UncompressedSize64)
		if total > 4*MaxPatch {
			return nil, errors.New("that patch file is far bigger than a patch should be")
		}
		rc, err := f.Open()
		if err != nil {
			return nil, fmt.Errorf("couldn't read %s from the patch: %w", f.Name, err)
		}
		b, err := io.ReadAll(io.LimitReader(rc, 4*MaxPatch))
		rc.Close()
		if err != nil {
			return nil, fmt.Errorf("couldn't read %s from the patch: %w", f.Name, err)
		}
		if f.Name == "archipelago.json" || (manifest == nil && path.Base(f.Name) == "archipelago.json") {
			manifest = b
			continue
		}
		p.files[f.Name] = b
	}
	if manifest == nil {
		return nil, errors.New("that zip isn't an Archipelago patch (it has no archipelago.json)")
	}
	var m struct {
		Game              string          `json:"game"`
		Player            int             `json:"player"`
		PlayerName        string          `json:"player_name"`
		Server            string          `json:"server"`
		PatchFileEnding   string          `json:"patch_file_ending"`
		ResultFileEnding  string          `json:"result_file_ending"`
		BaseChecksum      json.RawMessage `json:"base_checksum"`
		Procedure         json.RawMessage `json:"procedure"`
		CompatibleVersion int             `json:"compatible_version"`
	}
	if err := json.Unmarshal(manifest, &m); err != nil {
		return nil, errors.New("the patch's archipelago.json is damaged")
	}
	p.Game, p.Player, p.PlayerName, p.Server = m.Game, m.Player, m.PlayerName, m.Server
	p.PatchEnding = strings.ToLower(m.PatchFileEnding)
	if p.PatchEnding == "" {
		p.PatchEnding = strings.ToLower(path.Ext(fileName))
	}
	p.ResultEnding = strings.ToLower(m.ResultFileEnding)

	// base_checksum: one MD5, a list of them, or nothing
	var one string
	var many []string
	if json.Unmarshal(m.BaseChecksum, &one) == nil && one != "" {
		p.BaseMD5 = []string{strings.ToLower(one)}
	} else if json.Unmarshal(m.BaseChecksum, &many) == nil {
		for _, s := range many {
			if s != "" {
				p.BaseMD5 = append(p.BaseMD5, strings.ToLower(s))
			}
		}
	}

	p.System = SystemForExt[p.ResultEnding]
	if p.System == "" {
		p.System = SystemForPatch[p.PatchEnding]
	}
	if p.System == "" && p.ResultEnding == "" {
		p.System = "snes" // the container's default output is .sfc
	}

	// procedure: missing (old delta patches), "custom", or a list of [name, [args...]]
	var raw []json.RawMessage
	switch {
	case len(m.Procedure) == 0 || string(m.Procedure) == "null":
		p.steps = []step{{Name: "apply_bsdiff4", Args: []string{"delta.bsdiff4"}}}
	case json.Unmarshal(m.Procedure, &raw) == nil:
		for _, r := range raw {
			var pair []json.RawMessage
			if json.Unmarshal(r, &pair) != nil || len(pair) == 0 {
				return nil, errors.New("the patch's list of steps is damaged")
			}
			var s step
			if json.Unmarshal(pair[0], &s.Name) != nil {
				return nil, errors.New("the patch's list of steps is damaged")
			}
			if len(pair) > 1 {
				var args []any
				if json.Unmarshal(pair[1], &args) != nil {
					return nil, errors.New("the patch's list of steps is damaged")
				}
				for _, a := range args {
					s.Args = append(s.Args, fmt.Sprint(a))
				}
			}
			p.steps = append(p.steps, s)
		}
	default:
		p.Unsupported = "this game's patch is made by its own program in Archipelago"
	}
	if m.CompatibleVersion > ContainerVersion {
		p.Unsupported = "it was made by a newer version of Archipelago than RetroPixels knows"
	}
	if p.Unsupported == "" {
		for _, s := range p.steps {
			if !p.supportsStep(s) {
				p.Unsupported = fmt.Sprintf("making this game's ROM needs Archipelago's own code for %s (step %q)", orGame(p.Game), s.Name)
				break
			}
			if len(s.Args) == 1 && p.files[s.Args[0]] == nil {
				return nil, fmt.Errorf("the patch is missing its file %s", s.Args[0])
			}
		}
	}
	if p.System == "" && p.Unsupported == "" {
		p.Unsupported = "RetroPixels doesn't know which console its ROM (" + p.ResultEnding + ") is for"
	}
	return p, nil
}

func orGame(g string) string {
	if g == "" {
		return "this game"
	}
	return g
}

// BaseCandidates are the ways a dumped ROM might differ from the one a patch
// expects: a copier header on SNES games, the iNES header, N64 byte order.
func BaseCandidates(rom []byte, system string) [][]byte {
	c := [][]byte{rom}
	switch system {
	case "snes":
		if len(rom)%1024 == 512 {
			c = append(c, rom[512:])
		}
	case "nes":
		if len(rom) > 16 && string(rom[:4]) == "NES\x1a" {
			c = append(c, rom[16:])
		}
	case "n64":
		if len(rom) >= 4 {
			switch binary.BigEndian.Uint32(rom) {
			case 0x37804012: // .v64
				out := make([]byte, len(rom))
				for i := 0; i+1 < len(rom); i += 2 {
					out[i], out[i+1] = rom[i+1], rom[i]
				}
				c = append(c, out)
			case 0x40123780: // .n64
				out := make([]byte, len(rom))
				for i := 0; i+3 < len(rom); i += 4 {
					out[i], out[i+1], out[i+2], out[i+3] = rom[i+3], rom[i+2], rom[i+1], rom[i]
				}
				c = append(c, out)
			}
		}
	}
	return c
}

// MD5Hex is the MD5 of b in hex, as base_checksum writes it.
func MD5Hex(b []byte) string { s := md5.Sum(b); return hex.EncodeToString(s[:]) }

// PickBase returns the form of rom the patch was made against.
func (p *Patch) PickBase(rom []byte) ([]byte, error) {
	cands := BaseCandidates(rom, p.System)
	if len(p.BaseMD5) == 0 {
		// Nothing to check against: use the usual form (no copier header, big-endian N64).
		if p.System == "nes" {
			return cands[0], nil
		}
		return cands[len(cands)-1], nil
	}
	for _, c := range cands {
		h := MD5Hex(c)
		for _, want := range p.BaseMD5 {
			if h == want {
				// NES patches are made against the ROM with its iNES header,
				// so keep the header when only the bare ROM matched.
				if p.System == "nes" {
					return rom, nil
				}
				return c, nil
			}
		}
	}
	return nil, &WrongBaseError{Game: orGame(p.Game)}
}

// WrongBaseError: the base ROM given isn't the one the patch was made against.
type WrongBaseError struct{ Game string }

func (e *WrongBaseError) Error() string {
	return "that ROM isn't the version of " + e.Game + " this patch needs. Choose the original ROM Archipelago asks for in its setup guide"
}

// Apply makes the patched ROM from the base ROM.
func (p *Patch) Apply(base []byte) ([]byte, error) {
	if p.Unsupported != "" {
		return nil, errors.New("RetroPixels can't make this ROM: " + p.Unsupported)
	}
	rom, err := p.PickBase(base)
	if err != nil {
		return nil, err
	}
	rom = append([]byte(nil), rom...)
	for _, s := range p.steps {
		switch s.Name {
		case "apply_bsdiff4":
			rom, err = BSDiff4(rom, p.files[s.Args[0]])
		case "apply_tokens":
			rom, err = ApplyTokens(rom, p.files[s.Args[0]])
		case "calc_snes_crc":
			rom, err = SNESHeaderCRC(rom)
		case "write_crc":
			rom, err = SNESMirrorCRC(rom)
		}
		if err != nil {
			return nil, fmt.Errorf("%s: %w", s.Name, err)
		}
	}
	return rom, nil
}

// Token kinds in apply_tokens files.
const (
	tokWrite = 0
	tokCopy  = 1
	tokRLE   = 2
	tokAnd   = 3
	tokOr    = 4
	tokXor   = 5
)

// spliceAt replaces rom[off:off+n] with data the way a Python slice
// assignment does (past the end it appends).
func spliceAt(rom []byte, off int, n int, data []byte) []byte {
	if off > len(rom) {
		off = len(rom)
	}
	end := off + n
	if end > len(rom) {
		end = len(rom)
	}
	if end-off == len(data) {
		copy(rom[off:], data)
		return rom
	}
	out := make([]byte, 0, len(rom)-(end-off)+len(data))
	out = append(out, rom[:off]...)
	out = append(out, data...)
	return append(out, rom[end:]...)
}

// ApplyTokens runs a token file: u32 count, then per token
// u8 kind, u32 offset, u32 size, size bytes of arguments (all little-endian).
func ApplyTokens(rom, tok []byte) ([]byte, error) {
	bad := errors.New("the token data is damaged")
	if len(tok) < 4 {
		return nil, bad
	}
	count := binary.LittleEndian.Uint32(tok)
	p := 4
	for i := uint32(0); i < count; i++ {
		if p+9 > len(tok) {
			return nil, bad
		}
		kind := tok[p]
		off := int(binary.LittleEndian.Uint32(tok[p+1:]))
		size := int(binary.LittleEndian.Uint32(tok[p+5:]))
		p += 9
		if size < 0 || size > len(tok)-p {
			return nil, bad
		}
		arg := tok[p : p+size]
		p += size
		switch kind {
		case tokAnd, tokOr, tokXor:
			if size < 1 || off >= len(rom) {
				return nil, bad
			}
			switch kind {
			case tokAnd:
				rom[off] &= arg[0]
			case tokOr:
				rom[off] |= arg[0]
			default:
				rom[off] ^= arg[0]
			}
		case tokCopy, tokRLE:
			if size < 8 {
				return nil, bad
			}
			n := int(binary.LittleEndian.Uint32(arg))
			v := int(binary.LittleEndian.Uint32(arg[4:]))
			if n > MaxROM {
				return nil, bad
			}
			var data []byte
			if kind == tokCopy {
				if v > len(rom) {
					v = len(rom)
				}
				e := v + n
				if e > len(rom) {
					e = len(rom)
				}
				data = append([]byte(nil), rom[v:e]...)
			} else {
				if v > 255 {
					return nil, bad
				}
				data = bytes.Repeat([]byte{byte(v)}, n)
			}
			rom = spliceAt(rom, off, n, data)
		case tokWrite:
			rom = spliceAt(rom, off, size, arg)
		default:
			return nil, bad
		}
	}
	return rom, nil
}

// SNESHeaderCRC is calc_snes_crc: the checksum of everything but the four
// checksum bytes, plus what those bytes add up to once filled in (0x1FE).
func SNESHeaderCRC(rom []byte) ([]byte, error) {
	if len(rom) < 0x8000 {
		return nil, errors.New("the ROM is too small to be a SNES game")
	}
	sum := 0x1fe
	for i, b := range rom {
		if i < 0x7fdc || i >= 0x7fe0 {
			sum += int(b)
		}
	}
	crc := sum & 0xffff
	inv := crc ^ 0xffff
	rom[0x7fdc], rom[0x7fdd], rom[0x7fde], rom[0x7fdf] = byte(inv), byte(inv>>8), byte(crc), byte(crc>>8)
	return rom, nil
}

// mirrorSum is the SNES checksum of a ROM whose size isn't a power of two:
// the part past the largest power of two counts as if repeated to fill it.
func mirrorSum(b []byte, mask int) int {
	n := len(b)
	for mask > 0 && n&mask == 0 {
		mask >>= 1
	}
	if mask > n {
		mask = n
	}
	part1 := 0
	for _, v := range b[:mask] {
		part1 += int(v)
	}
	part1 &= 0xffff
	part2 := 0
	if next := n - mask; next > 0 {
		part2 = mirrorSum(b[mask:], mask>>1)
		for next < mask {
			next += next
			part2 += part2
		}
	}
	return (part1 + part2) & 0xffff
}

// SNESMirrorCRC is Super Metroid's write_crc.
func SNESMirrorCRC(rom []byte) ([]byte, error) {
	if len(rom) < 0x8000 {
		return nil, errors.New("the ROM is too small to be a SNES game")
	}
	crc := mirrorSum(rom, 0x800000)
	inv := crc ^ 0xffff
	rom[0x7fdc], rom[0x7fdd], rom[0x7fde], rom[0x7fdf] = byte(inv), byte(inv>>8), byte(crc), byte(crc>>8)
	return rom, nil
}

// BaseKey names the base ROM a patch needs, for remembering it: patches for
// the same game take the same ROM.
func (p *Patch) BaseKey() string { return p.Game + "|" + p.PatchEnding }

// IsPatchName reports whether a file name looks like an Archipelago patch
// file (.apsm, .apred, .apemerald...): an ending that starts with "ap".
func IsPatchName(name string) bool {
	ext := strings.ToLower(path.Ext(strings.ReplaceAll(name, "\\", "/")))
	return len(ext) > 3 && strings.HasPrefix(ext, ".ap") && SystemForExt[ext] == ""
}

// UnzipBase: a base ROM given as a .zip is the biggest file inside (as the web
// page does); anything else is returned as it is.
func UnzipBase(data []byte) ([]byte, error) {
	if len(data) < 4 || string(data[:4]) != "PK\x03\x04" {
		return data, nil
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return data, nil
	}
	var best *zip.File
	for _, f := range zr.File {
		if strings.HasSuffix(f.Name, "/") {
			continue
		}
		if best == nil || f.UncompressedSize64 > best.UncompressedSize64 {
			best = f
		}
	}
	if best == nil {
		return nil, errors.New("there's no ROM inside that .zip")
	}
	if best.UncompressedSize64 > MaxROM {
		return nil, errors.New("the file inside that .zip is too big to be a ROM")
	}
	rc, err := best.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	return io.ReadAll(io.LimitReader(rc, MaxROM+1))
}
