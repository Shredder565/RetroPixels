// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/
// coreinit_MEM_ExpHeap.cpp and coreinit_MEM_ExpHeap.h, rewritten in Go for
// RetroPixels.

package wiiu

// Expanded heaps: malloc-like heaps that allocate from the front or (with a
// negative alignment) the back. Every block, free or used, starts with a
// 0x14-byte header; the heap keeps a chain of free blocks in address order
// and a chain of used blocks in the order they were allocated.

const (
	// MEMExpHeap, after MEMHeapBase
	expFreeChain = 0x40 // { head, tail }
	expUsedChain = 0x48
	expGroupID   = 0x50 // u16
	expFields    = 0x52 // u16: bit 0 alloc mode (0 first fit, 1 nearest size), bit 1 keep alignment gaps as free blocks
	expHeapSize  = 0x54

	expModeNear    = 1
	expAlignMargin = 2

	// a block's header
	mbFields = 0x00 // bit 31 allocated from the tail, bits 8..30 alignment gap before the header, bits 0..7 group
	mbSize   = 0x04 // of the data after the header
	mbPrev   = 0x08
	mbNext   = 0x0c
	mbType   = 0x10 // u16
	mbHeader = 0x14

	mbFree = 0x4652 // "FR"
	mbUsed = 0x5544 // "UD"

	chainHead = 0
	chainTail = 4
)

func (m *Machine) mbInit(start, end uint32, typ uint16) uint32 {
	m.Mem.Fill(start, 0, mbHeader)
	m.Mem.Write16(start+mbType, typ)
	m.Mem.Write32(start+mbSize, end-(start+mbHeader))
	return start
}

func (m *Machine) mbEnd(b uint32) uint32 { return b + mbHeader + m.Mem.Read32(b+mbSize) }

// mbRegion is all the memory a block takes, its alignment gap included.
func (m *Machine) mbRegion(b uint32) (start, end uint32) {
	gap := m.Mem.Read32(b+mbFields) >> 8 & 0x7fffff
	return b - gap, m.mbEnd(b)
}

// mbRemove takes b out of a chain and returns the block before it.
func (m *Machine) mbRemove(chain, b uint32) uint32 {
	mem := m.Mem
	prev, next := mem.Read32(b+mbPrev), mem.Read32(b+mbNext)
	if prev != 0 {
		mem.Write32(prev+mbNext, next)
	} else {
		mem.Write32(chain+chainHead, next)
	}
	if next != 0 {
		mem.Write32(next+mbPrev, prev)
	} else {
		mem.Write32(chain+chainTail, prev)
	}
	return prev
}

// mbInsert puts b into a chain after prev (first if prev is 0).
func (m *Machine) mbInsert(chain, b, prev uint32) uint32 {
	mem := m.Mem
	mem.Write32(b+mbPrev, prev)
	var next uint32
	if prev != 0 {
		next = mem.Read32(prev + mbNext)
		mem.Write32(prev+mbNext, b)
	} else {
		next = mem.Read32(chain + chainHead)
		mem.Write32(chain+chainHead, b)
	}
	mem.Write32(b+mbNext, next)
	if next != 0 {
		mem.Write32(next+mbPrev, b)
	} else {
		mem.Write32(chain+chainTail, b)
	}
	return b
}

// expRecycle makes start..end a free block again, merged with the free
// blocks on either side.
func (m *Machine) expRecycle(heap, start, end uint32) bool {
	mem := m.Mem
	chain := heap + expFreeChain
	fill := m.heapFlagsOf(heap)&heapOptFill != 0
	newStart, newEnd := start, end
	var prev uint32
	for b := mem.Read32(chain + chainHead); b != 0; b = mem.Read32(b + mbNext) {
		if b >= start {
			if b == end {
				newEnd = m.mbEnd(b)
				m.mbRemove(chain, b)
				if fill {
					m.heapFill(b, mbHeader, fillOnFree)
				}
			}
			break
		}
		prev = b
	}
	if prev != 0 && m.mbEnd(prev) == start {
		newStart = prev
		prev = m.mbRemove(chain, prev)
	}
	if newEnd-newStart < mbHeader {
		return false
	}
	if fill {
		m.heapFill(start, end-start, fillOnFree)
	}
	m.mbInsert(chain, m.mbInit(newStart, newEnd, mbFree), prev)
	return true
}

