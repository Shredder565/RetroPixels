package snes

import (
	"fmt"
	"math/rand"
)

// Cartridge chip additions to the generated test ROMs (fuzzrom_test.go).

// ---------------------------------------------------------------------------
// SA-1: the second CPU runs its own random code (arithmetic, bit reader, DMA,
// character conversion, timer IRQs, BW-RAM and its bitmap view), the SNES CPU
// pokes the SNES-side registers, resets and interrupts it, and shows I-RAM and
// BW-RAM on screen by DMA. The ROM is 2 MB with the code in both halves, so
// every Super MMC setting finds it, while the graphics data differ.

func sa1Fuzz(f *fuzzGen) {
	rng := f.rng
	f.sramAt = 0x400000
	sa1Regs := []int{0x220a, 0x220b, 0x2210, 0x2211, 0x2212, 0x2213, 0x2214, 0x2215, 0x2225, 0x2227,
		0x2230, 0x2231, 0x2232, 0x2233, 0x2234, 0x2238, 0x2239, 0x223f,
		0x2250, 0x2251, 0x2252, 0x2253, 0x2254, 0x2258, 0x2259, 0x225a, 0x225b,
		0x2300, 0x2301, 0x2302, 0x2303, 0x2304, 0x2305, 0x2306, 0x2307, 0x2308, 0x2309, 0x230a, 0x230b, 0x230c, 0x230d, 0x230e}
	sa1Mem := func() int {
		switch rng.Intn(4) {
		case 0:
			return 0x0100 + rng.Intn(0x500) // I-RAM (the stack is at $07xx)
		case 1:
			return 0x3000 + rng.Intn(0x500)
		}
		return 0x6000 + rng.Intn(0x1f00) // BW-RAM window
	}
	sa1Ctx := blockCtx{
		dps:       []int{0x0000, 0x0100, 0x0400, 0x3000, 0x3100, 0x6000, 0x7e00, 0x0080},
		dbs:       []int{0x00, 0x40, 0x60, 0x00, 0x41, 0x80},
		longBanks: []byte{0x40, 0x41, 0x60, 0x61, 0x00},
		mvBanks:   []byte{0x40, 0x41},
		target: func() int {
			if rng.Intn(3) == 0 {
				return sa1Regs[rng.Intn(len(sa1Regs))]
			}
			return sa1Mem()
		},
		idxTarget: sa1Mem,
	}
	sa1Ctx.extra = func(a *asm816) {
		for i := 0; i < 1+rng.Intn(3); i++ {
			sa1Unit(f, a)
		}
	}
	snesRegs := []int{0x2201, 0x2202, 0x2221, 0x2222, 0x2223, 0x2224, 0x2226, 0x2228, 0x2229, 0x2231,
		0x2300, 0x2301, 0x2302, 0x2303, 0x2304, 0x2305, 0x2306, 0x2307, 0x2308, 0x2309, 0x230a, 0x230b, 0x230c, 0x230d, 0x230e}
	base := f.snesCtx.target
	// indexed and direct page accesses must not reach the SA-1's vector and
	// control registers ($2200-$222A)
	f.snesCtx.dps = []int{0x0000, 0x0100, 0x0200, 0x0800, 0x1000, 0x1c00, 0x4300, 0x0080}
	f.snesCtx.idxTarget = func() int {
		switch rng.Intn(5) {
		case 0:
			return 0x3000 + rng.Intn(0x500)
		case 1:
			return 0x6000 + rng.Intn(0x1f00)
		case 2:
			return 0x4300 + rng.Intn(0x80)
		}
		return 0x0100 + rng.Intn(0x1c00)
	}
	f.snesCtx.target = func() int {
		switch rng.Intn(6) {
		case 0:
			return snesRegs[rng.Intn(len(snesRegs))]
		case 1:
			return 0x3000 + rng.Intn(0x500)
		case 2:
			return 0x6000 + rng.Intn(0x2000)
		}
		return base()
	}
	vectors := func(a *asm816) {
		a.rep(0x20)
		a.abs(0xa9, "sa1reset").sta(0x2203)
		a.abs(0xa9, "sa1nmi").sta(0x2205)
		a.abs(0xa9, "sa1irq").sta(0x2207)
		a.abs(0xa9, "nmi").sta(0x220c)
		a.abs(0xa9, "irq").sta(0x220e)
		a.sep(0x20)
		a.set(0x222a, 0xff) // SA-1 I-RAM writable (its stack)
	}
	f.extraInit = func(a *asm816) {
		vectors(a)
		a.set(0x2229, 0xff).set(0x2226, 0x80*rng.Intn(2)).set(0x2228, rng.Intn(16))
		a.set(0x2201, rng.Intn(256)&0xa0)
		a.set(0x2224, rng.Intn(256))
		a.stz(0x2200) // start the SA-1
	}
	f.extraIRQ = func(a *asm816) {
		a.set(0x2202, 0xa0) // acknowledge the SA-1's interrupts
	}
	f.extraNMI = func(a *asm816) {
		vectors(a)
		// now and then restart the SA-1 (it may have crashed itself)
		a.ldaA(vFrame).op8(0x29, 0x3f).br(0xd0, "sa1norst")
		a.set(0x2200, 0x20)
		a.lda8(rng.Intn(256) & 0x90).sta(0x2200)
		a.label("sa1norst")
		// or interrupt it
		a.ldaA(vFrame).op8(0x29, 0x03).op8(0xc9, 1).br(0xd0, "sa1noirq")
		a.set(0x2200, 0x80|rng.Intn(2)<<4)
		a.label("sa1noirq")
		for i := 0; i < 2+rng.Intn(4); i++ {
			r := snesRegs[rng.Intn(10)]
			if rng.Intn(2) == 0 {
				a.lda8(rng.Intn(256))
			} else {
				a.ldaA(vPad).op8(0x49, rng.Intn(256))
			}
			a.sta(r)
		}
		// show BW-RAM (through character conversion when it's on) and I-RAM
		a.set(0x2115, 0x80)
		a.rep(0x20).lda16(rng.Intn(0x8000)).sta(0x2116).sep(0x20)
		a.set(0x4300, 0x01).set(0x4301, 0x18)
		a.rep(0x20).ldaA(vFrame).op16(0x29, 0x1ff0).sta(0x4302).lda16(0x200).sta(0x4305).sep(0x20)
		a.set(0x4304, 0x40)
		a.set(0x420b, 0x01)
		a.rep(0x20).lda16(0x3000).sta(0x4302).lda16(0x100).sta(0x4305).sep(0x20)
		a.set(0x4304, 0x00)
		a.set(0x420b, 0x01)
		// SA-1 results into scroll registers
		a.ldaA(0x2306).sta(0x2111).ldaA(0x2307).sta(0x2111)
		a.ldaA(0x2302).sta(0x2112).ldaA(0x2300).sta(0x2112)
	}
	f.extraCode = func(a *asm816) {
		save := f.ctx
		f.ctx = &sa1Ctx
		a.label("sa1reset")
		a.db(0x78, 0x18, 0xfb)
		a.rep(0x30).ldx16(0x07ff).db(0x9a).lda16(0).db(0x5b)
		a.sep(0x20).lda8(0).db(0x48, 0xab)
		a.set(0x222a, 0xff)
		a.set(0x2227, 0x80*rng.Intn(2))
		a.set(0x2225, rng.Intn(256))
		a.set(0x223f, rng.Intn(256)&0x80)
		a.set(0x220a, rng.Intn(256)&0x70|0x80)
		a.set(0x2210, rng.Intn(256)&0x83)
		a.rep(0x20).lda16(rng.Intn(341)).sta(0x2212).lda16(rng.Intn(262)).sta(0x2214).sep(0x20)
		a.stz(0x2211)
		if rng.Intn(4) != 0 {
			a.db(0x58)
		}
		a.label("sa1main")
		a.rep(0x30).ldaA(0x0700).db(0x1a).sta(0x0700).op16(0x29, 0x1e).db(0xaa) // LDA INC STA AND TAX
		a.sep(0x30)
		a.abs(0xfc, "sa1blocks")
		a.ldaA(0x0700).op8(0x29, 0xff).br(0xd0, "sa1nowai")
		a.db(0xcb) // WAI
		a.label("sa1nowai")
		a.abs(0x4c, "sa1main")
		for _, l := range []string{"sa1nmi", "sa1irq"} {
			a.label(l)
			a.db(0x08, 0x0b, 0x8b)
			a.rep(0x30).db(0x48, 0xda, 0x5a)
			a.sep(0x30).lda8(0).db(0x48, 0xab)
			a.set(0x220b, 0xf0)
			for i := 0; i < 1+rng.Intn(3); i++ {
				sa1Unit(f, a)
			}
			a.rep(0x30).db(0x7a, 0xfa, 0x68)
			a.db(0xab, 0x2b, 0x28, 0x40)
		}
		names := make([]string, 16)
		for i := range names {
			names[i] = "sa1block" + string(rune('a'+i))
			f.block(a, names[i])
		}
		a.label("sa1blocks")
		for _, nm := range names {
			a.wordOf(nm)
		}
		f.ctx = save
	}
	f.romPost = func() {
		half := len(f.rom)
		r := make([]byte, 2*half)
		copy(r, f.rom)
		copy(r[half:], f.rom)
		// different graphics data in the second half
		for _, i := range []int{0, 2} {
			o := half + f.romOff(f.dataAddr(i))
			f.rng.Read(r[o : o+0x8000])
		}
		f.rom = r
	}
}

