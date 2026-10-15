package gb

// Test ROMs for the cartridge chips no freely distributable game uses (or
// that need more coverage than the test suites give): each is a small
// program that pokes the chip at random, with the chip's interesting command
// sequences mixed in, and shows what it reads back on the screen. The same
// ROM runs in the web core and here (TestCompareCartsJS), and any difference
// in the chip shows up in the picture or the battery save.
//
// The program copies itself to work RAM and runs from there, so it keeps
// running whatever the chip maps in; every ROM bank also starts with the same
// start-up code, for chips that map another bank at 0000.

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// asm is a tiny SM83 assembler: raw bytes plus labels for jumps.
type asm struct {
	base   int
	buf    []byte
	labels map[string]int
	fix    []fixup
}

type fixup struct {
	at    int
	label string
	rel   bool
}

func newAsm(base int) *asm { return &asm{base: base, labels: map[string]int{}} }

func (a *asm) db(b ...int) *asm {
	for _, x := range b {
		a.buf = append(a.buf, byte(x))
	}
	return a
}
func (a *asm) pc() int           { return a.base + len(a.buf) }
func (a *asm) label(name string) { a.labels[name] = a.pc() }
func (a *asm) org(addr int) {
	for a.pc() < addr {
		a.buf = append(a.buf, 0)
	}
}

// jr / jp / call to a label: op is the opcode (0x18 jr, 0x20 jr nz, 0x28 jr z, 0xc3 jp, 0xca jp z, 0xcd call).
func (a *asm) jr(op int, l string) *asm {
	a.db(op, 0)
	a.fix = append(a.fix, fixup{len(a.buf) - 1, l, true})
	return a
}
func (a *asm) jp(op int, l string) *asm {
	a.db(op, 0, 0)
	a.fix = append(a.fix, fixup{len(a.buf) - 2, l, false})
	return a
}
func (a *asm) call(l string) *asm { return a.jp(0xcd, l) }
func (a *asm) dw(l string) *asm {
	a.db(0, 0)
	a.fix = append(a.fix, fixup{len(a.buf) - 2, l, false})
	return a
}

func (a *asm) link() []byte {
	for _, f := range a.fix {
		t, ok := a.labels[f.label]
		if !ok {
			panic("no label " + f.label)
		}
		if f.rel {
			d := t - (a.base + f.at + 1)
			if d < -128 || d > 127 {
				panic("jr too far to " + f.label)
			}
			a.buf[f.at] = byte(d)
		} else {
			a.buf[f.at], a.buf[f.at+1] = byte(t), byte(t>>8)
		}
	}
	return a.buf
}

// pokeEntry is one entry of a chip's list of interesting accesses: write
// val ^ (random & xmask) to addr, or read addr and show it. chain: go on
// with the next entry.
type pokeEntry struct {
	addr, val, xmask int
	read, chain      bool
	pulse, readAfter bool // write val, then val | 0x40 (a clock pulse); read the address afterwards
}

type cartSpec struct {
	name                       string
	typ, romCode, ramCode, cgb int
	seed                       int
	pokes                      [][]pokeEntry // single accesses and sequences (each picked at random)
}

const (
	tableBase  = 0xc200 // action jump table (16 words)
	valsBase   = 0xc240 // 16 "interesting" byte values
	entryBase  = 0xc300 // poke entries, 4 bytes each: lo, hi, val, xmask (up to 256)
	flagsBase  = 0xc700 // per entry: bit 0 chain, bit 1 read
	startsBase = 0xc800 // starting entry of each poke (a power of two of them)
	ptrLo      = 0x80   // HRAM: where the next result goes in VRAM
	ptrHi      = 0x81
	sIdx       = 0x84
	sRnd       = 0x85
	sVal       = 0x86
)

