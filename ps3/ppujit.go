package ps3

// The PPU recompiler: PowerPC code is translated, a block at a time, into
// x86-64 code that does the same thing to the PPUState, and run directly.
// Instructions it doesn't translate (system function calls, system calls,
// most floating point and vector instructions for now, rare system
// instructions) end the block and run in the interpreter, then the
// recompiled code carries on.
//
// While recompiled code runs these x86 registers are fixed:
//
//	RBX  the PPUState
//	R12  the guest memory table for reads: for each 64 KB page, its host
//	     address or 0
//	RDI  the same for writes (0 also for pages whose writes are watched,
//	     such as pages holding compiled code)
//	R13  the block table: for each 64 KB page, the address of a table of
//	     16384 block entry points (one per instruction) or 0
//	R9   the instruction budget left (signed)
//
// Each block starts by checking the budget, then subtracts its length. A
// block ends by putting the next PC in EAX and jumping to the dispatcher,
// which jumps to that block or returns to Go to have it compiled. Returning
// to Go (RET) leaves the reason in EAX and the PC in the PPUState.

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"unsafe"
)

const (
	jitExitBudget  = 1 // the budget ran out (PC = the next block)
	jitExitCompile = 2 // no code for PC yet
	jitExitInterp  = 3 // run the instruction at PC in the interpreter

	jitMaxBlock = 128 // instructions per block
	jitBufSize  = 64 << 20
)

var (
	offGPR     = int32(unsafe.Offsetof(PPUState{}.GPR))
	offFPR     = int32(unsafe.Offsetof(PPUState{}.FPR))
	offVR      = int32(unsafe.Offsetof(PPUState{}.VR))
	offCR      = int32(unsafe.Offsetof(PPUState{}.CR))
	offLR      = int32(unsafe.Offsetof(PPUState{}.LR))
	offCTR     = int32(unsafe.Offsetof(PPUState{}.CTR))
	offXER     = int32(unsafe.Offsetof(PPUState{}.XER))
	offFPSCR   = int32(unsafe.Offsetof(PPUState{}.FPSCR))
	offPC      = int32(unsafe.Offsetof(PPUState{}.PC))
	offFpLast  = int32(unsafe.Offsetof(PPUState{}.fpLast))
	offFpDirty = int32(unsafe.Offsetof(PPUState{}.fpDirty))
)

var jitFMA = jitSupported && cpuHasFMA()

func fpr(r uint32) int32 { return offFPR + int32(r)*8 }

func gpr(r uint32) int32 { return offGPR + int32(r)*8 }

// jitInterpHist (PS3_JITHIST=1): what the recompilers leave to the
// interpreters, by opcode (debugging).
var jitInterpHist = func() map[string]int {
	if os.Getenv("PS3_JITHIST") != "" {
		return map[string]int{}
	}
	return nil
}()

// jitOff turns the recompiler off (PS3_NOJIT=1), for comparing.
var jitOff = os.Getenv("PS3_NOJIT") != ""

type blockPage [pageSize / 4]uintptr

// JIT is the recompiler's state.
type JIT struct {
	mem      *Memory
	buf      []byte // executable memory
	base     uintptr
	used     int
	dispatch uintptr

	pages [numPages]*blockPage // Go's references to the block tables
	// sites: for each page, the jumps (rel32 addresses) to each PC in it
	sites [numPages]map[uint32][]uintptr
	table [numPages]uintptr // what the code reads
	// each page's compiled range (byte offsets), for writes to code
	lo, hi [numPages]uint32

	Blocks, Flushes, Translated, Interp int // counters for tests and tuning
}

// jitArgs is what jitCall loads into the fixed registers.
type jitArgs struct {
	ctx    uintptr
	mem    uintptr
	blocks uintptr
	budget int64
	memW   uintptr
}

// newJIT makes a recompiler for mem, or returns nil where there's none.
func newJIT(mem *Memory) *JIT {
	if !jitSupported || jitOff {
		return nil
	}
	buf := jitAlloc(jitBufSize)
	if buf == nil {
		return nil
	}
	j := &JIT{mem: mem, buf: buf, base: uintptr(unsafe.Pointer(&buf[0]))}
	mem.codeWrite = j.codeWritten
	j.reset()
	runtime.SetFinalizer(j, func(j *JIT) { jitFree(j.buf) })
	return j
}

// reset forgets all compiled code.
func (j *JIT) reset() {
	for i := range j.table {
		if j.pages[i] != nil {
			j.mem.setCode(uint32(i), false)
		}
		j.table[i] = 0
		j.pages[i] = nil
		j.sites[i] = nil
	}
	// the byte shuffle tables (for the vector instructions), then the
	// dispatcher
	copy(j.buf, shufTables())
	j.used = (tabSize + 15) &^ 15
	a := &x86{base: j.base + uintptr(j.used)}
	j.emitDispatch(a)
	j.dispatch = j.place(a)
	j.Flushes++
}

// place copies assembled code into the buffer and returns its address.
func (j *JIT) place(a *x86) uintptr {
	at := j.base + uintptr(j.used)
	copy(j.buf[j.used:], a.b)
	j.used += (len(a.b) + 15) &^ 15
	return at
}

// codeWritten: a write to a page with compiled code. If it touches the
// code, the page's blocks are forgotten.
func (j *JIT) codeWritten(addr uint32, n int) {
	i := addr >> pageBits
	o := addr & pageMask
	if j.pages[i] == nil || o >= j.hi[i] || o+uint32(n) <= j.lo[i] {
		return
	}
	j.table[i] = 0
	j.pages[i] = nil
	j.mem.setCode(i, false)
	// jumps into the page's blocks go back to the dispatcher
	for _, ss := range j.sites[i] {
		for _, at := range ss {
			j.patchRel(at, j.dispatch)
		}
	}
	j.sites[i] = nil
}

// jitLinking chains blocks directly (PS3_NOLINK=1 turns it off).
var jitLinking = os.Getenv("PS3_NOLINK") == ""

// addSite notes a jump (its rel32 at at) to pc.
func (j *JIT) addSite(pc uint32, at uintptr) {
	i := pc >> pageBits
	if j.sites[i] == nil {
		j.sites[i] = map[uint32][]uintptr{}
	}
	j.sites[i][pc] = append(j.sites[i][pc], at)
}

// linkTo points the jumps to pc at its new block.
func (j *JIT) linkTo(pc uint32, code uintptr) {
	for _, at := range j.sites[pc>>pageBits][pc] {
		j.patchRel(at, code)
	}
}