// sa1Unit: one structured use of an SA-1 feature, from the SA-1 CPU (8-bit A).
func sa1Unit(f *fuzzGen, a *asm816) {
	rng := f.rng
	val := func() {
		switch rng.Intn(3) {
		case 0:
			a.lda8(rng.Intn(256))
		case 1:
			a.ldaA(0x0100 + rng.Intn(0x600))
		default:
			a.ldaA(0x6000 + rng.Intn(0x2000))
		}
	}
	switch rng.Intn(8) {
	case 0: // arithmetic
		a.set(0x2250, rng.Intn(3))
		for r := 0x2251; r <= 0x2254; r++ {
			val()
			a.sta(r)
		}
		for i := 0; i < 1+rng.Intn(5); i++ {
			a.ldaA(0x2306 + rng.Intn(6)).sta(0x0100 + rng.Intn(0x600))
		}
	case 1: // bit reader
		a.set(0x2258, rng.Intn(256)&0x8f)
		bank := []int{0x00, 0x01, 0x20, 0x80, 0xc0, 0xc8, 0xd3}[rng.Intn(7)]
		addr := rng.Intn(0x10000)
		if bank < 0xc0 {
			addr |= 0x8000
		}
		a.set(0x2259, addr&0xff).set(0x225a, addr>>8).set(0x225b, bank)
		for i := 0; i < 1+rng.Intn(6); i++ {
			if rng.Intn(3) == 0 {
				a.set(0x2258, rng.Intn(16))
			}
			a.ldaA(0x230c).sta(0x6000 + rng.Intn(0x2000)).ldaA(0x230d).sta(0x0100 + rng.Intn(0x600))
		}
	case 2: // normal DMA
		src := rng.Intn(3)
		toBw := rng.Intn(2)
		a.set(0x2230, 0x80|src|toBw<<2|rng.Intn(2)<<3)
		s := []int{0x008000 | rng.Intn(0x8000), 0xc00000 | rng.Intn(0x100000)}[rng.Intn(2)]
		if src == 1 {
			s = rng.Intn(0x2000)
		} else if src == 2 {
			s = 0x100 + rng.Intn(0x600)
		}
		a.set(0x2232, s&0xff).set(0x2233, s>>8&0xff).set(0x2234, s>>16)
		n := 1 + rng.Intn(0x200)
		if toBw == 0 {
			n = 1 + rng.Intn(0x100)
		}
		a.set(0x2238, n&0xff).set(0x2239, n>>8)
		d := 0x100 + rng.Intn(0x300)
		if toBw != 0 {
			d = rng.Intn(0x2000)
		}
		a.set(0x2235, d&0xff).set(0x2236, d>>8)
		if toBw != 0 {
			a.set(0x2237, d>>16)
		}
	case 3: // character conversion 2
		a.set(0x2230, 0xa0|rng.Intn(16))
		a.set(0x2231, rng.Intn(256)&0x1f)
		d := 0x100 + rng.Intn(0x500)
		a.set(0x2235, d&0xff).set(0x2236, d>>8)
		for i := 0; i < 16*(1+rng.Intn(3)); i++ {
			if rng.Intn(2) == 0 {
				a.set(0x2240+i&15, rng.Intn(256))
			} else {
				a.ldaA(0x6000 + rng.Intn(0x2000)).sta(0x2240 + i&15)
			}
		}
	case 4: // character conversion 1 (the SNES DMA picks it up)
		a.set(0x2231, rng.Intn(256)&0x1f)
		a.set(0x2230, 0xb0|rng.Intn(16)&0x0b)
		s := rng.Intn(0x1800)
		a.set(0x2232, s&0xff).set(0x2233, s>>8).set(0x2234, 0x40)
		d := 0x100 + rng.Intn(0x500)
		a.set(0x2235, d&0xff).set(0x2236, d>>8)
	case 5: // interrupt the SNES CPU, maybe with the SA-1's vectors
		a.set(0x2209, 0x80|rng.Intn(256)&0x5f)
	case 6: // timer
		a.set(0x2210, rng.Intn(256)&0x83)
		a.set(0x2212, rng.Intn(256)).set(0x2214, rng.Intn(256))
		a.ldaA(0x2302).sta(0x0100 + rng.Intn(0x600)).ldaA(0x2304).sta(0x0100 + rng.Intn(0x600))
	default: // BW-RAM views
		a.set(0x2225, rng.Intn(256))
		a.set(0x223f, rng.Intn(256)&0x80)
		for i := 0; i < 4; i++ {
			val()
			a.sta(0x6000 + rng.Intn(0x2000))
		}
	}
}

