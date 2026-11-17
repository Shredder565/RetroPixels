package wiiu

// Block heaps: the program supplies the header, the range to manage and
// memory for "tracks". Each track describes one span of the range, free or
// used, and the tracks are linked in address order. Unused tracks wait in a
// spare chain; carving a block out of a span takes one spare track for the
// piece in front and one for the piece behind.
//
// Written for RetroPixels from the SDK's structure layouts and the
// behaviour programs rely on.

const (
	// after MEMHeapBase
	bhFirstTrack = 0x50 // a track kept in the header itself
	bhHead       = 0x64
	bhTail       = 0x68
	bhSpare      = 0x6c // spare tracks, chained by their next field
	bhSpareCount = 0x70
	bhHeapSize   = 0x80

	// a track: first byte, last byte, free flag, neighbours
	trFirst  = 0x00
	trLast   = 0x04
	trIsFree = 0x08
	trPrev   = 0x0c
	trNext   = 0x10
	trSize   = 0x14

	trackGroupHdr = 0x10 // in front of the tracks in memory given for them
)

// track is one track in the program's memory.
type track struct {
	mem *Memory
	at  uint32
}

func (m *Machine) track(at uint32) track { return track{m.Mem, at} }

func (t track) get(f uint32) uint32        { return t.mem.Read32(t.at + f) }
func (t track) set(f, v uint32)            { t.mem.Write32(t.at+f, v) }
func (t track) free() bool                 { return t.get(trIsFree) != 0 }
func (t track) span() (first, last uint32) { return t.get(trFirst), t.get(trLast) }

func (t track) fill(first, last uint32, free bool) {
	t.set(trFirst, first)
	t.set(trLast, last)
	t.set(trIsFree, b2u(free))
}

func (m *Machine) isBlockHeap(heap uint32) bool { return m.heapKind(heap, heapMagicBlock) }

// initBlockHeap is MEMInitBlockHeap(heap, start, end, tracks, their size,
// flags): one free span over start..end-1.
func (m *Machine) initBlockHeap(heap, start, end, tracks, tracksSize, flags uint32) uint32 {
	if heap == 0 || start == 0 || end == 0 || start >= end {
		return 0
	}
	if tracks == 0 || tracksSize == 0 {
		tracks, tracksSize = 0, 0
	}
	m.memInitHeapBase(heap, heapMagicBlock, start, end, flags)
	m.Mem.Fill(heap+heapBaseSize, 0, bhHeapSize-heapBaseSize)
	first := heap + bhFirstTrack
	m.track(first).fill(start, end-1, true)
	m.Mem.Write32(heap+bhHead, first)
	m.Mem.Write32(heap+bhTail, first)
	if tracksSize != 0 && m.addBlockTracks(heap, tracks, tracksSize) != 0 {
		m.memDestroyHeapBase(heap)
		return 0
	}
	return heap
}

// addBlockTracks is MEMAddBlockHeapTracking: the memory becomes a group
// header and as many spare tracks as fit.
func (m *Machine) addBlockTracks(heap, at, size uint32) int32 {
	if !m.isBlockHeap(heap) {
		return 0
	}
	if at == 0 || size <= trackGroupHdr+trSize {
		return -4
	}
	mem := m.Mem
	count := (size - trackGroupHdr) / trSize
	tracks := at + trackGroupHdr
	for i, v := range [4]uint32{0, 0, tracks, count} {
		mem.Write32(at+uint32(i)*4, v)
	}
	mem.Write32(tracks+trPrev, 0)
	for i := uint32(0); i < count; i++ {
		t := tracks + i*trSize
		next := t + trSize
		if i == count-1 {
			next = mem.Read32(heap + bhSpare)
		} else {
			mem.Write32(next+trPrev, 0)
		}
		mem.Write32(t+trNext, next)
	}
	mem.Write32(heap+bhSpare, tracks)
	mem.Write32(heap+bhSpareCount, mem.Read32(heap+bhSpareCount)+count)
	return 0
}

