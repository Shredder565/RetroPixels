package n64

// The console's memory as named memory domains, the way BizHawk's N64 core
// names them, for Archipelago's BizHawk Client (internal/apbridge reads and
// writes through these, between frames).

import (
	"fmt"
	"math"
)

// MemoryDomains: "RDRAM", "ROM" and "System Bus".
func (m *Machine) MemoryDomains() []string { return []string{"RDRAM", "ROM", "System Bus"} }

// MemorySize is a domain's size in bytes (the System Bus is the whole 4 GB
// address space).
func (m *Machine) MemorySize(domain string) int {
	switch domain {
	case "RDRAM":
		return len(m.rdram)
	case "ROM":
		return len(m.rom)
	case "System Bus":
		if systemBusSize > uint64(math.MaxInt) { // (32-bit builds: as much as an int holds)
			return math.MaxInt
		}
		return int(systemBusSize)
	}
	return 0
}

var systemBusSize uint64 = 1 << 32

// busArea: the memory behind a System Bus address (KSEG0 / KSEG1 views of
// RDRAM, and the cartridge at 0x10000000), and the offset in it; nil when
// there's nothing there that can be read safely.
func (m *Machine) busArea(a uint32) ([]byte, int) {
	p := a & 0x1fffffff
	if int(p) < len(m.rdram) {
		return m.rdram, int(p)
	}
	if p >= 0x10000000 && p < 0x1fc00000 {
		return m.rom, int(p - 0x10000000)
	}
	return nil, 0
}

func (m *Machine) domain(domain string, addr uint32, n int) ([]byte, error) {
	var mem []byte
	switch domain {
	case "RDRAM":
		mem = m.rdram
	case "ROM":
		mem = m.rom
	default:
		return nil, fmt.Errorf("no memory domain named %q", domain)
	}
	if int64(addr)+int64(n) > int64(len(mem)) {
		return nil, fmt.Errorf("address out of range for %s", domain)
	}
	return mem, nil
}

// ReadMemory copies len(buf) bytes from addr in a domain (all inside it).
func (m *Machine) ReadMemory(domain string, addr uint32, buf []byte) error {
	if domain == "System Bus" {
		if int64(addr)+int64(len(buf)) > 1<<32 {
			return fmt.Errorf("address out of range for %s", domain)
		}
		for i := range buf {
			mem, off := m.busArea(addr + uint32(i))
			if off < len(mem) {
				buf[i] = mem[off]
			} else {
				buf[i] = 0
			}
		}
		return nil
	}
	mem, err := m.domain(domain, addr, len(buf))
	if err != nil {
		return err
	}
	copy(buf, mem[addr:])
	return nil
}

// WriteMemory writes data at addr in a domain (System Bus writes reach RDRAM
// and the cartridge ROM; elsewhere they're dropped).
func (m *Machine) WriteMemory(domain string, addr uint32, data []byte) error {
	if domain == "System Bus" {
		if int64(addr)+int64(len(data)) > 1<<32 {
			return fmt.Errorf("address out of range for %s", domain)
		}
		for i, v := range data {
			mem, off := m.busArea(addr + uint32(i))
			if off < len(mem) {
				mem[off] = v
			}
		}
		return nil
	}
	mem, err := m.domain(domain, addr, len(data))
	if err != nil {
		return err
	}
	copy(mem[addr:], data)
	return nil
}
