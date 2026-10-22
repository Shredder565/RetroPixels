// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/
// coreinit_MEM_BlockHeap.cpp and coreinit_MEM_BlockHeap.h, rewritten in Go for
// RetroPixels.

package wiiu

// Block heaps: the program gives the header, the range to manage and memory
// for "tracks", one per free or used span, kept in address order. Spare
// tracks wait in a list of their own; an allocation that splits a span needs
// one or two.

const (
	// MEMBlockHeap, after MEMHeapBase
	blkTrack     = 0x50 // the first track, in the header
	blkHeadTrack = 0x64
	blkTailTrack = 0x68
	blkSpare     = 0x6c // spare tracks, linked by their next field
	blkSpareLeft = 0x70
	blkHeapSize  = 0x80

	// a track
	trkStart = 0x00
	trkEnd   = 0x04 // the last byte
	trkFree  = 0x08
	trkPrev  = 0x0c
	trkNext  = 0x10
	trkSize  = 0x14

	blkGroupHeader = 0x10 // before the tracks in memory given for them
)

func (m *Machine) isBlockHeap(heap uint32) bool {
	return heap != 0 && m.Mem.Read32(heap+heapMagic) == heapMagicBlock
}

// initBlockHeap is MEMInitBlockHeap.
func (m *Machine) initBlockHeap(heap, start, end, track, trackSize, flags uint32) uint32 {
	if heap == 0 || start == 0 || end == 0 || start >= end {
		return 0
	}
	if trackSize == 0 {
		track = 0
	} else if track == 0 {
		trackSize = 0
	}
	m.memInitHeapBase(heap, heapMagicBlock, start, end, flags)
	mem := m.Mem
	mem.Fill(heap+heapBaseSize, 0, blkHeapSize-heapBaseSize)
	t := heap + blkTrack
	mem.Write32(t+trkStart, start)
	mem.Write32(t+trkEnd, end-1)
	mem.Write32(t+trkFree, 1)
	mem.Write32(heap+blkHeadTrack, t)
	mem.Write32(heap+blkTailTrack, t)
	if trackSize != 0 && m.blockAddTracking(heap, track, trackSize) != 0 {
		m.memDestroyHeapBase(heap)
		return 0
	}
	return heap
}

func (m *Machine) blockAddTracking(heap, track, size uint32) int32 {
	mem := m.Mem
	if !m.isBlockHeap(heap) {
		return 0
	}
	if track == 0 || size <= blkGroupHeader+trkSize {
		return -4
	}
	n := (size - blkGroupHeader) / trkSize
	mem.Write32(track, 0)
	mem.Write32(track+4, 0)
	mem.Write32(track+8, track+blkGroupHeader)
	mem.Write32(track+12, n)
	first := track + blkGroupHeader
	mem.Write32(first+trkPrev, 0)
	for i := uint32(0); i+1 < n; i++ {
		t := first + i*trkSize
		mem.Write32(t+trkNext, t+trkSize)
		mem.Write32(t+trkSize+trkPrev, 0)
	}
	mem.Write32(first+(n-1)*trkSize+trkNext, mem.Read32(heap+blkSpare))
	mem.Write32(heap+blkSpare, first)
	mem.Write32(heap+blkSpareLeft, mem.Read32(heap+blkSpareLeft)+n)
	return 0
}

func (m *Machine) blockTakeSpare(heap uint32) uint32 {
	mem := m.Mem
	t := mem.Read32(heap + blkSpare)
	mem.Write32(heap+blkSpare, mem.Read32(t+trkNext))
	mem.Write32(t+trkNext, 0)
	mem.Write32(heap+blkSpareLeft, mem.Read32(heap+blkSpareLeft)-1)
	return t
}

func (m *Machine) blockReleaseSpare(heap, t uint32) {
	mem := m.Mem
	mem.Write32(t+trkNext, mem.Read32(heap+blkSpare))
	mem.Write32(heap+blkSpare, t)
	mem.Write32(heap+blkSpareLeft, mem.Read32(heap+blkSpareLeft)+1)
}

