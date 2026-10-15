package arcade

// Generated test ROM sets, shaped like the real sets (the same file names and
// sizes) so the loader takes them. Their program ROMs hold a small
// "exerciser": a loop that writes random values all over the board's video,
// sprite, sound and control registers, copies the controls and switches into
// video memory (so the inputs change the picture), and counts its
// interrupts. The graphics ROMs and PROMs are random bytes. A "chaos" set has
// random bytes for its program too.

import (
	"fmt"
	"sort"
)

type rng struct{ s uint64 }

func (r *rng) next() uint64 {
	r.s ^= r.s << 13
	r.s ^= r.s >> 7
	r.s ^= r.s << 17
	return r.s
}

func (r *rng) bytes(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(r.next() >> 24)
	}
	return b
}

// asm: a tiny assembler for the 8080 subset of instructions the Z80 shares
// (and a few Z80 extras), with labels.
type asm struct {
	org    int
	b      []byte
	labels map[string]int
	fix    []struct {
		at    int
		label string
		rel   bool
	}
}

func newAsm(org int) *asm { return &asm{org: org, labels: map[string]int{}} }

func (a *asm) pc() int        { return a.org + len(a.b) }
func (a *asm) label(n string) { a.labels[n] = a.pc() }
func (a *asm) db(v ...int) {
	for _, x := range v {
		a.b = append(a.b, byte(x))
	}
}
func (a *asm) dw(v int) { a.db(v&0xff, v>>8&0xff) }
func (a *asm) ref(op int, l string) {
	a.db(op)
	a.fix = append(a.fix, struct {
		at    int
		label string
		rel   bool
	}{len(a.b), l, false})
	a.dw(0)
}
func (a *asm) jmp(l string)  { a.ref(0xc3, l) }
func (a *asm) call(l string) { a.ref(0xcd, l) }
func (a *asm) jnc(l string)  { a.ref(0xd2, l) }
func (a *asm) mvi(v int)     { a.db(0x3e, v) } // A = v
func (a *asm) sta(ad int)    { a.db(0x32); a.dw(ad) }
func (a *asm) lda(ad int)    { a.db(0x3a); a.dw(ad) }
func (a *asm) lxi(rp, v int) { a.db(0x01 | rp<<4); a.dw(v) } // 0 BC 1 DE 2 HL 3 SP
func (a *asm) out(port int)  { a.db(0xd3, port) }
func (a *asm) in(port int)   { a.db(0xdb, port) }
func (a *asm) done() []byte {
	for _, f := range a.fix {
		ad, ok := a.labels[f.label]
		if !ok {
			panic("no label " + f.label)
		}
		a.b[f.at], a.b[f.at+1] = byte(ad), byte(ad>>8)
	}
	return a.b
}

// rnd: DE = the next value of a 16-bit LFSR (HL kept).
func (a *asm) rndRoutine() {
	a.label("rnd")
	a.db(0xeb)                   // XCHG
	a.db(0x29)                   // DAD H
	a.jnc("rnd_ok")              //
	a.db(0x7c, 0xee, 0x10, 0x67) // MOV A,H; XRI 10; MOV H,A
	a.db(0x7d, 0xee, 0x21, 0x6f) // MOV A,L; XRI 21; MOV L,A
	a.label("rnd_ok")
	a.db(0xeb, 0xc9) // XCHG; RET
}

// poke: a random value at base + (random & mask).
func (a *asm) poke(base, mask int) {
	a.call("rnd")
	a.db(0x7a, 0xe6, mask>>8&0xff, 0x67) // MOV A,D; ANI mh; MOV H,A
	a.db(0x7b, 0xe6, mask&0xff, 0x6f)    // MOV A,E; ANI ml; MOV L,A
	a.lxi(0, base)
	a.db(0x09) // DAD B
	a.call("rnd")
	a.db(0x7b, 0x77) // MOV A,E; MOV M,A
}

// rndA: A = random & mask.
func (a *asm) rndA(mask int) {
	a.call("rnd")
	a.db(0x7a, 0xe6, mask)
}

// counter: increment the 16-bit counter at ad, saving the registers.
func (a *asm) countInt(ad, show int) {
	a.db(0xf5, 0xe5) // PUSH PSW; PUSH H
	a.db(0x2a)
	a.dw(ad) // LHLD
	a.db(0x23, 0x22)
	a.dw(ad) // INX H; SHLD
	a.db(0x7d)
	a.sta(show)
	a.db(0xe1, 0xf1) // POP H; POP PSW
}

type poke struct{ base, mask, n int }

