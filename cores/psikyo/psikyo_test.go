package psikyo

import (
	"bytes"
	"encoding/binary"
	"testing"

	"retropixels/cores/arcade"
)

// ------------------------------------------------------------ EEPROM

// eeClock sends bits to the EEPROM, one rising clock edge each.
func eeClock(e *eeprom, now int64, bits ...int) {
	for _, b := range bits {
		e.write(true, false, b != 0, now)
		e.write(true, true, b != 0, now)
	}
}

func eeCommand(e *eeprom, now int64, op, addr int) {
	bits := []int{1, op >> 1 & 1, op & 1}
	for i := eeAddrBits - 1; i >= 0; i-- {
		bits = append(bits, addr>>i&1)
	}
	eeClock(e, now, bits...)
}

func eeDeselect(e *eeprom, now int64) { e.write(false, false, false, now) }

func eeReadByte(e *eeprom, now int64) uint8 {
	v := uint8(0)
	for i := 0; i < 8; i++ {
		eeClock(e, now, 0)
		v <<= 1
		if e.dataOut(now) {
			v |= 1
		}
	}
	return v
}

func newEEPROM() *eeprom {
	e := &eeprom{}
	for i := range e.mem {
		e.mem[i] = uint8(i)
	}
	e.reset()
	return e
}

func TestEEPROMRead(t *testing.T) {
	e := newEEPROM()
	eeCommand(e, 0, 2, 0x40)
	if e.dataOut(0) {
		t.Fatal("no dummy 0 before the data")
	}
	if a, b := eeReadByte(e, 0), eeReadByte(e, 0); a != 0x40 || b != 0x41 {
		t.Fatalf("read %02x %02x, want 40 41 (sequential)", a, b)
	}
	eeDeselect(e, 0)
}

func TestEEPROMWriteNeedsEnable(t *testing.T) {
	e := newEEPROM()
	eeCommand(e, 0, 1, 5)
	eeClock(e, 0, 1, 0, 1, 0, 0, 1, 0, 1)
	eeDeselect(e, 0)
	if e.mem[5] != 5 {
		t.Fatal("written without EWEN")
	}
	eeCommand(e, 0, 0, 3<<(eeAddrBits-2)) // EWEN
	eeDeselect(e, 0)
	eeCommand(e, 0, 1, 5)
	eeClock(e, 0, 1, 0, 1, 0, 0, 1, 0, 1)
	eeDeselect(e, 100)
	if e.mem[5] != 0xa5 {
		t.Fatalf("mem[5] = %02x", e.mem[5])
	}
	// busy: DO low until the write time has passed
	e.write(true, false, false, 101)
	if e.dataOut(101) {
		t.Fatal("not busy after a write")
	}
	if !e.dataOut(100 + eeWriteTime) {
		t.Fatal("still busy after the write time")
	}
	eeDeselect(e, 100+eeWriteTime)
	now := int64(200 + eeWriteTime)
	eeCommand(e, now, 3, 6) // ERASE
	eeDeselect(e, now)
	if e.mem[6] != 0xff {
		t.Fatalf("erase left %02x", e.mem[6])
	}
	now += eeWriteTime
	eeCommand(e, now, 0, 0) // EWDS
	eeDeselect(e, now)
	eeCommand(e, now, 1, 7)
	eeClock(e, now, 0, 0, 0, 0, 0, 0, 0, 0)
	eeDeselect(e, now)
	if e.mem[7] != 7 {
		t.Fatal("written after EWDS")
	}
}

func TestEEPROMWriteAllEraseAll(t *testing.T) {
	e := newEEPROM()
	eeCommand(e, 0, 0, 3<<(eeAddrBits-2))
	eeDeselect(e, 0)
	eeCommand(e, 0, 0, 1<<(eeAddrBits-2)) // WRAL
	eeClock(e, 0, 0, 1, 0, 1, 0, 1, 0, 1)
	eeDeselect(e, 0)
	for i, v := range e.mem {
		if v != 0x55 {
			t.Fatalf("WRAL: mem[%d] = %02x", i, v)
		}
	}
	now := int64(eeWriteTime)
	eeCommand(e, now, 0, 2<<(eeAddrBits-2)) // ERAL
	eeDeselect(e, now)
	for i, v := range e.mem {
		if v != 0xff {
			t.Fatalf("ERAL: mem[%d] = %02x", i, v)
		}
	}
}

