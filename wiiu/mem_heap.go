// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// coreinit's MEM heaps: the part every kind of heap has in common, the
// intrusive lists heaps are kept in, the base heaps, the system heap
// RetroPixels allocates from, and allocators.
//
// Programs look inside heaps, so a heap lives in the program's memory with
// the header Cafe OS gives it (the SDK's MEMHeapHeader), followed by the
// fields of its kind. Each heap sits in a list: the child list of the heap
// whose memory holds it, or, for a heap that isn't inside another one, one
// of three lists by memory area (MEM1, the foreground bucket, the rest).
//
// At start-up a program has three base heaps: a frame heap over MEM1, a
// frame heap over the free part of the foreground bucket, and an expanded
// heap over MEM2 after the loaded modules (the default heap). One that
// exports __preinit_user may swap in heaps of its own before main.

// heap tags, as they appear in memory
const (
	tagExpHeap   = 0x45585048 // "EXPH"
	tagFrmHeap   = 0x46524d48 // "FRMH"
	tagUnitHeap  = 0x554e5448 // "UNTH"
	tagBlockHeap = 0x424c4b48 // "BLKH"
	tagUserHeap  = 0x55535248 // "USRH"
)

// MEMHeapHeader
const (
	hhTag      = 0x00
	hhLink     = 0x04 // where the heap is linked into its list (MEMLink)
	hhChildren = 0x0c // the heaps made inside this one (MEMList)
	hhFirst    = 0x18 // the first byte the heap hands out
	hhLimit    = 0x1c // one past the last
	hhOptions  = 0x33 // a byte of MEM_HEAP_FLAG_*
	hhSize     = 0x34
)

// MEM_HEAP_FLAG_*
const (
	memZeroAlloc = 1 << 0 // allocations come back zeroed
	memDebugFill = 1 << 1 // memory is filled with a marker on create, alloc and free
	memUseLock   = 1 << 2 // the heap may be used from several threads
)

// MEMList is { head, tail, u16 count, u16 offset of the link in each
// object }; a MEMLink is { prev, next }.
const (
	mlHead    = 0x0
	mlTail    = 0x4
	mlCount   = 0x8
	mlLinkOff = 0xa
	mlBytes   = 0xc

	lkPrev = 0x0
	lkNext = 0x4
)

// MEMAllocator: a pointer to its { alloc, free } functions, the heap, and
// two words for the kind's own use.
const (
	maFuncs = 0x0
	maHeap  = 0x4
	maArg   = 0x8
)

// MEMArena values: base heaps are kept per arena.
const (
	arenaMEM1    = 0
	arenaMEM2    = 1
	arenaFG      = 8
	arenaCount   = 9
	arenaInvalid = 9
)

// MEMHeapFillType: which of the three marker values a debug fill uses.
const (
	markOnCreate = 0
	markOnAlloc  = 1
	markOnFree   = 2
)

// Heaps is coreinit's heap bookkeeping.
type Heaps struct {
	areaLists   [3]uint32 // top-level MEMLists: the rest of memory, MEM1, the foreground bucket
	arenas      [arenaCount]uint32
	marks       [3]uint32 // the debug fill values
	defaultHeap uint32    // the heap behind MEMAllocFromDefaultHeap
	system      uint32    // the heap behind OSAllocFromSystem (RetroPixels uses it too)
	mem2Start   uint32    // where the program's part of MEM2 starts
	preinitArgs uint32    // the three heap handles __preinit_user gets pointers to
	funcTables  map[string]uint32
	started     bool // the base heaps are in place (after __preinit_user)
}

func newHeaps() *Heaps {
	h := &Heaps{funcTables: map[string]uint32{}}
	h.marks = [3]uint32{0xc3c3c3c3, 0xf3f3f3f3, 0xd3d3d3d3}
	return h
}

// setup makes the system heap and the base heaps once the program is loaded
// (the program's MEM2 starts after the modules' data).
func (h *Heaps) setup(m *Machine) {
	lists := m.sysAlloc(uint32(len(h.areaLists))*mlBytes, 4)
	for i := range h.areaLists {
		h.areaLists[i] = lists + uint32(i)*mlBytes
		m.list(h.areaLists[i]).init(hhLink)
	}
	h.mem2Start = DataBase
	if top := (m.dataEnd + 0xfff) &^ 0xfff; top > h.mem2Start {
		h.mem2Start = top
	}
	m.initForegroundBucket()
	h.system = m.createExpHeap(SysHeapBase, SysHeapSize, memUseLock)

	h.preinitArgs = m.sysAlloc(12, 4)
	mem1, mem1Size := m.memBound(1)
	m.Mem.Write32(h.preinitArgs, m.createFrmHeap(mem1, mem1Size, 0))
	fg, fgSize := m.fgFreeArea()
	m.Mem.Write32(h.preinitArgs+4, m.createFrmHeap(fg, fgSize, 0))
	mem2, mem2Size := m.memBound(2)
	h.defaultHeap = m.createExpHeap(mem2, mem2Size, memUseLock)
	m.Mem.Write32(h.preinitArgs+8, h.defaultHeap)

	m.dynAlloc = m.thunk("coreinit", "__RPDynLoadAlloc")
	m.dynFree = m.thunk("coreinit", "__RPDynLoadFree")
}

