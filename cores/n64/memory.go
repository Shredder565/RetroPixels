package n64

// Memory domains for Archipelago's BizHawk Client (core.Memory): the
// machine's own (RDRAM, ROM, System Bus), as RetroPixels.exe serves them.

func (c *N64) MemoryDomains() []string      { return c.m.MemoryDomains() }
func (c *N64) MemorySize(domain string) int { return c.m.MemorySize(domain) }
func (c *N64) ReadMemory(domain string, addr uint32, buf []byte) error {
	return c.m.ReadMemory(domain, addr, buf)
}
func (c *N64) WriteMemory(domain string, addr uint32, data []byte) error {
	return c.m.WriteMemory(domain, addr, data)
}
