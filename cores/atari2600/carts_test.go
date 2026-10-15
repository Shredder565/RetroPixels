package atari2600

import "testing"

// blocks is a ROM whose every byte is its 1K block number.
func blocks(size int) []byte {
	r := make([]byte, size)
	for i := range r {
		r[i] = byte(i >> 10)
	}
	return r
}

func mkCart(t *testing.T, rom []byte, name string) cart {
	t.Helper()
	c, err := createCart(rom, name, nil)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestCarts(t *testing.T) {
	eq := func(name string, got, want int) {
		t.Helper()
		if got != want {
			t.Errorf("%s: got %d, want %d", name, got, want)
		}
	}
	rd := func(c cart, a int) int { return int(c.read(a, nil)) }
	wr := func(c cart, a, v int) { c.write(a, uint8(v), nil) }

	if typ, _ := detectType(blocks(8192), "Game (F8SC)"); typ != "F8SC" {
		t.Errorf("tag F8SC: %s", typ)
	}
	if typ, _ := detectType(blocks(65536), "Game [3E+]"); typ != "3E+" {
		t.Errorf("tag 3E+: %s", typ)
	}
	if typ, _ := detectType(blocks(32768), "x (DPCP)"); typ != "DPC+" {
		t.Errorf("tag DPC+: %s", typ)
	}

	// F8: start bank 1; hotspots
	c := mkCart(t, blocks(8192), "(F8)")
	eq("F8 start", rd(c, 0x1000), 4)
	rd(c, 0x1ff8)
	eq("F8 b0", rd(c, 0x1000), 0)
	wr(c, 0x1ff9, 0)
	eq("F8 b1", rd(c, 0x1c00), 7)
	// F6SC RAM
	c = mkCart(t, blocks(16384), "(F6SC)")
	wr(c, 0x1005, 0x5a)
	eq("SC ram", rd(c, 0x1085), 0x5a)
	rd(c, 0x1ff8)
	eq("F6 b2", rd(c, 0x1400), 9)
	// FA: 256 RAM
	c = mkCart(t, blocks(12288), "(FA)")
	wr(c, 0x10ff, 0x33)
	eq("FA ram", rd(c, 0x11ff), 0x33)
	rd(c, 0x1ff9)
	eq("FA b1", rd(c, 0x1800), 6)
	// FA2: 28K, 7 banks from $1FF5, flash save / load
	c = mkCart(t, blocks(28*1024), "")
	if c.kind() != "FA2" {
		t.Errorf("FA2 type %s", c.kind())
	}
	rd(c, 0x1ffb)
	eq("FA2 b6", rd(c, 0x1400), 25)
	wr(c, 0x1000, 0x77)
	wr(c, 0x10ff, 2)
	rd(c, 0x1ff4)
	eq("FA2 saved", int(c.sram()[0]), 0x77)
	wr(c, 0x1000, 0)
	wr(c, 0x10ff, 1)
	rd(c, 0x1ff4)
	eq("FA2 loaded", rd(c, 0x1100), 0x77)
	eq("FA2 done", rd(c, 0x11ff), 0)
	// FA2 battery save handed in at start
	c, _ = createCart(blocks(24*1024), "", []byte{9, 8, 7})
	wr(c, 0x10ff, 1)
	rd(c, 0x1ff4)
	eq("FA2 restored", rd(c, 0x1102), 7)
	// EF / DF / BF
	c = mkCart(t, blocks(65536), "(EF)")
	rd(c, 0x1fe3)
	eq("EF b3", rd(c, 0x1000), 12)
	c = mkCart(t, blocks(131072), "(DF)")
	rd(c, 0x1fdf)
	eq("DF b31", rd(c, 0x1000), 124)
	rd(c, 0x1fc1)
	eq("DF b1", rd(c, 0x1000), 4)
	c = mkCart(t, blocks(262144), "(BFSC)")
	rd(c, 0x1fbf)
	eq("BF b63", rd(c, 0x1200), 252)
	wr(c, 0x1010, 9)
	eq("BFSC ram", rd(c, 0x1090), 9)
	// F0 steps
	c = mkCart(t, blocks(65536), "(F0)")
	b0 := rd(c, 0x1000)
	rd(c, 0x1ff0)
	eq("F0 step", rd(c, 0x1000), ((b0/4+1)%16)*4)
	// E0
	c = mkCart(t, blocks(8192), "(E0)")
	rd(c, 0x1fe2)
	rd(c, 0x1fed)
	eq("E0 s0", rd(c, 0x1000), 2)
	eq("E0 s1", rd(c, 0x1400), 5)
	eq("E0 s3", rd(c, 0x1c00), 7)
	// E7
	c = mkCart(t, blocks(16384), "(E7)")
	rd(c, 0x1fe3)
	eq("E7 rom", rd(c, 0x1000), 6)
	rd(c, 0x1fe7)
	wr(c, 0x1010, 0x42)
	eq("E7 1k ram", rd(c, 0x1410), 0x42)
	rd(c, 0x1fe9)
	wr(c, 0x1801, 0x24)
	eq("E7 256 ram", rd(c, 0x1901), 0x24)
	rd(c, 0x1fe8)
	if rd(c, 0x1901) == 0x24 {
		t.Error("E7 256 other bank")
	}
	eq("E7 fixed", rd(c, 0x1fff), 15)
	// 3F
	c = mkCart(t, blocks(8192), "(3F)")
	h := c.(tiaHooker)
	h.tiaWrite(0x3f, 2)
	eq("3F b2", rd(c, 0x1000), 4)
	eq("3F fixed", rd(c, 0x1800), 6)
	h.tiaWrite(0x10, 1)
	eq("3F any", rd(c, 0x1000), 2)
	// 3E
	c = mkCart(t, blocks(16384), "(3E)")
	h = c.(tiaHooker)
	h.tiaWrite(0x3e, 3)
	wr(c, 0x1400, 0x99)
	eq("3E ram", rd(c, 0x1000), 0x99)
	h.tiaWrite(0x3f, 1)
	eq("3E rom", rd(c, 0x1000), 2)
	h.tiaWrite(0x3e, 3)
	eq("3E keep", rd(c, 0x1000), 0x99)
	// 3E+
	c = mkCart(t, blocks(65536), "(3E+)")
	h = c.(tiaHooker)
	eq("3E+ boot", rd(c, 0x1ffc), 0)
	h.tiaWrite(0x3f, 1<<6|9)
	eq("3E+ seg1", rd(c, 0x1400), 9)
	h.tiaWrite(0x3e, 2<<6|5)
	wr(c, 0x1a10, 0x66)
	eq("3E+ ram", rd(c, 0x1810), 0x66)
	eq("3E+ seg0", rd(c, 0x1000), 0)
	// plant code at the start of the last 4K (or of the file) with the reset vector on it
	plant := func(r []byte, home int, code ...byte) []byte {
		end := len(r)
		blk := min(4096, end)
		copy(r[end-blk:], code)
		r[end-4], r[end-3] = byte(home), byte(home>>8)
		return r
	}
	// CV
	r := plant(blocks(2048), 0xf800, 0x9d, 0xff, 0xf3, 0x4c, 0x00, 0xf8)
	c = mkCart(t, r, "")
	if c.kind() != "CV" {
		t.Errorf("CV type %s", c.kind())
	}
	wr(c, 0x1405, 0x12)
	eq("CV ram", rd(c, 0x1005), 0x12)
	eq("CV rom", rd(c, 0x1800), 0x9d)
	r = plant(blocks(2048), 0xf800, 0x9d, 0xff, 0xf2, 0x4c, 0x00, 0xf8)
	if c := mkCart(t, r, ""); c.kind() != "2K" {
		t.Errorf("2K with stores below $1400 detected as %s", c.kind())
	}
	// 0840 / UA / SB / X07 / MDM via bus accesses
	acc := func(c cart, a int) { c.(snooper).access(a) }
	r = plant(blocks(8192), 0xf000, 0xad, 0x40, 0x08, 0xad, 0x00, 0x08, 0x60)
	c = mkCart(t, r, "")
	if c.kind() != "0840" {
		t.Errorf("0840 type %s", c.kind())
	}
	acc(c, 0x0840)
	eq("0840 b1", rd(c, 0x1400), 5)
	acc(c, 0x0800)
	eq("0840 b0", rd(c, 0x1400), 1)
	r = plant(blocks(8192), 0xf000, 0x8d, 0x40, 0x02, 0x2c, 0x20, 0x02, 0x60)
	c = mkCart(t, r, "")
	if c.kind() != "UA" {
		t.Errorf("UA type %s", c.kind())
	}
	acc(c, 0x0240)
	eq("UA b1", rd(c, 0x1400), 5)
	acc(c, 0x0220)
	eq("UA b0", rd(c, 0x1400), 1)
	c = mkCart(t, blocks(131072), "")
	if c.kind() != "SB" {
		t.Errorf("SB type %s", c.kind())
	}
	eq("SB start", rd(c, 0x1000), 124)
	acc(c, 0x0805)
	eq("SB b5", rd(c, 0x1000), 20)
	c = mkCart(t, blocks(65536), "(X07)")
	acc(c, 0x08ad)
	eq("X07 b10", rd(c, 0x1000), 40)
	acc(c, 0x080d|14<<4)
	acc(c, 0x0040)
	eq("X07 b15", rd(c, 0x1000), 60)
	acc(c, 0x0000)
	eq("X07 b14", rd(c, 0x1000), 56)
	r = blocks(65536)
	copy(r[0xff8:], "MDMC")
	c = mkCart(t, r, "")
	if c.kind() != "MDM" {
		t.Errorf("MDM type %s", c.kind())
	}
	acc(c, 0x0803)
	eq("MDM b3", rd(c, 0x1000), 12)
	acc(c, 0x0885)
	eq("MDM lock", rd(c, 0x1000), 20)
	acc(c, 0x0801)
	eq("MDM locked", rd(c, 0x1000), 20)

	// unsupported and odd files
	for _, tc := range []struct {
		rom  []byte
		name string
	}{{blocks(6144), ""}, {blocks(32768), "(CDFJ)"}, {blocks(50000), ""}, {blocks(4096), "(AR)"}} {
		if _, err := createCart(tc.rom, tc.name, nil); err == nil {
			t.Errorf("%d bytes %q: no error", len(tc.rom), tc.name)
		}
	}
}