// fuzzProgram: the program that runs in work RAM.
func fuzzProgram(spec cartSpec) []byte {
	var entries []pokeEntry
	var starts []int
	for _, p := range spec.pokes {
		starts = append(starts, len(entries))
		for i, e := range p {
			e.chain = i < len(p)-1
			entries = append(entries, e)
		}
	}
	if len(entries) > 256 || len(starts) == 0 || len(starts) > 256 {
		panic("too many pokes")
	}
	nStarts := 1
	for nStarts < len(starts) {
		nStarts *= 2
	}
	for i := len(starts); i < nStarts; i++ {
		starts = append(starts, starts[i%len(spec.pokes)])
	}

	a := newAsm(0xc000)
	a.label("main")
	a.call("rnd").db(0xe6, 0x0f) // and 15
	a.db(0x87, 0x6f, 0x26, tableBase>>8, 0x2a, 0x66, 0x6f, 0xe9)
	// rnd: 16-bit Galois LFSR in DE; A = a random byte (B is lost)
	a.label("rnd")
	a.db(0x06, 0x08) // ld b,8: eight steps, a new byte
	a.label("rnd1")
	a.db(0xcb, 0x3a, 0xcb, 0x1b, 0x30, 0x04, 0x7a, 0xee, 0xb4, 0x57, 0x05).jr(0x20, "rnd1") // srl d; rr e; jr nc; ld a,d; xor b4; ld d,a; dec b; jr nz
	a.db(0x7b, 0xc9)                                                                        // ld a,e; ret
	// rndval: a value biased toward the ones chips care about
	a.label("rndval")
	a.call("rnd").db(0xe6, 0x03).jr(0x28, "rv_full").db(0x3d).jr(0x28, "rv_nib").db(0x3d).jr(0x28, "rv_small")
	a.call("rnd").db(0xe6, 0x0f, 0xc6, valsBase&0xff, 0x6f, 0x26, valsBase>>8, 0x7e, 0xc9)
	a.label("rv_full")
	a.jp(0xc3, "rnd")
	a.label("rv_nib")
	a.call("rnd").db(0xe6, 0x0f, 0xc9)
	a.label("rv_small")
	a.call("rnd").db(0xe6, 0x03, 0xc9)
	// store: show A in VRAM tile data 8000-8FFF
	a.label("store")
	a.db(0x4f, 0xf0, ptrLo, 0x6f, 0xf0, ptrHi, 0x67, 0x71, 0x23, 0x7c, 0xfe, 0x90).jr(0x20, "st1").db(0x26, 0x80)
	a.label("st1")
	a.db(0x7d, 0xe0, ptrLo, 0x7c, 0xe0, ptrHi).jp(0xc3, "main")
	// write a random value to the chip's registers (0000-7FFF)
	a.label("romw")
	a.call("rndval").db(0x4f).call("rnd").db(0xe6, 0x7f, 0x67).call("rnd").db(0x6f, 0x71).jp(0xc3, "main")
	// write / read cartridge RAM (A000-BFFF)
	a.label("ramw")
	a.call("rndval").db(0x4f).call("rnd").db(0xe6, 0x1f, 0xf6, 0xa0, 0x67).call("rnd").db(0x6f, 0x71).jp(0xc3, "main")
	a.label("ramr")
	a.call("rnd").db(0xe6, 0x1f, 0xf6, 0xa0, 0x67).call("rnd").db(0x6f, 0x7e).jp(0xc3, "store")
	// read the switchable ROM bank, or bank 0
	a.label("romr")
	a.call("rnd").db(0xe6, 0x3f, 0xf6, 0x40, 0x67).call("rnd").db(0x6f, 0x7e).jp(0xc3, "store")
	a.label("rom0r")
	a.call("rnd").db(0xe6, 0x3f, 0x67).call("rnd").db(0x6f, 0x7e).jp(0xc3, "store")
	// one of the chip's interesting accesses (or sequences)
	a.label("poke")
	a.call("rnd").db(0xe6, nStarts-1, 0x6f, 0x26, startsBase>>8, 0x7e)
	a.label("pokeI")
	a.db(0xe0, sIdx).call("rnd").db(0xe0, sRnd, 0xf0, sIdx)
	a.db(0x6f, 0x26, 0x00, 0x29, 0x29, 0x01, entryBase&0xff, entryBase>>8, 0x09) // HL = entryBase + idx*4
	a.db(0x2a, 0x4f, 0x2a, 0x47)                                                 // ld a,(hl+); ld c,a; ld a,(hl+); ld b,a  (BC = address)
	a.db(0xf0, sRnd, 0x23, 0xa6, 0x2b, 0xae)                                     // ldh a,(rnd); inc hl; and (hl) [xmask]; dec hl; xor (hl) [val]
	a.db(0xe0, sVal)                                                             // the value
	a.db(0xf0, sIdx, 0x6f, 0x26, flagsBase>>8, 0x7e)                             // HL = its flags
	a.db(0xe6, 0x02).jr(0x20, "pk_read")
	a.db(0xf0, sVal, 0x02)                // ld (bc),a
	a.db(0xcb, 0x56).jr(0x28, "pk_after") // bit 2,(hl): a clock pulse?
	a.db(0xf6, 0x40, 0x02)                // or 40; ld (bc),a
	a.label("pk_after")
	a.db(0xcb, 0x5e).jr(0x28, "pk_next") // bit 3,(hl): read back after?
	a.label("pk_read")
	a.db(0x0a)                             // ld a,(bc)
	a.db(0xe5).call("store_keep").db(0xe1) // push hl; show it; pop hl
	a.label("pk_next")
	a.db(0x7e, 0xe6, 0x01).jp(0xca, "main")  // ld a,(hl); and 1; jp z,main
	a.db(0xf0, sIdx, 0x3c).jr(0x18, "pokeI") // next entry
	// store_keep: store as a subroutine
	a.label("store_keep")
	a.db(0x4f, 0xf0, ptrLo, 0x6f, 0xf0, ptrHi, 0x67, 0x71, 0x23, 0x7c, 0xfe, 0x90).jr(0x20, "sk1").db(0x26, 0x80)
	a.label("sk1")
	a.db(0x7d, 0xe0, ptrLo, 0x7c, 0xe0, ptrHi, 0xc9)
	// read an interesting address
	a.label("peek")
	a.call("rnd").db(0xe6, nStarts-1, 0x6f, 0x26, startsBase>>8, 0x7e)
	a.db(0x6f, 0x26, 0x00, 0x29, 0x29, 0x01, entryBase&0xff, entryBase>>8, 0x09)
	a.db(0x2a, 0x4f, 0x46, 0x0a).jp(0xc3, "store") // ld a,(hl+); ld c,a; ld b,(hl); ld a,(bc)
	// wait a while (clocks tick on, the camera finishes)
	a.label("wait")
	a.call("rnd").db(0x47)
	a.label("wt1")
	a.db(0x05).jr(0x20, "wt1").jp(0xc3, "main")
	// sound registers, so the sound differs too
	a.label("apuw")
	a.call("rnd").db(0xe6, 0x1f, 0xc6, 0x10, 0x4f).call("rndval")
	a.db(0x47, 0x79, 0xfe, 0x26, 0x78).jr(0x20, "aw1").db(0xf6, 0x80) // keep the sound on
	a.label("aw1")
	a.db(0xe2).jp(0xc3, "main")
	a.label("wavew")
	a.call("rnd").db(0xe6, 0x0f, 0xc6, 0x30, 0x4f).call("rnd").db(0xe2).jp(0xc3, "main")

	if a.pc() > tableBase {
		panic(fmt.Sprintf("fuzz program too long (%x)", a.pc()))
	}
	a.org(tableBase)
	for _, l := range []string{"romw", "romw", "romw", "poke", "poke", "ramw", "ramw", "ramr", "ramr", "romr", "rom0r", "peek", "wait", "apuw", "wavew", "poke"} {
		a.dw(l)
	}
	a.org(valsBase)
	a.db(0x0a, 0x00, 0x01, 0x40, 0x08, 0x0b, 0x0c, 0x0d, 0x0e, 0x80, 0xff, 0x10, 0x20, 0x55, 0xaa, 0x30)
	a.org(entryBase)
	for _, e := range entries {
		a.db(e.addr&0xff, e.addr>>8, e.val, e.xmask)
	}
	a.org(flagsBase)
	for _, e := range entries {
		f := 0
		if e.chain {
			f |= 1
		}
		if e.read {
			f |= 2
		}
		if e.pulse {
			f |= 4
		}
		if e.readAfter {
			f |= 8
		}
		a.db(f)
	}
	a.org(startsBase)
	for _, s := range starts {
		a.db(s)
	}
	return a.link()
}

