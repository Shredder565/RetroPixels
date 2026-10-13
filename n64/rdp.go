package n64

// RDP: the Reality Display Processor (rasterizer). Commands arrive through
// the DP registers from RDRAM or DMEM. This file holds the registers, the
// command decoder and the render state; rdp_tmem.go loads and samples
// textures, rdp_raster.go walks triangle and rectangle edges, and
// rdp_pixel.go colours, blends and writes pixels.

type RDP struct {
	m *Machine
	rdpRegs

	dirty bool     // modes changed since the pixel pipeline was set up
	pp    rdpPipe  // per-primitive pixel pipeline state
	sp    rdpSpans // edge walker output for the current primitive
}

// rdpTile is one of the 8 tile descriptors (Set Tile + Set Tile Size).
type rdpTile struct {
	Fmt, Size, Line, Tmem, Pal     uint32
	ClampT, MirrorT, MaskT, ShiftT uint32
	ClampS, MirrorS, MaskS, ShiftS uint32
	SL, TL, SH, TH                 uint32 // 10.2
}

// rdpImage is a Set Color Image / Set Texture Image target.
type rdpImage struct {
	Fmt, Size, Width, Addr uint32
}

// rdpRegs is everything the commands set: the whole resumable RDP state.
// Fields are exported only so encoding/binary can save them.
type rdpRegs struct {
	Tmem  [4096]byte
	Tiles [8]rdpTile

	Color, Tex rdpImage
	ZAddr      uint32

	ScXH, ScYH, ScXL, ScYL uint32 // 10.2
	ScField, ScOdd         uint32

	Modes   uint64 // Set Other Modes word
	Combine uint64 // Set Combine word

	Fill, Fog, Blend, Prim, Env uint32
	PrimMinLevel, PrimLodFrac   uint32
	PrimZ, PrimDZ               uint32
	KeyR, KeyGB, Convert        uint64 // raw command words

	Noise uint32

	// a command that was cut by DPC_END, waiting for the rest of its words
	Cmd  [22]uint64
	CmdN uint32
}

func (d *RDP) reset() {
	d.rdpRegs = rdpRegs{Noise: 1}
	d.dirty = true
}

func (m *Machine) dpRead(r uint32) uint32 {
	dp := &m.dp
	switch r {
	case 0:
		return dp.start
	case 1:
		return dp.end
	case 2:
		return dp.current
	case 3:
		return dp.status | 0x80 // command buffer ready
	case 4:
		return dp.clock
	}
	return 0
}

func (m *Machine) dpWrite(r uint32, v uint32) {
	dp := &m.dp
	switch r {
	case 0:
		if dp.status&0x400 == 0 {
			dp.start = v & 0xfffff8
			dp.current = dp.start
		}
	case 1:
		dp.end = v & 0xfffff8
		m.RDP.process()
	case 3:
		s := dp.status
		if v&1 != 0 {
			s &^= 1
		}
		if v&2 != 0 {
			s |= 1
		}
		if v&4 != 0 {
			s &^= 2
		}
		if v&8 != 0 {
			s |= 2
		}
		if v&16 != 0 {
			s &^= 4
		}
		if v&32 != 0 {
			s |= 4
		}
		if v&0x200 != 0 {
			dp.clock = 0
		}
		unfreeze := dp.status&2 != 0 && s&2 == 0
		dp.status = s
		if unfreeze {
			m.RDP.process()
		}
	}
}

// process runs the commands between DPC_CURRENT and DPC_END. A command whose
// words are not all there yet waits in Cmd for the next DPC_END write.
func (d *RDP) process() {
	m := d.m
	dp := &m.dp
	if dp.status&2 != 0 { // frozen
		return
	}
	for dp.current < dp.end {
		var w uint64
		if dp.status&1 != 0 { // from DMEM
			a := dp.current & 0xff8
			w = uint64(m.read32(0x04000000|a))<<32 | uint64(m.read32(0x04000000|(a+4)))
		} else {
			a := dp.current & 0xfffff8
			if a+8 > rdramSize {
				break
			}
			w = uint64(m.read32(a))<<32 | uint64(m.read32(a+4))
		}
		dp.current += 8
		d.Cmd[d.CmdN] = w
		d.CmdN++
		if int(d.CmdN) >= rdpCmdLen((d.Cmd[0]>>56)&0x3f) {
			d.CmdN = 0
			d.command(d.Cmd[:])
		}
	}
}

