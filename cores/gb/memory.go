package gb

// Memory domains for Archipelago's BizHawk Client (core.Memory), named and
// laid out like BizHawk's Gambatte core, as web/emu/archipelago.js does.

import "retropixels/core"

// cartRAMPeek: the cartridge RAM at an A000-BFFF address as the CPU sees it,
// or the selected bank when the RAM is switched off.
func (g *GB) cartRAMPeek(a int) byte {
	cb := g.cb
	if cb.ramOn {
		return byte(g.cart.ramRead(a))
	}
	if len(cb.ram) > 0 {
		return cb.ram[(cb.ramOff+(a&0x1fff))%len(cb.ram)]
	}
	return 0xff
}

func (g *GB) peek(a int) byte {
	for {
		switch {
		case a < 0x8000:
			return byte(g.cart.romRead(a))
		case a < 0xa000:
			return g.vram[g.vbank*0x2000+(a&0x1fff)]
		case a < 0xc000:
			return g.cartRAMPeek(a)
		case a < 0xd000:
			return g.wram[a&0xfff]
		case a < 0xe000:
			return g.wram[g.wbank*0x1000+(a&0xfff)]
		case a < 0xfe00:
			a -= 0x2000
			continue
		case a < 0xfea0:
			return g.oam[a-0xfe00]
		case a < 0xff00:
			return 0xff
		case a < 0xff80:
			return byte(g.ioRead(a))
		case a < 0xffff:
			return g.hram[a-0xff80]
		}
		return byte(g.ie)
	}
}

func (g *GB) poke(a int, v byte) {
	for {
		switch {
		case a < 0x8000: // cartridge registers: leave alone
		case a < 0xa000:
			g.vram[g.vbank*0x2000+(a&0x1fff)] = v
		case a < 0xc000:
			cb := g.cb
			if cb.ramOn {
				g.cart.ramWrite(a, int(v))
			} else if len(cb.ram) > 0 {
				cb.ram[(cb.ramOff+(a&0x1fff))%len(cb.ram)] = v
			}
		case a < 0xd000:
			g.wram[a&0xfff] = v
		case a < 0xe000:
			g.wram[g.wbank*0x1000+(a&0xfff)] = v
		case a < 0xfe00:
			a -= 0x2000
			continue
		case a < 0xfea0:
			g.oam[a-0xfe00] = v
		case a < 0xff00:
		case a < 0xff80:
			g.write(a, int(v))
		case a < 0xffff:
			g.hram[a-0xff80] = v
		default:
			g.ie = int(v)
		}
		return
	}
}

func (g *GB) domains() core.Domains {
	wram, vram := 0x2000, 0x2000
	if g.cgb {
		wram, vram = 0x8000, 0x4000
	}
	d := core.Domains{
		{Name: "ROM", Mem: g.cb.rom},
		{Name: "WRAM", Mem: g.wram[:], Size: wram},
		{Name: "VRAM", Mem: g.vram[:], Size: vram},
		{Name: "OAM", Mem: g.oam[:]},
		{Name: "HRAM", Mem: g.hram[:]},
		{Name: "System Bus", Size: 0x10000, Peek: func(a uint32) byte { return g.peek(int(a)) }, Poke: func(a uint32, v byte) { g.poke(int(a), v) }},
	}
	if len(g.cb.ram) > 0 {
		d = append(d, core.Domain{Name: "CartRAM", Mem: g.cb.ram})
	}
	return d
}

// IsCGB: the game runs as a Game Boy Color game (BizHawk calls the system GBC).
func (g *GB) IsCGB() bool { return g.cgb }

func (g *GB) MemoryDomains() []string      { return g.domains().MemoryDomains() }
func (g *GB) MemorySize(domain string) int { return g.domains().MemorySize(domain) }
func (g *GB) ReadMemory(domain string, addr uint32, buf []byte) error {
	return g.domains().ReadMemory(domain, addr, buf)
}
func (g *GB) WriteMemory(domain string, addr uint32, data []byte) error {
	return g.domains().WriteMemory(domain, addr, data)
}
