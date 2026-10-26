// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// Expanded heaps (MEMExpHeap): general-purpose heaps that hand out blocks
// of any size from the bottom of the heap or, with a negative alignment,
// from the top. Every block, free or allocated, is preceded by a 0x14-byte
// header. Free blocks are kept in a list sorted by address; allocated blocks
// in a list in the order they were made.

// MEMExpHeap fields after the common header
const (
	ehFree  = 0x40 // list of free blocks { first, last }
	ehUsed  = 0x48 // list of allocated blocks
	ehGroup = 0x50 // u16: group ID stamped into new blocks
	ehAttr  = 0x52 // u16: bit 0 best-fit search, bit 1 keep alignment gaps as free blocks
	ehSize  = 0x54

	ehBestFit  = 1 << 0
	ehKeepGaps = 1 << 1

	// block lists { first, last }
	blFirst = 0
	blLast  = 4
)

// block header (MEMExpHeapBlock)
const (
	ebAttr = 0x00 // bit 31 from the top, bits 8..30 padding in front of the header, bits 0..7 group
	ebSize = 0x04 // bytes after the header
	ebPrev = 0x08
	ebNext = 0x0c
	ebTag  = 0x10 // u16
	ebLen  = 0x14

	ebFreeTag = 0x4652 // "FR"
	ebUsedTag = 0x5544 // "UD"

	ebMinFree = ebLen + 4 // the smallest leftover worth keeping as a free block
)

// ---- block lists ----

func (m *Machine) blkUnlink(list, b uint32) (before uint32) {
	mem := m.Mem
	before, after := mem.Read32(b+ebPrev), mem.Read32(b+ebNext)
	if before == 0 {
		mem.Write32(list+blFirst, after)
	} else {
		mem.Write32(before+ebNext, after)
	}
	if after == 0 {
		mem.Write32(list+blLast, before)
	} else {
		mem.Write32(after+ebPrev, before)
	}
	return before
}

// blkLinkAfter inserts b after `after` (at the front when after is 0).
func (m *Machine) blkLinkAfter(list, after, b uint32) {
	mem := m.Mem
	var follow uint32
	if after == 0 {
		follow = mem.Read32(list + blFirst)
		mem.Write32(list+blFirst, b)
	} else {
		follow = mem.Read32(after + ebNext)
		mem.Write32(after+ebNext, b)
	}
	mem.Write32(b+ebPrev, after)
	mem.Write32(b+ebNext, follow)
	if follow == 0 {
		mem.Write32(list+blLast, b)
	} else {
		mem.Write32(follow+ebPrev, b)
	}
}

// ---- blocks ----

// newBlock writes a fresh header at hdr for a block whose data runs up to
// end.
func (m *Machine) newBlock(hdr, end uint32, tag uint16) uint32 {
	mem := m.Mem
	mem.Fill(hdr, 0, ebLen)
	mem.Write32(hdr+ebSize, end-hdr-ebLen)
	mem.Write16(hdr+ebTag, tag)
	return hdr
}

func (m *Machine) blockData(b uint32) uint32 { return b + ebLen }
func (m *Machine) blockEnd(b uint32) uint32  { return b + ebLen + m.Mem.Read32(b+ebSize) }

// blockFootprint is all the memory the block at b occupies, including the
// padding recorded in front of its header.
func (m *Machine) blockFootprint(b uint32) (lo, hi uint32) {
	pad := m.Mem.Read32(b+ebAttr) >> 8 & 0x7fffff
	return b - pad, m.blockEnd(b)
}

// ---- the heap ----

func (m *Machine) isExpHeap(heap uint32) bool { return m.heapTagIs(heap, tagExpHeap) }

// createExpHeap is MEMCreateExpHeapEx.
func (m *Machine) createExpHeap(start, size, options uint32) uint32 {
	heap, end, ok := heapSpan(start, size, ehSize+ebLen+4)
	if !ok {
		return 0
	}
	m.heapHeaderInit(heap, tagExpHeap, heap+ehSize, end, options)
	mem := m.Mem
	mem.Fill(heap+hhSize, 0, ehSize-hhSize)
	whole := m.newBlock(heap+ehSize, end, ebFreeTag)
	mem.Write32(heap+ehFree+blFirst, whole)
	mem.Write32(heap+ehFree+blLast, whole)
	return heap
}

