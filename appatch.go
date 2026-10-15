package main

// Archipelago patch files (.apsm, .aplttp, .apred, .apemerald, ...).
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
	"net/http"
	"path"
	"strings"
)

const (
	apContainerVersion = 7         // the newest patch container version this reader knows
	apMaxPatch         = 64 << 20  // patch files are small; this is generous
	apMaxROM           = 128 << 20 // the largest base ROM accepted
)

type apStep struct {
	Name string
	Args []string
}

type apPatch struct {
	Game         string   `json:"game"`
	Player       int      `json:"player"`
	PlayerName   string   `json:"playerName"`
	Server       string   `json:"server"`
	PatchEnding  string   `json:"patchEnding"`
	ResultEnding string   `json:"resultEnding"`
	BaseMD5      []string `json:"baseMd5"`
	System       string   `json:"system"`      // RetroPixels' name for the console ("snes", "gb", ...)
	Unsupported  string   `json:"unsupported"` // why RetroPixels can't make this ROM itself ("" = it can)

	steps []apStep
	files map[string][]byte
}

// The console each kind of output file plays on.
var apSystemForExt = map[string]string{
	".sfc": "snes", ".smc": "snes",
	".gb": "gb", ".gbc": "gb",
	".gba": "gba",
	".nes": "nes",
	".z64": "n64", ".n64": "n64", ".v64": "n64",
	".md": "segaMD", ".gen": "segaMD", ".bin": "segaMD",
	".sms": "sms",
	".a26": "atari2600",
}

// Patch file endings whose manifest may not say what they make.
var apSystemForPatch = map[string]string{
	".aplttp": "snes", ".apsm": "snes", ".apsmz3": "snes", ".apsmw": "snes", ".apkdl3": "snes", ".apyi": "snes",
	".apeb": "snes", ".apl2ac": "snes", ".apsoe": "snes", ".apdkc3": "snes", ".apmq": "snes",
	".apred": "gb", ".apblue": "gb", ".apsml2": "gb", ".apladx": "gb",
	".apemerald": "gba", ".apmlss": "gba", ".apygo06": "gba", ".apbn3": "gba", ".apcvcotm": "gba",
	".apmm2": "nes", ".apmm3": "nes", ".aptloz": "nes", ".apff1": "nes",
	".apz5": "n64", ".apcv64": "n64",
	".apzl": "sms",
}

// Steps that some game replaces with its own code, by game.
var apOverridden = map[string]map[string]bool{
	"Gauntlet Legends": {"apply_bsdiff4": true},
}