// expTake allocates size bytes at at from free block fb.
func (m *Machine) expTake(heap, fb, at, size uint32, fromTail bool) uint32 {
	mem := m.Mem
	chain := heap + expFreeChain
	fields := uint32(mem.Read16(heap + expFields))
	flags := m.heapFlagsOf(heap)

	freeStart, _ := m.mbRegion(fb)
	freeEnd := at - mbHeader
	restStart, restEnd := at+size, m.mbEnd(fb)

	prev := m.mbRemove(chain, fb)
	if freeEnd-freeStart >= 0x18 && (fromTail || fields&expAlignMargin != 0) {
		prev = m.mbInsert(chain, m.mbInit(freeStart, freeEnd, mbFree), prev)
	} else {
		freeEnd = freeStart
	}
	if restEnd-restStart >= 0x18 && (!fromTail || fields&expAlignMargin != 0) {
		m.mbInsert(chain, m.mbInit(restStart, restEnd, mbFree), prev)
	} else {
		restStart = restEnd
	}

	if flags&heapOptClear != 0 {
		mem.Fill(freeEnd, 0, restStart-freeEnd)
	} else if flags&heapOptFill != 0 {
		m.heapFill(freeEnd, restStart-freeEnd, fillOnAlloc)
	}

	b := m.mbInit(at-mbHeader, restStart, mbUsed)
	f := (b-freeEnd)&0x7fffff<<8 | uint32(mem.Read16(heap+expGroupID))&0xff
	if fromTail {
		f |= 1 << 31
	}
	mem.Write32(b+mbFields, f)
	m.mbInsert(heap+expUsedChain, b, mem.Read32(heap+expUsedChain+chainTail))
	return at
}

func (m *Machine) expAllocFromTail(heap, size, align uint32) uint32 {
	mem := m.Mem
	first := mem.Read16(heap+expFields)&expModeNear == 0
	var found, at uint32
	best := uint64(0xffffffff)
	for b := mem.Read32(heap + expFreeChain + chainTail); b != 0; b = mem.Read32(b + mbPrev) {
		data := uint64(b + mbHeader)
		n := uint64(mem.Read32(b + mbSize))
		if data+n < uint64(size) {
			continue
		}
		a := (data + n - uint64(size)) &^ uint64(align-1)
		if a < data || best <= n {
			continue
		}
		found, at, best = b, uint32(a), n
		if first || best == uint64(size) {
			break
		}
	}
	if found == 0 {
		return 0
	}
	return m.expTake(heap, found, at, size, true)
}

func (m *Machine) expAllocFromHead(heap, size, align uint32) uint32 {
	mem := m.Mem
	first := mem.Read16(heap+expFields)&expModeNear == 0
	var found, at uint32
	best := uint64(0xffffffff)
	for b := mem.Read32(heap + expFreeChain + chainHead); b != 0; b = mem.Read32(b + mbNext) {
		data := uint64(b + mbHeader)
		a := (data + uint64(align-1)) &^ uint64(align-1)
		n := uint64(mem.Read32(b + mbSize))
		if n < a-data+uint64(size) || best <= n {
			continue
		}
		found, at, best = b, uint32(a), n
		if first || best == uint64(size) {
			break
		}
	}
	if found == 0 {
		return 0
	}
	return m.expTake(heap, found, at, size, false)
}

func (m *Machine) isExpHeap(heap uint32) bool {
	return heap != 0 && m.Mem.Read32(heap+heapMagic) == heapMagicExp
}

// createExpHeap is MEMCreateExpHeapEx.
func (m *Machine) createExpHeap(start, size, flags uint32) uint32 {
	if start == 0 {
		return 0
	}
	lo := (uint64(start) + 3) &^ 3
	hi := (uint64(start) + uint64(size)) &^ 3
	if lo > hi || hi-lo < 0x6c {
		return 0
	}
	heap, end := uint32(lo), uint32(hi)
	m.memInitHeapBase(heap, heapMagicExp, heap+expHeapSize, end, flags)
	mem := m.Mem
	mem.Fill(heap+heapBaseSize, 0, expHeapSize-heapBaseSize)
	b := m.mbInit(heap+expHeapSize, end, mbFree)
	mem.Write32(heap+expFreeChain+chainHead, b)
	mem.Write32(heap+expFreeChain+chainTail, b)
	return heap
}