func (m *Machine) spareTrack(heap uint32) track {
	mem := m.Mem
	t := m.track(mem.Read32(heap + bhSpare))
	mem.Write32(heap+bhSpare, t.get(trNext))
	t.set(trNext, 0)
	mem.Write32(heap+bhSpareCount, mem.Read32(heap+bhSpareCount)-1)
	return t
}

func (m *Machine) retireTrack(heap uint32, t track) {
	mem := m.Mem
	t.set(trNext, mem.Read32(heap+bhSpare))
	mem.Write32(heap+bhSpare, t.at)
	mem.Write32(heap+bhSpareCount, mem.Read32(heap+bhSpareCount)+1)
}

// carve takes at..at+size-1 out of free track t. It fails when there aren't
// the spare tracks for what is left in front and behind.
func (m *Machine) carve(heap uint32, t track, at, size uint32) bool {
	mem := m.Mem
	first, last := t.span()
	front, behind := at > first, at+size-1 < last
	if mem.Read32(heap+bhSpareCount) < b2u(front)+b2u(behind) {
		return false
	}
	prev := t.get(trPrev)
	if front {
		p := m.spareTrack(heap)
		p.fill(first, at-1, true)
		p.set(trPrev, prev)
		p.set(trNext, t.at)
		if prev != 0 {
			m.track(prev).set(trNext, p.at)
		}
		if mem.Read32(heap+bhHead) == t.at {
			mem.Write32(heap+bhHead, p.at)
		}
		prev = p.at
	}
	t.fill(at, at+size-1, false)
	t.set(trPrev, prev)
	if behind {
		s := m.spareTrack(heap)
		next := t.get(trNext)
		s.fill(at+size, last, true)
		s.set(trPrev, t.at)
		s.set(trNext, next)
		if next != 0 {
			m.track(next).set(trPrev, s.at)
		}
		if mem.Read32(heap+bhTail) == t.at {
			mem.Write32(heap+bhTail, s.at)
		}
		t.set(trNext, s.at)
	}
	mem.Fill(at, 0, size)
	return true
}

// blockAlloc is MEMAllocFromBlockHeapEx: the first free span that fits,
// from the low end, or from the high end for a negative alignment. Only
// that span is tried.
func (m *Machine) blockAlloc(heap, size uint32, align int32) uint32 {
	if !m.isBlockHeap(heap) || size == 0 {
		return 0
	}
	if align == 0 {
		align = 4
	}
	high := align < 0
	mask := uint64(absAlign(align)) - 1
	dir, at := uint32(trNext), m.Mem.Read32(heap+bhHead)
	if high {
		dir, at = trPrev, m.Mem.Read32(heap+bhTail)
	}
	for ; at != 0; at = m.track(at).get(dir) {
		t := m.track(at)
		if !t.free() {
			continue
		}
		first, last := t.span()
		from, to := uint64(first), uint64(last)+1
		var p uint64
		if high {
			if to < uint64(size) {
				continue
			}
			if p = (to - uint64(size)) &^ mask; p < from {
				continue
			}
		} else {
			if p = (from + mask) &^ mask; p >= to || to-p < uint64(size) {
				continue
			}
		}
		if m.carve(heap, t, uint32(p), size) {
			return uint32(p)
		}
		return 0
	}
	return 0
}

// blockAllocAt is MEMAllocFromBlockHeapAt: exactly at..at+size-1.
func (m *Machine) blockAllocAt(heap, at, size uint32) uint32 {
	if !m.isBlockHeap(heap) || size == 0 {
		return 0
	}
	for a := m.Mem.Read32(heap + bhHead); a != 0; a = m.track(a).get(trNext) {
		t := m.track(a)
		first, last := t.span()
		if at < first || at > last {
			continue
		}
		if t.free() && uint64(at)+uint64(size)-1 <= uint64(last) && m.carve(heap, t, at, size) {
			return at
		}
		return 0
	}
	return 0
}

// trackAt is the track whose span starts at p, or 0.
func (m *Machine) trackAt(heap, p uint32) uint32 {
	mem := m.Mem
	if p < mem.Read32(heap+heapStart) || p > mem.Read32(heap+heapEnd) {
		return 0
	}
	for a := mem.Read32(heap + bhHead); a != 0; a = m.track(a).get(trNext) {
		if m.track(a).get(trFirst) == p {
			return a
		}
	}
	return 0
}

