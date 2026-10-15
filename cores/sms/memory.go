package sms

// Memory domains for Archipelago's BizHawk Client (core.Memory), named like
// BizHawk's SMS core, as web/emu/archipelago.js does.

import "retropixels/core"

func (s *SMS) domains() core.Domains {
	return core.Domains{
		{Name: "Main RAM", Mem: s.ram[:]},
		{Name: "ROM", Mem: s.rom},
		{Name: "Cart (Volatile) RAM", Mem: s.cartRam[:]},
	}
}

func (s *SMS) MemoryDomains() []string      { return s.domains().MemoryDomains() }
func (s *SMS) MemorySize(domain string) int { return s.domains().MemorySize(domain) }
func (s *SMS) ReadMemory(domain string, addr uint32, buf []byte) error {
	return s.domains().ReadMemory(domain, addr, buf)
}
func (s *SMS) WriteMemory(domain string, addr uint32, data []byte) error {
	return s.domains().WriteMemory(domain, addr, data)
}
