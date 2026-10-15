package nes

// Memory domains for Archipelago's BizHawk Client (core.Memory), named and
// laid out like BizHawk's NesHawk core, as web/emu/archipelago.js does.

import "retropixels/core"

func (n *NES) busPeek(a uint32) byte {
	switch {
	case a < 0x2000:
		return n.ram[a&0x7ff]
	case a < 0x4020: // PPU / sound / controller registers: reading them changes them
		return 0
	}
	if v := n.m.cpuRead(int(a)); v >= 0 {
		return byte(v)
	}
	return 0
}

func (n *NES) busPoke(a uint32, v byte) {
	switch {
	case a < 0x2000:
		n.ram[a&0x7ff] = v
	case a >= 0x6000 && a < 0x8000 && n.mb.prgRam != nil:
		n.mb.prgRam[a&0x1fff] = v
	}
}

func (n *NES) domains() core.Domains {
	m := n.mb
	d := core.Domains{
		{Name: "RAM", Mem: n.ram[:]},
		{Name: "PRG ROM", Mem: m.prg},
		{Name: "WRAM", Mem: m.prgRam},
		{Name: "CIRAM (nametables)", Mem: n.ppu.vram, Size: 0x800},
		{Name: "OAM", Mem: n.ppu.oam[:]},
		{Name: "PALRAM", Mem: n.ppu.palRam[:]},
		{Name: "System Bus", Size: 0x10000, Peek: n.busPeek, Poke: n.busPoke},
	}
	if n.battery {
		d = append(d, core.Domain{Name: "Battery RAM", Mem: m.prgRam})
	}
	chr := "CHR VROM"
	if m.chrRam {
		chr = "CHR VRAM"
	}
	return append(d, core.Domain{Name: chr, Mem: m.chr})
}

func (n *NES) MemoryDomains() []string      { return n.domains().MemoryDomains() }
func (n *NES) MemorySize(domain string) int { return n.domains().MemorySize(domain) }
func (n *NES) ReadMemory(domain string, addr uint32, buf []byte) error {
	return n.domains().ReadMemory(domain, addr, buf)
}
func (n *NES) WriteMemory(domain string, addr uint32, data []byte) error {
	return n.domains().WriteMemory(domain, addr, data)
}