// buildFuzzROM: the cartridge image.
func buildFuzzROM(spec cartSpec) []byte {
	prog := fuzzProgram(spec)
	size := 0x8000 << spec.romCode
	rom := make([]byte, size)
	// start-up code (the same at the start of every bank)
	s := newAsm(0x100)
	s.db(0x00).jp(0xc3, "start")
	s.org(0x150)
	s.label("start")
	s.db(0xf3, 0x31, 0xfe, 0xff, 0xaf, 0xe0, 0x40)            // di; ld sp,fffe; LCD off
	s.db(0x21, 0x00, 0x04, 0x11, 0x00, 0xc0, 0x01)            // ld hl,0400; ld de,c000; ld bc,len
	s.db(len(prog)&0xff, len(prog)>>8)                        //
	s.label("copy")                                           //
	s.db(0x2a, 0x12, 0x13, 0x0b, 0x78, 0xb1).jr(0x20, "copy") // ld a,(hl+); ld (de),a; inc de; dec bc; ld a,b; or c; jr nz
	s.db(0x21, 0x00, 0x98)                                    // ld hl,9800: tile map = 0,1,2...
	s.label("fill")
	s.db(0x7d, 0x22, 0x7c, 0xfe, 0x9c).jr(0x20, "fill")
	s.db(0x3e, 0x80, 0xe0, 0x68, 0x21).dw("pal").db(0x06, 0x08) // CGB palette 0
	s.label("pl")
	s.db(0x2a, 0xe0, 0x69, 0x05).jr(0x20, "pl")
	s.db(0x3e, 0xe4, 0xe0, 0x47, 0x3e, 0x77, 0xe0, 0x24, 0x3e, 0xff, 0xe0, 0x25) // BGP; sound volume and panning
	s.db(0x3e, 0x91, 0xe0, 0x40)                                                 // LCD on
	s.db(0x3e, 0x00, 0xe0, ptrLo, 0x3e, 0x80, 0xe0, ptrHi)                       // results from 8000
	s.db(0x11, spec.seed&0xff, (spec.seed>>8)|1)                                 // LFSR seed
	s.db(0xc3, 0x00, 0xc0)
	s.label("pal")
	s.db(0xff, 0x7f, 0xb5, 0x56, 0x4a, 0x29, 0x00, 0x00)
	code := s.link()
	if len(code)+0x100 > 0x400 {
		panic("start-up code too long")
	}
	for b := 0; b < size/0x4000; b++ {
		o := b * 0x4000
		copy(rom[o+0x100:], code)
		copy(rom[o+0x400:], prog)
		// the rest of the bank: data that differs from bank to bank
		x := uint32(b*2654435761 + 12345)
		for i := 0x1000; i < 0x4000; i++ {
			x ^= x << 13
			x ^= x >> 17
			x ^= x << 5
			rom[o+i] = byte(x)
		}
		rom[o+0x1000], rom[o+0x1001] = byte(b), byte(b>>8)
	}
	copy(rom[0x134:], spec.name)
	rom[0x143] = byte(spec.cgb)
	rom[0x147], rom[0x148], rom[0x149] = byte(spec.typ), byte(spec.romCode), byte(spec.ramCode)
	return rom
}

