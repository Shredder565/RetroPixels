// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/
// coreinit_MEM_UnitHeap.cpp and coreinit_MEM_UnitHeap.h, rewritten in Go for
// RetroPixels.

package wiiu

// Unit heaps: blocks of one size, the free ones linked through their first
// word.

const (
	// MEMUnitHeap, after MEMHeapBase
	unitFirstFree = 0x40
	unitBlockSize = 0x44
	unitHeapSize  = 0x48
)

func (m *Machine) isUnitHeap(heap uint32) bool {
	return heap != 0 && m.Mem.Read32(heap+heapMagic) == heapMagicUnit
}

// createUnitHeap is MEMCreateUnitHeapEx.
func (m *Machine) createUnitHeap(start, size, block, align, flags uint32) uint32 {
	if start == 0 {
		return 0
	}
	align = max(align, 4)
	lo := (uint64(start) + 3) &^ 3
	hi := (uint64(start) + uint64(size)) &^ 3
	if lo > hi {
		return 0
	}
	first := (lo + unitHeapSize + uint64(align) - 1) &^ (uint64(align) - 1)
	if first > hi {
		return 0
	}
	block = (block + align - 1) &^ (align - 1)
	if block == 0 {
		return 0
	}
	n := uint32((hi - first) / uint64(block))
	if n == 0 {
		return 0
	}
	heap, a := uint32(lo), uint32(first)
	m.memInitHeapBase(heap, heapMagicUnit, a, a+n*block, flags)
	mem := m.Mem
	mem.Fill(heap+heapBaseSize, 0, unitHeapSize-heapBaseSize)
	mem.Write32(heap+unitFirstFree, a)
	mem.Write32(heap+unitBlockSize, block)
	for i := uint32(0); i < n-1; i++ {
		mem.Write32(a+i*block, a+(i+1)*block)
	}
	mem.Write32(a+(n-1)*block, 0)
	return heap
}

func (m *Machine) unitAlloc(heap uint32) uint32 {
	mem := m.Mem
	if !m.isUnitHeap(heap) {
		return 0
	}
	b := mem.Read32(heap + unitFirstFree)
	if b == 0 {
		return 0
	}
	mem.Write32(heap+unitFirstFree, mem.Read32(b))
	size := mem.Read32(heap + unitBlockSize)
	if f := m.heapFlagsOf(heap); f&heapOptClear != 0 {
		mem.Fill(b, 0, size)
	} else if f&heapOptFill != 0 {
		m.heapFill(b, size, fillOnAlloc)
	}
	return b
}

func (m *Machine) unitFree(heap, p uint32) {
	mem := m.Mem
	if !m.isUnitHeap(heap) || p == 0 {
		return
	}
	if m.heapFlagsOf(heap)&heapOptFill != 0 {
		m.heapFill(p, mem.Read32(heap+unitBlockSize), fillOnFree)
	}
	mem.Write32(p, mem.Read32(heap+unitFirstFree))
	mem.Write32(heap+unitFirstFree, p)
}

func init() {
	c := "coreinit"
	reg(c, "MEMCreateUnitHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createUnitHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4)))
	})
	reg(c, "MEMDestroyUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		heap := arg(cpu, 0)
		if !m.isUnitHeap(heap) {
			ret(cpu, 0)
			return
		}
		m.memDestroyHeapBase(heap)
		ret(cpu, heap)
	})
	reg(c, "MEMCalcHeapSizeForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		block, n, align := arg(cpu, 0), arg(cpu, 1), max(arg(cpu, 2), 4)
		block = (block + align - 1) &^ (align - 1)
		ret(cpu, n*block+align-4+unitHeapSize)
	})
	reg(c, "MEMCountFreeBlockForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		var n uint32
		if heap := arg(cpu, 0); m.isUnitHeap(heap) {
			for b := m.Mem.Read32(heap + unitFirstFree); b != 0; b = m.Mem.Read32(b) {
				n++
			}
		}
		ret(cpu, n)
	})
	reg(c, "MEMAllocFromUnitHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.unitAlloc(arg(cpu, 0))) })
	reg(c, "MEMFreeToUnitHeap", func(m *Machine, cpu *CPU, t *Thread) { m.unitFree(arg(cpu, 0), arg(cpu, 1)) })

	reg(c, "MEMInitAllocatorForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Unit", arg(cpu, 1), 0)
	})
	reg(c, "__RPAllocatorUnitAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.unitAlloc(m.Mem.Read32(arg(cpu, 0)+allocatorHeap)))
	})
	reg(c, "__RPAllocatorUnitFree", func(m *Machine, cpu *CPU, t *Thread) {
		m.unitFree(m.Mem.Read32(arg(cpu, 0)+allocatorHeap), arg(cpu, 1))
	})
}
