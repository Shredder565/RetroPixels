package ctr

// gpu is the PICA200. In this stage it keeps its registers and reads
// command lists; drawing comes with the GPU stage.
type gpu struct {
	m   *Machine
	reg [0x400]uint32
}

func newGPU(m *Machine) *gpu { return &gpu{m: m} }

func (g *gpu) writeReg(id, v, mask uint32) {
	if id < uint32(len(g.reg)) {
		g.reg[id] = g.reg[id]&^mask | v&mask
	}
}

// runCommandList runs a command list at physical address pa: each
// command is a parameter word then a header (register, byte mask, extra
// parameter count, consecutive flag).
func (g *gpu) runCommandList(pa, size uint32) {
	buf := g.m.mem.physSlice(pa, size)
	if buf == nil {
		return
	}
	word := func(i uint32) uint32 {
		return uint32(buf[i]) | uint32(buf[i+1])<<8 | uint32(buf[i+2])<<16 | uint32(buf[i+3])<<24
	}
	for i := uint32(0); i+8 <= size; {
		param, hdr := word(i), word(i+4)
		i += 8
		id := hdr & 0x3ff
		mask := expandMask(hdr >> 16 & 15)
		extra := hdr >> 20 & 0xff
		seq := hdr&(1<<31) != 0
		g.writeReg(id, param, mask)
		for n := uint32(0); n < extra && i+4 <= size; n++ {
			if seq {
				id++
			}
			g.writeReg(id, word(i), mask)
			i += 4
		}
		if extra&1 != 0 { // keep 8-byte alignment
			i += 4
		}
	}
}

func expandMask(m uint32) uint32 {
	var r uint32
	for i := uint32(0); i < 4; i++ {
		if m>>i&1 != 0 {
			r |= 0xff << (8 * i)
		}
	}
	return r
}