// startHeaps calls the program's __preinit_user (when it has one) with
// pointers to the three base heap handles, makes whatever it left there the
// base heaps, then allocates and frees once through the loader's allocator
// (some programs set up their malloc on that first call). It ends by
// returning to cpu.LR.
func (m *Machine) startHeaps(cpu *CPU, t *Thread) {
	h := m.heaps
	h.started = true
	slots := h.preinitArgs
	adopt := func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
		order := [3]uint32{arenaMEM1, arenaFG, arenaMEM2}
		for i, arena := range order {
			if heap := m.Mem.Read32(slots + uint32(i)*4); heap != 0 {
				m.setBaseHeap(arena, heap)
			}
		}
		result := m.sysAlloc(4, 4)
		m.callGuest(cpu, t, m.dynAlloc, []uint32{0x500, 4, result}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
			p := m.Mem.Read32(result)
			if p == 0 {
				return
			}
			m.callGuest(cpu, t, m.dynFree, []uint32{p}, func(*Machine, *CPU, *Thread, uint32) {})
		})
	}
	main := m.mainModule()
	entry, ok := main.Exports["__preinit_user"]
	if !ok {
		adopt(m, cpu, t, 0)
		return
	}
	cpu.GPR[2], cpu.GPR[13] = main.Info.SDA2Base, main.Info.SDABase
	m.callGuest(cpu, t, entry, []uint32{slots, slots + 4, slots + 8}, adopt)
}

// allocSystem takes memory for RetroPixels' own use from the system heap.
func (m *Machine) allocSystem(size, align uint32) uint32 {
	p := m.expAlloc(m.heaps.system, size, int32(align))
	if p == 0 {
		panic("wiiu: out of system memory")
	}
	return p
}

func (m *Machine) freeSystem(p uint32) { m.expFree(m.heaps.system, p) }

// defaultHeapFunc reads the function pointer coreinit exports under name
// (MEMAllocFromDefaultHeap, MEMAllocFromDefaultHeapEx or
// MEMFreeToDefaultHeap): the program may have pointed it at its own code.
func (m *Machine) defaultHeapFunc(name string) uint32 {
	return m.Mem.Read32(m.dataExport("coreinit", name))
}

// memBound is OSGetMemBound for MEM1 (1) or the program's MEM2 (2).
func (m *Machine) memBound(which uint32) (addr, size uint32) {
	if which == 1 {
		return MEM1Base, MEM1Size
	}
	if which == 2 {
		lo := m.heaps.mem2Start
		return lo, MEM2End - lo
	}
	return 0, 0
}

// ---- the common heap header ----

// heapHeaderInit writes the common header and links the heap into the list
// it belongs in.
func (m *Machine) heapHeaderInit(heap, tag, first, limit, options uint32) {
	mem := m.Mem
	mem.Fill(heap, 0, hhSize)
	mem.Write32(heap+hhTag, tag)
	mem.Write32(heap+hhFirst, first)
	mem.Write32(heap+hhLimit, limit)
	mem.Write8(heap+hhOptions, uint8(options))
	m.list(heap + hhChildren).init(hhLink)
	m.list(m.parentListOf(heap)).append(heap)
}

// heapUnlink takes a heap out of its list (destroying it).
func (m *Machine) heapUnlink(heap uint32) {
	m.list(m.parentListOf(heap)).remove(heap)
}

func (m *Machine) heapOptions(heap uint32) uint32 { return uint32(m.Mem.Read8(heap + hhOptions)) }

func (m *Machine) heapTagIs(heap, tag uint32) bool {
	return heap != 0 && m.Mem.Read32(heap+hhTag) == tag
}

// heapMark fills n bytes at p with the debug fill value for kind.
func (m *Machine) heapMark(p, n, kind uint32) {
	m.Mem.Fill(p, uint8(m.heaps.marks[kind]), n)
}

