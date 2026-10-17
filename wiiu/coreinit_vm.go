package wiiu

// Virtual memory: a program can take addresses from the map range and
// point them at memory it already has (by physical address), so the same
// bytes appear in two places. Here the second place shares the first's
// pages. The range sits between MEM2 and the loader's area.

const (
	vmBase = 0xA0000000
	vmSize = 0x20000000
	vmPage = 0x20000 // the console maps in 128 KB steps
)

// Alias makes size bytes at addr the same memory as at src (both multiples
// of the page size), making src's pages if they aren't there yet.
func (m *Memory) Alias(addr, src, size uint32) bool {
	if addr&pageMask != 0 || src&pageMask != 0 {
		return false
	}
	for o := uint32(0); o < size; o += pageSize {
		p := m.page(src + o)
		if p == nil {
			return false
		}
		m.pages[(addr+o)>>pageBits] = p
		if m.jit != nil {
			m.jit.pageAdded((addr+o)>>pageBits, p)
		}
	}
	return true
}

// Unalias forgets the pages at addr.
func (m *Memory) Unalias(addr, size uint32) {
	for o := uint32(0); o < size; o += pageSize {
		m.pages[(addr+o)>>pageBits] = nil
	}
	m.InvalidateCode(addr, size)
}

func init() {
	c := "coreinit"
	// (u32 *start, u32 *size)
	reg(c, "OSGetMapVirtAddrRange", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, vmBase)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, vmSize)
		}
	})
	// physical memory not in use: none here (programs map memory they
	// allocated instead)
	reg(c, "OSGetAvailPhysAddrRange", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, 0)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
	})
	reg(c, "OSGetDataPhysAddrRange", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, toPhysical(DataBase))
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, MEM2End-DataBase)
		}
	})
	// (va or 0, size, align) -> va, or 0
	reg(c, "OSAllocVirtAddr", func(m *Machine, cpu *CPU, t *Thread) {
		va, size, align := arg(cpu, 0), arg(cpu, 1), max(arg(cpu, 2), vmPage)
		size = (size + vmPage - 1) &^ (vmPage - 1)
		if va == 0 {
			va = (max(m.vmNext, vmBase) + align - 1) &^ (align - 1)
		}
		if size == 0 || va < vmBase || va+size > vmBase+vmSize || va+size < va {
			ret(cpu, 0)
			return
		}
		m.vmNext = max(m.vmNext, va+size)
		ret(cpu, va)
	})
	reg(c, "OSFreeVirtAddr", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	// (va, pa, size, permission) -> ok
	reg(c, "OSMapMemory", func(m *Machine, cpu *CPU, t *Thread) {
		va, pa, size := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if va < vmBase || va+size > vmBase+vmSize {
			ret(cpu, 0)
			return
		}
		ok := m.Mem.Alias(va, toEffective(pa), size)
		if ok {
			if m.vmMaps == nil {
				m.vmMaps = map[uint32]uint32{}
			}
			for o := uint32(0); o < size; o += pageSize {
				m.vmMaps[(va+o)>>pageBits] = pa + o
			}
		}
		m.Mem.InvalidateCode(va, size)
		retBool(cpu, ok)
	})
	reg(c, "OSUnmapMemory", func(m *Machine, cpu *CPU, t *Thread) {
		va, size := arg(cpu, 0), arg(cpu, 1)
		if va >= vmBase && va+size <= vmBase+vmSize {
			m.Mem.Unalias(va, size)
			for o := uint32(0); o < size; o += pageSize {
				delete(m.vmMaps, (va+o)>>pageBits)
			}
		}
		ret(cpu, 1)
	})
	// (va) -> pa, or 0 when nothing is mapped there
	reg(c, "OSQueryVirtAddr", func(m *Machine, cpu *CPU, t *Thread) {
		va := arg(cpu, 0)
		if va >= vmBase && va < vmBase+vmSize {
			pa, ok := m.vmMaps[va>>pageBits]
			if !ok {
				ret(cpu, 0)
				return
			}
			ret(cpu, pa+va&pageMask)
			return
		}
		ret(cpu, toPhysical(va))
	})
}