func w(addr, val int) pokeEntry         { return pokeEntry{addr: addr, val: val} }
func wr(addr, val, xmask int) pokeEntry { return pokeEntry{addr: addr, val: val, xmask: xmask} }
func rd(addr int) pokeEntry             { return pokeEntry{addr: addr, read: true} }
func one(e ...pokeEntry) [][]pokeEntry  { return [][]pokeEntry{e} }
func singles(e ...pokeEntry) (out [][]pokeEntry) {
	for _, x := range e {
		out = append(out, []pokeEntry{x})
	}
	return
}
func cat(p ...[][]pokeEntry) (out [][]pokeEntry) {
	for _, x := range p {
		out = append(out, x...)
	}
	return
}

// microwire: bit-banged EEPROM commands for the MBC7 (bits: 0, 1, or 2 =
// random), then clocks with the output read back.
func microwire(readOut int, bits ...int) []pokeEntry {
	out := []pokeEntry{w(0, 0x0a), w(0x4000, 0x40), w(0xa080, 0x00), w(0xa080, 0x80)}
	for _, b := range bits {
		e := pokeEntry{addr: 0xa080, val: 0x80, pulse: true}
		switch b {
		case 1:
			e.val |= 2
		case 2:
			e.xmask = 2
		}
		out = append(out, e)
	}
	for i := 0; i < readOut; i++ {
		out = append(out, pokeEntry{addr: 0xa080, val: 0x80, pulse: true, readAfter: true})
	}
	return out
}

