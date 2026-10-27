package ps3

// The SPU recompiler: SPU code is translated, a block at a time, into
// x86-64 SSE code, the same way the PPU's is (ppujit.go). A register's
// four words are an xmm register's four lanes (word 0 in lane 0), so word
// operations map straight across; byte and halfword positions inside a
// word are mirrored (the SPU numbers bytes from the top), which the byte
// shuffles take care of. Local store is big-endian, so quadword loads and
// stores swap the bytes in each word.
//
// Single precision runs with denormals read and written as zero, as on the
// SPU. Instructions it doesn't translate (channels, stop, rarer forms) end
// the block and run in the interpreter.
//
// While recompiled code runs these x86 registers are fixed:
//
//	RBX  the SPUCore
//	R12  the local store
//	R13  the block table: an entry point for each local store word, or 0
//	RDI  the code map: a byte for each 16 bytes of local store, nonzero
//	     where there's compiled code (a store there goes to the
//	     interpreter, which forgets the code)
//	R9   the instruction budget left (signed)

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"strings"
	"unsafe"
)

const spuJitBufSize = 16 << 20

// spuJitLog (PS3_SPUJITLOG=file): the blocks compiled (debugging)
var spuJitLog = func() *os.File {
	if p := os.Getenv("PS3_SPUJITLOG"); p != "" {
		os.MkdirAll("/tmp/claude-0/ps3/spublk", 0o755)
		f, _ := os.Create(p)
		return f
	}
	return nil
}()

var (
	spuJitOK   = jitSupported && !jitOff && cpuHasSSE41()
	spuJitFMA  = spuJitOK && cpuHasFMA()
	spuJitAVX2 = spuJitOK && cpuHasAVX2()

	offSGPR = int32(unsafe.Offsetof(SPUCore{}.GPR))
	offSPC  = int32(unsafe.Offsetof(SPUCore{}.PC))
)

func sgpr(r uint32) int32 { return offSGPR + int32(r)*16 }

// the tables every block can use, at the start of the buffer
const (
	tabBswap   = 0                // [16]byte: big-endian words <-> lanes
	tabRotqby  = tabBswap + 16    // [16][16]byte by n
	tabShlqby  = tabRotqby + 256  // [32][16]byte
	tabRotqmby = tabShlqby + 512  // [32][16]byte
	tabFsm     = tabRotqmby + 512 // [16][16]byte
	tabSize    = tabFsm + 256
)

// spuJIT is one SPU's recompiler.
type spuJIT struct {
	buf      []byte
	base     uintptr
	used     int
	tabs     uintptr
	dispatch uintptr

	table [lsSize / 4]uintptr
	code  [lsSize / 16]byte
	// the blocks compiled from each 16 bytes
	lineBlocks [lsSize / 16][]uint32

	Blocks, Flushes, Translated, Interp, Invalidated int
}

func newSPUJIT() *spuJIT {
	if !spuJitOK {
		return nil
	}
	buf := jitAlloc(spuJitBufSize)
	if buf == nil {
		return nil
	}
	j := &spuJIT{buf: buf, base: uintptr(unsafe.Pointer(&buf[0]))}
	j.reset()
	return j
}

// shufTables makes the tables (tabBswap...) for byte shuffles of
// big-endian vectors held as host words.
func shufTables() []byte {
	t := make([]byte, tabSize)
	for i := 0; i < 16; i++ {
		t[tabBswap+i] = byte(i ^ 3)
	}
	for n := 0; n < 32; n++ {
		for jj := 0; jj < 16; jj++ {
			i := hostPos(jj) // the vector byte at host position jj
			if n < 16 {
				t[tabRotqby+16*n+jj] = byte(hostPos((i + n) & 15))
			}
			if i+n < 16 {
				t[tabShlqby+16*n+jj] = byte(hostPos(i + n))
			} else {
				t[tabShlqby+16*n+jj] = 0x80
			}
			if i-n >= 0 {
				t[tabRotqmby+16*n+jj] = byte(hostPos(i - n))
			} else {
				t[tabRotqmby+16*n+jj] = 0x80
			}
		}
	}
	for m := 0; m < 16; m++ {
		for k := 0; k < 4; k++ {
			if m>>(3-k)&1 != 0 {
				binary.LittleEndian.PutUint32(t[tabFsm+16*m+4*k:], 0xffffffff)
			}
		}
	}
	return t
}

// hostPos is where SPU byte i of a register is in its xmm register.
func hostPos(i int) int { return i ^ 3 }

func (j *spuJIT) reset() {
	clear(j.table[:])
	clear(j.code[:])
	for i := range j.lineBlocks {
		j.lineBlocks[i] = j.lineBlocks[i][:0]
	}
	j.used = 0
	t := shufTables()
	copy(j.buf, t)
	j.tabs = j.base
	j.used = (tabSize + 15) &^ 15
	a := &x86{base: j.base + uintptr(j.used)}
	j.emitDispatch(a)
	j.dispatch = j.place(a)
	j.Flushes++
}

func (j *spuJIT) place(a *x86) uintptr {
	at := j.base + uintptr(j.used)
	copy(j.buf[j.used:], a.b)
	j.used += (len(a.b) + 15) &^ 15
	return at
}

