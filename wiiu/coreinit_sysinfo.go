package wiiu

// coreinit's OSGetSystemInfo: a pointer to a block describing the machine,
// laid out as the SDK's OSSystemInfo (see wut's coreinit/systeminfo.h):
//
//	0x00 u32 bus clock (Hz)       0x04 u32 core clock (Hz)
//	0x08 u64 time base at boot    0x10 u32 L2 cache size of cores 0, 1, 2
//	0x1c u32 core clock / bus clock

import "time"

var systemInfoAddr uint32

// the Espresso's three cores have 512 KB, 2 MB and 512 KB of L2 cache
var l2Sizes = [3]uint32{512 << 10, 2 << 20, 512 << 10}

func (m *Machine) initSystemData() {
	m.bootTime = osTicksSince2000(time.Now())
	a := m.sysAlloc(0x20, 32)
	systemInfoAddr = a
	m.Mem.Write32(a, BusHz)
	m.Mem.Write32(a+4, CoreHz)
	m.Mem.Write64(a+8, m.bootTime)
	for i, n := range l2Sizes {
		m.Mem.Write32(a+0x10+4*uint32(i), n)
	}
	m.Mem.Write32(a+0x1c, CoreHz/BusHz)
}

func init() {
	reg("coreinit", "OSGetSystemInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, systemInfoAddr) })
}
