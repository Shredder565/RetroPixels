// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/
// coreinit_MEM_FrmHeap.cpp and coreinit_MEM_FrmHeap.h, rewritten in Go for
// RetroPixels.

package wiiu

// Frame heaps: a stack growing from each end, freed all at once or back to
// a recorded state. Recorded states are kept in the heap itself.

const (
	// MEMFrmHeap, after MEMHeapBase
	frmHead     = 0x40
	frmTail     = 0x44
	frmStates   = 0x48 // the latest recorded state
	frmHeapSize = 0x4c

	// a recorded state
	frmStateID   = 0x0
	frmStateHead = 0x4
	frmStateTail = 0x8
	frmStatePrev = 0xc
	frmStateSize = 0x10

	frmFreeHead = 1
	frmFreeTail = 2
)

func (m *Machine) isFrmHeap(heap uint32) bool {
	return heap != 0 && m.Mem.Read32(heap+heapMagic) == heapMagicFrame
}

// createFrmHeap is MEMCreateFrmHeapEx.
func (m *Machine) createFrmHeap(start, size, flags uint32) uint32 {
	lo := (uint64(start) + 3) &^ 3
	hi := (uint64(start) + uint64(size)) &^ 3
	if start == 0 || lo > hi || hi-lo < frmHeapSize {
		return 0
	}
	heap, end := uint32(lo), uint32(hi)
	m.memInitHeapBase(heap, heapMagicFrame, heap+frmHeapSize, end, flags)
	mem := m.Mem
	mem.Fill(heap+heapBaseSize, 0, frmHeapSize-heapBaseSize)
	mem.Write32(heap+frmHead, heap+frmHeapSize)
	mem.Write32(heap+frmTail, end)
	return heap
}

func (m *Machine) frmAllocHead(heap, size, align uint32) uint32 {
	mem := m.Mem
	head, tail := uint64(mem.Read32(heap+frmHead)), uint64(mem.Read32(heap+frmTail))
	a := (head + uint64(align) - 1) &^ (uint64(align) - 1)
	if a+uint64(size) > tail {
		return 0
	}
	mem.Write32(heap+frmHead, uint32(a)+size)
	if m.heapFlagsOf(heap)&heapOptClear != 0 {
		mem.Fill(uint32(head), 0, uint32(a-head)+size)
	}
	return uint32(a)
}

func (m *Machine) frmAllocTail(heap, size, align uint32) uint32 {
	mem := m.Mem
	head, tail := mem.Read32(heap+frmHead), mem.Read32(heap+frmTail)
	if size > tail {
		return 0
	}
	a := (tail - size) &^ (align - 1)
	if a < head {
		return 0
	}
	mem.Write32(heap+frmTail, a)
	if m.heapFlagsOf(heap)&heapOptClear != 0 {
		mem.Fill(a, 0, tail-a)
	}
	return a
}

// frmAlloc is MEMAllocFromFrmHeapEx: a negative alignment allocates from the
// back.
func (m *Machine) frmAlloc(heap, size uint32, align int32) uint32 {
	if !m.isFrmHeap(heap) || size > 0xfffffffc {
		return 0
	}
	if size == 0 {
		size = 4
	}
	size = (size + 3) &^ 3
	if align >= 0 {
		return m.frmAllocHead(heap, size, uint32(max(align, 1)))
	}
	return m.frmAllocTail(heap, size, uint32(-align))
}

func (m *Machine) frmFree(heap, mode uint32) {
	mem := m.Mem
	if mode&frmFreeHead != 0 {
		mem.Write32(heap+frmStates, 0)
		mem.Write32(heap+frmHead, mem.Read32(heap+heapStart))
	}
	if mode&frmFreeTail != 0 {
		mem.Write32(heap+frmStates, 0)
		mem.Write32(heap+frmTail, mem.Read32(heap+heapEnd))
	}
}

// frmAllocatable is MEMGetAllocatableSizeForFrmHeapEx (the alignment's sign
// doesn't matter: both ends share the one free gap).
func (m *Machine) frmAllocatable(heap uint32, align int32) uint32 {
	if align < 0 {
		align = -align
	}
	al := uint32(align)
	if al == 0 || al&(al-1) != 0 {
		return 0
	}
	head, tail := uint64(m.Mem.Read32(heap+frmHead)), uint64(m.Mem.Read32(heap+frmTail))
	a := (head + uint64(al) - 1) &^ (uint64(al) - 1)
	if a > tail {
		return 0
	}
	return uint32(tail - a)
}

