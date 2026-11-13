package nds

import "encoding/binary"

// ---------------- ARM9 ----------------

type bus9 struct{ m *Machine }

func (b *bus9) Fetch16(a uint32) uint32 { return b.Read16(a) }
func (b *bus9) Fetch32(a uint32) uint32 { return b.Read32(a) }

// sharedWRAM9 is the part of the shared work RAM the ARM9 has.
func (m *Machine) sharedWRAM9() []byte {
	switch m.wramcnt & 3 {
	case 0:
		return m.sharedWR
	case 1:
		return m.sharedWR[16<<10:]
	case 2:
		return m.sharedWR[:16<<10]
	}
	return nil
}

func (m *Machine) sharedWRAM7() []byte {
	switch m.wramcnt & 3 {
	case 1:
		return m.sharedWR[:16<<10]
	case 2:
		return m.sharedWR[16<<10:]
	case 3:
		return m.sharedWR
	}
	return nil
}

// mem9 is the plain memory at a for the ARM9 (nil for I/O and unmapped
// space), with the offset into it.
func (b *bus9) mem(a uint32) ([]byte, uint32) {
	m := b.m
	c := m.cp15
	if a < c.itcmEnd && c.itcmOn {
		return c.itcm, a & 0x7fff
	}
	if a-c.dtcmBase < c.dtcmSize && c.dtcmOn {
		return c.dtcm, (a - c.dtcmBase) & 0x3fff
	}
	switch a >> 24 {
	case 0x02:
		return m.mainRAM, a & 0x3fffff
	case 0x03:
		w := m.sharedWRAM9()
		if w == nil {
			return nil, 0
		}
		return w, a & uint32(len(w)-1)
	case 0x05:
		return m.pal, a & 0x7ff
	case 0x07:
		return m.oam, a & 0x7ff
	case 0xff:
		if a >= 0xffff0000 {
			return m.bios9, a & 0xfff
		}
	}
	return nil, 0
}

func (b *bus9) Read8(a uint32) uint32 {
	if p, o := b.mem(a); p != nil {
		return uint32(p[o])
	}
	switch a >> 24 {
	case 0x04:
		return uint32(b.m.io9Read8(a))
	case 0x06:
		r, o := b.m.vram.arm9Region(a)
		if r == nil {
			return 0
		}
		return uint32(r.read8(o))
	case 0x08, 0x09, 0x0a:
		return 0xff // no GBA cartridge
	}
	return 0
}

func (b *bus9) Read16(a uint32) uint32 {
	a &^= 1
	if p, o := b.mem(a); p != nil {
		return uint32(binary.LittleEndian.Uint16(p[o:]))
	}
	switch a >> 24 {
	case 0x04:
		return uint32(b.m.io9Read16(a))
	case 0x06:
		r, o := b.m.vram.arm9Region(a)
		if r == nil {
			return 0
		}
		return uint32(r.read16(o))
	case 0x08, 0x09, 0x0a:
		return 0xffff
	}
	return 0
}

func (b *bus9) Read32(a uint32) uint32 {
	a &^= 3
	if p, o := b.mem(a); p != nil {
		return binary.LittleEndian.Uint32(p[o:])
	}
	switch a >> 24 {
	case 0x04:
		return b.m.io9Read32(a)
	case 0x06:
		r, o := b.m.vram.arm9Region(a)
		if r == nil {
			return 0
		}
		return r.read32(o)
	case 0x08, 0x09, 0x0a:
		return 0xffffffff
	}
	return 0
}

func (b *bus9) Write8(a, v uint32) {
	switch a >> 24 {
	case 0x04:
		if !b.inTCM(a) {
			b.m.io9Write8(a, uint8(v))
			return
		}
	case 0x05, 0x07: // 8-bit writes to palettes and OAM are ignored
		if !b.inTCM(a) {
			return
		}
	case 0x06:
		if !b.inTCM(a) {
			if r, o := b.m.vram.arm9Region(a); r != nil {
				r.write8(o, uint8(v))
			}
			return
		}
	}
	if p, o := b.mem(a); p != nil && !b.readOnly(a) {
		p[o] = uint8(v)
	}
}

func (b *bus9) Write16(a, v uint32) {
	a &^= 1
	switch a >> 24 {
	case 0x04:
		if !b.inTCM(a) {
			b.m.io9Write16(a, uint16(v))
			return
		}
	case 0x06:
		if !b.inTCM(a) {
			if r, o := b.m.vram.arm9Region(a); r != nil {
				r.write16(o, uint16(v))
			}
			return
		}
	}
	if p, o := b.mem(a); p != nil && !b.readOnly(a) {
		binary.LittleEndian.PutUint16(p[o:], uint16(v))
	}
}

