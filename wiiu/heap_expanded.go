package wiiu

// Expanded heaps, coreinit's malloc. Each block, used or free, has a 0x14
// byte header just before its data. Free blocks form a chain in address
// order and used blocks a chain in the order they were handed out. A
// positive alignment allocates from the low end of the heap, a negative one
// from the high end.
//
// Written for RetroPixels from the SDK's structure layouts and the
// allocation behaviour programs rely on.

const (
	// after MEMHeapBase
	expFreeChain = 0x40 // first and last free block
	expUsedChain = 0x48 // first and last used block
	expGroupID   = 0x50 // u16
	expFields    = 0x52 // u16: bit 0 nearest-size mode, bit 1 keep alignment gaps free
	expHeapSize  = 0x54

	expModeNear    = 1
	expAlignMargin = 2

	// a block's header
	blkAttr    = 0x00 // bit 31 taken from the high end; bits 8-30 unused gap before the header; bits 0-7 group
	blkLen     = 0x04 // bytes of data
	blkPrev    = 0x08
	blkNext    = 0x0c
	blkTag     = 0x10 // u16
	blkHdrSize = 0x14

	tagFree = 'F'<<8 | 'R'
	tagUsed = 'U'<<8 | 'D'

	expMinFree = 0x18 // the smallest leftover kept as a free block
)

// blockChain is one of an expanded heap's chains: its first and last block,
// linked through the blocks' headers.
type blockChain struct {
	mem *Memory
	at  uint32
}

func (c blockChain) first() uint32 { return c.mem.Read32(c.at) }
func (c blockChain) last() uint32  { return c.mem.Read32(c.at + 4) }

// unlink takes b out and returns the block that was before it.
func (c blockChain) unlink(b uint32) uint32 {
	prev, next := c.mem.Read32(b+blkPrev), c.mem.Read32(b+blkNext)
	if prev == 0 {
		c.mem.Write32(c.at, next)
	} else {
		c.mem.Write32(prev+blkNext, next)
	}
	if next == 0 {
		c.mem.Write32(c.at+4, prev)
	} else {
		c.mem.Write32(next+blkPrev, prev)
	}
	return prev
}

// link puts b after prev, or first when prev is 0, and returns b.
func (c blockChain) link(b, prev uint32) uint32 {
	var next uint32
	if prev == 0 {
		next = c.first()
		c.mem.Write32(c.at, b)
	} else {
		next = c.mem.Read32(prev + blkNext)
		c.mem.Write32(prev+blkNext, b)
	}
	c.mem.Write32(b+blkPrev, prev)
	c.mem.Write32(b+blkNext, next)
	if next == 0 {
		c.mem.Write32(c.at+4, b)
	} else {
		c.mem.Write32(next+blkPrev, b)
	}
	return b
}

func (m *Machine) freeChain(heap uint32) blockChain { return blockChain{m.Mem, heap + expFreeChain} }
func (m *Machine) usedChain(heap uint32) blockChain { return blockChain{m.Mem, heap + expUsedChain} }

// newBlock writes a header at from for a block running to to.
func (m *Machine) newBlock(from, to uint32, tag uint16) uint32 {
	m.Mem.Fill(from, 0, blkHdrSize)
	m.Mem.Write16(from+blkTag, tag)
	m.Mem.Write32(from+blkLen, to-from-blkHdrSize)
	return from
}

func (m *Machine) blockData(b uint32) uint32 { return b + blkHdrSize }
func (m *Machine) blockTo(b uint32) uint32   { return b + blkHdrSize + m.Mem.Read32(b+blkLen) }

// blockFrom is where a block's memory starts, its unused gap included.
func (m *Machine) blockFrom(b uint32) uint32 {
	return b - (m.Mem.Read32(b+blkAttr)>>8)&0x7fffff
}

func (m *Machine) isExpHeap(heap uint32) bool { return m.heapKind(heap, heapMagicExp) }