func TestDefaultEEPROM(t *testing.T) {
	rom := make([]byte, 0x100000)
	// code from 0 to 0x30000 (start 0x10000 + length 0x20000)
	binary.BigEndian.PutUint32(rom[0x2e154:], 0x10000)
	binary.BigEndian.PutUint32(rom[0x2e15c:], 0x20000)
	binary.BigEndian.PutUint32(rom[0x100:], 0x00012345)
	binary.BigEndian.PutUint32(rom[0x2fffc:], 0x00010001)
	binary.BigEndian.PutUint32(rom[0x30000:], 0x77777777) // past the end
	var sum uint32 = 0x10000 + 0x20000 + 0x12345 + 0x10001
	e := defaultEEPROM(rom)
	if got := uint32(e[0xec])<<8 | uint32(e[0xed]); got != sum&0xffff {
		t.Fatalf("check word %04x, want %04x", got, sum&0xffff)
	}
	for i, v := range e {
		if i != 0xec && i != 0xed && v != 0xff {
			t.Fatalf("byte %02x = %02x", i, v)
		}
	}
}

// ------------------------------------------------------------ video

const testTile = gfxBase / 128 // the first 4-bit tile in the graphics ROMs

// testVideo has tile testTile with pen (x+y)&15 in every pixel but pen 0 at
// (0,0), tiles testTile+1 to +7 all pen 1, and colours 0x10000*colour + pen.
func testVideo() *video {
	gfx := make([]byte, 0x10000)
	for y := 0; y < 16; y++ {
		for x := 0; x < 16; x += 2 {
			gfx[y*8+x/2] = uint8((x+y)&15)<<4 | uint8((x+1+y)&15)
		}
	}
	for i := 128; i < 128*8; i++ {
		gfx[i] = 0x11
	}
	// an 8-bit tile: pens 0xc0+y*4 (fading pens from 0xc0)
	for y := 0; y < 16; y++ {
		for x := 0; x < 16; x++ {
			gfx[0x1000+y*16+x] = uint8(0xc0 + y*4)
		}
	}
	v := newVideo(gfx)
	for i := 0; i < 0x1400; i++ {
		binary.BigEndian.PutUint32(v.pal[i*4:], uint32(i)<<8)
	}
	return v
}

func (v *video) setReg(n int, val uint32) { binary.BigEndian.PutUint32(v.regs[n*4:], val) }
func (v *video) put32(a int, val uint32)  { binary.BigEndian.PutUint32(v.spr[a:], val) }

// sprite n at x, y: w x h tiles, zoom z (0x3f = 1:1), colour col, class.
func (v *video) sprite(n, x, y, w, h, z, col, class int, tile uint32, flipX bool) {
	a := n * 16
	v.put32(a, uint32(y&0x3ff)<<16|uint32(x&0x3ff))
	fx := uint32(0)
	if flipX {
		fx = 0x8000
	}
	v.put32(a+4, uint32(h-1)<<24|uint32(z)<<16|fx|uint32(class)<<12|uint32(w-1)<<8|uint32(z))
	v.put32(a+8, uint32(col)<<24|tile)
}

func (v *video) list(ns ...int) {
	for i, n := range ns {
		e := uint16(n)
		if i == len(ns)-1 {
			e |= 0x4000
		}
		binary.BigEndian.PutUint16(v.spr[0x3800+i*2:], e)
	}
}

func (v *video) px(x, y int) uint32 { return v.screen[y*ScreenW+x] }

func TestSprite(t *testing.T) {
	v := testVideo()
	v.setReg(2, 0x10000000) // class 0 at priority 1
	v.sprite(0, 100, 50, 1, 1, 0x3f, 2, 0, testTile, false)
	v.list(0)
	v.latch()
	v.render()
	if v.px(100, 50) != 0 {
		t.Errorf("pen 0 drawn: %06x", v.px(100, 50))
	}
	for _, p := range [][2]int{{1, 0}, {5, 3}, {15, 15}} {
		want := uint32(2*16+(p[0]+p[1])&15) << 0
		if got := v.px(100+p[0], 50+p[1]); got != want {
			t.Errorf("(%d,%d): %06x want %06x", p[0], p[1], got, want)
		}
	}
	if v.px(116, 50) != 0 || v.px(99, 51) != 0 {
		t.Error("drawn outside the tile")
	}
	// flipped: pixel 15 of the row shows source pixel 0
	v.sprite(0, 100, 50, 1, 1, 0x3f, 2, 0, testTile, true)
	v.latch()
	v.render()
	if got := v.px(114, 51); got != 2*16+(1+1) {
		t.Errorf("flip x: %06x", got)
	}
}

