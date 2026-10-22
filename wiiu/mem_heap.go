// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/
// coreinit_MEM.cpp, coreinit_MEM.h, coreinit_SysHeap.cpp, coreinit_Memory.cpp
// and coreinit_Init.cpp, rewritten in Go for RetroPixels.

package wiiu

// Memory heaps: what every kind of heap shares (coreinit's MEM* functions).
//
// A heap's header lives at its start in the program's memory, laid out as in
// Cafe OS, since programs read and walk them. Every heap is linked into a
// list: the list of the heap that contains it (each heap has a list of the
// heaps made inside it), or, at the top, one of three lists by memory area
// (MEM1, the foreground bucket, everything else).
//
// The program's base heaps are a frame heap over MEM1, a frame heap over the
// foreground bucket's free area and an expanded heap over the rest of MEM2
// (the default heap). A program that exports __preinit_user gets to replace
// them before anything else runs.

const (
	heapMagicUnit  = 0x554e5448 // "UNTH"
	heapMagicBlock = 0x424c4b48 // "BLKH"
	heapMagicFrame = 0x46524d48 // "FRMH"
	heapMagicExp   = 0x45585048 // "EXPH"
	heapMagicUser  = 0x55535248 // "USRH"

	// MEMHeapBase
	heapMagic     = 0x00
	heapLink      = 0x04 // MEMLink in the containing list
	heapChildList = 0x0c // MEMList of the heaps inside this one
	heapStart     = 0x18
	heapEnd       = 0x1c // one past the end
	heapFlags     = 0x33 // (a byte)
	heapBaseSize  = 0x34

	heapOptClear = 1 << 0 // zero what's allocated
	heapOptFill  = 1 << 1 // fill on create, alloc and free
	heapOptLock  = 1 << 2 // thread-safe

	// MEMList: head, tail, u16 count, u16 offset of the MEMLink in objects
	listHead   = 0x0
	listTail   = 0x4
	listCount  = 0x8
	listOffset = 0xa
	listSize   = 0xc

	// MEMLink
	linkPrev = 0x0
	linkNext = 0x4

	// MEMAllocator: funcs { alloc, free }, heap, param1, param2
	allocatorFuncs = 0x0
	allocatorHeap  = 0x4
	allocatorParam = 0x8

	arenaMEM1    = 0
	arenaMEM2    = 1
	arenaFG      = 8
	arenaCount   = 9
	arenaInvalid = 9

	fillOnCreate = 0
	fillOnAlloc  = 1
	fillOnFree   = 2
)

// Heaps is coreinit's heap bookkeeping.
type Heaps struct {
	lists       [3]uint32 // MEMLists in the system area: other memory, MEM1, foreground bucket
	base        [arenaCount]uint32
	fill        [3]uint32
	defaultHeap uint32 // what MEMAllocFromDefaultHeap allocates from
	system      uint32 // OSAllocFromSystem's heap, which RetroPixels uses too
	mem2Lo      uint32 // start of MEM2 as OSGetMemBound gives it
	preinit     uint32 // the three handles __preinit_user may change
	allocFuncs  map[string]uint32
	started     bool // the base heaps are set (__preinit_user has run)
}

func newHeaps() *Heaps {
	return &Heaps{fill: [3]uint32{0xc3c3c3c3, 0xf3f3f3f3, 0xd3d3d3d3}, allocFuncs: map[string]uint32{}}
}

// setup makes the system heap and the default heaps, once the program is
// loaded (MEM2 starts after its data).
func (h *Heaps) setup(m *Machine) {
	l := m.sysAlloc(3*listSize, 4)
	for i := range h.lists {
		h.lists[i] = l + uint32(i)*listSize
		m.memInitList(h.lists[i], heapLink)
	}
	h.mem2Lo = max((m.dataEnd+0xfff)&^0xfff, DataBase)
	m.initForegroundBucket()
	h.system = m.createExpHeap(SysHeapBase, SysHeapSize, heapOptLock)

	h.preinit = m.sysAlloc(12, 4)
	a, n := m.memBound(1)
	m.Mem.Write32(h.preinit, m.createFrmHeap(a, n, 0))
	a, n = m.fgFreeArea()
	m.Mem.Write32(h.preinit+4, m.createFrmHeap(a, n, 0))
	a, n = m.memBound(2)
	h.defaultHeap = m.createExpHeap(a, n, heapOptLock)
	m.Mem.Write32(h.preinit+8, h.defaultHeap)

	m.dynAlloc = m.thunk("coreinit", "__RPDynLoadAlloc")
	m.dynFree = m.thunk("coreinit", "__RPDynLoadFree")
}

