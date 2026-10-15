package nes

import (
	"errors"
	"fmt"
	"regexp"
	"strings"
)

// Nametable mirroring.
const (
	mirH = iota
	mirV
	mirOneLo
	mirOneHi
	mirFour
	mirCustom
)

type cart struct {
	prg, chr     []byte
	chrRam       bool
	mapper       int
	submapper    int
	mirror       int
	battery      bool
	nes2         bool
	prgRamSize   int
	prgNvramSize int
	chrRamSize   int
	flags6       int
	trainer      []byte
	board        string
	vram         []byte // the console's nametable RAM (4 KB, 2 KB used unless four-screen)
}

// UNIF files name the circuit board instead of giving a mapper number.
var unifBoards = map[string]int{
	"NROM": 0, "NROM-128": 0, "NROM-256": 0, "RROM": 0, "RROM-128": 0,
	"SAROM": 1, "SBROM": 1, "SCROM": 1, "SEROM": 1, "SFROM": 1, "SGROM": 1, "SHROM": 1, "SJROM": 1, "SKROM": 1, "SLROM": 1, "SL1ROM": 1, "SNROM": 1, "SOROM": 1, "SUROM": 1, "SXROM": 1,
	"UNROM": 2, "UOROM": 2, "CNROM": 3, "TBROM": 4, "TEROM": 4, "TFROM": 4, "TGROM": 4, "TKROM": 4, "TLROM": 4, "TNROM": 4, "TR1ROM": 4, "TSROM": 4, "TVROM": 4, "B4": 4, "HKROM": 4,
	"EKROM": 5, "ELROM": 5, "ETROM": 5, "EWROM": 5, "AMROM": 7, "ANROM": 7, "AN1ROM": 7, "AOROM": 7, "PEEOROM": 9, "PNROM": 9, "FJROM": 10, "FKROM": 10,
	"UNROM-512-8": 30, "UNROM-512-16": 30, "UNROM-512-32": 30, "CPROM": 13, "BNROM": 34, "NINA001": 34, "GNROM": 66, "MHROM": 66, "CDREAMS": 11,
	"TLSROM": 118, "TKSROM": 118, "TQROM": 119, "SA-72007": 145, "SA-72008": 133, "SA-0036": 149, "SA-0037": 148, "SA-016-1M": 79, "TC-U01-1.5M": 147,
	"SACHEN-8259A": 141, "SACHEN-8259B": 138, "SACHEN-8259C": 139, "SACHEN-8259D": 137, "SA-NROM": 143, "22211": 132, "NTBROM": 68,
	"KS7058": 171,
}

var (
	unifPrefix = regexp.MustCompile(`^(NES|HVC|UNL|BMC|BTL|IREM|KONAMI|TENGEN|TAITO|NAMCOT|SUNSOFT|JALECO|BANDAI|AVE|TXC)-`)
	unifSxROM  = regexp.MustCompile(`^S.ROM$`)
	unifTxROM  = regexp.MustCompile(`^T.ROM$`)
)