// ---------------------------------------------------------------------------
// Super FX: random GSU programs (ALU, multiplies, RAM / ROM buffers, plotting
// in every mode, loops, jumps, the cache) started by the SNES CPU each frame
// with random screen / clock / config registers; while the GSU owns the ROM
// the SNES CPU waits in WRAM. GSU RAM is shown on screen by DMA.

const gsuWait = 0x7f8000 // the SNES CPU's wait loop, copied to WRAM

func gsuFuzz(f *fuzzGen) {
	rng := f.rng
	const nProg = 4
	progBase := 0x018000
	// the GSU programs, in bank $01
	g := newAsm816(progBase & 0xffff)
	for i := 0; i < nProg; i++ {
		gsuProgram(rng, g, fmt.Sprintf("gsuprog%d", i))
	}
	g.resolve()
	if len(g.buf) > 0x7000 {
		panic("GSU programs too big")
	}
	f.extraIRQ = func(a *asm816) {
		a.ldaA(0x3031) // acknowledge the GSU's interrupt
	}
	f.extraNMI = func(a *asm816) {
		// copy the wait loop to WRAM
		a.rep(0x30).lda16(0x1f).abs(0xa2, "gsuwait").ldy16(gsuWait & 0xffff) // LDA #len-1 LDX #src LDY #dst
		a.db(0x54, gsuWait>>16, 0x00)                                        // MVN $7F,$00
		a.sep(0x30).lda8(0).db(0x48, 0xab)                                   // DB = 0 again
		// stop a GSU that is still running, now and then
		a.ldaA(0x3030).op8(0x29, 0x20).br(0xf0, "gsuidle")
		a.ldaA(vFrame).op8(0x29, 3).op8(0xf0, 3).abs(0x4c, "gsuskip") // BEQ +3  JMP
		a.stz(0x3030)
		a.label("gsuidle")
		// set it up
		a.set(0x3034, progBase>>16)
		a.ldaA(vFrame).op8(0x29, 0x3f).sta(0x3038)
		a.ldaA(vPad).op8(0x49, rng.Intn(256)).sta(0x303a)
		a.ldaA(vFrame).op8(0x49, rng.Intn(256)).op8(0x29, 1).sta(0x3039)
		a.lda8(rng.Intn(256) & 0xa0).sta(0x3037)
		a.lda8(rng.Intn(2)).sta(0x3033)
		for i := 0; i < 4+rng.Intn(8); i++ {
			reg := rng.Intn(14)
			a.ldaA(vPad + rng.Intn(4)).sta(0x3000 + reg*2 + rng.Intn(2))
		}
		if rng.Intn(3) == 0 { // preload some cache lines
			for i := 0; i < 16; i++ {
				a.set(0x3100+16*rng.Intn(32)+i, rng.Intn(256))
			}
		}
		// pick a program by the frame number; start it (R15) and wait if it owns the ROM
		a.ldaA(vFrame).op8(0x29, nProg-1).db(0x0a, 0xaa) // AND ASL TAX
		a.rep(0x20).abs(0xbd, "gsuentries").sep(0x20)    // LDA entries,X
		a.sta(0x301e)
		a.db(0xeb) // XBA
		a.stz(0x4200)
		a.rep(0x10).ldx16(500 + rng.Intn(2500))
		a.op24(0x22, gsuWait) // JSL
		a.sep(0x10)
		a.label("gsuskip")
		// GSU RAM to VRAM and the status to the scroll registers
		a.set(0x2115, 0x80)
		a.rep(0x20).lda16(rng.Intn(0x8000)).sta(0x2116).sep(0x20)
		a.set(0x4300, 0x01).set(0x4301, 0x18)
		a.rep(0x20).ldaA(vFrame).op16(0x29, 0x3f00).sta(0x4302).lda16(0x800).sta(0x4305).sep(0x20)
		a.set(0x4304, 0x70+rng.Intn(2))
		a.set(0x420b, 0x01)
		a.ldaA(0x3030).sta(0x2111).ldaA(0x3000 + 2*rng.Intn(16)).sta(0x2111)
		a.ldaA(0x3000 + 2*rng.Intn(16)).sta(0x2112).ldaA(0x3031).sta(0x2112)
	}
	f.extraCode = func(a *asm816) {
		// the wait loop (runs in WRAM bank $7F): A = R15's high byte
		a.label("gsuwait")
		a.sta(0x301f)
		a.label("gsuwl").ldaA(0x3030).op8(0x29, 0x20).br(0xf0, "gsuwd").db(0xca).br(0xd0, "gsuwl")
		a.stz(0x3030)
		a.label("gsuwd").db(0x6b) // RTL
		a.label("gsuentries")
		for i := 0; i < nProg; i++ {
			a.dw(g.labels[fmt.Sprintf("gsuprog%d", i)])
		}
	}
	f.romPost = func() {
		f.put(progBase, g.buf)
		// the extended header's RAM size
		h := f.romOff(0x00ffc0)
		f.rom[h-3] = byte(5 + rng.Intn(3))
	}
}

