// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// Block heaps (MEMBlockHeap). The program supplies the header, the address
// range to manage and memory for bookkeeping records ("tracks"). Each track
// describes one span of the range, free or in use; the tracks of the range
// form a doubly linked list in address order. Records not in use sit on a
// singly linked spare list. Carving a block out of a free span needs up to
// two spare records, for what's left in front of it and behind it.

// MEMBlockHeap fields after the common header
const (
	bhBuiltin   = 0x50 // a track inside the header, used for the initial span
	bhFirst     = 0x64 // the lowest span
	bhLast      = 0x68 // the highest span
	bhSpare     = 0x6c // spare tracks
	bhSpareN    = 0x70 // how many
	bhHeaderLen = 0x80
)

// a track (MEMBlockHeapTracking)
const (
	trStart = 0x00
	trLast  = 0x04 // the span's final byte
	trFree  = 0x08
	trPrev  = 0x0c
	trNext  = 0x10
	trLen   = 0x14

	trGroupHeader = 0x10 // memory added for tracks starts with this header
)

// span is a track read out of the program's memory.
type span struct {
	addr        uint32
	start, last uint32
	free        bool
	prev, next  uint32
}

func (m *Machine) loadSpan(addr uint32) span {
	mem := m.Mem
	return span{
		addr:  addr,
		start: mem.Read32(addr + trStart),
		last:  mem.Read32(addr + trLast),
		free:  mem.Read32(addr+trFree) != 0,
		prev:  mem.Read32(addr + trPrev),
		next:  mem.Read32(addr + trNext),
	}
}

func (m *Machine) storeSpan(s span) {
	mem := m.Mem
	mem.Write32(s.addr+trStart, s.start)
	mem.Write32(s.addr+trLast, s.last)
	var f uint32
	if s.free {
		f = 1
	}
	mem.Write32(s.addr+trFree, f)
	mem.Write32(s.addr+trPrev, s.prev)
	mem.Write32(s.addr+trNext, s.next)
}

func (s span) size() uint32 { return s.last - s.start + 1 }

func (m *Machine) isBlockHeap(heap uint32) bool { return m.heapTagIs(heap, tagBlockHeap) }

// initBlockHeap is MEMInitBlockHeap(heap, start, end, tracks, tracks size,
// options).
func (m *Machine) initBlockHeap(heap, start, end, tracks, tracksSize, options uint32) uint32 {
	if heap == 0 || start == 0 || end == 0 || end <= start {
		return 0
	}
	if tracks == 0 || tracksSize == 0 {
		tracks, tracksSize = 0, 0
	}
	m.heapHeaderInit(heap, tagBlockHeap, start, end, options)
	m.Mem.Fill(heap+hhSize, 0, bhHeaderLen-hhSize)
	whole := span{addr: heap + bhBuiltin, start: start, last: end - 1, free: true}
	m.storeSpan(whole)
	m.Mem.Write32(heap+bhFirst, whole.addr)
	m.Mem.Write32(heap+bhLast, whole.addr)
	if tracks != 0 && m.addBlockTracks(heap, tracks, tracksSize) != 0 {
		m.heapUnlink(heap)
		return 0
	}
	return heap
}

// addBlockTracks is MEMAddBlockHeapTracking: it carves mem into tracks and
// puts them on the spare list. -4 when the memory can't hold any.
func (m *Machine) addBlockTracks(heap, p, size uint32) int32 {
	if !m.isBlockHeap(heap) {
		return 0
	}
	if p == 0 || size <= trGroupHeader+trLen {
		return -4
	}
	mem := m.Mem
	n := (size - trGroupHeader) / trLen
	first := p + trGroupHeader
	// the group header: two zero words, then where the tracks are and how many
	mem.Write32(p, 0)
	mem.Write32(p+4, 0)
	mem.Write32(p+8, first)
	mem.Write32(p+12, n)
	for i := n; i > 0; i-- {
		m.spareTrack(heap, first+(i-1)*trLen)
	}
	return 0
}

// spareTrack puts a record on the spare list.
func (m *Machine) spareTrack(heap, tr uint32) {
	mem := m.Mem
	mem.Write32(tr+trPrev, 0)
	mem.Write32(tr+trNext, mem.Read32(heap+bhSpare))
	mem.Write32(heap+bhSpare, tr)
	mem.Write32(heap+bhSpareN, mem.Read32(heap+bhSpareN)+1)
}

