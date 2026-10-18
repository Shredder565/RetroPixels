package wiiu

// The RPX / RPL loader.
//
// Wii U programs are ELF files with Cafe OS extensions: the main program is
// an .rpx and its own libraries are .rpl files, both "RPL" ELF files. Their
// sections may be zlib-compressed (flag 0x08000000, with the inflated size in
// front), imports from other libraries are sections named .fimport_<lib>
// (functions) and .dimport_<lib> (data), exports are .fexports / .dexports,
// and a last FILEINFO section holds the stack size, the small-data bases and
// the like. Relocations are kept in the file, and the loader applies them
// once it knows where every import is.

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
)

const (
	shtNull     = 0
	shtProgbits = 1
	shtSymtab   = 2
	shtStrtab   = 3
	shtRela     = 4
	shtNobits   = 8
	shtExports  = 0x80000001
	shtImports  = 0x80000002
	shtCRCs     = 0x80000003
	shtFileInfo = 0x80000004

	shfWrite = 0x1
	shfAlloc = 0x2
	shfExec  = 0x4
	shfZlib  = 0x08000000

	shnAbs = 0xfff1

	rplType = 0xfe01
)

type elfSection struct {
	name                      string
	typ, flags, addr, off     uint32
	size, link, info, entsize uint32
	data                      []byte // inflated contents (nil for NOBITS)
	newAddr                   uint32 // where it went
	importLib                 string // for import sections
	importData                bool
}

// FileInfo is the RPL file information section.
type FileInfo struct {
	TextSize, TextAlign, DataSize, DataAlign, LoadSize, LoadAlign uint32
	TempSize, TrampAdjust, SDABase, SDA2Base, StackSize           uint32
	Flags, HeapSize                                               uint32
	TLSModuleIndex, TLSAlignShift                                 uint16
}

// Module is a loaded RPX or RPL.
type Module struct {
	Name     string
	Entry    uint32
	Info     FileInfo
	IsRPX    bool
	Exports  map[string]uint32
	Symbols  []Symbol // functions, for crash reports
	TextLo   uint32
	TextHi   uint32
	DataLo   uint32
	DataHi   uint32
	LoadHi   uint32 // the end of its loader sections (at LoaderBase and up)
	TLSAddr  uint32 // the TLS template
	TLSSize  uint32
	TLSIndex uint32

	// room after the code for trampolines: far calls to imports go
	// through one (a branch reaches only 32 MB)
	trampNext, trampEnd uint32
	tramps              map[uint32]uint32 // by target
}

// tramp returns a trampoline to target in the module's own room, or 0.
func (mod *Module) tramp(mem *Memory, target uint32) uint32 {
	if a, ok := mod.tramps[target]; ok {
		return a
	}
	if mod.trampNext+16 > mod.trampEnd {
		return 0
	}
	a := mod.trampNext
	mod.trampNext += 16
	mem.Write32(a, 0x3d800000|(target>>16))      // lis r12, hi
	mem.Write32(a+4, 0x618c0000|(target&0xffff)) // ori r12, r12, lo
	mem.Write32(a+8, 0x7d8903a6)                 // mtctr r12
	mem.Write32(a+12, 0x4e800420)                // bctr
	if mod.tramps == nil {
		mod.tramps = map[uint32]uint32{}
	}
	mod.tramps[target] = a
	return a
}

// Symbol is a named address in a module.
type Symbol struct {
	Name string
	Addr uint32
	Size uint32
}

// Importer finds what a module imports: a function or data symbol of a
// library. It returns the address to use.
type Importer interface {
	Import(lib, name string, data bool) (uint32, error)
}

// LoadPlan says where a module's sections go.
type LoadPlan struct {
	// Relocate places the code at TextAt and the data at DataAt instead of
	// the addresses in the file (each RPL after the first).
	Relocate bool
	TextAt   uint32
	DataAt   uint32
	LoadAt   uint32
	// DataAlloc, when set, gives the memory for the data instead of DataAt
	// (0 if there's none).
	DataAlloc func(size uint32) uint32
	// Placed, when set, is told about the module once its sections are in
	// memory and its exports known, before its imports are resolved (which
	// can load other modules, and they must go after this one).
	Placed func(mod *Module)
}

