// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// OSGetSystemInfo returns a pointer to a small struct describing the
// hardware the program runs on:
//
//	0x00 u32 bus clock (Hz)    0x04 u32 core clock (Hz)
//	0x08 u64 OSTime when the time base was 0
//	0x10 u32 L2 cache size of cores 0, 1, 2
//	0x1c u32 core clock / bus clock

import "time"

// the address of that struct, made once at start-up
var systemInfoAddr uint32

// initSystemData records the boot time and fills in the system info.
func (m *Machine) initSystemData() {
	m.bootTime = osTicksSince2000(time.Now())
	p := m.sysAlloc(0x20, 32)
	systemInfoAddr = p
	l2 := [3]uint32{512 << 10, 2 << 20, 512 << 10} // core 1 has the big cache
	mem := m.Mem
	mem.Write32(p+0x00, BusHz)
	mem.Write32(p+0x04, CoreHz)
	mem.Write64(p+0x08, m.bootTime)
	for i, n := range l2 {
		mem.Write32(p+0x10+uint32(i)*4, n)
	}
	mem.Write32(p+0x1c, CoreHz/BusHz)
}

func init() {
	reg("coreinit", "OSGetSystemInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, systemInfoAddr) })
}
