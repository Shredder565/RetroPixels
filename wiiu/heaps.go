package wiiu

// coreinit's memory heaps (the MEM* functions): the parts every kind of heap
// shares, the program's base heaps and the system heap.
//
// Programs read heap headers and walk heap lists themselves, so a heap keeps
// its header at its own start, laid out as Cafe OS lays it out: the common
// MEMHeapBase (0x34 bytes) and then the fields of its kind. Heaps nest. A
// heap made inside another heap's memory joins that heap's child list; any
// other heap joins one of three top lists, picked by the memory area it is
// in (MEM1, the foreground bucket, or anywhere else).
//
// Written for RetroPixels from the SDK's published structure layouts and the
// behaviour programs expect of these calls.

const (
	heapMagicUnit  = 'U'<<24 | 'N'<<16 | 'T'<<8 | 'H'
	heapMagicBlock = 'B'<<24 | 'L'<<16 | 'K'<<8 | 'H'
	heapMagicFrame = 'F'<<24 | 'R'<<16 | 'M'<<8 | 'H'
	heapMagicExp   = 'E'<<24 | 'X'<<16 | 'P'<<8 | 'H'
	heapMagicUser  = 'U'<<24 | 'S'<<16 | 'R'<<8 | 'H'

	// MEMHeapBase
	heapMagic     = 0x00
	heapLink      = 0x04 // where the heap sits in its parent's list
	heapChildList = 0x0c // the heaps made inside this one
	heapStart     = 0x18
	heapEnd       = 0x1c // one past the last byte
	heapFlags     = 0x33 // one byte of heapOpt* bits
	heapBaseSize  = 0x34

	heapOptClear = 1 // zero what is handed out
	heapOptFill  = 2 // fill memory with the debug values
	heapOptLock  = 4 // thread safe

	// MEMAllocator: the { alloc, free } pair, the heap and a parameter
	allocatorFuncs = 0x0
	allocatorHeap  = 0x4
	allocatorParam = 0x8

	arenaMEM1    = 0
	arenaMEM2    = 1
	arenaFG      = 8
	arenaCount   = 9
	arenaInvalid = 9

	// which debug fill value
	fillOnCreate = 0
	fillOnAlloc  = 1
	fillOnFree   = 2
)

// memList is a MEMList in the program's memory: the first and last objects,
// then a u16 count and the u16 offset of each object's MEMLink (its previous
// and next object).
type memList struct {
	mem *Memory
	at  uint32
}

const memListSize = 0xc

func (m *Machine) memList(at uint32) memList { return memList{m.Mem, at} }

func (l memList) init(linkAt uint32) {
	l.mem.Write32(l.at, 0)
	l.mem.Write32(l.at+4, 0)
	l.mem.Write16(l.at+8, 0)
	l.mem.Write16(l.at+10, uint16(linkAt))
}

func (l memList) head() uint32             { return l.mem.Read32(l.at) }
func (l memList) tail() uint32             { return l.mem.Read32(l.at + 4) }
func (l memList) linkOf(obj uint32) uint32 { return obj + uint32(l.mem.Read16(l.at+10)) }
func (l memList) count(d int) {
	l.mem.Write16(l.at+8, uint16(int(l.mem.Read16(l.at+8))+d))
}

// after is the object after obj, or the first one when obj is 0.
func (l memList) after(obj uint32) uint32 {
	if obj == 0 {
		return l.head()
	}
	return l.mem.Read32(l.linkOf(obj) + 4)
}

// before is the object before obj, or the last one when obj is 0.
func (l memList) before(obj uint32) uint32 {
	if obj == 0 {
		return l.tail()
	}
	return l.mem.Read32(l.linkOf(obj))
}

// insert puts obj in front of next, or at the end when next is 0.
func (l memList) insert(obj, next uint32) {
	prev := l.before(next)
	lk := l.linkOf(obj)
	l.mem.Write32(lk, prev)
	l.mem.Write32(lk+4, next)
	if prev == 0 {
		l.mem.Write32(l.at, obj)
	} else {
		l.mem.Write32(l.linkOf(prev)+4, obj)
	}
	if next == 0 {
		l.mem.Write32(l.at+4, obj)
	} else {
		l.mem.Write32(l.linkOf(next), obj)
	}
	l.count(1)
}

