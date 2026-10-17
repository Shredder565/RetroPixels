package wiiu

// coreinit: hardware registers, drivers and other system calls games make
// while starting up. There's no hardware behind them here.

func init() {
	c := "coreinit"
	zero := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	none := func(m *Machine, cpu *CPU, t *Thread) {}

	reg(c, "OSEnableForegroundExit", none)
	reg(c, "OSDisableForegroundExit", none)
	// registers read as zero, writes go nowhere
	reg(c, "OSReadRegister16", zero)
	reg(c, "OSReadRegister32Ex", zero)
	reg(c, "__OSReadRegister32Ex", zero)
	reg(c, "OSWriteRegister16", none)
	reg(c, "OSWriteRegister32Ex", none)
	reg(c, "__OSWriteRegister32Ex", none)
	reg(c, "bspGetHardwareVersion", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, 0x21201000) // a retail console's Latte
		}
		ret(cpu, 0)
	})
	// drivers: registered, never called back
	reg(c, "OSDriver_Register", zero)
	reg(c, "OSDriver_Deregister", zero)
	reg(c, "OSDriver_CopyToSaveArea", zero)
	reg(c, "OSDriver_CopyFromSaveArea", zero)
}
