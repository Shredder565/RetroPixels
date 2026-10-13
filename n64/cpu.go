// Package n64 is RetroPixels' own Nintendo 64 emulator. Original code written
// from the documented behaviour of the console's hardware.
package n64

import (
	"encoding/binary"
	"math/bits"
)

// VR4300 main processor (MIPS III). Registers are 64-bit; the console runs in
// 32-bit addressing mode, so addresses are kept sign-extended.

const (
	excInt    = 0
	excMod    = 1
	excTLBL   = 2
	excTLBS   = 3
	excAdEL   = 4
	excAdES   = 5
	excSys    = 8
	excBp     = 9
	excRI     = 10
	excCpU    = 11
	excOv     = 12
	excTr     = 13
	excFPE    = 15
	statusIE  = 1 << 0
	statusEXL = 1 << 1
	statusERL = 1 << 2
	statusBEV = 1 << 22
	statusFR  = 1 << 26
	statusCU1 = 1 << 29
)

// COP0 register numbers
const (
	c0Index    = 0
	c0Random   = 1
	c0EntryLo0 = 2
	c0EntryLo1 = 3
	c0Context  = 4
	c0PageMask = 5
	c0Wired    = 6
	c0BadVAddr = 8
	c0Count    = 9
	c0EntryHi  = 10
	c0Compare  = 11
	c0Status   = 12
	c0Cause    = 13
	c0EPC      = 14
	c0PRId     = 15
	c0Config   = 16
	c0LLAddr   = 17
	c0XContext = 20
	c0TagLo    = 28
	c0TagHi    = 29
	c0ErrorEPC = 30
)

type tlbEntry struct {
	mask, hi, lo0, lo1 uint64
}

// CPU is the VR4300.
type CPU struct {
	m      *Machine
	ram    *[rdramSize + 8]byte // RDRAM (with slack so 8-byte accesses never run off the end)
	gpr    [32]uint64
	hi, lo uint64
	pc     uint64 // address of the next instruction to execute
	next   uint64 // the one after it (differs from pc+4 after a branch)
	curPC  uint64
	delay  bool // the instruction running now is in a branch delay slot
	nDelay bool // the next one will be
	llbit  bool
	cop0   [32]uint64
	fpr    [32]uint64
	fcr31  uint32
	tlb    [32]tlbEntry
	// timing
	Cycles     uint64 // CPU clock cycles
	countBase  uint64 // cycles at which Count was 0
	compareAt  uint64 // cycle at which Count reaches Compare
	irqPending bool
	// instruction fetch: the 4 KB page of memory the PC is in
	fetchBase uint64
	fetchMem  []byte
	// recently used TLB translations, by virtual 4 KB page
	tlbCache [256]tlbCacheEntry
	tlbDirty bool // the last TLB lookup hit a writable page
}

type tlbCacheEntry struct {
	page  uint64 // virtual page + 1 (0 = empty)
	phys  uint32
	write bool // the page may be written
}

// ioCycles is what an uncached read of an RCP register or the cartridge
// costs the CPU.
const ioCycles = 20

func (c *CPU) reset() {
	*c = CPU{m: c.m, ram: c.ram}
	c.fetchBase = 1
	c.cop0[c0Status] = 0x34000000
	c.cop0[c0Config] = 0x7006e463
	c.cop0[c0PRId] = 0x00000b22
	c.cop0[c0Random] = 31
	c.fcr31 = 0
	c.pc = 0xffffffffbfc00000
	c.next = c.pc + 4
	c.updateCompare()
}

func sx32(v uint32) uint64 { return uint64(int64(int32(v))) }

// ---------------------------------------------------------------------
// address translation
// ---------------------------------------------------------------------

// translate returns the physical address for vaddr. On failure it raises the
// exception and returns ok = false.
func (c *CPU) translate(vaddr uint64, write bool) (p uint32, ok bool) {
	if vaddr+0x80000000 >= 0x40000000 { // not KSEG0 / KSEG1
		return c.translateSlow(vaddr, write)
	}
	return uint32(vaddr) & 0x1fffffff, true
}

//go:noinline
func (c *CPU) translateSlow(vaddr uint64, write bool) (uint32, bool) {
	v := uint32(vaddr)
	if uint64(int64(int32(v))) != vaddr {
		// 64-bit address: only XKPHYS-style direct ranges are handled
		if vaddr>>62 == 2 {
			return uint32(vaddr & 0x1fffffff), true
		}
		c.addressError(vaddr, write)
		return 0, false
	}
	e := &c.tlbCache[(vaddr>>12)&255]
	if e.page == vaddr>>12+1 && (e.write || !write) {
		return e.phys | v&0xfff, true
	}
	p, ok := c.tlbLookup(vaddr, write)
	if ok {
		*e = tlbCacheEntry{vaddr>>12 + 1, p &^ 0xfff, c.tlbDirty}
	}
	return p, ok
}

// flushTLBCache forgets cached translations (after the TLB or ASID changes).
func (c *CPU) flushTLBCache() {
	c.tlbCache = [256]tlbCacheEntry{}
	c.fetchBase = 1
	c.fetchMem = nil
}

