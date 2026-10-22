// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/
// coreinit/coreinit_SystemInfo.cpp and coreinit_SystemInfo.h, rewritten in
// Go for RetroPixels.

package wiiu

// coreinit: OSGetSystemInfo.

import "time"

// the OSSystemInfo the program reads clock speeds from:
// { u32 busClock; u32 coreClock; u64 baseTime; u32 l2CacheSize[3]; u32 cpuRatio }
var systemInfoAddr uint32

func (m *Machine) initSystemData() {
	m.bootTime = osTicksSince2000(time.Now())
	systemInfoAddr = m.sysAlloc(0x20, 32)
	mem := m.Mem
	mem.Write32(systemInfoAddr+0x00, BusHz)
	mem.Write32(systemInfoAddr+0x04, CoreHz)
	mem.Write64(systemInfoAddr+0x08, m.bootTime) // OSGetTime at time base 0
	mem.Write32(systemInfoAddr+0x10, 512*1024)
	mem.Write32(systemInfoAddr+0x14, 2*1024*1024)
	mem.Write32(systemInfoAddr+0x18, 512*1024)
	mem.Write32(systemInfoAddr+0x1c, CoreHz/BusHz)
}

func init() {
	reg("coreinit", "OSGetSystemInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, systemInfoAddr) })
}
