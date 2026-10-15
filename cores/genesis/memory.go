package genesis

// Memory domains for Archipelago's BizHawk Client (core.Memory), named like
// BizHawk's Genplus-gx core, as web/emu/archipelago.js does.

import "retropixels/core"

func (g *Genesis) busPeek(a uint32) byte {
	switch {
	case a >= 0xe00000:
		return g.ram[a&0xffff]
	case int(a) < g.romSize && a < 0x400000:
		return g.rom[a]
	}
	return 0
}

func (g *Genesis) busPoke(a uint32, v byte) {
	if a >= 0xe00000 {
		g.ram[a&0xffff] = v
	}
}

func (g *Genesis) domains() core.Domains {
	return core.Domains{
		{Name: "68K RAM", Mem: g.ram[:]},
		{Name: "Z80 RAM", Mem: g.zram[:]},
		{Name: "MD CART", Mem: g.rom, Size: g.romSize},
		{Name: "SRAM", Mem: g.cart.sram[:]},
		{Name: "M68K BUS", Size: 0x1000000, Peek: g.busPeek, Poke: g.busPoke},
	}
}

func (g *Genesis) MemoryDomains() []string      { return g.domains().MemoryDomains() }
func (g *Genesis) MemorySize(domain string) int { return g.domains().MemorySize(domain) }
func (g *Genesis) ReadMemory(domain string, addr uint32, buf []byte) error {
	return g.domains().ReadMemory(domain, addr, buf)
}
func (g *Genesis) WriteMemory(domain string, addr uint32, data []byte) error {
	return g.domains().WriteMemory(domain, addr, data)
}