func (c *CPU) tlbLookup(vaddr uint64, write bool) (uint32, bool) {
	asid := c.cop0[c0EntryHi] & 0xff
	v := vaddr & 0xffffffff
	for i := range c.tlb {
		e := &c.tlb[i]
		mask := e.mask | 0x1fff
		if (v &^ mask) != (e.hi & 0xffffe000 &^ mask) {
			continue
		}
		global := e.lo0&e.lo1&1 != 0
		if !global && e.hi&0xff != asid {
			continue
		}
		odd := v&((mask+1)>>1) != 0
		lo := e.lo0
		if odd {
			lo = e.lo1
		}
		if lo&2 == 0 { // not valid
			c.tlbException(vaddr, write, false)
			return 0, false
		}
		if write && lo&4 == 0 { // not dirty: write-protected
			c.setBadVAddr(vaddr)
			c.exception(excMod, 0)
			return 0, false
		}
		pfn := (lo >> 6) & 0xfffff
		off := v & (mask >> 1)
		c.tlbDirty = lo&4 != 0
		return uint32(pfn<<12) | uint32(off), true
	}
	c.tlbException(vaddr, write, true)
	return 0, false
}

func (c *CPU) setBadVAddr(vaddr uint64) {
	c.cop0[c0BadVAddr] = vaddr
	vpn2 := (vaddr >> 13) & 0x7ffff
	c.cop0[c0Context] = (c.cop0[c0Context] & 0xffffffffff800000) | vpn2<<4
	c.cop0[c0XContext] = (c.cop0[c0XContext] &^ 0x1fffffff0) | vpn2<<4
	c.cop0[c0EntryHi] = (vaddr & 0xffffffffffffe000) | (c.cop0[c0EntryHi] & 0xff)
}

func (c *CPU) tlbException(vaddr uint64, write, refill bool) {
	c.setBadVAddr(vaddr)
	code := uint32(excTLBL)
	if write {
		code = excTLBS
	}
	vec := uint64(0x180)
	if refill && c.cop0[c0Status]&statusEXL == 0 {
		vec = 0
	}
	c.exception(code, vec)
}

func (c *CPU) addressError(vaddr uint64, write bool) {
	c.cop0[c0BadVAddr] = vaddr
	if write {
		c.exception(excAdES, 0x180)
	} else {
		c.exception(excAdEL, 0x180)
	}
}

// exception enters the handler. vec is the vector offset (0 for TLB refill, 0x180 otherwise).
func (c *CPU) exception(code uint32, vec uint64) {
	if code != excTLBL && code != excTLBS {
		vec = 0x180
	}
	st := c.cop0[c0Status]
	cause := c.cop0[c0Cause] &^ (0x7c | 0x80000000 | 0x30000000)
	cause |= uint64(code) << 2
	if st&statusEXL == 0 {
		if c.delay {
			c.cop0[c0EPC] = c.curPC - 4
			cause |= 0x80000000
		} else {
			c.cop0[c0EPC] = c.curPC
		}
	} else {
		vec = 0x180
	}
	c.cop0[c0Cause] = cause
	c.cop0[c0Status] = st | statusEXL
	base := uint64(0xffffffff80000000)
	if st&statusBEV != 0 {
		base = 0xffffffffbfc00200
	}
	c.pc = base + vec
	c.next = c.pc + 4
	c.nDelay = false
}

func (c *CPU) copUnusable(n uint32) {
	c.cop0[c0Cause] = (c.cop0[c0Cause] &^ 0x30000000) | uint64(n)<<28
	c.exception(excCpU, 0x180)
	c.cop0[c0Cause] = (c.cop0[c0Cause] &^ 0x30000000) | uint64(n)<<28
}

// ---------------------------------------------------------------------
// memory access
// ---------------------------------------------------------------------

// Physical reads and writes. RDRAM is accessed directly; everything else goes
// through the machine's memory map and costs extra time.

func (c *CPU) rd8(p uint32) uint8 {
	if p < rdramSize {
		return c.ram[p]
	}
	return c.rdIO8(p)
}

func (c *CPU) rd16(p uint32) uint16 {
	if p < rdramSize {
		return binary.BigEndian.Uint16(c.ram[p : p+2])
	}
	return c.rdIO16(p)
}

func (c *CPU) rd32(p uint32) uint32 {
	if p < rdramSize {
		return binary.BigEndian.Uint32(c.ram[p : p+4])
	}
	return c.rdIO32(p)
}

func (c *CPU) rd64(p uint32) uint64 {
	if p < rdramSize {
		return binary.BigEndian.Uint64(c.ram[p : p+8])
	}
	return c.rdIO64(p)
}

//go:noinline
func (c *CPU) rdIO8(p uint32) uint8 {
	c.Cycles += ioCycles
	return c.m.read8(p)
}

//go:noinline
func (c *CPU) rdIO16(p uint32) uint16 {
	c.Cycles += ioCycles
	return c.m.read16(p)
}

//go:noinline
func (c *CPU) rdIO32(p uint32) uint32 {
	c.Cycles += ioCycles
	return c.m.read32(p)
}

//go:noinline
func (c *CPU) rdIO64(p uint32) uint64 {
	c.Cycles += ioCycles
	return uint64(c.m.read32(p))<<32 | uint64(c.m.read32(p+4))
}