func rdpCmdLen(op uint64) int {
	switch op {
	case 0x08:
		return 4
	case 0x09:
		return 6
	case 0x0a:
		return 12
	case 0x0b:
		return 14
	case 0x0c:
		return 12
	case 0x0d:
		return 14
	case 0x0e:
		return 20
	case 0x0f:
		return 22
	case 0x24, 0x25:
		return 2
	}
	return 1
}

// command runs one whole command.
func (d *RDP) command(c []uint64) {
	w := c[0]
	op := (w >> 56) & 0x3f
	switch op {
	case 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f:
		d.triangle(c, op&4 != 0, op&2 != 0, op&1 != 0)
	case 0x24, 0x25:
		d.texRect(w, c[1], op == 0x25)
	case 0x29: // full sync
		d.m.raise(intDP)
	case 0x2a:
		d.KeyGB = w
	case 0x2b:
		d.KeyR = w
	case 0x2c:
		d.Convert = w
		d.dirty = true
	case 0x2d:
		d.ScXH = uint32(w>>44) & 0xfff
		d.ScYH = uint32(w>>32) & 0xfff
		d.ScField = uint32(w>>25) & 1
		d.ScOdd = uint32(w>>24) & 1
		d.ScXL = uint32(w>>12) & 0xfff
		d.ScYL = uint32(w) & 0xfff
	case 0x2e:
		d.PrimZ = uint32(w>>16) & 0xffff
		d.PrimDZ = uint32(w) & 0xffff
	case 0x2f:
		d.Modes = w
		d.dirty = true
	case 0x30:
		d.loadTLUT(w)
	case 0x32:
		t := &d.Tiles[(w>>24)&7]
		t.SL, t.TL = uint32(w>>44)&0xfff, uint32(w>>32)&0xfff
		t.SH, t.TH = uint32(w>>12)&0xfff, uint32(w)&0xfff
	case 0x33:
		d.loadBlock(w)
	case 0x34:
		d.loadTile(w)
	case 0x35:
		t := &d.Tiles[(w>>24)&7]
		t.Fmt = uint32(w>>53) & 7
		t.Size = uint32(w>>51) & 3
		t.Line = uint32(w>>41) & 0x1ff
		t.Tmem = uint32(w>>32) & 0x1ff
		t.Pal = uint32(w>>20) & 15
		t.ClampT = uint32(w>>19) & 1
		t.MirrorT = uint32(w>>18) & 1
		t.MaskT = uint32(w>>14) & 15
		t.ShiftT = uint32(w>>10) & 15
		t.ClampS = uint32(w>>9) & 1
		t.MirrorS = uint32(w>>8) & 1
		t.MaskS = uint32(w>>4) & 15
		t.ShiftS = uint32(w) & 15
	case 0x36:
		d.fillRect(w)
	case 0x37:
		d.Fill = uint32(w)
	case 0x38:
		d.Fog = uint32(w)
	case 0x39:
		d.Blend = uint32(w)
	case 0x3a:
		d.Prim = uint32(w)
		d.PrimMinLevel = uint32(w>>40) & 31
		d.PrimLodFrac = uint32(w>>32) & 0xff
	case 0x3b:
		d.Env = uint32(w)
	case 0x3c:
		d.Combine = w
		d.dirty = true
	case 0x3d:
		d.Tex = rdpImage{uint32(w>>53) & 7, uint32(w>>51) & 3, uint32(w>>32)&0x3ff + 1, uint32(w) & 0x3ffffff}
	case 0x3e:
		d.ZAddr = uint32(w) & 0x3ffffff
	case 0x3f:
		d.Color = rdpImage{uint32(w>>53) & 7, uint32(w>>51) & 3, uint32(w>>32)&0x3ff + 1, uint32(w) & 0x3ffffff}
	}
}