// giveBack returns [lo, hi) to the free list, joined with any free block
// that touches it on either side. It reports false (and changes nothing)
// when the result would be too small to hold a header.
func (m *Machine) giveBack(heap, lo, hi uint32) bool {
	mem := m.Mem
	free := heap + ehFree
	debug := m.heapOptions(heap)&memDebugFill != 0

	// find the free blocks just below and just above
	var below, above uint32
	for b := mem.Read32(free + blFirst); b != 0; b = mem.Read32(b + ebNext) {
		if b >= lo {
			above = b
			break
		}
		below = b
	}
	newLo, newHi := lo, hi
	if above != 0 && above == hi {
		newHi = m.blockEnd(above)
		m.blkUnlink(free, above)
		if debug {
			m.heapMark(above, ebLen, markOnFree)
		}
	}
	after := below
	if below != 0 && m.blockEnd(below) == lo {
		newLo = below
		after = m.blkUnlink(free, below)
	}
	if newHi-newLo < ebLen {
		return false
	}
	if debug {
		m.heapMark(lo, hi-lo, markOnFree)
	}
	m.blkLinkAfter(free, after, m.newBlock(newLo, newHi, ebFreeTag))
	return true
}

// carveExp allocates size bytes of data at address at out of free block fb.
func (m *Machine) carveExp(heap, fb, at, size uint32, top bool) uint32 {
	mem := m.Mem
	free := heap + ehFree
	keepGaps := mem.Read16(heap+ehAttr)&ehKeepGaps != 0

	lo, _ := m.blockFootprint(fb)
	hi := m.blockEnd(fb)
	hdr := at - ebLen
	used0, used1 := lo, hi // what the new block will own

	after := m.blkUnlink(free, fb)
	// what's in front of the header
	if hdr-lo >= ebMinFree && (top || keepGaps) {
		m.blkLinkAfter(free, after, m.newBlock(lo, hdr, ebFreeTag))
		after = lo
		used0 = hdr
	}
	// what's behind the data
	if tail := at + size; hi-tail >= ebMinFree && (!top || keepGaps) {
		m.blkLinkAfter(free, after, m.newBlock(tail, hi, ebFreeTag))
		used1 = tail
	}

	switch opts := m.heapOptions(heap); {
	case opts&memZeroAlloc != 0:
		mem.Fill(used0, 0, used1-used0)
	case opts&memDebugFill != 0:
		m.heapMark(used0, used1-used0, markOnAlloc)
	}

	b := m.newBlock(hdr, used1, ebUsedTag)
	attr := (hdr-used0)&0x7fffff<<8 | uint32(mem.Read16(heap+ehGroup))&0xff
	if top {
		attr |= 1 << 31
	}
	mem.Write32(b+ebAttr, attr)
	used := heap + ehUsed
	m.blkLinkAfter(used, mem.Read32(used+blLast), b)
	return at
}

// fitExp searches the free list for a block that can hold size bytes with
// the alignment, from the bottom or the top. First fit, or the smallest
// block that fits when the heap is in best-fit mode.
func (m *Machine) fitExp(heap, size, align uint32, top bool) (block, at uint32) {
	mem := m.Mem
	bestFit := mem.Read16(heap+ehAttr)&ehBestFit != 0
	a := uint64(align)
	bestSize := uint64(1) << 32
	b := mem.Read32(heap + ehFree + blFirst)
	if top {
		b = mem.Read32(heap + ehFree + blLast)
	}
	for b != 0 {
		data := uint64(b + ebLen)
		n := uint64(mem.Read32(b + ebSize))
		var p uint64
		ok := false
		if top {
			if data+n >= uint64(size) {
				p = (data + n - uint64(size)) &^ (a - 1)
				ok = p >= data
			}
		} else {
			p = roundUp64(data, a)
			ok = p-data+uint64(size) <= n
		}
		if ok && n < bestSize {
			block, at, bestSize = b, uint32(p), n
			if !bestFit || n == uint64(size) {
				break
			}
		}
		if top {
			b = mem.Read32(b + ebPrev)
		} else {
			b = mem.Read32(b + ebNext)
		}
	}
	return block, at
}