func (c *CPU) wr8(p uint32, v uint8) {
	if p < rdramSize {
		c.ram[p] = v
		return
	}
	c.m.write8(p, v)
}

func (c *CPU) wr16(p uint32, v uint16) {
	if p < rdramSize {
		binary.BigEndian.PutUint16(c.ram[p:p+2], v)
		return
	}
	c.m.write16(p, v)
}

func (c *CPU) wr32(p uint32, v uint32) {
	if p < rdramSize {
		binary.BigEndian.PutUint32(c.ram[p:p+4], v)
		return
	}
	c.m.write32(p, v, 0xffffffff)
}

func (c *CPU) wr64(p uint32, v uint64) {
	if p < rdramSize {
		binary.BigEndian.PutUint64(c.ram[p:p+8], v)
		return
	}
	c.wrIO64(p, v)
}

//go:noinline
func (c *CPU) wrIO64(p uint32, v uint64) {
	c.m.write32(p, uint32(v>>32), 0xffffffff)
	c.m.write32(p+4, uint32(v), 0xffffffff)
}

// Virtual loads and stores; ok = false means an exception was raised.

func (c *CPU) load8(vaddr uint64) (uint8, bool) {
	p, ok := c.translate(vaddr, false)
	if !ok {
		return 0, false
	}
	return c.rd8(p), true
}

func (c *CPU) load16(vaddr uint64) (uint16, bool) {
	if vaddr&1 != 0 {
		c.addressError(vaddr, false)
		return 0, false
	}
	p, ok := c.translate(vaddr, false)
	if !ok {
		return 0, false
	}
	return c.rd16(p), true
}

func (c *CPU) load32(vaddr uint64) (uint32, bool) {
	if vaddr&3 != 0 {
		c.addressError(vaddr, false)
		return 0, false
	}
	p, ok := c.translate(vaddr, false)
	if !ok {
		return 0, false
	}
	return c.rd32(p), true
}

func (c *CPU) load64(vaddr uint64) (uint64, bool) {
	if vaddr&7 != 0 {
		c.addressError(vaddr, false)
		return 0, false
	}
	p, ok := c.translate(vaddr, false)
	if !ok {
		return 0, false
	}
	return c.rd64(p), true
}

func (c *CPU) store8(vaddr uint64, v uint8) bool {
	p, ok := c.translate(vaddr, true)
	if !ok {
		return false
	}
	c.wr8(p, v)
	return true
}

func (c *CPU) store16(vaddr uint64, v uint16) bool {
	if vaddr&1 != 0 {
		c.addressError(vaddr, true)
		return false
	}
	p, ok := c.translate(vaddr, true)
	if !ok {
		return false
	}
	c.wr16(p, v)
	return true
}

func (c *CPU) store32(vaddr uint64, v uint32) bool {
	if vaddr&3 != 0 {
		c.addressError(vaddr, true)
		return false
	}
	p, ok := c.translate(vaddr, true)
	if !ok {
		return false
	}
	c.wr32(p, v)
	return true
}

func (c *CPU) store64(vaddr uint64, v uint64) bool {
	if vaddr&7 != 0 {
		c.addressError(vaddr, true)
		return false
	}
	p, ok := c.translate(vaddr, true)
	if !ok {
		return false
	}
	c.wr64(p, v)
	return true
}

// ---------------------------------------------------------------------
// timer (Count / Compare)
// ---------------------------------------------------------------------

func (c *CPU) count() uint32 { return uint32((c.Cycles - c.countBase) >> 1) }

func (c *CPU) updateCompare() {
	cur := c.count()
	cmp := uint32(c.cop0[c0Compare])
	delta := uint64(cmp - cur)
	if delta == 0 {
		delta = 1 << 32
	}
	c.compareAt = c.Cycles + delta*2
}

// checkInterrupts raises an interrupt exception when one is pending and enabled.
func (c *CPU) checkInterrupts() bool {
	ip := uint64(0)
	if c.m.miIntr&c.m.miMask != 0 {
		ip |= 1 << 10 // IP2: the RCP
	}
	cause := (c.cop0[c0Cause] &^ (1 << 10)) | ip
	c.cop0[c0Cause] = cause
	st := c.cop0[c0Status]
	if st&statusIE == 0 || st&(statusEXL|statusERL) != 0 {
		return false
	}
	if cause&st&0xff00 == 0 {
		return false
	}
	c.curPC = c.pc
	c.delay = c.nDelay
	c.exception(excInt, 0x180)
	return true
}

// ---------------------------------------------------------------------
// running
// ---------------------------------------------------------------------

// fetch reads the instruction at pc, remembering which memory page it is in
// so the next fetches from that page are quick. ok = false means the fetch
// raised an exception.
func (c *CPU) fetch(pc uint64) (uint32, bool) {
	if off := pc - c.fetchBase; off < 0x1000 {
		return binary.BigEndian.Uint32(c.fetchMem[off:]), true
	}
	c.curPC = pc
	c.delay = c.nDelay
	if pc&3 != 0 {
		c.addressError(pc, false)
		return 0, false
	}
	p, ok := c.translate(pc, false)
	if !ok {
		return 0, false
	}
	base := p &^ 0xfff
	switch {
	case base < rdramSize:
		c.fetchMem = c.m.rdram[base : base+0x1000]
	case base == 0x04000000 || base == 0x04001000: // DMEM / IMEM
		c.fetchMem = c.m.spmem[base&0x1000 : base&0x1000+0x1000]
	default:
		c.fetchBase = 1
		c.Cycles += ioCycles
		return c.m.read32(p), true
	}
	c.fetchBase = pc &^ 0xfff
	return binary.BigEndian.Uint32(c.fetchMem[pc&0xfff:]), true
}