// takeTrack takes a record off the spare list.
func (m *Machine) takeTrack(heap uint32) uint32 {
	mem := m.Mem
	tr := mem.Read32(heap + bhSpare)
	mem.Write32(heap+bhSpare, mem.Read32(tr+trNext))
	mem.Write32(heap+bhSpareN, mem.Read32(heap+bhSpareN)-1)
	mem.Write32(tr+trNext, 0)
	return tr
}

// relink points the neighbours of s (and the heap's ends) at s.
func (m *Machine) relink(heap uint32, s span) {
	mem := m.Mem
	if s.prev == 0 {
		mem.Write32(heap+bhFirst, s.addr)
	} else {
		mem.Write32(s.prev+trNext, s.addr)
	}
	if s.next == 0 {
		mem.Write32(heap+bhLast, s.addr)
	} else {
		mem.Write32(s.next+trPrev, s.addr)
	}
}

// carve marks [at, at+size) of the free span s as used. The pieces left on
// either side become free spans of their own; without enough spare tracks
// for them nothing changes and carve fails. Carved memory is zeroed.
func (m *Machine) carve(heap uint32, s span, at, size uint32) bool {
	front := at > s.start
	back := at+size-1 < s.last
	need := uint32(0)
	if front {
		need++
	}
	if back {
		need++
	}
	if m.Mem.Read32(heap+bhSpareN) < need {
		return false
	}
	used := s
	used.start, used.last, used.free = at, at+size-1, false
	if front {
		f := span{addr: m.takeTrack(heap), start: s.start, last: at - 1, free: true, prev: s.prev, next: used.addr}
		m.storeSpan(f)
		m.relink(heap, f)
		used.prev = f.addr
	}
	if back {
		b := span{addr: m.takeTrack(heap), start: at + size, last: s.last, free: true, prev: used.addr, next: s.next}
		m.storeSpan(b)
		m.relink(heap, b)
		used.next = b.addr
	}
	m.storeSpan(used)
	m.relink(heap, used)
	m.Mem.Fill(at, 0, size)
	return true
}

// blockAlloc is MEMAllocFromBlockHeapEx; a negative alignment searches
// from the top of the range down. The first free span that fits is used.
func (m *Machine) blockAlloc(heap, size uint32, align int32) uint32 {
	if size == 0 || !m.isBlockHeap(heap) {
		return 0
	}
	down := align < 0
	a := uint64(align)
	switch {
	case align == 0:
		a = 4
	case down:
		a = uint64(-int64(align))
	}
	tr := m.Mem.Read32(heap + bhFirst)
	if down {
		tr = m.Mem.Read32(heap + bhLast)
	}
	for tr != 0 {
		s := m.loadSpan(tr)
		if s.free {
			lo, hi := uint64(s.start), uint64(s.last)+1
			var at uint64
			fits := false
			if down {
				if hi >= uint64(size) {
					at = (hi - uint64(size)) &^ (a - 1)
					fits = at >= lo
				}
			} else {
				at = roundUp64(lo, a)
				fits = at < hi && hi-at >= uint64(size)
			}
			if fits {
				if m.carve(heap, s, uint32(at), size) {
					return uint32(at)
				}
				return 0
			}
		}
		if down {
			tr = s.prev
		} else {
			tr = s.next
		}
	}
	return 0
}

// blockAllocAt is MEMAllocFromBlockHeapAt: exactly [at, at+size).
func (m *Machine) blockAllocAt(heap, at, size uint32) uint32 {
	if size == 0 || !m.isBlockHeap(heap) {
		return 0
	}
	for tr := m.Mem.Read32(heap + bhFirst); tr != 0; {
		s := m.loadSpan(tr)
		if at >= s.start && at <= s.last {
			if !s.free || uint64(at)+uint64(size)-1 > uint64(s.last) || !m.carve(heap, s, at, size) {
				return 0
			}
			return at
		}
		tr = s.next
	}
	return 0
}

// spanAt finds the span starting exactly at p.
func (m *Machine) spanAt(heap, p uint32) (span, bool) {
	mem := m.Mem
	if p < mem.Read32(heap+hhFirst) || p > mem.Read32(heap+hhLimit) {
		return span{}, false
	}
	for tr := mem.Read32(heap + bhFirst); tr != 0; {
		s := m.loadSpan(tr)
		if s.start == p {
			return s, true
		}
		tr = s.next
	}
	return span{}, false
}