func joinE(p ...[]pokeEntry) (out []pokeEntry) {
	for _, x := range p {
		out = append(out, x...)
	}
	return
}

func rep(n, b int) []int {
	out := make([]int, n)
	for i := range out {
		out[i] = b
	}
	return out
}

func joinBits(p ...[]int) (out []int) {
	for _, x := range p {
		out = append(out, x...)
	}
	return
}

func fuzzSpecs() []cartSpec {
	mbc3 := cat(singles(w(0, 0x0a), w(0, 0), wr(0x2000, 0, 0xff), wr(0x4000, 0, 0x07), wr(0x4000, 0x08, 0x07), w(0x4000, 0x08), w(0x4000, 0x0c),
		wr(0xa000, 0, 0xff), wr(0xa000, 0, 0x3f), rd(0xa000), rd(0xa000)),
		one(w(0x6000, 0), w(0x6000, 1)), one(w(0x6000, 0), w(0x6000, 1)),
		one(w(0, 0x0a), w(0x4000, 0x0c), w(0xa000, 0x40)), one(w(0, 0x0a), w(0x4000, 0x0c), w(0xa000, 0x00)),
		one(w(0, 0x0a), w(0x4000, 0x0c), w(0xa000, 0x00)), one(w(0, 0x0a), w(0x4000, 0x0c), w(0xa000, 0x01)),
		one(w(0, 0x0a), w(0x4000, 0x08), w(0x6000, 0), w(0x6000, 1), rd(0xa000), w(0x4000, 0x09), rd(0xa000), w(0x4000, 0x0a), rd(0xa000), w(0x4000, 0x0b), rd(0xa000), w(0x4000, 0x0c), rd(0xa000)))
	mbc1 := singles(w(0, 0x0a), w(0, 0), wr(0x2000, 0, 0x1f), wr(0x4000, 0, 3), wr(0x6000, 0, 1), rd(0xa000), rd(0x4000), rd(0x0000))
	mbc5 := singles(w(0, 0x0a), w(0, 0), wr(0x2000, 0, 0xff), wr(0x3000, 0, 1), wr(0x4000, 0, 0x0f), wr(0x4000, 0x08, 0x07), wr(0x4000, 0, 0x07), rd(0xa000))
	flashSetup := one(w(0x2000, 2), w(0x3000, 1), w(0x2800, 8), w(0x3800, 8), w(0x0c00, 1), w(0x1000, 1))
	unlock := append(append([]pokeEntry{}, flashSetup[0]...), w(0x5555, 0xaa), w(0x6aaa, 0x55))
	seq := func(e ...pokeEntry) []pokeEntry { return append(append([]pokeEntry{}, unlock...), e...) }
	mbc6 := cat(flashSetup, flashSetup, flashSetup,
		one(seq(w(0x5555, 0xa0), wr(0x4123, 0, 0xff))...), one(seq(w(0x5555, 0xa0), wr(0x5555, 0, 0xff))...),
		one(seq(w(0x5555, 0xa0), wr(0x7f00, 0, 0xff))...),
		one(seq(w(0x5555, 0x90), rd(0x4000), rd(0x4001), rd(0x6002))...), one(w(0x4000, 0xf0)),
		one(seq(w(0x5555, 0x80), w(0x5555, 0xaa), w(0x6aaa, 0x55), w(0x5555, 0x30))...),
		one(seq(w(0x5555, 0x80), w(0x5555, 0xaa), w(0x6aaa, 0x55), w(0x5555, 0x10))...),
		one(seq(w(0x5555, 0xa0), w(0x3000, 0x0b), wr(0x6000, 0, 0xff), rd(0x6000), w(0x3000, 1))...),
		one(seq(w(0x5555, 0xa0), w(0x3000, 0x4b), wr(0x6010, 0, 0xff), rd(0x6010), w(0x3000, 1))...),
		one(w(0x3800, 8), w(0x3000, 0x4b), rd(0x6010), w(0x3000, 1)),
		one(seq(w(0x5555, 0xa0), pokeEntry{addr: 0x4123, xmask: 0xff, readAfter: true})...),
		one(seq(w(0x5555, 0xa0), w(0x3000, 0x0b), wr(0x6001, 0, 0xff), w(0x3000, 1))...),
		one(w(0x3800, 8), w(0x3000, 0x0b), rd(0x6000), rd(0x6001), w(0x3000, 0x2b), rd(0x6000), w(0x3000, 1)),
		singles(w(0, 0x0a), w(0, 0), wr(0x0400, 0, 7), wr(0x0800, 0, 7), wr(0x2000, 0, 0x7f), wr(0x3000, 0, 0x7f), w(0x2800, 0), w(0x3800, 0),
			w(0x2800, 8), w(0x3800, 8), w(0x1000, 0), w(0x1000, 1), rd(0x4000), rd(0x6000), rd(0xa000), rd(0xb000)))
	ewen := microwire(0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0)
	mbc7 := cat(one(w(0, 0x0a), w(0x4000, 0x40)), one(w(0, 0x0a), w(0x4000, 0x40)),
		one(ewen...), one(ewen...),
		one(microwire(0, joinBits([]int{1, 0, 1}, rep(5, 0), rep(3, 2), rep(16, 2))...)...), // write
		one(microwire(18, joinBits([]int{1, 1, 0}, rep(5, 0), rep(3, 2))...)...),            // read
		one(microwire(30, joinBits([]int{1, 1, 0}, rep(8, 2))...)...),                       // read on
		one(microwire(2, joinBits([]int{1, 1, 1}, rep(5, 0), rep(3, 2))...)...),             // erase
		one(microwire(0, joinBits([]int{1, 0, 0, 0, 1}, rep(6, 2), rep(16, 2))...)...),      // write all
		one(microwire(0, joinBits([]int{1, 0, 0, 1, 0}, rep(6, 2))...)...),                  // erase all
		one(microwire(0, joinBits([]int{1, 0, 0, 0, 0}, rep(6, 2))...)...),                  // write disable
		one(w(0xa000, 0x55), w(0xa010, 0xaa), rd(0xa020), rd(0xa030), rd(0xa040), rd(0xa050)),
		singles(w(0xa080, 0), wr(0xa080, 0x80, 0x42), wr(0xa080, 0x80, 0x42), rd(0xa080), rd(0xa060), w(0, 0), wr(0x2000, 0, 0x7f), w(0xa000, 0x55), w(0xa010, 0xaa)))
	huc3 := cat(singles(w(0, 0x0a), w(0, 0x0b), w(0, 0x0c), w(0, 0x0d), w(0, 0x0e), w(0, 0), wr(0x2000, 0, 0x7f), wr(0x4000, 0, 3),
		wr(0xa000, 0x10, 0x0f), wr(0xa000, 0x20, 0x0f), wr(0xa000, 0x30, 0x0f), wr(0xa000, 0x40, 0x0f), wr(0xa000, 0x50, 0x0f), wr(0xa000, 0x60, 0x03), rd(0xa000)),
		one(w(0, 0x0b), w(0xa000, 0x40), w(0xa000, 0x50), w(0xa000, 0x60), w(0xa000, 0x10), w(0, 0x0c), rd(0xa000), w(0, 0x0b), w(0xa000, 0x10), w(0, 0x0c), rd(0xa000),
			w(0, 0x0b), w(0xa000, 0x10), w(0, 0x0c), rd(0xa000), w(0, 0x0b), w(0xa000, 0x10), w(0, 0x0c), rd(0xa000)),
		one(w(0, 0x0b), w(0xa000, 0x40), w(0xa000, 0x50), wr(0xa000, 0x30, 0x0f), wr(0xa000, 0x30, 0x0f), wr(0xa000, 0x30, 0x0f), wr(0xa000, 0x30, 0x0f), w(0xa000, 0x61)),
		one(w(0, 0x0b), w(0xa000, 0x62), w(0, 0x0c), rd(0xa000), w(0, 0x0d), rd(0xa000)))
	tama := func(reg, val, xmask int) []pokeEntry { return []pokeEntry{w(0xa001, reg), wr(0xa000, val, xmask)} }
	tama5 := cat(
		one(joinE(tama(0, 0, 0x0f), tama(1, 0, 1))...),                                                                        // ROM bank
		one(joinE(tama(4, 0, 0x0f), tama(5, 0, 0x0f), tama(6, 0, 1), tama(7, 0, 0x0f))...),                                    // write RAM
		one(joinE(tama(6, 2, 1), tama(7, 0, 0x0f), []pokeEntry{w(0xa001, 0x0c), rd(0xa000), w(0xa001, 0x0d), rd(0xa000)})...), // read RAM
		one(joinE(tama(6, 4, 0), tama(7, 0, 7), []pokeEntry{w(0xa001, 0x0c), rd(0xa000), w(0xa001, 0x0d), rd(0xa000)})...),    // read clock
		one(joinE(tama(4, 0, 0x0f), tama(5, 0, 0x07), tama(6, 6, 0), tama(7, 0, 7))...),                                       // write clock
		singles(wr(0xa001, 0, 0x0f), wr(0xa000, 0, 0x0f), rd(0xa000), rd(0xa001), w(0xa001, 0x0a), rd(0xa000)))
	camera := cat(singles(w(0, 0x0a), w(0, 0), w(0x4000, 0x10), wr(0x4000, 0, 0x0f), wr(0x2000, 0, 0x3f), rd(0xa000), rd(0xa100), rd(0xa101)),
		one(w(0x4000, 0x10), wr(0xa001, 0, 0xff), wr(0xa002, 0, 0x03), wr(0xa003, 0, 0xff), w(0xa000, 0x03), rd(0xa000)),
		one(w(0x4000, 0x10), wr(0xa006, 0x40, 0x3f), wr(0xa007, 0x80, 0x3f), wr(0xa008, 0xc0, 0x3f), wr(0xa009, 0x40, 0x3f), wr(0xa00a, 0x80, 0x3f), wr(0xa00b, 0xc0, 0x3f), wr(0xa020, 0x40, 0x3f), wr(0xa021, 0x80, 0x3f), wr(0xa022, 0xc0, 0x3f)),
		one(w(0x4000, 0x10), rd(0xa000), w(0x4000, 0x00), rd(0xa100), rd(0xa180), rd(0xa200), rd(0xa400)))
	mmm01 := singles(wr(0, 0x0a, 0x40), w(0, 0x4a), wr(0x2000, 0, 0xff), wr(0x4000, 0, 0x7f), wr(0x6000, 0, 0x3d), rd(0x4000), rd(0x0000), rd(0xa000))
	huc1 := singles(w(0, 0x0a), w(0, 0x0e), wr(0x2000, 0, 0x3f), wr(0x4000, 0, 3), rd(0xa000), rd(0xa000))
	mbc2 := singles(w(0, 0x0a), w(0, 0), wr(0x0100, 0, 0x0f), wr(0x2100, 0, 0x0f), rd(0xa000), rd(0xa1ff))
	plain := singles(rd(0xa000), wr(0xa000, 0, 0xff))
	return []cartSpec{
		{name: "FUZZ ROM", typ: 0x00, romCode: 0, ramCode: 0, pokes: plain},
		{name: "FUZZ ROMRAMBAT", typ: 0x09, romCode: 0, ramCode: 2, pokes: plain},
		{name: "FUZZ MBC1", typ: 0x03, romCode: 5, ramCode: 3, pokes: mbc1},
		{name: "FUZZ MBC1 2MB", typ: 0x03, romCode: 6, ramCode: 2, pokes: mbc1, cgb: 0x80},
		{name: "FUZZ MBC2", typ: 0x06, romCode: 3, pokes: mbc2},
		{name: "FUZZ MBC3RTC", typ: 0x10, romCode: 4, ramCode: 3, pokes: mbc3},
		{name: "FUZZ MBC3TIMER", typ: 0x0f, romCode: 2, ramCode: 0, pokes: mbc3, cgb: 0xc0},
		{name: "FUZZ MBC3", typ: 0x13, romCode: 3, ramCode: 3, pokes: mbc3},
		{name: "FUZZ MBC30", typ: 0x10, romCode: 7, ramCode: 5, pokes: mbc3},
		{name: "FUZZ MBC5", typ: 0x1b, romCode: 6, ramCode: 4, pokes: mbc5, cgb: 0x80},
		{name: "FUZZ MBC5RUMBLE", typ: 0x1e, romCode: 4, ramCode: 3, pokes: mbc5},
		{name: "FUZZ MBC5 8MB", typ: 0x19, romCode: 8, ramCode: 0, pokes: mbc5},
		{name: "FUZZ MBC6", typ: 0x20, romCode: 5, ramCode: 0, pokes: mbc6, cgb: 0x80},
		{name: "FUZZ MBC7", typ: 0x22, romCode: 4, ramCode: 0, pokes: mbc7, cgb: 0x80},
		{name: "FUZZ HUC1", typ: 0xff, romCode: 5, ramCode: 3, pokes: huc1},
		{name: "FUZZ HUC3", typ: 0xfe, romCode: 5, ramCode: 3, pokes: huc3, cgb: 0x80},
		{name: "FUZZ MMM01", typ: 0x0d, romCode: 5, ramCode: 3, pokes: mmm01},
		{name: "FUZZ TAMA5", typ: 0xfd, romCode: 4, ramCode: 0, pokes: tama5},
		{name: "FUZZ CAMERA", typ: 0xfc, romCode: 5, ramCode: 4, pokes: camera},
		{name: "FUZZ UNKNOWN", typ: 0x2a, romCode: 0, pokes: plain},
	}
}

