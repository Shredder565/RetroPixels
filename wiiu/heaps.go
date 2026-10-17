package wiiu

// Memory heaps (the MEM* functions of coreinit).
//
// Cafe OS gives a program three kinds of heap: expanded heaps (malloc-like,
// allocating from the front or the back), frame heaps (a stack from each end,
// freed all at once or back to a recorded state) and unit heaps (blocks of
// one size). The program's base heaps are an expanded heap over MEM2 (the
// "default heap") and frame heaps over MEM1 and the foreground bucket.
//
// The bookkeeping lives here rather than in the program's memory; a heap's
// handle is its start address, where a small header with its tag goes.

import (
	"sort"
)

const (
	arenaMEM1 = 0
	arenaMEM2 = 1
	arenaFG   = 8

	heapHeader = 0x40

	tagExp   = 0x45585048 // "EXPH"
	tagFrm   = 0x46524d48 // "FRMH"
	tagUnit  = 0x554e5448 // "UNTH"
	tagBlock = 0x424c4b48 // "BLKH"

	systemHeapSize = 0x01000000
)

type span struct{ Addr, Size uint32 }

// Heap is one heap.
type Heap struct {
	Kind  uint32 // tagExp, tagFrm, tagUnit
	Start uint32 // the handle
	Lo    uint32 // first usable byte
	Hi    uint32 // end
	Flags uint32

	// expanded heap
	Free []span            // sorted by address
	Used map[uint32]span   // by the address handed out: the span it took
	Size map[uint32]uint32 // the size asked for

	// frame heap
	Head, Tail uint32
	States     []frmState

	// block heap (an expanded heap whose bookkeeping slots the program
	// supplies): how many slots it gave
	Block    bool
	Tracking uint32

	// unit heap
	Unit     uint32
	FreeUnit []uint32
}

type frmState struct{ ID, Head, Tail uint32 }

// Heaps are a program's heaps.
type Heaps struct {
	ByHandle map[uint32]*Heap
	Base     map[uint32]uint32 // arena -> heap handle
	System   uint32            // the heap RetroPixels takes thread stacks from
}

func newHeaps() *Heaps {
	return &Heaps{ByHandle: map[uint32]*Heap{}, Base: map[uint32]uint32{}}
}

func (h *Heaps) setup(m *Machine) {
	start := (m.dataEnd + 0xffff) &^ 0xffff
	if start < DataBase {
		start = DataBase
	}
	sysStart := uint32(MEM2End - systemHeapSize)
	h.System = h.createExp(m, sysStart, systemHeapSize, 0)
	h.Base[arenaMEM2] = h.createExp(m, start, sysStart-start, 0)
	h.Base[arenaMEM1] = h.createFrm(m, MEM1Base, MEM1Size, 0)
	h.Base[arenaFG] = h.createFrm(m, FGBase, FGSize, 0)
}

func (h *Heaps) allocSystem(m *Machine, size, align uint32) uint32 {
	a := h.get(h.System).expAlloc(size, int32(align))
	if a == 0 {
		panic("wiiu: out of system memory")
	}
	return a
}

func (h *Heaps) get(handle uint32) *Heap { return h.ByHandle[handle] }

func (h *Heaps) createExp(m *Machine, start, size, flags uint32) uint32 {
	start = (start + 3) &^ 3
	hp := &Heap{Kind: tagExp, Start: start, Lo: start + heapHeader, Hi: start + size, Flags: flags,
		Used: map[uint32]span{}, Size: map[uint32]uint32{}}
	if hp.Hi > hp.Lo {
		hp.Free = []span{{hp.Lo, hp.Hi - hp.Lo}}
	}
	h.ByHandle[start] = hp
	m.Mem.Write32(start, tagExp)
	return start
}

func (h *Heaps) createFrm(m *Machine, start, size, flags uint32) uint32 {
	start = (start + 3) &^ 3
	hp := &Heap{Kind: tagFrm, Start: start, Lo: start + heapHeader, Hi: start + size, Flags: flags}
	hp.Head, hp.Tail = hp.Lo, hp.Hi
	h.ByHandle[start] = hp
	m.Mem.Write32(start, tagFrm)
	return start
}