// createExpHeap is MEMCreateExpHeapEx: the whole heap starts as one free
// block after the header.
func (m *Machine) createExpHeap(start, size, flags uint32) uint32 {
	from := (uint64(start) + 3) &^ 3
	to := (uint64(start) + uint64(size)) &^ 3
	if start == 0 || from > to || to-from < expHeapSize+blkHdrSize+4 {
		return 0
	}
	heap, end := uint32(from), uint32(to)
	m.memInitHeapBase(heap, heapMagicExp, heap+expHeapSize, end, flags)
	m.Mem.Fill(heap+heapBaseSize, 0, expHeapSize-heapBaseSize)
	b := m.newBlock(heap+expHeapSize, end, tagFree)
	m.freeChain(heap).link(b, 0)
	return heap
}

// expAlloc is MEMAllocFromExpHeapEx. Handed some other kind of heap, it
// allocates from that.
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
	if align > 0 {
		return m.expPick(heap, size, uint32(align), false)
	}
	return m.expPick(heap, size, uint32(-align), true)
}

// expPick chooses the free block to take size bytes from: in first-fit mode
// the first that fits, searching from the end allocated from; in nearest
// mode the smallest that fits, stopping early at one of exactly size.
func (m *Machine) expPick(heap, size, align uint32, high bool) uint32 {
	mem := m.Mem
	nearest := mem.Read16(heap+expFields)&expModeNear != 0
	chain := m.freeChain(heap)
	mask := uint64(align) - 1
	var pick, pickAt uint32
	pickLen := uint64(1<<32 - 1)
	b := chain.first()
	if high {
		b = chain.last()
	}
	for ; b != 0; b = m.expStep(b, high) {
		data, n := uint64(m.blockData(b)), uint64(mem.Read32(b+blkLen))
		var at uint64
		if high {
			if data+n < uint64(size) {
				continue
			}
			at = (data + n - uint64(size)) &^ mask
			if at < data {
				continue
			}
		} else {
			at = (data + mask) &^ mask
			if at-data+uint64(size) > n {
				continue
			}
		}
		if n >= pickLen {
			continue
		}
		pick, pickAt, pickLen = b, uint32(at), n
		if !nearest || n == uint64(size) {
			break
		}
	}
	if pick == 0 {
		return 0
	}
	return m.expCarve(heap, pick, pickAt, size, high)
}

func (m *Machine) expStep(b uint32, back bool) uint32 {
	if back {
		return m.Mem.Read32(b + blkPrev)
	}
	return m.Mem.Read32(b + blkNext)
}

// expCarve hands out size bytes at data address at from free block fb. What
// is left in front and behind stays free if big enough, but a gap that only
// came from aligning stays free only with the heap's align-margin bit;
// otherwise the used block takes it.
func (m *Machine) expCarve(heap, fb, at, size uint32, high bool) uint32 {
	mem := m.Mem
	keepGaps := mem.Read16(heap+expFields)&expAlignMargin != 0
	chain := m.freeChain(heap)

	from, to := m.blockFrom(fb), m.blockTo(fb)
	usedFrom, usedTo := at-blkHdrSize, at+size
	prev := chain.unlink(fb)
	if usedFrom-from >= expMinFree && (high || keepGaps) {
		prev = chain.link(m.newBlock(from, usedFrom, tagFree), prev)
	} else {
		usedFrom = from
	}
	if to-usedTo >= expMinFree && (!high || keepGaps) {
		chain.link(m.newBlock(usedTo, to, tagFree), prev)
	} else {
		usedTo = to
	}

	switch flags := m.heapFlagsOf(heap); {
	case flags&heapOptClear != 0:
		mem.Fill(usedFrom, 0, usedTo-usedFrom)
	case flags&heapOptFill != 0:
		m.heapFill(usedFrom, usedTo-usedFrom, fillOnAlloc)
	}

	b := m.newBlock(at-blkHdrSize, usedTo, tagUsed)
	attr := (b-usedFrom)&0x7fffff<<8 | uint32(mem.Read16(heap+expGroupID))&0xff
	if high {
		attr |= 1 << 31
	}
	mem.Write32(b+blkAttr, attr)
	used := m.usedChain(heap)
	used.link(b, used.last())
	return at
}

