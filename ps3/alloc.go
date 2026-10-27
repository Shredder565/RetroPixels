package ps3

import "sort"

// allocator hands out ranges of an address region (first fit).
type allocator struct {
	base, size uint32
	used       map[uint32]uint32 // start -> size
}

func newAllocator(base, size uint32) *allocator {
	return &allocator{base: base, size: size, used: map[uint32]uint32{}}
}

func (a *allocator) alloc(size, align uint32) uint32 {
	if align == 0 {
		align = 0x10000
	}
	size = (size + align - 1) &^ (align - 1)
	starts := make([]uint32, 0, len(a.used))
	for s := range a.used {
		starts = append(starts, s)
	}
	sort.Slice(starts, func(i, j int) bool { return starts[i] < starts[j] })
	p := a.base
	for _, s := range starts {
		if p+size <= s {
			break
		}
		if e := s + a.used[s]; e > p {
			p = (e + align - 1) &^ (align - 1)
		}
	}
	if uint64(p)+uint64(size) > uint64(a.base)+uint64(a.size) {
		return 0
	}
	a.used[p] = size
	return p
}

func (a *allocator) free(addr uint32) (uint32, bool) {
	s, ok := a.used[addr]
	delete(a.used, addr)
	return s, ok
}

func (a *allocator) inUse() uint32 {
	var n uint32
	for _, s := range a.used {
		n += s
	}
	return n
}