func (m *Machine) frmRecordState(heap, id uint32) bool {
	mem := m.Mem
	head, tail := mem.Read32(heap+frmHead), mem.Read32(heap+frmTail)
	s := m.frmAllocHead(heap, frmStateSize, 4)
	if s == 0 {
		return false
	}
	mem.Write32(s+frmStateID, id)
	mem.Write32(s+frmStateHead, head)
	mem.Write32(s+frmStateTail, tail)
	mem.Write32(s+frmStatePrev, mem.Read32(heap+frmStates))
	mem.Write32(heap+frmStates, s)
	return true
}

// frmFreeByState goes back to the state recorded with id (0: the latest).
func (m *Machine) frmFreeByState(heap, id uint32) bool {
	mem := m.Mem
	s := mem.Read32(heap + frmStates)
	for s != 0 && id != 0 && mem.Read32(s+frmStateID) != id {
		s = mem.Read32(s + frmStatePrev)
	}
	if s == 0 {
		return false
	}
	mem.Write32(heap+frmHead, mem.Read32(s+frmStateHead))
	mem.Write32(heap+frmTail, mem.Read32(s+frmStateTail))
	mem.Write32(heap+frmStates, mem.Read32(s+frmStatePrev))
	return true
}

// frmAdjust is MEMAdjustFrmHeap: with nothing allocated from the back, the
// heap ends at its head; the heap's new size, or 0.
func (m *Machine) frmAdjust(heap uint32) uint32 {
	mem := m.Mem
	if mem.Read32(heap+frmTail) != mem.Read32(heap+heapEnd) {
		return 0
	}
	head := mem.Read32(heap + frmHead)
	mem.Write32(heap+heapEnd, head)
	mem.Write32(heap+frmTail, head)
	return head - heap
}

// frmResize is MEMResizeForMBlockFrmHeap: the latest block allocated from
// the front can grow or shrink; its new size, or 0.
func (m *Machine) frmResize(heap, p, size uint32) uint32 {
	mem := m.Mem
	head, tail := mem.Read32(heap+frmHead), mem.Read32(heap+frmTail)
	if p < mem.Read32(heap+heapStart) || p >= head {
		return 0
	}
	if s := mem.Read32(heap + frmStates); s != 0 && mem.Read32(s+frmStateHead) > p {
		return 0
	}
	size = (size + 3) &^ 3
	if uint64(p)+uint64(size) > uint64(tail) {
		return 0
	}
	mem.Write32(heap+frmHead, p+size)
	if size > 0 && p+size > head && m.heapFlagsOf(heap)&heapOptClear != 0 {
		mem.Fill(head, 0, p+size-head)
	}
	return size
}

func init() {
	c := "coreinit"
	frmHeap := func(fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			if h := arg(cpu, 0); m.isFrmHeap(h) {
				fn(m, cpu, h)
				return
			}
			ret(cpu, 0)
		}
	}

	reg(c, "MEMCreateFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createFrmHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)))
	})
	reg(c, "MEMDestroyFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		ret(cpu, heap)
	}))
	reg(c, "MEMAllocFromFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.frmAlloc(arg(cpu, 0), arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) { m.frmFree(heap, arg(cpu, 1)) }))
	reg(c, "MEMGetAllocatableSizeForFrmHeapEx", frmHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.frmAllocatable(heap, int32(arg(cpu, 1))))
	}))
	reg(c, "MEMiGetFreeStartForFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.Mem.Read32(heap+frmHead)) }))
	reg(c, "MEMiGetFreeEndForFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.Mem.Read32(heap+frmTail)) }))
	reg(c, "MEMRecordStateForFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) {
		retBool(cpu, m.frmRecordState(heap, arg(cpu, 1)))
	}))
	reg(c, "MEMFreeByStateToFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) {
		retBool(cpu, m.frmFreeByState(heap, arg(cpu, 1)))
	}))
	reg(c, "MEMAdjustFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) { ret(cpu, m.frmAdjust(heap)) }))
	reg(c, "MEMResizeForMBlockFrmHeap", frmHeap(func(m *Machine, cpu *CPU, heap uint32) {
		ret(cpu, m.frmResize(heap, arg(cpu, 1), arg(cpu, 2)))
	}))

	// a frame heap's allocator can't free single blocks
	reg(c, "MEMInitAllocatorForFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Frm", arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "__RPAllocatorFrmAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		ret(cpu, m.frmAlloc(m.Mem.Read32(a+allocatorHeap), arg(cpu, 1), int32(m.Mem.Read32(a+allocatorParam))))
	})
	reg(c, "__RPAllocatorFrmFree", func(m *Machine, cpu *CPU, t *Thread) {})
}
