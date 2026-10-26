// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// Frame heaps (MEMFrmHeap). The free space is one gap between a "low" mark
// that grows upward and a "high" mark that grows downward. Nothing is freed
// block by block: a whole side is reset at once, or the heap returns to a
// snapshot taken earlier. Snapshots are themselves allocated from the low
// side and chained newest first.

// MEMFrmHeap fields after the common header
const (
	fhLow      = 0x40 // first free byte
	fhHigh     = 0x44 // one past the last free byte
	fhSnapshot = 0x48 // the newest snapshot, or 0
	fhSize     = 0x4c
)

// a snapshot (MEMFrmHeapState)
const (
	snapTag   = 0x0
	snapLow   = 0x4
	snapHigh  = 0x8
	snapOlder = 0xc
	snapBytes = 0x10
)

// MEMFreeToFrmHeap modes
const (
	frmResetLow  = 1
	frmResetHigh = 2
)

func roundUp64(v, a uint64) uint64 { return (v + a - 1) &^ (a - 1) }

// heapSpan word-aligns the range [start, start+size) the way every heap
// creator does, and reports whether at least min bytes remain.
func heapSpan(start, size, min uint32) (lo, hi uint32, ok bool) {
	if start == 0 {
		return 0, 0, false
	}
	l := roundUp64(uint64(start), 4)
	h := (uint64(start) + uint64(size)) &^ 3
	if l > h || h-l < uint64(min) {
		return 0, 0, false
	}
	return uint32(l), uint32(h), true
}

func (m *Machine) isFrmHeap(heap uint32) bool { return m.heapTagIs(heap, tagFrmHeap) }

// createFrmHeap is MEMCreateFrmHeapEx.
func (m *Machine) createFrmHeap(start, size, options uint32) uint32 {
	heap, end, ok := heapSpan(start, size, fhSize)
	if !ok {
		return 0
	}
	m.heapHeaderInit(heap, tagFrmHeap, heap+fhSize, end, options)
	mem := m.Mem
	mem.Write32(heap+fhLow, heap+fhSize)
	mem.Write32(heap+fhHigh, end)
	mem.Write32(heap+fhSnapshot, 0)
	return heap
}

// frmClaim zeroes newly handed-out memory when the heap asks for it.
func (m *Machine) frmClaim(heap, p, n uint32) {
	if m.heapOptions(heap)&memZeroAlloc != 0 && n > 0 {
		m.Mem.Fill(p, 0, n)
	}
}

func (m *Machine) frmTakeLow(heap, size, align uint32) uint32 {
	mem := m.Mem
	low, high := mem.Read32(heap+fhLow), mem.Read32(heap+fhHigh)
	p := roundUp64(uint64(low), uint64(align))
	if p+uint64(size) > uint64(high) {
		return 0
	}
	newLow := uint32(p) + size
	mem.Write32(heap+fhLow, newLow)
	m.frmClaim(heap, low, newLow-low)
	return uint32(p)
}

func (m *Machine) frmTakeHigh(heap, size, align uint32) uint32 {
	mem := m.Mem
	low, high := mem.Read32(heap+fhLow), mem.Read32(heap+fhHigh)
	if size > high {
		return 0
	}
	p := (high - size) &^ (align - 1)
	if p < low {
		return 0
	}
	mem.Write32(heap+fhHigh, p)
	m.frmClaim(heap, p, high-p)
	return p
}

// frmAlloc is MEMAllocFromFrmHeapEx; a negative alignment takes from the
// high side.
func (m *Machine) frmAlloc(heap, size uint32, align int32) uint32 {
	if !m.isFrmHeap(heap) || size > 0xfffffffc {
		return 0
	}
	if size == 0 {
		size = 4
	}
	size = (size + 3) &^ 3
	if align < 0 {
		return m.frmTakeHigh(heap, size, uint32(-align))
	}
	if align == 0 {
		align = 1
	}
	return m.frmTakeLow(heap, size, uint32(align))
}

// frmReset is MEMFreeToFrmHeap: it empties the side(s) named by mode. Any
// snapshot is forgotten.
func (m *Machine) frmReset(heap, mode uint32) {
	mem := m.Mem
	if mode&(frmResetLow|frmResetHigh) != 0 {
		mem.Write32(heap+fhSnapshot, 0)
	}
	if mode&frmResetLow != 0 {
		mem.Write32(heap+fhLow, mem.Read32(heap+hhFirst))
	}
	if mode&frmResetHigh != 0 {
		mem.Write32(heap+fhHigh, mem.Read32(heap+hhLimit))
	}
}

// frmLargest is MEMGetAllocatableSizeForFrmHeapEx.
func (m *Machine) frmLargest(heap uint32, align int32) uint32 {
	a := uint64(align)
	if align < 0 {
		a = uint64(-int64(align))
	}
	if a == 0 || a&(a-1) != 0 {
		return 0
	}
	low := roundUp64(uint64(m.Mem.Read32(heap+fhLow)), a)
	high := uint64(m.Mem.Read32(heap + fhHigh))
	if low > high {
		return 0
	}
	return uint32(high - low)
}

