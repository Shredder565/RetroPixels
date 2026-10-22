// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/coreinit_Coroutine.cpp,
// rewritten in Go for RetroPixels.

package wiiu

// OSSwitchCoroutine and OSSwitchFiberEx, with the side effects programs see
// on hardware.

func init() {
	c := "coreinit"
	// (from, to): OSSwitchCoroutine keeps r31 and lr in a small stack frame
	// and puts to in r31 before saving, so from's r31 field holds to
	// (Injustice: Gods Among Us reads it). to starts with r3 = to, r4 = 1.
	reg(c, "OSSwitchCoroutine", func(m *Machine, cpu *CPU, t *Thread) {
		from, to := arg(cpu, 0), arg(cpu, 1)
		mem := m.Mem
		cpu.GPR[1] -= 0x10
		mem.Write32(cpu.GPR[1]+0xc, cpu.GPR[31])
		mem.Write32(cpu.GPR[1]+0x14, cpu.LR)
		cpu.GPR[31] = to
		m.coSave(cpu, from, m.thunk(c, "__RPSwitchCoroutineResume"))
		cpu.GPR[3], cpu.GPR[4] = to, 1
		m.coLoad(cpu, to)
	})
	// where a coroutine saved by OSSwitchCoroutine goes on
	reg(c, "__RPSwitchCoroutineResume", func(m *Machine, cpu *CPU, t *Thread) {
		mem := m.Mem
		cpu.GPR[31] = mem.Read32(cpu.GPR[1] + 0xc)
		cpu.LR = mem.Read32(cpu.GPR[1] + 0x14)
		cpu.GPR[1] += 0x10
	})
	// (p0, p1, p2, p3, entry, stack): entry(p0..p3) on another stack,
	// returning what it returns
	reg(c, "OSSwitchFiberEx", func(m *Machine, cpu *CPU, t *Thread) {
		mem := m.Mem
		sp := (arg(cpu, 5) - 0x10) &^ 0xf
		// the old stack and return address sit where entry won't touch them
		mem.Write32(sp, 0)
		mem.Write32(sp+8, cpu.GPR[1])
		mem.Write32(sp+0xc, cpu.LR)
		cpu.GPR[1] = sp
		cpu.LR = m.thunk(c, "__RPSwitchFiberReturn")
		cpu.PC = arg(cpu, 4)
		m.jumped = true
	})
	reg(c, "__RPSwitchFiberReturn", func(m *Machine, cpu *CPU, t *Thread) {
		sp := cpu.GPR[1]
		cpu.GPR[1] = m.Mem.Read32(sp + 8)
		cpu.LR = m.Mem.Read32(sp + 0xc)
	})
}