// Step runs one instruction.
func (c *CPU) Step() {
	pc := c.pc
	op, ok := c.fetch(pc)
	if !ok {
		return
	}
	c.curPC = pc
	c.delay = c.nDelay
	c.nDelay = false
	c.pc = c.next
	c.next += 4
	c.exec(op)
	c.gpr[0] = 0
}

// run executes instructions until the cycle counter reaches limit or the
// machine asks the CPU to stop early.
func (c *CPU) run(limit uint64) {
	m := c.m
	for c.Cycles < limit {
		if c.irqPending {
			c.irqPending = false
			c.checkInterrupts()
		}
		pc := c.pc
		var op uint32
		if off := pc - c.fetchBase; off < 0x1000 {
			op = binary.BigEndian.Uint32(c.fetchMem[off:])
		} else {
			var ok bool
			if op, ok = c.fetch(pc); !ok {
				c.Cycles++
				continue
			}
		}
		c.curPC = pc
		c.delay = c.nDelay
		c.nDelay = false
		c.pc = c.next
		c.next += 4
		c.exec(op)
		c.gpr[0] = 0
		c.Cycles++
		if m.stop {
			return
		}
	}
}

func (c *CPU) branch(cond bool, target uint64) {
	c.nDelay = true
	if cond {
		c.next = target
		// "wait here" loop with an empty delay slot: skip ahead to the next event
		if target == c.curPC && c.m.idleSkip {
			if p, ok := c.translate(c.curPC+4, false); ok && p < rdramSize && c.rd32(p) == 0 {
				c.m.idle = true
				c.m.stop = true
			}
		}
	}
}

func (c *CPU) branchLikely(cond bool, target uint64) {
	if cond {
		c.nDelay = true
		c.next = target
	} else { // skip the delay slot
		c.pc = c.next
		c.next += 4
	}
}