func (h *Heaps) createUnit(m *Machine, start, size, unit, align, flags uint32) uint32 {
	start = (start + 3) &^ 3
	if align < 4 {
		align = 4
	}
	unit = (unit + align - 1) &^ (align - 1)
	hp := &Heap{Kind: tagUnit, Start: start, Lo: start + heapHeader, Hi: start + size, Flags: flags, Unit: unit}
	first := (hp.Lo + align - 1) &^ (align - 1)
	for a := first; unit > 0 && a+unit <= hp.Hi; a += unit {
		hp.FreeUnit = append(hp.FreeUnit, a)
	}
	h.ByHandle[start] = hp
	m.Mem.Write32(start, tagUnit)
	return start
}

func (h *Heaps) destroy(m *Machine, handle uint32) {
	delete(h.ByHandle, handle)
	m.Mem.Write32(handle, 0)
}

// heapFor finds the heap an address was allocated from.
func (h *Heaps) heapFor(addr uint32) *Heap {
	var best *Heap
	for _, hp := range h.ByHandle {
		if addr >= hp.Lo && addr < hp.Hi && (best == nil || hp.Hi-hp.Lo < best.Hi-best.Lo) {
			best = hp
		}
	}
	return best
}

// expAlloc: align > 0 from the front, < 0 from the back
func (hp *Heap) expAlloc(size uint32, align int32) uint32 {
	if hp == nil || hp.Kind != tagExp {
		return 0
	}
	fromBack := align < 0
	al := uint32(align)
	if fromBack {
		al = uint32(-align)
	}
	if al < 4 {
		al = 4
	}
	if size == 0 {
		size = 1
	}
	size = (size + 3) &^ 3
	if fromBack {
		for i := len(hp.Free) - 1; i >= 0; i-- {
			f := hp.Free[i]
			if f.Size < size {
				continue
			}
			a := (f.Addr + f.Size - size) &^ (al - 1)
			if a < f.Addr {
				continue
			}
			hp.take(i, a, size)
			return a
		}
		return 0
	}
	for i, f := range hp.Free {
		a := (f.Addr + al - 1) &^ (al - 1)
		if a < f.Addr || a+size > f.Addr+f.Size {
			continue
		}
		hp.take(i, a, size)
		return a
	}
	return 0
}

// take cuts a..a+size out of free span i
func (hp *Heap) take(i int, a, size uint32) {
	f := hp.Free[i]
	var rest []span
	if a > f.Addr {
		rest = append(rest, span{f.Addr, a - f.Addr})
	}
	if end := a + size; end < f.Addr+f.Size {
		rest = append(rest, span{end, f.Addr + f.Size - end})
	}
	hp.Free = append(hp.Free[:i], append(rest, hp.Free[i+1:]...)...)
	hp.Used[a] = span{a, size}
	hp.Size[a] = size
}

func (hp *Heap) expFree(a uint32) bool {
	if hp == nil || hp.Kind != tagExp {
		return false
	}
	s, ok := hp.Used[a]
	if !ok {
		return false
	}
	delete(hp.Used, a)
	delete(hp.Size, a)
	hp.Free = append(hp.Free, s)
	sort.Slice(hp.Free, func(i, j int) bool { return hp.Free[i].Addr < hp.Free[j].Addr })
	// merge neighbours
	out := hp.Free[:0]
	for _, f := range hp.Free {
		if n := len(out); n > 0 && out[n-1].Addr+out[n-1].Size == f.Addr {
			out[n-1].Size += f.Size
			continue
		}
		out = append(out, f)
	}
	hp.Free = out
	return true
}

