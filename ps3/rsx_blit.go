package ps3

// The RSX's 2D engines, on their fixed subchannels: NV0039 (memory to
// memory, subchannel 1), NV3062 (2D destination surface, 3), NV309E
// (swizzled surface, 4), NV308A (image from the CPU, 5) and NV3089 (scaled
// image from memory, 6).

func (m *Machine) rsxBlitMethod(r *rsxState, d *rsxDraw, sub, reg, v uint32) {
	b := &d.blit
	switch sub {
	case 1: // NV0039
		switch reg {
		case 0x184:
			b.mmSrcDMA = v
		case 0x188:
			b.mmDstDMA = v
		case 0x30C:
			b.mmIn = v
		case 0x310:
			b.mmOut = v
		case 0x314:
			b.mmPitchIn = v
		case 0x318:
			b.mmPitchOut = v
		case 0x31C:
			b.mmLen = v
		case 0x320:
			b.mmN = v
		case 0x324:
			b.mmFormat = v
		case 0x328:
			m.rsxMemCopy(b)
		}
	case 3: // NV3062
		switch reg {
		case 0x184:
			// source DMA (unused: NV3089 gives the source)
		case 0x188:
			b.dstDMA = v
		case 0x300:
			b.dstFormat = v
		case 0x304:
			b.pitchIn, b.pitchOut = v&0xffff, v>>16
		case 0x308:
			b.srcOff = v
		case 0x30C:
			b.dstOff = v
		}
	case 5: // NV308A
		switch {
		case reg == 0x304:
			b.outX, b.outY = int(v&0xffff), int(v>>16)
		case reg == 0x308:
			b.outW, b.outH = int(v&0xffff), int(v>>16)
		case reg == 0x30C:
			b.inW, b.inH = v&0xffff, v>>16
		case reg >= 0x400 && reg < 0x2000:
			i := int(reg-0x400) / 4
			if i >= b.outW && b.outW != 0 {
				return
			}
			x, y := b.outX+i, b.outY
			switch b.dstFormat {
			case 4: // R5G6B5
				a := m.dmaAddr(b.dstDMA, b.dstOff+uint32(y)*b.pitchOut+uint32(x)*2)
				rr, g, bb := v>>16&0xff, v>>8&0xff, v&0xff
				m.Mem.Write16(a, uint16(rr*32/255<<11|g*64/255<<5|bb*32/255))
			default:
				a := m.dmaAddr(b.dstDMA, b.dstOff+uint32(y)*b.pitchOut+uint32(x)*4)
				m.Mem.Write32(a, v)
			}
		}
	case 6: // NV3089
		switch reg {
		case 0x184:
			b.srcDMA = v
		case 0x2FC:
			b.conv = v
		case 0x300:
			b.format = v
		case 0x304:
			b.op = v
		case 0x308:
			b.clipX, b.clipY = int(v&0xffff), int(v>>16)
		case 0x30C:
			b.clipW, b.clipH = int(v&0xffff), int(v>>16)
		case 0x310:
			b.outX, b.outY = int(v&0xffff), int(v>>16)
		case 0x314:
			b.outW, b.outH = int(v&0xffff), int(v>>16)
		case 0x318:
			b.dsdx = int32(v)
		case 0x31C:
			b.dtdy = int32(v)
		case 0x400:
			b.inW, b.inH = v&0xffff, v>>16
		case 0x404:
			b.inPitch, b.inOrigin = v&0xffff, v>>16&0xff
		case 0x408:
			b.inOff = v
		case 0x40C:
			b.inX, b.inY = v&0xffff, v>>16
			m.rsxScaledImage(b)
		}
	}
}

func (m *Machine) rsxMemCopy(b *blitState) {
	if rsxTrace {
		m.Logf("RSX copy %x->%x len %d lines %d pitch %d/%d", b.mmIn, b.mmOut, b.mmLen, b.mmN, b.mmPitchIn, b.mmPitchOut)
	}
	if b.mmLen == 0 || b.mmLen > 1<<24 {
		return
	}
	buf := make([]byte, b.mmLen)
	for i := uint32(0); i < b.mmN; i++ {
		src := m.dmaAddr(b.mmSrcDMA, b.mmIn+i*b.mmPitchIn)
		dst := m.dmaAddr(b.mmDstDMA, b.mmOut+i*b.mmPitchOut)
		if src == 0 || dst == 0 {
			return
		}
		m.Mem.ReadBytes(src, buf)
		m.Mem.WriteBytes(dst, buf)
	}
}