// the dispatcher: EAX = the next PC
func (j *spuJIT) emitDispatch(a *x86) {
	a.movRR(rCX, rAX)
	a.aluRI(aluAnd, rCX, lsMask&^3)
	a.opRMI(true, []byte{0x8b}, rDX, r13, rCX, 2, 0) // mov rdx, [r13 + pc*2]
	a.testRR64(rDX, rDX)
	miss := a.jcc(ccE)
	a.jmpReg(rDX)
	a.patch(miss)
	a.movStore(rBX, offSPC, rCX)
	a.movRI(rAX, jitExitCompile)
	a.ret()
}

// lsWrote: local store a..a+n-1 changed (DMA, the interpreter's stores).
// The blocks compiled from it are forgotten.
func (j *spuJIT) lsWrote(a uint32, n int) {
	if n <= 0 {
		return
	}
	a &= lsMask
	first, last := a>>4, (a+uint32(n)-1)>>4
	if last >= lsSize/16 {
		j.lsWrote(0, int(a+uint32(n)-lsSize))
		last = lsSize/16 - 1
	}
	for l := first; l <= last; l++ {
		if j.code[l] == 0 {
			continue
		}
		if spuJitLog != nil {
			fmt.Fprintf(spuJitLog, "write %05x+%x hits code at %05x\n", a, n, l<<4)
		}
		for _, pc := range j.lineBlocks[l] {
			j.table[pc>>2] = 0
		}
		j.lineBlocks[l] = j.lineBlocks[l][:0]
		j.code[l] = 0
		j.Invalidated++
	}
}

// run runs up to n instructions; it returns how many ran.
func (j *spuJIT) run(c *SPUCore, n int) int {
	left := int64(n)
	ran := 0
	for left > 0 && !c.stop {
		pc := c.PC & lsMask &^ 3
		entry := j.table[pc>>2]
		if entry == 0 {
			entry = j.compile(c, pc)
		}
		args := jitArgs{ctx: uintptr(unsafe.Pointer(c)), mem: uintptr(unsafe.Pointer(&c.LS[0])),
			blocks: uintptr(unsafe.Pointer(&j.table[0])), budget: left, memW: uintptr(unsafe.Pointer(&j.code[0]))}
		exit := spuJitCall(entry, &args)
		ran += int(left - args.budget)
		left = args.budget
		switch exit {
		case jitExitBudget:
			c.Ran += uint64(ran)
			return ran
		case jitExitInterp:
			if left <= 0 {
				break
			}
			pc := c.PC & lsMask &^ 3
			w := binary.BigEndian.Uint32(c.LS[pc:])
			c.PC = pc + 4
			c.stalled = false
			c.exec(w)
			j.Interp++
			if jitInterpHist != nil {
				jitInterpHist["spu "+spuOpName(w)]++
			}
			if c.stalled {
				c.PC = pc
				c.stop = true
				break
			}
			ran++
			left--
		}
	}
	c.Ran += uint64(ran)
	return ran
}

// spuOpName: the mnemonic of w (for statistics).
func spuOpName(w uint32) string {
	d := SPUDisasm(0, w)
	if i := strings.IndexByte(d, ' '); i > 0 {
		return d[:i]
	}
	return d
}

func (j *spuJIT) compile(c *SPUCore, pc uint32) uintptr {
	if j.used > len(j.buf)-1<<20 {
		j.reset()
	}
	a := &x86{base: j.base + uintptr(j.used)}
	b := &spuBlock{a: a, j: j, c: c, start: pc}
	b.build()
	code := j.place(a)
	if spuJitLog != nil {
		fmt.Fprintf(spuJitLog, "block %05x n %d at %x len %x\n", pc, b.n, code, len(a.b))
		os.WriteFile(fmt.Sprintf("/tmp/claude-0/ps3/spublk/%05x.bin", pc), a.b, 0o644)
	}
	j.table[pc>>2] = code
	for l := pc >> 4; l <= (pc+uint32(4*b.n)-1)>>4 && l < lsSize/16 && b.n > 0; l++ {
		j.code[l] = 1
		j.lineBlocks[l] = append(j.lineBlocks[l], pc)
	}
	j.Blocks++
	return code
}

type spuBlock struct {
	a     *x86
	j     *spuJIT
	c     *SPUCore
	start uint32
	n     int
	slow  []*jitSlow
	cur   *jitSlow
	pool  [][16]byte
	refs  []poolRef
}

type poolRef struct{ at, immLen, idx int }