// expAlloc is MEMAllocFromExpHeapEx: a negative alignment allocates from the
// back.
func (m *Machine) expAlloc(heap, size uint32, align int32) uint32 {
	if !m.isExpHeap(heap) {
		if heap != 0 {
			return m.heapAlloc(heap, size, align)
		}
		return 0
	}
	if align == 0 || size > 0xfffffffc {
		return 0
	}
	if size == 0 {
		size = 1
	}
	size = (size + 3) &^ 3
	if align < 0 {
		return m.expAllocFromTail(heap, size, uint32(-align))
	}
	return m.expAllocFromHead(heap, size, uint32(align))
}

// expUsed reports whether p is a block allocated from heap.
func (m *Machine) expUsed(heap, p uint32) bool {
	mem := m.Mem
	if p < mem.Read32(heap+heapStart)+mbHeader || p >= mem.Read32(heap+heapEnd) {
		return false
	}
	return mem.Read16(p-mbHeader+mbType) == mbUsed
}

// expFree is MEMFreeToExpHeap.
func (m *Machine) expFree(heap, p uint32) {
	if p == 0 || !m.isExpHeap(heap) || !m.expUsed(heap, p) {
		return
	}
	b := p - mbHeader
	start, end := m.mbRegion(b)
	m.mbRemove(heap+expUsedChain, b)
	m.expRecycle(heap, start, end)
}

// expResize is MEMResizeForMBlockExpHeap: the block's new size, or 0.
func (m *Machine) expResize(heap, p, size uint32) uint32 {
	mem := m.Mem
	if !m.isExpHeap(heap) || !m.expUsed(heap, p) {
		return 0
	}
	b := p - mbHeader
	size = (size + 3) &^ 3
	old := mem.Read32(b + mbSize)
	if size == old {
		return old
	}
	flags := m.heapFlagsOf(heap)
	chain := heap + expFreeChain
	if size < old {
		end := m.mbEnd(b)
		mem.Write32(b+mbSize, size)
		if !m.expRecycle(heap, p+size, end) {
			mem.Write32(b+mbSize, old)
		}
		return mem.Read32(b + mbSize)
	}
	end := m.mbEnd(b)
	for fb := mem.Read32(chain + chainHead); fb != 0; fb = mem.Read32(fb + mbNext) {
		if fb != end {
			continue
		}
		if uint64(size) > uint64(old)+uint64(mem.Read32(fb+mbSize))+mbHeader {
			return 0
		}
		oldStart, rEnd := m.mbRegion(fb)
		prev := m.mbRemove(chain, fb)
		rStart := p + size
		if rEnd-rStart < mbHeader {
			rStart = rEnd
		}
		mem.Write32(b+mbSize, rStart-p)
		if rEnd-rStart >= mbHeader {
			m.mbInsert(chain, m.mbInit(rStart, rEnd, mbFree), prev)
		}
		if flags&heapOptClear != 0 {
			mem.Fill(oldStart, 0, rStart-oldStart)
		} else if flags&heapOptFill != 0 {
			m.heapFill(oldStart, rStart-oldStart, fillOnAlloc)
		}
		return mem.Read32(b + mbSize)
	}
	return 0
}

func (m *Machine) expTotalFree(heap uint32) uint32 {
	var n uint32
	for b := m.Mem.Read32(heap + expFreeChain + chainHead); b != 0; b = m.Mem.Read32(b + mbNext) {
		n += m.Mem.Read32(b + mbSize)
	}
	return n
}

// expAllocatable is MEMGetAllocatableSizeForExpHeapEx: the largest block
// that could be allocated with this alignment.
func (m *Machine) expAllocatable(heap uint32, align int32) uint32 {
	if align < 0 {
		align = -align
	}
	al := uint64(max(align, 1))
	var result uint32
	smallestGap := uint32(0xffffffff)
	for b := m.Mem.Read32(heap + expFreeChain + chainHead); b != 0; b = m.Mem.Read32(b + mbNext) {
		data, end := uint64(b+mbHeader), uint64(m.mbEnd(b))
		a := (data + al - 1) &^ (al - 1)
		if a >= end {
			continue
		}
		size, gap := uint32(end-a), uint32(a-data)
		if size < result || size == result && smallestGap <= gap {
			continue
		}
		smallestGap, result = gap, size
	}
	return result
}

