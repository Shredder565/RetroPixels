// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// Unit heaps (MEMUnitHeap): a pool of equal-sized blocks. Free blocks form
// a singly linked stack threaded through each block's first word.

// MEMUnitHeap fields after the common header
const (
	uhFreeTop = 0x40 // the free block handed out next, or 0
	uhUnit    = 0x44 // block size, alignment included
	uhSize    = 0x48
)

func (m *Machine) isUnitHeap(heap uint32) bool { return m.heapTagIs(heap, tagUnitHeap) }

// unitStride is a block size rounded up to the alignment (at least 4).
func unitStride(unit, align uint32) (uint32, uint32) {
	if align < 4 {
		align = 4
	}
	return (unit + align - 1) &^ (align - 1), align
}

// createUnitHeap is MEMCreateUnitHeapEx.
func (m *Machine) createUnitHeap(start, size, unit, align, options uint32) uint32 {
	heap, end, ok := heapSpan(start, size, 0)
	if !ok {
		return 0
	}
	stride, align := unitStride(unit, align)
	first := roundUp64(uint64(heap)+uhSize, uint64(align))
	if stride == 0 || first > uint64(end) {
		return 0
	}
	count := (uint64(end) - first) / uint64(stride)
	if count == 0 {
		return 0
	}
	base := uint32(first)
	limit := base + uint32(count)*stride
	m.heapHeaderInit(heap, tagUnitHeap, base, limit, options)
	mem := m.Mem
	mem.Write32(heap+uhFreeTop, base)
	mem.Write32(heap+uhUnit, stride)
	// chain every block to the one after it; the last ends the chain
	for b := base; b < limit; b += stride {
		next := b + stride
		if next >= limit {
			next = 0
		}
		mem.Write32(b, next)
	}
	return heap
}

func (m *Machine) unitAlloc(heap uint32) uint32 {
	if !m.isUnitHeap(heap) {
		return 0
	}
	mem := m.Mem
	b := mem.Read32(heap + uhFreeTop)
	if b == 0 {
		return 0
	}
	mem.Write32(heap+uhFreeTop, mem.Read32(b))
	n := mem.Read32(heap + uhUnit)
	switch opts := m.heapOptions(heap); {
	case opts&memZeroAlloc != 0:
		mem.Fill(b, 0, n)
	case opts&memDebugFill != 0:
		m.heapMark(b, n, markOnAlloc)
	}
	return b
}

func (m *Machine) unitFree(heap, p uint32) {
	if p == 0 || !m.isUnitHeap(heap) {
		return
	}
	mem := m.Mem
	if m.heapOptions(heap)&memDebugFill != 0 {
		m.heapMark(p, mem.Read32(heap+uhUnit), markOnFree)
	}
	mem.Write32(p, mem.Read32(heap+uhFreeTop))
	mem.Write32(heap+uhFreeTop, p)
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
		m.heapUnlink(heap)
		ret(cpu, heap)
	})
	// (unit size, count, alignment): bytes a heap of count blocks needs
	reg(c, "MEMCalcHeapSizeForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		stride, align := unitStride(arg(cpu, 0), arg(cpu, 2))
		ret(cpu, uhSize+(align-4)+arg(cpu, 1)*stride)
	})
	reg(c, "MEMCountFreeBlockForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		heap, n := arg(cpu, 0), uint32(0)
		if m.isUnitHeap(heap) {
			for b := m.Mem.Read32(heap + uhFreeTop); b != 0; b = m.Mem.Read32(b) {
				n++
			}
		}
		ret(cpu, n)
	})
	reg(c, "MEMAllocFromUnitHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.unitAlloc(arg(cpu, 0))) })
	reg(c, "MEMFreeToUnitHeap", func(m *Machine, cpu *CPU, t *Thread) { m.unitFree(arg(cpu, 0), arg(cpu, 1)) })

	reg(c, "MEMInitAllocatorForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.setAllocator(arg(cpu, 0), "Unit", arg(cpu, 1), 0)
	})
	reg(c, "__RPAllocatorUnitAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		heap, _ := m.allocatorHeapArg(arg(cpu, 0))
		ret(cpu, m.unitAlloc(heap))
	})
	reg(c, "__RPAllocatorUnitFree", func(m *Machine, cpu *CPU, t *Thread) {
		heap, _ := m.allocatorHeapArg(arg(cpu, 0))
		m.unitFree(heap, arg(cpu, 1))
	})
}