func (b *bus9) Write32(a, v uint32) {
	a &^= 3
	switch a >> 24 {
	case 0x04:
		if !b.inTCM(a) {
			b.m.io9Write32(a, v)
			return
		}
	case 0x06:
		if !b.inTCM(a) {
			if r, o := b.m.vram.arm9Region(a); r != nil {
				r.write32(o, v)
			}
			return
		}
	}
	if p, o := b.mem(a); p != nil && !b.readOnly(a) {
		binary.LittleEndian.PutUint32(p[o:], v)
	}
}

func (b *bus9) inTCM(a uint32) bool {
	c := b.m.cp15
	return a < c.itcmEnd && c.itcmOn || a-c.dtcmBase < c.dtcmSize && c.dtcmOn
}

func (b *bus9) readOnly(a uint32) bool { return a >= 0xffff0000 && !b.inTCM(a) }

// ---------------- ARM7 ----------------

type bus7 struct{ m *Machine }

func (b *bus7) Fetch16(a uint32) uint32 { return b.Read16(a) }
func (b *bus7) Fetch32(a uint32) uint32 { return b.Read32(a) }

func (b *bus7) mem(a uint32) ([]byte, uint32) {
	m := b.m
	switch a >> 24 {
	case 0x00:
		if a < 0x4000 {
			return m.bios7, a
		}
	case 0x02:
		return m.mainRAM, a & 0x3fffff
	case 0x03:
		if a < 0x03800000 {
			if w := m.sharedWRAM7(); w != nil {
				return w, a & uint32(len(w)-1)
			}
		}
		return m.arm7WRAM, a & 0xffff
	}
	return nil, 0
}

func (b *bus7) Read8(a uint32) uint32 {
	if p, o := b.mem(a); p != nil {
		return uint32(p[o])
	}
	switch a >> 24 {
	case 0x04:
		return uint32(b.m.io7Read8(a))
	case 0x06:
		return uint32(b.m.vram.arm7.read8(a & 0x3ffff))
	case 0x08, 0x09, 0x0a:
		return 0xff
	}
	return 0
}

func (b *bus7) Read16(a uint32) uint32 {
	a &^= 1
	if p, o := b.mem(a); p != nil {
		return uint32(binary.LittleEndian.Uint16(p[o:]))
	}
	switch a >> 24 {
	case 0x04:
		return uint32(b.m.io7Read16(a))
	case 0x06:
		return uint32(b.m.vram.arm7.read16(a & 0x3ffff))
	case 0x08, 0x09, 0x0a:
		return 0xffff
	}
	return 0
}

func (b *bus7) Read32(a uint32) uint32 {
	a &^= 3
	if p, o := b.mem(a); p != nil {
		return binary.LittleEndian.Uint32(p[o:])
	}
	switch a >> 24 {
	case 0x04:
		return b.m.io7Read32(a)
	case 0x06:
		return b.m.vram.arm7.read32(a & 0x3ffff)
	case 0x08, 0x09, 0x0a:
		return 0xffffffff
	}
	return 0
}

func (b *bus7) Write8(a, v uint32) {
	switch a >> 24 {
	case 0x00:
		return
	case 0x04:
		b.m.io7Write8(a, uint8(v))
		return
	case 0x06:
		b.m.vram.arm7.write8(a&0x3ffff, uint8(v))
		return
	}
	if p, o := b.mem(a); p != nil {
		p[o] = uint8(v)
	}
}

func (b *bus7) Write16(a, v uint32) {
	a &^= 1
	switch a >> 24 {
	case 0x00:
		return
	case 0x04:
		b.m.io7Write16(a, uint16(v))
		return
	case 0x06:
		b.m.vram.arm7.write16(a&0x3ffff, uint16(v))
		return
	}
	if p, o := b.mem(a); p != nil {
		binary.LittleEndian.PutUint16(p[o:], uint16(v))
	}
}

func (b *bus7) Write32(a, v uint32) {
	a &^= 3
	switch a >> 24 {
	case 0x00:
		return
	case 0x04:
		b.m.io7Write32(a, v)
		return
	case 0x06:
		b.m.vram.arm7.write32(a&0x3ffff, v)
		return
	}
	if p, o := b.mem(a); p != nil {
		binary.LittleEndian.PutUint32(p[o:], v)
	}
}