func TestSpriteNeedsLatch(t *testing.T) {
	// sprites show what was latched at the last vblank
	v := testVideo()
	v.setReg(2, 0x10000000)
	v.sprite(0, 10, 10, 1, 1, 0x3f, 1, 0, testTile+1, false)
	v.list(0)
	v.render()
	if v.px(12, 12) != 0 {
		t.Fatal("drawn before the latch")
	}
	v.latch()
	v.render()
	if v.px(12, 12) != 17 {
		t.Fatalf("%06x", v.px(12, 12))
	}
}

func TestSpriteSizeAndZoom(t *testing.T) {
	v := testVideo()
	v.setReg(2, 0x10000000)
	v.sprite(0, 0, 0, 2, 2, 0x3f, 1, 0, testTile+1, false)     // 32x32 of pen 1
	v.sprite(1, 100, 100, 2, 1, 0x1f, 3, 0, testTile+1, false) // 2x1 tiles at half size: 16x8
	v.list(0, 1)
	v.latch()
	v.render()
	if v.px(31, 31) != 17 || v.px(32, 31) != 0 {
		t.Error("2x2 sprite size")
	}
	if v.px(115, 107) != 49 || v.px(116, 100) != 0 || v.px(100, 108) != 0 {
		t.Errorf("half zoom: %06x %06x %06x", v.px(115, 107), v.px(116, 100), v.px(100, 108))
	}
}

func TestSpriteOrderAndClasses(t *testing.T) {
	v := testVideo()
	// class 0 priority 2, class 1 priority 1
	v.setReg(2, 0x21000000)
	v.sprite(0, 10, 10, 1, 1, 0x3f, 1, 0, testTile+1, false)
	v.sprite(1, 10, 10, 1, 1, 0x3f, 2, 1, testTile+1, false)
	v.list(0, 1)
	v.latch()
	v.render()
	// later sprites win whatever the classes
	if v.px(12, 12) != 2*16+1 {
		t.Fatalf("%06x", v.px(12, 12))
	}
}

// layer sets layer 0 up: register bank 8, tilemap bank 9, priority pri.
func (v *video) layer(pri int, mode uint32, scroll uint32, deep bool) {
	v.setReg(6, 8<<24)
	bits := uint32(8)
	if deep {
		bits |= 4
	}
	v.setReg(7, v.reg(7)&0xffff0fff|bits<<12)
	v.put32(8*0x800+0x7f0, uint32(pri)<<24|mode<<8|9)
	v.put32(8*0x800+0x3f0, scroll)
}

func TestLayer(t *testing.T) {
	v := testVideo()
	v.layer(1, 0, 0, false)
	for i := 0; i < 32*16; i++ {
		v.put32(9*0x800+i*4, 5<<24|testTile)
	}
	v.render()
	if got := v.px(17, 2); got != 5*16+(1+2) {
		t.Fatalf("(17,2) = %06x", got)
	}
	// scrolling right by 3: map x = screen x - 3
	v.layer(1, 0, 3, false)
	v.render()
	if got := v.px(20, 2); got != 5*16+(1+2) {
		t.Fatalf("scrolled: %06x", got)
	}
	// scroll y: map y = screen y - scroll
	v.layer(1, 0, 4<<16, false)
	v.render()
	if got := v.px(1, 6); got != 5*16+(1+2) {
		t.Fatalf("scrolled y: %06x", got)
	}
}

func TestLayerOverSpriteOfSamePriority(t *testing.T) {
	v := testVideo()
	v.layer(3, 0, 0, false)
	for i := 0; i < 32*16; i++ {
		v.put32(9*0x800+i*4, 5<<24|testTile+1)
	}
	v.setReg(2, 0x30000000) // sprites at priority 3 too
	v.sprite(0, 40, 40, 1, 1, 0x3f, 1, 0, testTile+1, false)
	v.list(0)
	v.latch()
	v.render()
	if v.px(41, 41) != 5*16+1 {
		t.Fatalf("sprite drawn over a layer of the same priority: %06x", v.px(41, 41))
	}
	v.setReg(2, 0x40000000)
	v.render()
	if v.px(41, 41) != 1*16+1 {
		t.Fatalf("sprite above the layer not drawn: %06x", v.px(41, 41))
	}
}