type exSpec struct {
	z80     bool
	org     int
	sp      int
	counter int
	pokes   []poke
	ins     [][2]int // copy (from, to)
	outs    [][2]int // OUT port with random & mask
	inPorts [][2]int // IN port, store to
	every   func(a *asm)
	init    func(a *asm)
	irqStub func(a *asm) // code placed at org (vectors) before the main code
}

// exerciser builds the program.
func exerciser(s exSpec, size int, seed uint64) []byte {
	rom, _ := exerciserL(s, size, seed)
	return rom
}

// exerciserL also returns the program's labels.
func exerciserL(s exSpec, size int, seed uint64) ([]byte, map[string]int) {
	a := newAsm(0)
	a.db(0xf3) // DI
	a.jmp("init")
	if s.irqStub != nil {
		s.irqStub(a)
	}
	for a.pc() < 0x100 {
		a.db(0)
	}
	a.label("init")
	a.lxi(3, s.sp)
	a.lxi(1, int(seed&0xffff)|1) // DE: the LFSR
	if s.init != nil {
		s.init(a)
	}
	a.label("loop")
	for _, p := range s.pokes {
		for i := 0; i < p.n; i++ {
			a.poke(p.base, p.mask)
		}
	}
	for _, c := range s.ins {
		a.lda(c[0])
		a.sta(c[1])
	}
	for _, o := range s.outs {
		a.rndA(o[1])
		a.out(o[0])
	}
	for _, p := range s.inPorts {
		a.in(p[0])
		a.sta(p[1])
	}
	if s.every != nil {
		s.every(a)
	}
	a.db(0xfb) // EI
	a.jmp("loop")
	a.rndRoutine()
	code := a.done()
	if len(code) > size {
		panic(fmt.Sprintf("program too big: %d", len(code)))
	}
	rom := make([]byte, size)
	copy(rom, code)
	return rom, a.labels
}

// testSet is a generated ROM set: files in zip order.
type testSet struct {
	name    string // for the test's messages
	zipName string
	order   []string
	files   map[string][]byte
	frames  int
}

func (t *testSet) add(name string, data []byte) {
	if t.files == nil {
		t.files = map[string][]byte{}
	}
	t.order = append(t.order, name)
	t.files[name] = data
}

// split cuts a program into the set's chips (in file-list order of role cpu).
func splitInto(t *testSet, g *Game, role string, data []byte, rename map[string]string) {
	for _, f := range g.Files {
		if f.Role != role {
			continue
		}
		off := f.Offset
		if off == Next {
			off = 0
		}
		n := f.Name
		if r, ok := rename[n]; ok {
			n = r
		}
		t.add(n, append([]byte(nil), data[off:off+f.Size]...))
	}
}

func gameByID(id string, nth int) *Game {
	for _, e := range AllGames() {
		if e.Game.ID == id {
			if nth == 0 {
				return e.Game
			}
			nth--
		}
	}
	return nil
}

// fillRest adds every listed file not added yet, with random bytes.
func fillRest(t *testSet, g *Game, r *rng, rename map[string]string) {
	for _, f := range g.Files {
		n := f.Name
		if x, ok := rename[n]; ok {
			n = x
		}
		if _, ok := t.files[n]; ok || f.Optional && r.next()%2 == 0 {
			continue
		}
		t.add(n, r.bytes(f.Size))
	}
}

// ---- the boards' programs ----

func pacmanProgram(seed uint64) ([]byte, map[string]int) {
	return exerciserL(exSpec{
		z80: true, sp: 0x4f00, counter: 0x4e00,
		irqStub: func(a *asm) {
			for a.pc() < 0x38 {
				a.db(0)
			}
			a.jmp("irq")
		},
		init: func(a *asm) {
			a.mvi(0x3f)
			a.db(0xed, 0x47) // LD I,A
			a.db(0xed, 0x5e) // IM 2
			a.mvi(0x00)
			a.out(0)
			a.mvi(1)
			a.sta(0x5000) // interrupts on
			a.sta(0x5001) // sound on
		},
		pokes: []poke{{0x4000, 0x3ff, 4}, {0x4400, 0x3ff, 4}, {0x4ff0, 0xf, 3}, {0x5040, 0x1f, 4}, {0x5060, 0xf, 3}, {0x5002, 0x7, 1}},
		ins:   [][2]int{{0x5000, 0x4100}, {0x5040, 0x4101}, {0x5080, 0x4102}, {0x5000, 0x4500}},
		outs:  [][2]int{{0, 0xfe}},
		every: func(a *asm) {
			a.mvi(1)
			a.sta(0x5000)
			a.sta(0x5001)
			a.sta(0x50c0) // watchdog
			a.jmp("after")
			a.label("irq")
			a.countInt(0x4e00, 0x4040)
			a.db(0xfb, 0xed, 0x4d) // EI; RETI
			a.label("after")
		},
	}, 0x3f00, seed)
}