// innermostHeap searches list (and the heaps inside each heap in it) for
// the deepest heap whose range holds p.
func (m *Machine) innermostHeap(list, p uint32) uint32 {
	l := m.list(list)
	for heap := l.next(0); heap != 0; heap = l.next(heap) {
		if p < m.Mem.Read32(heap+hhFirst) || p >= m.Mem.Read32(heap+hhLimit) {
			continue
		}
		if inner := m.innermostHeap(heap+hhChildren, p); inner != 0 {
			return inner
		}
		return heap
	}
	return 0
}

// areaListOf is the top-level list for memory at p.
func (m *Machine) areaListOf(p uint32) uint32 {
	h := m.heaps
	if p >= MEM1Base && p-MEM1Base < MEM1Size {
		return h.areaLists[1]
	}
	if p >= FGBase && p-FGBase < FGSize {
		return h.areaLists[2]
	}
	return h.areaLists[0]
}

// parentListOf is the list a heap at p goes in: the child list of the heap
// holding p, or the area's list.
func (m *Machine) parentListOf(p uint32) uint32 {
	top := m.areaListOf(p)
	if outer := m.innermostHeap(top, p); outer != 0 {
		return outer + hhChildren
	}
	return top
}

// setBaseHeap is MEMSetBaseHeapHandle: an arena's base heap is set once;
// later calls change nothing and get the one already there.
func (m *Machine) setBaseHeap(arena, heap uint32) uint32 {
	if arena >= arenaCount {
		return 0
	}
	h := m.heaps
	if cur := h.arenas[arena]; cur != 0 {
		return cur
	}
	h.arenas[arena] = heap
	return 0
}

// ---- MEMList ----

// memList is a view of a MEMList in the program's memory.
type memList struct {
	mem  *Memory
	addr uint32
}

func (m *Machine) list(addr uint32) memList { return memList{m.Mem, addr} }

func (l memList) init(linkOffset uint32) {
	l.mem.Write32(l.addr+mlHead, 0)
	l.mem.Write32(l.addr+mlTail, 0)
	l.mem.Write16(l.addr+mlCount, 0)
	l.mem.Write16(l.addr+mlLinkOff, uint16(linkOffset))
}

// link is the address of obj's MEMLink.
func (l memList) link(obj uint32) uint32 { return obj + uint32(l.mem.Read16(l.addr+mlLinkOff)) }

func (l memList) head() uint32 { return l.mem.Read32(l.addr + mlHead) }
func (l memList) tail() uint32 { return l.mem.Read32(l.addr + mlTail) }

func (l memList) setLinks(obj, prev, next uint32) {
	k := l.link(obj)
	l.mem.Write32(k+lkPrev, prev)
	l.mem.Write32(k+lkNext, next)
}

func (l memList) addCount(d int) {
	l.mem.Write16(l.addr+mlCount, uint16(int(l.mem.Read16(l.addr+mlCount))+d))
}

// only makes obj the one object of an empty list.
func (l memList) only(obj uint32) {
	l.setLinks(obj, 0, 0)
	l.addCount(1)
	l.mem.Write32(l.addr+mlHead, obj)
	l.mem.Write32(l.addr+mlTail, obj)
}

func (l memList) append(obj uint32) {
	if l.head() == 0 {
		l.only(obj)
		return
	}
	last := l.tail()
	l.addCount(1)
	l.setLinks(obj, last, 0)
	l.mem.Write32(l.link(last)+lkNext, obj)
	l.mem.Write32(l.addr+mlTail, obj)
}

func (l memList) prepend(obj uint32) {
	first := l.head()
	if first == 0 {
		l.only(obj)
		return
	}
	l.addCount(1)
	l.setLinks(obj, 0, first)
	l.mem.Write32(l.link(first)+lkPrev, obj)
	l.mem.Write32(l.addr+mlHead, obj)
}

// insertBefore puts obj in front of at (at the end when at is 0).
func (l memList) insertBefore(at, obj uint32) {
	if at == 0 {
		l.append(obj)
		return
	}
	before := l.mem.Read32(l.link(at) + lkPrev)
	if before == 0 {
		l.prepend(obj)
		return
	}
	l.setLinks(obj, before, at)
	l.mem.Write32(l.link(before)+lkNext, obj)
	l.mem.Write32(l.link(at)+lkPrev, obj)
	l.addCount(1)
}

func (l memList) remove(obj uint32) {
	k := l.link(obj)
	before, after := l.mem.Read32(k+lkPrev), l.mem.Read32(k+lkNext)
	if before == 0 {
		l.mem.Write32(l.addr+mlHead, after)
	} else {
		l.mem.Write32(l.link(before)+lkNext, after)
	}
	if after == 0 {
		l.mem.Write32(l.addr+mlTail, before)
	} else {
		l.mem.Write32(l.link(after)+lkPrev, before)
	}
	l.setLinks(obj, 0, 0)
	l.addCount(-1)
}

