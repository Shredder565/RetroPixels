package walkthrough

// A Step is one goal of a walkthrough: somewhere to go, and how to tell
// from memory that it's done.
type Step struct {
	Text  string // what to do, in capitals (the overlay's font has no lower case)
	Area  int
	X, Y  int // the world map room to go to
	ItemX int // where in that room, in pixels (0, 0 = the room's middle)
	ItemY int
	Done  func(s MetroidState) bool
}

func hasGear(bit byte) func(MetroidState) bool {
	return func(s MetroidState) bool { return s.Gear&bit != 0 }
}

// itemPos: an item's place in its room from its item table byte (%yyyyxxxx,
// in 16-pixel blocks; the item is drawn at the middle of its block).
func itemPos(b int) (x, y int) { return (b&0x0F)*16 + 8, (b>>4)*16 + 8 }

func step(text string, area, x, y, pos int, done func(MetroidState) bool) Step {
	ix, iy := 0, 0
	if pos >= 0 {
		ix, iy = itemPos(pos)
	}
	return Step{Text: text, Area: area, X: x, Y: y, ItemX: ix, ItemY: iy, Done: done}
}

// metroidRoute: a standard first playthrough of Metroid, as goals. The map
// positions and in-room item positions are the game's own item tables
// (SpecItmsTbl in each area bank). Steps can be done in any order; the guide
// always points at the first one that isn't done yet.
var metroidRoute = []Step{
	step("GET THE MORPH BALL", Brinstar, 0x02, 0x0E, 0x96, hasGear(GearMorph)),
	step("GET MISSILES", Brinstar, 0x12, 0x0B, 0x67, func(s MetroidState) bool { return s.MaxMissiles > 0 }),
	step("GET THE BOMBS", Brinstar, 0x19, 0x05, 0x37, hasGear(GearBombs)),
	step("GET THE LONG BEAM", Brinstar, 0x07, 0x05, 0x37, hasGear(GearLongBeam)),
	step("GET THE ICE BEAM", Brinstar, 0x13, 0x09, 0x37, hasGear(GearIce)),
	step("GET THE VARIA SUIT", Brinstar, 0x0F, 0x02, 0x37, hasGear(GearVaria)),
	step("GET THE HIGH JUMP BOOTS", Norfair, 0x1B, 0x11, 0x37, hasGear(GearHighJump)),
	step("GET THE SCREW ATTACK", Norfair, 0x0F, 0x10, 0x37, hasGear(GearScrew)),
	step("GET THE WAVE BEAM", Norfair, 0x12, 0x15, 0x37, hasGear(GearWave)),
	step("DEFEAT RIDLEY", Ridley, 0x11, 0x1D, -1, func(s MetroidState) bool { return s.RidleyDead }),
	step("DEFEAT KRAID", Kraid, 0x08, 0x1D, -1, func(s MetroidState) bool { return s.KraidDead }),
	step("ENTER TOURIAN", Brinstar, 0x03, 0x02, -1, func(s MetroidState) bool { return s.Area == Tourian }),
	step("DESTROY MOTHER BRAIN", Tourian, 0x02, 0x0B, -1, func(MetroidState) bool { return false }),
}

// elevator is a ride between two areas: from (X, Y) in From, Down or up.
type elevator struct {
	From, To int
	X, Y     int
	Down     bool
}

// Elevator rooms, from the item tables (it_Elevator); each ride arrives two
// rooms further down or up the same shaft.
var elevators = []elevator{
	{Brinstar, Norfair, 0x16, 0x0B, true},
	{Brinstar, Kraid, 0x07, 0x12, true},
	{Brinstar, Tourian, 0x03, 0x02, true},
	{Norfair, Brinstar, 0x16, 0x0D, false},
	{Norfair, Ridley, 0x19, 0x16, true},
	{Kraid, Brinstar, 0x07, 0x14, false},
	{Ridley, Norfair, 0x19, 0x18, false},
	{Tourian, Brinstar, 0x03, 0x04, false},
}

// nextArea: the area to ride to next on the way from one area to another
// (Brinstar is the hub; Ridley's Lair is reached through Norfair).
func nextArea(from, to int) int {
	switch {
	case from == to:
		return to
	case from == Brinstar && to == Ridley:
		return Norfair
	case from == Brinstar:
		return to
	case from == Norfair && to == Ridley:
		return Ridley
	case from == Ridley:
		return Norfair
	default:
		return Brinstar
	}
}

func findElevator(from, to int) (elevator, bool) {
	for _, e := range elevators {
		if e.From == from && e.To == to {
			return e, true
		}
	}
	return elevator{}, false
}
