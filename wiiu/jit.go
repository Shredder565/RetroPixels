package wiiu

// The CPU recompiler: PowerPC code is translated, a block at a time, into
// x86-64 code that does the same thing to the Context, and run directly.
// Instructions it doesn't translate (library calls, floating point for now,
// rare system instructions) end the block and run in the interpreter, then
// the recompiled code carries on.
//
// While recompiled code runs these x86 registers are fixed:
//
//	RBX  the Context
//	R12  the guest memory table: for each 64 KB page, its host address or 0
//	R13  the block table: for each 64 KB page, the address of a table of
//	     16384 block entry points (one per instruction) or 0
//	R9   the instruction budget left (signed)
//
// Each block starts by checking the budget, then subtracts its length. A
// block ends by putting the next PC in EAX and jumping to the dispatcher,
// which jumps to that block or returns to Go to have it compiled. Returning
// to Go (RET) leaves the reason in EAX and the PC in the Context.
//
// The parts that need the host (executable memory and the call into it)
// are in jit_amd64.go; elsewhere the interpreter runs everything.

import (
	"runtime"
	"unsafe"
)

const (
	jitExitBudget  = 1 // the budget ran out (PC = the next block)
	jitExitCompile = 2 // no code for PC yet
	jitExitInterp  = 3 // run the instruction at PC in the interpreter

	jitMaxBlock = 128 // instructions per block
	jitBufSize  = 32 << 20
)

var (
	offGPR   = int32(unsafe.Offsetof(Context{}.GPR))
	offCR    = int32(unsafe.Offsetof(Context{}.CR))
	offLR    = int32(unsafe.Offsetof(Context{}.LR))
	offCTR   = int32(unsafe.Offsetof(Context{}.CTR))
	offXER   = int32(unsafe.Offsetof(Context{}.XER))
	offPC    = int32(unsafe.Offsetof(Context{}.PC))
	offFPR   = int32(unsafe.Offsetof(Context{}.FPR))
	offPS1   = int32(unsafe.Offsetof(Context{}.PS1))
	offFPSCR = int32(unsafe.Offsetof(Context{}.FPSCR))
)

func gpr(r uint32) int32 { return offGPR + int32(r)*4 }

type blockPage [pageSize / 4]uintptr

// JIT is the recompiler's state, shared by the three cores.
type JIT struct {
	mem      *Memory
	buf      []byte // executable memory
	base     uintptr
	used     int
	dispatch uintptr

	pages  [numPages]*blockPage // Go's references to the block tables
	table  [numPages]uintptr    // what the code reads
	hostMn [numPages]uintptr    // guest page -> host address

	Blocks, Flushes int // counters for tests and tuning
}

// jitArgs is what jitCall loads into the fixed registers.
type jitArgs struct {
	ctx    uintptr
	mem    uintptr
	blocks uintptr
	budget int64
}

// newJIT makes a recompiler for mem, or returns nil where there's none.
func newJIT(mem *Memory) *JIT {
	buf := jitAlloc(jitBufSize)
	if buf == nil {
		return nil
	}
	j := &JIT{mem: mem, buf: buf, base: uintptr(unsafe.Pointer(&buf[0]))}
	for i, p := range mem.pages {
		if p != nil {
			j.hostMn[i] = uintptr(unsafe.Pointer(&p[0]))
		}
	}
	j.reset()
	runtime.SetFinalizer(j, func(j *JIT) { jitFree(j.buf) })
	return j
}

// reset forgets all compiled code.
func (j *JIT) reset() {
	for i := range j.table {
		j.table[i] = 0
		j.pages[i] = nil
	}
	j.used = 0
	a := &x86{base: j.base}
	j.emitDispatch(a)
	j.place(a)
	j.dispatch = j.base
	j.Flushes++
}

// place copies assembled code into the buffer and returns its address.
func (j *JIT) place(a *x86) uintptr {
	at := j.base + uintptr(j.used)
	copy(j.buf[j.used:], a.b)
	j.used += (len(a.b) + 15) &^ 15
	return at
}

// pageAdded tells the recompiler a guest page now has memory.
func (j *JIT) pageAdded(i uint32, p []byte) { j.hostMn[i] = uintptr(unsafe.Pointer(&p[0])) }

// invalidate forgets the code compiled from a guest page.
func (j *JIT) invalidate(page uint32) {
	j.table[page] = 0
	j.pages[page] = nil
}

func (j *JIT) lookup(pc uint32) uintptr {
	if p := j.pages[pc>>pageBits]; p != nil {
		return p[pc&pageMask>>2]
	}
	return 0
}

func (j *JIT) setEntry(pc uint32, code uintptr) {
	p := j.pages[pc>>pageBits]
	if p == nil {
		p = new(blockPage)
		j.pages[pc>>pageBits] = p
		j.table[pc>>pageBits] = uintptr(unsafe.Pointer(&p[0]))
	}
	p[pc&pageMask>>2] = code
}