func (c *CPU) exec(op uint32) {
	rs := (op >> 21) & 31
	rt := (op >> 16) & 31
	imm := uint64(int64(int16(op)))
	g := &c.gpr
	switch op >> 26 {
	case 0x00:
		c.special(op)
	case 0x01:
		c.regimm(op)
	case 0x02: // J
		c.branch(true, (c.curPC+4)&0xfffffffff0000000|uint64(op&0x3ffffff)<<2)
	case 0x03: // JAL
		g[31] = c.curPC + 8
		c.branch(true, (c.curPC+4)&0xfffffffff0000000|uint64(op&0x3ffffff)<<2)
	case 0x04:
		c.branch(g[rs] == g[rt], c.curPC+4+imm<<2)
	case 0x05:
		c.branch(g[rs] != g[rt], c.curPC+4+imm<<2)
	case 0x06:
		c.branch(int64(g[rs]) <= 0, c.curPC+4+imm<<2)
	case 0x07:
		c.branch(int64(g[rs]) > 0, c.curPC+4+imm<<2)
	case 0x08: // ADDI
		a, b := int32(g[rs]), int32(imm)
		r := a + b
		if (a^r)&(b^r) < 0 {
			c.exception(excOv, 0x180)
			return
		}
		g[rt] = sx32(uint32(r))
	case 0x09:
		g[rt] = sx32(uint32(g[rs]) + uint32(imm))
	case 0x0a:
		if int64(g[rs]) < int64(imm) {
			g[rt] = 1
		} else {
			g[rt] = 0
		}
	case 0x0b:
		if g[rs] < imm {
			g[rt] = 1
		} else {
			g[rt] = 0
		}
	case 0x0c:
		g[rt] = g[rs] & uint64(op&0xffff)
	case 0x0d:
		g[rt] = g[rs] | uint64(op&0xffff)
	case 0x0e:
		g[rt] = g[rs] ^ uint64(op&0xffff)
	case 0x0f:
		g[rt] = sx32(op << 16)
	case 0x10:
		c.cop0op(op)
	case 0x11:
		if c.cop0[c0Status]&statusCU1 == 0 {
			c.copUnusable(1)
			return
		}
		c.cop1op(op)
	case 0x12:
		if c.cop0[c0Status]&(1<<30) == 0 {
			c.copUnusable(2)
			return
		}
		// COP2 doesn't exist on this console
	case 0x14:
		c.branchLikely(g[rs] == g[rt], c.curPC+4+imm<<2)
	case 0x15:
		c.branchLikely(g[rs] != g[rt], c.curPC+4+imm<<2)
	case 0x16:
		c.branchLikely(int64(g[rs]) <= 0, c.curPC+4+imm<<2)
	case 0x17:
		c.branchLikely(int64(g[rs]) > 0, c.curPC+4+imm<<2)
	case 0x18: // DADDI
		a, b := int64(g[rs]), int64(imm)
		r := a + b
		if (a^r)&(b^r) < 0 {
			c.exception(excOv, 0x180)
			return
		}
		g[rt] = uint64(r)
	case 0x19:
		g[rt] = g[rs] + imm
	case 0x1a: // LDL
		addr := g[rs] + imm
		p, ok := c.translate(addr, false)
		if !ok {
			return
		}
		mem := c.rd64(p &^ 7)
		sh := (addr & 7) * 8
		g[rt] = (g[rt] & ((1 << sh) - 1)) | (mem << sh)
	case 0x1b: // LDR
		addr := g[rs] + imm
		p, ok := c.translate(addr, false)
		if !ok {
			return
		}
		mem := c.rd64(p &^ 7)
		sh := (7 - addr&7) * 8
		mask := uint64(0xffffffffffffffff) >> sh
		g[rt] = (g[rt] &^ mask) | (mem >> sh)
	case 0x20: // LB
		addr := g[rs] + imm
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = uint64(int64(int8(c.rd8(p))))
	case 0x21: // LH
		addr := g[rs] + imm
		if addr&1 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = uint64(int64(int16(c.rd16(p))))
	case 0x22: // LWL
		addr := g[rs] + imm
		p, ok := c.translate(addr, false)
		if !ok {
			return
		}
		mem := c.rd32(p &^ 3)
		sh := uint32(addr&3) * 8
		r := (uint32(g[rt]) & ((1 << sh) - 1)) | (mem << sh)
		g[rt] = sx32(r)
	case 0x23: // LW
		addr := g[rs] + imm
		if addr&3 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = sx32(c.rd32(p))
	case 0x24: // LBU
		addr := g[rs] + imm
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = uint64(c.rd8(p))
	case 0x25: // LHU
		addr := g[rs] + imm
		if addr&1 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = uint64(c.rd16(p))
	case 0x26: // LWR
		addr := g[rs] + imm
		p, ok := c.translate(addr, false)
		if !ok {
			return
		}
		mem := c.rd32(p &^ 3)
		sh := (3 - uint32(addr&3)) * 8
		mask := uint32(0xffffffff) >> sh
		r := (uint32(g[rt]) &^ mask) | (mem >> sh)
		g[rt] = sx32(r)
	case 0x27: // LWU
		addr := g[rs] + imm
		if addr&3 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = uint64(c.rd32(p))
	case 0x28: // SB
		addr := g[rs] + imm
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, true); !ok {
				return
			}
		}
		c.wr8(p, uint8(g[rt]))
	case 0x29: // SH
		addr := g[rs] + imm
		if addr&1 != 0 {
			c.addressError(addr, true)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, true); !ok {
				return
			}
		}
		c.wr16(p, uint16(g[rt]))
	case 0x2a: // SWL
		addr := g[rs] + imm
		p, ok := c.translate(addr, true)
		if !ok {
			return
		}
		sh := uint32(addr&3) * 8
		c.m.write32(p&^3, uint32(g[rt])>>sh, 0xffffffff>>sh)
	case 0x2b: // SW
		addr := g[rs] + imm
		if addr&3 != 0 {
			c.addressError(addr, true)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, true); !ok {
				return
			}
		}
		c.wr32(p, uint32(g[rt]))
	case 0x2c: // SDL
		addr := g[rs] + imm
		p, ok := c.translate(addr, true)
		if !ok {
			return
		}
		sh := (addr & 7) * 8
		v := g[rt] >> sh
		mask := uint64(0xffffffffffffffff) >> sh
		c.m.write32(p&^7, uint32(v>>32), uint32(mask>>32))
		c.m.write32(p&^7+4, uint32(v), uint32(mask))
	case 0x2d: // SDR
		addr := g[rs] + imm
		p, ok := c.translate(addr, true)
		if !ok {
			return
		}
		sh := (7 - addr&7) * 8
		v := g[rt] << sh
		mask := uint64(0xffffffffffffffff) << sh
		c.m.write32(p&^7, uint32(v>>32), uint32(mask>>32))
		c.m.write32(p&^7+4, uint32(v), uint32(mask))
	case 0x2e: // SWR
		addr := g[rs] + imm
		p, ok := c.translate(addr, true)
		if !ok {
			return
		}
		sh := (3 - uint32(addr&3)) * 8
		c.m.write32(p&^3, uint32(g[rt])<<sh, 0xffffffff<<sh)
	case 0x2f: // CACHE
	case 0x30: // LL
		addr := g[rs] + imm
		if v, ok := c.load32(addr); ok {
			g[rt] = sx32(v)
			c.llbit = true
			p, _ := c.translate(addr, false)
			c.cop0[c0LLAddr] = uint64(p >> 4)
		}
	case 0x31: // LWC1
		if c.cop0[c0Status]&statusCU1 == 0 {
			c.copUnusable(1)
			return
		}
		addr := g[rs] + imm
		if addr&3 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		c.setFPR32(rt, c.rd32(p))
	case 0x34: // LLD
		addr := g[rs] + imm
		if v, ok := c.load64(addr); ok {
			g[rt] = v
			c.llbit = true
			p, _ := c.translate(addr, false)
			c.cop0[c0LLAddr] = uint64(p >> 4)
		}
	case 0x35: // LDC1
		if c.cop0[c0Status]&statusCU1 == 0 {
			c.copUnusable(1)
			return
		}
		addr := g[rs] + imm
		if addr&7 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		c.setFPR64(rt, c.rd64(p))
	case 0x37: // LD
		addr := g[rs] + imm
		if addr&7 != 0 {
			c.addressError(addr, false)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, false); !ok {
				return
			}
		}
		g[rt] = c.rd64(p)
	case 0x38: // SC
		if c.llbit {
			if c.store32(g[rs]+imm, uint32(g[rt])) {
				g[rt] = 1
			}
		} else {
			g[rt] = 0
		}
	case 0x39: // SWC1
		if c.cop0[c0Status]&statusCU1 == 0 {
			c.copUnusable(1)
			return
		}
		addr := g[rs] + imm
		if addr&3 != 0 {
			c.addressError(addr, true)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, true); !ok {
				return
			}
		}
		c.wr32(p, c.getFPR32(rt))
	case 0x3c: // SCD
		if c.llbit {
			if c.store64(g[rs]+imm, g[rt]) {
				g[rt] = 1
			}
		} else {
			g[rt] = 0
		}
	case 0x3d: // SDC1
		if c.cop0[c0Status]&statusCU1 == 0 {
			c.copUnusable(1)
			return
		}
		addr := g[rs] + imm
		if addr&7 != 0 {
			c.addressError(addr, true)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, true); !ok {
				return
			}
		}
		c.wr64(p, c.getFPR64(rt))
	case 0x3f: // SD
		addr := g[rs] + imm
		if addr&7 != 0 {
			c.addressError(addr, true)
			return
		}
		p := uint32(addr) & 0x1fffffff
		if addr+0x80000000 >= 0x40000000 {
			var ok bool
			if p, ok = c.translateSlow(addr, true); !ok {
				return
			}
		}
		c.wr64(p, g[rt])
	default:
		c.exception(excRI, 0x180)
	}
}