func (l memList) remove(obj uint32) {
	lk := l.linkOf(obj)
	prev, next := l.mem.Read32(lk), l.mem.Read32(lk+4)
	if prev == 0 {
		l.mem.Write32(l.at, next)
	} else {
		l.mem.Write32(l.linkOf(prev)+4, next)
	}
	if next == 0 {
		l.mem.Write32(l.at+4, prev)
	} else {
		l.mem.Write32(l.linkOf(next), prev)
	}
	l.mem.Write32(lk, 0)
	l.mem.Write32(lk+4, 0)
	l.count(-1)
}

// Heaps is coreinit's heap bookkeeping.
type Heaps struct {
	top         [3]uint32 // the top lists: anywhere else, MEM1, foreground bucket
	base        [arenaCount]uint32
	fill        [3]uint32
	defaultHeap uint32 // what MEMAllocFromDefaultHeap allocates from
	system      uint32 // OSAllocFromSystem's heap, also RetroPixels' own
	mem2Lo      uint32 // where OSGetMemBound says MEM2 starts
	handles     uint32 // the three base heap handles __preinit_user can change
	allocPairs  map[string]uint32
	started     bool // the base heaps are settled (__preinit_user has run)
}

func newHeaps() *Heaps {
	return &Heaps{fill: [3]uint32{0xc3c3c3c3, 0xf3f3f3f3, 0xd3d3d3d3}, allocPairs: map[string]uint32{}}
}

// setup makes the system heap and the base heaps once the program is loaded,
// since MEM2 is what follows its data.
func (h *Heaps) setup(m *Machine) {
	lists := m.sysAlloc(3*memListSize, 4)
	for i := range h.top {
		h.top[i] = lists + uint32(i)*memListSize
		m.memList(h.top[i]).init(heapLink)
	}
	h.mem2Lo = max((m.dataEnd+0xfff)&^0xfff, DataBase)
	m.initForegroundBucket()
	h.system = m.createExpHeap(SysHeapBase, SysHeapSize, heapOptLock)

	// MEM1 and the foreground bucket get frame heaps, MEM2 the default heap
	h.handles = m.sysAlloc(12, 4)
	at, n := m.memBound(1)
	m.Mem.Write32(h.handles, m.createFrmHeap(at, n, 0))
	at, n = m.fgFreeArea()
	m.Mem.Write32(h.handles+4, m.createFrmHeap(at, n, 0))
	at, n = m.memBound(2)
	h.defaultHeap = m.createExpHeap(at, n, heapOptLock)
	m.Mem.Write32(h.handles+8, h.defaultHeap)

	m.dynAlloc = m.thunk("coreinit", "__RPDynLoadAlloc")
	m.dynFree = m.thunk("coreinit", "__RPDynLoadFree")
}

// startHeaps lets the program's __preinit_user, if it exports one, replace
// the base heaps, sets them, and then allocates and frees once through the
// loader's allocator (Watch Dogs sets up its malloc on that first call). It
// returns to cpu.LR.
func (m *Machine) startHeaps(cpu *CPU, t *Thread) {
	h := m.heaps
	h.started = true
	settle := func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
		for i, arena := range [3]uint32{arenaMEM1, arenaFG, arenaMEM2} {
			if heap := m.Mem.Read32(h.handles + uint32(i)*4); heap != 0 {
				m.memSetBaseHeapHandle(arena, heap)
			}
		}
		out := m.sysAlloc(4, 4)
		m.callGuest(cpu, t, m.dynAlloc, []uint32{0x500, 4, out}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
			if p := m.Mem.Read32(out); p != 0 {
				m.callGuest(cpu, t, m.dynFree, []uint32{p}, func(*Machine, *CPU, *Thread, uint32) {})
			}
		})
	}
	main := m.mainModule()
	fn, ok := main.Exports["__preinit_user"]
	if !ok {
		settle(m, cpu, t, 0)
		return
	}
	cpu.GPR[2] = main.Info.SDA2Base
	cpu.GPR[13] = main.Info.SDABase
	m.callGuest(cpu, t, fn, []uint32{h.handles, h.handles + 4, h.handles + 8}, settle)
}