// the dispatcher: EAX = the next PC
func (j *JIT) emitDispatch(a *x86) {
	a.movRR(rCX, rAX)
	a.shiftRI(shShr, rCX, pageBits)
	a.movLoad64Idx(rDX, r13, rCX, 8)
	a.testRR64(rDX, rDX)
	miss1 := a.jcc(ccE)
	a.movRR(rCX, rAX)
	a.aluRI(aluAnd, rCX, pageMask&^3)
	a.movLoad64Idx(rDX, rDX, rCX, 2) // (pc & 0xfffc) / 4 * 8
	a.testRR64(rDX, rDX)
	miss2 := a.jcc(ccE)
	a.jmpReg(rDX)
	a.patch(miss1)
	a.patch(miss2)
	a.movStore(rBX, offPC, rAX)
	a.movRI(rAX, jitExitCompile)
	a.ret()
}

// run runs code on c until the budget is spent or the interpreter is needed.
func (j *JIT) run(c *CPU, n int) {
	c.stop = false
	left := int64(n)
	for left > 0 {
		pc := c.PC
		entry := j.lookup(pc)
		if entry == 0 {
			entry = j.compile(pc)
		}
		args := jitArgs{ctx: uintptr(unsafe.Pointer(c.Context)), mem: uintptr(unsafe.Pointer(&j.hostMn[0])),
			blocks: uintptr(unsafe.Pointer(&j.table[0])), budget: left}
		exit := jitCall(entry, &args)
		ran := left - args.budget
		c.Ran += uint64(ran)
		left = args.budget
		switch exit {
		case jitExitBudget:
			return
		case jitExitInterp:
			if left <= 0 {
				return
			}
			c.stepCached()
			left--
			if c.stop {
				return
			}
		}
	}
}

// stepCached runs one instruction with the interpreter.
func (c *CPU) stepCached() {
	pc := c.PC
	m := c.Mem
	var d *decoded
	if cp := m.code[pc>>pageBits]; cp != nil && cp[pc&pageMask>>2].fn != nil {
		d = &cp[pc&pageMask>>2]
	} else {
		d = c.decodeAt(pc)
	}
	c.PC = pc + 4
	d.fn(c, d.inst)
	c.Ran++
}

// compile translates the block at pc and returns its entry point.
func (j *JIT) compile(pc uint32) uintptr {
	if j.used > len(j.buf)-1<<20 {
		j.reset()
	}
	a := &x86{base: j.base + uintptr(j.used)}
	b := &jitBlock{a: a, j: j, start: pc}
	b.build()
	if len(a.b) > len(j.buf)-j.used {
		j.reset()
		return j.compile(pc)
	}
	code := j.place(a)
	j.setEntry(pc, code)
	j.Blocks++
	return code
}

type jitSlow struct {
	at     []int // jumps to this exit
	refund int
	pc     uint32
}

type jitBlock struct {
	a     *x86
	j     *JIT
	start uint32
	n     int // instructions in the block so far
	slow  []*jitSlow
	cur   *jitSlow // the current instruction's slow path
}

func (b *jitBlock) build() {
	a := b.a
	mem := b.j.mem
	// budget
	a.testRR64(r9, r9)
	budgetOut := a.jcc(ccLE)
	a.byte1(0x49, 0x81, 0xe9) // sub r9, imm32
	lenAt := a.pos()
	a.imm32(0)

	pc := b.start
	done := false
	for b.n < jitMaxBlock && !done {
		if mem.pages[pc>>pageBits] == nil {
			break
		}
		inst := mem.Read32(pc)
		b.cur = &jitSlow{pc: pc}
		mark := a.pos()
		ok, ends := b.inst(inst, pc)
		if !ok {
			a.b = a.b[:mark]
			break
		}
		if len(b.cur.at) > 0 {
			b.slow = append(b.slow, b.cur)
			b.cur.refund = b.n // patched below: instructions after this one
		}
		b.n++
		pc += 4
		done = ends
	}
	if !done {
		// the next instruction runs in the interpreter
		a.movStoreImm(rBX, offPC, pc)
		a.movRI(rAX, jitExitInterp)
		a.ret()
	}
	if b.n == 0 {
		// nothing compiled: straight to the interpreter
		a.b = a.b[:0]
		a.movStoreImm(rBX, offPC, b.start)
		a.movRI(rAX, jitExitInterp)
		a.ret()
		return
	}
	n := uint32(b.n)
	a.b[lenAt], a.b[lenAt+1], a.b[lenAt+2], a.b[lenAt+3] = byte(n), byte(n>>8), byte(n>>16), byte(n>>24)
	// out of line: the budget exit and the slow paths
	a.patch(budgetOut)
	a.movStoreImm(rBX, offPC, b.start)
	a.movRI(rAX, jitExitBudget)
	a.ret()
	for _, s := range b.slow {
		for _, at := range s.at {
			a.patch(at)
		}
		// instructions s and after weren't run
		a.aluRI64(aluAdd, r9, int32(b.n-s.refund))
		a.movStoreImm(rBX, offPC, s.pc)
		a.movRI(rAX, jitExitInterp)
		a.ret()
	}
}

