package wiiu

// coreinit: the MEM* heap functions, allocators and intrusive lists.

func init() {
	c := "coreinit"

	reg(c, "MEMGetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.heaps.Base[arg(cpu, 0)]) })
	reg(c, "MEMSetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.heaps.Base[arg(cpu, 0)])
		m.heaps.Base[arg(cpu, 0)] = arg(cpu, 1)
	})
	reg(c, "MEMGetArena", func(m *Machine, cpu *CPU, t *Thread) {
		for a, h := range m.heaps.Base {
			if h == arg(cpu, 0) {
				ret(cpu, a)
				return
			}
		}
		ret(cpu, 9) // MEM_ARENA_INVALID
	})
	reg(c, "MEMFindContainHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.heapFor(arg(cpu, 0)); hp != nil {
			ret(cpu, hp.Start)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMFindParentHeap", func(m *Machine, cpu *CPU, t *Thread) {
		h := m.heaps.get(arg(cpu, 0))
		if h == nil {
			ret(cpu, 0)
			return
		}
		for _, hp := range m.heaps.ByHandle {
			if hp != h && h.Start >= hp.Lo && h.Start < hp.Hi {
				ret(cpu, hp.Start)
				return
			}
		}
		ret(cpu, 0)
	})
	reg(c, "MEMDumpHeap", func(m *Machine, cpu *CPU, t *Thread) {})

	// expanded heaps
	reg(c, "MEMCreateExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		start, size := arg(cpu, 0), arg(cpu, 1)
		if start == 0 || size < heapHeader+16 {
			ret(cpu, 0)
			return
		}
		ret(cpu, m.heaps.createExp(m, start, size, arg(cpu, 2)))
	})
	reg(c, "MEMDestroyExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.heaps.destroy(m, arg(cpu, 0))
		ret(cpu, arg(cpu, 0))
	})
	reg(c, "MEMAllocFromExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(arg(cpu, 0))
		ret(cpu, m.heapAlloc(hp, arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.heaps.get(arg(cpu, 0)).expFree(arg(cpu, 1))
	})
	reg(c, "MEMGetAllocatableSizeForExpHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagExp {
			a := int32(arg(cpu, 1))
			if a < 0 {
				a = -a
			}
			ret(cpu, hp.expLargest(uint32(a)))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMGetTotalFreeSizeForExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagExp {
			ret(cpu, hp.expTotalFree())
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMResizeForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagExp {
			ret(cpu, hp.expResize(arg(cpu, 1), arg(cpu, 2)))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMGetSizeForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.heapFor(arg(cpu, 0)); hp != nil && hp.Kind == tagExp {
			ret(cpu, hp.Size[arg(cpu, 0)])
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMSetAllocModeForExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "MEMGetAllocModeForExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "MEMSetGroupIDForExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "MEMGetGroupIDForMBlockExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "MEMAdjustExpHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil {
			ret(cpu, hp.Hi-hp.Start)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMCheckExpHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })

	// frame heaps
	reg(c, "MEMCreateFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		start, size := arg(cpu, 0), arg(cpu, 1)
		if start == 0 || size < heapHeader {
			ret(cpu, 0)
			return
		}
		ret(cpu, m.heaps.createFrm(m, start, size, arg(cpu, 2)))
	})
	reg(c, "MEMDestroyFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.heaps.destroy(m, arg(cpu, 0))
		ret(cpu, arg(cpu, 0))
	})
	reg(c, "MEMAllocFromFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(arg(cpu, 0))
		ret(cpu, m.heapAlloc(hp, arg(cpu, 1), int32(arg(cpu, 2))))
	})
	reg(c, "MEMFreeToFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagFrm {
			hp.frmFree(arg(cpu, 1))
		}
	})
	reg(c, "MEMRecordStateForFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(arg(cpu, 0))
		if hp == nil || hp.Kind != tagFrm {
			ret(cpu, 0)
			return
		}
		hp.States = append(hp.States, frmState{arg(cpu, 1), hp.Head, hp.Tail})
		ret(cpu, 1)
	})
	reg(c, "MEMFreeByStateToFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(arg(cpu, 0))
		if hp == nil || hp.Kind != tagFrm || len(hp.States) == 0 {
			ret(cpu, 0)
			return
		}
		id := arg(cpu, 1)
		for i := len(hp.States) - 1; i >= 0; i-- {
			s := hp.States[i]
			if id == 0 || s.ID == id {
				hp.Head, hp.Tail = s.Head, s.Tail
				hp.States = hp.States[:i]
				ret(cpu, 1)
				return
			}
		}
		ret(cpu, 0)
	})
	reg(c, "MEMGetAllocatableSizeForFrmHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagFrm {
			a := int32(arg(cpu, 1))
			if a < 0 {
				a = -a
			}
			ret(cpu, hp.frmLargest(uint32(a)))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMAdjustFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagFrm {
			hp.Hi, hp.Tail = hp.Head, hp.Head
			ret(cpu, hp.Hi-hp.Start)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMResizeForMBlockFrmHeap", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(arg(cpu, 0))
		ptr, size := arg(cpu, 1), (arg(cpu, 2)+3)&^3
		if hp == nil || hp.Kind != tagFrm || ptr >= hp.Head || ptr+size > hp.Tail {
			ret(cpu, 0)
			return
		}
		hp.Head = ptr + size
		ret(cpu, size)
	})

	// unit heaps
	reg(c, "MEMCreateUnitHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.heaps.createUnit(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4)))
	})
	reg(c, "MEMDestroyUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.heaps.destroy(m, arg(cpu, 0))
		ret(cpu, arg(cpu, 0))
	})
	reg(c, "MEMAllocFromUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(arg(cpu, 0))
		if hp == nil || hp.Kind != tagUnit || len(hp.FreeUnit) == 0 {
			ret(cpu, 0)
			return
		}
		a := hp.FreeUnit[0]
		hp.FreeUnit = hp.FreeUnit[1:]
		ret(cpu, a)
	})
	reg(c, "MEMFreeToUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagUnit && arg(cpu, 1) != 0 {
			hp.FreeUnit = append([]uint32{arg(cpu, 1)}, hp.FreeUnit...)
		}
	})
	reg(c, "MEMCountFreeBlockForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(arg(cpu, 0)); hp != nil && hp.Kind == tagUnit {
			ret(cpu, uint32(len(hp.FreeUnit)))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMCalcHeapSizeForUnitHeap", func(m *Machine, cpu *CPU, t *Thread) {
		unit, n, align := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if align < 4 {
			align = 4
		}
		unit = (unit + align - 1) &^ (align - 1)
		ret(cpu, heapHeader+align+unit*n)
	})

	// the default heap (the program calls these through function pointers)
	reg(c, "MEMAllocFromDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.heapAlloc(m.heaps.get(m.heaps.Base[arenaMEM2]), arg(cpu, 0), 0x40))
	})
	reg(c, "MEMAllocFromDefaultHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.heapAlloc(m.heaps.get(m.heaps.Base[arenaMEM2]), arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "MEMFreeToDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			if hp := m.heaps.heapFor(p); hp != nil && hp.Kind == tagExp {
				hp.expFree(p)
			}
		}
	})
	reg(c, "OSAllocFromSystem", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.heaps.get(m.heaps.System).expAlloc(arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "OSFreeToSystem", func(m *Machine, cpu *CPU, t *Thread) {
		m.heaps.get(m.heaps.System).expFree(arg(cpu, 0))
	})

	// allocators: { funcs, heap, align, unknown }, funcs = { alloc, free }
	allocFuncs := func(m *Machine, kind string) uint32 {
		key := "allocfuncs:" + kind
		if a, ok := m.perThread[key]; ok {
			return a
		}
		a := m.sysAlloc(8, 8)
		m.Mem.Write32(a, m.thunk("coreinit", "__RPAllocator"+kind+"Alloc"))
		m.Mem.Write32(a+4, m.thunk("coreinit", "__RPAllocator"+kind+"Free"))
		if m.perThread == nil {
			m.perThread = map[string]uint32{}
		}
		m.perThread[key] = a
		return a
	}
	initAlloc := func(kind string, heapArg int, alignArg int) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a := arg(cpu, 0)
			heap := uint32(0)
			if heapArg >= 0 {
				heap = arg(cpu, heapArg)
			} else {
				heap = m.heaps.Base[arenaMEM2]
			}
			align := uint32(0x40)
			if alignArg >= 0 {
				align = arg(cpu, alignArg)
			}
			m.Mem.Write32(a, allocFuncs(m, kind))
			m.Mem.Write32(a+4, heap)
			m.Mem.Write32(a+8, align)
			m.Mem.Write32(a+12, 0)
		}
	}
	reg(c, "MEMInitAllocatorForDefaultHeap", initAlloc("Heap", -1, -1))
	reg(c, "MEMInitAllocatorForExpHeap", initAlloc("Heap", 1, 2))
	reg(c, "MEMInitAllocatorForFrmHeap", initAlloc("Heap", 1, 2))
	reg(c, "MEMInitAllocatorForUnitHeap", initAlloc("Unit", 1, -1))
	reg(c, "__RPAllocatorHeapAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		hp := m.heaps.get(m.Mem.Read32(a + 4))
		ret(cpu, m.heapAlloc(hp, arg(cpu, 1), int32(m.Mem.Read32(a+8))))
	})
	reg(c, "__RPAllocatorHeapFree", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		hp := m.heaps.get(m.Mem.Read32(a + 4))
		if hp != nil && hp.Kind == tagExp {
			hp.expFree(arg(cpu, 1))
		}
	})
	reg(c, "__RPAllocatorUnitAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		hp := m.heaps.get(m.Mem.Read32(arg(cpu, 0) + 4))
		if hp == nil || len(hp.FreeUnit) == 0 {
			ret(cpu, 0)
			return
		}
		a := hp.FreeUnit[0]
		hp.FreeUnit = hp.FreeUnit[1:]
		ret(cpu, a)
	})
	reg(c, "__RPAllocatorUnitFree", func(m *Machine, cpu *CPU, t *Thread) {
		if hp := m.heaps.get(m.Mem.Read32(arg(cpu, 0) + 4)); hp != nil {
			hp.FreeUnit = append([]uint32{arg(cpu, 1)}, hp.FreeUnit...)
		}
	})
	// MEMAllocFromAllocator(allocator, size) calls allocator->funcs->alloc
	reg(c, "MEMAllocFromAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		fn := m.Mem.Read32(m.Mem.Read32(arg(cpu, 0)))
		cpu.PC = fn
		m.jumped = true
	})
	reg(c, "MEMFreeToAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		fn := m.Mem.Read32(m.Mem.Read32(arg(cpu, 0)) + 4)
		cpu.PC = fn
		m.jumped = true
	})

	// intrusive lists: list { head, tail, u16 count, u16 offset }, link { prev, next }
	reg(c, "MEMInitList", func(m *Machine, cpu *CPU, t *Thread) {
		l := arg(cpu, 0)
		m.Mem.Write32(l, 0)
		m.Mem.Write32(l+4, 0)
		m.Mem.Write16(l+8, 0)
		m.Mem.Write16(l+10, uint16(arg(cpu, 1)))
	})
	reg(c, "MEMAppendListObject", func(m *Machine, cpu *CPU, t *Thread) { m.listInsert(arg(cpu, 0), 0, arg(cpu, 1)) })
	reg(c, "MEMPrependListObject", func(m *Machine, cpu *CPU, t *Thread) {
		m.listInsert(arg(cpu, 0), m.Mem.Read32(arg(cpu, 0)), arg(cpu, 1))
	})
	reg(c, "MEMInsertListObject", func(m *Machine, cpu *CPU, t *Thread) { m.listInsert(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)) })
	reg(c, "MEMRemoveListObject", func(m *Machine, cpu *CPU, t *Thread) { m.listRemove(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMGetNextListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l, o := arg(cpu, 0), arg(cpu, 1)
		if o == 0 {
			ret(cpu, m.Mem.Read32(l))
			return
		}
		ret(cpu, m.Mem.Read32(o+uint32(m.Mem.Read16(l+10))+4))
	})
	reg(c, "MEMGetPrevListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l, o := arg(cpu, 0), arg(cpu, 1)
		if o == 0 {
			ret(cpu, m.Mem.Read32(l+4))
			return
		}
		ret(cpu, m.Mem.Read32(o+uint32(m.Mem.Read16(l+10))))
	})
	reg(c, "MEMGetNthListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l, n := arg(cpu, 0), arg(cpu, 1)
		off := uint32(m.Mem.Read16(l + 10))
		o := m.Mem.Read32(l)
		for i := uint32(0); i < n && o != 0; i++ {
			o = m.Mem.Read32(o + off + 4)
		}
		ret(cpu, o)
	})
}