// allocSystem takes memory for RetroPixels' own use from the system heap.
func (m *Machine) allocSystem(size, align uint32) uint32 {
	if p := m.expAlloc(m.heaps.system, size, int32(align)); p != 0 {
		return p
	}
	panic("wiiu: out of system memory")
}

func (m *Machine) freeSystem(p uint32) { m.expFree(m.heaps.system, p) }

// defaultHeapFunc is where the program's MEMAllocFromDefaultHeap,
// MEMAllocFromDefaultHeapEx or MEMFreeToDefaultHeap pointer points: the
// program may have pointed it at its own allocator.
func (m *Machine) defaultHeapFunc(name string) uint32 {
	return m.Mem.Read32(m.dataExport("coreinit", name))
}

// memBound is OSGetMemBound: MEM1, or MEM2 after the loaded modules.
func (m *Machine) memBound(which uint32) (at, size uint32) {
	switch which {
	case 1:
		return MEM1Base, MEM1Size
	case 2:
		return m.heaps.mem2Lo, MEM2End - m.heaps.mem2Lo
	}
	return 0, 0
}

// ---- the common header ----

func (m *Machine) memInitHeapBase(heap, magic, start, end, flags uint32) {
	mem := m.Mem
	mem.Fill(heap, 0, heapBaseSize)
	mem.Write32(heap+heapMagic, magic)
	mem.Write32(heap+heapStart, start)
	mem.Write32(heap+heapEnd, end)
	mem.Write8(heap+heapFlags, uint8(flags))
	m.memList(heap + heapChildList).init(heapLink)
	m.memList(m.parentList(heap)).insert(heap, 0)
}

func (m *Machine) memDestroyHeapBase(heap uint32) {
	m.memList(m.parentList(heap)).remove(heap)
}

func (m *Machine) heapFlagsOf(heap uint32) uint32 { return uint32(m.Mem.Read8(heap + heapFlags)) }

func (m *Machine) heapKind(heap uint32, magic uint32) bool {
	return heap != 0 && m.Mem.Read32(heap+heapMagic) == magic
}

// heapFill fills n bytes at p with the debug value for when.
func (m *Machine) heapFill(p, n, when uint32) {
	m.Mem.Fill(p, uint8(m.heaps.fill[when]), n)
}

// innermostHeap is the deepest heap, in list or nested below it, whose
// memory holds p.
func (m *Machine) innermostHeap(list, p uint32) uint32 {
	l := m.memList(list)
	for heap := l.head(); heap != 0; heap = l.after(heap) {
		if p < m.Mem.Read32(heap+heapStart) || p >= m.Mem.Read32(heap+heapEnd) {
			continue
		}
		if inner := m.innermostHeap(heap+heapChildList, p); inner != 0 {
			return inner
		}
		return heap
	}
	return 0
}

// topList is the top list for memory at p.
func (m *Machine) topList(p uint32) uint32 {
	switch {
	case p >= MEM1Base && p-MEM1Base < MEM1Size:
		return m.heaps.top[1]
	case p >= FGBase && p-FGBase < FGSize:
		return m.heaps.top[2]
	}
	return m.heaps.top[0]
}

// parentList is the list a heap starting at heap belongs to.
func (m *Machine) parentList(heap uint32) uint32 {
	top := m.topList(heap)
	if outer := m.innermostHeap(top, heap); outer != 0 {
		return outer + heapChildList
	}
	return top
}

func (m *Machine) memFindContainHeap(p uint32) uint32 { return m.innermostHeap(m.topList(p), p) }

// memSetBaseHeapHandle sets an arena's heap the first time only; it returns
// the heap already set, or 0.
func (m *Machine) memSetBaseHeapHandle(arena, heap uint32) uint32 {
	if arena >= arenaCount {
		return 0
	}
	if old := m.heaps.base[arena]; old != 0 {
		return old
	}
	m.heaps.base[arena] = heap
	return 0
}

// ---- allocators ----

// heapAlloc allocates from a heap of any kind.
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

// allocatorPair is the { alloc, free } function pair shared by the
// allocators of one kind.
func (m *Machine) allocatorPair(kind string) uint32 {
	if p, ok := m.heaps.allocPairs[kind]; ok {
		return p
	}
	p := m.sysAlloc(8, 8)
	m.Mem.Write32(p, m.thunk("coreinit", "__RPAllocator"+kind+"Alloc"))
	m.Mem.Write32(p+4, m.thunk("coreinit", "__RPAllocator"+kind+"Free"))
	m.heaps.allocPairs[kind] = p
	return p
}