// toPC ends the block going to a known PC.
func (b *jitBlock) toPC(pc uint32) {
	b.a.movRIKeepFlags(rAX, pc)
	b.a.jmpAbs(b.j.dispatch)
}

// slowIf jumps to the current instruction's interpreter exit on cc.
func (b *jitBlock) slowIf(cc int) { b.cur.at = append(b.cur.at, b.a.jcc(cc)) }

// crFromFlags sets CR field f from the flags of a compare (signed or not)
// and XER[SO]. Uses EAX, ECX, EDX.
func (b *jitBlock) crFromFlags(f uint32, signed bool) {
	a := b.a
	lt, gt := ccB, ccA
	if signed {
		lt, gt = ccL, ccG
	}
	a.setcc(lt, rCX)
	a.setcc(gt, rDX)
	a.setcc(ccE, rAX)
	a.movzx8(rCX, rCX)
	a.movzx8(rDX, rDX)
	a.movzx8(rAX, rAX)
	a.shiftRI(shShl, rCX, 3)
	a.shiftRI(shShl, rDX, 2)
	a.aluRR(aluAdd, rAX, rAX)
	a.aluRR(aluOr, rCX, rDX)
	a.aluRR(aluOr, rCX, rAX)
	a.movLoad(rDX, rBX, offXER)
	a.shiftRI(shShr, rDX, 31)
	a.aluRR(aluOr, rCX, rDX)
	sh := uint8(28 - 4*f)
	if sh != 0 {
		a.shiftRI(shShl, rCX, sh)
	}
	a.movLoad(rDX, rBX, offCR)
	a.aluRI(aluAnd, rDX, ^(uint32(0xf) << sh))
	a.aluRR(aluOr, rDX, rCX)
	a.movStore(rBX, offCR, rDX)
}

// cr0 sets CR0 from the result in EAX (which it clobbers).
func (b *jitBlock) cr0() {
	b.a.testRR(rAX, rAX)
	b.crFromFlags(0, true)
}

// setCA sets XER[CA] from the carry flag (clobbers EDX, ECX).
func (b *jitBlock) setCA(cc int) {
	a := b.a
	a.setcc(cc, rCX)
	a.movzx8(rCX, rCX)
	a.shiftRI(shShl, rCX, 29)
	a.movLoad(rDX, rBX, offXER)
	a.aluRI(aluAnd, rDX, ^uint32(xerCA))
	a.aluRR(aluOr, rDX, rCX)
	a.movStore(rBX, offXER, rDX)
}

// loadCA puts XER[CA] into the carry flag (clobbers EDX).
func (b *jitBlock) loadCA() {
	a := b.a
	a.movLoad(rDX, rBX, offXER)
	a.shiftRI(shShl, rDX, 3) // CA (bit 29) to bit 32: the carry flag
}

// ea computes an effective address into ECX: (ra|0) + d.
func (b *jitBlock) eaD(ra uint32, d uint32) {
	a := b.a
	if ra == 0 {
		a.movRI(rCX, d)
		return
	}
	a.movLoad(rCX, rBX, gpr(ra))
	if d != 0 {
		a.aluRI(aluAdd, rCX, d)
	}
}

// eaX: (ra|0) + rb into ECX.
func (b *jitBlock) eaX(ra, rb uint32) {
	a := b.a
	a.movLoad(rCX, rBX, gpr(rb))
	if ra != 0 {
		a.aluRM(aluAdd, rCX, rBX, gpr(ra))
	}
}

// hostAddr: for the guest address in ECX, the page's host address in RDX
// and the offset in EAX (64-bit clean), or the slow path when the page has
// no memory yet or an access of size bytes would leave it.
func (b *jitBlock) hostAddr(size uint32) {
	a := b.a
	a.movRR(rAX, rCX)
	a.shiftRI(shShr, rAX, pageBits)
	a.movLoad64Idx(rDX, r12, rAX, 8)
	a.testRR64(rDX, rDX)
	b.slowIf(ccE)
	a.movRR(rAX, rCX)
	a.aluRI(aluAnd, rAX, pageMask)
	if size > 1 {
		a.aluRI(aluCmp, rAX, pageSize-size)
		b.slowIf(ccA)
	}
}

// load reads size bytes at ECX into EAX (sign-extending halfwords when
// signed).
func (b *jitBlock) load(size uint32, signed bool) {
	a := b.a
	b.hostAddr(size)
	switch size {
	case 1:
		a.load8Idx(rAX, rDX, rAX)
	case 2:
		a.load16Idx(rAX, rDX, rAX)
		a.rol16by8(rAX)
		if signed {
			a.movsx16(rAX, rAX)
		} else {
			a.movzx16(rAX, rAX)
		}
	case 4:
		a.load32Idx(rAX, rDX, rAX)
		a.bswap(rAX)
	}
}

// store writes R8D's low size bytes at ECX.
func (b *jitBlock) store(size uint32) {
	a := b.a
	b.hostAddr(size)
	switch size {
	case 1:
		a.store8Idx(rDX, rAX, r8)
	case 2:
		a.rol16by8(r8)
		a.store16Idx(rDX, rAX, r8)
	case 4:
		a.bswap(r8)
		a.store32Idx(rDX, rAX, r8)
	}
}