// heapAlloc allocates from any kind of heap.
func (m *Machine) heapAlloc(hp *Heap, size uint32, align int32) uint32 {
	if hp == nil {
		return 0
	}
	switch hp.Kind {
	case tagExp:
		a := hp.expAlloc(size, align)
		if a != 0 && hp.Flags&1 != 0 { // MEM_HEAP_FLAG_ZERO_ALLOCATED
			m.Mem.Fill(a, 0, size)
		}
		return a
	case tagFrm:
		a := hp.frmAlloc(size, align)
		if a != 0 && hp.Flags&1 != 0 {
			m.Mem.Fill(a, 0, size)
		}
		return a
	case tagUnit:
		if len(hp.FreeUnit) == 0 {
			return 0
		}
		a := hp.FreeUnit[0]
		hp.FreeUnit = hp.FreeUnit[1:]
		return a
	}
	return 0
}

// listInsert puts obj before 'before' (0 = at the end).
func (m *Machine) listInsert(l, before, obj uint32) {
	mem := m.Mem
	off := uint32(mem.Read16(l + 10))
	if before == 0 {
		tail := mem.Read32(l + 4)
		mem.Write32(obj+off, tail)
		mem.Write32(obj+off+4, 0)
		if tail != 0 {
			mem.Write32(tail+off+4, obj)
		} else {
			mem.Write32(l, obj)
		}
		mem.Write32(l+4, obj)
	} else {
		prev := mem.Read32(before + off)
		mem.Write32(obj+off, prev)
		mem.Write32(obj+off+4, before)
		mem.Write32(before+off, obj)
		if prev != 0 {
			mem.Write32(prev+off+4, obj)
		} else {
			mem.Write32(l, obj)
		}
	}
	mem.Write16(l+8, mem.Read16(l+8)+1)
}

func (m *Machine) listRemove(l, obj uint32) {
	mem := m.Mem
	off := uint32(mem.Read16(l + 10))
	prev, next := mem.Read32(obj+off), mem.Read32(obj+off+4)
	if prev != 0 {
		mem.Write32(prev+off+4, next)
	} else {
		mem.Write32(l, next)
	}
	if next != 0 {
		mem.Write32(next+off, prev)
	} else {
		mem.Write32(l+4, prev)
	}
	mem.Write32(obj+off, 0)
	mem.Write32(obj+off+4, 0)
	if n := mem.Read16(l + 8); n > 0 {
		mem.Write16(l+8, n-1)
	}
}
