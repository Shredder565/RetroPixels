package ps3

// Drawing: stage 4. For now the RSX only keeps its registers and the
// picture is the display buffer as it is in video memory.

type rsxDraw struct{}

func (m *Machine) rsxDraw(r *rsxState, reg, v uint32) {}

// Screen: the last flipped picture as 0x00RRGGBB pixels.
type Screen struct {
	W, H int
	Pix  []uint32
}

func (m *Machine) present(id uint32) {
	g := m.gcm
	var off, pitch, w, h uint32
	if id&0x100 != 0 {
		s := g.rescSrcs[id&7]
		off, pitch, w, h = s.offset, s.pitch, s.width, s.height
	} else {
		d := g.display[id&7]
		off, pitch, w, h = d.offset, d.pitch, d.width, d.height
	}
	if w == 0 || h == 0 || w > 4096 || h > 4096 {
		return
	}
	sc := m.screen
	if sc.W != int(w) || sc.H != int(h) {
		sc = &Screen{W: int(w), H: int(h), Pix: make([]uint32, w*h)}
		m.screen = sc
	}
	row := make([]byte, w*4)
	for y := uint32(0); y < h; y++ {
		m.Mem.ReadBytes(RSXBase+off+y*pitch, row)
		for x := uint32(0); x < w; x++ {
			// A8R8G8B8 in memory order
			sc.Pix[y*w+x] = uint32(row[4*x+1])<<16 | uint32(row[4*x+2])<<8 | uint32(row[4*x+3])
		}
	}
}

// Picture returns the last frame shown (nil before the first flip).
func (m *Machine) Picture() *Screen { return m.screen }