func TestLayerTransparencyAndPenAlpha(t *testing.T) {
	v := testVideo()
	// colour 0x2000 + 0x80 blends: a sprite below, a translucent layer above
	binary.BigEndian.PutUint32(v.pal[(1*16+1)*4:], 0xff000000) // sprite red
	binary.BigEndian.PutUint32(v.pal[(5*16+1)*4:], 0x0000ff00) // layer blue
	v.setReg(2, 0x10000000)
	v.sprite(0, 0, 0, 1, 1, 0x3f, 1, 0, testTile+1, false)
	v.list(0)
	v.latch()
	v.layer(2, 0x20, 0, false) // transparency 0x20: opacity alpha6(0x1f)
	for i := 0; i < 32*16; i++ {
		v.put32(9*0x800+i*4, 5<<24|testTile+1)
	}
	v.render()
	a := alpha6(0x1f)
	want := uint32((0xff*(256-a))>>8)<<16 | uint32((0xff*a)>>8)
	if got := v.px(3, 3); got != want {
		t.Fatalf("blend %06x want %06x", got, want)
	}
	if penAlpha(0x80, 0xff, true) != 0 || penAlpha(0x80, 0xc0, true) != alpha6(0x3f) || penAlpha(0x80, 5, false) != alpha6(0x3f) {
		t.Fatal("per-pen alpha")
	}
	if penAlpha(0, 7, false) != 256 {
		t.Fatal("opaque")
	}
}

func TestShading(t *testing.T) {
	v := testVideo()
	v.layer(1, 0, 0, false)
	for i := 0; i < 32*16; i++ {
		v.put32(9*0x800+i*4, 5<<24|testTile+1)
	}
	binary.BigEndian.PutUint32(v.pal[(5*16+1)*4:], 0x80808000)
	v.setReg(7, v.reg(7)&0x00ffffff|10<<24) // shading bank 10
	v.setReg(2, 1)                          // at priority 1
	v.put32(10*0x800+0x400+7*4, 0x00ff0080) // line 7: all the way to green
	v.put32(10*0x800+0x400+8*4, 0x00ff0040) // line 8: halfway
	v.render()
	if v.px(5, 6) != 0x808080 || v.px(5, 7) != 0x00ff00 || v.px(5, 8) != 0x40bf40 {
		t.Fatalf("%06x %06x %06x", v.px(5, 6), v.px(5, 7), v.px(5, 8))
	}
	// shading below the layer's priority leaves it alone
	v.setReg(2, 0)
	v.render()
	if v.px(5, 7) != 0x808080 {
		t.Fatalf("%06x", v.px(5, 7))
	}
}

// ------------------------------------------------------------ the board, with a test program

// testProgram: enables interrupts and loops; the vblank handler counts
// frames at 06000000, copies the inputs to 06000004 and 06000008 and
// acknowledges the interrupt.
func testProgram() []byte {
	rom := make([]byte, 0x100000)
	put16 := func(a int, ops ...uint16) {
		for i, op := range ops {
			binary.BigEndian.PutUint16(rom[a+i*2:], op)
		}
	}
	binary.BigEndian.PutUint32(rom[0:], 0x400)
	binary.BigEndian.PutUint32(rom[4:], 0x06100000)
	binary.BigEndian.PutUint32(rom[66*4:], 0x500)
	put16(0x400, 0xe000, 0x400e, 0xaffe, 0x0009) // mov #0,r0; ldc r0,sr; bra $; nop
	put16(0x500,
		0xd106, // mov.l @(0x51c),r1
		0x6012, // mov.l @r1,r0
		0x7001, // add #1,r0
		0x2102, // mov.l r0,@r1
		0xd205, // mov.l @(0x520),r2
		0x6322, // mov.l @r2,r3
		0x1131, // mov.l r3,@(4,r1)
		0x5321, // mov.l @(4,r2),r3
		0x1132, // mov.l r3,@(8,r1)
		0xd404, // mov.l @(0x524),r4
		0x2400, // mov.b r0,@r4
		0x002b, // rte
		0x0009, 0x0009)
	binary.BigEndian.PutUint32(rom[0x51c:], 0x06000000)
	binary.BigEndian.PutUint32(rom[0x520:], 0x03000000)
	binary.BigEndian.PutUint32(rom[0x524:], 0x0405ffdc)
	return rom
}

// testFiles splits a program image into the set's two program ROMs.
func testFiles(rom []byte) arcade.Files {
	f := arcade.Files{}
	for lane, name := range []string{"2prog_h.u21", "1prog_l.u22"} {
		d := make([]byte, 0x80000)
		for k := 0; k < len(d); k += 2 {
			at := k*2 + lane*2
			d[k], d[k+1] = rom[at+1], rom[at]
		}
		f[name] = d
	}
	return f
}

func testBoard(t *testing.T, opts arcade.Opts) *board {
	t.Helper()
	g := &games()[0]
	b, err := newBoard(g, testFiles(testProgram()), opts)
	if err != nil {
		t.Fatal(err)
	}
	return b.(*board)
}