func (m *Machine) initAllocator(a uint32, kind string, heap, param uint32) {
	mem := m.Mem
	mem.Write32(a+allocatorFuncs, m.allocatorPair(kind))
	mem.Write32(a+allocatorHeap, heap)
	mem.Write32(a+allocatorParam, param)
	mem.Write32(a+allocatorParam+4, 0)
}

// allocatorFor registers __RPAllocator<kind>Alloc and Free, which take the
// allocator in r3 and act on its heap.
func allocatorFor(kind string, alloc func(m *Machine, heap, size uint32, param int32) uint32,
	free func(m *Machine, heap, p uint32)) {
	reg("coreinit", "__RPAllocator"+kind+"Alloc", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		ret(cpu, alloc(m, m.Mem.Read32(a+allocatorHeap), arg(cpu, 1), int32(m.Mem.Read32(a+allocatorParam))))
	})
	reg("coreinit", "__RPAllocator"+kind+"Free", func(m *Machine, cpu *CPU, t *Thread) {
		free(m, m.Mem.Read32(arg(cpu, 0)+allocatorHeap), arg(cpu, 1))
	})
}

// onHeap wraps a call taking a heap of one kind in r3: any other heap gets 0.
func onHeap(magic uint32, fn func(m *Machine, cpu *CPU, heap uint32)) hleFunc {
	return func(m *Machine, cpu *CPU, t *Thread) {
		if heap := arg(cpu, 0); m.heapKind(heap, magic) {
			fn(m, cpu, heap)
			return
		}
		ret(cpu, 0)
	}
}

// jumpTo continues at the guest function at fn, with the arguments as they
// are.
func (m *Machine) jumpTo(cpu *CPU, fn uint32) {
	cpu.PC = fn
	m.jumped = true
}