// inst translates one instruction. ok = false: it isn't translated (the
// block ends before it); ends: it was a branch.
func (b *jitBlock) inst(inst, pc uint32) (ok, ends bool) {
	a := b.a
	op := inst >> 26
	rd := inst >> 21 & 31
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	simm := uint32(int32(int16(inst)))
	uimm := inst & 0xffff
	rc := inst&1 != 0

	switch op {
	case 7: // mulli
		a.movLoad(rAX, rBX, gpr(ra))
		a.imulRRI(rAX, rAX, simm)
		a.movStore(rBX, gpr(rd), rAX)
	case 8: // subfic
		a.movRI(rAX, simm)
		a.aluRM(aluSub, rAX, rBX, gpr(ra))
		a.movStore(rBX, gpr(rd), rAX)
		b.setCA(ccAE)
	case 10, 11: // cmpli, cmpi
		a.movLoad(rAX, rBX, gpr(ra))
		if op == 10 {
			a.aluRI(aluCmp, rAX, uimm)
		} else {
			a.aluRI(aluCmp, rAX, simm)
		}
		b.crFromFlags(rd>>2, op == 11)
	case 12, 13: // addic, addic.
		a.movLoad(rAX, rBX, gpr(ra))
		a.aluRI(aluAdd, rAX, simm)
		a.movStore(rBX, gpr(rd), rAX)
		a.movRR(r8, rAX)
		b.setCA(ccB)
		if op == 13 {
			a.movRR(rAX, r8)
			b.cr0()
		}
	case 14: // addi
		if ra == 0 {
			a.movStoreImm(rBX, gpr(rd), simm)
		} else {
			a.movLoad(rAX, rBX, gpr(ra))
			a.aluRI(aluAdd, rAX, simm)
			a.movStore(rBX, gpr(rd), rAX)
		}
	case 15: // addis
		if ra == 0 {
			a.movStoreImm(rBX, gpr(rd), uimm<<16)
		} else {
			a.movLoad(rAX, rBX, gpr(ra))
			a.aluRI(aluAdd, rAX, uimm<<16)
			a.movStore(rBX, gpr(rd), rAX)
		}
	case 16: // bc
		target := uint32(int32(int16(inst & 0xfffc)))
		if inst&2 == 0 {
			target += pc
		}
		b.branch(rd, ra, inst&1 != 0, pc, func() { b.toPC(target) })
		return true, true
	case 18: // b
		li := inst & 0x03fffffc
		if li&0x02000000 != 0 {
			li |= 0xfc000000
		}
		target := li
		if inst&2 == 0 {
			target += pc
		}
		if inst&1 != 0 {
			a.movStoreImm(rBX, offLR, pc+4)
		}
		b.toPC(target)
		return true, true
	case 19:
		return b.inst19(inst, pc)
	case 20: // rlwimi
		mask := rotMask(inst>>6&31, inst>>1&31)
		a.movLoad(rAX, rBX, gpr(rd))
		if rb != 0 {
			a.shiftRI(shRol, rAX, uint8(rb))
		}
		a.aluRI(aluAnd, rAX, mask)
		a.movLoad(rCX, rBX, gpr(ra))
		a.aluRI(aluAnd, rCX, ^mask)
		a.aluRR(aluOr, rAX, rCX)
		a.movStore(rBX, gpr(ra), rAX)
		if rc {
			b.cr0()
		}
	case 21: // rlwinm
		mask := rotMask(inst>>6&31, inst>>1&31)
		a.movLoad(rAX, rBX, gpr(rd))
		if rb != 0 {
			a.shiftRI(shRol, rAX, uint8(rb))
		}
		if mask != 0xffffffff {
			a.aluRI(aluAnd, rAX, mask)
		}
		a.movStore(rBX, gpr(ra), rAX)
		if rc {
			b.cr0()
		}
	case 23: // rlwnm
		mask := rotMask(inst>>6&31, inst>>1&31)
		a.movLoad(rCX, rBX, gpr(rb))
		a.movLoad(rAX, rBX, gpr(rd))
		a.shiftRCL(shRol, rAX)
		a.aluRI(aluAnd, rAX, mask)
		a.movStore(rBX, gpr(ra), rAX)
		if rc {
			b.cr0()
		}
	case 24, 25, 26, 27: // ori, oris, xori, xoris
		v := uimm
		if op&1 != 0 {
			v <<= 16
		}
		if v == 0 && rd == ra {
			break // nop
		}
		aop := aluOr
		if op >= 26 {
			aop = aluXor
		}
		a.movLoad(rAX, rBX, gpr(rd))
		if v != 0 {
			a.aluRI(aop, rAX, v)
		}
		a.movStore(rBX, gpr(ra), rAX)
	case 28, 29: // andi., andis.
		v := uimm
		if op == 29 {
			v <<= 16
		}
		a.movLoad(rAX, rBX, gpr(rd))
		a.aluRI(aluAnd, rAX, v)
		a.movStore(rBX, gpr(ra), rAX)
		b.cr0()
	case 31:
		return b.inst31(inst, pc)
	case 32, 33, 34, 35, 40, 41, 42, 43: // lwz(u), lbz(u), lhz(u), lha(u)
		update := op&1 != 0
		if update && (ra == 0 || ra == rd) {
			return false, false
		}
		b.eaD(ra, simm)
		if update {
			a.movRR(r8, rCX)
		}
		switch op {
		case 32, 33:
			b.load(4, false)
		case 34, 35:
			b.load(1, false)
		case 40, 41:
			b.load(2, false)
		default:
			b.load(2, true)
		}
		a.movStore(rBX, gpr(rd), rAX)
		if update {
			a.movStore(rBX, gpr(ra), r8)
		}
	case 36, 37, 38, 39, 44, 45: // stw(u), stb(u), sth(u)
		update := op&1 != 0
		if update && ra == 0 {
			return false, false
		}
		b.eaD(ra, simm)
		a.movLoad(r8, rBX, gpr(rd))
		a.movRR(r10, rCX)
		switch op {
		case 36, 37:
			b.store(4)
		case 38, 39:
			b.store(1)
		default:
			b.store(2)
		}
		if update {
			a.movStore(rBX, gpr(ra), r10)
		}
	case 46, 47: // lmw, stmw
		n := 32 - rd
		b.eaD(ra, simm)
		b.hostAddr(4 * n)
		// RDX + RAX = the first word
		a.opRMI(true, []byte{0x8d}, rDX, rDX, rAX, 1, 0) // lea rdx, [rdx+rax]
		for i := uint32(0); i < n; i++ {
			if op == 46 {
				a.movLoad(rAX, rDX, int32(4*i))
				a.bswap(rAX)
				a.movStore(rBX, gpr(rd+i), rAX)
			} else {
				a.movLoad(rAX, rBX, gpr(rd+i))
				a.bswap(rAX)
				a.movStore(rDX, int32(4*i), rAX)
			}
		}
	default:
		return false, false
	}
	return true, false
}

