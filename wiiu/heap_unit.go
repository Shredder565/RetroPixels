package wiiu

// Unit heaps: equal-sized blocks, the free ones chained through their first
// word.
//
// Written for RetroPixels from the SDK's structure layouts and the
// behaviour programs rely on.

const (
	// after MEMHeapBase
	unitFree     = 0x40 // the first free block
	unitBlockLen = 0x44
	unitHeapSize = 0x48
)

func (m *Machine) isUnitHeap(heap uint32) bool { return m.heapKind(heap, heapMagicUnit) }

// createUnitHeap is MEMCreateUnitHeapEx: as many blocks as fit, aligned, after
// the header.
func (m *Machine) createUnitHeap(start, size, blockLen, align, flags uint32) uint32 {
	if start == 0 {
		return 0
	}
	align = max(align, 4)
	from := (uint64(start) + 3) &^ 3
	to := (uint64(start) + uint64(size)) &^ 3
	first := (from + unitHeapSize + uint64(align) - 1) &^ (uint64(align) - 1)
	blockLen = (blockLen + align - 1) &^ (align - 1)
	if from > to || first > to || blockLen == 0 {
		return 0
	}
	count := uint32((to - first) / uint64(blockLen))
	if count == 0 {
		return 0
	}
	heap, b := uint32(from), uint32(first)
	m.memInitHeapBase(heap, heapMagicUnit, b, b+count*blockLen, flags)
	mem := m.Mem
	mem.Fill(heap+heapBaseSize, 0, unitHeapSize-heapBaseSize)
	mem.Write32(heap+unitFree, b)
	mem.Write32(heap+unitBlockLen, blockLen)
	for i := uint32(1); i <= count; i++ {
		next := b + blockLen
		if i == count {
			next = 0
		}
		mem.Write32(b, next)
		b += blockLen
	}
	return heap
}

func (m *Machine) unitAlloc(heap uint32) uint32 {
	if !m.isUnitHeap(heap) {
		return 0
	}
	mem := m.Mem
	b := mem.Read32(heap + unitFree)
	if b == 0 {
		return 0
	}
	mem.Write32(heap+unitFree, mem.Read32(b))
	n := mem.Read32(heap + unitBlockLen)
	switch f := m.heapFlagsOf(heap); {
	case f&heapOptClear != 0:
		mem.Fill(b, 0, n)
	case f&heapOptFill != 0:
		m.heapFill(b, n, fillOnAlloc)
	}
	return b
}

func (m *Machine) unitGiveBack(heap, b uint32) {
	if !m.isUnitHeap(heap) || b == 0 {
		return
	}
	mem := m.Mem
	if m.heapFlagsOf(heap)&heapOptFill != 0 {
		m.heapFill(b, mem.Read32(heap+unitBlockLen), fillOnFree)
	}
	mem.Write32(b, mem.Read32(heap+unitFree))
	mem.Write32(heap+unitFree, b)
}

func init() {
	c := "coreinit"
	reg(c, "MEMCreateUnitHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.createUnitHeap(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4)))
	})
	reg(c, "MEMDestroyUnitHeap", onHeap(heapMagicUnit, func(m *Machine, cpu *CPU, heap uint32) {
		m.memDestroyHeapBase(heap)
		ret(cpu, heap)
	}))
	// the size a unit heap of count blocks needs, at worst
	reg(c, "MEMCalcHeapSizeForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		blockLen, count, align := arg(cpu, 0), arg(cpu, 1), max(arg(cpu, 2), 4)
		blockLen = (blockLen + align - 1) &^ (align - 1)
		ret(cpu, unitHeapSize+align-4+count*blockLen)
	})
	reg(c, "MEMCountFreeBlockForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		var n uint32
		if heap := arg(cpu, 0); m.isUnitHeap(heap) {
			for b := m.Mem.Read32(heap + unitFree); b != 0; b = m.Mem.Read32(b) {
				n++
			}
		}
		ret(cpu, n)
	})
	reg(c, "MEMAllocFromUnitHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.unitAlloc(arg(cpu, 0))) })
	reg(c, "MEMFreeToUnitHeap", func(m *Machine, cpu *CPU, t *Thread) { m.unitGiveBack(arg(cpu, 0), arg(cpu, 1)) })

	reg(c, "MEMInitAllocatorForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Unit", arg(cpu, 1), 0)
	})
	allocatorFor("Unit", func(m *Machine, heap, _ uint32, _ int32) uint32 { return m.unitAlloc(heap) },
		func(m *Machine, heap, p uint32) { m.unitGiveBack(heap, p) })
}