// startHeaps runs the program's __preinit_user, if it has one, then makes
// the heaps it left the base heaps, and allocates once through the loader's
// allocator (Watch Dogs sets up its malloc then). It returns to cpu.LR.
func (m *Machine) startHeaps(cpu *CPU, t *Thread) {
	h := m.heaps
	h.started = true
	dummy := func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
		p := h.preinit
		for i, arena := range []uint32{arenaMEM1, arenaFG, arenaMEM2} {
			if hp := m.Mem.Read32(p + uint32(i)*4); hp != 0 {
				m.memSetBaseHeapHandle(arena, hp)
			}
		}
		out := m.sysAlloc(4, 4)
		m.callGuest(cpu, t, m.dynAlloc, []uint32{0x500, 4, out}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
			if a := m.Mem.Read32(out); a != 0 {
				m.callGuest(cpu, t, m.dynFree, []uint32{a}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {})
			}
		})
	}
	main := m.mainModule()
	if fn, ok := main.Exports["__preinit_user"]; ok {
		cpu.GPR[2] = main.Info.SDA2Base
		cpu.GPR[13] = main.Info.SDABase
		p := h.preinit
		m.callGuest(cpu, t, fn, []uint32{p, p + 4, p + 8}, dummy)
		return
	}
	dummy(m, cpu, t, 0)
}

// allocSystem takes memory for RetroPixels' own use from the system heap.
func (m *Machine) allocSystem(size, align uint32) uint32 {
	a := m.expAlloc(m.heaps.system, size, int32(align))
	if a == 0 {
		panic("wiiu: out of system memory")
	}
	return a
}

func (m *Machine) freeSystem(a uint32) { m.expFree(m.heaps.system, a) }

// defaultHeapFunc is the function the program's MEMAllocFromDefaultHeap,
// MEMAllocFromDefaultHeapEx or MEMFreeToDefaultHeap pointer points to (the
// program may have pointed it at its own allocator).
func (m *Machine) defaultHeapFunc(name string) uint32 {
	return m.Mem.Read32(m.dataExport("coreinit", name))
}

// memBound is OSGetMemBound: MEM1, or MEM2 after the loaded modules.
func (m *Machine) memBound(typ uint32) (addr, size uint32) {
	switch typ {
	case 1:
		return MEM1Base, MEM1Size
	case 2:
		return m.heaps.mem2Lo, MEM2End - m.heaps.mem2Lo
	}
	return 0, 0
}

// heap base

func (m *Machine) memInitHeapBase(heap, magic, start, end, flags uint32) {
	mem := m.Mem
	mem.Fill(heap, 0, heapBaseSize)
	mem.Write32(heap+heapMagic, magic)
	mem.Write32(heap+heapStart, start)
	mem.Write32(heap+heapEnd, end)
	mem.Write8(heap+heapFlags, uint8(flags))
	m.memInitList(heap+heapChildList, heapLink)
	m.memAppendListObject(m.memListFor(heap), heap)
}

func (m *Machine) memDestroyHeapBase(heap uint32) {
	m.memRemoveListObject(m.memListFor(heap), heap)
}

func (m *Machine) heapFlagsOf(heap uint32) uint32 { return uint32(m.Mem.Read8(heap + heapFlags)) }

// memFindIn finds the innermost heap in list (or below) whose memory holds a.
func (m *Machine) memFindIn(list, a uint32) uint32 {
	for hp := m.memNextListObject(list, 0); hp != 0; hp = m.memNextListObject(list, hp) {
		if m.Mem.Read32(hp+heapStart) <= a && a < m.Mem.Read32(hp+heapEnd) {
			if in := m.memFindIn(hp+heapChildList, a); in != 0 {
				return in
			}
			return hp
		}
	}
	return 0
}