func (c *CPU) special(op uint32) {
	rs := (op >> 21) & 31
	rt := (op >> 16) & 31
	rd := (op >> 11) & 31
	sa := (op >> 6) & 31
	g := &c.gpr
	switch op & 63 {
	case 0x00:
		g[rd] = sx32(uint32(g[rt]) << sa)
	case 0x02:
		g[rd] = sx32(uint32(g[rt]) >> sa)
	case 0x03:
		g[rd] = sx32(uint32(int32(g[rt]) >> sa))
	case 0x04:
		g[rd] = sx32(uint32(g[rt]) << (g[rs] & 31))
	case 0x06:
		g[rd] = sx32(uint32(g[rt]) >> (g[rs] & 31))
	case 0x07:
		g[rd] = sx32(uint32(int32(g[rt]) >> (g[rs] & 31)))
	case 0x08: // JR
		c.branch(true, g[rs])
	case 0x09: // JALR
		t := g[rs]
		g[rd] = c.curPC + 8
		c.branch(true, t)
	case 0x0c:
		c.exception(excSys, 0x180)
	case 0x0d:
		c.exception(excBp, 0x180)
	case 0x0f: // SYNC
	case 0x10:
		g[rd] = c.hi
	case 0x11:
		c.hi = g[rs]
	case 0x12:
		g[rd] = c.lo
	case 0x13:
		c.lo = g[rs]
	case 0x14:
		g[rd] = g[rt] << (g[rs] & 63)
	case 0x16:
		g[rd] = g[rt] >> (g[rs] & 63)
	case 0x17:
		g[rd] = uint64(int64(g[rt]) >> (g[rs] & 63))
	case 0x18: // MULT
		r := int64(int32(g[rs])) * int64(int32(g[rt]))
		c.lo = sx32(uint32(r))
		c.hi = sx32(uint32(r >> 32))
		c.Cycles += 4
	case 0x19: // MULTU
		r := uint64(uint32(g[rs])) * uint64(uint32(g[rt]))
		c.lo = sx32(uint32(r))
		c.hi = sx32(uint32(r >> 32))
		c.Cycles += 4
	case 0x1a: // DIV
		n, d := int32(g[rs]), int32(g[rt])
		if d == 0 {
			if n >= 0 {
				c.lo = 0xffffffffffffffff
			} else {
				c.lo = 1
			}
			c.hi = sx32(uint32(n))
		} else if n == -0x80000000 && d == -1 {
			c.lo = sx32(uint32(n))
			c.hi = 0
		} else {
			c.lo = sx32(uint32(n / d))
			c.hi = sx32(uint32(n % d))
		}
		c.Cycles += 36
	case 0x1b: // DIVU
		n, d := uint32(g[rs]), uint32(g[rt])
		if d == 0 {
			c.lo = 0xffffffffffffffff
			c.hi = sx32(n)
		} else {
			c.lo = sx32(n / d)
			c.hi = sx32(n % d)
		}
		c.Cycles += 36
	case 0x1c: // DMULT
		a, b := int64(g[rs]), int64(g[rt])
		hi, lo := bits.Mul64(uint64(a), uint64(b))
		if a < 0 {
			hi -= uint64(b)
		}
		if b < 0 {
			hi -= uint64(a)
		}
		c.hi, c.lo = hi, lo
		c.Cycles += 7
	case 0x1d: // DMULTU
		c.hi, c.lo = bits.Mul64(g[rs], g[rt])
		c.Cycles += 7
	case 0x1e: // DDIV
		n, d := int64(g[rs]), int64(g[rt])
		if d == 0 {
			if n >= 0 {
				c.lo = 0xffffffffffffffff
			} else {
				c.lo = 1
			}
			c.hi = uint64(n)
		} else if n == -0x8000000000000000 && d == -1 {
			c.lo = uint64(n)
			c.hi = 0
		} else {
			c.lo = uint64(n / d)
			c.hi = uint64(n % d)
		}
		c.Cycles += 68
	case 0x1f: // DDIVU
		n, d := g[rs], g[rt]
		if d == 0 {
			c.lo = 0xffffffffffffffff
			c.hi = n
		} else {
			c.lo = n / d
			c.hi = n % d
		}
		c.Cycles += 68
	case 0x20: // ADD
		a, b := int32(g[rs]), int32(g[rt])
		r := a + b
		if (a^r)&(b^r) < 0 {
			c.exception(excOv, 0x180)
			return
		}
		g[rd] = sx32(uint32(r))
	case 0x21:
		g[rd] = sx32(uint32(g[rs]) + uint32(g[rt]))
	case 0x22: // SUB
		a, b := int32(g[rs]), int32(g[rt])
		r := a - b
		if (a^b)&(a^r) < 0 {
			c.exception(excOv, 0x180)
			return
		}
		g[rd] = sx32(uint32(r))
	case 0x23:
		g[rd] = sx32(uint32(g[rs]) - uint32(g[rt]))
	case 0x24:
		g[rd] = g[rs] & g[rt]
	case 0x25:
		g[rd] = g[rs] | g[rt]
	case 0x26:
		g[rd] = g[rs] ^ g[rt]
	case 0x27:
		g[rd] = ^(g[rs] | g[rt])
	case 0x2a:
		if int64(g[rs]) < int64(g[rt]) {
			g[rd] = 1
		} else {
			g[rd] = 0
		}
	case 0x2b:
		if g[rs] < g[rt] {
			g[rd] = 1
		} else {
			g[rd] = 0
		}
	case 0x2c: // DADD
		a, b := int64(g[rs]), int64(g[rt])
		r := a + b
		if (a^r)&(b^r) < 0 {
			c.exception(excOv, 0x180)
			return
		}
		g[rd] = uint64(r)
	case 0x2d:
		g[rd] = g[rs] + g[rt]
	case 0x2e: // DSUB
		a, b := int64(g[rs]), int64(g[rt])
		r := a - b
		if (a^b)&(a^r) < 0 {
			c.exception(excOv, 0x180)
			return
		}
		g[rd] = uint64(r)
	case 0x2f:
		g[rd] = g[rs] - g[rt]
	case 0x30: // TGE
		if int64(g[rs]) >= int64(g[rt]) {
			c.exception(excTr, 0x180)
		}
	case 0x31:
		if g[rs] >= g[rt] {
			c.exception(excTr, 0x180)
		}
	case 0x32:
		if int64(g[rs]) < int64(g[rt]) {
			c.exception(excTr, 0x180)
		}
	case 0x33:
		if g[rs] < g[rt] {
			c.exception(excTr, 0x180)
		}
	case 0x34:
		if g[rs] == g[rt] {
			c.exception(excTr, 0x180)
		}
	case 0x36:
		if g[rs] != g[rt] {
			c.exception(excTr, 0x180)
		}
	case 0x38:
		g[rd] = g[rt] << sa
	case 0x3a:
		g[rd] = g[rt] >> sa
	case 0x3b:
		g[rd] = uint64(int64(g[rt]) >> sa)
	case 0x3c:
		g[rd] = g[rt] << (sa + 32)
	case 0x3e:
		g[rd] = g[rt] >> (sa + 32)
	case 0x3f:
		g[rd] = uint64(int64(g[rt]) >> (sa + 32))
	default:
		c.exception(excRI, 0x180)
	}
}

