package nds

// gpu3D is the DS's 3D engine. In this first stage it takes the game's
// commands and answers its status reads, so 3D games keep running, but it
// doesn't draw yet: the 3D layer is see-through.
type gpu3D struct {
	m         *Machine
	disp3dcnt uint32
	regs      map[uint32]uint32
	busyUntil int64
}

func newGPU3D(m *Machine) *gpu3D {
	return &gpu3D{m: m, regs: map[uint32]uint32{}, busyUntil: never}
}

func (g *gpu3D) read32(a uint32) uint32 {
	switch a {
	case 0x04000060:
		return g.disp3dcnt
	case 0x04000600: // GXSTAT: FIFO empty, not busy
		return 0x06000000 | g.regs[a]&0xc0000000
	case 0x04000604: // polygon and vertex counts
		return 0
	}
	if a >= 0x04000640 && a < 0x04000680 { // clip matrix: identity
		i := (a - 0x04000640) / 4
		if i%5 == 0 {
			return 0x1000
		}
		return 0
	}
	if a >= 0x04000680 && a < 0x040006a4 { // vector matrix
		i := (a - 0x04000680) / 4
		if i%4 == 0 {
			return 0x1000
		}
		return 0
	}
	return g.regs[a&^3]
}

func (g *gpu3D) write32(a, v uint32) {
	switch a {
	case 0x04000060:
		g.disp3dcnt = v & 0x7fff
		return
	case 0x04000600:
		g.regs[a] = g.regs[a]&^0xc0000000 | v&0xc0000000
		if v&(1<<15) != 0 { // acknowledge the matrix stack error
			g.regs[a] &^= 1 << 15
		}
		return
	}
	g.regs[a&^3] = v
}

func (g *gpu3D) write16(a uint32, v uint16) {
	old := g.read32(a &^ 3)
	sh := a & 2 * 8
	g.write32(a&^3, old&^(0xffff<<sh)|uint32(v)<<sh)
}

func (g *gpu3D) write8(a uint32, v uint8) {
	old := g.read32(a &^ 3)
	sh := a & 3 * 8
	g.write32(a&^3, old&^(0xff<<sh)|uint32(v)<<sh)
}

func (g *gpu3D) waiting() bool { return false }
func (g *gpu3D) run()          {}
func (g *gpu3D) vblank()       {}
func (g *gpu3D) renderFrame()  {}

// line fills BG0 with line y of the 3D picture (see-through for now).
func (g *gpu3D) line(y int, out *[256]uint32, alpha *[256]uint8) {
	for i := range out {
		out[i] = 0
		alpha[i] = 0
	}
}

func (g *gpu3D) saveState() []byte {
	var b []byte
	b = append(b, byte(g.disp3dcnt), byte(g.disp3dcnt>>8))
	return b
}

func (g *gpu3D) loadState(b []byte) error {
	if len(b) >= 2 {
		g.disp3dcnt = uint32(b[0]) | uint32(b[1])<<8
	}
	return nil
}
