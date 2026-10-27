package ps3

// Memory: the 32-bit address space a PS3 game sees. Everything is
// big-endian, as on the console. The space is 64 KB pages, made the first
// time a mapped range is touched.
//
//	0x00010000  the game's program (EBOOT.BIN) and its data
//	0x10000000  RetroPixels' own area: system function entry points, TLS, HLE data
//	0x20000000  PRX modules the game loads (TMNT_r.sprx)
//	0x30000000  user memory (sys_memory_allocate), 256 MB
//	0x40000000  more user memory (sys_mmapper, cellGcmMapMainMemory), 256 MB
//	0xC0000000  the RSX's local memory (256 MB of video memory)
//	0xD0000000  PPU thread stacks
//	0xE0000000  SPU local stores, as the PPU sees them

import (
	"encoding/binary"
	"math"
	"unsafe"
)

const (
	pageBits = 16
	pageSize = 1 << pageBits
	pageMask = pageSize - 1
	numPages = 1 << (32 - pageBits)

	SysBase    = 0x10000000
	SysSize    = 0x01000000
	PRXBase    = 0x20000000
	UserBase   = 0x30000000
	UserSize   = 0x10000000
	MMapBase   = 0x40000000
	MMapSize   = 0x10000000
	RSXBase    = 0xC0000000
	RSXSize    = 0x10000000
	StackBase  = 0xD0000000
	StackSize  = 0x10000000
	SPUBase    = 0xE0000000
	SPUStride  = 0x00100000
	SPUMaxArea = 0x01000000
)

type region struct {
	start, end uint64
}

// Memory is the address space.
type Memory struct {
	pages  [numPages][]byte
	mapped [numPages]bool
	// Fault is called for an access to an address nothing is mapped at.
	Fault func(addr uint32, write bool)
	// Write hooks: a page with a hook calls it before each write (code
	// pages the PPU has decoded, RSX control registers).
	hooked [numPages]bool
	Hook   func(addr uint32, n int)

	// For the recompiler: each page's host address (0 when it has no
	// memory yet), and the same for writes (0 also when writes are hooked).
	hostR, hostW [numPages]uintptr
	// code: pages the recompiler has compiled code from; codeWrite is
	// called on a write to one.
	watch, code [numPages]bool
	codeWrite   func(addr uint32, n int)
}

func NewMemory() *Memory {
	return &Memory{}
}

// Map makes addr..addr+size-1 usable (64 KB granularity).
func (m *Memory) Map(addr, size uint32) {
	for p := uint64(addr) >> pageBits; p < (uint64(addr)+uint64(size)+pageMask)>>pageBits; p++ {
		m.mapped[p] = true
	}
}

// Unmap removes addr..addr+size-1 (and drops its contents).
func (m *Memory) Unmap(addr, size uint32) {
	for p := uint64(addr) >> pageBits; p < (uint64(addr)+uint64(size)+pageMask)>>pageBits; p++ {
		if m.code[p] && m.codeWrite != nil {
			m.codeWrite(uint32(p<<pageBits), pageSize)
		}
		m.mapped[p] = false
		m.pages[p] = nil
		m.setHost(uint32(p))
	}
}

// setHost refreshes page i's entries in the recompiler's tables.
func (m *Memory) setHost(i uint32) {
	m.hooked[i] = m.watch[i] || m.code[i]
	if p := m.pages[i]; p != nil {
		m.hostR[i] = uintptr(unsafe.Pointer(&p[0]))
	} else {
		m.hostR[i] = 0
	}
	m.hostW[i] = m.hostR[i]
	if m.hooked[i] {
		m.hostW[i] = 0
	}
}

// Watch makes writes to page i call Hook.
func (m *Memory) Watch(i uint32) {
	m.watch[i] = true
	m.setHost(i)
}

// setCode marks page i as holding compiled code (or not).
func (m *Memory) setCode(i uint32, v bool) {
	m.code[i] = v
	m.setHost(i)
}

// hookWrite runs the write hooks for addr..addr+n-1 (on one page).
func (m *Memory) hookWrite(addr uint32, n int) {
	i := addr >> pageBits
	if m.code[i] && m.codeWrite != nil {
		m.codeWrite(addr, n)
	}
	if m.watch[i] && m.Hook != nil {
		m.Hook(addr, n)
	}
}