func (p *apPatch) supportsStep(s apStep) bool {
	if apOverridden[p.Game][s.Name] {
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

// readAPPatch reads a patch file's manifest and files.
func readAPPatch(data []byte, fileName string) (*apPatch, error) {
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil, errors.New("that isn't an Archipelago patch file (it should be a zip inside)")
	}
	p := &apPatch{files: map[string][]byte{}}
	var manifest []byte
	var total int64
	for _, f := range zr.File {
		if strings.HasSuffix(f.Name, "/") {
			continue
		}
		total += int64(f.UncompressedSize64)
		if total > 4*apMaxPatch {
			return nil, errors.New("that patch file is far bigger than a patch should be")
		}
		rc, err := f.Open()
		if err != nil {
			return nil, fmt.Errorf("couldn't read %s from the patch: %w", f.Name, err)
		}
		b, err := io.ReadAll(io.LimitReader(rc, 4*apMaxPatch))
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

	p.System = apSystemForExt[p.ResultEnding]
	if p.System == "" {
		p.System = apSystemForPatch[p.PatchEnding]
	}
	if p.System == "" && p.ResultEnding == "" {
		p.System = "snes" // the container's default output is .sfc
	}

	// procedure: missing (old delta patches), "custom", or a list of [name, [args...]]
	var raw []json.RawMessage
	switch {
	case len(m.Procedure) == 0 || string(m.Procedure) == "null":
		p.steps = []apStep{{Name: "apply_bsdiff4", Args: []string{"delta.bsdiff4"}}}
	case json.Unmarshal(m.Procedure, &raw) == nil:
		for _, r := range raw {
			var pair []json.RawMessage
			if json.Unmarshal(r, &pair) != nil || len(pair) == 0 {
				return nil, errors.New("the patch's list of steps is damaged")
			}
			var s apStep
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
	if m.CompatibleVersion > apContainerVersion {
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

// baseCandidates are the ways a dumped ROM might differ from the one a patch
// expects: a copier header on SNES games, the iNES header, N64 byte order.
func baseCandidates(rom []byte, system string) [][]byte {
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

func md5hex(b []byte) string { s := md5.Sum(b); return hex.EncodeToString(s[:]) }

// pickBase returns the form of rom the patch was made against.
func (p *apPatch) pickBase(rom []byte) ([]byte, error) {
	cands := baseCandidates(rom, p.System)
	if len(p.BaseMD5) == 0 {
		// Nothing to check against: use the usual form (no copier header, big-endian N64).
		if p.System == "nes" {
			return cands[0], nil
		}
		return cands[len(cands)-1], nil
	}
	for _, c := range cands {
		h := md5hex(c)
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
	return nil, &apWrongBase{game: orGame(p.Game)}
}

type apWrongBase struct{ game string }

func (e *apWrongBase) Error() string {
	return "that ROM isn't the version of " + e.game + " this patch needs. Choose the original ROM Archipelago asks for in its setup guide"
}

// apply makes the patched ROM from the base ROM.
func (p *apPatch) apply(base []byte) ([]byte, error) {
	if p.Unsupported != "" {
		return nil, errors.New("RetroPixels can't make this ROM: " + p.Unsupported)
	}
	rom, err := p.pickBase(base)
	if err != nil {
		return nil, err
	}
	rom = append([]byte(nil), rom...)
	for _, s := range p.steps {
		switch s.Name {
		case "apply_bsdiff4":
			rom, err = bsdiff4Patch(rom, p.files[s.Args[0]])
		case "apply_tokens":
			rom, err = applyAPTokens(rom, p.files[s.Args[0]])
		case "calc_snes_crc":
			rom, err = snesHeaderCRC(rom)
		case "write_crc":
			rom, err = snesMirrorCRC(rom)
		}
		if err != nil {
			return nil, fmt.Errorf("%s: %w", s.Name, err)
		}
	}
	return rom, nil
}

// Token kinds in apply_tokens files.
const (
	apTokWrite = 0
	apTokCopy  = 1
	apTokRLE   = 2
	apTokAnd   = 3
	apTokOr    = 4
	apTokXor   = 5
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

// applyAPTokens runs a token file: u32 count, then per token
// u8 kind, u32 offset, u32 size, size bytes of arguments (all little-endian).
func applyAPTokens(rom, tok []byte) ([]byte, error) {
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
		case apTokAnd, apTokOr, apTokXor:
			if size < 1 || off >= len(rom) {
				return nil, bad
			}
			switch kind {
			case apTokAnd:
				rom[off] &= arg[0]
			case apTokOr:
				rom[off] |= arg[0]
			default:
				rom[off] ^= arg[0]
			}
		case apTokCopy, apTokRLE:
			if size < 8 {
				return nil, bad
			}
			n := int(binary.LittleEndian.Uint32(arg))
			v := int(binary.LittleEndian.Uint32(arg[4:]))
			if n > apMaxROM {
				return nil, bad
			}
			var data []byte
			if kind == apTokCopy {
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
		case apTokWrite:
			rom = spliceAt(rom, off, size, arg)
		default:
			return nil, bad
		}
	}
	return rom, nil
}

// snesHeaderCRC is calc_snes_crc: the checksum of everything but the four
// checksum bytes, plus what those bytes add up to once filled in (0x1FE).
func snesHeaderCRC(rom []byte) ([]byte, error) {
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

// snesMirrorCRC is Super Metroid's write_crc.
func snesMirrorCRC(rom []byte) ([]byte, error) {
	if len(rom) < 0x8000 {
		return nil, errors.New("the ROM is too small to be a SNES game")
	}
	crc := mirrorSum(rom, 0x800000)
	inv := crc ^ 0xffff
	rom[0x7fdc], rom[0x7fdd], rom[0x7fde], rom[0x7fdf] = byte(inv), byte(inv>>8), byte(crc), byte(crc>>8)
	return rom, nil
}

// ---------------- HTTP ----------------

func registerAPPatchRoutes(mux *http.ServeMux) {
	// Body: the patch file. Answers what it is, so the page knows which ROM to ask for.
	mux.HandleFunc("/api/ap/inspect", guard(func(w http.ResponseWriter, r *http.Request) {
		data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, apMaxPatch))
		if err != nil {
			fail(w, errors.New("that patch file is too big"))
			return
		}
		p, err := readAPPatch(data, r.URL.Query().Get("name"))
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, p)
	}))

	// Body: u32 patch length (little-endian), the patch file, then the base ROM.
	// Answers the patched ROM.
	mux.HandleFunc("/api/ap/patch", guard(func(w http.ResponseWriter, r *http.Request) {
		data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, apMaxPatch+apMaxROM+4))
		if err != nil || len(data) < 4 {
			fail(w, errors.New("the patch and ROM didn't arrive"))
			return
		}
		n := int(binary.LittleEndian.Uint32(data))
		if n > len(data)-4 {
			fail(w, errors.New("the patch and ROM didn't arrive"))
			return
		}
		p, err := readAPPatch(data[4:4+n], r.URL.Query().Get("name"))
		if err != nil {
			fail(w, err)
			return
		}
		rom, err := p.apply(data[4+n:])
		if err != nil {
			var wb *apWrongBase
			code := "patch"
			if errors.As(err, &wb) {
				code = "wrongBase"
			}
			writeJSON(w, 400, map[string]any{"error": err.Error(), "code": code})
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(rom)
	}))
}