// writeFuzzROMs writes the test ROMs into dir, returning their paths.
func writeFuzzROMs(t testing.TB, dir string) []string {
	var out []string
	for i, s := range fuzzSpecs() {
		s.seed = 0x1234 + i*0x0f0f
		p := filepath.Join(dir, fmt.Sprintf("%02d %s.gb", i, s.name))
		if s.cgb != 0 {
			p += "c"
		}
		if err := os.WriteFile(p, buildFuzzROM(s), 0o644); err != nil {
			t.Fatal(err)
		}
		out = append(out, p)
	}
	return out
}

// TestCompareCartsJS: the cartridge-chip test ROMs, in the web core and here.
// GB_FUZZ_FRAMES sets the frames per ROM (default 300); GB_FUZZ_OUT keeps
// the ROMs in that folder.
func TestCompareCartsJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	frames := 300
	if s := os.Getenv("GB_FUZZ_FRAMES"); s != "" {
		fmt.Sscan(s, &frames)
	}
	dir := os.Getenv("GB_FUZZ_OUT")
	if dir == "" {
		dir = t.TempDir()
	}
	for _, p := range writeFuzzROMs(t, dir) {
		p := p
		t.Run(filepath.Base(p), func(t *testing.T) {
			t.Parallel()
			compareROM(t, node, p, frames)
		})
	}
}