// expResize grows or shrinks a block in place; the new size, or 0
func (hp *Heap) expResize(a, size uint32) uint32 {
	s, ok := hp.Used[a]
	if !ok {
		return 0
	}
	size = (size + 3) &^ 3
	if size <= s.Size {
		if size < s.Size {
			hp.Used[a] = span{a, size}
			hp.Free = append(hp.Free, span{a + size, s.Size - size})
			sort.Slice(hp.Free, func(i, j int) bool { return hp.Free[i].Addr < hp.Free[j].Addr })
		}
		hp.Size[a] = size
		return size
	}
	end := a + s.Size
	for i, f := range hp.Free {
		if f.Addr == end && f.Size >= size-s.Size {
			need := size - s.Size
			if f.Size == need {
				hp.Free = append(hp.Free[:i], hp.Free[i+1:]...)
			} else {
				hp.Free[i] = span{f.Addr + need, f.Size - need}
			}
			hp.Used[a] = span{a, size}
			hp.Size[a] = size
			return size
		}
	}
	return 0
}

func (hp *Heap) expLargest(align uint32) uint32 {
	if align < 4 {
		align = 4
	}
	var best uint32
	for _, f := range hp.Free {
		a := (f.Addr + align - 1) &^ (align - 1)
		if a >= f.Addr+f.Size {
			continue
		}
		if n := f.Addr + f.Size - a; n > best {
			best = n
		}
	}
	return best &^ 3
}

func (hp *Heap) expTotalFree() uint32 {
	var n uint32
	for _, f := range hp.Free {
		n += f.Size
	}
	return n
}

func (hp *Heap) frmAlloc(size uint32, align int32) uint32 {
	if hp == nil || hp.Kind != tagFrm {
		return 0
	}
	size = (size + 3) &^ 3
	if align >= 0 {
		al := uint32(align)
		if al < 4 {
			al = 4
		}
		a := (hp.Head + al - 1) &^ (al - 1)
		if a+size > hp.Tail || a+size < a {
			return 0
		}
		hp.Head = a + size
		return a
	}
	al := uint32(-align)
	if al < 4 {
		al = 4
	}
	if hp.Tail < size {
		return 0
	}
	a := (hp.Tail - size) &^ (al - 1)
	if a < hp.Head {
		return 0
	}
	hp.Tail = a
	return a
}

func (hp *Heap) frmFree(mode uint32) {
	if mode&1 != 0 {
		hp.Head = hp.Lo
	}
	if mode&2 != 0 {
		hp.Tail = hp.Hi
	}
	if mode&3 == 3 {
		hp.States = nil
	}
}

func (hp *Heap) frmLargest(align uint32) uint32 {
	if align < 4 {
		align = 4
	}
	a := (hp.Head + align - 1) &^ (align - 1)
	if a >= hp.Tail {
		return 0
	}
	return hp.Tail - a
}

// createBlock makes a block heap over [start, end) whose handle is the
// program's MEMBlockHeap at handle.
func (h *Heaps) createBlock(m *Machine, handle, start, end, tracking, flags uint32) uint32 {
	hp := &Heap{Kind: tagExp, Block: true, Start: handle, Lo: start, Hi: end, Flags: flags, Tracking: tracking,
		Used: map[uint32]span{}, Size: map[uint32]uint32{}}
	if end > start {
		hp.Free = []span{{start, end - start}}
	}
	h.ByHandle[handle] = hp
	m.Mem.Write32(handle, tagBlock)
	return handle
}

// trackingLeft: bookkeeping slots a block heap has free (one per used or
// free span).
func (hp *Heap) trackingLeft() uint32 {
	n := uint32(len(hp.Used) + len(hp.Free))
	if n >= hp.Tracking {
		return 0
	}
	return hp.Tracking - n
}

// expAllocAt takes exactly [a, a+size) if it's free.
func (hp *Heap) expAllocAt(a, size uint32) uint32 {
	size = (size + 3) &^ 3
	for i, f := range hp.Free {
		if a >= f.Addr && a+size <= f.Addr+f.Size {
			hp.take(i, a, size)
			return a
		}
	}
	return 0
}