func init() {
	c := "coreinit"

	reg(c, "MEMGetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		var heap uint32
		if a := arg(cpu, 0); a < arenaCount {
			heap = m.heaps.base[a]
		}
		ret(cpu, heap)
	})
	reg(c, "MEMSetBaseHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.memSetBaseHeapHandle(arg(cpu, 0), arg(cpu, 1)))
	})
	reg(c, "MEMGetArena", func(m *Machine, cpu *CPU, t *Thread) {
		heap := arg(cpu, 0)
		arena := uint32(arenaInvalid)
		for i, b := range m.heaps.base {
			if b != 0 && b == heap {
				arena = uint32(i)
				break
			}
		}
		ret(cpu, arena)
	})
	reg(c, "MEMFindContainHeap", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.memFindContainHeap(arg(cpu, 0))) })
	reg(c, "MEMFindParentHeap", func(m *Machine, cpu *CPU, t *Thread) {
		l := m.parentList(arg(cpu, 0))
		parent := l - heapChildList
		for _, top := range m.heaps.top {
			if l == top {
				parent = 0
			}
		}
		ret(cpu, parent)
	})
	reg(c, "MEMGetFillValForHeap", func(m *Machine, cpu *CPU, t *Thread) {
		var v uint32
		if i := arg(cpu, 0); i < 3 {
			v = m.heaps.fill[i]
		}
		ret(cpu, v)
	})
	reg(c, "MEMSetFillValForHeap", func(m *Machine, cpu *CPU, t *Thread) {
		i := arg(cpu, 0)
		if i >= 3 {
			ret(cpu, 0)
			return
		}
		ret(cpu, m.heaps.fill[i])
		m.heaps.fill[i] = arg(cpu, 1)
	})
	reg(c, "MEMCreateUserHeapHandle", func(m *Machine, cpu *CPU, t *Thread) {
		at, size := arg(cpu, 0), arg(cpu, 1)
		m.memInitHeapBase(at, heapMagicUser, at+0x40, at+size, 0)
		ret(cpu, at)
	})
	reg(c, "MEMDumpHeap", func(m *Machine, cpu *CPU, t *Thread) {})

	// lists
	reg(c, "MEMInitList", func(m *Machine, cpu *CPU, t *Thread) { m.memList(arg(cpu, 0)).init(arg(cpu, 1)) })
	reg(c, "MEMAppendListObject", func(m *Machine, cpu *CPU, t *Thread) { m.memList(arg(cpu, 0)).insert(arg(cpu, 1), 0) })
	reg(c, "MEMPrependListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l := m.memList(arg(cpu, 0))
		l.insert(arg(cpu, 1), l.head())
	})
	reg(c, "MEMInsertListObject", func(m *Machine, cpu *CPU, t *Thread) { m.memList(arg(cpu, 0)).insert(arg(cpu, 2), arg(cpu, 1)) })
	reg(c, "MEMRemoveListObject", func(m *Machine, cpu *CPU, t *Thread) { m.memList(arg(cpu, 0)).remove(arg(cpu, 1)) })
	reg(c, "MEMGetNextListObject", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.memList(arg(cpu, 0)).after(arg(cpu, 1))) })
	reg(c, "MEMGetPrevListObject", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.memList(arg(cpu, 0)).before(arg(cpu, 1))) })
	reg(c, "MEMGetNthListObject", func(m *Machine, cpu *CPU, t *Thread) {
		l := m.memList(arg(cpu, 0))
		obj := l.head()
		for n := arg(cpu, 1); n > 0 && obj != 0; n-- {
			obj = l.after(obj)
		}
		ret(cpu, obj)
	})

	// The default heap. Programs reach these through function pointers of
	// the same names, which they may point elsewhere.
	reg(c, "MEMAllocFromDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.defaultHeap, arg(cpu, 0), 0x40))
	})
	reg(c, "MEMAllocFromDefaultHeapEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.defaultHeap, arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "MEMFreeToDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) { m.expFree(m.heaps.defaultHeap, arg(cpu, 0)) })

	// The loader's allocator, (size, align, where to put the address), goes
	// through the default heap pointers.
	reg(c, "__RPDynLoadAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		out := arg(cpu, 2)
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMAllocFromDefaultHeapEx"), []uint32{arg(cpu, 0), arg(cpu, 1)},
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

	reg(c, "OSAllocFromSystem", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.expAlloc(m.heaps.system, arg(cpu, 0), int32(arg(cpu, 1))))
	})
	reg(c, "OSFreeToSystem", func(m *Machine, cpu *CPU, t *Thread) { m.freeSystem(arg(cpu, 0)) })

	reg(c, "OSGetMemBound", func(m *Machine, cpu *CPU, t *Thread) {
		at, n := m.memBound(arg(cpu, 0))
		if n == 0 {
			ret(cpu, 0xffffffff)
			return
		}
		for i, v := range [2]uint32{at, n} {
			if p := arg(cpu, 1+i); p != 0 {
				m.Mem.Write32(p, v)
			}
		}
		ret(cpu, 0)
	})

	// allocators over the default heap call through its pointers, with the
	// size or block moved up into r3
	reg(c, "MEMInitAllocatorForDefaultHeap", func(m *Machine, cpu *CPU, t *Thread) {
		m.initAllocator(arg(cpu, 0), "Default", m.heaps.base[arenaMEM2], 0)
	})
	reg(c, "__RPAllocatorDefaultAlloc", func(m *Machine, cpu *CPU, t *Thread) {
		cpu.GPR[3] = cpu.GPR[4]
		m.jumpTo(cpu, m.defaultHeapFunc("MEMAllocFromDefaultHeap"))
	})
	reg(c, "__RPAllocatorDefaultFree", func(m *Machine, cpu *CPU, t *Thread) {
		cpu.GPR[3] = cpu.GPR[4]
		m.jumpTo(cpu, m.defaultHeapFunc("MEMFreeToDefaultHeap"))
	})
	// MEMAllocFromAllocator(allocator, size) and MEMFreeToAllocator(allocator,
	// p) go straight on to the allocator's own functions
	reg(c, "MEMAllocFromAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		m.jumpTo(cpu, m.Mem.Read32(m.Mem.Read32(arg(cpu, 0)+allocatorFuncs)))
	})
	reg(c, "MEMFreeToAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		m.jumpTo(cpu, m.Mem.Read32(m.Mem.Read32(arg(cpu, 0)+allocatorFuncs)+4))
	})
}
