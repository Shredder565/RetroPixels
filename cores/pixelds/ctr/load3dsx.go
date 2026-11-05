package ctr

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// Loading 3DSX homebrew: three segments (code, read-only data, data with
// BSS) placed from 0x00100000, then relocated. An extended header may add
// an SMDH icon and a RomFS.

// Load3DSX makes a machine running a 3DSX program.
func Load3DSX(file []byte, opts Options) (*Machine, error) {
	le := binary.LittleEndian
	if len(file) < 0x20 || string(file[:4]) != "3DSX" {
		return nil, errors.New("not a 3DSX file")
	}
	hdrSize := uint32(le.Uint16(file[4:]))
	relSize := uint32(le.Uint16(file[6:]))
	codeSize := le.Uint32(file[0x10:])
	roSize := le.Uint32(file[0x14:])
	dataSize := le.Uint32(file[0x18:])
	bssSize := le.Uint32(file[0x1c:])
	m := newMachine(opts)
	m.title = "3DSX program"
	if hdrSize >= 0x2c {
		smdhOff, smdhSize, fsOff := le.Uint32(file[0x20:]), le.Uint32(file[0x24:]), le.Uint32(file[0x28:])
		if smdhOff+smdhSize <= uint32(len(file)) && smdhSize >= 0x208 {
			if t := smdhTitle(file[smdhOff : smdhOff+smdhSize]); t != "" {
				m.title = t
			}
		}
		if fsOff > 0 && fsOff < uint32(len(file)) {
			m.romfs = romfsLevel3(file[fsOff:])
		}
	}
	align := func(v uint32) uint32 { return (v + pageSize - 1) &^ (pageSize - 1) }
	sizes := [3]uint32{align(codeSize), align(roSize), align(dataSize)}
	addrs := [3]uint32{codeBase, codeBase + sizes[0], codeBase + sizes[0] + sizes[1]}
	total := sizes[0] + sizes[1] + sizes[2]
	pages, err := m.mem.allocPages(total / pageSize)
	if err != nil {
		return nil, err
	}
	// allocPages gives pages from the top down; map them in order
	for i := range pages {
		m.mem.mapPhys(codeBase+uint32(i)*pageSize, pages[len(pages)-1-i], pageSize)
	}
	pos := hdrSize
	var rel [3][2]uint32
	for i := range rel {
		if pos+8 > uint32(len(file)) {
			return nil, errors.New("3DSX file is cut short")
		}
		rel[i] = [2]uint32{le.Uint32(file[pos:]), le.Uint32(file[pos+4:])}
		pos += relSize
	}
	lens := [3]uint32{codeSize, roSize, dataSize - bssSize}
	for i := range lens {
		if pos+lens[i] > uint32(len(file)) {
			return nil, errors.New("3DSX file is cut short")
		}
		m.mem.writeBytes(addrs[i], file[pos:pos+lens[i]])
		pos += lens[i]
	}
	translate := func(a uint32) uint32 {
		switch {
		case a < sizes[0]:
			return addrs[0] + a
		case a < sizes[0]+sizes[1]:
			return addrs[1] + a - sizes[0]
		}
		return addrs[2] + a - sizes[0] - sizes[1]
	}
	for seg := 0; seg < 3; seg++ {
		segWords := [3]uint32{codeSize, roSize, dataSize}[seg] / 4
		for kind := 0; kind < 2; kind++ {
			p := addrs[seg]
			end := addrs[seg] + segWords*4
			for n := uint32(0); n < rel[seg][kind]; n++ {
				if pos+4 > uint32(len(file)) {
					return nil, errors.New("3DSX relocations are cut short")
				}
				skip, patch := uint32(le.Uint16(file[pos:])), uint32(le.Uint16(file[pos+2:]))
				pos += 4
				p += skip * 4
				for ; patch > 0 && p < end; patch-- {
					orig := m.mem.Read32(p)
					sub := orig >> 28
					a := translate(orig &^ 0xf0000000)
					if kind == 0 {
						if sub != 0 {
							return nil, fmt.Errorf("3DSX: unknown absolute relocation %d", sub)
						}
						m.mem.Write32(p, a)
					} else {
						d := a - p
						if sub == 1 {
							d &^= 1 << 31
						}
						m.mem.Write32(p, d)
					}
					p += 4
				}
			}
		}
	}
	if err := m.start(codeBase, 0x40000, 0x30); err != nil {
		return nil, err
	}
	return m, nil
}

// start maps the stack and makes the main thread.
func (m *Machine) start(entry, stackSize uint32, prio int32) error {
	pages, err := m.mem.allocPages(stackSize / pageSize)
	if err != nil {
		return err
	}
	for i, pa := range pages {
		m.mem.mapPhys(stackTop-stackSize+uint32(i)*pageSize, pa, pageSize)
	}
	t, err := m.kern.newThread(entry, 0, stackTop, prio)
	if err != nil {
		return err
	}
	m.kern.add(t)
	m.kern.load(t)
	return nil
}

// smdhTitle: the English short title from an SMDH.
func smdhTitle(b []byte) string {
	if len(b) < 0x208 || string(b[:4]) != "SMDH" {
		return ""
	}
	off := 8 + 1*0x200 // English
	var u []rune
	for i := 0; i < 0x80; i += 2 {
		c := binary.LittleEndian.Uint16(b[off+i:])
		if c == 0 {
			break
		}
		u = append(u, rune(c))
	}
	return string(u)
}

// romfsLevel3 finds the level-3 RomFS (what FS reads) in a RomFS image
// with an IVFC header, or returns the data as it is.
func romfsLevel3(b []byte) []byte {
	if len(b) >= 0x60 && string(b[:4]) == "IVFC" {
		lvl3Size := binary.LittleEndian.Uint64(b[0x4c:])
		hdr := binary.LittleEndian.Uint32(b[0x08:])
		off := (uint64(0x60+hdr) + 0xfff) &^ 0xfff
		_ = lvl3Size
		if off < uint64(len(b)) {
			return b[off:]
		}
	}
	return b
}