func be32(b []byte) uint32 { return binary.BigEndian.Uint32(b) }
func be16(b []byte) uint16 { return binary.BigEndian.Uint16(b) }

// ParseRPL checks that b is an RPX / RPL (or plain PowerPC ELF) file.
func ParseRPL(b []byte) error {
	if len(b) < 0x34 || string(b[:4]) != "\x7fELF" {
		return errors.New("not an ELF file")
	}
	if b[4] != 1 || b[5] != 2 {
		return errors.New("not a 32-bit big-endian ELF file")
	}
	if be16(b[18:]) != 20 {
		return errors.New("not a PowerPC program")
	}
	return nil
}

func readSections(b []byte) ([]*elfSection, error) {
	shoff := be32(b[0x20:])
	shentsize := uint32(be16(b[0x2e:]))
	shnum := uint32(be16(b[0x30:]))
	shstrndx := uint32(be16(b[0x32:]))
	if shentsize < 40 || uint64(shoff)+uint64(shnum)*uint64(shentsize) > uint64(len(b)) {
		return nil, errors.New("bad section table")
	}
	secs := make([]*elfSection, shnum)
	for i := uint32(0); i < shnum; i++ {
		h := b[shoff+i*shentsize:]
		s := &elfSection{
			typ: be32(h[4:]), flags: be32(h[8:]), addr: be32(h[12:]), off: be32(h[16:]),
			size: be32(h[20:]), link: be32(h[24:]), info: be32(h[28:]), entsize: be32(h[36:]),
		}
		s.name = fmt.Sprint(be32(h[0:])) // name offset for now
		if s.typ != shtNobits && s.typ != shtNull && s.size > 0 {
			if uint64(s.off)+uint64(s.size) > uint64(len(b)) {
				return nil, fmt.Errorf("section %d runs past the end of the file", i)
			}
			raw := b[s.off : s.off+s.size]
			if s.flags&shfZlib != 0 && len(raw) >= 4 {
				n := be32(raw)
				zr, err := zlib.NewReader(bytes.NewReader(raw[4:]))
				if err != nil {
					return nil, fmt.Errorf("section %d: %v", i, err)
				}
				out := make([]byte, n)
				if _, err := io.ReadFull(zr, out); err != nil {
					return nil, fmt.Errorf("section %d: %v", i, err)
				}
				s.data = out
				s.size = n
			} else {
				s.data = raw
			}
		}
		secs[i] = s
	}
	// names
	if shstrndx < shnum && secs[shstrndx].data != nil {
		strs := secs[shstrndx].data
		for _, s := range secs {
			var off uint32
			fmt.Sscan(s.name, &off)
			s.name = cstr(strs, off)
		}
	} else {
		for _, s := range secs {
			s.name = ""
		}
	}
	return secs, nil
}

func cstr(b []byte, off uint32) string {
	if off >= uint32(len(b)) {
		return ""
	}
	e := bytes.IndexByte(b[off:], 0)
	if e < 0 {
		return string(b[off:])
	}
	return string(b[off : off+uint32(e)])
}

func parseFileInfo(d []byte) FileInfo {
	var fi FileInfo
	g := func(o int) uint32 {
		if o+4 <= len(d) {
			return be32(d[o:])
		}
		return 0
	}
	fi.TextSize, fi.TextAlign = g(4), g(8)
	fi.DataSize, fi.DataAlign = g(12), g(16)
	fi.LoadSize, fi.LoadAlign = g(20), g(24)
	fi.TempSize, fi.TrampAdjust = g(28), g(32)
	fi.SDABase, fi.SDA2Base = g(36), g(40)
	fi.StackSize = g(44)
	fi.Flags = g(52)
	fi.HeapSize = g(56)
	if len(d) >= 0x5c {
		fi.TLSModuleIndex = be16(d[0x58:])
		fi.TLSAlignShift = be16(d[0x5a:])
	}
	return fi
}