func (m *Machine) rsxScaledImage(b *blitState) {
	if rsxTrace {
		m.Logf("RSX scaled image fmt %d in %dx%d pitch %d off %x at (%d,%d)/16 -> out (%d,%d) %dx%d clip (%d,%d) %dx%d dst %x pitch %d fmt %d dsdx %x dtdy %x",
			b.format, b.inW, b.inH, b.inPitch, b.inOff, b.inX, b.inY, b.outX, b.outY, b.outW, b.outH, b.clipX, b.clipY, b.clipW, b.clipH, b.dstOff, b.pitchOut, b.dstFormat, b.dsdx, b.dtdy)
	}
	if b.dsdx == 0 || b.dtdy == 0 || b.inW == 0 || b.inH == 0 {
		return
	}
	inBpp := uint32(4)
	if b.format == 7 || b.format == 1 || b.format == 2 {
		inBpp = 2
	} else if b.format == 8 {
		inBpp = 1
	}
	outBpp := uint32(4)
	if b.dstFormat == 4 {
		outBpp = 2
	}
	pitch := b.inPitch
	if pitch == 0 {
		pitch = b.inW * inBpp
	}
	src := m.dmaAddr(b.srcDMA, b.inOff)
	if src == 0 {
		return
	}
	fx, fy := float64(b.inX)/16, float64(b.inY)/16
	if b.inOrigin == 1 { // centre
		fx -= 0.5
		fy -= 0.5
	}
	sx, sy := float64(b.dsdx)/(1<<20), float64(b.dtdy)/(1<<20)
	x0, y0 := max(b.outX, b.clipX), max(b.outY, b.clipY)
	x1, y1 := min(b.outX+b.outW, b.clipX+b.clipW), min(b.outY+b.outH, b.clipY+b.clipH)
	// same format, scale 1: copy rows
	if sx == 1 && sy == 1 && inBpp == outBpp && (b.format == 3 || b.format == 4) == (outBpp == 4) {
		ix := int(fx+0.5) + (x0 - b.outX)
		iy := int(fy+0.5) + (y0 - b.outY)
		n := x1 - x0
		if ix < 0 || n <= 0 {
			return
		}
		if ix+n > int(b.inW) {
			n = int(b.inW) - ix
		}
		row := make([]byte, n*int(inBpp))
		for y := y0; y < y1; y++ {
			sy := iy + (y - y0)
			if sy < 0 || sy >= int(b.inH) {
				continue
			}
			m.Mem.ReadBytes(src+uint32(sy)*pitch+uint32(ix)*inBpp, row)
			m.Mem.WriteBytes(m.dmaAddr(b.dstDMA, b.dstOff+uint32(y)*b.pitchOut+uint32(x0)*outBpp), row)
		}
		return
	}
	for y := y0; y < y1; y++ {
		v := int(fy + (float64(y-b.outY)+0.5)*sy)
		if v < 0 {
			v = 0
		}
		if v >= int(b.inH) {
			v = int(b.inH) - 1
		}
		for x := x0; x < x1; x++ {
			u := int(fx + (float64(x-b.outX)+0.5)*sx)
			if u < 0 {
				u = 0
			}
			if u >= int(b.inW) {
				u = int(b.inW) - 1
			}
			a := src + uint32(v)*pitch + uint32(u)*inBpp
			var argb uint32
			switch inBpp {
			case 2:
				c := uint32(m.Mem.Read16(a))
				argb = 0xff000000 | (c>>11&31)*255/31<<16 | (c>>5&63)*255/63<<8 | (c&31)*255/31
			case 1:
				c := uint32(m.Mem.Read8(a))
				argb = 0xff000000 | c<<16 | c<<8 | c
			default:
				argb = m.Mem.Read32(a)
				if b.format == 12 || b.format == 13 {
					argb = argb&0xff00ff00 | argb>>16&0xff | argb&0xff<<16
				}
			}
			da := m.dmaAddr(b.dstDMA, b.dstOff+uint32(y)*b.pitchOut+uint32(x)*outBpp)
			if outBpp == 2 {
				rr, g, bb := argb>>16&0xff, argb>>8&0xff, argb&0xff
				m.Mem.Write16(da, uint16(rr>>3<<11|g>>2<<5|bb>>3))
			} else {
				m.Mem.Write32(da, argb)
			}
		}
	}
}
