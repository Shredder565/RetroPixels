package ctr

import (
	_ "embed"
	"encoding/binary"
)

// The system font. 3DS games draw their text with the shared font that
// APT hands them (GetSharedFont): a memory block mapped at 0x18000000
// holding a small header and, at 0x80, a font in the 3DS's CFNT format
// with its pointers set for that address. PixelDS can't include the
// console's own font, so it builds one from sysfont.bin: glyphs rendered
// from DejaVu Sans by tools/pixelds/sysfont/mkfont.py (licence in
// tools/pixelds/sysfont/LICENSE-DejaVu.txt).

//go:embed sysfont.bin
var sysfontBin []byte

const (
	sharedFontAddr = 0x18000000
	sharedFontSize = 0x332000 // as large as the console's
)

type glyphInfo struct {
	code      uint16
	left      int8
	w, adv    uint8
	cellIndex int
}

// buildSharedFont makes the shared font block's contents for mapping at
// base.
func buildSharedFont(base uint32) []byte {
	le := binary.LittleEndian
	b := sysfontBin
	if len(b) < 12 || string(b[:4]) != "PXFN" {
		return nil
	}
	cw, ch, baseline, ascent, lineFeed := int(b[4]), int(b[5]), b[6], b[7], b[8]
	n := int(le.Uint16(b[10:]))
	glyphs := make([]glyphInfo, n)
	for i := range glyphs {
		g := b[12+6*i:]
		glyphs[i] = glyphInfo{code: le.Uint16(g), left: int8(g[2]), w: g[3], adv: g[4], cellIndex: i}
	}
	cellBytes := cw * ch / 2
	cells := b[12+6*n:]

	// sheets: 256×512 A4 textures, cells of (cw+1)×(ch+1)
	const sheetW, sheetH = 256, 512
	cols, rows := sheetW/(cw+1), sheetH/(ch+1)
	perSheet := cols * rows
	sheets := (n + perSheet - 1) / perSheet
	sheetSize := sheetW * sheetH / 2

	// layout from the CFNT's start (the block's offset 0x80)
	const cfnt = 0x80
	finf := 0x14
	tglp := finf + 0x20
	cwdh := tglp + 0x20
	cwdhSize := (0x10 + 3*n + 3) &^ 3
	cmap := cwdh + cwdhSize
	cmapSize := (0x14 + 2 + 4*n + 3) &^ 3
	sheetOff := (cmap + cmapSize + 0x7f) &^ 0x7f
	fileSize := sheetOff + sheets*sheetSize
	out := make([]byte, cfnt+fileSize)
	f := out[cfnt:]
	addr := func(off int) uint32 { return base + cfnt + uint32(off) }

	// the block header: loaded, region, size
	le.PutUint32(out[0:], 2)
	le.PutUint32(out[4:], 1)
	le.PutUint32(out[8:], uint32(fileSize))

	copy(f, "CFNT")
	le.PutUint16(f[4:], 0xfeff)
	le.PutUint16(f[6:], 0x14)
	le.PutUint32(f[8:], 0x03000000)
	le.PutUint32(f[12:], uint32(fileSize))
	le.PutUint32(f[16:], 4)

	alt := 0
	for i, g := range glyphs {
		if g.code == '?' {
			alt = i
		}
	}
	p := f[finf:]
	copy(p, "FINF")
	le.PutUint32(p[4:], 0x20)
	p[8] = 1 // texture font
	p[9] = lineFeed
	le.PutUint16(p[10:], uint16(alt))
	p[12], p[13], p[14] = 0, uint8(cw), uint8(cw)
	p[15] = 1 // UTF-16
	le.PutUint32(p[16:], addr(tglp+8))
	le.PutUint32(p[20:], addr(cwdh+8))
	le.PutUint32(p[24:], addr(cmap+8))
	p[28], p[29], p[30] = uint8(ch), uint8(cw), ascent

	p = f[tglp:]
	copy(p, "TGLP")
	le.PutUint32(p[4:], 0x20)
	p[8], p[9], p[10], p[11] = uint8(cw), uint8(ch), baseline, uint8(cw)
	le.PutUint32(p[12:], uint32(sheetSize))
	le.PutUint16(p[16:], uint16(sheets))
	le.PutUint16(p[18:], 11) // A4
	le.PutUint16(p[20:], uint16(cols))
	le.PutUint16(p[22:], uint16(rows))
	le.PutUint16(p[24:], sheetW)
	le.PutUint16(p[26:], sheetH)
	le.PutUint32(p[28:], addr(sheetOff))

	p = f[cwdh:]
	copy(p, "CWDH")
	le.PutUint32(p[4:], uint32(cwdhSize))
	le.PutUint16(p[8:], 0)
	le.PutUint16(p[10:], uint16(n-1))
	for i, g := range glyphs {
		e := p[16+3*i:]
		e[0], e[1], e[2] = byte(g.left), g.w, g.adv
	}

	p = f[cmap:]
	copy(p, "CMAP")
	le.PutUint32(p[4:], uint32(cmapSize))
	le.PutUint16(p[8:], 0)
	le.PutUint16(p[10:], 0xffff)
	le.PutUint16(p[12:], 2) // scan: (code, index) pairs
	le.PutUint16(p[20:], uint16(n))
	for i, g := range glyphs {
		le.PutUint16(p[22+4*i:], g.code)
		le.PutUint16(p[24+4*i:], uint16(i))
	}

	// the glyphs into the sheets, 8×8 tiled, two texels a byte
	for i := range glyphs {
		sheet := i / perSheet
		cx, cy := (i%perSheet)%cols*(cw+1), (i%perSheet)/cols*(ch+1)
		s := f[sheetOff+sheet*sheetSize:]
		cell := cells[i*cellBytes:]
		for y := 0; y < ch; y++ {
			for x := 0; x < cw; x++ {
				v := cell[(y*cw+x)/2] >> (4 * (x & 1)) & 15
				t := int(tiledOffset(uint32(cx+x), uint32(cy+y), sheetW))
				s[t/2] |= v << (4 * (t & 1))
			}
		}
	}
	return out
}

// sharedFont makes (once) the memory block with the system font.
func (m *Machine) sharedFont() uint32 {
	if m.fontHandle != 0 {
		return m.fontHandle
	}
	data := buildSharedFont(sharedFontAddr)
	blk, h := m.kern.serviceBlock(sharedFontSize, "shared font")
	for i, pa := range blk.pages {
		off := i * pageSize
		if off >= len(data) {
			break
		}
		copy(m.mem.physSlice(pa, pageSize), data[off:])
	}
	m.fontHandle = h
	return h
}