// branch emits a conditional branch: taken() is the jump when the
// condition holds; otherwise it goes on to pc+4. LR is set (when lk)
// either way.
func (b *jitBlock) branch(bo, bi uint32, lk bool, pc uint32, taken func()) {
	a := b.a
	var not []int
	if bo&4 == 0 {
		a.aluMI(aluSub, rBX, offCTR, 1)
		if bo&2 == 0 {
			not = append(not, a.jcc(ccE)) // branch while CTR != 0
		} else {
			not = append(not, a.jcc(ccNE))
		}
	}
	if bo&0x10 == 0 {
		a.testMI(rBX, offCR, 1<<(31-bi))
		if bo&8 != 0 {
			not = append(not, a.jcc(ccE)) // branch if the bit is set
		} else {
			not = append(not, a.jcc(ccNE))
		}
	}
	if lk {
		a.movStoreImm(rBX, offLR, pc+4)
	}
	taken()
	if len(not) > 0 {
		for _, at := range not {
			a.patch(at)
		}
		if lk {
			a.movStoreImm(rBX, offLR, pc+4)
		}
		b.toPC(pc + 4)
	}
}

func (b *jitBlock) inst19(inst, pc uint32) (ok, ends bool) {
	a := b.a
	bt := inst >> 21 & 31
	ba := inst >> 16 & 31
	bb := inst >> 11 & 31
	lk := inst&1 != 0
	switch inst >> 1 & 0x3ff {
	case 16, 528: // bclr, bcctr
		from := offLR
		bo := bt
		if inst>>1&0x3ff == 528 {
			from = offCTR
			bo |= 4 // bcctr doesn't count down
		}
		// the target before LR changes
		a.movLoad(r11, rBX, from)
		a.aluRI(aluAnd, r11, ^uint32(3))
		b.branch(bo, ba, lk, pc, func() {
			a.movRR(rAX, r11)
			a.jmpAbs(b.j.dispatch)
		})
		return true, true
	case 150: // isync
	case 0: // mcrf
		a.movLoad(rAX, rBX, offCR)
		a.shiftRI(shShr, rAX, uint8(28-4*(ba>>2)))
		a.aluRI(aluAnd, rAX, 0xf)
		if sh := uint8(28 - 4*(bt>>2)); sh != 0 {
			a.shiftRI(shShl, rAX, sh)
		}
		a.movLoad(rDX, rBX, offCR)
		a.aluRI(aluAnd, rDX, ^(uint32(0xf) << (28 - 4*(bt>>2))))
		a.aluRR(aluOr, rDX, rAX)
		a.movStore(rBX, offCR, rDX)
	case 33, 129, 193, 225, 257, 289, 417, 449: // CR bit logic
		// EAX = bit ba, ECX = bit bb (each 0 or 1)
		a.movLoad(rAX, rBX, offCR)
		a.movRR(rCX, rAX)
		a.shiftRI(shShr, rAX, uint8(31-ba))
		a.aluRI(aluAnd, rAX, 1)
		a.shiftRI(shShr, rCX, uint8(31-bb))
		a.aluRI(aluAnd, rCX, 1)
		switch inst >> 1 & 0x3ff {
		case 33: // crnor
			a.aluRR(aluOr, rAX, rCX)
			a.aluRI(aluXor, rAX, 1)
		case 129: // crandc
			a.aluRI(aluXor, rCX, 1)
			a.aluRR(aluAnd, rAX, rCX)
		case 193: // crxor
			a.aluRR(aluXor, rAX, rCX)
		case 225: // crnand
			a.aluRR(aluAnd, rAX, rCX)
			a.aluRI(aluXor, rAX, 1)
		case 257: // crand
			a.aluRR(aluAnd, rAX, rCX)
		case 289: // creqv
			a.aluRR(aluXor, rAX, rCX)
			a.aluRI(aluXor, rAX, 1)
		case 417: // crorc
			a.aluRI(aluXor, rCX, 1)
			a.aluRR(aluOr, rAX, rCX)
		case 449: // cror
			a.aluRR(aluOr, rAX, rCX)
		}
		if sh := uint8(31 - bt); sh != 0 {
			a.shiftRI(shShl, rAX, sh)
		}
		a.movLoad(rDX, rBX, offCR)
		a.aluRI(aluAnd, rDX, ^(uint32(1) << (31 - bt)))
		a.aluRR(aluOr, rDX, rAX)
		a.movStore(rBX, offCR, rDX)
	default:
		return false, false
	}
	return true, false
}

