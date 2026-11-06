// Package ctr is the Nintendo 3DS side of PixelDS, RetroPixels' own DS and
// 3DS emulator: the ARM11 application processor with a high-level kernel
// and system services written in Go (no 3DS firmware is needed), the
// screens, buttons and touch screen. Everything here is new code written
// for RetroPixels from public hardware and system documentation.
package ctr

import (
	"encoding/binary"
	"fmt"
)

const (
	pageBits = 12
	pageSize = 1 << pageBits

	fcramBase = 0x20000000
	fcramSize = 128 << 20
	vramBase  = 0x18000000
	vramSize  = 6 << 20
	dspBase   = 0x1ff00000
	dspSize   = 512 << 10

	// the application's share of FCRAM (old 3DS, 64 MB mode)
	appMemSize = 64 << 20

	linearBase = 0x14000000 // the linear heap: FCRAM mapped one to one
	heapBase   = 0x08000000
	codeBase   = 0x00100000
	stackTop   = 0x10000000
	tlsBase    = 0x1ff82000
	configMem  = 0x1ff80000
	sharedPage = 0x1ff81000
	vramVBase  = 0x1f000000
	dspVBase   = 0x1ff00000
)

// memory is physical memory (FCRAM, VRAM, DSP RAM) and the application's
// virtual address space, kept as a table of 4 KB pages.
type memory struct {
	fcram, vram, dsp []byte
	pages            [1 << 20][]byte `snap:"-"` // virtual page → its 4 KB of physical memory (rebuilt from phys)
	phys             [1 << 20]uint32 // virtual page → physical address (0: none)
	used             []bool          // FCRAM pages in use (application region)
	linearNext       uint32          // next free linear heap offset
	fault            func(a uint32, write bool)
}

func newMemory() *memory {
	m := &memory{
		fcram: make([]byte, fcramSize),
		vram:  make([]byte, vramSize),
		dsp:   make([]byte, dspSize),
		used:  make([]bool, appMemSize/pageSize),
	}
	return m
}

// physSlice returns the memory at physical address pa, n bytes long (nil
// when that isn't RAM).
func (m *memory) physSlice(pa, n uint32) []byte {
	switch {
	case pa >= fcramBase && pa+n <= fcramBase+fcramSize:
		return m.fcram[pa-fcramBase : pa-fcramBase+n]
	case pa >= vramBase && pa+n <= vramBase+vramSize:
		return m.vram[pa-vramBase : pa-vramBase+n]
	case pa >= dspBase && pa+n <= dspBase+dspSize:
		return m.dsp[pa-dspBase : pa-dspBase+n]
	}
	return nil
}

// mapPhys maps size bytes of physical memory at pa to virtual address va.
func (m *memory) mapPhys(va, pa, size uint32) error {
	for off := uint32(0); off < size; off += pageSize {
		p := m.physSlice(pa+off, pageSize)
		if p == nil {
			return fmt.Errorf("no RAM at %08x", pa+off)
		}
		m.pages[(va+off)>>pageBits] = p
		m.phys[(va+off)>>pageBits] = pa + off
	}
	return nil
}

func (m *memory) unmap(va, size uint32) {
	for off := uint32(0); off < size; off += pageSize {
		m.pages[(va+off)>>pageBits] = nil
		m.phys[(va+off)>>pageBits] = 0
	}
}

func (m *memory) mapped(va, size uint32) bool {
	for off := uint32(0); off < size; off += pageSize {
		if m.pages[(va+off)>>pageBits] == nil {
			return false
		}
	}
	return true
}

// virtToPhys translates a virtual address (0 when unmapped).
func (m *memory) virtToPhys(va uint32) uint32 {
	p := m.phys[va>>pageBits]
	if p == 0 {
		return 0
	}
	return p + va&(pageSize-1)
}