func (c *CPU) regimm(op uint32) {
	rs := (op >> 21) & 31
	imm := uint64(int64(int16(op)))
	v := int64(c.gpr[rs])
	target := c.curPC + 4 + imm<<2
	switch (op >> 16) & 31 {
	case 0x00:
		c.branch(v < 0, target)
	case 0x01:
		c.branch(v >= 0, target)
	case 0x02:
		c.branchLikely(v < 0, target)
	case 0x03:
		c.branchLikely(v >= 0, target)
	case 0x08:
		if v >= int64(imm) {
			c.exception(excTr, 0x180)
		}
	case 0x09:
		if uint64(v) >= imm {
			c.exception(excTr, 0x180)
		}
	case 0x0a:
		if v < int64(imm) {
			c.exception(excTr, 0x180)
		}
	case 0x0b:
		if uint64(v) < imm {
			c.exception(excTr, 0x180)
		}
	case 0x0c:
		if v == int64(imm) {
			c.exception(excTr, 0x180)
		}
	case 0x0e:
		if v != int64(imm) {
			c.exception(excTr, 0x180)
		}
	case 0x10:
		c.gpr[31] = c.curPC + 8
		c.branch(v < 0, target)
	case 0x11:
		c.gpr[31] = c.curPC + 8
		c.branch(v >= 0, target)
	case 0x12:
		c.gpr[31] = c.curPC + 8
		c.branchLikely(v < 0, target)
	case 0x13:
		c.gpr[31] = c.curPC + 8
		c.branchLikely(v >= 0, target)
	default:
		c.exception(excRI, 0x180)
	}
}

