package nds

// Display capture: engine A's picture (or the 3D picture alone) and a VRAM
// bank, one of them or the two blended, written into a VRAM bank, line by
// line during the frame. Games use it for 3D on both screens and for
// motion blur.

var captureSizes = [4][2]int{{128, 128}, {256, 64}, {256, 128}, {256, 192}}

func (m *Machine) captureLine(y int) {
	c := m.dispCapCnt
	sz := captureSizes[c>>20&3]
	w, h := sz[0], sz[1]
	if y >= h {
		return
	}
	db := int(c >> 16 & 3)
	if m.vram.cnt[db]&0x87 != 0x80 { // the bank must be enabled for the LCD
		return
	}
	dst := m.vram.banks[db]
	var a, b [256]uint32 // bit 15: alpha
	if c&(1<<24) != 0 {
		row := m.gpu3d.out[y*256 : y*256+256]
		for x, v := range row {
			a[x] = v & 0x7fff
			if v>>16&31 != 0 {
				a[x] |= 0x8000
			}
		}
	} else {
		for x := range a {
			a[x] = m.gpuA.raw[x] | 0x8000
		}
	}
	if c&(1<<25) == 0 {
		src := m.vram.banks[m.gpuA.dispcnt>>18&3]
		off := int(c>>26&3)*0x8000 + y*w*2
		for x := 0; x < w; x++ {
			i := (off + x*2) & (len(src) - 1)
			b[x] = uint32(src[i]) | uint32(src[i+1])<<8
		}
	}
	mode := c >> 29 & 3
	eva, evb := c&31, c>>8&31
	if eva > 16 {
		eva = 16
	}
	if evb > 16 {
		evb = 16
	}
	off := int(c>>18&3)*0x8000 + y*w*2
	for x := 0; x < w; x++ {
		var o uint32
		switch mode {
		case 0:
			o = a[x]
		case 1:
			o = b[x]
		default:
			aa, ab := a[x]>>15, b[x]>>15
			ch := func(sh uint) uint32 {
				v := ((a[x]>>sh&31)*aa*eva + (b[x]>>sh&31)*ab*evb) / 16
				if v > 31 {
					v = 31
				}
				return v
			}
			o = ch(0) | ch(5)<<5 | ch(10)<<10
			if eva != 0 && aa != 0 || evb != 0 && ab != 0 {
				o |= 0x8000
			}
		}
		i := (off + x*2) & (len(dst) - 1)
		dst[i] = uint8(o)
		dst[i+1] = uint8(o >> 8)
	}
	m.vram.dirtyTex = true
}