// gsuProgram: one random Super FX program at label name, ending in STOP.
func gsuProgram(rng *rand.Rand, g *asm816, name string) {
	g.label(name)
	g.db(0x02) // CACHE now and then
	units := 40 + rng.Intn(120)
	ends := map[int][]string{}
	for i := 0; i < units; i++ {
		if rng.Intn(10) == 0 && i+1 < units {
			k := min(units, i+1+rng.Intn(3)) - 1
			l := fmt.Sprintf("%s_b%d", name, i)
			ends[k] = append(ends[k], l)
			g.db(0x05 + rng.Intn(11)).relTo(l).db(0x01) // branch, NOP in the delay slot
		}
		gsuUnit(rng, g, fmt.Sprintf("%s_u%d", name, i), true)
		for _, l := range ends[i] {
			g.label(l)
		}
	}
	g.db(0x00, 0x01) // STOP NOP
}

// gsuReg: a register to write (never R15; in a loop body never R12 / R13 either).
func gsuReg(rng *rand.Rand, top bool) int {
	for {
		r := rng.Intn(15)
		if top || (r != 12 && r != 13) {
			return r
		}
	}
}

func gsuUnit(rng *rand.Rand, g *asm816, name string, top bool) {
	pre := func() { // source / destination prefixes
		if rng.Intn(3) == 0 {
			g.db(0xb0 | rng.Intn(16)) // FROM
		}
		if rng.Intn(3) == 0 {
			g.db(0x10 | gsuReg(rng, top)) // TO
		}
	}
	alt := func() {
		if k := rng.Intn(4); k != 0 {
			g.db(0x3c + k) // ALT1..3
		}
	}
	switch rng.Intn(16) {
	case 0, 1, 2: // ALU
		pre()
		alt()
		ops := []int{0x50, 0x60, 0x70, 0x80, 0xc0}
		op := ops[rng.Intn(len(ops))]
		n := rng.Intn(16)
		if op == 0x70 || op == 0xc0 {
			n = 1 + rng.Intn(15)
		}
		g.db(op | n)
	case 3: // one-register operations
		pre()
		if rng.Intn(3) == 0 {
			g.db(0x3d)
		}
		ops := []int{0x03, 0x04, 0x4d, 0x4f, 0x95, 0x96, 0x97, 0x9e, 0xc0, 0x70, 0x9f}
		g.db(ops[rng.Intn(len(ops))])
	case 4: // IBT / IWT
		if rng.Intn(2) == 0 {
			g.db(0xa0|gsuReg(rng, top), rng.Intn(256))
		} else {
			g.db(0xf0|gsuReg(rng, top), rng.Intn(256), rng.Intn(256))
		}
	case 5: // INC / DEC
		g.db(0xd0 | gsuReg(rng, top) | 0x30*rng.Intn(2))
	case 6: // RAM loads and stores
		switch rng.Intn(5) {
		case 0:
			pre()
			if rng.Intn(2) == 0 {
				g.db(0x3d)
			}
			g.db(0x30 | rng.Intn(12)) // STW / STB (Rn)
		case 1:
			if rng.Intn(3) == 0 {
				g.db(0x10 | gsuReg(rng, top))
			}
			if rng.Intn(2) == 0 {
				g.db(0x3d)
			}
			g.db(0x40 | rng.Intn(12)) // LDW / LDB (Rn)
		case 2:
			pre()
			g.db(0x90) // SBK
		case 3: // LMS / SMS
			if rng.Intn(2) == 0 {
				g.db(0x3d, 0xa0|gsuReg(rng, top), rng.Intn(256))
			} else {
				g.db(0x3e, 0xa0|rng.Intn(16), rng.Intn(256))
			}
		default: // LM / SM
			if rng.Intn(2) == 0 {
				g.db(0x3d, 0xf0|gsuReg(rng, top), rng.Intn(256), rng.Intn(256))
			} else {
				g.db(0x3e, 0xf0|rng.Intn(16), rng.Intn(256), rng.Intn(256))
			}
		}
	case 7: // ROM buffer
		if rng.Intn(3) == 0 { // ROMB from R0
			g.db(0xa0, rng.Intn(0x20), 0x3f, 0xdf)
		}
		if rng.Intn(4) == 0 { // RAMB
			g.db(0xa0, rng.Intn(2), 0x3e, 0xdf)
		}
		g.db(0xfe, rng.Intn(256), rng.Intn(256)) // IWT R14
		for i := 0; i < 1+rng.Intn(3); i++ {
			if rng.Intn(3) == 0 {
				g.db(0xdf) // GETC
				continue
			}
			if rng.Intn(2) == 0 {
				g.db(0x10 | gsuReg(rng, top))
			}
			alt()
			g.db(0xef) // GETB...
		}
	case 8, 9: // plotting
		g.db(0xa0|rng.Intn(1), rng.Intn(256)) // IBT R0
		if rng.Intn(3) == 0 {
			g.db(0x3d, 0x4e) // CMODE (R0)
			g.db(0xa0, rng.Intn(256))
		}
		g.db(0x4e) // COLOR
		g.db(0xa1, rng.Intn(256), 0xa2, rng.Intn(256))
		for i := 0; i < 1+rng.Intn(12); i++ {
			if rng.Intn(6) == 0 {
				g.db(0x10|gsuReg(rng, top), 0x3d, 0x4c) // RPIX
			} else {
				g.db(0x4c) // PLOT
			}
			if rng.Intn(4) == 0 {
				g.db(0xd2) // INC R2
			}
		}
	case 10: // MOVE / MOVES
		g.db(0x20 | rng.Intn(16))
		if rng.Intn(2) == 0 {
			g.db(0x10 | gsuReg(rng, top))
		} else {
			g.db(0xb0 | rng.Intn(16))
		}
		if g.buf[len(g.buf)-2] == 0x2f && g.buf[len(g.buf)-1]&0xf0 == 0xb0 { // MOVES into R15
			g.buf[len(g.buf)-2] = 0x20
		}
	case 11: // LINK
		g.db(0x91 + rng.Intn(4))
	case 12: // a loop
		if !top {
			g.db(0x01)
			return
		}
		g.db(0xfc, 1+rng.Intn(40), 0).abs(0xfd, name+"_loop") // IWT R12,#n  IWT R13,#loop
		g.label(name + "_loop")
		for i := 0; i < 1+rng.Intn(4); i++ {
			gsuUnit(rng, g, fmt.Sprintf("%s_%d", name, i), false)
		}
		g.db(0x3c, 0x01) // LOOP NOP
	case 13: // a forward jump
		g.abs(0xf9, name+"_j").db(0x99, 0x01) // IWT R9,#l  JMP R9  NOP
		gsuUnit(rng, g, name+"_x", false)
		g.label(name + "_j")
	case 14:
		g.db(0x02) // CACHE
	default: // prefixes left hanging, NOP
		pre()
		g.db(0x01)
	}
}