func (b *jitBlock) inst31(inst, pc uint32) (ok, ends bool) {
	a := b.a
	rd := inst >> 21 & 31
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	rc := inst&1 != 0
	xo := inst >> 1 & 0x3ff
	if inst&0x400 != 0 && (xo&0x1ff == 266 || xo&0x1ff == 10 || xo&0x1ff == 138 || xo&0x1ff == 202 ||
		xo&0x1ff == 234 || xo&0x1ff == 40 || xo&0x1ff == 8 || xo&0x1ff == 136 || xo&0x1ff == 200 ||
		xo&0x1ff == 232 || xo&0x1ff == 104 || xo&0x1ff == 235 || xo&0x1ff == 491 || xo&0x1ff == 459) {
		return false, false // overflow-recording forms: the interpreter
	}
	// result in EAX goes to rd (arithmetic) or ra (logic), then CR0
	toRD := func() {
		a.movStore(rBX, gpr(rd), rAX)
		if rc {
			b.cr0()
		}
	}
	toRA := func() {
		a.movStore(rBX, gpr(ra), rAX)
		if rc {
			b.cr0()
		}
	}
	logic := func(op int, notA, notB, notR bool) {
		a.movLoad(rAX, rBX, gpr(rd))
		if notA {
			a.unary(2, rAX)
		}
		a.movLoad(rCX, rBX, gpr(rb))
		if notB {
			a.unary(2, rCX)
		}
		a.aluRR(op, rAX, rCX)
		if notR {
			a.unary(2, rAX)
		}
		toRA()
	}
	switch xo {
	case 266: // add
		a.movLoad(rAX, rBX, gpr(ra))
		a.aluRM(aluAdd, rAX, rBX, gpr(rb))
		toRD()
	case 10, 138, 202, 234: // addc, adde, addze, addme
		if xo != 10 {
			b.loadCA()
		}
		a.movLoad(rAX, rBX, gpr(ra))
		switch xo {
		case 10:
			a.aluRM(aluAdd, rAX, rBX, gpr(rb))
		case 138:
			a.aluRM(aluAdc, rAX, rBX, gpr(rb))
		case 202:
			a.aluRI(aluAdc, rAX, 0)
		case 234:
			a.aluRI(aluAdc, rAX, 0xffffffff)
		}
		a.movRR(r8, rAX)
		b.setCA(ccB)
		a.movRR(rAX, r8)
		toRD()
	case 40: // subf
		a.movLoad(rAX, rBX, gpr(rb))
		a.aluRM(aluSub, rAX, rBX, gpr(ra))
		toRD()
	case 8, 136, 200, 232: // subfc, subfe, subfze, subfme: ~ra + rb (+1 or CA)
		a.movLoad(rAX, rBX, gpr(ra))
		a.unary(2, rAX)
		if xo == 8 {
			a.byte1(0xf9) // stc
		} else {
			b.loadCA()
		}
		switch xo {
		case 8, 136:
			a.aluRM(aluAdc, rAX, rBX, gpr(rb))
		case 200:
			a.aluRI(aluAdc, rAX, 0)
		case 232:
			a.aluRI(aluAdc, rAX, 0xffffffff)
		}
		a.movRR(r8, rAX)
		b.setCA(ccB)
		a.movRR(rAX, r8)
		toRD()
	case 104: // neg
		a.movLoad(rAX, rBX, gpr(ra))
		a.unary(3, rAX)
		toRD()
	case 235: // mullw
		a.movLoad(rAX, rBX, gpr(ra))
		a.opRM(false, []byte{0x0f, 0xaf}, rAX, rBX, gpr(rb))
		toRD()
	case 75, 11: // mulhw, mulhwu
		a.movLoad(rAX, rBX, gpr(ra))
		a.movLoad(rCX, rBX, gpr(rb))
		if xo == 75 {
			a.unary(5, rCX)
		} else {
			a.unary(4, rCX)
		}
		a.movRR(rAX, rDX)
		toRD()
	case 459: // divwu (by 0 gives 0)
		a.movLoad(rCX, rBX, gpr(rb))
		a.movLoad(rAX, rBX, gpr(ra))
		a.testRR(rCX, rCX)
		zero := a.jcc(ccE)
		a.aluRR(aluXor, rDX, rDX)
		a.unary(6, rCX)
		done := a.jmp()
		a.patch(zero)
		a.aluRR(aluXor, rAX, rAX)
		a.patch(done)
		toRD()
	case 491: // divw (the odd cases in the interpreter)
		a.movLoad(rCX, rBX, gpr(rb))
		a.testRR(rCX, rCX)
		b.slowIf(ccE)
		a.aluRI(aluCmp, rCX, 0xffffffff)
		notM1 := a.jcc(ccNE)
		a.movLoad(rAX, rBX, gpr(ra))
		a.aluRI(aluCmp, rAX, 0x80000000)
		b.slowIf(ccE)
		a.patch(notM1)
		a.movLoad(rAX, rBX, gpr(ra))
		a.byte1(0x99) // cdq
		a.unary(7, rCX)
		toRD()
	case 0, 32: // cmp, cmpl
		a.movLoad(rAX, rBX, gpr(ra))
		a.aluRM(aluCmp, rAX, rBX, gpr(rb))
		b.crFromFlags(rd>>2, xo == 0)
	case 28: // and
		logic(aluAnd, false, false, false)
	case 60: // andc
		logic(aluAnd, false, true, false)
	case 444: // or
		if rd == rb && !rc {
			a.movLoad(rAX, rBX, gpr(rd))
			a.movStore(rBX, gpr(ra), rAX)
			break
		}
		logic(aluOr, false, false, false)
	case 412: // orc
		logic(aluOr, false, true, false)
	case 316: // xor
		logic(aluXor, false, false, false)
	case 124: // nor
		logic(aluOr, false, false, true)
	case 476: // nand
		logic(aluAnd, false, false, true)
	case 284: // eqv
		logic(aluXor, false, false, true)
	case 24, 536: // slw, srw
		a.movLoad(rCX, rBX, gpr(rb))
		a.movLoad(rAX, rBX, gpr(rd))
		a.aluRI(aluAnd, rCX, 63)
		a.aluRR(aluXor, rDX, rDX)
		a.aluRI(aluCmp, rCX, 32)
		if xo == 24 {
			a.shiftRCL(shShl, rAX)
		} else {
			a.shiftRCL(shShr, rAX)
		}
		// (the shift leaves the flags alone when CL & 31 is 0, and sets
		// them otherwise: compare again)
		a.aluRI(aluCmp, rCX, 32)
		a.cmovRR(ccAE, rAX, rDX)
		toRA()
	case 824: // srawi
		a.movLoad(rAX, rBX, gpr(rd))
		if rb == 0 {
			a.movStore(rBX, gpr(ra), rAX)
			a.aluRR(aluXor, rCX, rCX)
			a.testRR(rCX, rCX) // CA = 0
			b.setCA(ccNE)
			if rc {
				a.movLoad(rAX, rBX, gpr(ra))
				b.cr0()
			}
			break
		}
		// CA: negative and a 1 shifted out
		a.movRR(rCX, rAX)
		a.aluRI(aluAnd, rCX, 1<<rb-1)
		a.movRR(r8, rAX)
		a.shiftRI(shSar, r8, uint8(rb))
		a.movStore(rBX, gpr(ra), r8)
		a.testRR(rAX, rAX)
		a.setcc(ccS, rAX)
		a.testRR(rCX, rCX)
		a.setcc(ccNE, rCX)
		a.aluRR(aluAnd, rAX, rCX) // (low bytes; the rest doesn't matter)
		a.byte1(0xa8, 1)          // test al, 1
		b.setCA(ccNE)
		if rc {
			a.movRR(rAX, r8)
			b.cr0()
		}
	case 792: // sraw
		// n = rb & 63; n > 31: all sign; CA = negative && bits lost
		a.movLoad(rCX, rBX, gpr(rb))
		a.aluRI(aluAnd, rCX, 63)
		a.movLoad(rAX, rBX, gpr(rd))
		a.aluRI(aluCmp, rCX, 31)
		big := a.jcc(ccA)
		// n <= 31: lost = rs & ((1<<n)-1)
		a.movRR(r8, rAX)
		a.shiftRCL(shSar, r8)
		a.movRI(rDX, 1)
		a.shiftRCL(shShl, rDX)
		a.aluRI(aluSub, rDX, 1)
		a.aluRR(aluAnd, rDX, rAX)
		join := a.jmp()
		a.patch(big)
		a.movRR(r8, rAX)
		a.shiftRI(shSar, r8, 31)
		a.movRR(rDX, rAX) // lost = rs (nonzero when negative)
		a.patch(join)
		a.movStore(rBX, gpr(ra), r8)
		a.testRR(rAX, rAX)
		a.setcc(ccS, rAX)
		a.testRR(rDX, rDX)
		a.setcc(ccNE, rCX)
		a.aluRR(aluAnd, rAX, rCX)
		a.byte1(0xa8, 1) // test al, 1
		b.setCA(ccNE)
		if rc {
			a.movRR(rAX, r8)
			b.cr0()
		}
	case 26: // cntlzw
		a.movLoad(rCX, rBX, gpr(rd))
		a.movRI(rAX, 32)
		a.testRR(rCX, rCX)
		zero := a.jcc(ccE)
		a.bsr(rAX, rCX)
		a.aluRI(aluXor, rAX, 31)
		a.patch(zero)
		toRA()
	case 922, 954: // extsh, extsb
		a.movLoad(rAX, rBX, gpr(rd))
		if xo == 922 {
			a.movsx16(rAX, rAX)
		} else {
			a.movsx8(rAX, rAX)
		}
		toRA()
	case 23, 55, 87, 119, 279, 311, 343, 375, 534: // lwzx, lwzux, lbzx, lbzux, lhzx, lhzux, lhax, lhaux, lwbrx
		update := xo&32 != 0 && xo != 534
		if update && (ra == 0 || ra == rd) {
			return false, false
		}
		b.eaX(ra, rb)
		if update {
			a.movRR(r8, rCX)
		}
		switch xo {
		case 23, 55:
			b.load(4, false)
		case 534:
			b.hostAddr(4)
			a.load32Idx(rAX, rDX, rAX)
		case 87, 119:
			b.load(1, false)
		case 279, 311:
			b.load(2, false)
		default:
			b.load(2, true)
		}
		a.movStore(rBX, gpr(rd), rAX)
		if update {
			a.movStore(rBX, gpr(ra), r8)
		}
	case 151, 183, 215, 247, 407, 439, 662: // stwx, stwux, stbx, stbux, sthx, sthux, stwbrx
		update := xo&32 != 0 && xo != 662
		if update && ra == 0 {
			return false, false
		}
		b.eaX(ra, rb)
		a.movLoad(r8, rBX, gpr(rd))
		a.movRR(r10, rCX)
		switch xo {
		case 151, 183:
			b.store(4)
		case 662:
			b.hostAddr(4)
			a.store32Idx(rDX, rAX, r8)
		case 215, 247:
			b.store(1)
		default:
			b.store(2)
		}
		if update {
			a.movStore(rBX, gpr(ra), r10)
		}
	case 1014: // dcbz
		b.eaX(ra, rb)
		a.aluRI(aluAnd, rCX, ^uint32(31))
		b.hostAddr(32)
		a.opRMI(true, []byte{0x8d}, rDX, rDX, rAX, 1, 0) // lea rdx, [rdx+rax]
		a.aluRR(aluXor, rAX, rAX)
		for i := int32(0); i < 32; i += 8 {
			a.opRM(true, []byte{0x89}, rAX, rDX, i)
		}
	case 339: // mfspr
		switch inst>>16&31 | (inst>>11&31)<<5 {
		case 1:
			a.movLoad(rAX, rBX, offXER)
		case 8:
			a.movLoad(rAX, rBX, offLR)
		case 9:
			a.movLoad(rAX, rBX, offCTR)
		default:
			return false, false
		}
		a.movStore(rBX, gpr(rd), rAX)
	case 467: // mtspr
		var to int32
		switch inst>>16&31 | (inst>>11&31)<<5 {
		case 1:
			to = offXER
		case 8:
			to = offLR
		case 9:
			to = offCTR
		default:
			return false, false
		}
		a.movLoad(rAX, rBX, gpr(rd))
		a.movStore(rBX, to, rAX)
	case 19: // mfcr
		a.movLoad(rAX, rBX, offCR)
		a.movStore(rBX, gpr(rd), rAX)
	case 144: // mtcrf
		crm := inst >> 12 & 0xff
		var mask uint32
		for i := uint32(0); i < 8; i++ {
			if crm&(0x80>>i) != 0 {
				mask |= 0xf0000000 >> (i * 4)
			}
		}
		a.movLoad(rAX, rBX, gpr(rd))
		a.aluRI(aluAnd, rAX, mask)
		a.movLoad(rDX, rBX, offCR)
		a.aluRI(aluAnd, rDX, ^mask)
		a.aluRR(aluOr, rDX, rAX)
		a.movStore(rBX, offCR, rDX)
	case 54, 86, 246, 278, 470, 598, 854, 982: // dcbst, dcbf, dcbtst, dcbt, dcbi, sync, eieio, icbi
		if xo == 982 {
			return false, false // icbi forgets code: the interpreter
		}
	default:
		return false, false
	}
	return true, false
}
