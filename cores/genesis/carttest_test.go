package genesis

// Generated cartridge test programs: no freely distributable games use the
// EEPROMs, the J-Cart, the Realtec or SSF2 mappers, so these build small
// test cartridges for each. Every cartridge runs the same 68000 program, an
// interpreter for a table of bus accesses the test generates: writes (fixed
// or pseudo-random), reads, waits for the next frame and SR changes. Every
// value read is mixed into a checksum that the program shows as the backdrop
// colour and plays through the YM2612's DAC, so a single differing read
// shows in the compared pictures and sound. Interrupt handlers mix in when
// they run, and a Z80 program plays the 68000's ROM through the YM2612 and
// PSG.

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// asm68 is just enough of an assembler for the test program: words, labels
// and branches.
type asm68 struct {
	base   int
	code   []byte
	labels map[string]int
	fixW   map[int]string // word displacement (bra.w, dbcc) at offset -> label
	fixB   map[int]string // byte displacement (bcc.s) at offset -> label
}

func newAsm68(base int) *asm68 {
	return &asm68{base: base, labels: map[string]int{}, fixW: map[int]string{}, fixB: map[int]string{}}
}
func (a *asm68) w(ws ...int) *asm68 {
	for _, v := range ws {
		a.code = binary.BigEndian.AppendUint16(a.code, uint16(v))
	}
	return a
}
func (a *asm68) l(v int) *asm68 { return a.w(v>>16, v&0xffff) }
func (a *asm68) pc() int        { return a.base + len(a.code) }
func (a *asm68) label(n string) { a.labels[n] = a.pc() }
func (a *asm68) braW(op int, n string) *asm68 {
	a.w(op)
	a.fixW[len(a.code)] = n
	return a.w(0)
}
func (a *asm68) bccS(op int, n string) *asm68 {
	a.fixB[len(a.code)] = n
	return a.w(op)
}
func (a *asm68) link() []byte {
	for off, n := range a.fixW {
		d := a.labels[n] - (a.base + off)
		binary.BigEndian.PutUint16(a.code[off:], uint16(int16(d)))
	}
	for off, n := range a.fixB {
		d := a.labels[n] - (a.base + off + 2)
		a.code[off+1] = uint8(int8(d))
	}
	return a.code
}

const (
	progAt  = 0x200
	tableAt = 0x600
)