// allocPages takes n free FCRAM pages for ordinary memory (from the top of
// the application region, leaving the bottom to the linear heap); it
// returns their physical addresses.
func (m *memory) allocPages(n uint32) ([]uint32, error) {
	var out []uint32
	for i := len(m.used) - 1; i >= 0 && uint32(len(out)) < n; i-- {
		if !m.used[i] && uint32(i)*pageSize >= m.linearNext {
			out = append(out, fcramBase+uint32(i)*pageSize)
		}
	}
	if uint32(len(out)) < n {
		return nil, fmt.Errorf("out of memory")
	}
	for _, pa := range out {
		m.used[(pa-fcramBase)/pageSize] = true
		clear(m.physSlice(pa, pageSize))
	}
	return out, nil
}

func (m *memory) freePage(pa uint32) {
	if pa >= fcramBase && pa < fcramBase+appMemSize {
		m.used[(pa-fcramBase)/pageSize] = false
	}
}

// allocLinear takes size bytes of the linear heap: physically contiguous
// FCRAM from the bottom of the application region. It returns the
// physical address.
func (m *memory) allocLinear(size uint32) (uint32, error) {
	off := m.linearNext
	for i := off / pageSize; i < (off+size)/pageSize; i++ {
		if int(i) >= len(m.used) || m.used[i] {
			return 0, fmt.Errorf("out of linear memory")
		}
	}
	for i := off / pageSize; i < (off+size)/pageSize; i++ {
		m.used[i] = true
	}
	m.linearNext = off + size
	clear(m.physSlice(fcramBase+off, size))
	return fcramBase + off, nil
}

// usedBytes is the application memory in use.
func (m *memory) usedBytes() uint32 {
	var n uint32
	for _, u := range m.used {
		if u {
			n += pageSize
		}
	}
	return n
}

// ---------------- the CPU's bus ----------------

func (m *memory) page(a uint32, write bool) []byte {
	p := m.pages[a>>pageBits]
	if p == nil && m.fault != nil {
		m.fault(a, write)
	}
	return p
}

func (m *memory) Read8(a uint32) uint32 {
	if p := m.page(a, false); p != nil {
		return uint32(p[a&(pageSize-1)])
	}
	return 0
}

func (m *memory) Read16(a uint32) uint32 {
	o := a & (pageSize - 1)
	if o <= pageSize-2 {
		if p := m.page(a, false); p != nil {
			return uint32(binary.LittleEndian.Uint16(p[o:]))
		}
		return 0
	}
	return m.Read8(a) | m.Read8(a+1)<<8
}

func (m *memory) Read32(a uint32) uint32 {
	o := a & (pageSize - 1)
	if o <= pageSize-4 {
		if p := m.page(a, false); p != nil {
			return binary.LittleEndian.Uint32(p[o:])
		}
		return 0
	}
	return m.Read16(a) | m.Read16(a+2)<<16
}

func (m *memory) Write8(a, v uint32) {
	if p := m.page(a, true); p != nil {
		p[a&(pageSize-1)] = byte(v)
	}
}

func (m *memory) Write16(a, v uint32) {
	o := a & (pageSize - 1)
	if o <= pageSize-2 {
		if p := m.page(a, true); p != nil {
			binary.LittleEndian.PutUint16(p[o:], uint16(v))
		}
		return
	}
	m.Write8(a, v)
	m.Write8(a+1, v>>8)
}

func (m *memory) Write32(a, v uint32) {
	o := a & (pageSize - 1)
	if o <= pageSize-4 {
		if p := m.page(a, true); p != nil {
			binary.LittleEndian.PutUint32(p[o:], v)
		}
		return
	}
	m.Write16(a, v)
	m.Write16(a+2, v>>16)
}

func (m *memory) Fetch16(a uint32) uint32 { return m.Read16(a) }
func (m *memory) Fetch32(a uint32) uint32 { return m.Read32(a) }

// read and write blocks of guest memory (for services)

func (m *memory) readBytes(a, n uint32) []byte {
	out := make([]byte, n)
	for i := uint32(0); i < n; i++ {
		out[i] = byte(m.Read8(a + i))
	}
	return out
}

func (m *memory) writeBytes(a uint32, b []byte) {
	for i, x := range b {
		m.Write8(a+uint32(i), uint32(x))
	}
}

func (m *memory) readString(a, max uint32) string {
	var b []byte
	for i := uint32(0); i < max; i++ {
		c := byte(m.Read8(a + i))
		if c == 0 {
			break
		}
		b = append(b, c)
	}
	return string(b)
}