// ---------------------------------------------------------------------
// COP0
// ---------------------------------------------------------------------

func (c *CPU) readCop0(r uint32) uint64 {
	switch r {
	case c0Count:
		return uint64(c.count())
	case c0Random:
		w := uint32(c.cop0[c0Wired]) & 31
		if w >= 31 {
			return 31
		}
		span := uint64(32 - w)
		return uint64(w) + (c.Cycles/2)%span
	case c0Cause:
		c.checkCompare()
		return c.cop0[r]
	}
	return c.cop0[r]
}

func (c *CPU) checkCompare() {
	if c.Cycles >= c.compareAt {
		c.cop0[c0Cause] |= 1 << 15
		for c.compareAt <= c.Cycles {
			c.compareAt += 1 << 33
		}
	}
}

func (c *CPU) writeCop0(r uint32, v uint64) {
	switch r {
	case c0Count:
		c.countBase = c.Cycles - uint64(uint32(v))*2
		c.updateCompare()
	case c0Compare:
		c.cop0[r] = uint64(uint32(v))
		c.cop0[c0Cause] &^= 1 << 15
		c.updateCompare()
	case c0Status:
		c.cop0[r] = v & 0xff57ffff
		c.irqPending = true // interrupts may have just been enabled
	case c0Cause:
		c.cop0[r] = (c.cop0[r] &^ 0x300) | (v & 0x300)
		c.irqPending = true // software interrupts
	case c0Index:
		c.cop0[r] = (c.cop0[r] & 0x80000000) | (v & 0x3f)
	case c0Random, c0PRId, c0BadVAddr:
	case c0EntryHi:
		if (c.cop0[r]^v)&0xff != 0 {
			c.flushTLBCache() // new address space
		}
		c.cop0[r] = v & 0xc00000ffffffe0ff
	case c0EntryLo0, c0EntryLo1:
		c.cop0[r] = v & 0x3fffffff
	case c0PageMask:
		c.cop0[r] = v & 0x01ffe000
	case c0Wired:
		c.cop0[r] = v & 63
	case c0Context:
		c.cop0[r] = (c.cop0[r] & 0x7ffff0) | (v &^ 0x7fffff)
	case c0Config:
		c.cop0[r] = (c.cop0[r] &^ 0x0f00800f) | (v & 0x0f00800f)
	default:
		c.cop0[r] = v
	}
}

func (c *CPU) cop0op(op uint32) {
	rt := (op >> 16) & 31
	rd := (op >> 11) & 31
	switch (op >> 21) & 31 {
	case 0x00: // MFC0
		c.gpr[rt] = sx32(uint32(c.readCop0(rd)))
	case 0x01: // DMFC0
		c.gpr[rt] = c.readCop0(rd)
	case 0x04: // MTC0
		c.writeCop0(rd, sx32(uint32(c.gpr[rt])))
	case 0x05: // DMTC0
		c.writeCop0(rd, c.gpr[rt])
	case 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f:
		switch op & 63 {
		case 0x01: // TLBR
			e := c.tlb[c.cop0[c0Index]&31]
			c.cop0[c0PageMask] = e.mask
			g := e.lo0 & e.lo1 & 1
			c.cop0[c0EntryHi] = e.hi &^ 0x1000
			c.cop0[c0EntryLo0] = (e.lo0 &^ 1) | g
			c.cop0[c0EntryLo1] = (e.lo1 &^ 1) | g
		case 0x02: // TLBWI
			c.writeTLB(int(c.cop0[c0Index] & 31))
		case 0x06: // TLBWR
			c.writeTLB(int(c.readCop0(c0Random)))
		case 0x08: // TLBP
			c.cop0[c0Index] = 0x80000000
			hi := c.cop0[c0EntryHi]
			for i := range c.tlb {
				e := &c.tlb[i]
				mask := e.mask | 0x1fff
				if (hi&0xffffe000&^mask) == (e.hi&0xffffe000&^mask) && (e.lo0&e.lo1&1 != 0 || e.hi&0xff == hi&0xff) {
					c.cop0[c0Index] = uint64(i)
					break
				}
			}
		case 0x18: // ERET
			st := c.cop0[c0Status]
			if st&statusERL != 0 {
				c.pc = c.cop0[c0ErrorEPC]
				c.cop0[c0Status] = st &^ statusERL
			} else {
				c.pc = c.cop0[c0EPC]
				c.cop0[c0Status] = st &^ statusEXL
			}
			c.next = c.pc + 4
			c.llbit = false
			c.irqPending = true
		}
	}
}

func (c *CPU) writeTLB(i int) {
	c.tlb[i] = tlbEntry{
		mask: c.cop0[c0PageMask],
		hi:   c.cop0[c0EntryHi] &^ (c.cop0[c0PageMask] | 0x1f00),
		lo0:  c.cop0[c0EntryLo0],
		lo1:  c.cop0[c0EntryLo1],
	}
	c.flushTLBCache()
}

// PC returns the address of the next instruction (for debugging).
func (c *CPU) PC() uint64 { return c.pc }
