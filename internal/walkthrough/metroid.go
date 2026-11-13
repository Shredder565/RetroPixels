// Package walkthrough is RetroPixels' walkthrough mode: it reads where the
// player is in a game from the game's own memory, works out which step of a
// walkthrough they are on, and draws an arrow over the picture pointing to
// the next objective.
//
// The first (and so far only) game is Metroid (NES). Its memory map, world
// map, room door data and item table come from the community disassembly
// (github.com/metroidret/m1disasm); the walkthrough steps in metroidRoute are
// a standard route through the game, written as goals the memory can check
// (an item bit, a boss's statue raised, an area entered).
package walkthrough

// Mem is the part of core.Memory the guide reads.
type Mem interface {
	MemorySize(domain string) int
	ReadMemory(domain string, addr uint32, buf []byte) error
}

// Metroid areas, as the low four bits of InArea ($74).
const (
	Brinstar = 0
	Norfair  = 1
	Kraid    = 2
	Tourian  = 3
	Ridley   = 4
)

var areaNames = [...]string{"BRINSTAR", "NORFAIR", "KRAID'S LAIR", "TOURIAN", "RIDLEY'S LAIR"}

// AreaName: the area's name, in capitals for the overlay font.
func AreaName(a int) string {
	if a >= 0 && a < len(areaNames) {
		return areaNames[a]
	}
	return "?"
}

// areaBank: the 16 KB PRG bank that holds each area's rooms (MMC1).
var areaBank = [...]int{Brinstar: 1, Norfair: 2, Kraid: 4, Tourian: 3, Ridley: 5}

// Samus's gear bits (SamusGear, $6878).
const (
	GearBombs    = 0x01
	GearHighJump = 0x02
	GearLongBeam = 0x04
	GearScrew    = 0x08
	GearMorph    = 0x10
	GearVaria    = 0x20
	GearWave     = 0x40
	GearIce      = 0x80
)

// MetroidState is what the guide reads from Metroid's memory each frame.
type MetroidState struct {
	Playing      bool // the game engine is running (not the title, password or pause screen)
	Area         int
	RoomX, RoomY int  // the world map room Samus is in (0..31)
	X, Y         int  // Samus's position in the room, in pixels
	Vertical     bool // the room scrolls up and down (a shaft), not left and right
	Gear         byte
	Missiles     int
	MaxMissiles  int
	Tanks        int
	KraidDead    bool
	RidleyDead   bool
	OnElevator   bool
}

// RAM addresses (CPU addresses; $6000-$7FFF is the cartridge's WRAM).
const (
	ramGameMode    = 0x1D
	ramMainRoutine = 0x1E
	ramRoomPtrs    = 0x3B
	ramScrollDir   = 0x49
	ramMapPosY     = 0x4F
	ramMapPosX     = 0x50
	ramInArea      = 0x74
	ramScrollY     = 0xFC
	ramScrollX     = 0xFD
	ramPPUCtrl     = 0xFF
	ramSamusElev   = 0x307
	ramSamusHi     = 0x30C
	ramSamusY      = 0x30D
	ramSamusX      = 0x30E

	wramTanks      = 0x6877 - 0x6000
	wramGear       = 0x6878 - 0x6000
	wramMissiles   = 0x6879 - 0x6000
	wramMaxMiss    = 0x687A - 0x6000
	wramKraidStat  = 0x687B - 0x6000
	wramRidleyStat = 0x687C - 0x6000
	wramWorldMap   = 0x7000 - 0x6000
)

// metroidReader reads Metroid's memory through a core.
type metroidReader struct {
	mem  Mem
	ram  [0x800]byte
	wram [0x2000]byte
	prg  []byte // the whole PRG ROM (read once)
}

func (r *metroidReader) refresh() bool {
	if r.mem.ReadMemory("RAM", 0, r.ram[:]) != nil {
		return false
	}
	if r.mem.ReadMemory("WRAM", 0, r.wram[:]) != nil {
		return false
	}
	return true
}