// blockAllocAt takes at..at+size-1 out of free track t, splitting off what's
// left before and after; it fails without the spare tracks for that.
func (m *Machine) blockAllocAt(heap, t, at, size uint32) bool {
	mem := m.Mem
	start, end := mem.Read32(t+trkStart), mem.Read32(t+trkEnd)
	prefix, suffix := at-start, end+1-(at+size)
	need := uint32(0)
	if prefix > 0 {
		need++
	}
	if suffix > 0 {
		need++
	}
	if mem.Read32(heap+blkSpareLeft) < need {
		return false
	}
	prev := mem.Read32(t + trkPrev)
	if prefix > 0 {
		p := m.blockTakeSpare(heap)
		mem.Write32(p+trkFree, 1)
		mem.Write32(p+trkStart, start)
		mem.Write32(p+trkEnd, at-1)
		if mem.Read32(heap+blkHeadTrack) == t {
			mem.Write32(heap+blkHeadTrack, p)
		}
		if prev != 0 {
			mem.Write32(prev+trkNext, p)
		}
		mem.Write32(p+trkNext, t)
		mem.Write32(p+trkPrev, prev)
		prev = p
	}
	mem.Write32(t+trkFree, 0)
	mem.Write32(t+trkStart, at)
	mem.Write32(t+trkEnd, at+size-1)
	mem.Write32(t+trkPrev, prev)
	if suffix > 0 {
		s := m.blockTakeSpare(heap)
		next := mem.Read32(t + trkNext)
		mem.Write32(s+trkFree, 1)
		mem.Write32(s+trkStart, at+size)
		mem.Write32(s+trkEnd, end)
		mem.Write32(s+trkPrev, t)
		mem.Write32(s+trkNext, next)
		if mem.Read32(heap+blkTailTrack) == t {
			mem.Write32(heap+blkTailTrack, s)
		}
		if next != 0 {
			mem.Write32(next+trkPrev, s)
		}
		mem.Write32(t+trkNext, s)
	}
	mem.Fill(at, 0, size)
	return true
}

// blockAlloc is MEMAllocFromBlockHeapEx: a negative alignment allocates from
// the back.
func (m *Machine) blockAlloc(heap, size uint32, align int32) uint32 {
	mem := m.Mem
	if !m.isBlockHeap(heap) || size == 0 {
		return 0
	}
	if align == 0 {
		align = 4
	}
	fromTail := align < 0
	if fromTail {
		align = -align
	}
	al := uint64(align)
	t := mem.Read32(heap + blkHeadTrack)
	if fromTail {
		t = mem.Read32(heap + blkTailTrack)
	}
	for t != 0 {
		if mem.Read32(t+trkFree) != 0 {
			start, end := uint64(mem.Read32(t+trkStart)), uint64(mem.Read32(t+trkEnd))+1
			if !fromTail {
				if a := (start + al - 1) &^ (al - 1); a < end && end-a >= uint64(size) {
					if m.blockAllocAt(heap, t, uint32(a), size) {
						return uint32(a)
					}
					return 0
				}
			} else if end >= uint64(size) {
				if a := (end - uint64(size)) &^ (al - 1); a >= start {
					if m.blockAllocAt(heap, t, uint32(a), size) {
						return uint32(a)
					}
					return 0
				}
			}
		}
		if fromTail {
			t = mem.Read32(t + trkPrev)
		} else {
			t = mem.Read32(t + trkNext)
		}
	}
	return 0
}

// blockAllocFixed is MEMAllocFromBlockHeapAt: exactly at..at+size-1.
func (m *Machine) blockAllocFixed(heap, at, size uint32) uint32 {
	mem := m.Mem
	if !m.isBlockHeap(heap) || size == 0 {
		return 0
	}
	for t := mem.Read32(heap + blkHeadTrack); t != 0; t = mem.Read32(t + trkNext) {
		start, end := mem.Read32(t+trkStart), mem.Read32(t+trkEnd)
		if at < start || at > end {
			continue
		}
		if mem.Read32(t+trkFree) == 0 || uint64(at)+uint64(size)-1 > uint64(end) {
			return 0
		}
		if m.blockAllocAt(heap, t, at, size) {
			return at
		}
		return 0
	}
	return 0
}

func (m *Machine) blockFind(heap, a uint32) uint32 {
	mem := m.Mem
	if a < mem.Read32(heap+heapStart) || a > mem.Read32(heap+heapEnd) {
		return 0
	}
	for t := mem.Read32(heap + blkHeadTrack); t != 0; t = mem.Read32(t + trkNext) {
		if mem.Read32(t+trkStart) == a {
			return t
		}
	}
	return 0
}

