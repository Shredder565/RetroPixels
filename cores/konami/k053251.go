package konami

import "retropixels/cores/arcade"

// K053251 is the priority / colour mixer.
type K053251 struct {
	Regs [16]uint8
}

func (m *K053251) Reset()                 { m.Regs = [16]uint8{} }
func (m *K053251) Write(reg int, v uint8) { m.Regs[reg&15] = v & 0x3f }
func (m *K053251) Read(reg int) uint8     { return m.Regs[reg&15] }

// Priority is the register priority of input ci.
func (m *K053251) Priority(ci int) int { return int(m.Regs[ci] & 0x3f) }

// PaletteBase is input ci's palette offset in colours.
func (m *K053251) PaletteBase(ci int) int {
	r9, r10 := int(m.Regs[9]), int(m.Regs[10])
	switch ci {
	case 0:
		return 32 * (r9 & 3)
	case 1:
		return 32 * ((r9 >> 2) & 3)
	case 2:
		return 32 * ((r9 >> 4) & 3)
	case 3:
		return 16 * (r10 & 7)
	}
	return 16 * ((r10 >> 3) & 7)
}

// MixLayer is one input of Mix. Pri is the fixed priority when PriArr is nil.
type MixLayer struct {
	CI        int
	Pens      []uint16
	Pri       int
	PriArr    []uint8
	Shadow    []uint8
	ShadowPri bool
}

// Mix picks every pixel's winning pen (see k053251.js).
func (m *K053251) Mix(out []uint16, shade []uint8, n int, layers []MixLayer, bgPen int) {
	anyApart := false
	for _, l := range layers {
		if l.ShadowPri {
			anyApart = true
		}
	}
	for i := 0; i < n; i++ {
		best, pen, sh, from, hasSh := 255, uint16(bgPen), 255, -1, false
		for k := range layers {
			l := &layers[k]
			p := l.Pri
			if l.PriArr != nil {
				p = int(l.PriArr[i])
			}
			if s := l.Shadow; s != nil && s[i] != 0 {
				hasSh = true
				if !l.ShadowPri {
					if p < sh {
						sh = p
					}
					continue
				}
				if int(s[i])-1 < sh {
					sh = int(s[i]) - 1
				}
			}
			v := l.Pens[i]
			if v&15 == 0 {
				continue
			}
			if p < best {
				best, pen, from = p, v, k
			}
		}
		out[i] = pen
		shade[i] = 0
		if hasSh && sh <= best && !(anyApart && (from < 0 || layers[from].ShadowPri)) {
			shade[i] = 1
		}
	}
}

func (m *K053251) AppendState(w *arcade.StateW) { w.Bytes(m.Regs[:]) }
func (m *K053251) LoadState(r *arcade.StateR)   { r.Bytes(m.Regs[:]) }