// absorb merges span b (which follows a) into a and recycles b's record.
func (m *Machine) absorb(heap uint32, a, b span) span {
	a.last, a.next = b.last, b.next
	m.storeSpan(a)
	m.relink(heap, a)
	m.spareTrack(heap, b.addr)
	return a
}

// blockFree is MEMFreeToBlockHeap: the span becomes free and joins any free
// neighbour.
func (m *Machine) blockFree(heap, p uint32) {
	if !m.isBlockHeap(heap) {
		return
	}
	s, ok := m.spanAt(heap, p)
	if !ok || s.free {
		return
	}
	s.free = true
	m.storeSpan(s)
	if s.prev != 0 {
		if before := m.loadSpan(s.prev); before.free {
			s = m.absorb(heap, before, s)
		}
	}
	if s.next != 0 {
		if after := m.loadSpan(s.next); after.free {
			m.absorb(heap, s, after)
		}
	}
}

// blockLargest is MEMGetAllocatableSizeForBlockHeapEx.
func (m *Machine) blockLargest(heap uint32, align int32) uint32 {
	a := uint64(4)
	if align > 0 {
		a = uint64(align)
	} else if align < 0 {
		a = uint64(-int64(align))
	}
	var best uint32
	for tr := m.Mem.Read32(heap + bhFirst); tr != 0; {
		s := m.loadSpan(tr)
		if s.free {
			at := (uint64(s.start) + a - 1) / a * a
			if at <= uint64(s.last) {
				best = max(best, uint32(uint64(s.last)+1-at))
			}
		}
		tr = s.next
	}
	return best
}

// blockFreeTotal is MEMGetTotalFreeSizeForBlockHeap.
func (m *Machine) blockFreeTotal(heap uint32) uint32 {
	var n uint32
	for tr := m.Mem.Read32(heap + bhFirst); tr != 0; {
		s := m.loadSpan(tr)
		if s.free {
			n += s.size()
		}
		tr = s.next
	}
	return n
}

func init() {
	c := "coreinit"
	onBlock := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			heap := arg(cpu, 0)
			if !m.isBlockHeap(heap) {
				ret(cpu, 0)
				return
			}
			fn(m, cpu, heap)
		}
	}

	reg(c, "MEMInitBlockHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.initBlockHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5)))
	})
	reg(c, "MEMDestroyBlockHeap", onBlock(func(m *Machine, cpu *CPU, heap uint32) {
		m.heapUnlink(heap)
		m.Mem.Fill(heap, 0, bhHeaderLen)
		ret(cpu, heap)
	}))
	reg(c, "MEMAddBlockHeapTracking", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, uint32(m.addBlockTracks(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))))
	})
	reg(c, "MEMGetTrackingLeftInBlockHeap", onBlock(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.Mem.Read32(heap+bhSpareN))
	}))
	reg(c, "MEMAllocFromBlockHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.blockAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMAllocFromBlockHeapAt", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.blockAllocAt(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)))
	})
	reg(c, "MEMFreeToBlockHeap", func(m *Machine, cpu *CPU, t *Thread) { m.blockFree(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMGetAllocatableSizeForBlockHeapEx", onBlock(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.blockLargest(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMGetTotalFreeSizeForBlockHeap", onBlock(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.blockFreeTotal(heap))
	}))
	reg(c, "MEMGetSizeForMBlockBlockHeap", onBlock(func(m *Machine, cpu *CPU, heap uint32) {
		s, ok := m.spanAt(heap, arg(cpu, 1))
		if !ok || s.free {
			ret(cpu, 0)
			return
		}
		ret(cpu, s.size())
	}))

	reg(c, "MEMInitAllocatorForBlockHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.setAllocator(arg(cpu, 0), "Block", arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "__RPAllocatorBlockAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		heap, align := m.allocatorHeapArg(arg(cpu, 0))
		ret(cpu, m.blockAlloc(heap, arg(cpu, 1), align))
	})
	reg(c, "__RPAllocatorBlockFree", func(m *Machine, cpu *CPU, t *Thread) {
		heap, _ := m.allocatorHeapArg(arg(cpu, 0))
		m.blockFree(heap, arg(cpu, 1))
	})
}