// LoadRPL loads b into mem. name is the module's name ("men", "game_lib").
func LoadRPL(mem *Memory, b []byte, name string, plan LoadPlan, imp Importer, tramp func(target uint32) uint32) (*Module, error) {
	if err := ParseRPL(b); err != nil {
		return nil, err
	}
	secs, err := readSections(b)
	if err != nil {
		return nil, err
	}
	mod := &Module{Name: name, Exports: map[string]uint32{}, Entry: be32(b[0x18:])}
	mod.IsRPX = be16(b[0x10:]) == rplType && !strings.HasSuffix(strings.ToLower(name), ".rpl")
	for _, s := range secs {
		if s.typ == shtFileInfo && s.data != nil {
			mod.Info = parseFileInfo(s.data)
		}
	}

	// place the sections
	textBase, dataBase, loadBase := ^uint32(0), ^uint32(0), ^uint32(0)
	for _, s := range secs {
		if s.flags&shfAlloc == 0 || s.addr == 0 || s.typ == shtImports {
			continue
		}
		switch {
		case s.addr >= LoaderBase:
			loadBase = min(loadBase, s.addr)
		case s.addr >= DataBase:
			dataBase = min(dataBase, s.addr)
		default:
			textBase = min(textBase, s.addr)
		}
	}
	if plan.Relocate && plan.DataAlloc != nil && dataBase != ^uint32(0) {
		var dataEnd uint32
		for _, s := range secs {
			if s.flags&shfAlloc != 0 && s.addr >= DataBase && s.addr < LoaderBase && s.typ != shtImports {
				dataEnd = max(dataEnd, s.addr+s.size)
			}
		}
		if plan.DataAt = plan.DataAlloc(dataEnd - dataBase); plan.DataAt == 0 {
			return nil, fmt.Errorf("no memory for %s's data (%d KB)", name, (dataEnd-dataBase)>>10)
		}
	}
	mod.TextLo, mod.DataLo = ^uint32(0), ^uint32(0)
	for _, s := range secs {
		if s.flags&shfAlloc == 0 || s.addr == 0 || s.typ == shtImports {
			// (imports aren't needed in memory: their symbols are resolved)
			s.newAddr = s.addr
			continue
		}
		s.newAddr = s.addr
		if plan.Relocate {
			switch {
			case s.addr >= LoaderBase:
				s.newAddr = s.addr - loadBase + plan.LoadAt
			case s.addr >= DataBase:
				s.newAddr = s.addr - dataBase + plan.DataAt
			default:
				s.newAddr = s.addr - textBase + plan.TextAt
			}
		}
		if s.typ == shtNobits {
			mem.Fill(s.newAddr, 0, s.size)
		} else if s.data != nil {
			mem.WriteBytes(s.newAddr, s.data)
		}
		end := s.newAddr + s.size
		switch {
		case s.addr >= LoaderBase:
			mod.LoadHi = max(mod.LoadHi, end)
		case s.addr >= DataBase:
			mod.DataLo = min(mod.DataLo, s.newAddr)
			mod.DataHi = max(mod.DataHi, end)
		default:
			mod.TextLo = min(mod.TextLo, s.newAddr)
			mod.TextHi = max(mod.TextHi, end)
		}
		if strings.HasPrefix(s.name, ".tdata") || strings.HasPrefix(s.name, ".tbss") {
			if mod.TLSAddr == 0 || s.newAddr < mod.TLSAddr {
				mod.TLSAddr = s.newAddr
			}
			mod.TLSSize = max(mod.TLSSize, end-mod.TLSAddr)
		}
	}
	if mod.TextLo == ^uint32(0) {
		mod.TextLo, mod.TextHi = 0, 0
	} else {
		// room for a trampoline per imported function, after the code
		n := uint32(0)
		for _, s := range secs {
			if s.typ != shtSymtab || s.data == nil {
				continue
			}
			for o := 0; o+16 <= len(s.data); o += 16 {
				if x := int(be16(s.data[o+14:])); x > 0 && x < len(secs) && secs[x].typ == shtImports && secs[x].flags&shfExec != 0 {
					n++
				}
			}
		}
		mod.trampNext = (mod.TextHi + 15) &^ 15
		mod.trampEnd = mod.trampNext + n*16
		mem.Fill(mod.trampNext, 0, n*16)
		mod.TextHi = mod.trampEnd
	}
	if mod.DataLo == ^uint32(0) {
		mod.DataLo, mod.DataHi = 0, 0
	}
	// where an address in the file went
	moved := func(a uint32) uint32 {
		if !plan.Relocate {
			return a
		}
		for _, s := range secs {
			if s.flags&shfAlloc != 0 && s.addr != 0 && a >= s.addr && a <= s.addr+s.size {
				return a - s.addr + s.newAddr
			}
		}
		return a
	}
	mod.Entry = moved(mod.Entry)
	if plan.Relocate {
		if mod.Info.SDABase != 0 {
			mod.Info.SDABase = moved(mod.Info.SDABase)
		}
		if mod.Info.SDA2Base != 0 {
			mod.Info.SDA2Base = moved(mod.Info.SDA2Base)
		}
	}

	// exports
	for _, s := range secs {
		if s.typ != shtExports || len(s.data) < 8 {
			continue
		}
		n := be32(s.data)
		for k := uint32(0); k < n && 8+k*8+8 <= uint32(len(s.data)); k++ {
			e := s.data[8+k*8:]
			addr, noff := be32(e), be32(e[4:])&0x7fffffff
			mod.Exports[cstr(s.data, noff)] = moved(addr)
		}
	}

	if plan.Placed != nil {
		plan.Placed(mod)
	}

	// imports
	for _, s := range secs {
		if s.typ != shtImports {
			continue
		}
		lib := ""
		if len(s.data) > 8 {
			lib = cstr(s.data, 8)
		}
		if lib == "" {
			lib = strings.TrimPrefix(strings.TrimPrefix(s.name, ".fimport_"), ".dimport_")
		}
		s.importLib = lib
		s.importData = strings.HasPrefix(s.name, ".dimport_") || s.flags&shfExec == 0
	}

	// symbols
	type sym struct {
		name  string
		value uint32
		size  uint32
		shndx uint16
		info  uint8
	}
	symtabs := map[int][]sym{}
	for i, s := range secs {
		if s.typ != shtSymtab || s.data == nil {
			continue
		}
		var strs []byte
		if int(s.link) < len(secs) {
			strs = secs[s.link].data
		}
		var list []sym
		for o := 0; o+16 <= len(s.data); o += 16 {
			e := s.data[o:]
			list = append(list, sym{cstr(strs, be32(e)), be32(e[4:]), be32(e[8:]), be16(e[14:]), e[12]})
		}
		symtabs[i] = list
	}
	resolved := map[int][]uint32{}
	var importErrs []string
	for i, list := range symtabs {
		vals := make([]uint32, len(list))
		for j, y := range list {
			v := y.value
			switch {
			case y.shndx == 0:
				v = 0
			case y.shndx == shnAbs || int(y.shndx) >= len(secs):
			default:
				ts := secs[y.shndx]
				if ts.typ == shtImports && y.name == "" {
					v = 0 // the section's own symbol
				} else if ts.typ == shtImports {
					a, err := imp.Import(ts.importLib, y.name, ts.importData)
					if err != nil {
						importErrs = append(importErrs, err.Error())
					}
					v = a
				} else if ts.flags&shfAlloc != 0 {
					v = y.value - ts.addr + ts.newAddr
					if y.info&0xf == 2 && y.name != "" && ts.flags&shfExec != 0 { // STT_FUNC
						mod.Symbols = append(mod.Symbols, Symbol{y.name, v, y.size})
					}
				}
			}
			vals[j] = v
		}
		resolved[i] = vals
	}
	if len(importErrs) > 0 {
		return nil, errors.New(strings.Join(importErrs, "; "))
	}
	sort.Slice(mod.Symbols, func(i, j int) bool { return mod.Symbols[i].Addr < mod.Symbols[j].Addr })

	// relocations
	for _, s := range secs {
		if s.typ != shtRela || s.data == nil {
			continue
		}
		if int(s.info) >= len(secs) {
			continue
		}
		target := secs[s.info]
		if target.flags&shfAlloc == 0 {
			continue
		}
		vals := resolved[int(s.link)]
		for o := 0; o+12 <= len(s.data); o += 12 {
			e := s.data[o:]
			off, info, addend := be32(e), be32(e[4:]), be32(e[8:])
			si, typ := info>>8, info&0xff
			var S uint32
			if int(si) < len(vals) {
				S = vals[si]
			}
			P := off - target.addr + target.newAddr
			if err := relocate(mem, mod, typ, P, S, addend, tramp); err != nil {
				return nil, fmt.Errorf("%s: %v", name, err)
			}
		}
	}
	return mod, nil
}