// ---------------------------------------------------------------------------
// NEC uPD77C25 / uPD96050 (DSP-3, DSP-4, ST010, ST011, and DSP-1 / DSP-2 with
// a supplied program): random firmware (RetroPixels' own, made here) and an
// SNES side that reads and writes DR, SR and the ST01x data RAM every frame,
// showing what it reads in the palette and scroll registers.

// necRandomFirmware: a random program and data ROM in the dump layout.
func necRandomFirmware(rng *rand.Rand, m *necModel, bigEndian bool) []byte {
	var codes []int
	for c := 0x080; c <= 0x0be; c += 2 {
		codes = append(codes, c)
	}
	codes = append(codes, 0x0b1, 0x0b3, 0x100, 0x140)
	prog := make([]int, m.prog)
	for i := range prog {
		switch k := rng.Intn(100); {
		case k < 55: // OP
			prog[i] = rng.Intn(1 << 22)
		case k < 65: // RT
			prog[i] = 1<<22 | rng.Intn(1<<22)
		case k < 80: // LD
			prog[i] = 3<<22 | rng.Intn(1<<22)
		default: // JP
			c := codes[rng.Intn(len(codes))]
			t := rng.Intn(m.prog)
			if k < 85 { // a nearby target
				t = (i + rng.Intn(64) - 16) & (m.prog - 1)
			}
			if k < 87 { // wait for the SNES
				c, t = 0x0bc+2*rng.Intn(2), i
			}
			w := 2<<22 | c<<13
			if m.big {
				w |= (t&0x7ff)<<2 | (t>>11)&3
			} else {
				w |= (t & 0x7ff) << 2
			}
			prog[i] = w
		}
	}
	b := make([]byte, 0, m.fwSize())
	for _, w := range prog {
		if bigEndian {
			b = append(b, byte(w>>16), byte(w>>8), byte(w))
		} else {
			b = append(b, byte(w), byte(w>>8), byte(w>>16))
		}
	}
	for i := 0; i < m.data; i++ {
		b = append(b, byte(rng.Intn(256)), byte(rng.Intn(256)))
	}
	return b
}

