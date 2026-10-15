package gba

// Memory domains for Archipelago's BizHawk Client (core.Memory), named and
// laid out like BizHawk's mGBA core, as web/emu/archipelago.js does.

import "retropixels/core"

func (g *GBA) peek(a uint32) byte {
	switch a >> 24 {
	case 0:
		if a < 0x4000 {
			return g.bios[a]
		}
	case 2:
		return g.ewram[a&0x3ffff]
	case 3:
		return g.iwram[a&0x7fff]
	case 4:
		if a&0xffffff < 0x400 {
			return byte(g.ioRead16(int(a&0x3fe)) >> ((a & 1) * 8))
		}
	case 5:
		return g.pal[a&0x3ff]
	case 6:
		return g.vram[vaddr(a)]
	case 7:
		return g.oam[a&0x3ff]
	case 8, 9, 10, 11, 12, 13:
		if o := int(a & 0x1ffffff); o < g.romSize {
			return g.rom[o]
		}
	case 14, 15:
		if s := g.save.data; len(s) > 0 {
			return s[int(a&0xffff)%len(s)]
		}
	}
	return 0
}

func (g *GBA) poke(a uint32, v byte) {
	switch a >> 24 {
	case 2:
		g.ewram[a&0x3ffff] = v
	case 3:
		g.iwram[a&0x7fff] = v
	case 5:
		g.pal[a&0x3ff] = v
	case 6:
		g.vram[vaddr(a)] = v
	case 7:
		g.oam[a&0x3ff] = v
	}
}

func (g *GBA) domains() core.Domains {
	return core.Domains{
		{Name: "ROM", Mem: g.rom, Size: g.romSize},
		{Name: "EWRAM", Mem: g.ewram[:]},
		{Name: "IWRAM", Mem: g.iwram[:]},
		{Name: "BIOS", Mem: g.bios[:]},
		{Name: "PALRAM", Mem: g.pal[:]},
		{Name: "VRAM", Mem: g.vram[:]},
		{Name: "OAM", Mem: g.oam[:]},
		{Name: "SRAM", Mem: g.save.data, Wrote: func() { g.save.dirty = true }},
		{Name: "System Bus", Size: 0x10000000, Peek: g.peek, Poke: g.poke},
	}
}

func (g *GBA) MemoryDomains() []string      { return g.domains().MemoryDomains() }
func (g *GBA) MemorySize(domain string) int { return g.domains().MemorySize(domain) }
func (g *GBA) ReadMemory(domain string, addr uint32, buf []byte) error {
	return g.domains().ReadMemory(domain, addr, buf)
}
func (g *GBA) WriteMemory(domain string, addr uint32, data []byte) error {
	return g.domains().WriteMemory(domain, addr, data)
}