// Alias makes the pages at dst the same memory as those at src.
func (m *Memory) Alias(dst, src, size uint32) {
	for i := uint32(0); i < size; i += pageSize {
		sp := m.page(src + i)
		m.pages[(dst+i)>>pageBits] = sp
		m.mapped[(dst+i)>>pageBits] = true
		m.setHost((dst + i) >> pageBits)
	}
}

func (m *Memory) Mapped(addr uint32) bool { return m.mapped[addr>>pageBits] }

func (m *Memory) page(addr uint32) []byte {
	i := addr >> pageBits
	if p := m.pages[i]; p != nil {
		return p
	}
	if !m.mapped[i] {
		return nil
	}
	p := make([]byte, pageSize)
	m.pages[i] = p
	m.setHost(i)
	return p
}

func (m *Memory) fault(addr uint32, write bool) {
	if m.Fault != nil {
		m.Fault(addr, write)
	}
}

// Ptr returns the bytes at addr up to the end of its page (nil if unmapped).
func (m *Memory) Ptr(addr uint32) []byte {
	p := m.page(addr)
	if p == nil {
		return nil
	}
	return p[addr&pageMask:]
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
	if m.hooked[addr>>pageBits] {
		m.hookWrite(addr, 1)
	}
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
		if m.hooked[addr>>pageBits] {
			m.hookWrite(addr, 2)
		}
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
		if m.hooked[addr>>pageBits] {
			m.hookWrite(addr, 4)
		}
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
	o := addr & pageMask
	if o <= pageSize-8 {
		p := m.pages[addr>>pageBits]
		if p == nil {
			if p = m.page(addr); p == nil {
				m.fault(addr, false)
				return 0
			}
		}
		return binary.BigEndian.Uint64(p[o:])
	}
	return uint64(m.Read32(addr))<<32 | uint64(m.Read32(addr+4))
}

func (m *Memory) Write64(addr uint32, v uint64) {
	o := addr & pageMask
	if o <= pageSize-8 {
		if m.hooked[addr>>pageBits] {
			m.hookWrite(addr, 8)
		}
		p := m.pages[addr>>pageBits]
		if p == nil {
			if p = m.page(addr); p == nil {
				m.fault(addr, true)
				return
			}
		}
		binary.BigEndian.PutUint64(p[o:], v)
		return
	}
	m.Write32(addr, uint32(v>>32))
	m.Write32(addr+4, uint32(v))
}

func (m *Memory) ReadF32(addr uint32) float32 { return math.Float32frombits(m.Read32(addr)) }
func (m *Memory) ReadF64(addr uint32) float64 { return math.Float64frombits(m.Read64(addr)) }

// ReadBytes copies n bytes at addr.
func (m *Memory) ReadBytes(addr uint32, b []byte) {
	for len(b) > 0 {
		p := m.page(addr)
		o := addr & pageMask
		n := min(len(b), pageSize-int(o))
		if p == nil {
			m.fault(addr, false)
			clear(b[:n])
		} else {
			copy(b[:n], p[o:])
		}
		b = b[n:]
		addr += uint32(n)
	}
}

// WriteBytes copies b to addr.
func (m *Memory) WriteBytes(addr uint32, b []byte) {
	for len(b) > 0 {
		o := addr & pageMask
		n := min(len(b), pageSize-int(o))
		if m.hooked[addr>>pageBits] {
			m.hookWrite(addr, n)
		}
		p := m.page(addr)
		if p == nil {
			m.fault(addr, true)
		} else {
			copy(p[o:], b[:n])
		}
		b = b[n:]
		addr += uint32(n)
	}
}

// Fill sets n bytes at addr to v.
func (m *Memory) Fill(addr uint32, v byte, n uint32) {
	for n > 0 {
		o := addr & pageMask
		c := min(n, pageSize-o)
		if m.hooked[addr>>pageBits] {
			m.hookWrite(addr, int(c))
		}
		p := m.page(addr)
		if p != nil {
			s := p[o : o+c]
			for i := range s {
				s[i] = v
			}
		}
		n -= c
		addr += c
	}
}

// String reads a NUL-terminated string (at most max bytes).
func (m *Memory) String(addr uint32, max int) string {
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

// WriteString writes s and a NUL.
func (m *Memory) WriteString(addr uint32, s string) {
	m.WriteBytes(addr, append([]byte(s), 0))
}