// memAreaList is the top-level list for memory at a.
func (m *Machine) memAreaList(a uint32) uint32 {
	h := m.heaps
	switch {
	case a >= MEM1Base && a-MEM1Base < MEM1Size:
		return h.lists[1]
	case a >= FGBase && a-FGBase < FGSize:
		return h.lists[2]
	}
	return h.lists[0]
}

// memListFor is the list a heap at heap belongs in.
func (m *Machine) memListFor(heap uint32) uint32 {
	l := m.memAreaList(heap)
	if in := m.memFindIn(l, heap); in != 0 {
		return in + heapChildList
	}
	return l
}

func (m *Machine) memFindContainHeap(a uint32) uint32 { return m.memFindIn(m.memAreaList(a), a) }

func (m *Machine) memSetBaseHeapHandle(arena, heap uint32) uint32 {
	h := m.heaps
	if arena >= arenaCount {
		return 0
	}
	if old := h.base[arena]; old != 0 {
		return old
	}
	h.base[arena] = heap
	return 0
}

// heapFill fills n bytes at a with the fill value for when.
func (m *Machine) heapFill(a, n, when uint32) {
	m.Mem.Fill(a, uint8(m.heaps.fill[when]), n)
}

// lists

func (m *Machine) memInitList(l, offset uint32) {
	mem := m.Mem
	mem.Write32(l+listHead, 0)
	mem.Write32(l+listTail, 0)
	mem.Write16(l+listCount, 0)
	mem.Write16(l+listOffset, uint16(offset))
}

func (m *Machine) memLink(l, obj uint32) uint32 { return obj + uint32(m.Mem.Read16(l+listOffset)) }

func (m *Machine) memSetSingleObject(l, obj uint32) {
	mem := m.Mem
	lk := m.memLink(l, obj)
	mem.Write32(lk+linkPrev, 0)
	mem.Write32(lk+linkNext, 0)
	mem.Write16(l+listCount, mem.Read16(l+listCount)+1)
	mem.Write32(l+listHead, obj)
	mem.Write32(l+listTail, obj)
}

func (m *Machine) memAppendListObject(l, obj uint32) {
	mem := m.Mem
	tail := mem.Read32(l + listTail)
	if mem.Read32(l+listHead) == 0 {
		m.memSetSingleObject(l, obj)
		return
	}
	mem.Write16(l+listCount, mem.Read16(l+listCount)+1)
	lk := m.memLink(l, obj)
	mem.Write32(lk+linkPrev, tail)
	mem.Write32(lk+linkNext, 0)
	mem.Write32(m.memLink(l, tail)+linkNext, obj)
	mem.Write32(l+listTail, obj)
}

func (m *Machine) memPrependListObject(l, obj uint32) {
	mem := m.Mem
	head := mem.Read32(l + listHead)
	if head == 0 {
		m.memSetSingleObject(l, obj)
		return
	}
	mem.Write16(l+listCount, mem.Read16(l+listCount)+1)
	lk := m.memLink(l, obj)
	mem.Write32(lk+linkPrev, 0)
	mem.Write32(lk+linkNext, head)
	mem.Write32(m.memLink(l, head)+linkPrev, obj)
	mem.Write32(l+listHead, obj)
}

// memInsertListObject puts obj before 'before' (at the end if that's 0).
func (m *Machine) memInsertListObject(l, before, obj uint32) {
	mem := m.Mem
	if before == 0 {
		m.memAppendListObject(l, obj)
		return
	}
	prev := mem.Read32(m.memLink(l, before) + linkPrev)
	if prev == 0 {
		m.memPrependListObject(l, obj)
		return
	}
	lk := m.memLink(l, obj)
	mem.Write32(lk+linkPrev, prev)
	mem.Write32(lk+linkNext, before)
	mem.Write32(m.memLink(l, prev)+linkNext, obj)
	mem.Write32(m.memLink(l, before)+linkPrev, obj)
	mem.Write16(l+listCount, mem.Read16(l+listCount)+1)
}

