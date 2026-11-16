package wiiu

// OSSwitchCoroutine and OSSwitchFiberEx.

func init() {
	const lib = "coreinit"

	// OSSwitchCoroutine(from, to) saves the caller into from and goes on in
	// to. Like the system's version it first makes a 16-byte stack frame
	// holding r31 (at +0x0c) and the return address (in the caller's LR
	// slot, +0x14), and puts to in r31, so the r31 saved in from is to
	// (Injustice: Gods Among Us looks there). to is entered with r3 = to
	// and r4 = 1.
	reg(lib, "OSSwitchCoroutine", func(m *Machine, cpu *CPU, t *Thread) {
		from, to := arg(cpu, 0), arg(cpu, 1)
		sp := cpu.GPR[1] - 0x10
		m.Mem.Write32(sp+0x0c, cpu.GPR[31])
		m.Mem.Write32(sp+0x14, cpu.LR)
		cpu.GPR[1] = sp
		cpu.GPR[31] = to
		m.coSave(cpu, from, m.thunk(lib, "__RPSwitchCoroutineResume"))
		cpu.GPR[3], cpu.GPR[4] = to, 1
		m.coLoad(cpu, to)
	})
	// a coroutine saved by OSSwitchCoroutine resumes here: undo the frame
	reg(lib, "__RPSwitchCoroutineResume", func(m *Machine, cpu *CPU, t *Thread) {
		sp := cpu.GPR[1]
		cpu.GPR[31] = m.Mem.Read32(sp + 0x0c)
		cpu.LR = m.Mem.Read32(sp + 0x14)
		cpu.GPR[1] = sp + 0x10
	})

	// OSSwitchFiberEx(a, b, c, d, entry, stackTop) calls entry(a, b, c, d)
	// on the stack that ends at stackTop and returns what it returns. The
	// caller's stack pointer and return address wait at the top of the new
	// stack (+8 and +0x0c of its first frame, below a null back chain).
	reg(lib, "OSSwitchFiberEx", func(m *Machine, cpu *CPU, t *Thread) {
		sp := (arg(cpu, 5) - 0x10) &^ 0xf
		m.Mem.Write32(sp, 0)
		m.Mem.Write32(sp+8, cpu.GPR[1])
		m.Mem.Write32(sp+0x0c, cpu.LR)
		cpu.GPR[1] = sp
		cpu.LR = m.thunk(lib, "__RPSwitchFiberReturn")
		cpu.PC = arg(cpu, 4)
		m.jumped = true
	})
	reg(lib, "__RPSwitchFiberReturn", func(m *Machine, cpu *CPU, t *Thread) {
		top := cpu.GPR[1]
		cpu.GPR[1] = m.Mem.Read32(top + 8)
		cpu.LR = m.Mem.Read32(top + 0x0c)
	})
}