func relocate(mem *Memory, mod *Module, typ, P, S, A uint32, tramp func(uint32) uint32) error {
	v := S + A
	switch typ {
	case 0: // R_PPC_NONE
	case 1: // ADDR32
		mem.Write32(P, v)
	case 2: // ADDR24
		mem.Write32(P, mem.Read32(P)&^0x03fffffc|v&0x03fffffc)
	case 3: // ADDR16
		mem.Write16(P, uint16(v))
	case 4: // ADDR16_LO
		mem.Write16(P, uint16(v))
	case 5: // ADDR16_HI
		mem.Write16(P, uint16(v>>16))
	case 6: // ADDR16_HA
		mem.Write16(P, uint16((v+0x8000)>>16))
	case 7, 8, 9: // ADDR14
		mem.Write32(P, mem.Read32(P)&^0xfffc|v&0xfffc)
	case 10: // REL24
		d := int64(int32(v - P))
		if d < -0x2000000 || d >= 0x2000000 {
			if tramp == nil && mod.trampEnd == 0 {
				return fmt.Errorf("branch from %08X to %08X is out of range", P, v)
			}
			t := mod.tramp(mem, v)
			if (t == 0 || int64(int32(t-P)) < -0x2000000 || int64(int32(t-P)) >= 0x2000000) && tramp != nil {
				t = tramp(v)
			}
			d = int64(int32(t - P))
			if d < -0x2000000 || d >= 0x2000000 {
				return fmt.Errorf("branch from %08X to %08X is out of range", P, v)
			}
		}
		mem.Write32(P, mem.Read32(P)&^0x03fffffc|uint32(d)&0x03fffffc)
	case 11, 12, 13: // REL14
		mem.Write32(P, mem.Read32(P)&^0xfffc|(v-P)&0xfffc)
	case 26: // REL32
		mem.Write32(P, v-P)
	case 68: // DTPMOD32
		mem.Write32(P, mod.TLSIndex)
	case 78: // DTPREL32
		mem.Write32(P, v-mod.TLSAddr)
	case 109: // EMB_SDA21: the instruction's rA picks r13 / r2 / 0
		inst := mem.Read32(P)
		var base uint32
		switch inst >> 16 & 31 {
		case 13:
			base = mod.Info.SDABase
		case 2:
			base = mod.Info.SDA2Base
		}
		mem.Write32(P, inst&^0xffff|(v-base)&0xffff)
	case 116: // EMB_RELSDA
		mem.Write16(P, uint16(v-mod.Info.SDABase))
	case 251: // GHS_REL16_HA
		r := v - P
		mem.Write16(P, uint16((r+0x8000)>>16))
	case 252: // GHS_REL16_HI
		mem.Write16(P, uint16((v-P)>>16))
	case 253: // GHS_REL16_LO
		mem.Write16(P, uint16(v-P))
	default:
		return fmt.Errorf("unknown relocation type %d", typ)
	}
	return nil
}