func (m *Machine) memRemoveListObject(l, obj uint32) {
	mem := m.Mem
	lk := m.memLink(l, obj)
	prev, next := mem.Read32(lk+linkPrev), mem.Read32(lk+linkNext)
	if prev != 0 {
		mem.Write32(m.memLink(l, prev)+linkNext, next)
	} else {
		mem.Write32(l+listHead, next)
	}
	if next != 0 {
		mem.Write32(m.memLink(l, next)+linkPrev, prev)
	} else {
		mem.Write32(l+listTail, prev)
	}
	mem.Write32(lk+linkPrev, 0)
	mem.Write32(lk+linkNext, 0)
	mem.Write16(l+listCount, mem.Read16(l+listCount)-1)
}

func (m *Machine) memNextListObject(l, obj uint32) uint32 {
	if obj == 0 {
		return m.Mem.Read32(l + listHead)
	}
	return m.Mem.Read32(m.memLink(l, obj) + linkNext)
}

func (m *Machine) memPrevListObject(l, obj uint32) uint32 {
	if obj == 0 {
		return m.Mem.Read32(l + listTail)
	}
	return m.Mem.Read32(m.memLink(l, obj) + linkPrev)
}

// heapAlloc allocates from a heap of any kind, as an allocator would.
func (m *Machine) heapAlloc(heap, size uint32, align int32) uint32 {
	switch m.Mem.Read32(heap + heapMagic) {
	case heapMagicExp:
		return m.expAlloc(heap, size, align)
	case heapMagicFrame:
		return m.frmAlloc(heap, size, align)
	case heapMagicUnit:
		return m.unitAlloc(heap)
	case heapMagicBlock:
		return m.blockAlloc(heap, size, align)
	}
	return 0
}

// allocatorFuncs is the { alloc, free } pair for allocators of a kind.
func (m *Machine) allocatorFuncs(kind string) uint32 {
	h := m.heaps
	if a, ok := h.allocFuncs[kind]; ok {
		return a
	}
	a := m.sysAlloc(8, 8)
	m.Mem.Write32(a, m.thunk("coreinit", "__RPAllocator"+kind+"Alloc"))
	m.Mem.Write32(a+4, m.thunk("coreinit", "__RPAllocator"+kind+"Free"))
	h.allocFuncs[kind] = a
	return a
}

func (m *Machine) initAllocator(a uint32, kind string, heap, param uint32) {
	m.Mem.Write32(a+allocatorFuncs, m.allocatorFuncs(kind))
	m.Mem.Write32(a+allocatorHeap, heap)
	m.Mem.Write32(a+allocatorParam, param)
	m.Mem.Write32(a+allocatorParam+4, 0)
}