func pacmanSet(nth int, seed uint64, chaos bool, zip string) *testSet {
	r := &rng{seed | 1}
	ids := []string{"puckman", "puckman", "pacman"}
	g := gameByID(ids[nth], map[int]int{0: 0, 1: 1, 2: 0}[nth])
	t := &testSet{name: fmt.Sprintf("%s-%d", g.ID, nth), zipName: zip, frames: 3000}
	prog := make([]byte, 0x4000)
	if chaos {
		prog = r.bytes(0x4000)
		t.name += "-chaos"
	} else {
		code, labels := pacmanProgram(seed)
		copy(prog, code)
		for i := 0x3f00; i < 0x4000; i++ {
			prog[i] = 0x20 // the interrupt vector table: every entry is 0x2020
		}
		irq := newAsm(0x2020)
		irq.jmp("x")
		irq.labels["x"] = labels["irq"]
		copy(prog[0x2020:], irq.done())
	}
	splitInto(t, g, "cpu", prog, nil)
	// the PROMs under their other names now and then
	rename := map[string]string{}
	if nth == 2 && seed%2 == 0 {
		rename = map[string]string{"82s123.7f": "pm1-1.7f", "82s126.4a": "pm1-4.4a"}
	}
	fillRest(t, g, r, rename)
	return t
}

func galaxianProgram(seed uint64) []byte {
	return exerciser(exSpec{
		z80: true, sp: 0x4400,
		irqStub: func(a *asm) {
			for a.pc() < 0x66 {
				a.db(0)
			}
			a.countInt(0x4000, 0x5000)
			a.db(0xed, 0x45) // RETN
		},
		init: func(a *asm) {
			a.mvi(1)
			a.sta(0x7001)
			a.sta(0x7004)
		},
		pokes: []poke{{0x5000, 0x3ff, 4}, {0x5800, 0x3f, 3}, {0x5840, 0x1f, 4}, {0x5860, 0x1f, 4}, {0x6000, 7, 2}, {0x6800, 7, 3}, {0x7002, 7, 1}, {0x7800, 0, 1}},
		ins:   [][2]int{{0x6000, 0x5100}, {0x6800, 0x5101}, {0x7000, 0x5102}},
		every: func(a *asm) {
			a.mvi(1)
			a.sta(0x7001) // NMI on
			a.rndA(0x3f)
			a.sta(0x7800) // a tune
		},
	}, 0x2800, seed)
}

func galaxianSet(nth int, seed uint64, chaos bool, zip string) *testSet {
	r := &rng{seed | 1}
	g := gameByID([]string{"galaxian", "galmidw"}[nth], 0)
	t := &testSet{name: g.ID, zipName: zip, frames: 3000}
	prog := make([]byte, 0x2800)
	if chaos {
		prog = r.bytes(0x2800)
		t.name += "-chaos"
	} else {
		copy(prog, galaxianProgram(seed))
	}
	rename := map[string]string{}
	if seed%2 == 0 {
		rename = map[string]string{"7l": "galmidw.z", "1h.bin": "galaxian.1j", "6l.bpr": "6l"}
	}
	splitInto(t, g, "cpu", prog, rename)
	fillRest(t, g, r, rename)
	return t
}

func invadersProgram(seed uint64) []byte {
	return exerciser(exSpec{
		sp: 0x2400,
		irqStub: func(a *asm) {
			for a.pc() < 0x08 {
				a.db(0)
			}
			a.jmp("rst1")
			for a.pc() < 0x10 {
				a.db(0)
			}
			a.jmp("rst2")
		},
		pokes:   []poke{{0x2400, 0x1bff, 10}},
		outs:    [][2]int{{2, 0xff}, {4, 0xff}, {4, 0xff}, {3, 0xff}, {5, 0xff}, {6, 0xff}},
		inPorts: [][2]int{{0, 0x2410}, {1, 0x2430}, {2, 0x2450}, {3, 0x2470}},
		every: func(a *asm) {
			a.mvi(0x20)
			a.out(3) // sound amplifier on
			a.jmp("after")
			a.label("rst1")
			a.countInt(0x2000, 0x2500)
			a.db(0xfb, 0xc9)
			a.label("rst2")
			a.countInt(0x2002, 0x2501)
			a.db(0xfb, 0xc9)
			a.label("after")
		},
	}, 0x2000, seed)
}

func invadersSet(seed uint64, chaos bool, zip string) *testSet {
	r := &rng{seed | 1}
	g := gameByID("invaders", 0)
	t := &testSet{name: "invaders", zipName: zip, frames: 3000}
	prog := invadersProgram(seed)
	if chaos {
		prog = r.bytes(0x2000)
		t.name += "-chaos"
	}
	splitInto(t, g, "cpu", prog, nil)
	return t
}

