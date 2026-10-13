package n64

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"os"
	"testing"
)

// A tiny assembler for building test programs.
func rType(rs, rt, rd, sa, fn uint32) uint32 { return rs<<21 | rt<<16 | rd<<11 | sa<<6 | fn }
func iType(op, rs, rt uint32, imm uint16) uint32 {
	return op<<26 | rs<<21 | rt<<16 | uint32(imm)
}

const (
	zero, at, v0, v1, a0, a1, a2, a3 = 0, 1, 2, 3, 4, 5, 6, 7
	t0, t1, t2, t3, t4, t5           = 8, 9, 10, 11, 12, 13
	s0                               = 16
)

type asm struct{ code []uint32 }

func (a *asm) emit(w ...uint32)         { a.code = append(a.code, w...) }
func (a *asm) lui(rt uint32, v uint16)  { a.emit(iType(0x0f, 0, rt, v)) }
func (a *asm) ori(rt, rs uint32, v uint16) {
	a.emit(iType(0x0d, rs, rt, v))
}
func (a *asm) li(rt uint32, v uint32) { a.lui(rt, uint16(v>>16)); a.ori(rt, rt, uint16(v)) }
func (a *asm) sw(rt, base uint32, off int16) {
	a.emit(iType(0x2b, base, rt, uint16(off)))
}
func (a *asm) lw(rt, base uint32, off int16) {
	a.emit(iType(0x23, base, rt, uint16(off)))
}
func (a *asm) sh(rt, base uint32, off int16) {
	a.emit(iType(0x29, base, rt, uint16(off)))
}
func (a *asm) addiu(rt, rs uint32, v int16) { a.emit(iType(0x09, rs, rt, uint16(v))) }
func (a *asm) here() int                    { return len(a.code) }

// bne to an earlier instruction index (with a nop in the delay slot)
func (a *asm) bne(rs, rt uint32, to int) {
	off := to - (a.here() + 1)
	a.emit(iType(0x05, rs, rt, uint16(int16(off))), 0)
}

// testROM builds a small program that sets up a 16-bit 320x240 screen, starts
// the RSP on a counting loop and keeps redrawing the frame buffer with values
// mixed from a frame counter, the Count register, an FPU result and the RSP's
// counter, so the picture depends on exact timing.
func testROM() []byte {
	var a asm
	// VI setup
	a.lui(t0, 0xa440)
	for _, r := range [][2]uint32{{0x00, 2}, {0x04, 0x100000}, {0x08, 320}, {0x0c, 0x200},
		{0x18, 0x20d}, {0x24, 0x006c02ec}, {0x28, 0x002501ff}, {0x30, 0x200}, {0x34, 0x400}} {
		a.li(t1, r[1])
		a.sw(t1, t0, int16(r[0]))
	}
	// RSP program in IMEM: addi r1,r1,1; sw r1,0(r0); vadd v1,v1,v2; j 0; nop
	a.lui(t0, 0xa400)
	for i, w := range []uint32{0x20210001, 0xac010000, 0x4a020850, 0x08000000, 0} {
		a.li(t1, w)
		a.sw(t1, t0, int16(0x1000+4*i))
	}
	a.lui(t0, 0xa404)
	a.ori(t1, zero, 0x125) // clear halt, broke, interrupt, single step
	a.sw(t1, t0, 0x10)
	// FPU on
	a.li(t1, 0x34000000)
	a.emit(0x40800000 | t1<<16 | 12<<11) // mtc0 t1, Status
	loop := a.here()
	a.lui(a0, 0xa010)
	a.ori(a1, zero, 0)
	a.li(a2, 320*240)
	a.lui(t5, 0xa400)
	a.lw(t4, t5, 0) // the RSP's counter
	// f0 = float(s0); f2 = f0*f0; t3 = trunc(f2)
	a.emit(0x44800000 | s0<<16 | 0<<11) // mtc1 s0, f0
	a.emit(0x46800020)                  // cvt.s.w f0, f0
	a.emit(0x46000082)                  // mul.s f2, f0, f0
	a.emit(0x4600110d)                  // trunc.w.s f4, f2
	a.emit(0x44000000 | t3<<16 | 4<<11) // mfc1 t3, f4
	inner := a.here()
	a.emit(rType(a1, s0, t2, 0, 0x21))  // addu t2, a1, s0
	a.emit(0x40000000 | t1<<16 | 9<<11) // mfc0 t1, Count
	a.emit(rType(t2, t1, t2, 0, 0x26))  // xor t2, t2, t1
	a.emit(rType(t2, t3, t2, 0, 0x26))  // xor t2, t2, t3
	a.emit(rType(t2, t4, t2, 0, 0x26))  // xor t2, t2, t4
	a.sh(t2, a0, 0)
	a.addiu(a0, a0, 2)
	a.addiu(a1, a1, 1)
	a.bne(a1, a2, inner)
	a.addiu(s0, s0, 7)
	a.emit(0x08000000|((0x80000400/4+uint32(loop))&0x3ffffff), 0) // j loop

	rom := make([]byte, 0x101000)
	binary.BigEndian.PutUint32(rom[0:], 0x80371240)
	binary.BigEndian.PutUint32(rom[8:], 0x80000400)
	copy(rom[0x20:], "STATE TEST")
	copy(rom[0x3b:], "NTTE")
	for i, w := range a.code {
		binary.BigEndian.PutUint32(rom[0x1000+4*i:], w)
	}
	return rom
}