// absorb merges the free track after into the free track t, retiring after.
func (m *Machine) absorb(heap uint32, t, after track) {
	next := after.get(trNext)
	t.set(trLast, after.get(trLast))
	t.set(trNext, next)
	if next != 0 {
		m.track(next).set(trPrev, t.at)
	}
	if m.Mem.Read32(heap+bhTail) == after.at {
		m.Mem.Write32(heap+bhTail, t.at)
	}
	m.retireTrack(heap, after)
}

// blockFree is MEMFreeToBlockHeap: the span joins free neighbours.
func (m *Machine) blockFree(heap, p uint32) {
	if !m.isBlockHeap(heap) {
		return
	}
	a := m.trackAt(heap, p)
	if a == 0 || m.track(a).free() {
		return
	}
	t := m.track(a)
	t.set(trIsFree, 1)
	if prev := t.get(trPrev); prev != 0 && m.track(prev).free() {
		m.absorb(heap, m.track(prev), t)
		t = m.track(prev)
	}
	if next := t.get(trNext); next != 0 && m.track(next).free() {
		m.absorb(heap, t, m.track(next))
	}
}

// freeSpans calls fn with each free span's first and last byte.
func (m *Machine) freeSpans(heap uint32, fn func(first, last uint32)) {
	for a := m.Mem.Read32(heap + bhHead); a != 0; a = m.track(a).get(trNext) {
		if t := m.track(a); t.free() {
			fn(t.span())
		}
	}
}

func init() {
	c := "coreinit"
	blk := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc { return onHeap(heapMagicBlock, fn) }

	reg(c, "MEMInitBlockHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.initBlockHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5)))
	})
	reg(c, "MEMDestroyBlockHeap", blk(func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		m.Mem.Fill(heap, 0, bhHeapSize)
		ret(cpu, heap)
	}))
	reg(c, "MEMAddBlockHeapTracking", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, uint32(m.addBlockTracks(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))))
	})
	reg(c, "MEMGetTrackingLeftInBlockHeap", blk(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.Mem.Read32(heap+bhSpareCount))
	}))
	reg(c, "MEMAllocFromBlockHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.blockAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMAllocFromBlockHeapAt", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.blockAllocAt(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)))
	})
	reg(c, "MEMFreeToBlockHeap", func(m *Machine, cpu *CPU, t *Thread) { m.blockFree(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMGetAllocatableSizeForBlockHeapEx", blk(func(m *Machine, cpu *CPU, heap uint32) {
		align := absAlign(int32(arg(cpu, 1)))
		if align == 0 {
			align = 4
		}
		al := uint64(align)
		var best uint32
		m.freeSpans(heap, func(first, last uint32) {
			if at := (uint64(first) + al - 1) / al * al; at <= uint64(last) {
				best = max(best, uint32(uint64(last)+1-at))
			}
		})
		ret(cpu, best)
	}))
	reg(c, "MEMGetTotalFreeSizeForBlockHeap", blk(func(m *Machine, cpu *CPU, heap uint32) {
		var n uint32
		m.freeSpans(heap, func(first, last uint32) { n += last - first + 1 })
		ret(cpu, n)
	}))
	reg(c, "MEMGetSizeForMBlockBlockHeap", blk(func(m *Machine, cpu *CPU, heap uint32) {
		var n uint32
		if a := m.trackAt(heap, arg(cpu, 1)); a != 0 && !m.track(a).free() {
			first, last := m.track(a).span()
			n = last - first + 1
		}
		ret(cpu, n)
	}))

	reg(c, "MEMInitAllocatorForBlockHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Block", arg(cpu, 1), arg(cpu, 2))
	})
	allocatorFor("Block", func(m *Machine, heap, size uint32, align int32) uint32 { return m.blockAlloc(heap, size, align) },
		func(m *Machine, heap, p uint32) { m.blockFree(heap, p) })
}