// interpreter assembles the table interpreter (at progAt; the table at tableAt).
func interpreter(seed int) (code []byte, handler int) {
	a := newAsm68(progAt)
	a.w(0x46fc, 0x2700)    // move.w #$2700,sr
	a.w(0x41f9).l(tableAt) // lea table,a0
	a.w(0x7e00)            // moveq #0,d7
	a.w(0x2c3c).l(seed)    // move.l #seed,d6
	a.label("loop")
	a.w(0x3018)         // move.w (a0)+,d0
	a.w(0x2258)         // movea.l (a0)+,a1
	a.w(0x3218)         // move.w (a0)+,d1
	a.w(0x3818)         // move.w (a0)+,d4
	a.w(0xe548)         // lsl.w #2,d0
	a.w(0x4efb, 0x0002) // jmp 2(pc,d0.w)
	for _, n := range []string{"restart", "w8", "w16", "r8", "r16", "wait", "rnd8", "rnd16", "setsr"} {
		a.braW(0x6000, n)
	}
	a.label("restart")
	a.w(0x41f9).l(tableAt) // lea table,a0
	a.braW(0x6000, "loop")
	a.label("w8")
	a.w(0x1281) // move.b d1,(a1)
	a.braW(0x6000, "loop")
	a.label("w16")
	a.w(0x3281) // move.w d1,(a1)
	a.braW(0x6000, "loop")
	a.label("r8")
	a.w(0x7400, 0x1411) // moveq #0,d2; move.b (a1),d2
	a.braW(0x6000, "mix")
	a.label("r16")
	a.w(0x7400, 0x3411) // moveq #0,d2; move.w (a1),d2
	a.label("mix")
	a.w(0xebbf, 0xde82, 0xbf86)           // rol.l #5,d7; add.l d2,d7; eor.l d7,d6
	a.w(0x23fc).l(0xc0000000).l(0xc00004) // move.l #$c0000000,$c00004 (CRAM 0)
	a.w(0x33c7).l(0xc00000)               // move.w d7,$c00000
	a.w(0x13fc, 0x002a).l(0xa04000)       // move.b #$2a,$a04000 (YM DAC)
	a.w(0x13c7).l(0xa04001)               // move.b d7,$a04001
	a.braW(0x6000, "loop")
	a.label("wait")
	a.w(0x5341)            // subq.w #1,d1
	a.braW(0x6b00, "loop") // bmi.w loop
	a.w(0x3a3c, 30000)     // move.w #30000,d5
	a.label("w1")
	a.w(0x3639).l(0xc00004).w(0x0803, 0x0003) // move.w $c00004,d3; btst #3,d3
	a.braW(0x57cd, "w1")                      // dbeq d5,w1
	a.w(0x3a3c, 30000)
	a.label("w2")
	a.w(0x3639).l(0xc00004).w(0x0803, 0x0003)
	a.braW(0x56cd, "w2") // dbne d5,w2
	a.braW(0x6000, "wait")
	a.label("rnd8")
	a.w(0xefbe, 0x0686).l(0x9e3779b9) // rol.l #7,d6; addi.l #$9e3779b9,d6
	a.w(0x2606, 0xc641, 0x8644)       // move.l d6,d3; and.w d1,d3; or.w d4,d3
	a.w(0x1283)                       // move.b d3,(a1)
	a.braW(0x6000, "loop")
	a.label("rnd16")
	a.w(0xefbe, 0x0686).l(0x9e3779b9)
	a.w(0x2606, 0xc641, 0x8644)
	a.w(0x3283) // move.w d3,(a1)
	a.braW(0x6000, "loop")
	a.label("setsr")
	a.w(0x46c1) // move.w d1,sr
	a.braW(0x6000, "loop")
	a.label("handler")
	a.w(0xe7bf, 0x5287, 0x4e73) // rol.l #3,d7; addq.l #1,d7; rte
	code = a.link()
	if progAt+len(code) > tableAt {
		panic("test program too long")
	}
	return code, a.labels["handler"]
}

// Table commands.
const (
	cRestart = iota
	cW8
	cW16
	cR8
	cR16
	cWait
	cRnd8
	cRnd16
	cSetSR
)

type table struct{ b []byte }

func (t *table) e(cmd, addr, a, b int) {
	t.b = binary.BigEndian.AppendUint16(t.b, uint16(cmd))
	t.b = binary.BigEndian.AppendUint32(t.b, uint32(addr))
	t.b = binary.BigEndian.AppendUint16(t.b, uint16(a))
	t.b = binary.BigEndian.AppendUint16(t.b, uint16(b))
}
func (t *table) w8(addr, v int)         { t.e(cW8, addr, v, 0) }
func (t *table) w16(addr, v int)        { t.e(cW16, addr, v, 0) }
func (t *table) r8(addr int)            { t.e(cR8, addr, 0, 0) }
func (t *table) r16(addr int)           { t.e(cR16, addr, 0, 0) }
func (t *table) wait(n int)             { t.e(cWait, 0, n, 0) }
func (t *table) rnd8(addr, and, or int) { t.e(cRnd8, addr, and, or) }
func (t *table) rnd16(addr, and, or int) {
	t.e(cRnd16, addr, and, or)
}
func (t *table) sr(v int) { t.e(cSetSR, 0, v, 0) }

