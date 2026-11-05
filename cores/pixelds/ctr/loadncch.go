package ctr

import (
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
)

// Loading 3DS games: an NCCH (a .cxi, the first partition of an NCSD
// game card image, .3ds or .cci, or the first content of a CIA). The
// extended header says where the code, read-only data and data go, the
// stack size and the main thread's priority; ExeFS holds the code
// (often compressed) and the icon; RomFS the game's files. Games must be
// decrypted.

const mediaUnit = 0x200

// Load3DS makes a machine running a 3DS game from a .3ds, .cci, .cxi or
// .cia file.
func Load3DS(file []byte, opts Options) (*Machine, error) {
	ncch, err := findNCCH(file)
	if err != nil {
		return nil, err
	}
	return loadNCCH(ncch, opts)
}

// findNCCH finds the program's NCCH in a game file.
func findNCCH(f []byte) ([]byte, error) {
	le := binary.LittleEndian
	switch {
	case len(f) >= 0x200 && string(f[0x100:0x104]) == "NCCH":
		return f, nil
	case len(f) >= 0x200 && string(f[0x100:0x104]) == "NCSD":
		off := uint64(le.Uint32(f[0x120:])) * mediaUnit
		size := uint64(le.Uint32(f[0x124:])) * mediaUnit
		if off+size > uint64(len(f)) || size < 0x200 {
			return nil, errors.New("the game card image is cut short")
		}
		return f[off : off+size], nil
	case len(f) >= 0x20 && le.Uint32(f) == 0x2020: // CIA
		align := func(v uint64) uint64 { return (v + 63) &^ 63 }
		hdr := uint64(le.Uint32(f))
		cert, tik, tmd := uint64(le.Uint32(f[0x08:])), uint64(le.Uint32(f[0x0c:])), uint64(le.Uint32(f[0x10:]))
		content := uint64(le.Uint64(f[0x18:]))
		tmdOff := align(hdr) + align(cert) + align(tik)
		off := tmdOff + align(tmd)
		if off+0x200 > uint64(len(f)) {
			return nil, errors.New("the CIA is cut short")
		}
		// the TMD's first content record says whether it's encrypted
		if t := tmdOff; t+0x204+0x30 <= uint64(len(f)) {
			sigSize := map[uint32]uint64{0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3c, 0x10003: 0x200, 0x10004: 0x100, 0x10005: 0x3c}[binary.BigEndian.Uint32(f[t:])]
			if sigSize != 0 {
				rec := t + 4 + sigSize + (0x40-(4+sigSize)%0x40)%0x40 + 0xc4 + 0x900
				if rec+0x10 <= uint64(len(f)) && binary.BigEndian.Uint16(f[rec+6:])&1 != 0 {
					return nil, errors.New("this CIA is encrypted: PixelDS needs decrypted games (as Azahar does)")
				}
			}
		}
		end := min(off+content, uint64(len(f)))
		if string(f[off+0x100:off+0x104]) != "NCCH" {
			return nil, errors.New("this CIA's program couldn't be read: it may be encrypted")
		}
		return f[off:end], nil
	}
	return nil, errors.New("this isn't a 3DS game file (.3ds, .cci, .cxi or .cia)")
}

type codeSeg struct{ addr, pages, size uint32 }

