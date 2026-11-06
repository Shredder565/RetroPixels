package nds

// Drawing the 3D picture at a higher resolution: the 3D engine draws at
// sc times the DS's 256×192 (render3d.go), and the screens are put together
// at that size here. The 2D layers stay the DS's own pixels, made bigger;
// where a pixel showed the 3D picture (engine2D.hiOp says how), the
// detailed 3D pixels are used instead, with the same blending and
// brightness. The game itself still sees the 256×192 picture (display
// capture, and the 2D engine's own blending decisions), so it plays the
// same at any scale.

const (
	hiNone     = iota // no 3D here
	hi3D              // the 3D picture as it is
	hiBlend3D         // blended by its own alpha with hiSec
	hiBlend           // blended with hiSec by EVA (hiArg&0xff) and EVB
	hiBrighten        // brightened by hiArg
	hiDarken          // darkened by hiArg
)

// SetRenderScale draws 3D at n times the DS's resolution (1-8); FrameHi is
// the picture then.
func (m *Machine) SetRenderScale(n int) {
	m.gpu3d.setScale(n)
	m.frameHi = nil
	if sc := m.gpu3d.sc; sc > 1 {
		m.frameHi = make([]uint32, ScreenW*sc*ScreenH*2*sc)
	}
}

// RenderScale is the 3D picture's scale.
func (m *Machine) RenderScale() int { return m.gpu3d.sc }

// FrameHi is the two screens at the render scale (ScreenW×sc wide, top
// above bottom), or nil at scale 1.
func (m *Machine) FrameHi() []uint32 { return m.frameHi }

// finishLineHi puts line y of an engine onto the big picture, at row
// offset yo (0 top screen, ScreenH bottom).
func (m *Machine) finishLineHi(e *engine2D, y, yo int, off bool) {
	g := m.gpu3d
	sc := g.sc
	W := ScreenW * sc
	for sy := 0; sy < sc; sy++ {
		row := m.frameHi[((yo+y)*sc+sy)*W:][:W]
		if off {
			clear(row)
			continue
		}
		hrow := g.hi[(y*sc+sy)*g.W:][:g.W]
		for x := 0; x < ScreenW; x++ {
			op := e.hiOp[x]
			if op == hiNone || !e.isA {
				c := e.out[x]
				for k := 0; k < sc; k++ {
					row[x*sc+k] = c
				}
				continue
			}
			sec, arg := e.hiSec[x], e.hiArg[x]
			for k := 0; k < sc; k++ {
				v := hrow[x*sc+k]
				a := v >> 16 & 31
				c := v & 0x7fff
				switch {
				case a == 0:
					c = sec
				case op == hiBlend3D:
					c = blend3D(c, sec, a)
				case op == hiBlend:
					c = blend(c, sec, arg&0xff, arg>>8)
				case op == hiBrighten:
					c = brighten(c, arg)
				case op == hiDarken:
					c = darken(c, arg)
				}
				row[x*sc+k] = masterBright(rgb(c), e.masterBright)
			}
		}
	}
}

// masterBright is engine2D.brightness for one pixel.
func masterBright(c uint32, mb uint16) uint32 {
	f := uint32(mb & 0x1f)
	if f > 16 {
		f = 16
	}
	r, g, b := c>>16&0xff, c>>8&0xff, c&0xff
	switch mb >> 14 {
	case 1:
		r += (255 - r) * f / 16
		g += (255 - g) * f / 16
		b += (255 - b) * f / 16
	case 2:
		r -= r * f / 16
		g -= g * f / 16
		b -= b * f / 16
	default:
		return c
	}
	return r<<16 | g<<8 | b
}