// z80Prog: reads the 68000's ROM through the bank window and plays it through
// the YM2612's DAC and the PSG, moving the bank now and then; counts its
// vertical-blank interrupts (IM 1).
var z80Prog = []byte{
	0xf3, 0x31, 0x00, 0x20, // di; ld sp,$2000
	0xed, 0x56, // im 1
	0x3e, 0x2b, 0x32, 0x00, 0x40, 0x3e, 0x80, 0x32, 0x01, 0x40, // YM $2b = $80 (DAC on)
	0x3e, 0x2a, 0x32, 0x00, 0x40, // YM address $2a
	0xfb,             // ei
	0x21, 0x00, 0x80, // l1: ld hl,$8000
	0x7e,             // l2: ld a,(hl)
	0x32, 0x01, 0x40, // ld ($4001),a
	0x32, 0x11, 0x7f, // ld ($7f11),a
	0x23,       // inc hl
	0x7c, 0xb7, // ld a,h; or a
	0xf2, 0x16, 0x00, // jp p,l1
	0x3a, 0x00, 0x40, // ld a,($4000)
	0x32, 0x00, 0x60, // ld ($6000),a
	0xc3, 0x19, 0x00, // jp l2
}

func init() {
	// pad the Z80 program to its interrupt handler at $38
	for len(z80Prog) < 0x38 {
		z80Prog = append(z80Prog, 0)
	}
	z80Prog = append(z80Prog, 0x08, 0x3c, 0x08, 0xfb, 0xc9) // ex af,af'; inc a; ex af,af'; ei; ret
}

// header fields for a test cartridge
type hdr struct {
	names  string // domestic name (also the overseas one)
	code   string // at $180
	io     string // at $190
	ra     []byte // the 12 bytes at $1B0 (battery RAM / EEPROM information)
	region string // at $1F0
}

func put(rom []byte, at int, s string, n int) {
	for i := 0; i < n; i++ {
		c := byte(' ')
		if i < len(s) {
			c = s[i]
		}
		rom[at+i] = c
	}
}

// image builds the first part of a cartridge: vectors, header, program, table.
func image(h hdr, seed int, tb *table, extra func(t *table)) []byte {
	code, handler := interpreter(seed)
	t := &table{}
	// start the Z80: bus request, load its program, release its reset and the bus
	t.w16(0xa11100, 0x100)
	t.w16(0xa11200, 0)
	for i, b := range z80Prog {
		t.w8(0xa00000+i, int(b))
	}
	t.w16(0xa11200, 0x100)
	t.w16(0xa11100, 0)
	// a picture: display on, plane B showing tiles of all colours
	for _, r := range []int{0x8004, 0x8174, 0x8230, 0x8407, 0x8c81, 0x8f02, 0x9001, 0x8700} {
		t.w16(0xc00004, r)
	}
	t.w16(0xc00004, 0x4020) // VRAM $0020: tile 1
	t.w16(0xc00004, 0x0000)
	for i := 0; i < 16; i++ {
		t.w16(0xc00000, 0x0123+i*0x1111&0xffff)
	}
	t.w16(0xc00004, 0x6000) // VRAM $E000: plane B names
	t.w16(0xc00004, 0x0003)
	for i := 0; i < 64*2; i++ { // (two rows: the table has to fit Realtec's 8K)
		t.w16(0xc00000, 1+i%2)
	}
	if extra != nil {
		extra(t)
	}
	t.b = append(t.b, tb.b...)
	t.e(cRestart, 0, 0, 0)

	img := make([]byte, tableAt+len(t.b))
	binary.BigEndian.PutUint32(img[0:], 0x00fffe00)
	binary.BigEndian.PutUint32(img[4:], progAt)
	for v := 2; v < 64; v++ {
		binary.BigEndian.PutUint32(img[v*4:], uint32(handler))
	}
	put(img, 0x100, "SEGA MEGA DRIVE", 16)
	put(img, 0x120, h.names, 48)
	put(img, 0x150, h.names, 48)
	put(img, 0x180, h.code, 14)
	put(img, 0x190, h.io, 16)
	for i := 0; i < 12; i++ {
		img[0x1b0+i] = ' '
	}
	copy(img[0x1b0:], h.ra)
	put(img, 0x1f0, h.region, 3)
	copy(img[progAt:], code)
	copy(img[tableAt:], t.b)
	return img
}