// blockFree is MEMFreeToBlockHeap: the span is merged with free neighbours.
func (m *Machine) blockFree(heap, a uint32) {
	mem := m.Mem
	if !m.isBlockHeap(heap) {
		return
	}
	t := m.blockFind(heap, a)
	if t == 0 || mem.Read32(t+trkFree) != 0 {
		return
	}
	mem.Write32(t+trkFree, 1)
	if prev := mem.Read32(t + trkPrev); prev != 0 && mem.Read32(prev+trkFree) != 0 {
		next := mem.Read32(t + trkNext)
		mem.Write32(prev+trkEnd, mem.Read32(t+trkEnd))
		mem.Write32(prev+trkNext, next)
		if next != 0 {
			mem.Write32(next+trkPrev, prev)
		}
		if mem.Read32(heap+blkTailTrack) == t {
			mem.Write32(heap+blkTailTrack, prev)
		}
		m.blockReleaseSpare(heap, t)
		t = prev
	}
	if next := mem.Read32(t + trkNext); next != 0 && mem.Read32(next+trkFree) != 0 {
		after := mem.Read32(next + trkNext)
		mem.Write32(t+trkEnd, mem.Read32(next+trkEnd))
		mem.Write32(t+trkNext, after)
		if after != 0 {
			mem.Write32(after+trkPrev, t)
		}
		if mem.Read32(heap+blkTailTrack) == next {
			mem.Write32(heap+blkTailTrack, t)
		}
		m.blockReleaseSpare(heap, next)
	}
}

func (m *Machine) blockAllocatable(heap uint32, align int32) uint32 {
	mem := m.Mem
	if align < 0 {
		align = -align
	} else if align == 0 {
		align = 4
	}
	al := uint64(align)
	var best uint32
	for t := mem.Read32(heap + blkHeadTrack); t != 0; t = mem.Read32(t + trkNext) {
		if mem.Read32(t+trkFree) == 0 {
			continue
		}
		start, end := uint64(mem.Read32(t+trkStart)), uint64(mem.Read32(t+trkEnd))
		if a := (start + al - 1) / al * al; a <= end {
			best = max(best, uint32(end+1-a))
		}
	}
	return best
}

func (m *Machine) blockTotalFree(heap uint32) uint32 {
	mem := m.Mem
	var n uint32
	for t := mem.Read32(heap + blkHeadTrack); t != 0; t = mem.Read32(t + trkNext) {
		if mem.Read32(t+trkFree) != 0 {
			n += mem.Read32(t+trkEnd) - mem.Read32(t+trkStart) + 1
		}
	}
	return n
}

func init() {
	c := "coreinit"
	blockHeap := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			if h := arg(cpu, 0); m.isBlockHeap(h) {
				fn(m, cpu, h)
				return
			}
			ret(cpu, 0)
		}
	}

	// MEMInitBlockHeap(heap, start, end, track memory, its size, flags)
	reg(c, "MEMInitBlockHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.initBlockHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5)))
	})
	reg(c, "MEMDestroyBlockHeap", blockHeap(func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		m.Mem.Fill(heap, 0, blkHeapSize)
		ret(cpu, heap)
	}))
	reg(c, "MEMAddBlockHeapTracking", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, uint32(m.blockAddTracking(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))))
	})
	reg(c, "MEMGetTrackingLeftInBlockHeap", blockHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.Mem.Read32(heap+blkSpareLeft))
	}))
	reg(c, "MEMAllocFromBlockHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.blockAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMAllocFromBlockHeapAt", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.blockAllocFixed(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)))
	})
	reg(c, "MEMFreeToBlockHeap", func(m *Machine, cpu *CPU, t *Thread) { m.blockFree(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMGetAllocatableSizeForBlockHeapEx", blockHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.blockAllocatable(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMGetTotalFreeSizeForBlockHeap", blockHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.blockTotalFree(heap))
	}))
	reg(c, "MEMGetSizeForMBlockBlockHeap", blockHeap(func(m *Machine, cpu *CPU, heap uint32) {
		if t := m.blockFind(heap, arg(cpu, 1)); t != 0 && m.Mem.Read32(t+trkFree) == 0 {
			ret(cpu, m.Mem.Read32(t+trkEnd)-m.Mem.Read32(t+trkStart)+1)
			return
		}
		ret(cpu, 0)
	}))

	reg(c, "MEMInitAllocatorForBlockHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Block", arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "__RPAllocatorBlockAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		ret(cpu, m.blockAlloc(m.Mem.Read32(a+allocatorHeap), arg(cpu, 1), int32(m.Mem.Read32(a+allocatorParam))))
	})
	reg(c, "__RPAllocatorBlockFree", func(m *Machine, cpu *CPU, t *Thread) {
		m.blockFree(m.Mem.Read32(arg(cpu, 0)+allocatorHeap), arg(cpu, 1))
	})
}