func (b *board) ram32(a int) uint32 { return binary.BigEndian.Uint32(b.ram[a:]) }

func TestProgramLoad(t *testing.T) {
	rom := testProgram()
	b := testBoard(t, arcade.Opts{})
	if !bytes.Equal(b.rom[:0x1000], rom[:0x1000]) {
		t.Fatal("program ROMs interleaved wrongly")
	}
}

func TestVblankInterrupt(t *testing.T) {
	b := testBoard(t, arcade.Opts{})
	for i := 0; i < 10; i++ {
		b.RunFrame()
	}
	if n := b.ram32(0); n != 10 {
		t.Fatalf("%d interrupts in 10 frames", n)
	}
}

func TestInputs(t *testing.T) {
	b := testBoard(t, arcade.Opts{})
	b.RunFrame()
	if b.ram32(4) != 0xffffffff {
		t.Fatalf("idle inputs %08x", b.ram32(4))
	}
	b.SetInput([]arcade.Pad{{"Button 1": true, "Up": true, "Coin": true}, {"Start (1P)": true}})
	b.RunFrame()
	if got := b.ram32(4); got != 0xffffffff&^(0x88<<24|0x01<<16|0x01) {
		t.Fatalf("inputs %08x", got)
	}
	b.dips["test"] = 1
	b.RunFrame()
	if b.ram32(4)&0x20 != 0 {
		t.Fatal("service mode switch not seen")
	}
}

func TestRegionJumper(t *testing.T) {
	for _, region := range []int{0, 1, 2} {
		b := testBoard(t, arcade.Opts{Dips: map[string]int{"region": region}})
		b.RunFrame()
		if got := int(b.ram32(8)>>24) & 3; got != region {
			t.Errorf("region %d read as %d", region, got)
		}
	}
}

func TestEEPROMDefaultAndSRAM(t *testing.T) {
	b := testBoard(t, arcade.Opts{})
	s := b.SRAM()
	if len(s) != 256 || s[0] != 0xff {
		t.Fatalf("SRAM %d bytes, first %02x", len(s), s[0])
	}
	s[0] = 0x42
	b2 := testBoard(t, arcade.Opts{SRAM: s})
	if b2.SRAM()[0] != 0x42 {
		t.Fatal("SRAM not used")
	}
	// a MAME-style EEPROM file has its byte pairs swapped
	f := testFiles(testProgram())
	ee := make([]byte, 256)
	ee[0], ee[1] = 0x12, 0x34
	f["eeprom-dragnblz.u44"] = ee
	g := &games()[0]
	bb, err := newBoard(g, f, arcade.Opts{})
	if err != nil {
		t.Fatal(err)
	}
	if s := bb.(*board).SRAM(); s[0] != 0x34 || s[1] != 0x12 {
		t.Fatalf("EEPROM file: %02x %02x", s[0], s[1])
	}
}

func TestBoardStateRoundTrip(t *testing.T) {
	a := testBoard(t, arcade.Opts{})
	for i := 0; i < 5; i++ {
		a.RunFrame()
	}
	a.ymf.Write(4, 0xf9) // something in the sound chip's registers
	a.ymf.Write(5, 0x12)
	st, err := a.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		a.RunFrame()
	}
	b := testBoard(t, arcade.Opts{})
	if err := b.LoadState(st); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		b.RunFrame()
	}
	if !bytes.Equal(a.ram, b.ram) || a.cpu.PC != b.cpu.PC || a.cpu.Cycles != b.cpu.Cycles {
		t.Fatal("diverged after loading")
	}
	if err := b.LoadState(st[:len(st)/2]); err == nil {
		t.Fatal("half a state loaded")
	}
	if err := b.LoadState([]byte("nonsense")); err == nil {
		t.Fatal("nonsense loaded")
	}
}

func TestBoardReset(t *testing.T) {
	b := testBoard(t, arcade.Opts{})
	for i := 0; i < 3; i++ {
		b.RunFrame()
	}
	b.Reset()
	if b.ram32(0) != 0 || b.cpu.PC != 0x400 {
		t.Fatalf("after reset: count %d PC %08x", b.ram32(0), b.cpu.PC)
	}
	b.RunFrame()
	if b.ram32(0) != 1 {
		t.Fatalf("count %d", b.ram32(0))
	}
}

func TestAudioRate(t *testing.T) {
	b := testBoard(t, arcade.Opts{SampleRate: 44100})
	n := 0
	for i := 0; i < 60; i++ {
		b.RunFrame()
		n += len(b.TakeAudio())
	}
	if n < 44000 || n > 44200 {
		t.Fatalf("%d samples in a second", n)
	}
}