// cartROM fills a ROM of the given size with numbers and puts img at its start.
func cartROM(size int, img []byte, seed uint32) []byte {
	rom := make([]byte, size)
	x := seed | 1
	for i := range rom {
		x ^= x << 13
		x ^= x >> 17
		x ^= x << 5
		rom[i] = byte(x >> 7)
	}
	copy(rom, img)
	return rom
}

type rng32 struct{ s uint32 }

func (r *rng32) n(k int) int {
	r.s ^= r.s << 13
	r.s ^= r.s >> 17
	r.s ^= r.s << 5
	return int(r.s % uint32(k))
}

// ---- the I2C EEPROM, driven bit by bit ----

type i2c struct {
	t                  *table
	sda, scl           pin
	out                pin
	sclLevel, sdaLevel int
}

// lines writes both lines (one write when they share an address).
func (b *i2c) lines(scl, sda int) {
	b.sclLevel, b.sdaLevel = scl, sda
	if b.sda.addr == b.scl.addr {
		b.t.w8(b.sda.addr, sda<<b.sda.bit|scl<<b.scl.bit)
		return
	}
	if b.sda.addr&^1 == b.scl.addr&^1 { // one word write sets both bytes
		v := 0
		for _, p := range []struct {
			p pin
			l int
		}{{b.sda, sda}, {b.scl, scl}} {
			sh := p.p.bit
			if p.p.addr&1 == 0 {
				sh += 8
			}
			v |= p.l << sh
		}
		b.t.w16(b.sda.addr&^1, v)
		return
	}
	b.t.w8(b.scl.addr, scl<<b.scl.bit)
	b.t.w8(b.sda.addr, sda<<b.sda.bit)
}
func (b *i2c) start() { b.lines(1, 1); b.lines(1, 0); b.lines(0, 0) }
func (b *i2c) stop()  { b.lines(0, 0); b.lines(1, 0); b.lines(1, 1) }
func (b *i2c) bit(v int) {
	b.lines(0, v)
	b.lines(1, v)
	b.t.r8(b.out.addr)
	b.lines(0, v)
}
func (b *i2c) byte(v int) {
	for i := 7; i >= 0; i-- {
		b.bit((v >> i) & 1)
	}
	b.bit(1) // the acknowledge clock (released line)
}
func (b *i2c) readByte(ack bool) {
	for i := 0; i < 8; i++ {
		b.bit(1)
	}
	if ack {
		b.bit(0)
	} else {
		b.bit(1)
	}
}

// eepromTable: writes and reads at random addresses in the chip's protocol,
// with some bit errors and stray START / STOPs.
func eepromTable(t *table, w wiring, c chip, r *rng32, n int) {
	b := &i2c{t: t, sda: w.sda, scl: w.scl, out: w.out}
	addrBytes := func(addr int, read bool) {
		rw := 0
		if read {
			rw = 1
		}
		switch c.mode {
		case 1:
			b.byte((addr&0x7f)<<1 | rw)
		case 2:
			b.byte(0xa0 | (addr>>8&7)<<1)
			b.byte(addr & 0xff)
			if read {
				b.start()
				b.byte(0xa1 | (addr>>8&7)<<1)
			}
		case 3:
			b.byte(0xa0)
			b.byte(addr >> 8 & 0xff)
			b.byte(addr & 0xff)
			if read {
				b.start()
				b.byte(0xa1)
			}
		}
	}
	for k := 0; k < n; k++ {
		addr := r.n(c.size * 2)
		b.start()
		if r.n(2) == 0 {
			addrBytes(addr, false)
			for i := r.n(c.page + 3); i >= 0; i-- {
				b.byte(r.n(256))
			}
		} else {
			addrBytes(addr, true)
			for i := r.n(c.page + 5); i >= 0; i-- {
				b.readByte(i > 0)
			}
		}
		if r.n(10) == 0 { // a stray bit
			b.bit(r.n(2))
		}
		b.stop()
		if r.n(8) == 0 {
			t.wait(1)
		}
	}
}