func hexDigit(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

// subarray is JS's Uint8Array.subarray: clamped to the data.
func subarray(d []byte, from, to int) []byte {
	if from > len(d) {
		from = len(d)
	}
	if to > len(d) {
		to = len(d)
	}
	if to < from {
		to = from
	}
	return d[from:to]
}

func parseUNIF(data []byte) (*cart, error) {
	chunks := map[string][]byte{}
	var prgs, chrs [16][]byte
	for p := 32; p+8 <= len(data); {
		id := string(data[p : p+4])
		ln := int(int32(uint32(data[p+4]) | uint32(data[p+5])<<8 | uint32(data[p+6])<<16 | uint32(data[p+7])<<24))
		body := subarray(data, p+8, p+8+ln)
		if strings.HasPrefix(id, "PRG") && hexDigit(id[3]) >= 0 {
			prgs[hexDigit(id[3])] = body
		} else if strings.HasPrefix(id, "CHR") && hexDigit(id[3]) >= 0 {
			chrs[hexDigit(id[3])] = body
		} else {
			chunks[id] = body
		}
		p += 8 + ln
		if ln < 0 {
			break
		}
	}
	name := ""
	if mapr, ok := chunks["MAPR"]; ok {
		var sb strings.Builder
		for _, c := range mapr {
			if c == 0 {
				break
			}
			sb.WriteRune(rune(c))
		}
		name = strings.ToUpper(strings.TrimSpace(sb.String()))
	}
	board := unifPrefix.ReplaceAllString(name, "")
	mapper, ok := unifBoards[board]
	if !ok && unifSxROM.MatchString(board) {
		mapper, ok = 1, true
	}
	if !ok && unifTxROM.MatchString(board) {
		mapper, ok = 4, true
	}
	if !ok {
		n := name
		if n == "" {
			n = "unknown board"
		}
		return nil, fmt.Errorf("This NES game uses a circuit board RetroPixels doesn't support yet (%s).", n)
	}
	cat := func(list [16][]byte) []byte {
		var o []byte
		for _, x := range list {
			if x != nil {
				o = append(o, x...)
			}
		}
		if o == nil {
			o = []byte{}
		}
		return o
	}
	prg := cat(prgs)
	chr := cat(chrs)
	chrRam := len(chr) == 0
	if chrRam {
		chr = make([]byte, 0x2000)
	} else if len(chr) < 0x2000 {
		chr = append(chr, make([]byte, 0x2000-len(chr))...)
	}
	m := 5
	if mirr, ok := chunks["MIRR"]; ok {
		if len(mirr) > 0 {
			m = int(mirr[0])
		} else {
			m = -1 // undefined in the web version
		}
	}
	mirror := mirH
	switch m {
	case 0:
		mirror = mirH
	case 1:
		mirror = mirV
	case 2:
		mirror = mirOneLo
	case 3:
		mirror = mirOneHi
	case 4:
		mirror = mirFour
	}
	flags6 := 0
	if m == 1 {
		flags6 = 1
	}
	_, batr := chunks["BATR"]
	cr := 0
	if chrRam {
		cr = 0x2000
	}
	return &cart{prg: padPrg(prg), chr: chr, chrRam: chrRam, mapper: mapper, mirror: mirror, battery: batr,
		prgRamSize: 0x2000, chrRamSize: cr, flags6: flags6, board: board}, nil
}

// padPrg repeats a PRG image smaller than 8 KB to fill 8 KB (the web version
// only does this for iNES files; a UNIF file that small would not run there).
func padPrg(prg []byte) []byte {
	if len(prg) >= 0x2000 {
		return prg
	}
	p := make([]byte, 0x2000)
	if len(prg) > 0 {
		for i := range p {
			p[i] = prg[i%len(prg)]
		}
	}
	return p
}

func parseROM(data []byte) (*cart, error) {
	at := func(i int) byte {
		if i < len(data) {
			return data[i]
		}
		return 0
	}
	if (at(0) == 0x46 && at(1) == 0x44 && at(2) == 0x53 && at(3) == 0x1a) ||
		(at(0) == 0x01 && len(data) >= 15 && string(data[1:15]) == "*NINTENDO-HVC*") {
		return nil, errors.New("This is a Famicom Disk System game. The Disk System needs Nintendo's own start-up program (its BIOS), " +
			"which RetroPixels can't include, so disk games can't be played. Cartridge (.nes) games work.")
	}
	if at(0) == 0x55 && at(1) == 0x4e && at(2) == 0x49 && at(3) == 0x46 {
		return parseUNIF(data)
	}
	if at(0) != 0x4e || at(1) != 0x45 || at(2) != 0x53 || at(3) != 0x1a {
		return nil, errors.New("This isn't an NES game file (.nes)")
	}
	d := func(i int) int { return int(at(i)) }
	nes2 := d(7)&0x0c == 0x08
	prgSize, chrSize := d(4)*0x4000, d(5)*0x2000
	mapper, submapper := d(6)>>4|d(7)&0xf0, 0
	prgRamSize, prgNvramSize, chrRamSize := 0x2000, 0, 0x2000
	if nes2 {
		mapper |= (d(8) & 0x0f) << 8
		submapper = d(8) >> 4
		// Sizes: either a bank count, or (when the top nibble is $F) an exponent-multiplier.
		big := func(lo, hi, unit int) int {
			if hi == 0x0f {
				return (1 << (lo >> 2)) * ((lo&3)*2 + 1)
			}
			return (hi<<8 | lo) * unit
		}
		prgSize = big(d(4), d(9)&0x0f, 0x4000)
		chrSize = big(d(5), d(9)>>4, 0x2000)
		shift := func(n int) int {
			if n != 0 {
				return 64 << n
			}
			return 0
		}
		prgRamSize = shift(d(10) & 0x0f)
		prgNvramSize = shift(d(10) >> 4)
		chrRamSize = shift(d(11)&0x0f) + shift(d(11)>>4)
	}
	// Old dumps sometimes have junk in bytes 7-15; ignore the high nibble then.
	if !nes2 && (d(12) != 0 || d(13) != 0 || d(14) != 0 || d(15) != 0) {
		mapper &= 0x0f
	}
	trainer := 0
	if d(6)&4 != 0 {
		trainer = 512
	}
	off := 16 + trainer
	prg := padPrg(append([]byte{}, subarray(data, off, off+prgSize)...))
	off += prgSize
	chr := append([]byte{}, subarray(data, off, off+chrSize)...)
	chrRam := len(chr) == 0
	if chrRam {
		chr = make([]byte, max(0x2000, chrRamSize))
	} else if len(chr) < 0x2000 {
		// (only possible with an odd NES 2.0 size; the web version would read
		// past the end, here it reads zeros)
		chr = append(chr, make([]byte, 0x2000-len(chr))...)
	}
	mirror := mirH
	if d(6)&8 != 0 {
		mirror = mirFour
	} else if d(6)&1 != 0 {
		mirror = mirV
	}
	c := &cart{prg: prg, chr: chr, chrRam: chrRam, mapper: mapper, submapper: submapper, mirror: mirror,
		battery: d(6)&2 != 0, nes2: nes2, prgRamSize: prgRamSize, prgNvramSize: prgNvramSize, chrRamSize: chrRamSize, flags6: d(6)}
	if chrRam {
		c.chrRamSize = len(chr)
	}
	if trainer != 0 {
		c.trainer = append([]byte{}, subarray(data, 16, 16+512)...)
	}
	return c, nil
}
