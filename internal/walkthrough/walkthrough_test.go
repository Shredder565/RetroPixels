package walkthrough

import (
	"errors"
	"math"
	"testing"
)

// fakeMem is a pretend Metroid: RAM, WRAM and a 128 KB PRG ROM.
type fakeMem struct {
	ram  [0x800]byte
	wram [0x2000]byte
	prg  [0x20000]byte
}

func (m *fakeMem) MemorySize(d string) int {
	switch d {
	case "RAM":
		return len(m.ram)
	case "WRAM":
		return len(m.wram)
	case "PRG ROM":
		return len(m.prg)
	}
	return 0
}

func (m *fakeMem) ReadMemory(d string, a uint32, buf []byte) error {
	var src []byte
	switch d {
	case "RAM":
		src = m.ram[:]
	case "WRAM":
		src = m.wram[:]
	case "PRG ROM":
		src = m.prg[:]
	default:
		return errors.New("no domain")
	}
	copy(buf, src[a:])
	return nil
}

func newFake() *fakeMem {
	m := &fakeMem{}
	starts := [...][3]int{{1, 0x03, 0x0E}, {2, 0x16, 0x0D}, {3, 0x03, 0x04}, {4, 0x07, 0x14}, {5, 0x19, 0x18}}
	for _, st := range starts {
		o := st[0]*0x4000 + 0x95D7 - 0x8000
		m.prg[o], m.prg[o+1] = byte(st[1]), byte(st[2])
	}
	for i := 0; i < 0x400; i++ {
		m.wram[wramWorldMap+i] = 0xFF
	}
	// Brinstar's room table at $A000: room n's data at $A100+n*16
	b := 1*0x4000 - 0x8000
	m.prg[b+0x959A], m.prg[b+0x959B] = 0x00, 0xA0
	for n := 0; n < 8; n++ {
		p := 0xA100 + n*16
		m.prg[b+0xA000+n*2], m.prg[b+0xA000+n*2+1] = byte(p), byte(p>>8)
		m.prg[b+p] = 0      // attribute
		m.prg[b+p+1] = 0xFD // no structures
		m.prg[b+p+2] = 0xFF // nothing else
	}
	// room 1: doors both sides; room 2: a door on the left only
	m.prg[b+0xA100+16+2], m.prg[b+0xA100+16+3], m.prg[b+0xA100+16+4], m.prg[b+0xA100+16+5], m.prg[b+0xA100+16+6] = 2, 0xA1, 2, 0xB1, 0xFF
	m.prg[b+0xA100+32+2], m.prg[b+0xA100+32+3], m.prg[b+0xA100+32+4] = 2, 0xB1, 0xFF
	m.ram[ramMainRoutine] = 3
	return m
}

func (m *fakeMem) set(x, y int, room byte) { m.wram[wramWorldMap+y*32+x] = room }

func TestNotMetroid(t *testing.T) {
	m := newFake()
	m.prg[1*0x4000+0x95D7-0x8000] = 9
	if New("nes", m) != nil {
		t.Fatal("guide for a ROM that isn't Metroid")
	}
	if New("snes", newFake()) != nil {
		t.Fatal("guide for another system")
	}
}

func TestRoomFromScroll(t *testing.T) {
	m := newFake()
	g := New("nes", m)
	set := func(mapX, mapY, dir, sx, sy, ppu, hi byte) MetroidState {
		m.ram[ramMapPosX], m.ram[ramMapPosY], m.ram[ramScrollDir] = mapX, mapY, dir
		m.ram[ramScrollX], m.ram[ramScrollY], m.ram[ramPPUCtrl], m.ram[ramSamusHi] = sx, sy, ppu, hi
		g.Update()
		return g.State()
	}
	// these three were read from the real game, walking left then right from the start
	if s := set(3, 14, 2, 0, 0, 0x90, 0); s.RoomX != 3 || s.RoomY != 14 || s.Vertical {
		t.Errorf("start: %+v", s)
	}
	if s := set(2, 14, 2, 148, 0, 0x97, 0); s.RoomX != 3 {
		t.Errorf("left: %+v", s)
	}
	if s := set(3, 14, 3, 189, 0, 0x97, 0); s.RoomX != 3 {
		t.Errorf("right: %+v", s)
	}
	// a shaft scrolling down, Samus on the camera's top name table
	if s := set(5, 6, 1, 0, 40, 0x90, 0); s.RoomX != 5 || s.RoomY != 5 || !s.Vertical {
		t.Errorf("shaft: %+v", s)
	}
}

// A corridor (rooms 0) along y=10 from x=1 to x=4, a door at x=4's right
// into a shaft (room 1, doors both sides) at x=5 running y=8..12, and a
// corridor that only the shaft reaches at y=12 (x=6, room 2, door on its
// left). A corridor room directly under the first corridor (x=2, y=11) is
// not reachable from it: corridors don't scroll down.
func TestPath(t *testing.T) {
	m := newFake()
	for x := 1; x <= 4; x++ {
		m.set(x, 10, 0)
	}
	m.set(2, 11, 0)
	for y := 8; y <= 12; y++ {
		m.set(5, y, 1)
	}
	m.set(6, 12, 2)
	g := New("nes", m)
	g.Update()
	s := MetroidState{Area: Brinstar, RoomX: 1, RoomY: 10}
	if dx, dy, n := g.path(s, 6, 12); dx != 1 || dy != 0 || n != 7 {
		t.Errorf("to the far corridor: %d,%d %d", dx, dy, n)
	}
	if _, _, n := g.path(s, 2, 11); n != 0 {
		t.Errorf("walked down out of a corridor (%d rooms)", n)
	}
	s = MetroidState{Area: Brinstar, RoomX: 5, RoomY: 8, Vertical: true}
	if dx, dy, n := g.path(s, 6, 12); dx != 0 || dy != 1 || n != 5 {
		t.Errorf("down the shaft: %d,%d %d", dx, dy, n)
	}
}

func TestNextArea(t *testing.T) {
	for _, c := range [][3]int{
		{Brinstar, Ridley, Norfair}, {Norfair, Ridley, Ridley}, {Ridley, Kraid, Norfair},
		{Kraid, Tourian, Brinstar}, {Brinstar, Kraid, Kraid}, {Norfair, Kraid, Brinstar},
	} {
		if got := nextArea(c[0], c[1]); got != c[2] {
			t.Errorf("%s to %s: via %s", AreaName(c[0]), AreaName(c[1]), AreaName(got))
		}
		if _, ok := findElevator(c[0], c[2]); !ok {
			t.Errorf("no elevator %s to %s", AreaName(c[0]), AreaName(c[2]))
		}
	}
}

func TestDraw(t *testing.T) {
	pix := make([]uint32, 256*240)
	Draw(pix, 256, 240, Hint{})
	for _, p := range pix {
		if p != 0 {
			t.Fatal("drew a hint that isn't shown")
		}
	}
	Draw(pix, 256, 240, Hint{Show: true, Title: "GET THE MORPH BALL", Detail: "NEXT ROOM", Angle: math.Pi})
	// the arrow's tip is at the left of the panel
	if pix[(240-4-11)*256+4+12-8] != 0xF8D838 {
		t.Error("no arrow pointing left")
	}
	Draw(pix[:10], 256, 240, Hint{Show: true}) // too small: nothing, no panic
	for r := range font {
		if len(font[r]) != 15 {
			t.Errorf("glyph %q is %d pixels", r, len(font[r]))
		}
	}
}