// patchRel sets the rel32 at at to reach target.
func (j *JIT) patchRel(at, target uintptr) {
	rel := uint32(int32(int64(target) - int64(at+4)))
	o := at - j.base
	j.buf[o], j.buf[o+1], j.buf[o+2], j.buf[o+3] = byte(rel), byte(rel>>8), byte(rel>>16), byte(rel>>24)
}

func (j *JIT) lookup(pc uint32) uintptr {
	if p := j.pages[pc>>pageBits]; p != nil {
		return p[pc&pageMask>>2]
	}
	return 0
}

func (j *JIT) setEntry(pc uint32, code uintptr) {
	i := pc >> pageBits
	p := j.pages[i]
	if p == nil {
		p = new(blockPage)
		j.pages[i] = p
		j.table[i] = uintptr(unsafe.Pointer(&p[0]))
		j.lo[i], j.hi[i] = pageSize, 0
		j.mem.setCode(i, true)
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

// run runs up to n instructions on c until the budget is spent or an
// instruction stops it.
func (j *JIT) run(c *PPU, n int) {
	left := int64(n)
	for left > 0 {
		pc := c.PC
		entry := j.lookup(pc)
		if entry == 0 {
			entry = j.compile(pc)
		}
		args := jitArgs{ctx: uintptr(unsafe.Pointer(c.PPUState)), mem: uintptr(unsafe.Pointer(&j.mem.hostR[0])),
			blocks: uintptr(unsafe.Pointer(&j.table[0])), budget: left, memW: uintptr(unsafe.Pointer(&j.mem.hostW[0]))}
		exit := jitCall(entry, &args)
		c.Ran += uint64(left - args.budget)
		left = args.budget
		switch exit {
		case jitExitBudget:
			return
		case jitExitInterp:
			if left <= 0 {
				return
			}
			if jitInterpHist != nil {
				w := c.Mem.Read32(c.PC)
				k := fmt.Sprintf("ppu %d/%d", w>>26, w>>1&0x3ff)
				if w>>26 == 4 {
					if w&0x3f >= 32 {
						k = fmt.Sprintf("ppu va%d", w&0x3f)
					} else {
						k = fmt.Sprintf("ppu vx%d", w&0x7ff)
					}
				}
				jitInterpHist[k]++
			}
			c.step1()
			j.Interp++
			left--
			if c.stop {
				return
			}
		}
	}
}

// step1 runs one instruction with the interpreter.
func (c *PPU) step1() {
	pc := c.PC
	m := c.Mem
	var inst uint32
	if p := m.pages[pc>>pageBits]; p != nil {
		o := pc & pageMask
		inst = uint32(p[o])<<24 | uint32(p[o+1])<<16 | uint32(p[o+2])<<8 | uint32(p[o+3])
	} else {
		inst = m.Read32(pc)
	}
	c.PC = pc + 4
	c.exec(inst)
	c.Ran++
}

// ppuJitLog (PS3_PPUJITLOG=dir): every block compiled, for debugging
var ppuJitLog = os.Getenv("PS3_PPUJITLOG")

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
	if ppuJitLog != "" {
		os.WriteFile(fmt.Sprintf("%s/%x.bin", ppuJitLog, code), a.b, 0o644)
		f, _ := os.OpenFile(ppuJitLog+"/index.txt", os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		fmt.Fprintf(f, "%x %x %08x %d\n", code, code+uintptr(len(a.b)), pc, b.n)
		f.Close()
	}
	if jitLinking {
		for _, l := range b.links {
			j.addSite(l.pc, code+uintptr(l.off))
		}
	}
	j.setEntry(pc, code)
	if jitLinking {
		j.linkTo(pc, code)
	}
	i := pc >> pageBits
	j.lo[i] = min(j.lo[i], pc&pageMask)
	j.hi[i] = max(j.hi[i], pc&pageMask+uint32(4*b.n)+4)
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
	pool  [][16]byte
	refs  []poolRef
	links []jitLink // jumps to other blocks
}

// jitLink is a jump at the end of a block to a known PC: its rel32 goes
// straight to that PC's block once there is one, else to the dispatcher
// (EAX holds the PC either way).
type jitLink struct {
	off int // of the rel32, in the block
	pc  uint32
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
	done, stopped := false, false
	for b.n < jitMaxBlock && !done {
		if pc>>pageBits != b.start>>pageBits || mem.pages[pc>>pageBits] == nil {
			break
		}
		inst := mem.Read32(pc)
		b.cur = &jitSlow{pc: pc}
		mark, marks, links := a.pos(), len(b.refs), len(b.links)
		ok, ends := b.inst(inst, pc)
		if !ok {
			a.b = a.b[:mark]
			b.refs = b.refs[:marks]
			b.links = b.links[:links]
			stopped = true
			break
		}
		if len(b.cur.at) > 0 {
			b.slow = append(b.slow, b.cur)
			b.cur.refund = b.n
		}
		b.n++
		pc += 4
		done = ends
	}
	if !done && !stopped && b.n > 0 {
		b.toPC(pc) // the block is as long as it gets: on to the next
		done = true
	}
	if !done {
		// the next instruction runs in the interpreter (or starts the
		// next block)
		a.movStoreImm(rBX, offPC, pc)
		a.movRI(rAX, jitExitInterp)
		a.ret()
	}
	if b.n == 0 {
		// nothing compiled: straight to the interpreter
		a.b = a.b[:0]
		b.pool, b.refs, b.links = nil, nil, nil
		a.movStoreImm(rBX, offPC, b.start)
		a.movRI(rAX, jitExitInterp)
		a.ret()
		return
	}
	b.j.Translated += b.n
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
	b.layPool()
}

// layPool puts the block's constants after its code, 16-byte aligned.
func (b *jitBlock) layPool() {
	a := b.a
	if len(b.pool) == 0 {
		return
	}
	for len(a.b)%16 != 0 {
		a.byte1(0xcc)
	}
	at := len(a.b)
	for _, v := range b.pool {
		a.byte1(v[:]...)
	}
	for _, r := range b.refs {
		a.ripTo(r.at, r.immLen, a.base+uintptr(at+16*r.idx))
	}
}

// toPC ends the block going to a known PC.
func (b *jitBlock) toPC(pc uint32) {
	b.a.movRIKeepFlags(rAX, pc)
	b.links = append(b.links, jitLink{off: b.a.pos() + 1, pc: pc})
	target := b.j.dispatch
	if jitLinking {
		if c := b.j.lookup(pc); c != 0 {
			target = c
		}
	}
	b.a.jmpAbs(target)
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

// cr0 sets CR0 from the 64-bit result in RAX (which it clobbers).
func (b *jitBlock) cr0() {
	b.a.testRR64(rAX, rAX)
	b.crFromFlags(0, true)
}

// setCA sets XER[CA] from condition cc (clobbers EDX, ECX).
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
	a.shiftRI(shShl, rDX, 3) // CA (bit 29) out the top: the carry flag
}

// eaD computes an effective address into ECX: (ra|0) + d.
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
// no memory (or, for a write, its writes are watched) or an access of size
// bytes would leave it.
func (b *jitBlock) hostAddr(size uint32, write bool) {
	a := b.a
	a.movRR(rAX, rCX)
	a.shiftRI(shShr, rAX, pageBits)
	if write {
		a.movLoad64Idx(rDX, rDI, rAX, 8)
	} else {
		a.movLoad64Idx(rDX, r12, rAX, 8)
	}
	a.testRR64(rDX, rDX)
	b.slowIf(ccE)
	a.movRR(rAX, rCX)
	a.aluRI(aluAnd, rAX, pageMask)
	if size > 1 {
		a.aluRI(aluCmp, rAX, pageSize-size)
		b.slowIf(ccA)
	}
}

// load reads size bytes at ECX into RAX, extended to 64 bits.
func (b *jitBlock) load(size uint32, signed bool) {
	a := b.a
	b.hostAddr(size, false)
	switch size {
	case 1:
		a.load8Idx(rAX, rDX, rAX)
	case 2:
		a.load16Idx(rAX, rDX, rAX)
		a.rol16by8(rAX)
		if signed {
			a.movsx16to64(rAX, rAX)
		} else {
			a.movzx16(rAX, rAX)
		}
	case 4:
		a.load32Idx(rAX, rDX, rAX)
		a.bswap(rAX)
		if signed {
			a.movsxd(rAX, rAX)
		}
	case 8:
		a.load64Idx(rAX, rDX, rAX)
		a.bswap64(rAX)
	}
}

// store writes R8's low size bytes at ECX (clobbering R8).
func (b *jitBlock) store(size uint32) {
	a := b.a
	b.hostAddr(size, true)
	switch size {
	case 1:
		a.store8Idx(rDX, rAX, r8)
	case 2:
		a.rol16by8(r8)
		a.store16Idx(rDX, rAX, r8)
	case 4:
		a.bswap(r8)
		a.store32Idx(rDX, rAX, r8)
	case 8:
		a.bswap64(r8)
		a.store64Idx(rDX, rAX, r8)
	}
}

// loadTo: a load of size bytes at ECX into GPR rd (and ECX into ra when
// update).
func (b *jitBlock) loadTo(size uint32, signed bool, rd, ra uint32, update bool) {
	a := b.a
	if update {
		a.movRR(r8, rCX)
	}
	b.load(size, signed)
	a.movStore64(rBX, gpr(rd), rAX)
	if update {
		a.movStore64(rBX, gpr(ra), r8) // (zero-extended by the 32-bit move)
	}
}

// storeFrom: a store of GPR rs's low size bytes at ECX.
func (b *jitBlock) storeFrom(size uint32, rs, ra uint32, update bool) {
	a := b.a
	if size == 8 {
		a.movLoad64(r8, rBX, gpr(rs))
	} else {
		a.movLoad(r8, rBX, gpr(rs))
	}
	a.movRR(r10, rCX)
	b.store(size)
	if update {
		a.movStore64(rBX, gpr(ra), r10)
	}
}

// mask64 into R11.
func (b *jitBlock) andMask(r int, m uint64) {
	a := b.a
	switch {
	case m == ^uint64(0):
	case m>>32 == 0 && m&0x80000000 == 0 || int64(m) == int64(int32(m)):
		a.aluRI64(aluAnd, r, int32(m))
	case m>>32 == 0:
		a.aluRI(aluAnd, r, uint32(m)) // (a 32-bit and clears the top)
	default:
		a.movRI64(r11, m)
		a.aluRR64(aluAnd, r, r11)
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
	case 4:
		if !spuJitOK {
			return false, false
		}
		return b.vmx(inst), false
	case 7: // mulli
		a.movLoad64(rAX, rBX, gpr(ra))
		a.imulRRI64(rAX, rAX, int32(simm))
		a.movStore64(rBX, gpr(rd), rAX)
	case 8: // subfic: simm - ra, CA = no borrow
		a.movImm64(rAX, uint64(int64(int32(simm))))
		a.aluRM64(aluSub, rAX, rBX, gpr(ra))
		a.movStore64(rBX, gpr(rd), rAX)
		b.setCA(ccAE)
	case 10, 11: // cmpli, cmpi
		l := inst&(1<<21) != 0
		if l {
			a.movLoad64(rAX, rBX, gpr(ra))
		} else {
			a.movLoad(rAX, rBX, gpr(ra))
		}
		v := simm
		if op == 10 {
			v = uimm
		}
		if l {
			a.aluRI64(aluCmp, rAX, int32(v))
		} else {
			a.aluRI(aluCmp, rAX, v)
		}
		b.crFromFlags(rd>>2, op == 11)
	case 12, 13: // addic, addic.
		a.movLoad64(rAX, rBX, gpr(ra))
		a.aluRI64(aluAdd, rAX, int32(simm))
		a.movStore64(rBX, gpr(rd), rAX)
		a.movRR64(r8, rAX)
		b.setCA(ccB)
		if op == 13 {
			a.movRR64(rAX, r8)
			b.cr0()
		}
	case 14, 15: // addi, addis
		v := uint64(int64(int32(simm)))
		if op == 15 {
			v = uint64(int64(int32(uimm << 16)))
		}
		if ra == 0 {
			a.storeImm64(rBX, gpr(rd), v)
		} else {
			a.movLoad64(rAX, rBX, gpr(ra))
			if v != 0 {
				a.aluRI64(aluAdd, rAX, int32(v))
			}
			a.movStore64(rBX, gpr(rd), rAX)
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
			a.storeImm64(rBX, offLR, uint64(pc+4))
		}
		b.toPC(target)
		return true, true
	case 19:
		return b.inst19(inst, pc)
	case 20, 21, 23: // rlwimi, rlwinm, rlwnm
		m := mask64(inst>>6&31+32, inst>>1&31+32)
		a.movLoad(rAX, rBX, gpr(rd))
		if op == 23 {
			a.movLoad(rCX, rBX, gpr(rb))
			a.shiftRCL(shRol, rAX)
		} else if rb != 0 {
			a.shiftRI(shRol, rAX, uint8(rb))
		}
		if m>>32 != 0 {
			// the mask reaches the top half: the rotated word is in both
			a.movRR(rCX, rAX)
			a.shiftRI64(shShl, rCX, 32)
			a.aluRR64(aluOr, rAX, rCX)
		}
		b.andMask(rAX, m)
		if op == 20 {
			a.movLoad64(rCX, rBX, gpr(ra))
			b.andMask(rCX, ^m)
			a.aluRR64(aluOr, rAX, rCX)
		}
		a.movStore64(rBX, gpr(ra), rAX)
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
		a.movLoad64(rAX, rBX, gpr(rd))
		if v != 0 {
			if v&0x80000000 != 0 {
				a.movRI(rCX, v)
				a.aluRR64(aop, rAX, rCX)
			} else {
				a.aluRI64(aop, rAX, int32(v))
			}
		}
		a.movStore64(rBX, gpr(ra), rAX)
	case 28, 29: // andi., andis.
		v := uimm
		if op == 29 {
			v <<= 16
		}
		a.movLoad(rAX, rBX, gpr(rd))
		a.aluRI(aluAnd, rAX, v) // (the top half becomes 0)
		a.movStore64(rBX, gpr(ra), rAX)
		b.cr0()
	case 30:
		return b.inst30(inst)
	case 31:
		return b.inst31(inst, pc)
	case 32, 33, 34, 35, 40, 41, 42, 43: // lwz(u), lbz(u), lhz(u), lha(u)
		update := op&1 != 0
		if update && (ra == 0 || ra == rd) {
			return false, false
		}
		b.eaD(ra, simm)
		switch op {
		case 32, 33:
			b.loadTo(4, false, rd, ra, update)
		case 34, 35:
			b.loadTo(1, false, rd, ra, update)
		case 40, 41:
			b.loadTo(2, false, rd, ra, update)
		default:
			b.loadTo(2, true, rd, ra, update)
		}
	case 36, 37, 38, 39, 44, 45: // stw(u), stb(u), sth(u)
		update := op&1 != 0
		if update && ra == 0 {
			return false, false
		}
		b.eaD(ra, simm)
		switch op {
		case 36, 37:
			b.storeFrom(4, rd, ra, update)
		case 38, 39:
			b.storeFrom(1, rd, ra, update)
		default:
			b.storeFrom(2, rd, ra, update)
		}
	case 58: // ld, ldu, lwa
		ds := uint32(int32(int16(inst & 0xfffc)))
		switch inst & 3 {
		case 0, 2:
			b.eaD(ra, ds)
			if inst&3 == 0 {
				b.loadTo(8, false, rd, ra, false)
			} else {
				b.loadTo(4, true, rd, ra, false)
			}
		case 1:
			if ra == 0 || ra == rd {
				return false, false
			}
			b.eaD(ra, ds)
			b.loadTo(8, false, rd, ra, true)
		default:
			return false, false
		}
	case 62: // std, stdu
		ds := uint32(int32(int16(inst & 0xfffc)))
		switch inst & 3 {
		case 0:
			b.eaD(ra, ds)
			b.storeFrom(8, rd, ra, false)
		case 1:
			if ra == 0 {
				return false, false
			}
			b.eaD(ra, ds)
			b.storeFrom(8, rd, ra, true)
		default:
			return false, false
		}
	case 48, 49, 50, 51, 52, 53, 54, 55: // lfs(u) lfd(u) stfs(u) stfd(u)
		update := op&1 != 0
		if update && ra == 0 {
			return false, false
		}
		b.eaD(ra, simm)
		b.fpMem(op < 52, op&2 != 0, rd, ra, update)
	case 59, 63:
		return b.fpArith(inst), false
	default:
		return false, false
	}
	return true, false
}

// fpMem: a float load or store at ECX (double: 8 bytes as they are;
// single: converted, with NaNs and infinities left to the interpreter,
// which keeps NaN payloads).
func (b *jitBlock) fpMem(load, double bool, d, ra uint32, update bool) {
	a := b.a
	offF := offFPR + int32(d)*8
	a.movRR(r10, rCX)
	if load {
		if double {
			b.load(8, false)
			a.movStore64(rBX, offF, rAX)
		} else {
			b.load(4, false)
			a.movRR(rDX, rAX)
			a.aluRI(aluAnd, rDX, 0x7f800000)
			a.aluRI(aluCmp, rDX, 0x7f800000)
			b.slowIf(ccE)
			a.movdToX(0, rAX)
			a.cvtss2sd(0)
			a.movsdStore(rBX, offF, 0)
		}
	} else {
		if double {
			a.movLoad64(r8, rBX, offF)
		} else {
			a.movLoad64(rAX, rBX, offF)
			a.movRR64(rDX, rAX)
			a.shiftRI64(shShr, rDX, 52)
			a.aluRI(aluAnd, rDX, 0x7ff)
			a.aluRI(aluCmp, rDX, 0x7ff)
			b.slowIf(ccE)
			a.movqToX(0, rAX)
			a.cvtsd2ss(0)
			a.movdFromX(r8, 0)
		}
		if double {
			b.store(8)
		} else {
			b.store(4)
		}
	}
	if update {
		a.movStore64(rBX, gpr(ra), r10)
	}
}

// branch emits a conditional branch: taken() is the jump when the
// condition holds; otherwise it goes on to pc+4. LR is set (when lk)
// either way.
func (b *jitBlock) branch(bo, bi uint32, lk bool, pc uint32, taken func()) {
	a := b.a
	var not []int
	if bo&4 == 0 {
		a.aluMI64(aluSub, rBX, offCTR, 1)
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
		a.storeImm64(rBX, offLR, uint64(pc+4))
	}
	taken()
	if len(not) > 0 {
		for _, at := range not {
			a.patch(at)
		}
		if lk {
			a.storeImm64(rBX, offLR, uint64(pc+4))
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

// inst30: the 64-bit rotates.
func (b *jitBlock) inst30(inst uint32) (ok, ends bool) {
	a := b.a
	s, ra := inst>>21&31, inst>>16&31
	sh := inst>>11&31 | (inst>>1&1)<<5
	mb := inst>>6&31 | (inst>>5&1)<<5
	a.movLoad64(rAX, rBX, gpr(s))
	switch inst >> 2 & 7 {
	case 0, 1, 2, 3:
		if sh != 0 {
			a.shiftRI64(shRol, rAX, uint8(sh))
		}
		var m uint64
		switch inst >> 2 & 7 {
		case 0: // rldicl
			m = mask64(mb, 63)
		case 1: // rldicr
			m = mask64(0, mb)
		default: // rldic, rldimi
			m = mask64(mb, 63-sh)
		}
		b.andMask(rAX, m)
		if inst>>2&7 == 3 {
			a.movLoad64(rCX, rBX, gpr(ra))
			b.andMask(rCX, ^m)
			a.aluRR64(aluOr, rAX, rCX)
		}
	default:
		switch inst >> 1 & 15 {
		case 8: // rldcl
			a.movLoad(rCX, rBX, gpr(inst>>11&31))
			a.shiftRCL64(shRol, rAX)
			b.andMask(rAX, mask64(mb, 63))
		case 9: // rldcr
			a.movLoad(rCX, rBX, gpr(inst>>11&31))
			a.shiftRCL64(shRol, rAX)
			b.andMask(rAX, mask64(0, mb))
		default:
			return false, false
		}
	}
	a.movStore64(rBX, gpr(ra), rAX)
	if inst&1 != 0 {
		b.cr0()
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
	switch xo & 0x1ff {
	case 8, 10, 11, 40, 104, 136, 138, 200, 202, 232, 233, 234, 235, 266, 457, 459, 489, 491, 9, 73, 75:
		if xo&0x200 != 0 {
			return false, false // overflow-recording forms: the interpreter
		}
	}
	// result in RAX goes to rd (arithmetic) or ra (logic), then CR0
	toRD := func() {
		a.movStore64(rBX, gpr(rd), rAX)
		if rc {
			b.cr0()
		}
	}
	toRA := func() {
		a.movStore64(rBX, gpr(ra), rAX)
		if rc {
			b.cr0()
		}
	}
	logic := func(op int, notB, notR bool) {
		a.movLoad64(rAX, rBX, gpr(rd))
		a.movLoad64(rCX, rBX, gpr(rb))
		if notB {
			a.unary64(2, rCX)
		}
		a.aluRR64(op, rAX, rCX)
		if notR {
			a.unary64(2, rAX)
		}
		toRA()
	}
	// carry: x (ra, or ~ra) + y (+ CA), CA from the carry out
	carry := func(notA bool, y int, useCA bool, one bool) {
		a.movLoad64(rAX, rBX, gpr(ra))
		if notA {
			a.unary64(2, rAX)
		}
		if y == 0 {
			a.movLoad64(rCX, rBX, gpr(rb))
		}
		switch {
		case useCA:
			b.loadCA()
		case one:
			a.byte1(0xf9) // stc
		default:
			a.byte1(0xf8) // clc
		}
		switch y {
		case 0:
			a.aluRR64(aluAdc, rAX, rCX)
		case 1: // + 0
			a.aluRI64(aluAdc, rAX, 0)
		case 2: // + ~0
			a.aluRI64(aluAdc, rAX, -1)
		}
		a.movRR64(r8, rAX)
		b.setCA(ccB)
		a.movRR64(rAX, r8)
		toRD()
	}
	switch xo & 0x1ff {
	case 266: // add
		a.movLoad64(rAX, rBX, gpr(ra))
		a.aluRM64(aluAdd, rAX, rBX, gpr(rb))
		toRD()
		return true, false
	case 10: // addc
		carry(false, 0, false, false)
		return true, false
	case 138: // adde
		carry(false, 0, true, false)
		return true, false
	case 202: // addze
		carry(false, 1, true, false)
		return true, false
	case 234: // addme
		carry(false, 2, true, false)
		return true, false
	case 8: // subfc
		carry(true, 0, false, true)
		return true, false
	case 136: // subfe
		carry(true, 0, true, false)
		return true, false
	case 200: // subfze
		carry(true, 1, true, false)
		return true, false
	case 232: // subfme
		carry(true, 2, true, false)
		return true, false
	case 40: // subf
		a.movLoad64(rAX, rBX, gpr(rb))
		a.aluRM64(aluSub, rAX, rBX, gpr(ra))
		toRD()
		return true, false
	case 104: // neg
		a.movLoad64(rAX, rBX, gpr(ra))
		a.unary64(3, rAX)
		toRD()
		return true, false
	case 235: // mullw
		a.movLoad(rAX, rBX, gpr(ra))
		a.movsxd(rAX, rAX)
		a.movLoad(rCX, rBX, gpr(rb))
		a.movsxd(rCX, rCX)
		a.imulRR64(rAX, rCX)
		toRD()
		return true, false
	case 233: // mulld
		a.movLoad64(rAX, rBX, gpr(ra))
		a.opRM(true, []byte{0x0f, 0xaf}, rAX, rBX, gpr(rb))
		toRD()
		return true, false
	case 75: // mulhw
		a.movLoad(rAX, rBX, gpr(ra))
		a.movsxd(rAX, rAX)
		a.movLoad(rCX, rBX, gpr(rb))
		a.movsxd(rCX, rCX)
		a.imulRR64(rAX, rCX)
		a.shiftRI64(shSar, rAX, 32)
		toRD()
		return true, false
	case 11: // mulhwu
		a.movLoad(rAX, rBX, gpr(ra))
		a.movLoad(rCX, rBX, gpr(rb))
		a.imulRR64(rAX, rCX)
		a.shiftRI64(shShr, rAX, 32)
		toRD()
		return true, false
	case 9, 73: // mulhdu, mulhd
		a.movLoad64(rAX, rBX, gpr(ra))
		a.movLoad64(rCX, rBX, gpr(rb))
		if xo == 73 {
			a.unary64(5, rCX)
		} else {
			a.unary64(4, rCX)
		}
		a.movRR64(rAX, rDX)
		toRD()
		return true, false
	case 459, 457: // divwu, divdu (by 0 in the interpreter)
		w := xo == 457
		if w {
			a.movLoad64(rCX, rBX, gpr(rb))
			a.testRR64(rCX, rCX)
		} else {
			a.movLoad(rCX, rBX, gpr(rb))
			a.testRR(rCX, rCX)
		}
		b.slowIf(ccE)
		a.aluRR(aluXor, rDX, rDX)
		if w {
			a.movLoad64(rAX, rBX, gpr(ra))
			a.unary64(6, rCX)
		} else {
			a.movLoad(rAX, rBX, gpr(ra))
			a.unary(6, rCX)
		}
		toRD()
		return true, false
	case 491: // divw (the odd cases in the interpreter)
		a.movLoad(rCX, rBX, gpr(rb))
		a.testRR(rCX, rCX)
		b.slowIf(ccE)
		a.aluRI(aluCmp, rCX, 0xffffffff)
		b.slowIf(ccE)
		a.movLoad(rAX, rBX, gpr(ra))
		a.byte1(0x99) // cdq
		a.unary(7, rCX)
		a.movRR(rAX, rAX) // the quotient, zero-extended
		toRD()
		return true, false
	case 489: // divd
		a.movLoad64(rCX, rBX, gpr(rb))
		a.testRR64(rCX, rCX)
		b.slowIf(ccE)
		a.aluRI64(aluCmp, rCX, -1)
		b.slowIf(ccE)
		a.movLoad64(rAX, rBX, gpr(ra))
		a.byte1(0x48, 0x99) // cqo
		a.unary64(7, rCX)
		toRD()
		return true, false
	}
	switch xo {
	case 0, 32: // cmp, cmpl
		if inst&(1<<21) != 0 {
			a.movLoad64(rAX, rBX, gpr(ra))
			a.aluRM64(aluCmp, rAX, rBX, gpr(rb))
		} else {
			a.movLoad(rAX, rBX, gpr(ra))
			a.aluRM(aluCmp, rAX, rBX, gpr(rb))
		}
		b.crFromFlags(rd>>2, xo == 0)
	case 28: // and
		logic(aluAnd, false, false)
	case 60: // andc
		logic(aluAnd, true, false)
	case 444: // or
		if rd == rb && !rc {
			a.movLoad64(rAX, rBX, gpr(rd))
			a.movStore64(rBX, gpr(ra), rAX)
			break
		}
		logic(aluOr, false, false)
	case 412: // orc
		logic(aluOr, true, false)
	case 316: // xor
		logic(aluXor, false, false)
	case 124: // nor
		logic(aluOr, false, true)
	case 476: // nand
		logic(aluAnd, false, true)
	case 284: // eqv
		logic(aluXor, false, true)
	case 24, 536: // slw, srw: shifts of 32 to 63 give 0
		a.movLoad(rCX, rBX, gpr(rb))
		a.movLoad(rAX, rBX, gpr(rd))
		if xo == 24 {
			a.shiftRCL(shShl, rAX)
		} else {
			a.shiftRCL(shShr, rAX)
		}
		a.aluRR(aluXor, rDX, rDX)
		a.testRI(rCX, 32)
		a.cmovRR(ccNE, rAX, rDX)
		toRA()
	case 27, 539: // sld, srd: shifts of 64 to 127 give 0
		a.movLoad(rCX, rBX, gpr(rb))
		a.movLoad64(rAX, rBX, gpr(rd))
		if xo == 27 {
			a.shiftRCL64(shShl, rAX)
		} else {
			a.shiftRCL64(shShr, rAX)
		}
		a.aluRR(aluXor, rDX, rDX)
		a.testRI(rCX, 64)
		a.cmovRR64(ccNE, rAX, rDX)
		toRA()
	case 824, 826, 827: // srawi, sradi
		n := rb
		w := xo != 824
		if w {
			n |= (inst >> 1 & 1) << 5
			a.movLoad64(rAX, rBX, gpr(rd))
		} else {
			a.movLoad(rAX, rBX, gpr(rd))
			a.movsxd(rAX, rAX)
		}
		if n == 0 {
			a.movStore64(rBX, gpr(ra), rAX)
			a.aluRR(aluXor, rCX, rCX) // CA = 0 (ZF set)
			b.setCA(ccNE)
		} else {
			// CA: negative and a 1 shifted out
			a.movRR64(rCX, rAX)
			b.andMask(rCX, 1<<n-1)
			a.movRR64(r8, rAX)
			a.shiftRI64(shSar, r8, uint8(n))
			a.movStore64(rBX, gpr(ra), r8)
			a.testRR64(rAX, rAX)
			a.setcc(ccS, rAX)
			a.testRR64(rCX, rCX)
			a.setcc(ccNE, rCX)
			a.aluRR(aluAnd, rAX, rCX)
			a.byte1(0xa8, 1) // test al, 1
			b.setCA(ccNE)
		}
		if rc {
			a.movLoad64(rAX, rBX, gpr(ra))
			b.cr0()
		}
	case 26, 58: // cntlzw, cntlzd
		if xo == 26 {
			a.movLoad(rCX, rBX, gpr(rd))
			a.movRI(rAX, 32)
			a.testRR(rCX, rCX)
		} else {
			a.movLoad64(rCX, rBX, gpr(rd))
			a.movRI(rAX, 64)
			a.testRR64(rCX, rCX)
		}
		zero := a.jcc(ccE)
		if xo == 26 {
			a.bsr(rAX, rCX)
			a.aluRI(aluXor, rAX, 31)
		} else {
			a.bsr64(rAX, rCX)
			a.aluRI(aluXor, rAX, 63)
		}
		a.patch(zero)
		toRA()
	case 922, 954, 986: // extsh, extsb, extsw
		a.movLoad(rAX, rBX, gpr(rd))
		switch xo {
		case 922:
			a.movsx16to64(rAX, rAX)
		case 954:
			a.movsx8to64(rAX, rAX)
		default:
			a.movsxd(rAX, rAX)
		}
		toRA()
	case 23, 55, 87, 119, 279, 311, 343, 375, 21, 53, 341, 373, 534, 532: // the indexed loads
		update := false
		switch xo {
		case 55, 119, 311, 375, 53, 373:
			update = true
		}
		if update && (ra == 0 || ra == rd) {
			return false, false
		}
		if update {
			a.movLoad(rCX, rBX, gpr(ra))
			a.aluRM(aluAdd, rCX, rBX, gpr(rb))
		} else {
			b.eaX(ra, rb)
		}
		switch xo {
		case 23, 55:
			b.loadTo(4, false, rd, ra, update)
		case 87, 119:
			b.loadTo(1, false, rd, ra, update)
		case 279, 311:
			b.loadTo(2, false, rd, ra, update)
		case 343, 375:
			b.loadTo(2, true, rd, ra, update)
		case 21, 53:
			b.loadTo(8, false, rd, ra, update)
		case 341, 373:
			b.loadTo(4, true, rd, ra, update)
		case 534: // lwbrx
			b.hostAddr(4, false)
			a.load32Idx(rAX, rDX, rAX)
			a.movStore64(rBX, gpr(rd), rAX)
		case 532: // ldbrx
			b.hostAddr(8, false)
			a.load64Idx(rAX, rDX, rAX)
			a.movStore64(rBX, gpr(rd), rAX)
		}
	case 151, 183, 215, 247, 407, 439, 149, 181, 662, 660: // the indexed stores
		update := false
		switch xo {
		case 183, 247, 439, 181:
			update = true
		}
		if update && ra == 0 {
			return false, false
		}
		if update {
			a.movLoad(rCX, rBX, gpr(ra))
			a.aluRM(aluAdd, rCX, rBX, gpr(rb))
		} else {
			b.eaX(ra, rb)
		}
		switch xo {
		case 151, 183:
			b.storeFrom(4, rd, ra, update)
		case 215, 247:
			b.storeFrom(1, rd, ra, update)
		case 407, 439:
			b.storeFrom(2, rd, ra, update)
		case 149, 181:
			b.storeFrom(8, rd, ra, update)
		case 662: // stwbrx
			a.movLoad(r8, rBX, gpr(rd))
			b.hostAddr(4, true)
			a.store32Idx(rDX, rAX, r8)
		case 660: // stdbrx
			a.movLoad64(r8, rBX, gpr(rd))
			b.hostAddr(8, true)
			a.store64Idx(rDX, rAX, r8)
		}
	case 535, 567, 599, 631, 663, 695, 727, 759: // lfsx ... stfdux
		update := xo&32 != 0
		if update && ra == 0 {
			return false, false
		}
		if update {
			a.movLoad(rCX, rBX, gpr(ra))
			a.aluRM(aluAdd, rCX, rBX, gpr(rb))
		} else {
			b.eaX(ra, rb)
		}
		b.fpMem(xo < 663, xo&64 != 0, rd, ra, update)
	case 339: // mfspr
		switch inst>>16&31 | (inst>>11&31)<<5 {
		case 1:
			a.movLoad64(rAX, rBX, offXER)
		case 8:
			a.movLoad64(rAX, rBX, offLR)
		case 9:
			a.movLoad64(rAX, rBX, offCTR)
		default:
			return false, false
		}
		a.movStore64(rBX, gpr(rd), rAX)
	case 467: // mtspr
		switch inst>>16&31 | (inst>>11&31)<<5 {
		case 1:
			a.movLoad(rAX, rBX, gpr(rd))
			a.aluRI(aluAnd, rAX, 0xe000007f)
			a.movStore64(rBX, offXER, rAX)
		case 8:
			a.movLoad64(rAX, rBX, gpr(rd))
			a.movStore64(rBX, offLR, rAX)
		case 9:
			a.movLoad64(rAX, rBX, gpr(rd))
			a.movStore64(rBX, offCTR, rAX)
		default:
			return false, false
		}
	case 19: // mfcr
		a.movLoad(rAX, rBX, offCR)
		a.movStore64(rBX, gpr(rd), rAX)
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
	case 103, 359, 231, 487: // lvx lvxl stvx stvxl
		if !spuJitOK {
			return false, false
		}
		b.eaX(ra, rb)
		a.aluRI(aluAnd, rCX, ^uint32(15))
		store := xo == 231 || xo == 487
		b.hostAddr(16, store)
		if store {
			a.movdquLoad(0, rBX, offVR+16*int32(rd))
		} else {
			a.movdquLoadIdx(0, rDX, rAX)
		}
		at := a.xRip(0x66, ssePshufb, 0)
		a.ripTo(at, 0, b.j.base)
		if store {
			a.movdquStoreIdx(rDX, rAX, 0)
		} else {
			a.movdquStore(rBX, offVR+16*int32(rd), 0)
		}
	case 519, 775, 551, 807: // lvlx lvlxl lvrx lvrxl
		if !spuJitOK {
			return false, false
		}
		b.lvlrx(xo == 551 || xo == 807, rd, ra, rb)
	case 983: // stfiwx
		b.eaX(ra, rb)
		a.movLoad(r8, rBX, fpr(rd))
		a.bswap(r8)
		b.hostAddr(4, true)
		a.store32Idx(rDX, rAX, r8)
	case 790: // lhbrx
		b.eaX(ra, rb)
		b.hostAddr(2, false)
		a.load16Idx(rAX, rDX, rAX)
		a.movStore64(rBX, gpr(rd), rAX)
	case 918: // sthbrx
		b.eaX(ra, rb)
		a.movLoad(r8, rBX, gpr(rd))
		b.hostAddr(2, true)
		a.store16Idx(rDX, rAX, r8)
	case 54, 86, 246, 278, 470, 598, 854, 342, 374, 822: // cache hints, sync, eieio
	default:
		return false, false
	}
	return true, false
}

// fpResult: x0 is FPR d's new value, which also sets the FPSCR result
// flags (pending: ppu_fpu.go's syncFPRF).
func (b *jitBlock) fpResult(d uint32) {
	a := b.a
	a.movsdStore(rBX, fpr(d), 0)
	a.movsdStore(rBX, offFpLast, 0)
	a.movStoreImm(rBX, offFpDirty, 1)
}

// negX flips the sign of xmm x (clobbers RAX, xmm7).
func (b *jitBlock) negX(x int) {
	a := b.a
	a.movRI64(rAX, 1<<63)
	a.movqToX(7, rAX)
	a.xorpd(x, 7)
}

// fpArith: the floating point arithmetic of primary opcodes 59 (single)
// and 63 (double). The record forms (Rc) go to the interpreter.
func (b *jitBlock) fpArith(inst uint32) bool {
	a := b.a
	op := inst >> 26
	d, ra, rb, rc := inst>>21&31, inst>>16&31, inst>>11&31, inst>>6&31
	if inst&1 != 0 {
		return false
	}
	if op == 63 && inst&0x20 == 0 {
		return b.fp63X(inst, d, ra, rb)
	}
	xo := inst >> 1 & 31
	switch xo {
	case 18, 20, 21, 25: // fdiv fsub fadd fmul
		a.movsdLoad(0, rBX, fpr(ra))
		if xo == 25 {
			a.movsdLoad(1, rBX, fpr(rc))
		} else {
			a.movsdLoad(1, rBX, fpr(rb))
		}
		a.sd(map[uint32]byte{18: sseDiv, 20: sseSub, 21: sseAdd, 25: sseMul}[xo], 0, 1)
	case 22: // fsqrt
		a.movsdLoad(1, rBX, fpr(rb))
		a.sd(sseSqrt, 0, 1)
	case 24, 26: // fres (single), frsqrte (double)
		if (xo == 24) != (op == 59) {
			return false
		}
		a.movsdLoad(1, rBX, fpr(rb))
		if xo == 26 {
			a.sd(sseSqrt, 1, 1)
		}
		a.movRI64(rAX, math.Float64bits(1))
		a.movqToX(0, rAX)
		a.sd(sseDiv, 0, 1)
	case 23: // fsel (double only): a >= 0 ? c : b
		if op != 63 {
			return false
		}
		a.movsdLoad(0, rBX, fpr(ra))
		a.xorpd(1, 1)
		a.ucomisd(0, 1)
		a.movLoad64(rAX, rBX, fpr(rb))
		a.movLoad64(rCX, rBX, fpr(rc))
		a.cmovRR64(ccAE, rAX, rCX)
		a.movStore64(rBX, fpr(d), rAX)
		return true
	case 28, 29, 30, 31: // fmsub fmadd fnmsub fnmadd: (-)(a*c +/- b)
		if !jitFMA {
			return false
		}
		a.movsdLoad(0, rBX, fpr(ra))
		a.movsdLoad(1, rBX, fpr(rc))
		a.movsdLoad(2, rBX, fpr(rb))
		if xo == 28 || xo == 30 {
			b.negX(2)
		}
		a.vfmadd213sd(0, 1, 2)
		if xo >= 30 {
			b.negX(0)
		}
	default:
		return false
	}
	if op == 59 {
		a.cvtsd2ss(0)
		a.cvtss2sd(0)
	}
	b.fpResult(d)
	return true
}

// fp63X: the X-form double instructions.
func (b *jitBlock) fp63X(inst, d, ra, rb uint32) bool {
	a := b.a
	switch inst >> 1 & 0x3ff {
	case 72, 40, 264, 136: // fmr fneg fabs fnabs
		a.movLoad64(rAX, rBX, fpr(rb))
		switch inst >> 1 & 0x3ff {
		case 40:
			a.movRI64(rCX, 1<<63)
			a.aluRR64(aluXor, rAX, rCX)
		case 264:
			a.movRI64(rCX, 1<<63-1)
			a.aluRR64(aluAnd, rAX, rCX)
		case 136:
			a.movRI64(rCX, 1<<63)
			a.aluRR64(aluOr, rAX, rCX)
		}
		a.movStore64(rBX, fpr(d), rAX)
	case 12: // frsp (NaNs in the interpreter, which keeps their payload)
		a.movLoad64(rAX, rBX, fpr(rb))
		a.movRR64(rCX, rAX)
		a.shiftRI64(shShr, rCX, 52)
		a.aluRI(aluAnd, rCX, 0x7ff)
		a.aluRI(aluCmp, rCX, 0x7ff)
		b.slowIf(ccE)
		a.movqToX(0, rAX)
		a.cvtsd2ss(0)
		a.cvtss2sd(0)
		b.fpResult(d)
	case 15, 815: // fctiwz, fctidz (positive overflow in the interpreter)
		a.movsdLoad(0, rBX, fpr(rb))
		if inst>>1&0x3ff == 15 {
			a.movRI64(rAX, math.Float64bits(2147483647))
		} else {
			a.movRI64(rAX, math.Float64bits(9.223372036854775807e18))
		}
		a.movqToX(1, rAX)
		a.ucomisd(0, 1)
		b.slowIf(ccAE)
		if inst>>1&0x3ff == 15 {
			a.cvttsd2si(rAX, 0) // (32-bit: NaN and too small give 0x80000000)
			a.movRI64(rCX, 0xfff8000000000000)
			a.aluRR64(aluOr, rAX, rCX)
		} else {
			a.byte1(0xf2)
			a.rex(true, rAX, 0, 0)
			a.byte1(0x0f, 0x2c)
			a.modRR(rAX, 0) // cvttsd2si rax, xmm0
		}
		a.movStore64(rBX, fpr(d), rAX)
	case 846: // fcfid
		a.movLoad64(rAX, rBX, fpr(rb))
		a.byte1(0xf2)
		a.rex(true, 0, 0, rAX)
		a.byte1(0x0f, 0x2a)
		a.modRR(0, rAX) // cvtsi2sd xmm0, rax
		b.fpResult(d)
	case 0, 32: // fcmpu, fcmpo
		b.fcmp(d>>2, ra, rb)
	default:
		return false
	}
	return true
}

// fcmp sets CR field f and FPSCR's FPCC from comparing FPRs ra and rb.
func (b *jitBlock) fcmp(f, ra, rb uint32) {
	a := b.a
	// the pending result flags first: only their C bit stays
	a.movLoad(rDX, rBX, offFPSCR)
	a.aluMI(aluCmp, rBX, offFpDirty, 0)
	clean := a.jcc(ccE)
	a.movLoad64(rAX, rBX, offFpLast)
	a.movRR64(rCX, rAX)
	a.shiftRI64(shShl, rCX, 1) // without the sign
	a.aluRR(aluXor, r8, r8)
	// C: NaN (above infinity), -0, or denormal (nonzero, exponent 0)
	a.movRI64(r10, 0xffe0000000000000)
	a.aluRR64(aluCmp, rCX, r10)
	a.setcc(ccA, r8)
	a.movRI64(r10, 0x0020000000000000)
	a.aluRR64(aluCmp, rCX, r10)
	notTiny := a.jcc(ccAE)
	a.testRR64(rCX, rCX)
	a.setcc(ccNE, r10)
	a.aluRR(aluOr, r8, r10)
	a.testRR64(rAX, rAX)
	a.setcc(ccS, r10)
	a.aluRR(aluOr, r8, r10)
	a.patch(notTiny)
	a.aluRI(aluAnd, r8, 1)
	a.shiftRI(shShl, r8, 16)
	a.aluRI(aluAnd, rDX, ^uint32(0x10000))
	a.aluRR(aluOr, rDX, r8)
	a.movStoreImm(rBX, offFpDirty, 0)
	a.patch(clean)
	// the compare: FPCC = 8 <, 4 >, 2 =, 1 unordered
	a.movsdLoad(0, rBX, fpr(ra))
	a.movsdLoad(1, rBX, fpr(rb))
	a.ucomisd(0, 1)
	a.movRI(rCX, 1)
	un := a.jcc(ccP)
	a.movRI(rCX, 2)
	eq := a.jcc(ccE)
	a.movRI(rCX, 4)
	gt := a.jcc(ccA)
	a.movRI(rCX, 8)
	a.patch(un)
	a.patch(eq)
	a.patch(gt)
	a.aluRI(aluAnd, rDX, ^uint32(0xf<<12))
	a.movRR(rAX, rCX)
	a.shiftRI(shShl, rAX, 12)
	a.aluRR(aluOr, rDX, rAX)
	a.movStore(rBX, offFPSCR, rDX)
	if sh := uint8(28 - 4*f); sh != 0 {
		a.shiftRI(shShl, rCX, sh)
	}
	a.movLoad(rDX, rBX, offCR)
	a.aluRI(aluAnd, rDX, ^(uint32(0xf) << (28 - 4*f)))
	a.aluRR(aluOr, rDX, rCX)
	a.movStore(rBX, offCR, rDX)
}
