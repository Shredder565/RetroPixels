package wiiu

// Frame heaps: memory handed out from both ends towards the middle, and
// only ever given back in bulk (one end or both) or by going back to a
// state recorded earlier. Recorded states live in the heap itself.
//
// Written for RetroPixels from the SDK's structure layouts and the
// behaviour programs rely on.

const (
	// after MEMHeapBase
	frmLow      = 0x40 // where the free middle starts
	frmHigh     = 0x44 // where it ends
	frmLatest   = 0x48 // the newest recorded state
	frmHeapSize = 0x4c

	// a recorded state: its id, both ends, the state before it
	stateID   = 0x0
	stateLow  = 0x4
	stateHigh = 0x8
	statePrev = 0xc
	stateSize = 0x10

	frmFreeLow  = 1
	frmFreeHigh = 2
)

func (m *Machine) isFrmHeap(heap uint32) bool { return m.heapKind(heap, heapMagicFrame) }

// createFrmHeap is MEMCreateFrmHeapEx.
func (m *Machine) createFrmHeap(start, size, flags uint32) uint32 {
	from := (uint64(start) + 3) &^ 3
	to := (uint64(start) + uint64(size)) &^ 3
	if start == 0 || from > to || to-from < frmHeapSize {
		return 0
	}
	heap := uint32(from)
	m.memInitHeapBase(heap, heapMagicFrame, heap+frmHeapSize, uint32(to), flags)
	m.Mem.Fill(heap+heapBaseSize, 0, frmHeapSize-heapBaseSize)
	m.Mem.Write32(heap+frmLow, heap+frmHeapSize)
	m.Mem.Write32(heap+frmHigh, uint32(to))
	return heap
}

func (m *Machine) frmClear(heap, p, n uint32) {
	if m.heapFlagsOf(heap)&heapOptClear != 0 {
		m.Mem.Fill(p, 0, n)
	}
}

// frmTakeLow hands out size bytes from the low end.
func (m *Machine) frmTakeLow(heap, size, align uint32) uint32 {
	mem := m.Mem
	low, high := mem.Read32(heap+frmLow), mem.Read32(heap+frmHigh)
	at := (uint64(low) + uint64(align) - 1) &^ (uint64(align) - 1)
	if at+uint64(size) > uint64(high) {
		return 0
	}
	mem.Write32(heap+frmLow, uint32(at)+size)
	m.frmClear(heap, low, uint32(at)-low+size)
	return uint32(at)
}

// frmTakeHigh hands out size bytes from the high end.
func (m *Machine) frmTakeHigh(heap, size, align uint32) uint32 {
	mem := m.Mem
	low, high := mem.Read32(heap+frmLow), mem.Read32(heap+frmHigh)
	if size > high {
		return 0
	}
	at := (high - size) &^ (align - 1)
	if at < low {
		return 0
	}
	mem.Write32(heap+frmHigh, at)
	m.frmClear(heap, at, high-at)
	return at
}

// frmAlloc is MEMAllocFromFrmHeapEx; a negative alignment takes from the
// high end.
func (m *Machine) frmAlloc(heap, size uint32, align int32) uint32 {
	if !m.isFrmHeap(heap) || size > 0xfffffffc {
		return 0
	}
	size = (max(size, 4) + 3) &^ 3
	if align < 0 {
		return m.frmTakeHigh(heap, size, uint32(-align))
	}
	return m.frmTakeLow(heap, size, uint32(max(align, 1)))
}

// frmFree is MEMFreeToFrmHeap: everything from one end or both, recorded
// states included.
func (m *Machine) frmFree(heap, ends uint32) {
	mem := m.Mem
	for _, e := range [2]struct{ bit, field, reset uint32 }{
		{frmFreeLow, frmLow, heapStart}, {frmFreeHigh, frmHigh, heapEnd},
	} {
		if ends&e.bit != 0 {
			mem.Write32(heap+frmLatest, 0)
			mem.Write32(heap+e.field, mem.Read32(heap+e.reset))
		}
	}
}

// frmAllocatable is MEMGetAllocatableSizeForFrmHeapEx: both ends share one
// free middle, so the sign of the alignment makes no difference.
func (m *Machine) frmAllocatable(heap uint32, align int32) uint32 {
	al := uint64(absAlign(align))
	if al == 0 || al&(al-1) != 0 {
		return 0
	}
	low, high := uint64(m.Mem.Read32(heap+frmLow)), uint64(m.Mem.Read32(heap+frmHigh))
	at := (low + al - 1) &^ (al - 1)
	if at > high {
		return 0
	}
	return uint32(high - at)
}