func frameHash(m *Machine) uint32 {
	b := make([]byte, 0, len(m.Frame)*4)
	for _, p := range m.Frame[:m.FrameW*m.FrameH] {
		b = binary.LittleEndian.AppendUint32(b, p)
	}
	return crc32.ChecksumIEEE(b)
}

func checkStateRoundTrip(t *testing.T, rom []byte, n, k int) {
	m, err := New(rom)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < n; i++ {
		m.RunFrame()
	}
	st := m.SaveState()
	var want []uint32
	for i := 0; i < k; i++ {
		m.RunFrame()
		want = append(want, frameHash(m))
	}
	if err := m.LoadState(st); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < k; i++ {
		m.RunFrame()
		if h := frameHash(m); h != want[i] {
			t.Fatalf("frame %d after loading differs (%08x, want %08x)", i, h, want[i])
		}
	}
	// a state saved again right after loading matches the original
	if err := m.LoadState(st); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(m.SaveState(), st) {
		t.Fatal("saving straight after loading gives a different state")
	}
}

func TestSaveStateRoundTrip(t *testing.T) {
	rom := testROM()
	m, _ := New(rom)
	m.RunFrame()
	m.RunFrame()
	if m.FrameW != 320 || m.FrameH != 240 {
		t.Fatalf("test program's screen is %dx%d", m.FrameW, m.FrameH)
	}
	if m.RSP.r[1] == 0 {
		t.Fatal("the RSP didn't run")
	}
	checkStateRoundTrip(t, rom, 5, 4)
}

// TestSaveStateROM does the same with a real test program when one is
// available (set N64_TEST_ROM).
func TestSaveStateROM(t *testing.T) {
	path := os.Getenv("N64_TEST_ROM")
	if path == "" {
		t.Skip("N64_TEST_ROM not set")
	}
	rom, err := os.ReadFile(path)
	if err != nil {
		t.Skip(err)
	}
	checkStateRoundTrip(t, rom, 30, 10)
}

func TestLoadStateRejectsBadData(t *testing.T) {
	m, _ := New(testROM())
	m.RunFrame()
	st := m.SaveState()
	if err := m.LoadState([]byte("hello")); err == nil {
		t.Error("accepted junk")
	}
	bad := append([]byte(nil), st...)
	bad[len(bad)/2] ^= 0x55
	if err := m.LoadState(bad); err == nil {
		t.Error("accepted a damaged state")
	}
	if err := m.LoadState(st[:len(st)-100]); err == nil {
		t.Error("accepted a cut-short state")
	}
	other := testROM()
	copy(other[0x20:], "OTHER GAME")
	m2, _ := New(other)
	if err := m2.LoadState(st); err == nil {
		t.Error("accepted another game's state")
	}
	if err := m.LoadState(st); err != nil {
		t.Errorf("rejected a good state: %v", err)
	}
}

func TestSavesExportImport(t *testing.T) {
	m, _ := New(testROM())
	s := m.Save
	for i := range s.eeprom {
		s.eeprom[i] = byte(i * 3)
	}
	for i := range s.sram {
		s.sram[i] = byte(i * 5)
	}
	s.paks[2][100] = 42
	blob := s.Export()
	m2, _ := New(testROM())
	if err := m2.Save.Import(blob); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(m2.Save.Export(), blob) {
		t.Fatal("export/import round trip changed the data")
	}
}
