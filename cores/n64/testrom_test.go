package n64

// Small Nintendo 64 test programs, assembled here, for checking the
// controls and the saves: no freely-distributable test ROM reads all four
// controllers or writes every kind of save memory.
//
// The program reads the four controllers through the PIF every frame and
// draws what it got (the buttons, the stick and whether the controller is
// plugged in) as rows of white and black squares, and writes save memory:
// the EEPROM and a Controller Pak every few frames, and either the SRAM or
// the FlashRAM.

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

type asm struct {
	code   []uint32
	labels map[string]int
	fix    []struct {
		at   int
		name string
		jump bool
	}
}

const (
	zr, at, v0, v1, a0, a1, a2, a3 = 0, 1, 2, 3, 4, 5, 6, 7
	t0, t1, t2, t3, t4, t5, t6, t7 = 8, 9, 10, 11, 12, 13, 14, 15
	s0, s1, s2, s3, s4, s5, s6, s7 = 16, 17, 18, 19, 20, 21, 22, 23
	t8, t9                         = 24, 25
)

const codeBase = 0x80000400

func (a *asm) emit(w ...uint32) { a.code = append(a.code, w...) }
func (a *asm) i(op, rs, rt uint32, imm uint16) {
	a.emit(op<<26 | rs<<21 | rt<<16 | uint32(imm))
}
func (a *asm) r(rs, rt, rd, sa, fn uint32) { a.emit(rs<<21 | rt<<16 | rd<<11 | sa<<6 | fn) }

func (a *asm) lui(rt uint32, v uint16)        { a.i(0x0f, 0, rt, v) }
func (a *asm) ori(rt, rs uint32, v uint16)    { a.i(0x0d, rs, rt, v) }
func (a *asm) andi(rt, rs uint32, v uint16)   { a.i(0x0c, rs, rt, v) }
func (a *asm) addiu(rt, rs uint32, v int16)   { a.i(0x09, rs, rt, uint16(v)) }
func (a *asm) sltiu(rt, rs uint32, v int16)   { a.i(0x0b, rs, rt, uint16(v)) }
func (a *asm) lw(rt, base uint32, off int16)  { a.i(0x23, base, rt, uint16(off)) }
func (a *asm) lbu(rt, base uint32, off int16) { a.i(0x24, base, rt, uint16(off)) }
func (a *asm) sb(rt, base uint32, off int16)  { a.i(0x28, base, rt, uint16(off)) }
func (a *asm) sh(rt, base uint32, off int16)  { a.i(0x29, base, rt, uint16(off)) }
func (a *asm) sw(rt, base uint32, off int16)  { a.i(0x2b, base, rt, uint16(off)) }
func (a *asm) addu(rd, rs, rt uint32)         { a.r(rs, rt, rd, 0, 0x21) }
func (a *asm) or(rd, rs, rt uint32)           { a.r(rs, rt, rd, 0, 0x25) }
func (a *asm) and(rd, rs, rt uint32)          { a.r(rs, rt, rd, 0, 0x24) }
func (a *asm) sll(rd, rt, sa uint32)          { a.r(0, rt, rd, sa, 0x00) }
func (a *asm) srl(rd, rt, sa uint32)          { a.r(0, rt, rd, sa, 0x02) }
func (a *asm) srlv(rd, rt, rs uint32)         { a.r(rs, rt, rd, 0, 0x06) }
func (a *asm) li(rt uint32, v uint32)         { a.lui(rt, uint16(v>>16)); a.ori(rt, rt, uint16(v)) }
func (a *asm) label(n string)                 { a.labels[n] = len(a.code) }
func (a *asm) branch(op, rs, rt uint32, to string) {
	a.fix = append(a.fix, struct {
		at   int
		name string
		jump bool
	}{len(a.code), to, false})
	a.i(op, rs, rt, 0)
	a.emit(0) // delay slot
}
func (a *asm) beq(rs, rt uint32, to string) { a.branch(0x04, rs, rt, to) }
func (a *asm) bne(rs, rt uint32, to string) { a.branch(0x05, rs, rt, to) }
func (a *asm) j(to string) {
	a.fix = append(a.fix, struct {
		at   int
		name string
		jump bool
	}{len(a.code), to, true})
	a.emit(0x02 << 26)
	a.emit(0)
}