// state decodes the memory read by refresh.
func (r *metroidReader) state() MetroidState {
	ram, w := &r.ram, &r.wram
	s := MetroidState{
		Playing:     ram[ramGameMode] == 0 && ram[ramMainRoutine] == 3,
		Area:        int(ram[ramInArea] & 0x0F),
		Gear:        w[wramGear],
		Missiles:    int(w[wramMissiles]),
		MaxMissiles: int(w[wramMaxMiss]),
		Tanks:       int(w[wramTanks]),
		KraidDead:   w[wramKraidStat]&0x80 != 0,
		RidleyDead:  w[wramRidleyStat]&0x80 != 0,
		OnElevator:  ram[ramSamusElev] != 0,
		X:           int(ram[ramSamusX]),
		Y:           int(ram[ramSamusY]),
	}
	// The room Samus is in, the way the game works it out for items it
	// adds to the unique item history (GetItemXYPos, 07:DC1C): MapPosX/Y is
	// the room at the edge the screen is scrolling towards; step back to
	// the room at the camera's top-left corner, then on to the next room if
	// Samus is on the other name table.
	x, y := int(ram[ramMapPosX]), int(ram[ramMapPosY])
	dir := ram[ramScrollDir] & 3
	horiz := dir&2 != 0
	s.Vertical = !horiz
	if horiz {
		if dir&1 != 0 && ram[ramScrollX] != 0 { // scrolling right
			x--
		}
	} else if dir&1 != 0 && ram[ramScrollY] != 0 { // scrolling down
		y--
	}
	if (ram[ramPPUCtrl]^ram[ramSamusHi])&1 != 0 {
		if horiz {
			x++
		} else {
			y++
		}
	}
	s.RoomX, s.RoomY = x&31, y&31
	return s
}

// room: the room number on the world map at (x, y), or 0xFF for none.
func (r *metroidReader) room(x, y int) byte {
	if x < 0 || y < 0 || x > 31 || y > 31 {
		return 0xFF
	}
	return r.wram[wramWorldMap+y*32+x]
}

// prgByte: a byte of the area's bank at CPU address a ($8000-$BFFF).
func (r *metroidReader) prgByte(area int, a int) (byte, bool) {
	if area < 0 || area >= len(areaBank) || a < 0x8000 || a > 0xBFFF {
		return 0, false
	}
	off := areaBank[area]*0x4000 + a - 0x8000
	if off >= len(r.prg) {
		return 0, false
	}
	return r.prg[off], true
}

// doors: whether room number n of the area has a door on its left and on
// its right, from the room's data in ROM (the enemy/door list after the
// structures; LoadDoor, 07:EB8C: bit 4 of the door byte is the side, 0 =
// right, 1 = left).
func (r *metroidReader) doors(area int, n byte) (left, right bool) {
	// RoomPtrTable for the current area is copied to RAM at $3B; every
	// area's AreaPointers_ROM is at $959A in its own bank, so read that
	// one instead (the guide may ask about another area).
	tl, ok1 := r.prgByte(area, 0x959A)
	th, ok2 := r.prgByte(area, 0x959B)
	if !ok1 || !ok2 {
		return
	}
	table := int(th)<<8 | int(tl)
	pl, ok1 := r.prgByte(area, table+int(n)*2)
	ph, ok2 := r.prgByte(area, table+int(n)*2+1)
	if !ok1 || !ok2 {
		return
	}
	p := int(ph)<<8 | int(pl)
	p++                        // the room's attribute byte
	for i := 0; i < 200; i++ { // structures: 3 bytes each until $FD
		b, ok := r.prgByte(area, p)
		if !ok || b == 0xFF {
			return
		}
		if b == 0xFD {
			p++
			break
		}
		if b == 0xFE {
			p++
			continue
		}
		p += 3
	}
	for i := 0; i < 64; i++ { // enemies, doors, elevators... until $FF
		b, ok := r.prgByte(area, p)
		if !ok || b == 0xFF {
			return
		}
		switch b & 0x0F {
		case 1, 7: // enemy, pipe bug hole
			p += 3
		case 2: // door
			d, _ := r.prgByte(area, p+1)
			if d&0x10 != 0 {
				left = true
			} else {
				right = true
			}
			p += 2
		case 4: // elevator
			p += 2
		case 6: // statues
			p++
		default:
			return
		}
	}
	return
}

// isMetroid: whether a PRG ROM is Metroid's (the US/Japan cartridge
// layout): 128 KB, with every area's start room (AreaMapPosX/Y at $95D7 in
// each area's bank) where the disassembly has it. These are also where each
// elevator ride arrives, which the route's elevator table relies on.
func isMetroid(prg []byte) bool {
	if len(prg) != 0x20000 {
		return false
	}
	starts := [...][3]int{{1, 0x03, 0x0E}, {2, 0x16, 0x0D}, {3, 0x03, 0x04}, {4, 0x07, 0x14}, {5, 0x19, 0x18}}
	for _, st := range starts {
		o := st[0]*0x4000 + 0x95D7 - 0x8000
		if int(prg[o]) != st[1] || int(prg[o+1]) != st[2] {
			return false
		}
	}
	return true
}
