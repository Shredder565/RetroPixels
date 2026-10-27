package ps3

// Loading programs: the game's ELF (EBOOT.BIN, decrypted) and PRX modules.

import (
	"errors"
	"fmt"
)

type elfProg struct {
	typ, flags           uint32
	off, vaddr, paddr    uint64
	filesz, memsz, align uint64
}

type elfFile struct {
	data  []byte
	typ   uint16
	entry uint64
	progs []elfProg
}

func parseELF(b []byte) (*elfFile, error) {
	if len(b) < 0x40 || be.Uint32(b) != 0x7F454C46 || b[4] != 2 || b[5] != 2 {
		return nil, errors.New("not a 64-bit big-endian ELF")
	}
	f := &elfFile{data: b, typ: be.Uint16(b[0x10:]), entry: be.Uint64(b[0x18:])}
	phoff := be.Uint64(b[0x20:])
	phent := uint64(be.Uint16(b[0x36:]))
	phnum := int(be.Uint16(b[0x38:]))
	for i := 0; i < phnum; i++ {
		o := phoff + uint64(i)*phent
		if o+0x38 > uint64(len(b)) {
			return nil, errors.New("the program's header is cut short")
		}
		p := b[o:]
		f.progs = append(f.progs, elfProg{
			typ: be.Uint32(p), flags: be.Uint32(p[4:]),
			off: be.Uint64(p[8:]), vaddr: be.Uint64(p[16:]), paddr: be.Uint64(p[24:]),
			filesz: be.Uint64(p[32:]), memsz: be.Uint64(p[40:]), align: be.Uint64(p[48:]),
		})
	}
	return f, nil
}

func (f *elfFile) segData(p elfProg) []byte {
	if p.off+p.filesz > uint64(len(f.data)) {
		return nil
	}
	return f.data[p.off : p.off+p.filesz]
}

// loadExec loads the game's program and links its imports; it returns the
// entry point's function descriptor.
func (m *Machine) loadExec(b []byte) (uint32, error) {
	f, err := parseELF(b)
	if err != nil {
		return 0, err
	}
	var prxInfo uint32
	for _, p := range f.progs {
		switch p.typ {
		case 1: // PT_LOAD
			if p.memsz == 0 {
				continue
			}
			m.Mem.Map(uint32(p.vaddr), uint32(p.memsz))
			d := f.segData(p)
			if d == nil {
				return 0, errors.New("the program is cut short")
			}
			m.Mem.WriteBytes(uint32(p.vaddr), d)
			m.Mem.Fill(uint32(p.vaddr+p.filesz), 0, uint32(p.memsz-p.filesz))
			m.Logf("loaded %08X-%08X", p.vaddr, p.vaddr+p.memsz)
		case 7: // PT_TLS
			m.tlsImage, m.tlsFile, m.tlsMem = uint32(p.vaddr), uint32(p.filesz), uint32(p.memsz)
		}
	}
	for _, p := range f.progs {
		switch p.typ {
		case 0x60000001: // process parameters
			a := uint32(p.vaddr)
			if m.Mem.Read32(a+4) == 0x13bcc5f6 {
				m.proc.sdk = m.Mem.Read32(a + 12)
				m.proc.primaryPrio = int32(m.Mem.Read32(a + 16))
				m.proc.primaryStack = m.Mem.Read32(a + 20)
				m.proc.mallocPage = m.Mem.Read32(a + 24)
			}
		case 0x60000002: // PRX info: the import tables
			prxInfo = uint32(p.vaddr)
		}
	}
	m.Logf("SDK %x, main thread priority %d, stack %x, malloc page %x", m.proc.sdk, m.proc.primaryPrio, m.proc.primaryStack, m.proc.mallocPage)
	if prxInfo != 0 {
		stubStart := m.Mem.Read32(prxInfo + 24)
		stubEnd := m.Mem.Read32(prxInfo + 28)
		m.linkImports(stubStart, stubEnd)
	}
	return uint32(f.entry), nil
}

// linkImports points each imported function at RetroPixels' version.
func (m *Machine) linkImports(start, end uint32) {
	for a := start; a < end; {
		size := uint32(m.Mem.Read8(a))
		if size == 0 {
			break
		}
		nf := uint32(m.Mem.Read16(a + 6))
		nv := uint32(m.Mem.Read16(a + 8))
		mod := m.Mem.String(m.Mem.Read32(a+0x10), 64)
		fnids := m.Mem.Read32(a + 0x14)
		fstubs := m.Mem.Read32(a + 0x18)
		for i := uint32(0); i < nf; i++ {
			nid := m.Mem.Read32(fnids + 4*i)
			m.Mem.Write32(fstubs+4*i, m.hleEntryFor(mod, nid))
		}
		if nv != 0 {
			m.Logf("%s imports %d variables (not supported)", mod, nv)
		}
		a += size
	}
}

// prxModule is a loaded PRX.
type prxModule struct {
	id      uint32
	name    string
	base    uint32
	size    uint32
	toc     uint32
	start   uint32 // module_start OPD
	stop    uint32
	started bool
	path    string
	segs    []uint32 // each segment's address
}

