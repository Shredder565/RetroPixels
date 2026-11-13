package nds

import "encoding/binary"

const pageSize = 16 << 10

// region is an address space made of 16 KB pages of VRAM banks; nil pages
// read as zero.
type region [][]byte

func (r region) page(a uint32) []byte { return r[(a>>14)%uint32(len(r))] }

func (r region) read8(a uint32) uint8 {
	if p := r.page(a); p != nil {
		return p[a&0x3fff]
	}
	return 0
}

func (r region) read16(a uint32) uint16 {
	if p := r.page(a); p != nil {
		return binary.LittleEndian.Uint16(p[a&0x3ffe:])
	}
	return 0
}

func (r region) read32(a uint32) uint32 {
	if p := r.page(a); p != nil {
		return binary.LittleEndian.Uint32(p[a&0x3ffc:])
	}
	return 0
}

func (r region) write8(a uint32, v uint8) {
	if p := r.page(a); p != nil {
		p[a&0x3fff] = v
	}
}

func (r region) write16(a uint32, v uint16) {
	if p := r.page(a); p != nil {
		binary.LittleEndian.PutUint16(p[a&0x3ffe:], v)
	}
}

func (r region) write32(a uint32, v uint32) {
	if p := r.page(a); p != nil {
		binary.LittleEndian.PutUint32(p[a&0x3ffc:], v)
	}
}

var bankSizes = [9]int{128 << 10, 128 << 10, 128 << 10, 128 << 10, 64 << 10, 16 << 10, 16 << 10, 32 << 10, 16 << 10}

// vram is the nine banks and where VRAMCNT maps them.
type vram struct {
	banks   [9][]byte
	cnt     [9]uint8
	wramcnt uint8

	abg, bbg, aobj, bobj, lcdc, arm7 region
	tex, texpal                      region
	abgExt, aobjExt, bbgExt, bobjExt region
	dirtyTex                         bool // texture memory changed (the 3D engine's caches)
}

func (v *vram) init() {
	for i := range v.banks {
		v.banks[i] = make([]byte, bankSizes[i])
	}
	v.remap()
}

func (v *vram) setCnt(bank int, val uint8) {
	if v.cnt[bank] == val {
		return
	}
	v.cnt[bank] = val
	v.remap()
}

// remap rebuilds every region from the VRAMCNT registers.
func (v *vram) remap() {
	v.abg = make(region, 32)
	v.bbg = make(region, 8)
	v.aobj = make(region, 16)
	v.bobj = make(region, 8)
	v.lcdc = make(region, 64)
	v.arm7 = make(region, 16)
	v.tex = make(region, 32)
	v.texpal = make(region, 8)
	v.abgExt = make(region, 2)
	v.aobjExt = make(region, 1)
	v.bbgExt = make(region, 2)
	v.bobjExt = make(region, 1)
	v.dirtyTex = true
	lcdcPage := [9]int{0, 8, 16, 24, 32, 36, 37, 38, 40}
	for b := 0; b < 9; b++ {
		c := v.cnt[b]
		if c&0x80 == 0 {
			continue
		}
		mst, ofs := int(c&7), int(c>>3&3)
		if b < 2 {
			mst &= 3
		}
		bank := v.banks[b]
		n := len(bank) / pageSize
		put := func(r region, start int) {
			for i := 0; i < n; i++ {
				if p := start + i; p < len(r) {
					r[p] = bank[i*pageSize : (i+1)*pageSize]
				}
			}
		}
		putN := func(r region, start, pages int) {
			for i := 0; i < pages && i < n; i++ {
				if p := start + i; p < len(r) {
					r[p] = bank[i*pageSize : (i+1)*pageSize]
				}
			}
		}
		if mst == 0 {
			put(v.lcdc, lcdcPage[b])
			continue
		}
		switch b {
		case 0, 1, 2, 3: // A-D, 128 KB
			switch mst {
			case 1:
				put(v.abg, ofs*8)
			case 2:
				if b < 2 {
					put(v.aobj, (ofs&1)*8)
				} else {
					put(v.arm7, (ofs&1)*8)
				}
			case 3:
				put(v.tex, ofs*8)
			case 4:
				if b == 2 {
					put(v.bbg, 0)
				} else if b == 3 {
					put(v.bobj, 0)
				}
			}
		case 4: // E, 64 KB
			switch mst {
			case 1:
				put(v.abg, 0)
			case 2:
				put(v.aobj, 0)
			case 3:
				put(v.texpal, 0)
			case 4:
				putN(v.abgExt, 0, 2)
			}
		case 5, 6: // F, G, 16 KB
			p := ofs&1 + (ofs>>1)*4
			switch mst {
			case 1:
				put(v.abg, p)
			case 2:
				put(v.aobj, p)
			case 3:
				put(v.texpal, p)
			case 4:
				put(v.abgExt, ofs&1)
			case 5:
				put(v.aobjExt, 0)
			}
		case 7: // H, 32 KB
			switch mst & 3 {
			case 1:
				put(v.bbg, 0)
			case 2:
				put(v.bbgExt, 0)
			}
		case 8: // I, 16 KB
			switch mst & 3 {
			case 1:
				put(v.bbg, 2)
			case 2:
				put(v.bobj, 0)
			case 3:
				put(v.bobjExt, 0)
			}
		}
	}
	// the BG and OBJ spaces repeat through their address ranges
}

// arm9Region is the region an ARM9 address in 0x06000000-0x06FFFFFF reaches,
// and the address within it.
func (v *vram) arm9Region(a uint32) (region, uint32) {
	switch a >> 21 & 7 {
	case 0:
		return v.abg, a & 0x7ffff
	case 1:
		return v.bbg, a & 0x1ffff
	case 2:
		return v.aobj, a & 0x3ffff
	case 3:
		return v.bobj, a & 0x1ffff
	}
	off := a & 0xfffff
	if off >= 0xa4000 {
		return nil, 0
	}
	return v.lcdc, off
}

// arm7Stat is VRAMSTAT: which of banks C and D the ARM7 has.
func (v *vram) arm7Stat() uint8 {
	var s uint8
	if v.cnt[2]&0x87 == 0x82 {
		s |= 1
	}
	if v.cnt[3]&0x87 == 0x82 {
		s |= 2
	}
	return s
}