// expGiveBack makes from..to free again, joined with free memory touching
// it on either side. It fails only for a piece too small for a header.
func (m *Machine) expGiveBack(heap, from, to uint32) bool {
	mem := m.Mem
	chain := m.freeChain(heap)
	debugFill := m.heapFlagsOf(heap)&heapOptFill != 0
	joinFrom, joinTo := from, to

	// the last free block in front of from, and one starting right at to
	var prev uint32
	b := chain.first()
	for b != 0 && b < from {
		prev, b = b, mem.Read32(b+blkNext)
	}
	if b != 0 && b == to {
		joinTo = m.blockTo(b)
		chain.unlink(b)
		if debugFill {
			m.heapFill(b, blkHdrSize, fillOnFree)
		}
	}
	if prev != 0 && m.blockTo(prev) == from {
		joinFrom = prev
		prev = chain.unlink(prev)
	}
	if joinTo-joinFrom < blkHdrSize {
		return false
	}
	if debugFill {
		m.heapFill(from, to-from, fillOnFree)
	}
	chain.link(m.newBlock(joinFrom, joinTo, tagFree), prev)
	return true
}

// expOwns reports whether p is the data of a used block of heap.
func (m *Machine) expOwns(heap, p uint32) bool {
	mem := m.Mem
	if p < mem.Read32(heap+heapStart)+blkHdrSize || p >= mem.Read32(heap+heapEnd) {
		return false
	}
	return mem.Read16(p-blkHdrSize+blkTag) == tagUsed
}

// expFree is MEMFreeToExpHeap.
func (m *Machine) expFree(heap, p uint32) {
	if p == 0 || !m.isExpHeap(heap) || !m.expOwns(heap, p) {
		return
	}
	b := p - blkHdrSize
	from, to := m.blockFrom(b), m.blockTo(b)
	m.usedChain(heap).unlink(b)
	m.expGiveBack(heap, from, to)
}

// expResize is MEMResizeForMBlockExpHeap: a block shrinks in place, or grows
// into a free block right after it. It returns the new size, or 0.
func (m *Machine) expResize(heap, p, size uint32) uint32 {
	mem := m.Mem
	if !m.isExpHeap(heap) || !m.expOwns(heap, p) {
		return 0
	}
	b := p - blkHdrSize
	size = (size + 3) &^ 3
	old := mem.Read32(b + blkLen)
	switch {
	case size == old:
		return old
	case size < old:
		to := m.blockTo(b)
		mem.Write32(b+blkLen, size)
		if !m.expGiveBack(heap, p+size, to) {
			mem.Write32(b+blkLen, old)
		}
		return mem.Read32(b + blkLen)
	}

	chain := m.freeChain(heap)
	next := m.blockTo(b)
	fb := chain.first()
	for fb != 0 && fb != next {
		fb = mem.Read32(fb + blkNext)
	}
	if fb == 0 || uint64(size) > uint64(old)+uint64(mem.Read32(fb+blkLen))+blkHdrSize {
		return 0
	}
	grownFrom, freeTo := m.blockFrom(fb), m.blockTo(fb)
	prev := chain.unlink(fb)
	newTo := p + size
	if freeTo-newTo < blkHdrSize {
		newTo = freeTo
	} else {
		chain.link(m.newBlock(newTo, freeTo, tagFree), prev)
	}
	mem.Write32(b+blkLen, newTo-p)
	switch flags := m.heapFlagsOf(heap); {
	case flags&heapOptClear != 0:
		mem.Fill(grownFrom, 0, newTo-grownFrom)
	case flags&heapOptFill != 0:
		m.heapFill(grownFrom, newTo-grownFrom, fillOnAlloc)
	}
	return newTo - p
}

func (m *Machine) expTotalFree(heap uint32) uint32 {
	var n uint32
	for b := m.freeChain(heap).first(); b != 0; b = m.Mem.Read32(b + blkNext) {
		n += m.Mem.Read32(b + blkLen)
	}
	return n
}

// expAllocatable is MEMGetAllocatableSizeForExpHeapEx: the largest block
// that alignment allows; between equal sizes, the one wasting least to
// alignment counts.
func (m *Machine) expAllocatable(heap uint32, align int32) uint32 {
	mask := uint64(max(absAlign(align), 1)) - 1
	var best uint32
	bestGap := uint32(1<<32 - 1)
	for b := m.freeChain(heap).first(); b != 0; b = m.Mem.Read32(b + blkNext) {
		data, to := uint64(m.blockData(b)), uint64(m.blockTo(b))
		at := (data + mask) &^ mask
		if at >= to {
			continue
		}
		n, gap := uint32(to-at), uint32(at-data)
		if n > best || n == best && gap < bestGap {
			best, bestGap = n, gap
		}
	}
	return best
}

