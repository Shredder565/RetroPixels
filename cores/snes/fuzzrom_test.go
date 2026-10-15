package snes

// Test ROMs made here, for what no freely distributable ROM covers (or
// covers too little): each is a program that sets the console up from random
// tables, then every frame runs blocks of random (but stack-safe) 65C816
// code, pokes the picture unit from tables mixed with the controller bits,
// starts DMA and HDMA transfers, writes the battery RAM, and talks to a
// random SPC700 program that plays random BRR samples through the DSP with
// echo, noise and pitch modulation. The same ROM runs in the web core and
// here (TestCompareFuzzJS); any difference shows up in the picture, the
// sound or the battery save. Variants cover LoROM / HiROM / ExHiROM, NTSC /
// PAL, FastROM, battery RAM sizes and a copier header.

import (
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// ---------------------------------------------------------------------------
// a tiny 65C816 assembler: bytes plus labels

type asm816 struct {
	base   int // CPU address of buf[0]
	buf    []byte
	labels map[string]int
	fixes  []fix816
}

type fix816 struct {
	at    int
	label string
	kind  int // 1: 8-bit relative, 2: 16-bit absolute, 3: 24-bit long, 4: 16-bit relative (BRL)
}

func newAsm816(base int) *asm816 { return &asm816{base: base, labels: map[string]int{}} }

func (a *asm816) pc() int { return a.base + len(a.buf) }
func (a *asm816) label(n string) *asm816 {
	if _, ok := a.labels[n]; ok {
		panic("label twice: " + n)
	}
	a.labels[n] = a.pc()
	return a
}
func (a *asm816) db(b ...int) *asm816 {
	for _, x := range b {
		a.buf = append(a.buf, byte(x))
	}
	return a
}
func (a *asm816) dw(v int) *asm816       { return a.db(v&0xff, v>>8&0xff) }
func (a *asm816) op8(op, v int) *asm816  { return a.db(op, v&0xff) }
func (a *asm816) op16(op, v int) *asm816 { return a.db(op, v&0xff, v>>8&0xff) }
func (a *asm816) op24(op, v int) *asm816 {
	return a.db(op, v&0xff, v>>8&0xff, v>>16&0xff)
}
func (a *asm816) ref(op int, l string, kind int) *asm816 {
	a.db(op)
	a.fixes = append(a.fixes, fix816{len(a.buf), l, kind})
	n := [5]int{0, 1, 2, 3, 2}[kind]
	for i := 0; i < n; i++ {
		a.buf = append(a.buf, 0)
	}
	return a
}
func (a *asm816) br(op int, l string) *asm816 { return a.ref(op, l, 1) }

// relTo: an 8-bit relative offset to l (the instruction ends with it).
func (a *asm816) relTo(l string) *asm816 {
	a.fixes = append(a.fixes, fix816{len(a.buf), l, 1})
	return a.db(0)
}
func (a *asm816) abs(op int, l string) *asm816  { return a.ref(op, l, 2) }
func (a *asm816) long(op int, l string) *asm816 { return a.ref(op, l, 3) }
func (a *asm816) wordOf(l string) *asm816 {
	a.fixes = append(a.fixes, fix816{len(a.buf), l, 2})
	return a.db(0, 0)
}
func (a *asm816) longOf(l string) *asm816 {
	a.fixes = append(a.fixes, fix816{len(a.buf), l, 3})
	return a.db(0, 0, 0)
}

func (a *asm816) resolve() {
	for _, f := range a.fixes {
		t, ok := a.labels[f.label]
		if !ok {
			panic("no label " + f.label)
		}
		switch f.kind {
		case 1:
			d := t - (a.base + f.at + 1)
			if d < -128 || d > 127 {
				panic(fmt.Sprintf("branch to %s too far (%d)", f.label, d))
			}
			a.buf[f.at] = byte(d)
		case 2:
			a.buf[f.at], a.buf[f.at+1] = byte(t), byte(t>>8)
		case 3:
			a.buf[f.at], a.buf[f.at+1], a.buf[f.at+2] = byte(t), byte(t>>8), byte(t>>16)
		case 4:
			d := t - (a.base + f.at + 2)
			a.buf[f.at], a.buf[f.at+1] = byte(d), byte(d>>8)
		}
	}
}

// common instructions (8-bit accumulator unless noted)
func (a *asm816) sep(v int) *asm816       { return a.op8(0xe2, v) }
func (a *asm816) rep(v int) *asm816       { return a.op8(0xc2, v) }
func (a *asm816) lda8(v int) *asm816      { return a.op8(0xa9, v) }
func (a *asm816) lda16(v int) *asm816     { return a.op16(0xa9, v) }
func (a *asm816) ldx16(v int) *asm816     { return a.op16(0xa2, v) }
func (a *asm816) ldy16(v int) *asm816     { return a.op16(0xa0, v) }
func (a *asm816) ldx8(v int) *asm816      { return a.op8(0xa2, v) }
func (a *asm816) sta(addr int) *asm816    { return a.op16(0x8d, addr) }
func (a *asm816) stz(addr int) *asm816    { return a.op16(0x9c, addr) }
func (a *asm816) ldaA(addr int) *asm816   { return a.op16(0xad, addr) }
func (a *asm816) staL(addr int) *asm816   { return a.op24(0x8f, addr) }
func (a *asm816) ldaL(addr int) *asm816   { return a.op24(0xaf, addr) }
func (a *asm816) set(addr, v int) *asm816 { return a.lda8(v).sta(addr) }

// ---------------------------------------------------------------------------
// ROM layout

type fuzzSpec struct {
	name    string
	mapping int
	pal     bool
	fast    bool
	sramK   int // header SRAM size byte (0 none)
	copier  bool
	seed    int64
	chip    int              // header chip byte (0: from sramK)
	title   string           // header title (default "RP FUZZ")
	extra   func(f *fuzzGen) // chip-specific additions
	romSize int
	mapMode int // header map mode byte (0: from mapping and fast)
}

// fuzzGen builds one ROM.
type fuzzGen struct {
	spec      fuzzSpec
	rng       *rand.Rand
	rom       []byte
	code      *asm816 // bank 0 code at $8000
	sram      [2]int  // CPU address range of battery RAM (0 if none)
	data      map[string]int
	extraNMI  func(a *asm816)
	extraInit func(a *asm816) // after the sound program is running
	extraIRQ  func(a *asm816)
	extraCode func(a *asm816) // more code (a second CPU's program)
	nmitimen  int
	ctx       *blockCtx
	snesCtx   blockCtx
	sramAt    int               // battery RAM CPU address when not the usual one
	romPost   func()            // changes to the ROM before the header is made
	firmware  map[string][]byte // chip programs to hand the cores
	appendFW  []byte            // a chip program appended to the ROM
	maxFrames int               // (for ROMs both cores refuse)
}

// cpuToROM maps a CPU address in banks $00/$80 $8000-$FFFF or $C0-$FF to the ROM offset.
func (f *fuzzGen) romOff(cpu int) int {
	bank, off := cpu>>16&0xff, cpu&0xffff
	switch f.spec.mapping {
	case mapLo:
		return (bank&0x7f)<<15 | off&0x7fff
	case mapHi:
		return (bank&0x3f)<<16 | off
	}
	if bank >= 0xc0 {
		return (bank&0x3f)<<16 | off
	}
	return 0x400000 | (bank&0x3f)<<16 | off
}

// dataBank: a bank (with its CPU address of offset 0) where random data lives.
func (f *fuzzGen) dataAddr(i int) int {
	switch f.spec.mapping {
	case mapLo:
		return (0x01+i)<<16 | 0x8000 // banks $01-$0f, upper half
	}
	return (0xc1 + i) << 16 // HiROM: banks $C1-
}

func (f *fuzzGen) put(cpu int, b []byte) { copy(f.rom[f.romOff(cpu):], b) }

func (f *fuzzGen) randBytes(n int) []byte {
	b := make([]byte, n)
	f.rng.Read(b)
	return b
}

func buildFuzzSNES(spec fuzzSpec) []byte { rom, _ := buildFuzz(spec); return rom }

func buildFuzz(spec fuzzSpec) ([]byte, *fuzzGen) {
	f := &fuzzGen{spec: spec, rng: rand.New(rand.NewSource(spec.seed)), data: map[string]int{}}
	f.snesCtx = blockCtx{
		dps:       []int{0x0000, 0x0100, 0x0200, 0x0800, 0x1000, 0x1c00, 0x2100, 0x4300, 0x0080},
		dbs:       []int{0x7e, 0x7f, 0x00, 0x7e, 0x80},
		longBanks: []byte{0x7e, 0x7f, 0x00, 0x80},
		mvBanks:   []byte{0x7e, 0x7f},
		target:    f.snesTarget,
	}
	f.ctx = &f.snesCtx
	if spec.extra != nil {
		spec.extra(f)
	}
	size := spec.romSize
	if size == 0 {
		size = 0x100000
		if spec.mapping == mapExHi {
			size = 0x500000
		}
	}
	f.rom = make([]byte, size)
	f.rng.Read(f.rom) // random data everywhere (graphics, tables, HDMA lists)
	if spec.sramK != 0 {
		if f.sramAt != 0 {
			f.sram = [2]int{f.sramAt, f.sramAt + min(0x8000, 0x400<<spec.sramK)}
		} else if spec.mapping == mapLo {
			f.sram = [2]int{0x700000, 0x700000 + min(0x8000, 0x400<<spec.sramK)}
		} else {
			f.sram = [2]int{0x306000, 0x306000 + min(0x2000, 0x400<<spec.sramK)}
		}
	}
	a := newAsm816(0x8000)
	f.code = a
	f.program(a)
	a.resolve()
	if len(a.buf) > 0x7fb0 {
		panic(fmt.Sprintf("program too big: %d bytes", len(a.buf)))
	}
	f.put(0x008000, a.buf)
	if f.romPost != nil {
		f.romPost()
	}
	f.header(a)
	f.rom = append(f.rom, f.appendFW...)
	if spec.copier {
		f.rom = append(make([]byte, 0x200), f.rom...)
	}
	return f.rom, f
}

func (f *fuzzGen) header(a *asm816) {
	h := f.romOff(0x00ffc0)
	r := f.rom
	t := f.spec.title
	if t == "" {
		t = "RP FUZZ"
	}
	title := []byte(fmt.Sprintf("%-21s", t))
	copy(r[h:], title[:21])
	mode := [3]int{0x20, 0x21, 0x25}[f.spec.mapping]
	if f.spec.fast {
		mode |= 0x10
	}
	if f.spec.mapMode != 0 {
		mode = f.spec.mapMode
	}
	r[h+0x15] = byte(mode)
	chip := f.spec.chip
	if chip == 0 && f.spec.sramK != 0 {
		chip = 0x02
	}
	r[h+0x16] = byte(chip)
	k := 0
	for 0x400<<k < len(r) {
		k++
	}
	r[h+0x17] = byte(k)
	r[h+0x18] = byte(f.spec.sramK)
	r[h+0x19] = 1
	if f.spec.pal {
		r[h+0x19] = 2
	}
	r[h+0x1a], r[h+0x1b] = 0x33, 0
	vec := func(at int, l string) {
		v := a.labels[l]
		r[f.romOff(0x00ff00|at)], r[f.romOff(0x00ff00|at)+1] = byte(v), byte(v>>8)
	}
	// native: COP E4, BRK E6, ABORT E8, NMI EA, IRQ EE; emulation: COP F4, NMI FA, RESET FC, IRQ/BRK FE
	vec(0xe4, "rti")
	vec(0xe6, "rti")
	vec(0xe8, "rti")
	vec(0xea, "nmi")
	vec(0xee, "irq")
	vec(0xf4, "rti")
	vec(0xfa, "rti")
	vec(0xfc, "reset")
	vec(0xfe, "rti")
	sum := 0
	r[h+0x1c], r[h+0x1d], r[h+0x1e], r[h+0x1f] = 0xff, 0xff, 0, 0
	for _, b := range r {
		sum += int(b)
	}
	sum &= 0xffff
	r[h+0x1e], r[h+0x1f] = byte(sum), byte(sum>>8)
	r[h+0x1c], r[h+0x1d] = byte(^sum), byte(^sum>>8)
}

// WRAM variables (bank 0)
const (
	vFrame = 0x1e00 // 16-bit frame counter (out of the random code's reach)
	vPad   = 0x1e02 // 16-bit pad 1
	vPad2  = 0x1e04
	vTmp   = 0x1e06
	vSram  = 0x1e08 // 16-bit battery RAM write pointer
	vApu   = 0x1e0a
)

func (f *fuzzGen) program(a *asm816) {
	rng := f.rng
	a.label("rti").db(0x40)
	// ---- start-up ----
	a.label("reset")
	a.db(0x78, 0x18, 0xfb)             // SEI CLC XCE
	a.rep(0x30).ldx16(0x1fff).db(0x9a) // TXS
	a.lda16(0).db(0x5b)                // TCD
	a.sep(0x20).lda8(0).db(0x48, 0xab) // PHA PLB
	a.stz(0x4200)
	a.set(0x2100, 0x80)
	a.set(0x420d, b2i(f.spec.fast))
	a.rep(0x20).stz(vFrame).stz(vSram).sep(0x20)
	// fill VRAM, CGRAM, OAM from random ROM data
	f.dmaFill(a, 0x80, 0x18, 1, f.dataAddr(0), 0) // VRAM (64 KB in one go: count 0)
	a.stz(0x2121)
	f.dmaFill(a, 0x00, 0x22, 0, f.dataAddr(1), 0x200)
	a.stz(0x2102).stz(0x2103)
	f.dmaFill(a, 0x00, 0x04, 0, f.dataAddr(1)+0x200, 0x220)
	// upload the sound program
	f.spcUpload(a)
	if f.extraInit != nil {
		f.extraInit(a)
	}
	// initial picture setup
	for r := 0x01; r <= 0x33; r++ {
		if r == 0x04 || r == 0x18 || r == 0x19 || r == 0x22 || (r >= 0x0d && r <= 0x14) || (r >= 0x1b && r <= 0x20) {
			continue
		}
		a.set(0x2100+r, rng.Intn(256))
	}
	a.set(0x2105, rng.Intn(8)|rng.Intn(16)<<4)
	a.set(0x212c, 0x1f)
	// timer IRQ position
	a.rep(0x20).lda16(rng.Intn(340)).sta(0x4207).lda16(rng.Intn(262)).sta(0x4209).sep(0x20)
	f.nmitimen = 0x81 | []int{0, 0x10, 0x20, 0x30}[rng.Intn(4)]
	a.set(0x4200, f.nmitimen)
	a.db(0x58) // CLI
	a.set(0x2100, 0x0f)
	// ---- main loop: random code blocks ----
	a.label("main")
	a.rep(0x30).ldaA(vFrame).op16(0x29, 0x1e).db(0xaa) // AND #$1e TAX
	a.sep(0x30)
	a.abs(0xfc, "blocks") // JSR (blocks,X)
	if rng.Intn(2) == 0 {
		a.db(0xcb) // WAI
	}
	a.abs(0x4c, "main")
	// ---- NMI: a new frame ----
	f.nmi(a)
	// ---- IRQ ----
	a.label("irq")
	a.db(0x08, 0x0b, 0x8b)           // PHP PHD PHB
	a.rep(0x30).db(0x48, 0xda, 0x5a) // PHA PHX PHY
	a.sep(0x20).lda8(0).db(0x48, 0xab)
	a.ldaA(0x4211)
	if f.extraIRQ != nil {
		f.extraIRQ(a)
	}
	for i := 0; i < 1+rng.Intn(4); i++ {
		f.randomPPUWrite(a)
	}
	a.rep(0x30).db(0x7a, 0xfa, 0x68) // PLY PLX PLA
	a.db(0xab, 0x2b, 0x28, 0x40)     // PLB PLD PLP RTI
	// ---- the blocks ----
	n := 16
	names := make([]string, n)
	for i := range names {
		names[i] = fmt.Sprintf("block%d", i)
		f.block(a, names[i])
	}
	a.label("blocks")
	for _, nm := range names {
		a.wordOf(nm)
	}
	if f.extraCode != nil {
		f.extraCode(a)
	}
}

// dmaFill: one DMA on channel 7 from ROM src to B-bus register bbad.
func (f *fuzzGen) dmaFill(a *asm816, vmain, bbad, mode, src, count int) {
	a.set(0x2115, vmain)
	a.stz(0x2116).stz(0x2117)
	a.set(0x4370, mode).set(0x4371, bbad)
	a.rep(0x20).lda16(src & 0xffff).sta(0x4372).lda16(count).sta(0x4375).sep(0x20)
	a.set(0x4374, src>>16)
	a.set(0x420b, 0x80)
}

// randomPPUWrite: one write of a random value (mixed with the pad) to a picture register.
func (f *fuzzGen) randomPPUWrite(a *asm816) {
	rng := f.rng
	regs := []int{0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
		0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
		0x30, 0x31, 0x32, 0x33, 0x21, 0x22, 0x02, 0x03, 0x04, 0x15, 0x16, 0x17, 0x18, 0x19}
	r := regs[rng.Intn(len(regs))]
	switch rng.Intn(3) {
	case 0:
		a.lda8(rng.Intn(256))
	case 1:
		a.ldaA(vPad).op8(0x49, rng.Intn(256)) // pad EOR #
	default:
		a.ldaA(vFrame).op8(0x69, rng.Intn(256)) // frame ADC #
	}
	a.sta(0x2100 + r)
}

func (f *fuzzGen) nmi(a *asm816) {
	rng := f.rng
	a.label("nmi")
	// back to a known state every frame, whatever the random code did
	a.rep(0x30).ldx16(0x1fff).db(0x9a).lda16(0).db(0x5b)
	a.sep(0x30).lda8(0).db(0x48, 0xab)
	a.ldaA(0x4210)
	a.rep(0x20).op16(0xee, vFrame).sep(0x20) // INC frame (16-bit)
	// wait for the automatic joypad read, then keep the pads
	a.label("joywait").ldaA(0x4212).op8(0x29, 1).br(0xd0, "joywait")
	a.rep(0x20).ldaA(0x4218).sta(vPad).ldaA(0x421a).sta(vPad2).sep(0x20)
	// also read the pads by hand now and then
	a.ldaA(vFrame).op8(0x29, 7).br(0xd0, "noserial")
	a.set(0x4016, 1).stz(0x4016)
	a.ldx8(16).label("serial").ldaA(0x4016).ldaA(0x4017).db(0xca).br(0xd0, "serial")
	a.label("noserial")
	// picture registers from tables, mixed with the frame counter and pad
	for i := 0; i < 6+rng.Intn(10); i++ {
		f.randomPPUWrite(a)
	}
	// pad scrolls BG1 and BG2 (so the inputs show)
	a.ldaA(vPad).sta(0x210d).stz(0x210d).ldaA(vPad + 1).sta(0x2110).stz(0x2110)
	// a DMA upload chosen by the frame number
	a.ldaA(vFrame).op8(0x29, 3).db(0xaa) // AND #3 TAX
	a.lda8(rng.Intn(256)).sta(0x2115)
	a.ldaA(vFrame).sta(0x2116).ldaA(vPad).sta(0x2117)
	a.set(0x4300, 0x01).set(0x4301, 0x18)
	a.rep(0x20).ldaA(vFrame).op16(0x29, 0x3ff0).op16(0x09, 0x8000).sta(0x4302).lda16(0x100 + rng.Intn(0x700)).sta(0x4305).sep(0x20)
	a.set(0x4304, f.dataAddr(2)>>16)
	a.set(0x420b, 0x01)
	// CGRAM / OAM refresh sometimes
	a.ldaA(vFrame).op8(0x29, 0x0f).br(0xd0, "nocg")
	a.ldaA(vPad).sta(0x2121)
	a.set(0x4310, 0x02).set(0x4311, 0x22)
	a.rep(0x20).ldaA(vFrame).op16(0x09, 0x8000).sta(0x4312).lda16(0x40).sta(0x4315).sep(0x20)
	a.set(0x4314, f.dataAddr(1)>>16)
	a.set(0x420b, 0x02)
	a.label("nocg")
	// HDMA on a few channels, tables in ROM
	for ch := 2; ch < 8; ch++ {
		o := 0x4300 + ch*16
		mode := rng.Intn(8)
		if rng.Intn(3) == 0 {
			mode |= 0x40 // indirect
		}
		hregs := []int{0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x1b, 0x1c, 0x1d, 0x1e, 0x21, 0x22, 0x26, 0x27, 0x28, 0x29, 0x2c, 0x2d, 0x31, 0x32, 0x00, 0x05, 0x06, 0x07, 0x2a, 0x30}
		a.set(o, mode).set(o+1, hregs[rng.Intn(len(hregs))])
		tbl := f.dataAddr(3) + rng.Intn(0x7000)
		// make the table's line counts sane (no zero terminator too soon) in the ROM data itself
		t := f.romOff(tbl)
		for k := 0; k < 64; {
			n := 1 + rng.Intn(40)
			if rng.Intn(4) == 0 {
				n |= 0x80
			}
			f.rom[t+k] = byte(n)
			step := []int{1, 2, 2, 4, 4, 4, 2, 4}[mode&7]
			if mode&0x40 != 0 {
				step = 2
			}
			k += 1 + step
		}
		a.rep(0x20).lda16(tbl & 0xffff).sta(o + 2).sep(0x20)
		a.set(o+4, tbl>>16)
		a.set(o+7, f.dataAddr(4)>>16) // indirect data bank
	}
	a.ldaA(vFrame).op8(0x49, rng.Intn(256)).op8(0x29, 0xfc).sta(0x420c)
	// battery RAM: keep a log of the pads
	if f.sram[1] > f.sram[0] {
		size := f.sram[1] - f.sram[0]
		a.rep(0x30).ldaA(vSram).op16(0x29, size-2).db(0xaa) // AND TAX
		a.ldaA(vPad).op24(0x9f, f.sram[0])                  // STA long,X
		a.ldaA(vFrame).op24(0x9f, f.sram[0]+0)              // (overwrites: frame then pad below)
		a.ldaA(vPad).op16(0x4d, vFrame)                     // EOR frame
		a.op24(0x9f, f.sram[0])
		a.ldaA(vSram).db(0x1a, 0x1a).sta(vSram) // INC INC
		a.sep(0x30)
	}
	// the sound program reads a port; tell it the pad and frame
	a.ldaA(vPad).sta(0x2141).ldaA(vFrame).sta(0x2142)
	if f.extraNMI != nil {
		f.extraNMI(a)
	}
	a.set(0x4200, f.nmitimen)
	a.set(0x2100, 0x0f)
	if rng.Intn(4) == 0 {
		a.set(0x2133, rng.Intn(256)) // interlace, overscan, pseudo-hires...
	}
	a.db(0x58)          // CLI
	a.abs(0x4c, "main") // the interrupted code is abandoned
}

// ---------------------------------------------------------------------------
// random code blocks

func (f *fuzzGen) block(a *asm816, name string) {
	rng := f.rng
	a.label(name)
	a.db(0x08, 0x8b, 0x0b) // PHP PHB PHD
	a.sep(0x10)            // 8-bit index registers throughout
	dps, dbs := f.ctx.dps, f.ctx.dbs
	a.rep(0x20).lda16(dps[rng.Intn(len(dps))]).db(0x5b).sep(0x20) // TCD
	a.lda8(dbs[rng.Intn(len(dbs))]).db(0x48, 0xab)
	n := 20 + rng.Intn(60)
	var insns [][]byte
	for i := 0; i < n; i++ {
		insns = append(insns, f.randInsn())
	}
	// forward branches over a few instructions
	ends := map[int][]string{}
	for i := 0; i < len(insns); i++ {
		if rng.Intn(10) == 0 {
			k := min(len(insns), i+1+rng.Intn(4)) - 1
			l := fmt.Sprintf("%s_%d", name, i)
			ends[k] = append(ends[k], l)
			ops := []int{0x10, 0x30, 0x50, 0x70, 0x90, 0xb0, 0xd0, 0xf0, 0x80}
			a.db(ops[rng.Intn(len(ops))]).relTo(l)
		}
		a.db(intsOf(insns[i])...)
		for _, l := range ends[i] {
			a.label(l)
		}
	}
	if f.ctx.extra != nil {
		a.sep(0x20) // (8-bit accumulator for the additions)
		f.ctx.extra(a)
	}
	a.db(0x2b, 0xab, 0x28, 0x60) // PLD PLB PLP RTS
}

func intsOf(b []byte) []int {
	o := make([]int, len(b))
	for i, x := range b {
		o[i] = int(x)
	}
	return o
}

// blockCtx: what random code may touch, for the CPU it runs on.
type blockCtx struct {
	dps, dbs           []int
	longBanks, mvBanks []byte
	target             func() int      // an absolute address to read or write
	idxTarget          func() int      // the same for indexed addressing (nil: target)
	extra              func(a *asm816) // added at the end of each block
}

func (f *fuzzGen) absTarget() int { return f.ctx.target() }

// safe write targets for absolute addressing (index registers are 8-bit)
func (f *fuzzGen) snesTarget() int {
	rng := f.rng
	switch rng.Intn(8) {
	case 0, 1:
		return 0x0100 + rng.Intn(0x1c00) // WRAM (bank 0 / 7E / 7F by DB)
	case 2:
		return 0x2100 + rng.Intn(0x34) // PPU
	case 3:
		return 0x2140 + rng.Intn(4)
	case 4:
		return 0x2180 + rng.Intn(4)
	case 5:
		return 0x4202 + rng.Intn(0x0c) // math, DMA start, HDMA enable, memsel...
	case 6:
		return 0x4300 + rng.Intn(0x80)
	}
	return 0x4016
}

// randInsn: one random instruction that doesn't touch the stack pointer or jump away.
func (f *fuzzGen) randInsn() []byte {
	rng := f.rng
	r8 := func() byte { return byte(rng.Intn(256)) }
	abs := func(op int) []byte { t := f.absTarget(); return []byte{byte(op), byte(t), byte(t >> 8)} }
	for {
		op := rng.Intn(256)
		switch op {
		// excluded: stack pointer, jumps, returns, interrupts, mode switch, stop
		case 0x00, 0x02, 0x40, 0x60, 0x6b, 0x4c, 0x5c, 0x6c, 0x7c, 0xdc, 0x20, 0x22, 0xfc,
			0x1b, 0x9a, 0xfb, 0xdb, 0xcb, 0x42,
			0x48, 0x68, 0xda, 0xfa, 0x5a, 0x7a, 0x08, 0x28, 0x8b, 0xab, 0x0b, 0x2b, 0x4b, 0xf4, 0xd4, 0x62,
			0x10, 0x30, 0x50, 0x70, 0x90, 0xb0, 0xd0, 0xf0, 0x80, 0x82:
			continue
		case 0xc2: // REP, never 16-bit index
			return []byte{0xc2, r8() &^ 0x10}
		case 0xe2:
			return []byte{0xe2, r8()}
		case 0x54, 0x44: // block move of a few bytes within WRAM
			banks := f.ctx.mvBanks
			return []byte{0xc2, 0x20, 0xa9, byte(rng.Intn(40)), 0, byte(op), banks[rng.Intn(2)], banks[rng.Intn(2)]}
		}
		// immediate operands: 8-bit with M/X set... the width depends on the
		// flags at run time, so immediates are always followed by a harmless
		// byte pair that decodes the same either way (a NOP-like LDA #)
		size := insnSize(op)
		if size < 0 {
			continue
		}
		mode := opMode(op)
		switch mode {
		case "imm":
			// 16-bit immediate if M (or X) is clear; pad so both decode: op lo hi + EA EA
			return []byte{byte(op), r8(), 0xea, 0xea}
		case "abs":
			return abs(op)
		case "absx", "absy":
			if f.ctx.idxTarget != nil {
				t := f.ctx.idxTarget()
				return []byte{byte(op), byte(t), byte(t >> 8)}
			}
			return abs(op)
		case "long", "longx":
			var t int
			if mode == "longx" && f.ctx.idxTarget != nil {
				t = f.ctx.idxTarget()
			} else {
				t = f.absTarget()
			}
			banks := f.ctx.longBanks
			if isWrite(op) {
				return []byte{byte(op), byte(t), byte(t >> 8), banks[rng.Intn(len(banks))]}
			}
			return []byte{byte(op), r8(), r8(), r8()}
		case "dp", "dpx", "dpy", "sr":
			if (mode == "sr") && isWrite(op) {
				continue // stack-relative writes could hit return addresses
			}
			if mode == "sr" {
				return []byte{byte(op), byte(1 + rng.Intn(6))}
			}
			return []byte{byte(op), r8()}
		case "ind", "indx", "indy", "indl", "indly", "srindy":
			if isWrite(op) {
				continue // pointers are random
			}
			if mode == "srindy" {
				return []byte{byte(op), byte(1 + rng.Intn(6))}
			}
			return []byte{byte(op), r8()}
		case "impl":
			return []byte{byte(op)}
		}
	}
}

// 65C816 opcode table: addressing mode by opcode (immediates are handled as one kind).
var opModes = func() [256]string {
	var m [256]string
	g1 := map[int]string{0x01: "indx", 0x03: "sr", 0x05: "dp", 0x07: "indl", 0x09: "imm", 0x0d: "abs", 0x0f: "long",
		0x11: "indy", 0x12: "ind", 0x13: "srindy", 0x15: "dpx", 0x17: "indly", 0x19: "absy", 0x1d: "absx", 0x1f: "longx"}
	for op := 0; op < 256; op++ {
		if md, ok := g1[op&0x1f]; ok {
			m[op] = md
		}
	}
	set := func(md string, ops ...int) {
		for _, o := range ops {
			m[o] = md
		}
	}
	set("dp", 0x06, 0x26, 0x46, 0x66, 0xc6, 0xe6, 0x04, 0x14, 0x24, 0x64, 0x84, 0x86, 0xa4, 0xa6, 0xc4, 0xe4)
	set("abs", 0x0e, 0x2e, 0x4e, 0x6e, 0xce, 0xee, 0x0c, 0x1c, 0x2c, 0x9c, 0x8c, 0x8e, 0xac, 0xae, 0xcc, 0xec)
	set("dpx", 0x16, 0x36, 0x56, 0x76, 0xd6, 0xf6, 0x34, 0x74, 0x94, 0xb4)
	set("dpy", 0x96, 0xb6)
	set("absx", 0x1e, 0x3e, 0x5e, 0x7e, 0xde, 0xfe, 0x3c, 0x9e, 0xbc)
	set("absy", 0xbe)
	set("imm", 0x89, 0xa0, 0xa2, 0xc0, 0xe0)
	set("impl", 0x0a, 0x2a, 0x4a, 0x6a, 0x3a, 0x1a, 0xe8, 0xc8, 0xca, 0x88, 0xaa, 0xa8, 0x8a, 0x98, 0xba, 0x9b, 0xbb,
		0x7b, 0x3b, 0xeb, 0x18, 0x38, 0x58, 0x78, 0xb8, 0xd8, 0xf8, 0xea)
	m[0x89] = "imm"
	return m
}()

func opMode(op int) string { return opModes[op] }

func insnSize(op int) int {
	if opModes[op] == "" {
		return -1
	}
	return 1
}

func isWrite(op int) bool {
	switch op {
	case 0x81, 0x83, 0x85, 0x87, 0x8d, 0x8f, 0x91, 0x92, 0x93, 0x95, 0x97, 0x99, 0x9d, 0x9f, // STA
		0x84, 0x8c, 0x94, 0x86, 0x8e, 0x96, 0x64, 0x74, 0x9c, 0x9e, // STY STX STZ
		0x06, 0x26, 0x46, 0x66, 0xc6, 0xe6, 0x0e, 0x2e, 0x4e, 0x6e, 0xce, 0xee, // RMW
		0x16, 0x36, 0x56, 0x76, 0xd6, 0xf6, 0x1e, 0x3e, 0x5e, 0x7e, 0xde, 0xfe,
		0x04, 0x0c, 0x14, 0x1c:
		return true
	}
	return false
}

// ---------------------------------------------------------------------------
// the sound program

// spcUpload sends the sound program with the boot ROM's protocol (as the web
// core's IPL handler takes it) and starts it.
func (f *fuzzGen) spcUpload(a *asm816) {
	blob := f.spcProgram()
	src := f.dataAddr(5)
	f.put(src, blob)
	a.rep(0x30).lda16(0xbbaa).label("ipl0").op16(0xcd, 0x2140).br(0xd0, "ipl0") // CMP $2140
	a.sep(0x20).ldx16(0x0200).op16(0x8e, 0x2142)                                // STX $2142
	a.set(0x2141, 1).set(0x2140, 0xcc)
	a.label("ipl1").op16(0xcd, 0x2140).br(0xd0, "ipl1")
	a.ldx16(0)
	a.label("ipl2").op24(0xbf, src).sta(0x2141) // LDA long,X
	a.db(0x8a).sta(0x2140)                      // TXA
	a.label("ipl3").op16(0xcd, 0x2140).br(0xd0, "ipl3")
	a.db(0xe8).op16(0xe0, len(blob)).br(0xd0, "ipl2") // INX CPX #len
	a.ldx16(0x0200).op16(0x8e, 0x2142).stz(0x2141)
	a.lda8((len(blob) + 2) & 0xff).sta(0x2140)
	a.label("ipl4").op16(0xcd, 0x2140).br(0xd0, "ipl4")
}

const (
	spcDir = 0x0a00 // sample directory (256 entries)
	spcBRR = 0x0e00 // BRR samples up to $2E00
	spcEnd = 0x2e00
)

// spcProgram: code at $0200, then the sample directory and random BRR data.
func (f *fuzzGen) spcProgram() []byte {
	rng := f.rng
	s := newAsm816(0x0200)
	s.db(0xcd, 0xef, 0xbd, 0x20) // MOV X,#$EF  MOV SP,X  CLRP
	for t := 0; t < 3; t++ {
		s.db(0x8f, rng.Intn(256), 0xfa+t)
	}
	s.db(0x8f, 0x07, 0xf1) // timers on
	dsp := func(r, v int) { s.db(0x8f, v&0xff, 0xf2, 0x8f, v&0xff, 0xf3); s.buf[len(s.buf)-5] = byte(r) }
	dsp(0x6c, 0x20)
	dsp(0x5d, spcDir>>8)
	dsp(0x6d, 0x80)
	dsp(0x7d, rng.Intn(16))
	for _, r := range []int{0x0c, 0x1c, 0x2c, 0x3c} {
		dsp(r, rng.Intn(256))
	}
	for r := 0x0f; r < 0x80; r += 0x10 {
		dsp(r, rng.Intn(64)-32)
	}
	for v := 0; v < 8; v++ {
		for r := 0; r < 8; r++ {
			dsp(v<<4|r, rng.Intn(256))
		}
	}
	dsp(0x0d, rng.Intn(256))
	dsp(0x2d, rng.Intn(256)&0xfe)
	dsp(0x3d, rng.Intn(256)&rng.Intn(256)&rng.Intn(256))
	dsp(0x4d, rng.Intn(256))
	dsp(0x6c, rng.Intn(32)|0x20*rng.Intn(2))
	dsp(0x5c, 0)
	dsp(0x4c, 1+rng.Intn(255))
	s.label("loop")
	var units [][]byte
	nu := 30 + rng.Intn(60)
	for i := 0; i < nu; i++ {
		units = append(units, f.spcUnit())
	}
	ends := map[int][]string{}
	for i, u := range units {
		if rng.Intn(8) == 0 && i+1 < len(units) {
			k := min(len(units), i+1+rng.Intn(3)) - 1
			l := fmt.Sprintf("u%d", i)
			ends[k] = append(ends[k], l)
			switch rng.Intn(4) {
			case 0: // CBNE dp,rel
				s.db(0x2e, rng.Intn(0xf0)).relTo(l)
			case 1: // BBS/BBC dp.bit,rel
				s.db(0x03|rng.Intn(8)<<5|0x10*rng.Intn(2), rng.Intn(0xf0)).relTo(l)
			case 2: // DBNZ dp,rel
				s.db(0x6e, rng.Intn(0xf0)).relTo(l)
			default:
				ops := []int{0x10, 0x30, 0x50, 0x70, 0x90, 0xb0, 0xd0, 0xf0, 0x2f, 0xfe}
				s.db(ops[rng.Intn(len(ops))]).relTo(l)
			}
		}
		s.db(intsOf(u)...)
		for _, l := range ends[i] {
			s.label(l)
		}
	}
	s.abs(0x5f, "loop")           // JMP
	s.label("sub").db(0xbc, 0x6f) // INC A  RET
	s.resolve()
	// the CALLs in the units point at sub
	sub := s.labels["sub"]
	for i := 0; i+2 < len(s.buf); i++ {
		if s.buf[i] == 0x3f && s.buf[i+1] == 0xee && s.buf[i+2] == 0xee {
			s.buf[i+1], s.buf[i+2] = byte(sub), byte(sub>>8)
		}
	}
	if len(s.buf) > spcDir-0x200 {
		panic(fmt.Sprintf("sound program too big: %d", len(s.buf)))
	}
	out := make([]byte, spcEnd-0x200)
	copy(out, s.buf)
	for i := 0; i < 256; i++ {
		start := spcBRR + 9*rng.Intn((spcEnd-spcBRR)/9-4)
		loop := start + 9*rng.Intn(4)
		if rng.Intn(4) == 0 {
			loop = spcBRR + 9*rng.Intn((spcEnd-spcBRR)/9)
		}
		o := spcDir - 0x200 + 4*i
		out[o], out[o+1], out[o+2], out[o+3] = byte(start), byte(start>>8), byte(loop), byte(loop>>8)
	}
	for p := spcBRR; p+9 <= spcEnd; p += 9 {
		o := p - 0x200
		rng.Read(out[o : o+9])
		rangeBits := rng.Intn(13)
		if rng.Intn(10) == 0 {
			rangeBits = 13 + rng.Intn(3)
		}
		flags := 0
		if rng.Intn(6) == 0 {
			flags = 1 | 2*rng.Intn(2)
		}
		out[o] = byte(rangeBits<<4 | rng.Intn(4)<<2 | flags)
	}
	return out
}

// spcUnit: one random piece of the sound program's loop (stack-balanced, no jumps).
func (f *fuzzGen) spcUnit() []byte {
	rng := f.rng
	r8 := func() byte { return byte(rng.Intn(256)) }
	dp := func() byte { return byte(rng.Intn(0xf0)) }
	dpw := func() byte { return byte(rng.Intn(0xef)) }
	dspReg := func() byte {
		for {
			r := rng.Intn(128)
			if r != 0x5d && r != 0x6d && r != 0x7d {
				return byte(r)
			}
		}
	}
	switch k := rng.Intn(20); {
	case k < 5: // DSP register write
		return []byte{0x8f, dspReg(), 0xf2, 0x8f, r8(), 0xf3}
	case k < 7: // DSP register from a port
		return []byte{0x8f, dspReg(), 0xf2, 0xe4, byte(0xf4 + rng.Intn(4)), 0xc4, 0xf3}
	case k < 8: // read a DSP register into the work RAM
		return []byte{0x8f, byte(rng.Intn(128)), 0xf2, 0xe4, 0xf3, 0xc4, dp()}
	case k < 10: // output ports
		srcs := []byte{0xc4, 0xd8, 0xcb}
		return []byte{srcs[rng.Intn(3)], byte(0xf4 + rng.Intn(4))}
	case k < 11: // wait for timer 0
		if rng.Intn(3) == 0 {
			return []byte{0xe4, 0xfd, 0xf0, 0xfc} // MOV A,$FD  BEQ -4
		}
		return []byte{0xe4, byte(0xfd + rng.Intn(3))}
	case k < 12: // key on / off
		if rng.Intn(2) == 0 {
			return []byte{0x8f, 0x4c, 0xf2, 0x8f, r8(), 0xf3}
		}
		return []byte{0x8f, 0x5c, 0xf2, 0x8f, r8() & r8(), 0xf3}
	case k < 13: // push, an operation, pop
		pushes := [][2]byte{{0x2d, 0xae}, {0x4d, 0xce}, {0x6d, 0xee}, {0x0d, 0x8e}}
		p := pushes[rng.Intn(4)]
		mid := f.spcALU(r8, dp, dpw)
		return append(append([]byte{p[0]}, mid...), p[1])
	case k < 14:
		return []byte{0x3f, 0xee, 0xee} // CALL sub (patched)
	}
	return f.spcALU(r8, dp, dpw)
}

func (f *fuzzGen) spcALU(r8, dp, dpw func() byte) []byte {
	rng := f.rng
	pick := func(ops ...int) byte { return byte(ops[rng.Intn(len(ops))]) }
	switch rng.Intn(13) {
	case 0: // A/X/Y, #imm
		return []byte{pick(0x88, 0xa8, 0x28, 0x08, 0x48, 0x68, 0xe8, 0xcd, 0x8d, 0xc8, 0xad), r8()}
	case 1: // reads of the direct page
		return []byte{pick(0x84, 0xa4, 0x24, 0x04, 0x44, 0x64, 0xe4, 0xf8, 0xeb, 0x3e, 0x7e, 0xba, 0x5a, 0x7a, 0x9a, 0xf4, 0xf9, 0xfb, 0x94, 0xb4, 0x74, 0x14, 0x34, 0x54, 0xe7, 0xf7, 0x87, 0x97), byte(rng.Intn(0x100))}
	case 2: // writes to the direct page
		return []byte{pick(0xc4, 0xd8, 0xcb, 0x0b, 0x4b, 0x2b, 0x6b, 0x8b, 0xab), dp()}
	case 3:
		return []byte{pick(0x3a, 0x1a, 0xda), dpw()}
	case 4: // SET1 / CLR1
		return []byte{byte(0x02 | rng.Intn(8)<<5 | 0x10*rng.Intn(2)), dp()}
	case 5: // dp,dp
		return []byte{pick(0x89, 0xfa, 0x29, 0x09, 0x49, 0x69, 0xa9), r8(), dp()}
	case 6: // dp,#imm
		return []byte{pick(0x98, 0x8f, 0x38, 0x18, 0x58, 0x78, 0xb8), r8(), dp()}
	case 7: // absolute reads (any address)
		return []byte{pick(0xe5, 0x85, 0xa5, 0x25, 0x05, 0x45, 0x65, 0xe9, 0xec, 0x1e, 0x5e, 0xf5, 0xf6, 0x95, 0x96, 0x15, 0x16), r8(), r8()}
	case 8: // bit operations: reads anywhere, writes to the work RAM
		if rng.Intn(2) == 0 {
			a := rng.Intn(0x2000) | rng.Intn(8)<<13
			return []byte{pick(0xaa, 0x0a, 0x2a, 0x4a, 0x6a, 0x8a), byte(a), byte(a >> 8)}
		}
		a := rng.Intn(0xf0) | rng.Intn(8)<<13
		return []byte{pick(0xca, 0xea), byte(a), byte(a >> 8)}
	case 9: // TSET1 / TCLR1
		return []byte{pick(0x0e, 0x4e), dp(), 0}
	case 10: // X/Y indexed reads
		return []byte{pick(0xe6, 0xbf, 0x66, 0x86, 0xa6, 0x26, 0x06, 0x46)}
	case 11: // MOV (X),A / (X)+,A only when X is in the work RAM
		return []byte{0xcd, dp(), pick(0xc6, 0xaf), 0xcd, r8()}
	}
	return []byte{pick(0x1c, 0x5c, 0x3c, 0x7c, 0xbc, 0x9c, 0x3d, 0x1d, 0xfc, 0xdc, 0x5d, 0x7d, 0xdd, 0xfd, 0x9f, 0xcf, 0x9e, 0xdf, 0xbe,
		0x60, 0x80, 0xed, 0xe0, 0x20, 0xa0, 0xc0, 0x00, 0x9d, 0x79)}
}

// ---------------------------------------------------------------------------

var fuzzVariants = []fuzzSpec{
	{name: "lorom", mapping: mapLo},
	{name: "lorom-sram8-fast", mapping: mapLo, sramK: 3, fast: true},
	{name: "lorom-sram32-pal", mapping: mapLo, sramK: 5, pal: true},
	{name: "lorom-copier-sram2", mapping: mapLo, sramK: 1, copier: true},
	{name: "hirom", mapping: mapHi},
	{name: "hirom-sram8-fast", mapping: mapHi, sramK: 3, fast: true},
	{name: "hirom-pal-sram2", mapping: mapHi, pal: true, sramK: 1},
	{name: "exhirom-sram8", mapping: mapExHi, sramK: 3, fast: true},
	{name: "exhirom-pal", mapping: mapExHi, pal: true},
	{name: "lorom-3mb", mapping: mapLo, romSize: 0x300000, sramK: 2},
	{name: "hirom-3mb-copier", mapping: mapHi, romSize: 0x300000, copier: true, sramK: 3},
	{name: "sa1", mapping: mapLo, mapMode: 0x23, chip: 0x34, extra: sa1Fuzz},
	{name: "sa1-bwram32", mapping: mapLo, mapMode: 0x23, chip: 0x35, sramK: 5, extra: sa1Fuzz},
	{name: "sa1-bwram128", mapping: mapLo, mapMode: 0x23, chip: 0x35, sramK: 7, extra: sa1Fuzz},
	{name: "gsu1", mapping: mapLo, chip: 0x13, extra: gsuFuzz},
	{name: "gsu2-battery", mapping: mapLo, chip: 0x15, sramK: 5, extra: gsuFuzz},
	{name: "gsu2-2mb-battery64", mapping: mapLo, chip: 0x1a, sramK: 6, romSize: 0x200000, extra: gsuFuzz},
	{name: "gsu2-fast", mapping: mapLo, chip: 0x14, fast: true, extra: gsuFuzz},
	{name: "dsp3", mapping: mapLo, chip: 0x03, title: "SD RP GX FUZZ", extra: necFuzz("DSP-3", "file")},
	{name: "dsp3-append", mapping: mapLo, chip: 0x05, title: "SD RP GX FUZZ", extra: necFuzz("DSP-3", "append")},
	{name: "dsp3-none", mapping: mapLo, chip: 0x03, title: "SD RP GX FUZZ", extra: necFuzz("DSP-3", "none")},
	{name: "dsp4-be", mapping: mapLo, chip: 0x03, title: "TOP GEAR 3000 RP", extra: necFuzz("DSP-4", "be")},
	{name: "st010", mapping: mapLo, chip: 0xf6, sramK: 2, title: "F1 ROC RP FUZZ", extra: necFuzz("ST010", "file")},
	{name: "st011", mapping: mapLo, chip: 0xf6, title: "RP SHOGI FUZZ", extra: necFuzz("ST011", "be")},
	{name: "st011-short", mapping: mapLo, chip: 0xf6, title: "RP SHOGI FUZZ", extra: necFuzz("ST011", "short")},
	{name: "dsp1-hle", mapping: mapLo, chip: 0x03, title: "RP DSP1 FUZZ", extra: necFuzz("DSP-1", "hle")},
	{name: "dsp1-hle-2mb", mapping: mapLo, chip: 0x05, title: "RP DSP1 FUZZ", romSize: 0x200000, sramK: 1, extra: necFuzz("DSP-1", "hle")},
	{name: "dsp1-hle-hirom", mapping: mapHi, chip: 0x05, title: "RP DSP1 FUZZ", sramK: 1, extra: necFuzz("DSP-1", "hle")},
	{name: "dsp1-lle", mapping: mapLo, chip: 0x03, title: "RP DSP1 FUZZ", extra: necFuzz("DSP-1", "file")},
	{name: "dsp2-hle", mapping: mapLo, chip: 0x05, title: "DUNGEON MASTER RP", sramK: 3, extra: necFuzz("DSP-2", "hle")},
	{name: "dsp2-lle", mapping: mapLo, chip: 0x03, title: "DUNGEON MASTER RP", extra: necFuzz("DSP-2", "append")},
}

// TestCompareFuzzJS builds the test ROMs above and compares them like
// TestCompareJS. SNES_FUZZ=<frames>[,<seeds>] turns it on; SNES_FUZZ_DIR
// keeps the ROMs.
func TestCompareFuzzJS(t *testing.T) {
	env := os.Getenv("SNES_FUZZ")
	if env == "" {
		t.Skip("SNES_FUZZ not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	frames, seeds := 3000, 1
	fmt.Sscanf(env, "%d,%d", &frames, &seeds)
	dir := os.Getenv("SNES_FUZZ_DIR")
	if dir == "" {
		dir = t.TempDir()
	}
	for seed := 1; seed <= seeds; seed++ {
		for i, v := range fuzzVariants {
			if only := os.Getenv("SNES_FUZZ_ONLY"); only != "" && !strings.Contains(v.name, only) {
				continue
			}
			v.seed = int64(seed)*1000 + int64(i)
			v.name = fmt.Sprintf("%s-%d", v.name, seed)
			path := filepath.Join(dir, "fuzz-"+v.name+".sfc")
			rom, f := buildFuzz(v)
			if err := os.WriteFile(path, rom, 0o644); err != nil {
				t.Fatal(err)
			}
			e := listEntry{path: path, frames: frames}
			if f.maxFrames != 0 {
				e.frames = min(frames, f.maxFrames)
			}
			if f.firmware != nil {
				e.fw = filepath.Join(dir, "fw-"+v.name)
				os.MkdirAll(e.fw, 0o755)
				for chip, b := range f.firmware {
					if err := os.WriteFile(filepath.Join(e.fw, chip), b, 0o644); err != nil {
						t.Fatal(err)
					}
				}
			}
			t.Run(v.name, func(t *testing.T) {
				t.Parallel()
				compareROM(t, node, e)
			})
		}
	}
}