// next is the object after obj (the first when obj is 0); prev likewise.
func (l memList) next(obj uint32) uint32 {
	if obj == 0 {
		return l.head()
	}
	return l.mem.Read32(l.link(obj) + lkNext)
}

func (l memList) prev(obj uint32) uint32 {
	if obj == 0 {
		return l.tail()
	}
	return l.mem.Read32(l.link(obj) + lkPrev)
}

// ---- allocators ----

// heapAlloc allocates from a heap of any kind, the way its allocator would.
func (m *Machine) heapAlloc(heap, size uint32, align int32) uint32 {
	switch m.Mem.Read32(heap + hhTag) {
	case tagExpHeap:
		return m.expAlloc(heap, size, align)
	case tagFrmHeap:
		return m.frmAlloc(heap, size, align)
	case tagUnitHeap:
		return m.unitAlloc(heap)
	case tagBlockHeap:
		return m.blockAlloc(heap, size, align)
	}
	return 0
}

// allocatorTable is the { alloc, free } function pair shared by the
// allocators of one kind, made the first time it's needed.
func (m *Machine) allocatorTable(kind string) uint32 {
	h := m.heaps
	if p, ok := h.funcTables[kind]; ok {
		return p
	}
	p := m.sysAlloc(8, 8)
	m.Mem.Write32(p, m.thunk("coreinit", "__RPAllocator"+kind+"Alloc"))
	m.Mem.Write32(p+4, m.thunk("coreinit", "__RPAllocator"+kind+"Free"))
	h.funcTables[kind] = p
	return p
}

func (m *Machine) setAllocator(a uint32, kind string, heap, param uint32) {
	mem := m.Mem
	mem.Write32(a+maFuncs, m.allocatorTable(kind))
	mem.Write32(a+maHeap, heap)
	mem.Write32(a+maArg, param)
	mem.Write32(a+maArg+4, 0)
}

// allocatorHeapArg reads the heap and the first parameter of the allocator
// at a.
func (m *Machine) allocatorHeapArg(a uint32) (heap uint32, param int32) {
	return m.Mem.Read32(a + maHeap), int32(m.Mem.Read32(a + maArg))
}