// loadPRX loads a PRX module (decrypted) at a free address.
func (m *Machine) loadPRX(b []byte, path string) (*prxModule, error) {
	f, err := parseELF(b)
	if err != nil {
		return nil, err
	}
	if f.typ != 0xffa4 {
		return nil, fmt.Errorf("not a PRX module (type %x)", f.typ)
	}
	// lay the loadable segments out one after another
	var total uint64
	for _, p := range f.progs {
		if p.typ == 1 {
			total = (total+0xff)&^0xff + p.memsz
		}
	}
	base := m.mmap.alloc(uint32(total), 0x10000)
	if base == 0 {
		return nil, errors.New("no memory for the module")
	}
	mod := &prxModule{id: m.newID(), base: base, size: uint32(total), path: path}
	addr := uint64(base)
	var segOff []uint64 // file offset of each load segment
	for _, p := range f.progs {
		if p.typ != 1 {
			continue
		}
		addr = (addr + 0xff) &^ 0xff
		d := f.segData(p)
		m.Mem.WriteBytes(uint32(addr), d)
		m.Mem.Fill(uint32(addr+p.filesz), 0, uint32(p.memsz-p.filesz))
		mod.segs = append(mod.segs, uint32(addr))
		segOff = append(segOff, p.off)
		addr += p.memsz
	}
	// relocations
	for _, p := range f.progs {
		if p.typ != 0x700000a4 {
			continue
		}
		d := f.segData(p)
		for i := 0; i+24 <= len(d); i += 24 {
			r := d[i:]
			off := be.Uint64(r)
			iv := r[10]
			ia := r[11]
			typ := be.Uint32(r[12:])
			ptr := be.Uint64(r[16:])
			if int(ia) >= len(mod.segs) {
				return nil, errors.New("the module's relocations are damaged")
			}
			raddr := mod.segs[ia] + uint32(off)
			var data uint64
			if iv != 0xff {
				if int(iv) >= len(mod.segs) {
					return nil, errors.New("the module's relocations are damaged")
				}
				data = uint64(mod.segs[iv])
			}
			data += ptr
			switch typ {
			case 1:
				m.Mem.Write32(raddr, uint32(data))
			case 4:
				m.Mem.Write16(raddr, uint16(data))
			case 5:
				m.Mem.Write16(raddr, uint16(data>>16))
			case 6:
				m.Mem.Write16(raddr, uint16(data>>16)+uint16(data>>15&1))
			case 10:
				v := m.Mem.Read32(raddr)
				m.Mem.Write32(raddr, v&^0x03fffffc|uint32(data-uint64(raddr))&0x03fffffc)
			case 11:
				v := m.Mem.Read32(raddr)
				m.Mem.Write32(raddr, v&^0xfffc|uint32(data-uint64(raddr))&0xfffc)
			case 38:
				m.Mem.Write64(raddr, data)
			case 44:
				m.Mem.Write64(raddr, data-uint64(raddr))
			case 57:
				v := m.Mem.Read16(raddr)
				m.Mem.Write16(raddr, v&3|uint16(data)&^3)
			default:
				m.Logf("PRX relocation type %d not supported", typ)
			}
		}
	}
	// module info: at the first segment, offset p_paddr - p_offset
	p0 := f.progs[0]
	mi := mod.segs[0] + uint32(p0.paddr-p0.off)
	mod.name = m.Mem.String(mi+4, 28)
	mod.toc = m.Mem.Read32(mi + 32)
	expStart, expEnd := m.Mem.Read32(mi+36)+mod.segs[0], m.Mem.Read32(mi+40)+mod.segs[0]
	impStart, impEnd := m.Mem.Read32(mi+44)+mod.segs[0], m.Mem.Read32(mi+48)+mod.segs[0]
	_ = segOff
	// exports were relocated already? The table addresses in module info
	// are segment offsets; entries inside are relocated addresses.
	for a := expStart; a < expEnd; {
		size := uint32(m.Mem.Read8(a))
		if size == 0 {
			break
		}
		nf := uint32(m.Mem.Read16(a + 6))
		nv := uint32(m.Mem.Read16(a + 8))
		attr := m.Mem.Read16(a + 4)
		nids := m.Mem.Read32(a + 0x14)
		addrs := m.Mem.Read32(a + 0x18)
		for i := uint32(0); i < nf+nv; i++ {
			nid := m.Mem.Read32(nids + 4*i)
			fa := m.Mem.Read32(addrs + 4*i)
			if attr&0x8000 != 0 {
				switch nid {
				case 0xbc9a0086:
					mod.start = fa
				case 0xab779874:
					mod.stop = fa
				}
			}
		}
		a += size
	}
	m.linkImports(impStart, impEnd)
	m.prx = append(m.prx, mod)
	m.objects[mod.id] = mod
	m.Logf("loaded module %s at %08X (%x bytes), start %08X", mod.name, base, total, mod.start)
	return mod, nil
}
