package wiiu

// OSScreen: coreinit's simple frame buffers for the TV and the GamePad, which
// homebrew (and some system software) draws text and pixels into without
// the GPU. Each screen has two buffers in the program's memory; drawing goes
// to one while the other is shown, and OSScreenFlipBuffersEx swaps them.
//
// The pictures the console shows (from OSScreen or GX2's scan buffers) are
// kept here too.

import "strings"

const (
	TVWidth   = 1280
	TVHeight  = 720
	DRCWidth  = 854
	DRCHeight = 480

	tvPitch  = 1280
	drcPitch = 896
)

type osScreenBuf struct {
	Addr    uint32 // the program's buffer (two frames)
	Enabled bool
	Draw    uint32 // which frame drawing goes to
	Shown   bool   // flipped at least once
}

// OSScreen is the state of both screens.
type OSScreen struct {
	Init bool
	Buf  [2]osScreenBuf // 0 = TV, 1 = GamePad

	// the pictures shown, 0x00RRGGBB
	TV, DRC           []uint32
	TVUsed, DRCUsed   bool
	TVFrame, DRCFrame uint64 // the frame each was last updated on
}

func newOSScreen() *OSScreen {
	return &OSScreen{TV: make([]uint32, TVWidth*TVHeight), DRC: make([]uint32, DRCWidth*DRCHeight)}
}

func screenDims(s uint32) (w, h, pitch uint32) {
	if s == 0 {
		return TVWidth, TVHeight, tvPitch
	}
	return DRCWidth, DRCHeight, drcPitch
}

func screenBufferSize(s uint32) uint32 {
	_, h, p := screenDims(s)
	return p * h * 4 * 2
}

func (o *OSScreen) frameAddr(s uint32) uint32 {
	_, h, p := screenDims(s)
	return o.Buf[s].Addr + o.Buf[s].Draw*p*h*4
}

// show copies the shown frame of screen s into the picture.
func (m *Machine) osScreenShow(s uint32) {
	o := m.screen
	b := &o.Buf[s]
	if b.Addr == 0 {
		return
	}
	w, h, p := screenDims(s)
	shown := b.Draw ^ 1
	base := b.Addr + shown*p*h*4
	dst := o.TV
	if s == 1 {
		dst = o.DRC
	}
	row := make([]byte, w*4)
	for y := uint32(0); y < h; y++ {
		m.Mem.ReadBytes(base+y*p*4, row)
		d := dst[y*w : y*w+w]
		for x := range d {
			d[x] = uint32(row[x*4])<<16 | uint32(row[x*4+1])<<8 | uint32(row[x*4+2])
		}
	}
	if s == 0 {
		o.TVUsed, o.TVFrame = true, m.Frame
	} else {
		o.DRCUsed, o.DRCFrame = true, m.Frame
	}
}

func init() {
	c := "coreinit"
	reg(c, "OSScreenInit", func(m *Machine, cpu *CPU, t *Thread) { m.screen.Init = true })
	reg(c, "OSScreenShutdown", func(m *Machine, cpu *CPU, t *Thread) { m.screen.Init = false })
	reg(c, "OSScreenGetBufferSizeEx", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, screenBufferSize(arg(cpu, 0)&1))
	})
	reg(c, "OSScreenSetBufferEx", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0) & 1
		m.screen.Buf[s].Addr = arg(cpu, 1)
		m.screen.Buf[s].Draw = 0
	})
	reg(c, "OSScreenEnableEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.screen.Buf[arg(cpu, 0)&1].Enabled = arg(cpu, 1) != 0
		ret(cpu, 0)
	})
	reg(c, "OSScreenClearBufferEx", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0) & 1
		if m.screen.Buf[s].Addr == 0 {
			return
		}
		_, h, p := screenDims(s)
		col := arg(cpu, 1)
		base := m.screen.frameAddr(s)
		row := make([]byte, p*4)
		for x := uint32(0); x < p; x++ {
			row[x*4], row[x*4+1], row[x*4+2], row[x*4+3] = byte(col>>24), byte(col>>16), byte(col>>8), byte(col)
		}
		for y := uint32(0); y < h; y++ {
			m.Mem.WriteBytes(base+y*p*4, row)
		}
		ret(cpu, 0)
	})
	reg(c, "OSScreenFlipBuffersEx", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0) & 1
		b := &m.screen.Buf[s]
		if b.Addr == 0 {
			return
		}
		b.Draw ^= 1
		b.Shown = true
		m.osScreenShow(s)
		ret(cpu, 0)
	})
	reg(c, "OSScreenPutPixelEx", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0) & 1
		x, y, col := arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		w, h, p := screenDims(s)
		if m.screen.Buf[s].Addr == 0 || x >= w || y >= h {
			return
		}
		m.Mem.Write32(m.screen.frameAddr(s)+(y*p+x)*4, col)
		ret(cpu, 0)
	})
	reg(c, "OSScreenPutFontEx", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0) & 1
		col, row := arg(cpu, 1), arg(cpu, 2)
		str := m.Mem.String(arg(cpu, 3), 1024)
		m.osScreenText(s, col, row, str)
		ret(cpu, 0)
	})
}

