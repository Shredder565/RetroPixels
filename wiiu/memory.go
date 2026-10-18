package wiiu

// Memory: the 32-bit address space a Wii U program sees.
//
// Cafe OS gives every program the same virtual layout, so RetroPixels keeps a
// flat 4 GB space of 64 KB pages, allocated the first time a mapped region is
// touched. Everything is big-endian, as on the console.
//
//	0x01000000  system area: RetroPixels' library entry points and OS data
//	0x02000000  the program's code (.text)
//	0x10000000  the program's data, then MEM2 (the main heap, 1 GB in all)
//	0xC0000000  the loader's area (import tables, file info)
//	0xE0000000  the foreground bucket (40 MB, shared with the system menu)
//	0xF4000000  MEM1 (32 MB of fast memory)

import (
	"encoding/binary"
	"fmt"
	"math"
)

const (
	pageBits = 16
	pageSize = 1 << pageBits
	pageMask = pageSize - 1
	numPages = 1 << (32 - pageBits)

	SystemBase = 0x01000000
	SystemSize = 0x01000000
	CodeBase   = 0x02000000
	DataBase   = 0x10000000
	MEM2End    = 0x50000000
	mem2Phys   = 0x50000000 // MEM2's physical address
	LoaderBase = 0xC0000000
	LoaderSize = 0x10000000
	FGBase     = 0xE0000000
	FGSize     = 0x02800000
	MEM1Base   = 0xF4000000
	MEM1Size   = 0x02000000
)

// region is a range of addresses that may hold memory.
type region struct {
	start, end uint32 // end is exclusive (0 = top of the space)
	name       string
}

// Memory is the address space.
type Memory struct {
	pages   [numPages][]byte
	code    [numPages]*codePage // decoded instructions (cpu_fast.go)
	jit     *JIT                // the recompiler, if it's on
	regions []region
	// Fault, when set, is called for an access outside any region; reads
	// then give 0 and writes are dropped.
	Fault func(addr uint32, write bool)
}

// NewMemory makes the standard Cafe OS layout.
func NewMemory() *Memory {
	m := &Memory{}
	m.Map(SystemBase, SystemSize, "system")
	m.Map(CodeBase, DataBase-CodeBase, "code")
	m.Map(DataBase, MEM2End-DataBase, "MEM2")
	m.Map(LoaderBase, LoaderSize, "loader")
	m.Map(FGBase, FGSize, "foreground")
	m.Map(MEM1Base, MEM1Size, "MEM1")
	return m
}

// Map makes addresses addr..addr+size-1 usable.
func (m *Memory) Map(addr, size uint32, name string) {
	m.regions = append(m.regions, region{addr, addr + size, name})
}

// Mapped reports whether addr is inside a region.
func (m *Memory) Mapped(addr uint32) bool {
	for _, r := range m.regions {
		if addr >= r.start && (r.end == 0 || addr < r.end) {
			return true
		}
	}
	return false
}

// page returns the page holding addr, making it if its region allows; nil
// when addr is unmapped.
func (m *Memory) page(addr uint32) []byte {
	p := m.pages[addr>>pageBits]
	if p != nil {
		return p
	}
	if !m.Mapped(addr) {
		// MEM2's physical addresses (0x50000000 up), which the GPU is given
		// in the command streams some games write themselves: the same
		// memory as MEM2 itself
		if addr >= mem2Phys && addr < mem2Phys+(MEM2End-DataBase) {
			if p = m.page(addr - mem2Phys + DataBase); p != nil {
				m.pages[addr>>pageBits] = p
				if m.jit != nil {
					m.jit.pageAdded(addr>>pageBits, p)
				}
			}
			return p
		}
		return nil
	}
	p = make([]byte, pageSize)
	m.pages[addr>>pageBits] = p
	if m.jit != nil {
		m.jit.pageAdded(addr>>pageBits, p)
	}
	return p
}

func (m *Memory) fault(addr uint32, write bool) {
	if m.Fault != nil {
		m.Fault(addr, write)
	}
}

func (m *Memory) Read8(addr uint32) uint8 {
	p := m.pages[addr>>pageBits]
	if p == nil {
		if p = m.page(addr); p == nil {
			m.fault(addr, false)
			return 0
		}
	}
	return p[addr&pageMask]
}

func (m *Memory) Write8(addr uint32, v uint8) {
	p := m.pages[addr>>pageBits]
	if p == nil {
		if p = m.page(addr); p == nil {
			m.fault(addr, true)
			return
		}
	}
	p[addr&pageMask] = v
}

func (m *Memory) Read16(addr uint32) uint16 {
	o := addr & pageMask
	if o <= pageSize-2 {
		p := m.pages[addr>>pageBits]
		if p == nil {
			if p = m.page(addr); p == nil {
				m.fault(addr, false)
				return 0
			}
		}
		return binary.BigEndian.Uint16(p[o:])
	}
	return uint16(m.Read8(addr))<<8 | uint16(m.Read8(addr+1))
}