func init() {
	c := "coreinit"

	reg(c, "MEMGetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 0); a < arenaCount {
			ret(cpu, m.heaps.base[a])
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMSetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.memSetBaseHeapHandle(arg(cpu, 0), arg(cpu, 1)))
	})
	reg(c, "MEMGetArena", func(m *Machine, cpu *CPU, t *Thread) {
		for a, h := range m.heaps.base {
			if h != 0 && h == arg(cpu, 0) {
				ret(cpu, uint32(a))
				return
			}
		}
		ret(cpu, arenaInvalid)
	})
	reg(c, "MEMFindContainHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.memFindContainHeap(arg(cpu, 0))) })
	reg(c, "MEMFindParentHeap", func(m *Machine, cpu *CPU, t *Thread) {
		heap := arg(cpu, 0)
		l := m.memListFor(heap)
		for _, top := range m.heaps.lists {
			if l == top {
				ret(cpu, 0)
				return
			}
		}
		ret(cpu, l-heapChildList)
	})
	reg(c, "MEMGetFillValForHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 3 {
			ret(cpu, m.heaps.fill[i])
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMSetFillValForHeap", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 3 {
			ret(cpu, m.heaps.fill[i])
			m.heaps.fill[i] = arg(cpu, 1)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "MEMCreateUserHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		a, size := arg(cpu, 0), arg(cpu, 1)
		m.memInitHeapBase(a, heapMagicUser, a+0x40, a+size, 0)
		ret(cpu, a)
	})
	reg(c, "MEMDumpHeap", func(m *Machine, cpu *CPU, t *Thread) {})

	// lists
	reg(c, "MEMInitList", func(m *Machine, cpu *CPU, t *Thread) { m.memInitList(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMAppendListObject", func(m *Machine, cpu *CPU, t *Thread) { m.memAppendListObject(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMPrependListObject", func(m *Machine, cpu *CPU, t *Thread) { m.memPrependListObject(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMInsertListObject", func(m *Machine, cpu *CPU, t *Thread) {
		m.memInsertListObject(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "MEMRemoveListObject", func(m *Machine, cpu *CPU, t *Thread) { m.memRemoveListObject(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "MEMGetNextListObject", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.memNextListObject(arg(cpu, 0), arg(cpu, 1))) })
	reg(c, "MEMGetPrevListObject", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.memPrevListObject(arg(cpu, 0), arg(cpu, 1))) })
	reg(c, "MEMGetNthListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l, n := arg(cpu, 0), arg(cpu, 1)
		o := m.memNextListObject(l, 0)
		for i := uint32(0); i < n && o != 0; i++ {
			o = m.memNextListObject(l, o)
		}
		ret(cpu, o)
	})

	// the default heap: the program calls these through the function
	// pointers of the same names, which it may change
	reg(c, "MEMAllocFromDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.defaultHeap, arg(cpu, 0), 0x40))
	})
	reg(c, "MEMAllocFromDefaultHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.defaultHeap, arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "MEMFreeToDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) { m.expFree(m.heaps.defaultHeap, arg(cpu, 0)) })

	// the loader's default allocator: (size, align, address out) through
	// the default heap pointers
	reg(c, "__RPDynLoadAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		out := arg(cpu, 2)
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMAllocFromDefaultHeapEx"), []uint32{arg(cpu, 0), arg(cpu, 1)},
			func(m *Machine, cpu *CPU, t *Thread, a uint32) {
				if out != 0 {
					m.Mem.Write32(out, a)
				}
				ret(cpu, 0)
			})
	})
	reg(c, "__RPDynLoadFree", func(m *Machine, cpu *CPU, t *Thread) {
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMFreeToDefaultHeap"), []uint32{arg(cpu, 0)},
			func(m *Machine, cpu *CPU, t *Thread, _ uint32) { ret(cpu, 0) })
	})

	// the system heap
	reg(c, "OSAllocFromSystem", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.system, arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "OSFreeToSystem", func(m *Machine, cpu *CPU, t *Thread) { m.freeSystem(arg(cpu, 0)) })

	reg(c, "OSGetMemBound", func(m *Machine, cpu *CPU, t *Thread) {
		a, n := m.memBound(arg(cpu, 0))
		if n == 0 {
			ret(cpu, 0xffffffff)
			return
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, a)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, n)
		}
		ret(cpu, 0)
	})

	// allocators
	reg(c, "MEMInitAllocatorForDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Default", m.heaps.base[arenaMEM2], 0)
	})
	reg(c, "__RPAllocatorDefaultAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		cpu.GPR[3] = cpu.GPR[4]
		cpu.PC = m.defaultHeapFunc("MEMAllocFromDefaultHeap")
		m.jumped = true
	})
	reg(c, "__RPAllocatorDefaultFree", func(m *Machine, cpu *CPU, t *Thread) {
		cpu.GPR[3] = cpu.GPR[4]
		cpu.PC = m.defaultHeapFunc("MEMFreeToDefaultHeap")
		m.jumped = true
	})
	// MEMAllocFromAllocator(allocator, size) and MEMFreeToAllocator(allocator,
	// p) go on to the allocator's functions
	reg(c, "MEMAllocFromAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		cpu.PC = m.Mem.Read32(m.Mem.Read32(arg(cpu, 0) + allocatorFuncs))
		m.jumped = true
	})
	reg(c, "MEMFreeToAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		cpu.PC = m.Mem.Read32(m.Mem.Read32(arg(cpu, 0)+allocatorFuncs) + 4)
		m.jumped = true
	})
}