func (b *spuBlock) build() {
	a := b.a
	a.testRR64(r9, r9)
	budgetOut := a.jcc(ccLE)
	a.byte1(0x49, 0x81, 0xe9) // sub r9, imm32
	lenAt := a.pos()
	a.imm32(0)

	pc := b.start
	done, stopped := false, false
	for b.n < jitMaxBlock && !done && pc < lsSize {
		w := binary.BigEndian.Uint32(b.c.LS[pc:])
		b.cur = &jitSlow{pc: pc}
		mark, marks := a.pos(), len(b.refs)
		ok, ends := b.inst(w, pc)
		if !ok {
			a.b = a.b[:mark]
			b.refs = b.refs[:marks]
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
		a.movStoreImm(rBX, offSPC, pc&lsMask)
		a.movRI(rAX, jitExitInterp)
		a.ret()
	}
	if b.n == 0 {
		a.b = a.b[:0]
		b.pool, b.refs = nil, nil
		a.movStoreImm(rBX, offSPC, b.start)
		a.movRI(rAX, jitExitInterp)
		a.ret()
		return
	}
	b.j.Translated += b.n
	n := uint32(b.n)
	a.b[lenAt], a.b[lenAt+1], a.b[lenAt+2], a.b[lenAt+3] = byte(n), byte(n>>8), byte(n>>16), byte(n>>24)
	a.patch(budgetOut)
	a.movStoreImm(rBX, offSPC, b.start)
	a.movRI(rAX, jitExitBudget)
	a.ret()
	for _, s := range b.slow {
		for _, at := range s.at {
			a.patch(at)
		}
		a.aluRI64(aluAdd, r9, int32(b.n-s.refund))
		a.movStoreImm(rBX, offSPC, s.pc)
		a.movRI(rAX, jitExitInterp)
		a.ret()
	}
	// the constants, 16-byte aligned (the block starts aligned)
	if len(b.pool) > 0 {
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
}

func (b *spuBlock) slowIf(cc int) { b.cur.at = append(b.cur.at, b.a.jcc(cc)) }

func (b *spuBlock) toPC(pc uint32) {
	b.a.movRIKeepFlags(rAX, pc&lsMask)
	b.a.jmpAbs(b.j.dispatch)
}

// constant index for v (lanes as host words)
func (b *spuBlock) constIdx(v [16]byte) int {
	for i, p := range b.pool {
		if p == v {
			return i
		}
	}
	b.pool = append(b.pool, v)
	return len(b.pool) - 1
}

func words(w0, w1, w2, w3 uint32) (v [16]byte) {
	binary.LittleEndian.PutUint32(v[0:], w0)
	binary.LittleEndian.PutUint32(v[4:], w1)
	binary.LittleEndian.PutUint32(v[8:], w2)
	binary.LittleEndian.PutUint32(v[12:], w3)
	return
}

func splatW(v uint32) [16]byte { return words(v, v, v, v) }

// xc: op x, [constant v] (66 prefix unless ps).
func (b *spuBlock) xc(pfx byte, op []byte, x int, v [16]byte) {
	at := b.a.xRip(pfx, op, x)
	b.refs = append(b.refs, poolRef{at, 0, b.constIdx(v)})
}

// ldc loads constant v into x.
func (b *spuBlock) ldc(x int, v [16]byte) { b.xc(0x66, sseMovdqa, x, v) }

// pc: a 66-prefixed integer op with a constant.
func (b *spuBlock) pc(op []byte, x int, v [16]byte) { b.xc(0x66, op, x, v) }

// tab: lea rdx, the table at off.
func (b *spuBlock) tab(off int) {
	at := b.a.leaRip(rDX)
	b.a.ripTo(at, 0, b.j.tabs+uintptr(off))
}

func (b *spuBlock) ld(x int, r uint32) { b.a.movdquLoad(x, rBX, sgpr(r)) }
func (b *spuBlock) st(r uint32, x int) { b.a.movdquStore(rBX, sgpr(r), x) }

// setWords stores constant words into register r.
func (b *spuBlock) setWords(r uint32, v Vec) {
	for k := 0; k < 4; k++ {
		b.a.movStoreImm(rBX, sgpr(r)+int32(4*k), v[k])
	}
}

// lsLoad: x = the quadword at ECX (masked here).
func (b *spuBlock) lsLoad(x int) {
	a := b.a
	a.aluRI(aluAnd, rCX, lsMask&^15)
	a.movdquLoadIdx(x, r12, rCX)
	b.tab(tabBswap)
	a.xRM(0x66, ssePshufb, x, rDX, 0)
}

// lsStore: the quadword at ECX = x (to the interpreter if it's code).
func (b *spuBlock) lsStore(x int) {
	a := b.a
	a.aluRI(aluAnd, rCX, lsMask&^15)
	a.movRR(rDX, rCX)
	a.shiftRI(shShr, rDX, 4)
	a.opRMI(false, []byte{0x80}, 7, rDI, rDX, 1, 0) // cmp byte [rdi+rdx], 0
	a.byte1(0)
	b.slowIf(ccNE)
	b.tab(tabBswap)
	a.xRM(0x66, ssePshufb, x, rDX, 0)
	a.movdquStoreIdx(r12, rCX, x)
}

// binop: rT = a op b (op on x0 with x1, result x0); swap: x1 op x0.
func (b *spuBlock) binop(rT, rA, rB uint32, op []byte, pfx byte, swap bool) {
	b.ld(0, rA)
	b.ld(1, rB)
	if swap {
		b.a.xRR(pfx, op, 1, 0)
		b.st(rT, 1)
		return
	}
	b.a.xRR(pfx, op, 0, 1)
	b.st(rT, 0)
}

// cond emits a branch: test sets the flags, jumping when cc holds.
func (b *spuBlock) cond(pc uint32, test func(), cc int, taken func()) {
	test()
	not := b.a.jcc(cc ^ 1)
	taken()
	b.a.patch(not)
	b.toPC(pc + 4)
}

func (b *spuBlock) inst(w, pc uint32) (ok, ends bool) {
	a := b.a
	rT := w & 0x7f
	rA := w >> 7 & 0x7f
	rB := w >> 14 & 0x7f
	switch w >> 28 {
	case 0x8, 0xb, 0xc, 0xd, 0xe, 0xf:
		t := w >> 21 & 0x7f
		rC := w & 0x7f
		b.ld(0, rA)
		b.ld(1, rB)
		b.ld(2, rC)
		switch w >> 28 {
		case 0x8: // selb: c ? b : a
			a.pi(ssePand, 1, 2)
			a.pi(ssePandn, 2, 0)
			a.pi(ssePor, 1, 2)
			b.st(t, 1)
		case 0xb: // shufb
			a.movdqa(3, 2)
			b.pc(ssePand, 3, splatW(0x0f0f0f0f))
			b.pc(ssePxor, 3, splatW(0x03030303)) // host byte positions
			a.pi(ssePshufb, 0, 3)
			a.pi(ssePshufb, 1, 3)
			a.movdqa(4, 2)
			b.pc(ssePand, 4, splatW(0x10101010))
			b.pc(ssePcmpeqb, 4, splatW(0x10101010)) // from b
			a.pi(ssePand, 1, 4)
			a.pi(ssePandn, 4, 0)
			a.pi(ssePor, 1, 4) // the selected bytes
			a.pi(ssePxor, 5, 5)
			a.pi(ssePcmpgtb, 5, 2) // the special codes (top bit set)
			a.movdqa(6, 2)
			b.pc(ssePand, 6, splatW(0x40404040))
			b.pc(ssePcmpeqb, 6, splatW(0x40404040))
			a.movdqa(7, 2)
			b.pc(ssePand, 7, splatW(0x20202020))
			b.pc(ssePcmpeqb, 7, splatW(0x20202020))
			b.pc(ssePand, 7, splatW(0x7f7f7f7f))
			a.pi(ssePandn, 7, 6) // 10x: 00, 110: ff, 111: 80
			a.pi(ssePand, 7, 5)
			a.pi(ssePandn, 5, 1)
			a.pi(ssePor, 5, 7)
			b.st(t, 5)
		case 0xc: // mpya
			a.pshiftI(shGrpD, shExtL, 0, 16)
			a.pshiftI(shGrpD, shExtA, 0, 16)
			a.pshiftI(shGrpD, shExtL, 1, 16)
			a.pshiftI(shGrpD, shExtA, 1, 16)
			a.pi(ssePmulld, 0, 1)
			a.pi(ssePaddd, 0, 2)
			b.st(t, 0)
		default:
			if !spuJitFMA {
				return false, false
			}
			switch w >> 28 {
			case 0xd: // fnms: c - a*b
				a.vfnmadd213ps(0, 1, 2)
			case 0xe: // fma
				a.vfmadd213ps(0, 1, 2)
			case 0xf: // fms
				a.vfmsub213ps(0, 1, 2)
			}
			b.st(t, 0)
		}
		return true, false
	}
	switch w >> 25 {
	case 0x21: // ila
		v := i18(w)
		b.setWords(rT, Vec{v, v, v, v})
		return true, false
	case 0x08, 0x09: // hbra, hbrr
		return true, false
	}
	if ok, known := b.inst8(w, rT, rA, pc); known {
		return ok, false
	}
	if ok, ends, known := b.inst9(w, rT, pc); known {
		return ok, ends
	}
	switch w >> 22 {
	case 0x1d8, 0x1da: // cflts, csflt
		return b.conv(w, rT, rA), false
	case 0x1d9, 0x1db:
		return false, false
	}
	return b.inst11(w, rT, rA, rB, pc)
}

// inst8: the RI10 forms. known = false: not one of them.
func (b *spuBlock) inst8(w, rT, rA, pc uint32) (ok, known bool) {
	a := b.a
	im := i10(w)
	u := uint32(im)
	h := uint32(uint16(im)) * 0x10001
	by := uint32(uint8(im)) * 0x01010101
	imm := func(op []byte, v uint32) {
		b.ld(0, rA)
		b.pc(op, 0, splatW(v))
		b.st(rT, 0)
	}
	switch w >> 24 {
	case 0x34: // lqd
		a.movLoad(rCX, rBX, sgpr(rA))
		a.aluRI(aluAdd, rCX, u<<4)
		b.lsLoad(0)
		b.st(rT, 0)
	case 0x24: // stqd
		a.movLoad(rCX, rBX, sgpr(rA))
		a.aluRI(aluAdd, rCX, u<<4)
		b.ld(0, rT)
		b.lsStore(0)
	case 0x1c: // ai
		imm(ssePaddd, u)
	case 0x1d: // ahi
		imm(ssePaddw, h)
	case 0x0c, 0x0d: // sfi, sfhi: im - a
		b.ld(1, rA)
		if w>>24 == 0x0c {
			b.ldc(0, splatW(u))
			a.pi(ssePsubd, 0, 1)
		} else {
			b.ldc(0, splatW(h))
			a.pi(ssePsubw, 0, 1)
		}
		b.st(rT, 0)
	case 0x14:
		imm(ssePand, u)
	case 0x15:
		imm(ssePand, h)
	case 0x16:
		imm(ssePand, by)
	case 0x04:
		imm(ssePor, u)
	case 0x05:
		imm(ssePor, h)
	case 0x06:
		imm(ssePor, by)
	case 0x44:
		imm(ssePxor, u)
	case 0x45:
		imm(ssePxor, h)
	case 0x46:
		imm(ssePxor, by)
	case 0x74: // mpyi
		b.ld(0, rA)
		a.pshiftI(shGrpD, shExtL, 0, 16)
		a.pshiftI(shGrpD, shExtA, 0, 16)
		b.pc(ssePmulld, 0, splatW(u))
		b.st(rT, 0)
	case 0x75: // mpyui
		b.ld(0, rA)
		b.pc(ssePand, 0, splatW(0xffff))
		b.pc(ssePmulld, 0, splatW(u&0xffff))
		b.st(rT, 0)
	case 0x7c: // ceqi
		imm(ssePcmpeqd, u)
	case 0x4c: // cgti
		imm(ssePcmpgtd, u)
	case 0x5c: // clgti
		b.ld(0, rA)
		b.pc(ssePxor, 0, splatW(0x80000000))
		b.pc(ssePcmpgtd, 0, splatW(u^0x80000000))
		b.st(rT, 0)
	case 0x7d: // ceqhi
		imm(ssePcmpeqw, h)
	case 0x4d: // cgthi
		imm(ssePcmpgtw, h)
	case 0x5d: // clgthi
		b.ld(0, rA)
		b.pc(ssePxor, 0, splatW(0x80008000))
		b.pc(ssePcmpgtw, 0, splatW(h^0x80008000))
		b.st(rT, 0)
	case 0x7e: // ceqbi
		imm(ssePcmpeqb, by)
	case 0x4e: // cgtbi
		imm(ssePcmpgtb, by)
	case 0x5e: // clgtbi
		b.ld(0, rA)
		b.pc(ssePxor, 0, splatW(0x80808080))
		b.pc(ssePcmpgtb, 0, splatW(by^0x80808080))
		b.st(rT, 0)
	case 0x7f, 0x4f, 0x5f: // halts
		return false, true
	default:
		return false, false
	}
	return true, true
}

// inst9: the RI16 forms.
func (b *spuBlock) inst9(w, rT, pc uint32) (ok, ends, known bool) {
	a := b.a
	im := uint32(i16(w))
	rel := (pc + im<<2) & lsMask
	abs := im << 2 & lsMask
	switch w >> 23 {
	case 0x061, 0x067: // lqa, lqr
		ea := abs
		if w>>23 == 0x067 {
			ea = rel
		}
		a.movRI(rCX, ea)
		b.lsLoad(0)
		b.st(rT, 0)
	case 0x041, 0x047: // stqa, stqr
		ea := abs
		if w>>23 == 0x047 {
			ea = rel
		}
		a.movRI(rCX, ea)
		b.ld(0, rT)
		b.lsStore(0)
	case 0x083: // ilh
		v := u16f(w) * 0x10001
		b.setWords(rT, Vec{v, v, v, v})
	case 0x082: // ilhu
		v := u16f(w) << 16
		b.setWords(rT, Vec{v, v, v, v})
	case 0x081: // il
		b.setWords(rT, Vec{im, im, im, im})
	case 0x0c1: // iohl
		for k := int32(0); k < 4; k++ {
			a.aluMI(aluOr, rBX, sgpr(rT)+4*k, u16f(w))
		}
	case 0x065: // fsmbi
		m := u16f(w)
		var r Vec
		for k := 0; k < 16; k++ {
			if m>>(15-k)&1 != 0 {
				r = r.withB(k, 0xff)
			}
		}
		b.setWords(rT, r)
	case 0x064: // br
		b.toPC(rel)
		return true, true, true
	case 0x060: // bra
		b.toPC(abs)
		return true, true, true
	case 0x066, 0x062: // brsl, brasl
		b.setWords(rT, Vec{(pc + 4) & lsMask, 0, 0, 0})
		if w>>23 == 0x066 {
			b.toPC(rel)
		} else {
			b.toPC(abs)
		}
		return true, true, true
	case 0x042, 0x040: // brnz, brz
		cc := ccNE
		if w>>23 == 0x040 {
			cc = ccE
		}
		b.cond(pc, func() { a.aluMI(aluCmp, rBX, sgpr(rT), 0) }, cc, func() { b.toPC(rel) })
		return true, true, true
	case 0x046, 0x044: // brhnz, brhz
		cc := ccNE
		if w>>23 == 0x044 {
			cc = ccE
		}
		b.cond(pc, func() { a.testMI(rBX, sgpr(rT), 0xffff) }, cc, func() { b.toPC(rel) })
		return true, true, true
	default:
		return false, false, false
	}
	return true, false, true
}

// conv: cflts and csflt, when the scale is a normal float.
func (b *spuBlock) conv(w, rT, rA uint32) bool {
	a := b.a
	i8 := int(w >> 14 & 0xff)
	b.ld(0, rA)
	if w>>22 == 0x1da { // csflt: float(a) * 2^-(155-i8)
		e := -(155 - i8)
		if e < -126 || e > 127 {
			return false
		}
		a.ps(sseCvtdq2ps, 0, 0)
		if e != 0 {
			b.xc(0, sseMulps, 0, splatW(math.Float32bits(float32(math.Ldexp(1, e)))))
		}
		b.st(rT, 0)
		return true
	}
	// cflts: int(a * 2^(173-i8)), saturating, NaN -> 0
	e := 173 - i8
	if e < -126 || e > 127 {
		return false
	}
	if e != 0 {
		b.xc(0, sseMulps, 0, splatW(math.Float32bits(float32(math.Ldexp(1, e)))))
	}
	a.movdqa(1, 0)
	a.xRR(0xf3, sseCvttps2dq, 1, 0)
	b.ldc(2, splatW(0x4f000000)) // 2^31
	a.cmpps(2, 0, 2)             // 2^31 <= x: 0x7fffffff
	a.pi(ssePxor, 1, 2)
	a.cmpps(0, 0, 7) // ordered: NaN -> 0
	a.pi(ssePand, 1, 0)
	b.st(rT, 1)
	return true
}

func (b *spuBlock) inst11(w, rT, rA, rB, pc uint32) (ok, ends bool) {
	a := b.a
	op := w >> 21
	n7 := uint32(i7(w))
	logic := func(op []byte, notB, notR bool) {
		b.ld(0, rA)
		b.ld(1, rB)
		if notB || notR {
			a.pi(ssePcmpeqd, 7, 7)
		}
		if notB {
			a.pi(ssePxor, 1, 7)
		}
		a.pi(op, 0, 1)
		if notR {
			a.pi(ssePxor, 0, 7)
		}
		b.st(rT, 0)
	}
	unsignedCmp := func(op []byte, bias uint32) {
		b.ld(0, rA)
		b.ld(1, rB)
		b.pc(ssePxor, 0, splatW(bias))
		b.pc(ssePxor, 1, splatW(bias))
		a.pi(op, 0, 1)
		b.st(rT, 0)
	}
	// shift by an immediate: grp/ext, n; zero when out of range
	shiftImm := func(grp byte, ext int, n, limit uint32) {
		if n >= limit {
			a.pi(ssePxor, 0, 0)
		} else {
			b.ld(0, rA)
			if n != 0 {
				a.pshiftI(grp, ext, 0, uint8(n))
			}
		}
		b.st(rT, 0)
	}
	rotImm := func(grp byte, n, size uint32) {
		b.ld(0, rA)
		if n != 0 {
			a.movdqa(1, 0)
			a.pshiftI(grp, shExtL, 0, uint8(n))
			a.pshiftI(grp, shExtR, 1, uint8(size-n))
			a.pi(ssePor, 0, 1)
		}
		b.st(rT, 0)
	}
	// byte shuffle by a table entry: n in ECX (masked by mask)
	shufVar := func(table int, mask uint32) {
		a.aluRI(aluAnd, rCX, mask)
		a.shiftRI(shShl, rCX, 4)
		b.tab(table)
		b.ld(0, rA)
		a.xRMI(0x66, ssePshufb, 0, rDX, rCX, 1, 0)
		b.st(rT, 0)
	}
	shufImm := func(table int, n uint32) {
		b.tab(table + 16*int(n))
		b.ld(0, rA)
		a.xRM(0x66, ssePshufb, 0, rDX, 0)
		b.st(rT, 0)
	}
	branchTo := func() { // EAX = a's word 0 as a PC; to the dispatcher
		a.aluRI(aluAnd, rAX, lsMask&^3)
		a.jmpAbs(b.j.dispatch)
	}
	switch op {
	case 0x001, 0x201, 0x002, 0x003, 0x1ac, 0x10c, 0x3ba: // lnop nop sync dsync hbr mtspr fscrwr
	case 0x00c: // mfspr
		b.setWords(rT, Vec{})
	case 0x1c4: // lqx
		a.movLoad(rCX, rBX, sgpr(rA))
		a.aluRM(aluAdd, rCX, rBX, sgpr(rB))
		b.lsLoad(0)
		b.st(rT, 0)
	case 0x144: // stqx
		a.movLoad(rCX, rBX, sgpr(rA))
		a.aluRM(aluAdd, rCX, rBX, sgpr(rB))
		b.ld(0, rT)
		b.lsStore(0)
	case 0x1f4, 0x1f5, 0x1f6, 0x1f7, 0x1d4, 0x1d5, 0x1d6, 0x1d7: // cbd chd cwd cdd and the x forms
		a.movLoad(rCX, rBX, sgpr(rA))
		if op >= 0x1f4 {
			a.aluRI(aluAdd, rCX, n7)
		} else {
			a.aluRM(aluAdd, rCX, rBX, sgpr(rB))
		}
		b.setWords(rT, Vec{0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f})
		off := sgpr(rT)
		switch op & 3 {
		case 0: // byte (t&15) = 03
			a.aluRI(aluAnd, rCX, 15)
			a.aluRI(aluXor, rCX, 3)
			a.opRMI(false, []byte{0xc6}, 0, rBX, rCX, 1, off) // mov byte [rbx+rcx+off], 3
			a.byte1(3)
		case 1: // halfword (t&14)/2 = 0203
			a.aluRI(aluAnd, rCX, 14)
			a.aluRI(aluXor, rCX, 2)
			a.byte1(0x66)
			a.opRMI(false, []byte{0xc7}, 0, rBX, rCX, 1, off)
			a.byte1(0x03, 0x02)
		case 2: // word = 00010203
			a.aluRI(aluAnd, rCX, 12)
			a.opRMI(false, []byte{0xc7}, 0, rBX, rCX, 1, off)
			a.imm32(0x00010203)
		case 3: // doubleword = 0001020304050607
			a.aluRI(aluAnd, rCX, 8)
			a.opRMI(false, []byte{0xc7}, 0, rBX, rCX, 1, off)
			a.imm32(0x00010203)
			a.opRMI(false, []byte{0xc7}, 0, rBX, rCX, 1, off+4)
			a.imm32(0x04050607)
		}
	case 0x0c0: // a
		b.binop(rT, rA, rB, ssePaddd, 0x66, false)
	case 0x0c8: // ah
		b.binop(rT, rA, rB, ssePaddw, 0x66, false)
	case 0x040: // sf: b - a
		b.binop(rT, rA, rB, ssePsubd, 0x66, true)
	case 0x048: // sfh
		b.binop(rT, rA, rB, ssePsubw, 0x66, true)
	case 0x3c4, 0x3c7: // mpy, mpys
		b.ld(0, rA)
		b.ld(1, rB)
		for _, x := range []int{0, 1} {
			a.pshiftI(shGrpD, shExtL, x, 16)
			a.pshiftI(shGrpD, shExtA, x, 16)
		}
		a.pi(ssePmulld, 0, 1)
		if op == 0x3c7 {
			a.pshiftI(shGrpD, shExtA, 0, 16)
		}
		b.st(rT, 0)
	case 0x3cc: // mpyu
		b.ld(0, rA)
		b.ld(1, rB)
		b.pc(ssePand, 0, splatW(0xffff))
		b.pc(ssePand, 1, splatW(0xffff))
		a.pi(ssePmulld, 0, 1)
		b.st(rT, 0)
	case 0x3c5: // mpyh
		b.ld(0, rA)
		b.ld(1, rB)
		a.pshiftI(shGrpD, shExtR, 0, 16)
		b.pc(ssePand, 1, splatW(0xffff))
		a.pi(ssePmulld, 0, 1)
		a.pshiftI(shGrpD, shExtL, 0, 16)
		b.st(rT, 0)
	case 0x3c6, 0x3ce: // mpyhh, mpyhhu
		ext := shExtA
		if op == 0x3ce {
			ext = shExtR
		}
		b.ld(0, rA)
		b.ld(1, rB)
		a.pshiftI(shGrpD, ext, 0, 16)
		a.pshiftI(shGrpD, ext, 1, 16)
		a.pi(ssePmulld, 0, 1)
		b.st(rT, 0)
	case 0x2b6: // xsbh
		b.ld(0, rA)
		a.pshiftI(shGrpW, shExtL, 0, 8)
		a.pshiftI(shGrpW, shExtA, 0, 8)
		b.st(rT, 0)
	case 0x2ae: // xshw
		b.ld(0, rA)
		a.pshiftI(shGrpD, shExtL, 0, 16)
		a.pshiftI(shGrpD, shExtA, 0, 16)
		b.st(rT, 0)
	case 0x0c1:
		logic(ssePand, false, false)
	case 0x2c1: // andc: a & ~b
		b.ld(0, rA)
		b.ld(1, rB)
		a.pi(ssePandn, 1, 0)
		b.st(rT, 1)
	case 0x041:
		logic(ssePor, false, false)
	case 0x2c9: // orc
		logic(ssePor, true, false)
	case 0x241:
		logic(ssePxor, false, false)
	case 0x0c9: // nand
		logic(ssePand, false, true)
	case 0x049: // nor
		logic(ssePor, false, true)
	case 0x249: // eqv
		logic(ssePxor, false, true)
	case 0x1f0: // orx
		a.movLoad(rAX, rBX, sgpr(rA))
		for k := int32(1); k < 4; k++ {
			a.aluRM(aluOr, rAX, rBX, sgpr(rA)+4*k)
		}
		a.movStore(rBX, sgpr(rT), rAX)
		for k := int32(1); k < 4; k++ {
			a.movStoreImm(rBX, sgpr(rT)+4*k, 0)
		}
	case 0x1b4: // fsm
		a.movLoad(rCX, rBX, sgpr(rA))
		a.aluRI(aluAnd, rCX, 15)
		a.shiftRI(shShl, rCX, 4)
		b.tab(tabFsm)
		a.xRMI(0xf3, sseMovdquLoad, 0, rDX, rCX, 1, 0)
		b.st(rT, 0)
	// shifts and rotates by immediates
	case 0x07f: // shlhi
		shiftImm(shGrpW, shExtL, n7&0x1f, 16)
	case 0x07b: // shli
		shiftImm(shGrpD, shExtL, n7&0x3f, 32)
	case 0x07c: // rothi
		rotImm(shGrpW, n7&15, 16)
	case 0x078: // roti
		rotImm(shGrpD, n7&31, 32)
	case 0x07d: // rothmi
		shiftImm(shGrpW, shExtR, -n7&0x1f, 16)
	case 0x079: // rotmi
		shiftImm(shGrpD, shExtR, -n7&0x3f, 32)
	case 0x07e: // rotmahi
		shiftImm(shGrpW, shExtA, min(-n7&0x1f, 15), 16)
	case 0x07a: // rotmai
		shiftImm(shGrpD, shExtA, min(-n7&0x3f, 31), 32)
	case 0x1ff: // shlqbyi
		shufImm(tabShlqby, n7&0x1f)
	case 0x1fc: // rotqbyi
		shufImm(tabRotqby, n7&15)
	case 0x1fd: // rotqmbyi
		shufImm(tabRotqmby, -n7&0x1f)
	case 0x1df, 0x1dc, 0x1dd, 0x1cf, 0x1cc, 0x1cd: // shlqby rotqby rotqmby and the bi forms
		a.movLoad(rCX, rBX, sgpr(rB))
		if op < 0x1d0 {
			a.shiftRI(shShr, rCX, 3)
		}
		switch op & 3 {
		case 3:
			shufVar(tabShlqby, 0x1f)
		case 0:
			shufVar(tabRotqby, 15)
		default:
			a.unary(3, rCX) // neg
			shufVar(tabRotqmby, 0x1f)
		}
	// word shifts by registers (AVX2's per-lane shifts)
	case 0x05b, 0x058, 0x059, 0x05a: // shl rot rotm rotma
		if !spuJitAVX2 {
			return false, false
		}
		b.ld(0, rA)
		b.ld(1, rB)
		switch op {
		case 0x05b:
			b.pc(ssePand, 1, splatW(0x3f))
			a.vex38(0x47, 2, 0, 1) // vpsllvd (32 and up give 0)
		case 0x058:
			b.pc(ssePand, 1, splatW(31))
			b.ldc(3, splatW(32))
			a.pi(ssePsubd, 3, 1)
			a.vex38(0x47, 2, 0, 1)
			a.vex38(0x45, 4, 0, 3) // vpsrlvd
			a.pi(ssePor, 2, 4)
		default:
			a.pi(ssePxor, 3, 3)
			a.pi(ssePsubd, 3, 1)
			b.pc(ssePand, 3, splatW(0x3f))
			if op == 0x059 {
				a.vex38(0x45, 2, 0, 3)
			} else {
				a.vex38(0x46, 2, 0, 3) // vpsravd (32 and up: all sign)
			}
		}
		b.st(rT, 2)
	// compares
	case 0x3c0:
		b.binop(rT, rA, rB, ssePcmpeqd, 0x66, false)
	case 0x240:
		b.binop(rT, rA, rB, ssePcmpgtd, 0x66, false)
	case 0x2c0:
		unsignedCmp(ssePcmpgtd, 0x80000000)
	case 0x3c8:
		b.binop(rT, rA, rB, ssePcmpeqw, 0x66, false)
	case 0x248:
		b.binop(rT, rA, rB, ssePcmpgtw, 0x66, false)
	case 0x2c8:
		unsignedCmp(ssePcmpgtw, 0x80008000)
	case 0x3d0:
		b.binop(rT, rA, rB, ssePcmpeqb, 0x66, false)
	case 0x250:
		b.binop(rT, rA, rB, ssePcmpgtb, 0x66, false)
	case 0x2d0:
		unsignedCmp(ssePcmpgtb, 0x80808080)
	// branches
	case 0x1a8: // bi
		a.movLoad(rAX, rBX, sgpr(rA))
		branchTo()
		return true, true
	case 0x1a9: // bisl
		a.movLoad(rAX, rBX, sgpr(rA))
		b.setWords(rT, Vec{(pc + 4) & lsMask, 0, 0, 0})
		branchTo()
		return true, true
	case 0x128, 0x129, 0x12a, 0x12b: // biz binz bihz bihnz
		var test func()
		if op >= 0x12a {
			test = func() { a.testMI(rBX, sgpr(rT), 0xffff) }
		} else {
			test = func() { a.aluMI(aluCmp, rBX, sgpr(rT), 0) }
		}
		cc := ccE
		if op&1 != 0 {
			cc = ccNE
		}
		b.cond(pc, test, cc, func() {
			a.movLoad(rAX, rBX, sgpr(rA))
			branchTo()
		})
		return true, true
	// single precision
	case 0x2c4:
		b.binop(rT, rA, rB, sseAddps, 0, false)
	case 0x2c5:
		b.binop(rT, rA, rB, sseSubps, 0, false)
	case 0x2c6:
		b.binop(rT, rA, rB, sseMulps, 0, false)
	case 0x1b8: // frest: 1/a
		b.ld(1, rA)
		b.ldc(0, splatW(0x3f800000))
		a.ps(sseDivps, 0, 1)
		b.st(rT, 0)
	case 0x3d4: // fi
		b.ld(0, rB)
		b.st(rT, 0)
	case 0x3c2, 0x2c2, 0x3ca, 0x2ca: // fceq fcgt fcmeq fcmgt
		b.ld(0, rA)
		b.ld(1, rB)
		if op&8 != 0 {
			b.xc(0, sseAndps, 0, splatW(0x7fffffff))
			b.xc(0, sseAndps, 1, splatW(0x7fffffff))
		}
		if op == 0x3c2 || op == 0x3ca {
			a.cmpps(0, 1, 0)
			b.st(rT, 0)
		} else {
			a.cmpps(1, 0, 1) // b < a
			b.st(rT, 1)
		}
	default:
		return false, false
	}
	return true, false
}