func init() {
	c := "coreinit"

	// base heaps and fill values
	reg(c, "MEMGetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		var heap uint32
		if arena := arg(cpu, 0); arena < arenaCount {
			heap = m.heaps.arenas[arena]
		}
		ret(cpu, heap)
	})
	reg(c, "MEMSetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.setBaseHeap(arg(cpu, 0), arg(cpu, 1)))
	})
	reg(c, "MEMGetArena", func(m *Machine, cpu *CPU, t *Thread) {
		heap := arg(cpu, 0)
		arena := uint32(arenaInvalid)
		for i := uint32(0); heap != 0 && i < arenaCount; i++ {
			if m.heaps.arenas[i] == heap {
				arena = i
				break
			}
		}
		ret(cpu, arena)
	})
	reg(c, "MEMFindContainHeap", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		ret(cpu, m.innermostHeap(m.areaListOf(p), p))
	})
	reg(c, "MEMFindParentHeap", func(m *Machine, cpu *CPU, t *Thread) {
		l := m.parentListOf(arg(cpu, 0))
		parent := l - hhChildren
		for _, top := range m.heaps.areaLists {
			if top == l {
				parent = 0
			}
		}
		ret(cpu, parent)
	})
	reg(c, "MEMGetFillValForHeap", func(m *Machine, cpu *CPU, t *Thread) {
		var v uint32
		if kind := arg(cpu, 0); kind < uint32(len(m.heaps.marks)) {
			v = m.heaps.marks[kind]
		}
		ret(cpu, v)
	})
	reg(c, "MEMSetFillValForHeap", func(m *Machine, cpu *CPU, t *Thread) {
		kind := arg(cpu, 0)
		if kind >= uint32(len(m.heaps.marks)) {
			ret(cpu, 0)
			return
		}
		old := m.heaps.marks[kind]
		m.heaps.marks[kind] = arg(cpu, 1)
		ret(cpu, old)
	})
	// (memory, size): a heap the program manages itself, header only
	reg(c, "MEMCreateUserHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		p, size := arg(cpu, 0), arg(cpu, 1)
		m.heapHeaderInit(p, tagUserHeap, p+0x40, p+size, 0)
		ret(cpu, p)
	})
	reg(c, "MEMDumpHeap", func(m *Machine, cpu *CPU, t *Thread) {})

	// lists
	reg(c, "MEMInitList", func(m *Machine, cpu *CPU, t *Thread) { m.list(arg(cpu, 0)).init(arg(cpu, 1)) })
	reg(c, "MEMAppendListObject", func(m *Machine, cpu *CPU, t *Thread) { m.list(arg(cpu, 0)).append(arg(cpu, 1)) })
	reg(c, "MEMPrependListObject", func(m *Machine, cpu *CPU, t *Thread) { m.list(arg(cpu, 0)).prepend(arg(cpu, 1)) })
	reg(c, "MEMInsertListObject", func(m *Machine, cpu *CPU, t *Thread) {
		m.list(arg(cpu, 0)).insertBefore(arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "MEMRemoveListObject", func(m *Machine, cpu *CPU, t *Thread) { m.list(arg(cpu, 0)).remove(arg(cpu, 1)) })
	reg(c, "MEMGetNextListObject", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.list(arg(cpu, 0)).next(arg(cpu, 1))) })
	reg(c, "MEMGetPrevListObject", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.list(arg(cpu, 0)).prev(arg(cpu, 1))) })
	reg(c, "MEMGetNthListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l, n := m.list(arg(cpu, 0)), arg(cpu, 1)
		obj := l.head()
		for ; n > 0 && obj != 0; n-- {
			obj = l.next(obj)
		}
		ret(cpu, obj)
	})

	// The default heap. Programs reach these through the exported function
	// pointers of the same names, which they may point elsewhere.
	reg(c, "MEMAllocFromDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.defaultHeap, arg(cpu, 0), 0x40))
	})
	reg(c, "MEMAllocFromDefaultHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.defaultHeap, arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "MEMFreeToDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) { m.expFree(m.heaps.defaultHeap, arg(cpu, 0)) })

	// The loader's own allocator, (size, align, where to put the address),
	// which goes through the default heap pointers.
	reg(c, "__RPDynLoadAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		size, align, out := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMAllocFromDefaultHeapEx"), []uint32{size, align},
			func(m *Machine, cpu *CPU, t *Thread, p uint32) {
				if out != 0 {
					m.Mem.Write32(out, p)
				}
				ret(cpu, 0)
			})
	})
	reg(c, "__RPDynLoadFree", func(m *Machine, cpu *CPU, t *Thread) {
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMFreeToDefaultHeap"), []uint32{arg(cpu, 0)},
			func(m *Machine, cpu *CPU, t *Thread, _ uint32) { ret(cpu, 0) })
	})

	// the system heap and memory bounds
	reg(c, "OSAllocFromSystem", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.system, arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "OSFreeToSystem", func(m *Machine, cpu *CPU, t *Thread) { m.freeSystem(arg(cpu, 0)) })
	// (type, u32 *addr, u32 *size)
	reg(c, "OSGetMemBound", func(m *Machine, cpu *CPU, t *Thread) {
		addr, size := m.memBound(arg(cpu, 0))
		if size == 0 {
			ret(cpu, 0xffffffff)
			return
		}
		for i, v := range [2]uint32{addr, size} {
			if p := arg(cpu, 1+i); p != 0 {
				m.Mem.Write32(p, v)
			}
		}
		ret(cpu, 0)
	})

	// allocators
	reg(c, "MEMInitAllocatorForDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.setAllocator(arg(cpu, 0), "Default", m.heaps.arenas[arenaMEM2], 0)
	})
	// the default allocator's functions get (allocator, size or pointer)
	// and go on to the default heap pointers with the second argument
	viaDefault := func(name string) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			cpu.GPR[3] = cpu.GPR[4]
			cpu.PC = m.defaultHeapFunc(name)
			m.jumped = true
		}
	}
	reg(c, "__RPAllocatorDefaultAlloc", viaDefault("MEMAllocFromDefaultHeap"))
	reg(c, "__RPAllocatorDefaultFree", viaDefault("MEMFreeToDefaultHeap"))
	// MEMAllocFromAllocator(allocator, size) and MEMFreeToAllocator(allocator,
	// p) jump to the allocator's own functions with the same arguments
	viaTable := func(slot uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			table := m.Mem.Read32(arg(cpu, 0) + maFuncs)
			cpu.PC = m.Mem.Read32(table + slot)
			m.jumped = true
		}
	}
	reg(c, "MEMAllocFromAllocator", viaTable(0))
	reg(c, "MEMFreeToAllocator", viaTable(4))
}