func (m *Memory) Write16(addr uint32, v uint16) {
	o := addr & pageMask
	if o <= pageSize-2 {
		p := m.pages[addr>>pageBits]
		if p == nil {
			if p = m.page(addr); p == nil {
				m.fault(addr, true)
				return
			}
		}
		binary.BigEndian.PutUint16(p[o:], v)
		return
	}
	m.Write8(addr, uint8(v>>8))
	m.Write8(addr+1, uint8(v))
}

func (m *Memory) Read32(addr uint32) uint32 {
	o := addr & pageMask
	if o <= pageSize-4 {
		p := m.pages[addr>>pageBits]
		if p == nil {
			if p = m.page(addr); p == nil {
				m.fault(addr, false)
				return 0
			}
		}
		return binary.BigEndian.Uint32(p[o:])
	}
	return uint32(m.Read16(addr))<<16 | uint32(m.Read16(addr+2))
}

func (m *Memory) Write32(addr uint32, v uint32) {
	o := addr & pageMask
	if o <= pageSize-4 {
		p := m.pages[addr>>pageBits]
		if p == nil {
			if p = m.page(addr); p == nil {
				m.fault(addr, true)
				return
			}
		}
		binary.BigEndian.PutUint32(p[o:], v)
		return
	}
	m.Write16(addr, uint16(v>>16))
	m.Write16(addr+2, uint16(v))
}

func (m *Memory) Read64(addr uint32) uint64 {
	return uint64(m.Read32(addr))<<32 | uint64(m.Read32(addr+4))
}

func (m *Memory) Write64(addr uint32, v uint64) {
	m.Write32(addr, uint32(v>>32))
	m.Write32(addr+4, uint32(v))
}

func (m *Memory) ReadF32(addr uint32) float32 { return math.Float32frombits(m.Read32(addr)) }
func (m *Memory) WriteF32(addr uint32, v float32) {
	m.Write32(addr, math.Float32bits(v))
}

// Bytes copies n bytes starting at addr.
func (m *Memory) Bytes(addr uint32, n int) []byte {
	out := make([]byte, n)
	m.ReadBytes(addr, out)
	return out
}

// ReadBytes fills buf from addr.
func (m *Memory) ReadBytes(addr uint32, buf []byte) {
	for len(buf) > 0 {
		o := addr & pageMask
		n := pageSize - int(o)
		if n > len(buf) {
			n = len(buf)
		}
		p := m.page(addr)
		if p == nil {
			m.fault(addr, false)
			for i := range buf[:n] {
				buf[i] = 0
			}
		} else {
			copy(buf[:n], p[o:])
		}
		buf = buf[n:]
		addr += uint32(n)
	}
}

// WriteBytes copies data to addr.
func (m *Memory) WriteBytes(addr uint32, data []byte) {
	for len(data) > 0 {
		o := addr & pageMask
		n := pageSize - int(o)
		if n > len(data) {
			n = len(data)
		}
		p := m.page(addr)
		if p == nil {
			m.fault(addr, true)
		} else {
			copy(p[o:], data[:n])
		}
		data = data[n:]
		addr += uint32(n)
	}
}

// Fill sets n bytes at addr to v.
func (m *Memory) Fill(addr uint32, v byte, n uint32) {
	for n > 0 {
		o := addr & pageMask
		k := uint32(pageSize) - o
		if k > n {
			k = n
		}
		p := m.page(addr)
		if p == nil {
			m.fault(addr, true)
		} else {
			s := p[o : o+k]
			for i := range s {
				s[i] = v
			}
		}
		n -= k
		addr += k
	}
}

// Move copies n bytes from src to dst inside the space (overlap allowed).
func (m *Memory) Move(dst, src, n uint32) {
	if n == 0 || dst == src {
		return
	}
	buf := m.Bytes(src, int(n))
	m.WriteBytes(dst, buf)
}

// String reads a zero-ended string at addr (at most max bytes).
func (m *Memory) String(addr uint32, max int) string {
	if addr == 0 {
		return ""
	}
	var b []byte
	for i := 0; i < max; i++ {
		c := m.Read8(addr + uint32(i))
		if c == 0 {
			break
		}
		b = append(b, c)
	}
	return string(b)
}

// WriteString stores s and a zero byte at addr.
func (m *Memory) WriteString(addr uint32, s string) {
	m.WriteBytes(addr, append([]byte(s), 0))
}

// Slice returns the memory at addr..addr+n-1 directly when it lies in one
// page (for fast bulk access), or nil.
func (m *Memory) Slice(addr uint32, n int) []byte {
	o := int(addr & pageMask)
	if o+n > pageSize {
		return nil
	}
	p := m.page(addr)
	if p == nil {
		return nil
	}
	return p[o : o+n]
}

func (m *Memory) regionName(addr uint32) string {
	for _, r := range m.regions {
		if addr >= r.start && (r.end == 0 || addr < r.end) {
			return r.name
		}
	}
	return fmt.Sprintf("unmapped %08X", addr)
}