// expAlloc is MEMAllocFromExpHeapEx; a negative alignment allocates from
// the top. A heap of another kind is handed to that kind's allocator.
func (m *Machine) expAlloc(heap, size uint32, align int32) uint32 {
	if !m.isExpHeap(heap) {
		if heap == 0 {
			return 0
		}
		return m.heapAlloc(heap, size, align)
	}
	if align == 0 || size > 0xfffffffc {
		return 0
	}
	size = (max(size, 1) + 3) &^ 3
	top := align < 0
	a := uint32(align)
	if top {
		a = uint32(-int64(align))
	}
	b, at := m.fitExp(heap, size, a, top)
	if b == 0 {
		return 0
	}
	return m.carveExp(heap, b, at, size, top)
}

// ownsExp reports whether p is the data of an allocated block of heap.
func (m *Machine) ownsExp(heap, p uint32) bool {
	mem := m.Mem
	if p < mem.Read32(heap+hhFirst)+ebLen || p >= mem.Read32(heap+hhLimit) {
		return false
	}
	return mem.Read16(p-ebLen+ebTag) == ebUsedTag
}

// expFree is MEMFreeToExpHeap.
func (m *Machine) expFree(heap, p uint32) {
	if p == 0 || !m.isExpHeap(heap) || !m.ownsExp(heap, p) {
		return
	}
	b := p - ebLen
	lo, hi := m.blockFootprint(b)
	m.blkUnlink(heap+ehUsed, b)
	m.giveBack(heap, lo, hi)
}

// expResize is MEMResizeForMBlockExpHeap: the block grows into a free block
// right behind it, or shrinks and frees its tail. It returns the block's
// size afterwards, or 0 when it can't grow.
func (m *Machine) expResize(heap, p, size uint32) uint32 {
	if !m.isExpHeap(heap) || !m.ownsExp(heap, p) {
		return 0
	}
	mem := m.Mem
	b := p - ebLen
	size = (size + 3) &^ 3
	cur := mem.Read32(b + ebSize)
	switch {
	case size == cur:
		return cur
	case size < cur:
		end := m.blockEnd(b)
		mem.Write32(b+ebSize, size)
		if !m.giveBack(heap, p+size, end) {
			mem.Write32(b+ebSize, cur)
		}
		return mem.Read32(b + ebSize)
	}

	// growing: the next free block must start right at our end
	end := m.blockEnd(b)
	free := heap + ehFree
	var next uint32
	for fb := mem.Read32(free + blFirst); fb != 0; fb = mem.Read32(fb + ebNext) {
		if fb == end {
			next = fb
			break
		}
	}
	if next == 0 || uint64(size) > uint64(cur)+ebLen+uint64(mem.Read32(next+ebSize)) {
		return 0
	}
	gained, nextEnd := end, m.blockEnd(next)
	after := m.blkUnlink(free, next)
	newEnd := p + size
	if nextEnd-newEnd >= ebLen {
		m.blkLinkAfter(free, after, m.newBlock(newEnd, nextEnd, ebFreeTag))
	} else {
		newEnd = nextEnd
	}
	mem.Write32(b+ebSize, newEnd-p)
	switch opts := m.heapOptions(heap); {
	case opts&memZeroAlloc != 0:
		mem.Fill(gained, 0, newEnd-gained)
	case opts&memDebugFill != 0:
		m.heapMark(gained, newEnd-gained, markOnAlloc)
	}
	return newEnd - p
}

// expFreeTotal is MEMGetTotalFreeSizeForExpHeap.
func (m *Machine) expFreeTotal(heap uint32) uint32 {
	var n uint32
	for b := m.Mem.Read32(heap + ehFree + blFirst); b != 0; b = m.Mem.Read32(b + ebNext) {
		n += m.Mem.Read32(b + ebSize)
	}
	return n
}

// expLargest is MEMGetAllocatableSizeForExpHeapEx: the biggest block the
// alignment allows; among equal sizes, the one wasting least on alignment.
func (m *Machine) expLargest(heap uint32, align int32) uint32 {
	a := uint64(1)
	if align > 0 {
		a = uint64(align)
	} else if align < 0 {
		a = uint64(-int64(align))
	}
	var best, bestWaste uint32 = 0, 0xffffffff
	for b := m.Mem.Read32(heap + ehFree + blFirst); b != 0; b = m.Mem.Read32(b + ebNext) {
		data, end := uint64(b+ebLen), uint64(m.blockEnd(b))
		p := roundUp64(data, a)
		if p >= end {
			continue
		}
		n, waste := uint32(end-p), uint32(p-data)
		if n > best || n == best && waste < bestWaste {
			best, bestWaste = n, waste
		}
	}
	return best
}