// expAdjust is MEMAdjustExpHeap: it gives back the free memory at the end.
func (m *Machine) expAdjust(heap uint32) uint32 {
	mem := m.Mem
	tail := mem.Read32(heap + expFreeChain + chainTail)
	if tail == 0 || m.mbEnd(tail) != mem.Read32(heap+heapEnd) {
		return 0
	}
	m.mbRemove(heap+expFreeChain, tail)
	end := mem.Read32(heap+heapEnd) - (mbHeader + mem.Read32(tail+mbSize))
	mem.Write32(heap+heapEnd, end)
	return end - heap
}

func init() {
	c := "coreinit"
	expHeap := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			if h := arg(cpu, 0); m.isExpHeap(h) {
				fn(m, cpu, h)
				return
			}
			ret(cpu, 0)
		}
	}

	reg(c, "MEMCreateExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createExpHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)&0xffff))
	})
	reg(c, "MEMDestroyExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		ret(cpu, heap)
	}))
	reg(c, "MEMAllocFromExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToExpHeap", func(m *Machine, cpu *CPU, t *Thread) { m.expFree(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMSetAllocModeForExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		f := m.Mem.Read16(heap + expFields)
		ret(cpu, uint32(f&1))
		m.Mem.Write16(heap+expFields, f&^1|uint16(arg(cpu, 1))&1)
	}))
	reg(c, "MEMGetAllocModeForExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, uint32(m.Mem.Read16(heap+expFields)&1))
	}))
	reg(c, "MEMAdjustExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.expAdjust(heap)) }))
	reg(c, "MEMResizeForMBlockExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.expResize(heap, arg(cpu, 1), arg(cpu, 2)))
	}))
	reg(c, "MEMGetTotalFreeSizeForExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.expTotalFree(heap)) }))
	reg(c, "MEMGetAllocatableSizeForExpHeapEx", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.expAllocatable(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMSetGroupIDForExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, uint32(m.Mem.Read16(heap+expGroupID)))
		m.Mem.Write16(heap+expGroupID, uint16(arg(cpu, 1)))
	}))
	reg(c, "MEMGetGroupIDForExpHeap", expHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, uint32(m.Mem.Read16(heap+expGroupID)))
	}))
	// MEMVisitAllocatedForExpHeap(heap, visitor, param) calls
	// visitor(block, heap, param) for each allocated block
	reg(c, "MEMVisitAllocatedForExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		heap, visitor, param := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if !m.isExpHeap(heap) || visitor == 0 {
			return
		}
		var visit func(m *Machine, cpu *CPU, t *Thread, b uint32)
		visit = func(m *Machine, cpu *CPU, t *Thread, b uint32) {
			if b == 0 {
				ret(cpu, 0)
				return
			}
			m.callGuest(cpu, t, visitor, []uint32{b + mbHeader, heap, param}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
				visit(m, cpu, t, m.Mem.Read32(b+mbNext))
			})
		}
		visit(m, cpu, t, m.Mem.Read32(heap+expUsedChain+chainHead))
	})
	reg(c, "MEMGetSizeForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			ret(cpu, m.Mem.Read32(p-mbHeader+mbSize))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMGetGroupIDForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)-mbHeader+mbFields)&0xff)
	})
	reg(c, "MEMGetAllocDirForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)-mbHeader+mbFields)>>31)
	})
	reg(c, "MEMCheckExpHeap", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.isExpHeap(arg(cpu, 0))) })
	reg(c, "MEMCheckForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })

	reg(c, "MEMInitAllocatorForExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Exp", arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "__RPAllocatorExpAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		ret(cpu, m.expAlloc(m.Mem.Read32(a+allocatorHeap), arg(cpu, 1), int32(m.Mem.Read32(a+allocatorParam))))
	})
	reg(c, "__RPAllocatorExpFree", func(m *Machine, cpu *CPU, t *Thread) {
		m.expFree(m.Mem.Read32(arg(cpu, 0)+allocatorHeap), arg(cpu, 1))
	})
}