func absAlign(v int32) int32 {
	if v < 0 {
		return -v
	}
	return v
}

// expAdjust is MEMAdjustExpHeap: free memory at the very end of the heap is
// given up, and the heap's new size returned (0 if there was none).
func (m *Machine) expAdjust(heap uint32) uint32 {
	mem := m.Mem
	chain := m.freeChain(heap)
	last := chain.last()
	if last == 0 || m.blockTo(last) != mem.Read32(heap+heapEnd) {
		return 0
	}
	chain.unlink(last)
	end := last
	mem.Write32(heap+heapEnd, end)
	return end - heap
}

func init() {
	c := "coreinit"
	exp := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc { return onHeap(heapMagicExp, fn) }
	u16Field := func(off uint32, mask uint16) (get, set hleFunc) {
		get = exp(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, uint32(m.Mem.Read16(heap+off)&mask)) })
		set = exp(func(m *Machine, cpu *CPU, heap uint32) {
			v := m.Mem.Read16(heap + off)
			ret(cpu, uint32(v&mask))
			m.Mem.Write16(heap+off, v&^mask|uint16(arg(cpu, 1))&mask)
		})
		return
	}

	reg(c, "MEMCreateExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createExpHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)&0xffff))
	})
	reg(c, "MEMDestroyExpHeap", exp(func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		ret(cpu, heap)
	}))
	reg(c, "MEMAllocFromExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToExpHeap", func(m *Machine, cpu *CPU, t *Thread) { m.expFree(arg(cpu, 0), arg(cpu, 1)) })
	getMode, setMode := u16Field(expFields, expModeNear)
	reg(c, "MEMGetAllocModeForExpHeap", getMode)
	reg(c, "MEMSetAllocModeForExpHeap", setMode)
	getGroup, setGroup := u16Field(expGroupID, 0xffff)
	reg(c, "MEMGetGroupIDForExpHeap", getGroup)
	reg(c, "MEMSetGroupIDForExpHeap", setGroup)
	reg(c, "MEMAdjustExpHeap", exp(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.expAdjust(heap)) }))
	reg(c, "MEMResizeForMBlockExpHeap", exp(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.expResize(heap, arg(cpu, 1), arg(cpu, 2)))
	}))
	reg(c, "MEMGetTotalFreeSizeForExpHeap", exp(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.expTotalFree(heap)) }))
	reg(c, "MEMGetAllocatableSizeForExpHeapEx", exp(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.expAllocatable(heap, int32(arg(cpu, 1))))
	}))

	// MEMVisitAllocatedForExpHeap(heap, visitor, param) calls
	// visitor(block, heap, param) for each used block, oldest first
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
			m.callGuest(cpu, t, visitor, []uint32{m.blockData(b), heap, param}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
				visit(m, cpu, t, m.Mem.Read32(b+blkNext))
			})
		}
		visit(m, cpu, t, m.usedChain(heap).first())
	})

	// about one block, from its data address
	attr := func(p uint32, m *Machine) uint32 { return m.Mem.Read32(p - blkHdrSize + blkAttr) }
	reg(c, "MEMGetSizeForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		var n uint32
		if p := arg(cpu, 0); p != 0 {
			n = m.Mem.Read32(p - blkHdrSize + blkLen)
		}
		ret(cpu, n)
	})
	reg(c, "MEMGetGroupIDForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, attr(arg(cpu, 0), m)&0xff) })
	reg(c, "MEMGetAllocDirForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, attr(arg(cpu, 0), m)>>31) })
	reg(c, "MEMCheckExpHeap", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.isExpHeap(arg(cpu, 0))) })
	reg(c, "MEMCheckForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })

	reg(c, "MEMInitAllocatorForExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Exp", arg(cpu, 1), arg(cpu, 2))
	})
	allocatorFor("Exp", func(m *Machine, heap, size uint32, align int32) uint32 { return m.expAlloc(heap, size, align) },
		func(m *Machine, heap, p uint32) { m.expFree(heap, p) })
}