// expTrim is MEMAdjustExpHeap: a free block at the very top of the heap is
// cut off. It returns the heap's new size, or 0.
func (m *Machine) expTrim(heap uint32) uint32 {
	mem := m.Mem
	last := mem.Read32(heap + ehFree + blLast)
	if last == 0 || m.blockEnd(last) != mem.Read32(heap+hhLimit) {
		return 0
	}
	m.blkUnlink(heap+ehFree, last)
	mem.Write32(heap+hhLimit, last)
	return last - heap
}

func init() {
	c := "coreinit"
	onExp := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			heap := arg(cpu, 0)
			if !m.isExpHeap(heap) {
				ret(cpu, 0)
				return
			}
			fn(m, cpu, heap)
		}
	}
	blockAttr := func(p uint32, m *Machine) uint32 { return m.Mem.Read32(p - ebLen + ebAttr) }

	reg(c, "MEMCreateExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createExpHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)&0xffff))
	})
	reg(c, "MEMDestroyExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		m.heapUnlink(heap)
		ret(cpu, heap)
	}))
	reg(c, "MEMAllocFromExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToExpHeap", func(m *Machine, cpu *CPU, t *Thread) { m.expFree(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMSetAllocModeForExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		attr := m.Mem.Read16(heap + ehAttr)
		m.Mem.Write16(heap+ehAttr, attr&^ehBestFit|uint16(arg(cpu, 1))&ehBestFit)
		ret(cpu, uint32(attr&ehBestFit))
	}))
	reg(c, "MEMGetAllocModeForExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, uint32(m.Mem.Read16(heap+ehAttr)&ehBestFit))
	}))
	reg(c, "MEMAdjustExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.expTrim(heap)) }))
	reg(c, "MEMResizeForMBlockExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.expResize(heap, arg(cpu, 1), arg(cpu, 2)))
	}))
	reg(c, "MEMGetTotalFreeSizeForExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.expFreeTotal(heap)) }))
	reg(c, "MEMGetAllocatableSizeForExpHeapEx", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.expLargest(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMSetGroupIDForExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		old := m.Mem.Read16(heap + ehGroup)
		m.Mem.Write16(heap+ehGroup, uint16(arg(cpu, 1)))
		ret(cpu, uint32(old))
	}))
	reg(c, "MEMGetGroupIDForExpHeap", onExp(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, uint32(m.Mem.Read16(heap+ehGroup)))
	}))
	// (heap, visitor, user): visitor(data, heap, user) for every allocated
	// block, oldest first
	reg(c, "MEMVisitAllocatedForExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		heap, visitor, user := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if visitor == 0 || !m.isExpHeap(heap) {
			return
		}
		var step func(m *Machine, cpu *CPU, t *Thread, b uint32)
		step = func(m *Machine, cpu *CPU, t *Thread, b uint32) {
			if b == 0 {
				ret(cpu, 0)
				return
			}
			m.callGuest(cpu, t, visitor, []uint32{m.blockData(b), heap, user}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
				step(m, cpu, t, m.Mem.Read32(b+ebNext))
			})
		}
		step(m, cpu, t, m.Mem.Read32(heap+ehUsed+blFirst))
	})
	reg(c, "MEMGetSizeForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		if p == 0 {
			ret(cpu, 0)
			return
		}
		ret(cpu, m.Mem.Read32(p-ebLen+ebSize))
	})
	reg(c, "MEMGetGroupIDForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, blockAttr(arg(cpu, 0), m)&0xff)
	})
	reg(c, "MEMGetAllocDirForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, blockAttr(arg(cpu, 0), m)>>31)
	})
	reg(c, "MEMCheckExpHeap", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.isExpHeap(arg(cpu, 0))) })
	reg(c, "MEMCheckForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })

	reg(c, "MEMInitAllocatorForExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.setAllocator(arg(cpu, 0), "Exp", arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "__RPAllocatorExpAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		heap, align := m.allocatorHeapArg(arg(cpu, 0))
		ret(cpu, m.expAlloc(heap, arg(cpu, 1), align))
	})
	reg(c, "__RPAllocatorExpFree", func(m *Machine, cpu *CPU, t *Thread) {
		heap, _ := m.allocatorHeapArg(arg(cpu, 0))
		m.expFree(heap, arg(cpu, 1))
	})
}