func (a *asm) link() {
	for _, f := range a.fix {
		to, ok := a.labels[f.name]
		if !ok {
			panic("no label " + f.name)
		}
		if f.jump {
			a.code[f.at] |= ((codeBase + uint32(to)*4) >> 2) & 0x3ffffff
		} else {
			a.code[f.at] |= uint32(uint16(int16(to - (f.at + 1))))
		}
	}
}

// Where things are in RDRAM (physical addresses).
const (
	tmplPads = 0x8400   // PIF commands: read the 4 controllers and write an EEPROM block (from the ROM)
	tmplPak  = 0x8480   // PIF commands: write a Controller Pak block (from the ROM)
	results  = 0x200000 // what the PIF answered
	fbuf     = 0x100000 // the 16-bit 320x240 picture
)

// padTestROM builds the program. flash: write FlashRAM instead of SRAM.
// id is the 4-character game code (the save sizes and the TV system follow it).
func padTestROM(id string, flash bool) []byte {
	a := &asm{labels: map[string]int{}}
	// VI: 16-bit 320x240 at fbuf
	a.lui(t0, 0xa440)
	for _, r := range [][2]uint32{{0x00, 2}, {0x04, fbuf}, {0x08, 320}, {0x0c, 0x200},
		{0x18, 0x20d}, {0x24, 0x006c02ec}, {0x28, 0x002501ff}, {0x30, 0x200}, {0x34, 0x400}} {
		a.li(t1, r[1])
		a.sw(t1, t0, int16(r[0]))
	}
	a.li(s1, 0xa0000000|results)
	a.li(s2, 0xa0000000|tmplPads)
	a.li(s3, 0xa0000000|tmplPak)
	a.li(s4, 0xa0000000|fbuf)
	a.lui(s5, 0xa480) // SI
	a.lui(s6, 0xa440) // VI
	a.lui(s7, 0xa800) // cartridge save memory
	a.ori(s0, zr, 0)  // frame counter

	a.label("frame")
	// wait for the next frame: the scan line goes past 0x200, then back below it
	a.label("w1")
	a.lw(t0, s6, 0x10)
	a.sltiu(t1, t0, 0x200)
	a.bne(t1, zr, "w1")
	a.label("w2")
	a.lw(t0, s6, 0x10)
	a.sltiu(t1, t0, 0x200)
	a.beq(t1, zr, "w2")

	// EEPROM block (frame & 63) = controller 1's last answer + the frame counter
	a.andi(t0, s0, 63)
	a.sb(t0, s2, 31)
	for k := 0; k < 4; k++ {
		a.lbu(t0, s1, int16(3+k))
		a.sb(t0, s2, int16(32+k))
	}
	for k := 0; k < 4; k++ {
		a.srl(t0, s0, uint32(24-8*k))
		a.sb(t0, s2, int16(36+k))
	}
	// run the commands and fetch the answers
	a.li(t0, tmplPads)
	a.sw(t0, s5, 0x00)
	a.li(t0, 0x1fc007c0)
	a.sw(t0, s5, 0x10)
	a.li(t0, results)
	a.sw(t0, s5, 0x00)
	a.li(t0, 0x1fc007c0)
	a.sw(t0, s5, 0x04)

	// every 8th frame: a Controller Pak block, address 0x600 + 32 * ((frame >> 3) & 15)
	a.andi(t0, s0, 7)
	a.bne(t0, zr, "nopak")
	a.srl(t0, s0, 3)
	a.andi(t0, t0, 15)
	a.sll(t0, t0, 5)
	a.addiu(t0, t0, 0x600)
	a.srl(t1, t0, 8)
	a.sb(t1, s3, 3)
	a.sb(t0, s3, 4)
	a.ori(t2, zr, 0)
	a.label("pakfill")
	a.addu(t3, s0, t2)
	a.lbu(t4, s1, 4) // with controller 1's buttons
	a.addu(t3, t3, t4)
	a.addu(t5, s3, t2)
	a.sb(t3, t5, 5)
	a.addiu(t2, t2, 1)
	a.sltiu(t1, t2, 32)
	a.bne(t1, zr, "pakfill")
	a.li(t0, tmplPak)
	a.sw(t0, s5, 0x00)
	a.li(t0, 0x1fc007c0)
	a.sw(t0, s5, 0x10)
	a.label("nopak")

	// controller 1's answer as a word
	a.lbu(t6, s1, 3)
	for k := 4; k < 7; k++ {
		a.sll(t6, t6, 8)
		a.lbu(t0, s1, int16(k))
		a.or(t6, t6, t0)
	}
	if !flash {
		// SRAM word (frame & 0x1fff) = that word
		a.andi(t0, s0, 0x1fff)
		a.sll(t0, t0, 2)
		a.addu(t0, t0, s7)
		a.sw(t6, t0, 0)
	} else {
		// every 16th frame program a FlashRAM page, every 256th erase a sector first
		a.andi(t0, s0, 15)
		a.bne(t0, zr, "noflash")
		a.andi(t0, s0, 255)
		a.bne(t0, zr, "noerase")
		a.srl(t0, s0, 8)
		a.andi(t0, t0, 7)
		a.sll(t0, t0, 7)
		a.lui(t1, 0x4b00)
		a.or(t1, t1, t0)
		a.lui(t2, 0xa801)
		a.sw(t1, t2, 0)
		a.lui(t1, 0x7800)
		a.sw(t1, t2, 0)
		a.label("noerase")
		a.lui(t2, 0xa801)
		a.lui(t1, 0xb400)
		a.sw(t1, t2, 0)
		a.ori(t3, zr, 0)
		a.label("flfill")
		a.addu(t1, t6, t3)
		a.sll(t4, t3, 2)
		a.addu(t4, t4, s7)
		a.sw(t1, t4, 0)
		a.addiu(t3, t3, 1)
		a.sltiu(t1, t3, 32)
		a.bne(t1, zr, "flfill")
		a.srl(t0, s0, 4)
		a.andi(t0, t0, 0x3ff)
		a.lui(t1, 0xa500)
		a.or(t1, t1, t0)
		a.sw(t1, t2, 0)
		a.lui(t1, 0xf000)
		a.sw(t1, t2, 0)
		a.label("noflash")
	}

	// draw: row p (0-3) = controller p's 4 answer bytes and its "rx" byte,
	// row 4 = the frame counter; 40 squares of 6x6 pixels each
	a.ori(t7, zr, 0) // row
	a.label("row")
	// t8 = the 5 bytes' first 4 as a word, t9 = the fifth
	// t1 = row * 7 (row*8 - row)
	a.sll(t1, t7, 3)
	a.r(t1, t7, t1, 0, 0x23) // subu t1, t1, t7
	a.addu(t1, t1, s1)
	a.ori(t9, zr, 4)
	a.bne(t7, t9, "notcounter")
	a.or(t8, s0, zr)
	a.ori(t9, zr, 0)
	a.j("havebits")
	a.label("notcounter")
	a.lbu(t8, t1, 3)
	for k := 4; k < 7; k++ {
		a.sll(t8, t8, 8)
		a.lbu(t0, t1, int16(k))
		a.or(t8, t8, t0)
	}
	a.lbu(t9, t1, 1)
	a.label("havebits")
	// square b (0-39): bit 31-b of t8 for b < 32, then bit 7-(b-32) of t9
	a.ori(t2, zr, 0) // b
	a.label("square")
	a.sltiu(t0, t2, 32)
	a.beq(t0, zr, "low")
	a.ori(t0, zr, 31)
	a.r(t0, t2, t0, 0, 0x23) // subu t0, t0, t2
	a.srlv(t3, t8, t0)
	a.j("gotbit")
	a.label("low")
	a.ori(t0, zr, 39)
	a.r(t0, t2, t0, 0, 0x23)
	a.srlv(t3, t9, t0)
	a.label("gotbit")
	a.andi(t3, t3, 1)
	a.ori(t4, zr, 0x0001) // black
	a.beq(t3, zr, "black")
	a.ori(t4, zr, 0xffff) // white
	a.label("black")
	// top-left pixel: y = 16 + row*10, x = 8 + b*7
	a.sll(t0, t7, 3)
	a.sll(t1, t7, 1)
	a.addu(t0, t0, t1)
	a.addiu(t0, t0, 16) // y
	a.sll(t1, t0, 8)
	a.sll(t0, t0, 6)
	a.addu(t0, t0, t1) // y*320
	a.sll(t1, t2, 3)
	a.r(t1, t2, t1, 0, 0x23) // b*7
	a.addu(t0, t0, t1)
	a.addiu(t0, t0, 8)
	a.sll(t0, t0, 1)
	a.addu(t0, t0, s4) // pixel address
	a.ori(t5, zr, 0)   // dy
	a.label("dy")
	for dx := 0; dx < 6; dx++ {
		a.sh(t4, t0, int16(dx*2))
	}
	a.addiu(t0, t0, 640)
	a.addiu(t5, t5, 1)
	a.sltiu(t1, t5, 6)
	a.bne(t1, zr, "dy")
	a.addiu(t2, t2, 1)
	a.sltiu(t0, t2, 40)
	a.bne(t0, zr, "square")
	a.addiu(t7, t7, 1)
	a.sltiu(t0, t7, 5)
	a.bne(t0, zr, "row")

	a.addiu(s0, s0, 1)
	a.j("frame")
	a.link()

	rom := make([]byte, 0x101000)
	binary.BigEndian.PutUint32(rom[0:], 0x80371240)
	binary.BigEndian.PutUint32(rom[8:], codeBase)
	copy(rom[0x20:], "RP PAD TEST")
	copy(rom[0x3b:], id)
	for i, w := range a.code {
		binary.BigEndian.PutUint32(rom[0x1000+4*i:], w)
	}
	if 4*len(a.code) > tmplPads-0x400 {
		panic("the test program is too long")
	}
	// PIF commands: 4 controllers "read" (tx 1, rx 4), then EEPROM "write" (tx 10, rx 1)
	p := rom[0x1000+tmplPads-0x400:]
	for ch := 0; ch < 4; ch++ {
		copy(p[ch*7:], []byte{0x01, 0x04, 0x01, 0, 0, 0, 0})
	}
	copy(p[28:], []byte{0x0a, 0x01, 0x05})
	p[41] = 0xfe
	p[63] = 0x01
	// Controller Pak "write" on channel 0 (tx 35, rx 1)
	q := rom[0x1000+tmplPak-0x400:]
	copy(q, []byte{0x23, 0x01, 0x03})
	q[38] = 0xfe
	q[63] = 0x01
	return rom
}

// N64_WRITE_TEST_ROMS=<dir> writes the test programs there (for the
// comparison with the web player and for trying in the app).
func TestWriteTestROMs(t *testing.T) {
	dir := os.Getenv("N64_WRITE_TEST_ROMS")
	if dir == "" {
		t.Skip("N64_WRITE_TEST_ROMS not set")
	}
	for name, rom := range map[string][]byte{
		"padtest-sram.z64":  padTestROM("NTTE", false),
		"padtest-flash.z64": padTestROM("NB7P", true), // 16 Kbit EEPROM, FlashRAM, PAL
	} {
		if err := os.WriteFile(filepath.Join(dir, name), rom, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}