// frmRecord is MEMRecordStateForFrmHeap: the state takes a little memory
// from the low end itself.
func (m *Machine) frmRecord(heap, id uint32) bool {
	mem := m.Mem
	low, high := mem.Read32(heap+frmLow), mem.Read32(heap+frmHigh)
	s := m.frmTakeLow(heap, stateSize, 4)
	if s == 0 {
		return false
	}
	mem.Write32(s+stateID, id)
	mem.Write32(s+stateLow, low)
	mem.Write32(s+stateHigh, high)
	mem.Write32(s+statePrev, mem.Read32(heap+frmLatest))
	mem.Write32(heap+frmLatest, s)
	return true
}

// frmRestore is MEMFreeByStateToFrmHeap: back to the state recorded with id,
// or to the newest when id is 0.
func (m *Machine) frmRestore(heap, id uint32) bool {
	mem := m.Mem
	s := mem.Read32(heap + frmLatest)
	if id != 0 {
		for s != 0 && mem.Read32(s+stateID) != id {
			s = mem.Read32(s + statePrev)
		}
	}
	if s == 0 {
		return false
	}
	mem.Write32(heap+frmLow, mem.Read32(s+stateLow))
	mem.Write32(heap+frmHigh, mem.Read32(s+stateHigh))
	mem.Write32(heap+frmLatest, mem.Read32(s+statePrev))
	return true
}

// frmAdjust is MEMAdjustFrmHeap: with nothing taken from the high end, the
// heap is cut back to its low end. It returns the new size, or 0.
func (m *Machine) frmAdjust(heap uint32) uint32 {
	mem := m.Mem
	if mem.Read32(heap+frmHigh) != mem.Read32(heap+heapEnd) {
		return 0
	}
	low := mem.Read32(heap + frmLow)
	mem.Write32(heap+heapEnd, low)
	mem.Write32(heap+frmHigh, low)
	return low - heap
}

// frmResize is MEMResizeForMBlockFrmHeap: only the newest block from the low
// end, recorded after any state, can change size. It returns the new size,
// or 0.
func (m *Machine) frmResize(heap, p, size uint32) uint32 {
	mem := m.Mem
	low, high := mem.Read32(heap+frmLow), mem.Read32(heap+frmHigh)
	if p < mem.Read32(heap+heapStart) || p >= low {
		return 0
	}
	if s := mem.Read32(heap + frmLatest); s != 0 && mem.Read32(s+stateLow) > p {
		return 0
	}
	size = (size + 3) &^ 3
	if uint64(p)+uint64(size) > uint64(high) {
		return 0
	}
	mem.Write32(heap+frmLow, p+size)
	if size > 0 && p+size > low {
		m.frmClear(heap, low, p+size-low)
	}
	return size
}

func init() {
	c := "coreinit"
	frm := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc { return onHeap(heapMagicFrame, fn) }

	reg(c, "MEMCreateFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createFrmHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)))
	})
	reg(c, "MEMDestroyFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		ret(cpu, heap)
	}))
	reg(c, "MEMAllocFromFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.frmAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) { m.frmFree(heap, arg(cpu, 1)) }))
	reg(c, "MEMGetAllocatableSizeForFrmHeapEx", frm(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.frmAllocatable(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMiGetFreeStartForFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.Mem.Read32(heap+frmLow)) }))
	reg(c, "MEMiGetFreeEndForFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.Mem.Read32(heap+frmHigh)) }))
	reg(c, "MEMRecordStateForFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) {
		retBool(cpu, m.frmRecord(heap, arg(cpu, 1)))
	}))
	reg(c, "MEMFreeByStateToFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) {
		retBool(cpu, m.frmRestore(heap, arg(cpu, 1)))
	}))
	reg(c, "MEMAdjustFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.frmAdjust(heap)) }))
	reg(c, "MEMResizeForMBlockFrmHeap", frm(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.frmResize(heap, arg(cpu, 1), arg(cpu, 2)))
	}))

	// an allocator over a frame heap can't give single blocks back
	reg(c, "MEMInitAllocatorForFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Frm", arg(cpu, 1), arg(cpu, 2))
	})
	allocatorFor("Frm", func(m *Machine, heap, size uint32, align int32) uint32 { return m.frmAlloc(heap, size, align) },
		func(*Machine, uint32, uint32) {})
}