// necFuzz makes the extension for one chip; how: "file", "be" (byte-swapped
// file), "append" (program appended to the ROM), "none", "short".
func necFuzz(chip, how string) func(f *fuzzGen) {
	return func(f *fuzzGen) {
		rng := f.rng
		spec := necFiles[chip]
		var fw []byte
		if how != "hle" {
			fw = necRandomFirmware(rng, spec.model, how == "be")
		}
		switch how {
		case "file", "be":
			f.firmware = map[string][]byte{chip: fw}
		case "short":
			f.firmware = map[string][]byte{chip: fw[:len(fw)-1-rng.Intn(100)]}
			f.maxFrames = 2
		case "append":
			f.appendFW = fw
		case "none":
			f.maxFrames = 2
		}
		st := chip == "ST010" || chip == "ST011"
		var drAddr, srAddr, ramAddr func() int
		if st {
			f.sramAt = 0x680000
			drAddr = func() int { return (0x60+rng.Intn(8)+0x80*rng.Intn(2))<<16 | rng.Intn(0x800)<<1 }
			srAddr = func() int { return drAddr() | 1 }
			ramAddr = func() int { return (0x68+rng.Intn(8)+0x80*rng.Intn(2))<<16 | rng.Intn(0x1000) }
		} else {
			// where DR and SR are (snes_necdsp.js, snes_dsp.js)
			b0, b1, dr0, dr1, sr0, sr1 := 0x20, 0x3f, 0x8000, 0xbfff, 0xc000, 0xffff
			switch {
			case chip == "DSP-4":
				b0 = 0x30
			case chip == "DSP-1" && f.spec.mapping != mapLo:
				b0, b1, dr0, dr1, sr0, sr1 = 0x00, 0x1f, 0x6000, 0x6fff, 0x7000, 0x7fff
			case chip == "DSP-1" && f.spec.romSize > 0x100000:
				b0, b1, dr0, dr1, sr0, sr1 = 0x60, 0x6f, 0x0000, 0x3fff, 0x4000, 0x7fff
			}
			bank := func() int { return b0 + rng.Intn(b1-b0+1) + 0x80*rng.Intn(2) }
			drAddr = func() int { return bank()<<16 | (dr0 + rng.Intn(dr1-dr0+1)) }
			srAddr = func() int { return bank()<<16 | (sr0 + rng.Intn(sr1-sr0+1)) }
		}
		if how == "hle" {
			f.extraNMI = func(a *asm816) { dspHLECommands(f, a, chip, drAddr, srAddr) }
			return
		}
		f.extraNMI = func(a *asm816) {
			for i := 0; i < 8+rng.Intn(24); i++ {
				k := rng.Intn(6)
				if !st && k >= 4 {
					k = rng.Intn(4)
				}
				switch k {
				case 0: // SR
					a.op24(0xaf, srAddr()).sta([]int{0x2122, 0x210d, 0x210e, 0x2111}[rng.Intn(4)])
				case 1: // DR
					a.op24(0xaf, drAddr()).sta([]int{0x2122, 0x210f, 0x2110, 0x2112}[rng.Intn(4)])
				case 2: // write DR
					switch rng.Intn(3) {
					case 0:
						a.lda8(rng.Intn(256))
					case 1:
						a.ldaA(vPad)
					default:
						a.ldaA(vFrame)
					}
					a.op24(0x8f, drAddr())
				case 3: // wait (a while) for the chip to ask
					l := fmt.Sprintf("necw%d", len(a.buf))
					d := fmt.Sprintf("necd%d", len(a.buf))
					a.ldx8(1 + rng.Intn(255))
					a.label(l).op24(0xaf, srAddr()).br(0x30, d).db(0xca).br(0xd0, l)
					a.label(d)
				case 4: // data RAM
					a.op24(0xaf, ramAddr()).sta(0x2122)
				default:
					a.ldaA(vPad).op8(0x49, rng.Intn(256)).op24(0x8f, ramAddr())
				}
			}
		}
	}
}

