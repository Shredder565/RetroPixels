package wiiu

// ELFToRPX turns a PowerPC ELF executable into an RPX, the way homebrew
// toolchains do (wut's elf2rpl). RetroPixels uses it to build its own test
// programs; it's also handy for anyone building Wii U homebrew with a plain
// PowerPC compiler.
//
// The ELF must be linked with its relocations kept (ld -q / --emit-relocs),
// code at 0x02000000, data at 0x10000000, and each library's imports in a
// section named .fimport_<lib> (functions) or .dimport_<lib> (data) at
// 0xC0000000 and up, starting with an 8-byte header and the library's name.
// An RPL's exports go in .fexports (functions) and .dexports (data), also at
// 0xC0000000 and up: a count, a word for the signature, then for each one
// its address and the offset of its name from the section's start.

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"hash/crc32"
	"strings"
)

// ELFToRPX converts elf. stackSize is the main thread's stack (0 = 64 KB).
func ELFToRPX(elf []byte, stackSize uint32, compress bool) ([]byte, error) {
	if err := ParseRPL(elf); err != nil {
		return nil, err
	}
	if stackSize == 0 {
		stackSize = 0x10000
	}
	secs, err := readSections(elf)
	if err != nil {
		return nil, err
	}
	shstrndx := int(be16(elf[0x32:]))
	type outSec struct {
		nameOff                                   uint32
		typ, flags, addr, size, link, info, align uint32
		entsize                                   uint32
		data                                      []byte
		zip                                       bool
	}
	var outs []*outSec
	shstr := secs[shstrndx].data
	var textSize, dataSize, loadSize uint32
	shAlign := func(i int) uint32 {
		h := elf[be32(elf[0x20:])+uint32(i)*uint32(be16(elf[0x2e:])):]
		return be32(h[32:])
	}
	for i, s := range secs {
		nameOff := uint32(0)
		h := elf[be32(elf[0x20:])+uint32(i)*uint32(be16(elf[0x2e:])):]
		nameOff = be32(h)
		o := &outSec{nameOff: nameOff, typ: s.typ, flags: s.flags &^ shfZlib, addr: s.addr, size: s.size,
			link: s.link, info: s.info, align: shAlign(i), entsize: s.entsize, data: s.data}
		if strings.HasPrefix(s.name, ".fimport_") || strings.HasPrefix(s.name, ".dimport_") {
			o.typ = shtImports
		}
		if s.name == ".fexports" || s.name == ".dexports" {
			o.typ = shtExports
		}
		if o.flags&shfAlloc != 0 && o.addr != 0 && o.typ != shtImports {
			end := o.addr + o.size
			switch {
			case o.addr >= LoaderBase:
				loadSize = max(loadSize, end-LoaderBase)
			case o.addr >= DataBase:
				dataSize = max(dataSize, end-DataBase)
			default:
				textSize = max(textSize, end-CodeBase)
			}
		}
		if compress && o.data != nil && len(o.data) > 0x40 && o.typ != shtNobits && i != shstrndx {
			o.zip = true
		}
		outs = append(outs, o)
	}

	// section names for the two new sections
	shstr = append([]byte(nil), shstr...)
	crcName := uint32(len(shstr))
	shstr = append(shstr, []byte(".rplcrcs\x00")...)
	fiName := uint32(len(shstr))
	shstr = append(shstr, []byte(".rplfileinfo\x00")...)
	outs[shstrndx].data = shstr
	outs[shstrndx].size = uint32(len(shstr))

	// file info
	fi := make([]byte, 0x60)
	put := func(o int, v uint32) { binary.BigEndian.PutUint32(fi[o:], v) }
	put(0, 0xcafe0402)
	put(4, textSize)
	put(8, 32)
	put(12, dataSize)
	put(16, 4096)
	put(20, loadSize)
	put(24, 4)
	put(44, stackSize)
	put(56, 0x8000) // heap size
	outs = append(outs, nil, nil)

	// CRCs of each section's data
	crcs := make([]byte, 4*len(outs))
	for i, o := range outs[:len(outs)-2] {
		if o.data != nil && o.typ != shtNobits {
			binary.BigEndian.PutUint32(crcs[i*4:], crc32.ChecksumIEEE(o.data))
		}
	}
	outs[len(outs)-2] = &outSec{nameOff: crcName, typ: shtCRCs, size: uint32(len(crcs)), align: 4, entsize: 4, data: crcs}
	outs[len(outs)-1] = &outSec{nameOff: fiName, typ: shtFileInfo, size: uint32(len(fi)), align: 4, data: fi}
	binary.BigEndian.PutUint32(crcs[(len(outs)-1)*4:], crc32.ChecksumIEEE(fi))

	// lay out: header, section table, then the data
	var out bytes.Buffer
	hdr := make([]byte, 0x34)
	copy(hdr, elf[:0x34])
	hdr[7] = 0xca // OSABI: Cafe OS
	hdr[8] = 0xfe
	binary.BigEndian.PutUint16(hdr[0x10:], rplType)
	binary.BigEndian.PutUint32(hdr[0x1c:], 0) // no program headers
	binary.BigEndian.PutUint16(hdr[0x2a:], 0)
	binary.BigEndian.PutUint16(hdr[0x2c:], 0)
	binary.BigEndian.PutUint32(hdr[0x20:], 0x40)
	binary.BigEndian.PutUint16(hdr[0x2e:], 40)
	binary.BigEndian.PutUint16(hdr[0x30:], uint16(len(outs)))
	out.Write(hdr)
	for out.Len() < 0x40 {
		out.WriteByte(0)
	}
	table := make([]byte, 40*len(outs))
	out.Write(table)
	offs := make([]uint32, len(outs))
	sizes := make([]uint32, len(outs))
	for i, o := range outs {
		if o.data == nil || o.typ == shtNobits || o.typ == shtNull {
			sizes[i] = o.size
			continue
		}
		for out.Len()%64 != 0 {
			out.WriteByte(0)
		}
		offs[i] = uint32(out.Len())
		data := o.data
		if o.zip {
			var z bytes.Buffer
			var n [4]byte
			binary.BigEndian.PutUint32(n[:], uint32(len(data)))
			z.Write(n[:])
			w, _ := zlib.NewWriterLevel(&z, 6)
			w.Write(data)
			w.Close()
			data = z.Bytes()
			o.flags |= shfZlib
		}
		out.Write(data)
		sizes[i] = uint32(len(data))
	}
	b := out.Bytes()
	for i, o := range outs {
		h := b[0x40+40*i:]
		binary.BigEndian.PutUint32(h[0:], o.nameOff)
		binary.BigEndian.PutUint32(h[4:], o.typ)
		binary.BigEndian.PutUint32(h[8:], o.flags)
		binary.BigEndian.PutUint32(h[12:], o.addr)
		binary.BigEndian.PutUint32(h[16:], offs[i])
		binary.BigEndian.PutUint32(h[20:], sizes[i])
		binary.BigEndian.PutUint32(h[24:], o.link)
		binary.BigEndian.PutUint32(h[28:], o.info)
		binary.BigEndian.PutUint32(h[32:], o.align)
		binary.BigEndian.PutUint32(h[36:], o.entsize)
	}
	if textSize == 0 {
		return nil, errors.New("the program has no code at 0x02000000")
	}
	return b, nil
}