// ---- the test cartridges ----

type cartTest struct {
	name   string
	rom    []byte
	frames int
}

func cartTests() []cartTest {
	var out []cartTest
	add := func(name string, rom []byte, frames int) { out = append(out, cartTest{name, rom, frames}) }
	ra := func(kind byte, st, en int) []byte {
		b := []byte{'R', 'A', kind, 0x20, 0, 0, 0, 0, 0, 0, 0, 0}
		binary.BigEndian.PutUint32(b[4:], uint32(st))
		binary.BigEndian.PutUint32(b[8:], uint32(en))
		return b
	}

	// Battery RAM: writes, reads and the write protect at $A130F1.
	sramTable := func(lo, hi int, r *rng32) *table {
		t := &table{}
		for k := 0; k < 3000; k++ {
			a := lo + r.n(hi-lo)
			switch r.n(9) {
			case 0, 1, 2:
				t.rnd8(a, 0xff, 0)
			case 3:
				t.rnd16(a&^1, 0xffff, 0)
			case 4, 5:
				t.r8(a)
			case 6:
				t.r16(a &^ 1)
			case 7:
				if r.n(4) == 0 {
					t.w8(0xa130f1, r.n(4))
				} else {
					t.r16(lo + r.n(64)*2)
				}
			default:
				t.wait(1)
			}
		}
		return t
	}
	{
		r := &rng32{11}
		img := image(hdr{names: "SRAM TEST", code: "GM 00000001-00", io: "J6", ra: ra(0xf8, 0x200001, 0x203fff), region: "JUE"},
			1, sramTable(0x1fff00, 0x204200, r), nil)
		add("sram.bin", cartROM(0x80000, img, 1), 2000)
	}
	{
		r := &rng32{12}
		img := image(hdr{names: "BIG SRAM", code: "GM 00000002-00", io: "J", ra: ra(0xe0, 0x200000, 0x20ffff), region: "U"},
			2, sramTable(0x1ffff0, 0x210100, r), nil)
		add("sram-3mb.bin", cartROM(0x300000, img, 2), 2000)
	}
	{
		r := &rng32{13}
		img := image(hdr{names: "SRAM PAST 64K", code: "GM 00000003-00", ra: ra(0xf8, 0x20f001, 0x21ffff), region: "E"},
			3, sramTable(0x20e000, 0x220100, r), nil)
		add("sram-high.bin", cartROM(0x80000, img, 3), 1500)
	}
	{
		img := image(hdr{names: "NO SRAM", code: "GM 00000004-00", region: "J"}, 4, sramTable(0x1fff00, 0x200100, &rng32{14}), nil)
		add("nosram-jp.bin", cartROM(0x48765, img, 4), 1200) // an odd size: the tail is mirrored
	}

	// EEPROMs in each wiring and size.
	ee := []struct {
		file, names, code string
		ra                []byte
		wire              string
		c                 chip
	}{
		{"ee-sega-header.bin", "EEPROM BY HEADER", "GM 00000010-00", []byte{'R', 'A', 0xe8, 0x40, 0, 0x20, 0, 1, 0, 0x20, 0, 1}, "sega", x24c01},
		{"ee-sega.bin", "WONDER BOY", "GM MK-1215 -00", nil, "sega", x24c01},
		{"ee-ea.bin", "RINGS", "GM T-50176-00", nil, "ea", x24c01},
		{"ee-acclaim16.bin", "JAM", "GM T-081326 00", nil, "acclaim16", c02},
		{"ee-acclaim32-c04.bin", "JAM TE", "GM T-81406-00", nil, "acclaim32", c04},
		{"ee-acclaim32-c16.bin", "QB96", "GM T-081586-00", nil, "acclaim32", c16},
		{"ee-acclaim32-c65.bin", "BIG HURT", "GM T-81476-00", nil, "acclaim32", c65},
		{"ee-cm-c08.bin", "MICRO MACHINES 2", "GM T-120146-50", nil, "codemasters", c08},
		{"ee-cm-c16.bin", "MICRO MACHINES TURBO TOURNAMENT 96", "GM T-120146-00", nil, "codemasters", c16},
		{"ee-cm-c65.bin", "BRIAN LARA CRICKET 96", "GM T-120146-00", nil, "codemasters", c65},
	}
	for i, e := range ee {
		r := &rng32{uint32(100 + i)}
		t := &table{}
		eepromTable(t, wirings[e.wire], e.c, r, 220)
		t.r16(0x380000) // (the Codemasters games are recognised by this address in their code)
		t.wait(2)
		img := image(hdr{names: e.names, code: e.code, ra: e.ra, region: "U"}, 10+i, t, nil)
		add(e.file, cartROM(0x80000, img, uint32(10+i)), 1500)
	}

	// The J-Cart: pads 3 and 4 at $38FFFE, with the TH line.
	{
		t := &table{}
		for k := 0; k < 4; k++ {
			t.w16(0x38fffe, 1)
			t.r16(0x38fffe)
			t.r8(0x38ffff)
			t.w8(0x38ffff, 0)
			t.r16(0x38fffe)
			t.r8(0x38fffe)
			t.wait(1)
		}
		img := image(hdr{names: "PETE SAMPRAS TENNIS", code: "GM T-120056-00", io: "J", region: "E"}, 20, t, nil)
		add("jcart.bin", cartROM(0x100000, img, 20), 1200)
	}

	// 6-button pads: the TH pulses, the time-out, both ports and the
	// version register; and the same with a game that needs 3-button pads.
	padTable := func(r *rng32) *table {
		t := &table{}
		t.w8(0xa10009, 0x40)
		t.w8(0xa1000b, 0x40)
		for k := 0; k < 400; k++ {
			port := 3 + 2*r.n(2)
			for p := r.n(6); p >= 0; p-- {
				t.w8(0xa10000+port, 0x40)
				t.r8(0xa10000 + port)
				t.w8(0xa10000+port, 0)
				t.r8(0xa10000 + port)
			}
			switch r.n(6) {
			case 0:
				t.wait(1)
			case 1:
				t.r16(0xa10000)
				t.r8(0xa10007)
				t.rnd8(0xa10009+2*r.n(3), 0xff, 0)
				t.w8(0xa10009, 0x40)
				t.w8(0xa1000b, 0x40)
			case 2:
				t.r16(0xa10002)
				t.r16(0xa10004)
			}
		}
		return t
	}
	{
		img := image(hdr{names: "SIX BUTTONS", code: "GM 00000030-00", io: "J6", region: "JUE"}, 30, padTable(&rng32{30}), nil)
		add("pad6.bin", cartROM(0x80000, img, 30), 1500)
		img = image(hdr{names: "SUNSET RIDERS", code: "GM 00000031-00", io: "J", region: "U"}, 31, padTable(&rng32{31}), nil)
		add("pad3.bin", cartROM(0x80000, img, 31), 1000)
	}

	// The SSF2 mapper (games over 4 MB): bank writes and reads all over.
	{
		r := &rng32{40}
		t := &table{}
		for k := 0; k < 3000; k++ {
			switch r.n(5) {
			case 0:
				t.rnd8(0xa130f1+2*(1+r.n(7)), 0x3f, 0)
			case 1:
				t.wait(1)
			default:
				t.r16(0x80000 + r.n(0x380000)&^1)
			}
		}
		img := image(hdr{names: "BIG GAME", code: "GM 00000040-00", region: "U"}, 40, t, nil)
		add("ssf2.bin", cartROM(0x500000, img, 40), 1200)
	}

	// Realtec: the boot 8K seen everywhere, then the bank registers. The
	// program is in every 128K bank (and the boot 8K) so it keeps running.
	{
		r := &rng32{50}
		t := &table{}
		for k := 0; k < 300; k++ {
			switch r.n(6) {
			case 0:
				t.rnd8(0x400000+0x2000*r.n(3)+r.n(2), 0xff, 0)
			case 1:
				t.wait(1)
			default:
				t.r16(r.n(0x400000) &^ 1)
			}
		}
		img := image(hdr{names: "REALTEC", code: "GM 00000050-00", region: "U"}, 50, t, nil)
		if len(img) > 0x2000 {
			panic("realtec image too big")
		}
		rom := cartROM(0x80000, nil, 50)
		for _, at := range []int{0, 0x20000, 0x40000, 0x60000, 0x7e000} {
			copy(rom[at:], img)
			if at != 0x7e000 {
				copy(rom[at+0x100:], "SEGB")
			}
		}
		add("realtec.bin", rom, 1200)
	}

	// The VDP: random register, data and control writes (DMA of every kind,
	// interlace, shadow / highlight, window, H interrupts, HV latch...),
	// with interrupts on; NTSC and PAL. Also as an .smd file.
	vdpTable := func(r *rng32) *table {
		t := &table{}
		t.sr(0x2000)
		dma := func(kind int) {
			t.w16(0xc00004, 0x8174) // DMA on
			t.rnd16(0xc00004, 0xff, 0x9300)
			t.rnd16(0xc00004, 0x03, 0x9400)
			t.rnd16(0xc00004, 0xff, 0x9500)
			t.rnd16(0xc00004, 0xff, 0x9600)
			switch kind {
			case 0: // from the ROM
				t.rnd16(0xc00004, 0x03, 0x9700)
			case 1: // from RAM
				t.w16(0xc00004, 0x977f)
			case 2: // fill
				t.w16(0xc00004, 0x9780)
			default: // copy
				t.rnd16(0xc00004, 0x3f, 0x97c0)
			}
			switch r.n(5) {
			case 0: // to CRAM
				t.rnd16(0xc00004, 0x007e, 0xc000)
				t.w16(0xc00004, 0x0080)
			case 1: // to VSRAM
				t.rnd16(0xc00004, 0x004e, 0x4000)
				t.w16(0xc00004, 0x0090)
			default: // to VRAM
				t.rnd16(0xc00004, 0x3fff, 0x4000)
				t.rnd16(0xc00004, 0x0003, 0x0080)
			}
			if kind == 2 {
				t.rnd16(0xc00000, 0xffff, 0)
			}
		}
		// a start: colours, tiles from the ROM, sprites
		for k := 0; k < 8; k++ {
			dma(0)
		}
		for k := 0; k < 6000; k++ {
			switch r.n(20) {
			case 0, 1, 2: // a register
				reg := r.n(24)
				and, or := 0xff, 0
				switch reg {
				case 1:
					and, or = 0xbf, 0x44 // display on, mode 5
				case 2, 3, 4, 5, 13:
					and = 0x7f
				case 7:
					and = 0x3f
				case 12:
					and = 0x8f
				case 15:
					and, or = 0x03, 0x02
				case 17, 18:
					and = 0x9f
				case 19, 20, 21, 22, 23: // (the DMA registers have their own case)
					reg = 10
				}
				t.rnd16(0xc00004, and, 0x8000|reg<<8|or)
			case 3: // any control word pair
				t.rnd16(0xc00004, 0xffff, 0)
				t.rnd16(0xc00004, 0x00f3, 0)
			case 4: // colours
				t.rnd16(0xc00004, 0x007e, 0xc000)
				t.w16(0xc00004, 0)
				for i := r.n(32); i >= 0; i-- {
					t.rnd16(0xc00000, 0x0eee, 0)
				}
			case 5: // vertical scroll
				t.rnd16(0xc00004, 0x004e, 0x4000)
				t.w16(0xc00004, 0x0010)
				for i := r.n(8); i >= 0; i-- {
					t.rnd16(0xc00000, 0x03ff, 0)
				}
			case 6, 7: // VRAM data
				t.rnd16(0xc00004, 0x3fff, 0x4000)
				t.rnd16(0xc00004, 0x0003, 0)
				for i := r.n(16); i >= 0; i-- {
					t.rnd16(0xc00000, 0xffff, 0)
				}
			case 8, 9:
				dma(r.n(4))
			case 10: // reads
				t.rnd16(0xc00004, 0x3fff, 0)
				t.rnd16(0xc00004, 0x0033, 0)
				t.r16(0xc00000)
				t.r16(0xc00000)
				t.r16(0xc00004)
				t.r16(0xc00008)
				t.r8(0xc00009)
			case 11:
				t.rnd8(0xc00011, 0xff, 0) // PSG
				t.r8(0xc00005)
			case 12:
				t.rnd8(0xff0000+r.n(0x10000), 0xff, 0)
				t.rnd16(0xff0000+r.n(0x10000)&^1, 0xffff, 0)
			case 13, 14, 15:
				t.wait(1 + r.n(2))
			case 16:
				t.rnd16(0xa11100, 0x100, 0) // the Z80's bus
				t.r16(0xa11100)
				t.r8(0xa00000 + r.n(0x2000))
				t.r8(0xa04000)
				t.rnd8(0xa04000+r.n(4), 0xff, 0)
				t.w16(0xa11100, 0)
			case 17:
				t.rnd16(0xa11200, 0x100, 0) // the Z80's reset
				t.w16(0xa11200, 0x100)
			case 18: // shadow / highlight and interlace on for a while
				t.rnd16(0xc00004, 0x0e, 0x8c81)
			default: // the 68000's interrupt mask
				t.sr(0x2000 + 0x100*r.n(7))
			}
		}
		return t
	}
	for i, region := range []string{"U", "E", "J"} {
		r := &rng32{uint32(60 + i)}
		img := image(hdr{names: "VDP TEST", code: "GM 00000060-00", io: "J6", region: region}, 60+i, vdpTable(r), nil)
		add("vdp-"+region+".bin", cartROM(0x80000, img, uint32(60+i)), 2500)
	}
	{
		img := image(hdr{names: "SMD FILE", code: "GM 00000070-00", region: "U"}, 70, vdpTable(&rng32{70}), nil)
		rom := cartROM(0x80000, img, 70)
		smd := make([]byte, 512+len(rom))
		smd[8], smd[9] = 0xaa, 0xbb
		for b := 0; b < len(rom); b += 16384 {
			for i := 0; i < 8192; i++ {
				smd[512+b+i] = rom[b+i*2+1]
				smd[512+b+8192+i] = rom[b+i*2]
			}
		}
		add("interleaved.smd", smd, 1000)
	}

	// Virtua Racing: refused by both cores the same way.
	{
		img := image(hdr{names: "VIRTUA RACING", code: "GM MK-1229 -00", region: "U"}, 80, &table{}, nil)
		add("svp.bin", cartROM(0x80000, img, 80), 10)
	}
	return out
}

func writeCartTests(t testing.TB, dir string) []string {
	var paths []string
	for _, c := range cartTests() {
		p := filepath.Join(dir, c.name)
		if err := os.WriteFile(p, c.rom, 0o644); err != nil {
			t.Fatal(err)
		}
		paths = append(paths, p)
	}
	return paths
}

func TestCartsMatchJS(t *testing.T) {
	dir := t.TempDir()
	if d := os.Getenv("RP_MD_CARTS"); d != "" { // keep them, to try in the app
		dir = d
		os.MkdirAll(dir, 0o755)
	}
	paths := writeCartTests(t, dir)
	frames := map[string]int{}
	for _, c := range cartTests() {
		frames[c.name] = c.frames
	}
	// RP_MD_CART_FRAMES=full runs each for its whole length (1000-2500
	// frames); by default 300 frames each (100 with -short).
	if os.Getenv("RP_MD_CART_FRAMES") != "full" {
		n := 300
		if testing.Short() {
			n = 100
		}
		for k := range frames {
			frames[k] = min(frames[k], n)
		}
	}
	for _, p := range paths {
		compareGames(t, []string{p}, frames[filepath.Base(p)])
	}
}