// dsp1ParamCount: parameter words of each DSP-1 command (snes_dsp.js).
var dsp1ParamCount = map[int]int{
	0x00: 2, 0x20: 2, 0x10: 2, 0x30: 2, 0x04: 2, 0x24: 2, 0x08: 3, 0x18: 4, 0x38: 4, 0x28: 3, 0x0c: 3, 0x2c: 3,
	0x1c: 6, 0x3c: 6, 0x01: 4, 0x05: 4, 0x31: 4, 0x35: 4, 0x11: 4, 0x15: 4, 0x21: 4, 0x25: 4,
	0x0d: 3, 0x09: 3, 0x39: 3, 0x3d: 3, 0x19: 3, 0x1d: 3, 0x29: 3, 0x2d: 3, 0x03: 3, 0x33: 3, 0x13: 3, 0x23: 3,
	0x0b: 3, 0x3b: 3, 0x1b: 3, 0x2b: 3, 0x14: 6, 0x34: 6, 0x02: 7, 0x12: 7, 0x22: 7, 0x32: 7,
	0x0a: 1, 0x1a: 1, 0x2a: 1, 0x3a: 1, 0x06: 3, 0x16: 3, 0x26: 3, 0x36: 3, 0x0e: 2, 0x1e: 2, 0x2e: 2, 0x3e: 2,
	0x07: 1, 0x0f: 1, 0x17: 1, 0x27: 1, 0x37: 1, 0x2f: 1, 0x1f: 1, 0x3f: 1,
}

