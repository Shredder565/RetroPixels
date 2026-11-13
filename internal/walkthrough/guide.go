package walkthrough

import (
	"fmt"
	"math"
)

// Hint is what the overlay shows for the current frame.
type Hint struct {
	Show   bool
	Step   int    // index of the current step
	Title  string // the step ("GET THE MORPH BALL")
	Detail string // how to get there ("ELEVATOR TO NORFAIR", "3 ROOMS AWAY")
	// Angle the arrow points, in radians: 0 = right, pi/2 = down (screen
	// coordinates).
	Angle float64
	Here  bool // in the step's room: the arrow points at the item itself
}

// Guide follows one game.
type Guide struct {
	r     metroidReader
	route []Step
	doorC map[int][2]bool // area<<8|room -> left, right
	last  MetroidState
	hint  Hint
}

// New returns a guide for the game, or nil when there is no walkthrough
// for it (it recognises the game from its ROM, whatever the file is
// called). system is the core's system id ("nes"); mem is the running core
// (core.Memory).
func New(system string, mem Mem) *Guide {
	if system != "nes" || mem == nil {
		return nil
	}
	prgSize := mem.MemorySize("PRG ROM")
	if prgSize <= 0 || prgSize > 1<<20 {
		return nil
	}
	prg := make([]byte, prgSize)
	if mem.ReadMemory("PRG ROM", 0, prg) != nil || !isMetroid(prg) {
		return nil
	}
	return &Guide{r: metroidReader{mem: mem, prg: prg}, route: metroidRoute, doorC: map[int][2]bool{}}
}

// Game: the game the guide is for.
func (g *Guide) Game() string { return "Metroid" }

// State: what the guide last read.
func (g *Guide) State() MetroidState { return g.last }

// Hint: what Update last worked out.
func (g *Guide) Hint() Hint { return g.hint }

// Update reads the game's memory (call it on the game's thread, between
// frames) and works out the hint.
func (g *Guide) Update() Hint {
	g.hint = Hint{}
	if !g.r.refresh() {
		return g.hint
	}
	s := g.r.state()
	g.last = s
	if !s.Playing {
		return g.hint
	}
	si := -1
	for i, st := range g.route {
		if !st.Done(s) {
			si = i
			break
		}
	}
	if si < 0 {
		g.hint = Hint{Show: true, Step: len(g.route), Title: "ALL DONE!", Angle: math.NaN()}
		return g.hint
	}
	st := g.route[si]
	h := Hint{Show: true, Step: si, Title: st.Text}
	tx, ty := st.X, st.Y
	if st.Area != s.Area {
		to := nextArea(s.Area, st.Area)
		e, ok := findElevator(s.Area, to)
		if !ok {
			h.Detail = "HEAD FOR " + AreaName(st.Area)
			h.Angle = math.NaN()
			g.hint = h
			return h
		}
		if s.RoomX == e.X && s.RoomY == e.Y {
			if e.Down {
				h.Detail = "RIDE THE ELEVATOR DOWN TO " + AreaName(to)
				h.Angle = math.Pi / 2
			} else {
				h.Detail = "RIDE THE ELEVATOR UP TO " + AreaName(to)
				h.Angle = -math.Pi / 2
			}
			g.hint = h
			return h
		}
		h.Detail = "GO TO THE ELEVATOR TO " + AreaName(to)
		tx, ty = e.X, e.Y
	}
	if s.RoomX == tx && s.RoomY == ty {
		ix, iy := 128, 120
		if st.Area == s.Area && (st.ItemX != 0 || st.ItemY != 0) {
			ix, iy = st.ItemX, st.ItemY
		}
		h.Here = st.Area == s.Area
		if h.Detail == "" {
			h.Detail = "IT'S IN THIS ROOM"
		}
		dx, dy := float64(ix-s.X), float64(iy-s.Y)
		if math.Abs(dx) < 12 && math.Abs(dy) < 12 {
			h.Angle = math.NaN() // on top of it
		} else {
			h.Angle = math.Atan2(dy, dx)
		}
		g.hint = h
		return h
	}
	dx, dy, n := g.path(s, tx, ty)
	if n > 0 {
		if h.Detail == "" {
			if n == 1 {
				h.Detail = "NEXT ROOM"
			} else {
				h.Detail = fmt.Sprintf("%d ROOMS AWAY", n)
			}
		}
	} else {
		// no route found: point straight at the room
		dx, dy = sign(tx-s.RoomX), sign(ty-s.RoomY)
		if h.Detail == "" {
			h.Detail = "THAT WAY"
		}
	}
	h.Angle = math.Atan2(float64(dy), float64(dx))
	g.hint = h
	return h
}

func sign(v int) int {
	switch {
	case v < 0:
		return -1
	case v > 0:
		return 1
	}
	return 0
}

func (g *Guide) doors(area, x, y int) (left, right bool) {
	n := g.r.room(x, y)
	if n == 0xFF {
		return
	}
	k := area<<8 | int(n)
	d, ok := g.doorC[k]
	if !ok {
		d[0], d[1] = g.r.doors(area, n)
		g.doorC[k] = d
	}
	return d[0], d[1]
}

func (g *Guide) filled(x, y int) bool { return g.r.room(x, y) != 0xFF }

// path finds the way through the world map from Samus's room to (tx, ty)
// in her area, and returns the first room-to-room step (dx, dy) and how
// many rooms away the target is (0 = no way found).
//
// Metroid's rooms are corridors that scroll left and right and shafts that
// scroll up and down; going through a door switches between the two. So
// the search walks (room, scrolls up/down) states: corridors go left and
// right to the next room, shafts up and down, and either goes sideways
// through a door, coming out as the other kind (unless the room beyond has
// nothing above or below it, like the item rooms at the end of corridors).
func (g *Guide) path(s MetroidState, tx, ty int) (dx, dy, n int) {
	type node struct {
		x, y int
		v    bool
	}
	type prev struct {
		from  node
		first [2]int
		dist  int
	}
	start := node{s.RoomX, s.RoomY, s.Vertical}
	seen := map[node]prev{start: {}}
	q := []node{start}
	for len(q) > 0 {
		c := q[0]
		q = q[1:]
		pc := seen[c]
		if c.x == tx && c.y == ty {
			return pc.first[0], pc.first[1], pc.dist
		}
		push := func(nx, ny int, v bool) {
			nn := node{nx, ny, v}
			if _, ok := seen[nn]; ok {
				return
			}
			p := prev{from: c, first: pc.first, dist: pc.dist + 1}
			if c == start {
				p.first = [2]int{nx - c.x, ny - c.y}
			}
			seen[nn] = p
			q = append(q, nn)
		}
		cl, cr := g.doors(s.Area, c.x, c.y)
		for _, d := range []int{-1, 1} {
			nx := c.x + d
			if !g.filled(nx, c.y) {
				continue
			}
			nl, nr := g.doors(s.Area, nx, c.y)
			door := (d < 0 && (cl || nr)) || (d > 0 && (cr || nl))
			if door {
				v := !c.v
				if v && !g.filled(nx, c.y-1) && !g.filled(nx, c.y+1) {
					v = false
				}
				push(nx, c.y, v)
			} else if !c.v {
				push(nx, c.y, false)
			}
		}
		if c.v {
			for _, d := range []int{-1, 1} {
				if g.filled(c.x, c.y+d) {
					push(c.x, c.y+d, true)
				}
			}
		}
	}
	return 0, 0, 0
}