func dkongProgram(seed uint64) []byte {
	return exerciser(exSpec{
		z80: true, sp: 0x6c00,
		irqStub: func(a *asm) {
			for a.pc() < 0x66 {
				a.db(0)
			}
			a.countInt(0x6000, 0x7440)
			a.db(0xed, 0x45) // RETN
		},
		init: func(a *asm) {
			a.mvi(1)
			a.sta(0x7d84)
		},
		pokes: []poke{{0x6900, 0x1ff, 6}, {0x7400, 0x3ff, 4}, {0x7000, 0x3ff, 2}, {0x7c00, 0, 1}, {0x7d00, 7, 2}, {0x7d80, 7, 1}},
		ins:   [][2]int{{0x7c00, 0x7500}, {0x7c80, 0x7501}, {0x7d00, 0x7502}, {0x7d80, 0x7503}},
		every: func(a *asm) {
			// the DMA copies the sprite list from 6900 to 7000
			a.sta(0x7808)
			for _, w := range [][3]int{{0x7800, 0x00, 0x69}, {0x7801, 0x7f, 0x01}, {0x7802, 0x00, 0x70}} {
				a.mvi(w[1])
				a.sta(w[0])
				a.mvi(w[2])
				a.sta(w[0])
			}
			a.mvi(0)
			a.sta(0x7d85)
			a.mvi(1)
			a.sta(0x7d85)
			a.sta(0x7d84) // NMI on
		},
	}, 0x4000, seed)
}

// dkSoundProgram: the 8035 reads the tune latch and the tune data and plays
// a changing sawtooth through the DAC, toggling the volume and the data page.
func dkSoundProgram() []byte {
	p := make([]byte, 0x800)
	put := func(at int, b ...byte) { copy(p[at:], b) }
	put(0x000, 0x04, 0x10)                               // JMP 010
	put(0x003, 0x04, 0x40)                               // JMP 040 (external interrupt)
	put(0x007, 0x04, 0x50)                               // JMP 050 (timer interrupt)
	put(0x010, 0x23, 0xc0, 0x3a)                         // MOV A,#C0; OUTL P2,A
	put(0x013, 0x05, 0x25, 0x55)                         // EN I; EN TCNTI; STRT T
	put(0x016, 0x8a, 0x40, 0x80, 0x6a, 0xaa)             // ORL P2,#40; MOVX A,@R0; ADD A,R2; MOV R2,A
	put(0x01b, 0x9a, 0xbf, 0x80, 0x6b, 0x6a, 0xab, 0x39) // ANL P2,#BF; MOVX; ADD A,R3; ADD A,R2; MOV R3,A; OUTL P1,A
	put(0x022, 0x18)                                     // INC R0
	put(0x023, 0x36, 0x27)                               // JT0 027
	put(0x025, 0x1c, 0x00)                               // INC R4
	put(0x027, 0x56, 0x2b)                               // JT1 02B
	put(0x029, 0x1d, 0x00)                               // INC R5
	put(0x02b, 0xed, 0x16)                               // DJNZ R5,016
	put(0x02d, 0x0a, 0xd3, 0x87, 0x3a)                   // IN A,P2; XRL A,#87; OUTL P2,A
	put(0x031, 0x04, 0x16)                               // JMP 016
	put(0x040, 0x1e, 0x93)                               // INC R6; RETR
	put(0x050, 0x1f, 0x42, 0x39, 0x93)                   // INC R7; MOV A,T; OUTL P1,A; RETR
	return p
}

func dkongSet(seed uint64, chaos bool, zip string) *testSet {
	r := &rng{seed | 1}
	g := gameByID("dkong", 0)
	t := &testSet{name: "dkong", zipName: zip, frames: 3000}
	prog := dkongProgram(seed)
	snd := make([]byte, 0x1800)
	copy(snd, dkSoundProgram())
	copy(snd[0x1000:], r.bytes(0x800))
	if chaos {
		prog = r.bytes(0x4000)
		copy(snd, r.bytes(0x800))
		t.name += "-chaos"
	}
	splitInto(t, g, "cpu", prog, nil)
	splitInto(t, g, "sound", snd, nil)
	fillRest(t, g, r, nil)
	return t
}

// sortedNames: the set's files sorted (a zip made by another tool).
func (t *testSet) shuffle(r *rng) {
	sort.Slice(t.order, func(i, j int) bool { return t.order[i] < t.order[j] })
	for i := len(t.order) - 1; i > 0; i-- {
		j := int(r.next() % uint64(i+1))
		t.order[i], t.order[j] = t.order[j], t.order[i]
	}
}