// osScreenText draws text at a character position, in white.
func (m *Machine) osScreenText(s, col, row uint32, str string) {
	if m.screen.Buf[s].Addr == 0 {
		return
	}
	w, h, p := screenDims(s)
	base := m.screen.frameAddr(s)
	x0 := col*fontW + 8
	y0 := row*fontH + 4
	for i := 0; i < len(str); i++ {
		ch := str[i]
		gx := x0 + uint32(i)*fontW
		if gx+fontW > w {
			break
		}
		if ch < 32 || ch > 126 {
			continue
		}
		g := fontRows[int(ch-32)*fontH:]
		for yy := uint32(0); yy < fontH; yy++ {
			y := y0 + yy
			if y >= h {
				break
			}
			bits := g[yy]
			if bits == 0 {
				continue
			}
			for xx := uint32(0); xx < fontW; xx++ {
				if bits&(1<<(11-xx)) != 0 {
					m.Mem.Write32(base+(y*p+gx+xx)*4, 0xffffffff)
				}
			}
		}
	}
}

// Picture returns screen 0 (TV) or 1 (GamePad) as the console shows it,
// 0x00RRGGBB.
func (m *Machine) Picture(which int) (pix []uint32, w, h int) {
	if which == 1 {
		return m.screen.DRC, DRCWidth, DRCHeight
	}
	return m.screen.TV, TVWidth, TVHeight
}

// PictureUsed reports whether the program has shown anything on a screen.
func (m *Machine) PictureUsed(which int) bool {
	if which == 1 {
		return m.screen.DRCUsed
	}
	return m.screen.TVUsed
}

// DrawText writes lines of text on a picture (for RetroPixels' own messages,
// such as why a program stopped), in white on a dark band.
func DrawText(pix []uint32, w, h int, lines []string) {
	top := h/2 - len(lines)*fontH/2 - 8
	if top < 0 {
		top = 0
	}
	for y := top; y < top+len(lines)*fontH+16 && y < h; y++ {
		for x := 0; x < w; x++ {
			v := pix[y*w+x]
			pix[y*w+x] = (v >> 2 & 0x3f3f3f) // darken
		}
	}
	for i, line := range lines {
		x0 := (w - len(line)*fontW) / 2
		if x0 < 8 {
			x0 = 8
		}
		y0 := top + 8 + i*fontH
		for j := 0; j < len(line); j++ {
			ch := line[j]
			if ch < 32 || ch > 126 {
				continue
			}
			gx := x0 + j*fontW
			if gx+fontW > w {
				break
			}
			g := fontRows[int(ch-32)*fontH:]
			for yy := 0; yy < fontH && y0+yy < h; yy++ {
				bits := g[yy]
				for xx := 0; xx < fontW; xx++ {
					if bits&(1<<(11-xx)) != 0 {
						pix[(y0+yy)*w+gx+xx] = 0xffffff
					}
				}
			}
		}
	}
}

// WrapText splits s into lines of at most n characters.
func WrapText(s string, n int) []string {
	var out []string
	for _, para := range strings.Split(s, "\n") {
		words := strings.Fields(para)
		line := ""
		for _, w := range words {
			for len(w) > n {
				if line != "" {
					out = append(out, line)
					line = ""
				}
				out = append(out, w[:n])
				w = w[n:]
			}
			if line == "" {
				line = w
			} else if len(line)+1+len(w) <= n {
				line += " " + w
			} else {
				out = append(out, line)
				line = w
			}
		}
		out = append(out, line)
	}
	return out
}