func loadNCCH(n []byte, opts Options) (*Machine, error) {
	le := binary.LittleEndian
	if len(n) < 0x200 {
		return nil, errors.New("the game's header is cut short")
	}
	flags := n[0x188:0x190]
	unit := uint64(mediaUnit) << flags[6]
	if flags[7]&4 == 0 {
		return nil, errors.New("this game is encrypted: PixelDS needs decrypted games (as Azahar does)")
	}
	if flags[5]&2 == 0 {
		return nil, errors.New("this file has no program in it (it's data, an update or DLC)")
	}
	exhSize := le.Uint32(n[0x180:])
	if exhSize < 0x400 || 0x200+0x400 > len(n) {
		return nil, errors.New("the game has no extended header")
	}
	exh := n[0x200 : 0x200+0x800]
	seg := func(o int) codeSeg {
		return codeSeg{le.Uint32(exh[o:]), le.Uint32(exh[o+4:]), le.Uint32(exh[o+8:])}
	}
	text, ro, data := seg(0x10), seg(0x20), seg(0x30)
	stack := le.Uint32(exh[0x1c:])
	bss := le.Uint32(exh[0x3c:])
	compressed := exh[0x0d]&1 != 0
	prio := int32(exh[0x20f])
	exefsOff := uint64(le.Uint32(n[0x1a0:])) * unit
	exefsSize := uint64(le.Uint32(n[0x1a4:])) * unit
	if exefsOff+exefsSize > uint64(len(n)) || exefsSize < 0x200 {
		return nil, errors.New("the game's ExeFS is missing or cut short")
	}
	exefs := n[exefsOff : exefsOff+exefsSize]
	file := func(name string) []byte {
		for i := 0; i < 10; i++ {
			e := exefs[i*16:]
			if strings.TrimRight(string(e[:8]), "\x00") == name {
				off, size := uint64(le.Uint32(e[8:]))+0x200, uint64(le.Uint32(e[12:]))
				if off+size <= uint64(len(exefs)) {
					return exefs[off : off+size]
				}
			}
		}
		return nil
	}
	code := file(".code")
	if code == nil {
		return nil, errors.New("the game has no code in its ExeFS")
	}
	if compressed {
		var err error
		if code, err = blzDecompress(code); err != nil {
			return nil, fmt.Errorf("the game's code couldn't be unpacked: %w", err)
		}
	}
	m := newMachine(opts)
	m.programID = le.Uint64(n[0x118:])
	m.title = strings.TrimRight(string(exh[:8]), "\x00")
	if t := smdhTitle(file("icon")); t != "" {
		m.title = t
	}
	if pc := strings.TrimRight(string(n[0x150:0x160]), "\x00"); pc != "" {
		m.productCode = pc
	}
	// the segments: text, ro and data back to back in .code
	pos := uint32(0)
	roundUp := func(v uint32) uint32 { return (v + pageSize - 1) &^ (pageSize - 1) }
	for _, s := range []struct {
		seg   codeSeg
		extra uint32
	}{{text, 0}, {ro, 0}, {data, roundUp(bss)}} {
		span := s.seg.pages * pageSize // its room in .code
		if span == 0 {
			span = roundUp(s.seg.size)
		}
		size := span + s.extra
		if size == 0 {
			continue
		}
		pages, err := m.mem.allocPages(size / pageSize)
		if err != nil {
			return nil, err
		}
		for i := range pages {
			m.mem.mapPhys(s.seg.addr+uint32(i)*pageSize, pages[len(pages)-1-i], pageSize)
		}
		start := min(pos, uint32(len(code)))
		end := min(pos+span, uint32(len(code)))
		m.mem.writeBytes(s.seg.addr, code[start:end])
		pos += span
	}
	// RomFS
	if romOff, romSize := uint64(le.Uint32(n[0x1b0:]))*unit, uint64(le.Uint32(n[0x1b4:]))*unit; romOff > 0 && romOff < uint64(len(n)) {
		end := min(romOff+romSize, uint64(len(n)))
		m.romfs = romfsLevel3(n[romOff:end])
	}
	if prio <= 0 || prio > 0x3f {
		prio = 0x30
	}
	stack = (stack + pageSize - 1) &^ (pageSize - 1)
	if stack == 0 {
		stack = 0x4000
	}
	if err := m.start(text.addr, stack, prio); err != nil {
		return nil, err
	}
	m.logf("3DS game %q (%s, %016x): code %x+%x, ro %x+%x, data %x+%x+%x bss, priority %#x",
		m.title, m.productCode, m.programID, text.addr, text.size, ro.addr, ro.size, data.addr, data.size, bss, prio)
	return m, nil
}

// blzDecompress unpacks ExeFS code packed with the backwards LZ77 scheme:
// a footer gives the packed region and the unpacked size, and the packed
// region is read from its end, flags byte first, each set bit a copy of
// 3-18 bytes from up to 4 KB further on in the output.
func blzDecompress(c []byte) ([]byte, error) {
	if len(c) < 8 {
		return nil, errors.New("too short")
	}
	le := binary.LittleEndian
	f := le.Uint32(c[len(c)-8:])
	extra := le.Uint32(c[len(c)-4:])
	footer := int(f >> 24)
	packed := int(f & 0xffffff)
	outSize := len(c) + int(extra)
	if footer > len(c) || packed > len(c) || outSize < len(c) {
		return nil, errors.New("bad footer")
	}
	out := make([]byte, outSize)
	copy(out, c)
	idx := len(c) - footer
	stop := len(c) - packed
	o := outSize
	for idx > stop {
		idx--
		flags := c[idx]
		for b := 0; b < 8 && idx > stop && o > 0; b++ {
			if flags&0x80 != 0 {
				if idx-2 < 0 {
					return nil, errors.New("bad data")
				}
				idx -= 2
				v := int(c[idx]) | int(c[idx+1])<<8
				n := v>>12 + 3
				disp := v&0xfff + 2
				if o < n {
					return nil, errors.New("bad length")
				}
				for j := 0; j < n; j++ {
					if o+disp >= outSize {
						return nil, errors.New("bad distance")
					}
					v := out[o+disp]
					o--
					out[o] = v
				}
			} else {
				idx--
				o--
				out[o] = c[idx]
			}
			flags <<= 1
		}
	}
	return out, nil
}
