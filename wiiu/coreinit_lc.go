package wiiu

// The locked cache: 16 KB of each core's cache that a program can use as
// fast scratch memory, filled and emptied by DMA in 32-byte blocks. Here it
// is plain memory, and the DMA copies happen at once.

const (
	lcSize  = 0x4000
	lcUnit  = 0x200 // allocations are made in 512-byte pieces
	lcUnits = lcSize / lcUnit
)

type lockedCache struct {
	base uint32
	used [lcUnits]uint8 // at the start of each allocation, its length in pieces
	dma  bool
}

func (m *Machine) lc(core int) *lockedCache {
	l := &m.lcache[core]
	if l.base == 0 {
		l.base = m.sysAlloc(lcSize, lcSize)
	}
	return l
}

// taken reports which pieces are in use.
func (l *lockedCache) taken() (t [lcUnits]bool) {
	for i, n := range l.used {
		for j := 0; j < int(n); j++ {
			t[i+j] = true
		}
	}
	return
}

func (l *lockedCache) alloc(size uint32) uint32 {
	n := int((size + lcUnit - 1) / lcUnit)
	if n == 0 || n > lcUnits {
		return 0
	}
	t := l.taken()
	for i := 0; i+n <= lcUnits; i++ {
		free := true
		for j := i; j < i+n; j++ {
			if t[j] {
				free = false
				break
			}
		}
		if free {
			l.used[i] = uint8(n)
			return l.base + uint32(i)*lcUnit
		}
	}
	return 0
}

// largest is the biggest allocation that would fit now.
func (l *lockedCache) largest() uint32 {
	t := l.taken()
	best, run := 0, 0
	for _, x := range t {
		if x {
			run = 0
		} else if run++; run > best {
			best = run
		}
	}
	return uint32(best) * lcUnit
}

func (l *lockedCache) free() uint32 {
	n := 0
	for _, x := range l.taken() {
		if !x {
			n++
		}
	}
	return uint32(n) * lcUnit
}

func init() {
	c := "coreinit"
	reg(c, "LCHardwareIsAvailable", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "LCGetMaxSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, lcSize) })
	reg(c, "LCGetAllocatableSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.lc(cpu.Core).largest()) })
	reg(c, "LCGetUnallocated", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.lc(cpu.Core).free()) })
	reg(c, "LCAlloc", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.lc(cpu.Core).alloc(arg(cpu, 0))) })
	reg(c, "LCDealloc", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		for i := range m.lcache {
			l := &m.lcache[i]
			if l.base != 0 && a >= l.base && a < l.base+lcSize && (a-l.base)%lcUnit == 0 {
				l.used[(a-l.base)/lcUnit] = 0
			}
		}
	})
	reg(c, "LCEnableDMA", func(m *Machine, cpu *CPU, t *Thread) { m.lc(cpu.Core).dma = true; ret(cpu, 1) })
	reg(c, "LCDisableDMA", func(m *Machine, cpu *CPU, t *Thread) { m.lc(cpu.Core).dma = false })
	reg(c, "LCIsDMAEnabled", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.lc(cpu.Core).dma) })
	reg(c, "LCGetDMAQueueLength", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "LCWaitDMAQueue", func(m *Machine, cpu *CPU, t *Thread) {})
	// (destination, source, blocks of 32 bytes; 0 means 128)
	blocks := func(n uint32) uint32 {
		if n == 0 {
			n = 128
		}
		return n * 32
	}
	move := func(m *Machine, cpu *CPU) {
		dst, src, n := arg(cpu, 0), arg(cpu, 1), blocks(arg(cpu, 2))
		m.Mem.Move(dst, src, n)
		m.Mem.InvalidateCode(dst, n)
	}
	reg(c, "LCLoadDMABlocks", func(m *Machine, cpu *CPU, t *Thread) { move(m, cpu) })
	reg(c, "LCStoreDMABlocks", func(m *Machine, cpu *CPU, t *Thread) { move(m, cpu) })
}