// frmSnapshot is MEMRecordStateForFrmHeap.
func (m *Machine) frmSnapshot(heap, tag uint32) bool {
	mem := m.Mem
	low, high := mem.Read32(heap+fhLow), mem.Read32(heap+fhHigh)
	s := m.frmTakeLow(heap, snapBytes, 4)
	if s == 0 {
		return false
	}
	mem.Write32(s+snapTag, tag)
	mem.Write32(s+snapLow, low)
	mem.Write32(s+snapHigh, high)
	mem.Write32(s+snapOlder, mem.Read32(heap+fhSnapshot))
	mem.Write32(heap+fhSnapshot, s)
	return true
}

// frmRestore is MEMFreeByStateToFrmHeap: back to the snapshot tagged tag,
// or to the newest one when tag is 0.
func (m *Machine) frmRestore(heap, tag uint32) bool {
	mem := m.Mem
	s := mem.Read32(heap + fhSnapshot)
	if tag != 0 {
		for s != 0 && mem.Read32(s+snapTag) != tag {
			s = mem.Read32(s + snapOlder)
		}
	}
	if s == 0 {
		return false
	}
	mem.Write32(heap+fhLow, mem.Read32(s+snapLow))
	mem.Write32(heap+fhHigh, mem.Read32(s+snapHigh))
	mem.Write32(heap+fhSnapshot, mem.Read32(s+snapOlder))
	return true
}

// frmShrink is MEMAdjustFrmHeap: when nothing is taken from the high side,
// the heap is cut off at its low mark. It returns the heap's new size.
func (m *Machine) frmShrink(heap uint32) uint32 {
	mem := m.Mem
	if mem.Read32(heap+fhHigh) != mem.Read32(heap+hhLimit) {
		return 0
	}
	low := mem.Read32(heap + fhLow)
	mem.Write32(heap+hhLimit, low)
	mem.Write32(heap+fhHigh, low)
	return low - heap
}

// frmResizeLast is MEMResizeForMBlockFrmHeap: only the newest low-side
// block (one not under a snapshot) can change size.
func (m *Machine) frmResizeLast(heap, p, size uint32) uint32 {
	mem := m.Mem
	low, high := mem.Read32(heap+fhLow), mem.Read32(heap+fhHigh)
	if p < mem.Read32(heap+hhFirst) || p >= low {
		return 0
	}
	if s := mem.Read32(heap + fhSnapshot); s != 0 && p < mem.Read32(s+snapLow) {
		return 0
	}
	size = (size + 3) &^ 3
	end := uint64(p) + uint64(size)
	if end > uint64(high) {
		return 0
	}
	mem.Write32(heap+fhLow, uint32(end))
	if uint32(end) > low {
		m.frmClaim(heap, low, uint32(end)-low)
	}
	return size
}

func init() {
	c := "coreinit"
	// onFrm runs fn for a valid frame heap in r3 and returns 0 otherwise.
	onFrm := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			heap := arg(cpu, 0)
			if !m.isFrmHeap(heap) {
				ret(cpu, 0)
				return
			}
			fn(m, cpu, heap)
		}
	}

	reg(c, "MEMCreateFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createFrmHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)))
	})
	reg(c, "MEMDestroyFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) {
		m.heapUnlink(heap)
		ret(cpu, heap)
	}))
	reg(c, "MEMAllocFromFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.frmAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) { m.frmReset(heap, arg(cpu, 1)) }))
	reg(c, "MEMGetAllocatableSizeForFrmHeapEx", onFrm(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.frmLargest(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMiGetFreeStartForFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.Mem.Read32(heap+fhLow)) }))
	reg(c, "MEMiGetFreeEndForFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.Mem.Read32(heap+fhHigh)) }))
	reg(c, "MEMRecordStateForFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) {
		retBool(cpu, m.frmSnapshot(heap, arg(cpu, 1)))
	}))
	reg(c, "MEMFreeByStateToFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) {
		retBool(cpu, m.frmRestore(heap, arg(cpu, 1)))
	}))
	reg(c, "MEMAdjustFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.frmShrink(heap)) }))
	reg(c, "MEMResizeForMBlockFrmHeap", onFrm(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.frmResizeLast(heap, arg(cpu, 1), arg(cpu, 2)))
	}))

	// allocators over frame heaps; freeing a single block does nothing
	reg(c, "MEMInitAllocatorForFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.setAllocator(arg(cpu, 0), "Frm", arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "__RPAllocatorFrmAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		heap, align := m.allocatorHeapArg(arg(cpu, 0))
		ret(cpu, m.frmAlloc(heap, arg(cpu, 1), align))
	})
	reg(c, "__RPAllocatorFrmFree", func(m *Machine, cpu *CPU, t *Thread) {})
}
