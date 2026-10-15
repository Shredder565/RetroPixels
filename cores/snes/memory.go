package snes

// Memory domains for Archipelago's SNI Client (core.Memory), named and laid
// out like BizHawk's bsnes core, as web/emu/archipelago.js does; internal/
// apbridge maps the usb2snes address space onto them.

import "retropixels/core"

// cartRAM: the cartridge's RAM: the Super FX chip's, the SA-1's BW-RAM, or
// plain save RAM (the battery save is made from these same bytes).
func (s *SNES) cartRAM() []byte {
	switch {
	case s.gsu != nil:
		return s.gsu.ram
	case s.sa1 != nil:
		return s.sa1.bw
	case s.cart.sramSize > 0:
		return s.cart.sram[:s.cart.sramSize]
	}
	return nil
}

// words: 16-bit memory seen as little-endian bytes (VRAM, colours).
func words(w []uint16) core.Domain {
	return core.Domain{
		Size: len(w) * 2,
		Peek: func(a uint32) byte { return byte(w[a>>1] >> ((a & 1) * 8)) },
		Poke: func(a uint32, v byte) {
			k := a >> 1
			if a&1 != 0 {
				w[k] = w[k]&0x00ff | uint16(v)<<8
			} else {
				w[k] = w[k]&0xff00 | uint16(v)
			}
		},
	}
}

func (s *SNES) busPoke(a uint32, v byte) {
	bank, off := a>>16, a&0xffff
	switch {
	case bank == 0x7e || bank == 0x7f:
		s.wram[a&0x1ffff] = v
	case bank&0x40 == 0 && off < 0x2000:
		s.wram[off] = v
	}
}

func (s *SNES) domains() core.Domains {
	vram, cgram := words(s.ppu.vram[:]), words(s.ppu.cgram[:])
	vram.Name, cgram.Name = "VRAM", "CGRAM"
	d := core.Domains{
		{Name: "WRAM", Mem: s.wram[:]},
		{Name: "CARTROM", Mem: s.cart.rom, Size: s.cart.size},
		{Name: "CARTRAM", Mem: s.cartRAM()},
		vram,
		cgram,
		{Name: "OAM", Mem: s.ppu.oam[:]},
		{Name: "APURAM", Mem: s.apu.ram[:]},
		{Name: "System Bus", Size: 0x1000000, Peek: func(a uint32) byte { return s.peekA(int(a)) }, Poke: s.busPoke},
	}
	if s.sa1 != nil {
		d = append(d, core.Domain{Name: "SA1 IRAM", Mem: s.sa1.iram[:]})
	}
	return d
}

func (s *SNES) MemoryDomains() []string      { return s.domains().MemoryDomains() }
func (s *SNES) MemorySize(domain string) int { return s.domains().MemorySize(domain) }
func (s *SNES) ReadMemory(domain string, addr uint32, buf []byte) error {
	return s.domains().ReadMemory(domain, addr, buf)
}
func (s *SNES) WriteMemory(domain string, addr uint32, data []byte) error {
	return s.domains().WriteMemory(domain, addr, data)
}