// dspHLECommands: DSP-1 / DSP-2 commands with random parameters, the results
// read back into the palette and scroll registers.
func dspHLECommands(f *fuzzGen, a *asm816, chip string, drAddr, srAddr func() int) {
	rng := f.rng
	var cmds []int
	for c := range dsp1ParamCount {
		cmds = append(cmds, c)
	}
	sortInts(cmds)
	word := func() int {
		switch rng.Intn(8) {
		case 0:
			return []int{0, 1, 0xffff, 0x7fff, 0x8000, 0x4000, 0xc000, 0x38c0}[rng.Intn(8)]
		case 1, 2:
			return rng.Intn(0x200) - 0x100
		case 3:
			return rng.Intn(0x4000) - 0x2000
		}
		return rng.Intn(0x10000)
	}
	put := func(v int) {
		switch rng.Intn(8) {
		case 0:
			a.ldaA(vPad).op8(0x49, v&0xff)
		case 1:
			a.ldaA(vFrame).op8(0x49, v&0xff)
		default:
			a.lda8(v & 0xff)
		}
		a.op24(0x8f, drAddr())
	}
	read := func(n int) {
		for i := 0; i < n; i++ {
			a.op24(0xaf, drAddr()).sta([]int{0x2122, 0x210d, 0x210f, 0x2111, 0x2113, 0x2122}[rng.Intn(6)])
		}
	}
	for i := 0; i < 4+rng.Intn(8); i++ {
		if rng.Intn(6) == 0 {
			a.op24(0xaf, srAddr()).sta(0x2122)
		}
		if chip == "DSP-2" {
			cmd := []int{0x01, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0d, 0x0f, rng.Intn(256)}[rng.Intn(10)]
			put(cmd)
			nOut := 4
			switch cmd {
			case 0x01:
				for k := 0; k < 32; k++ {
					put(rng.Intn(256))
				}
				nOut = 32
			case 0x03:
				put(rng.Intn(256))
				nOut = 0
			case 0x05:
				n := rng.Intn(9)
				put(n)
				for k := 0; k < 2*n; k++ {
					put(rng.Intn(256))
				}
				nOut = n
			case 0x06:
				n := rng.Intn(9)
				put(n)
				for k := 0; k < n; k++ {
					put(rng.Intn(256))
				}
				nOut = n
			case 0x07, 0x08, 0x09:
				for k := 0; k < 4; k++ {
					put(rng.Intn(256))
				}
			case 0x0d:
				outW, inW := rng.Intn(20), rng.Intn(20)
				put(outW)
				put(inW)
				for k := 0; k < (inW+1)>>1; k++ {
					put(rng.Intn(256))
				}
				nOut = (outW + 1) >> 1
			}
			read(nOut + rng.Intn(3) - 1)
			continue
		}
		cmd := cmds[rng.Intn(len(cmds))]
		if rng.Intn(10) == 0 {
			cmd = rng.Intn(256)
		}
		put(cmd)
		n := dsp1ParamCount[cmd]
		if rng.Intn(10) == 0 {
			n = rng.Intn(8) // too few or too many
		}
		for k := 0; k < n; k++ {
			w := word()
			put(w)
			put(w >> 8)
		}
		nOut := 2 * (1 + rng.Intn(4))
		if cmd&0x0f == 0x0a {
			nOut = 8 * (1 + rng.Intn(4)) // raster lines follow one another
		}
		read(nOut)
	}
}

func sortInts(v []int) {
	for i := 1; i < len(v); i++ {
		for k := i; k > 0 && v[k] < v[k-1]; k-- {
			v[k], v[k-1] = v[k-1], v[k]
		}
	}
}
