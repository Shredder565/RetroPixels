// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// padscore: the WPAD and KPAD interfaces for Wii Remotes, Nunchuks and Pro
// Controllers. RetroPixels emulates only the GamePad, so every channel is
// empty: reads return no samples, probes report no device, and a connect
// callback, once registered, is told a single time that nothing is there.

const (
	wpadChannels = 7

	wpadOK           = 0
	wpadNoController = 0xffffffff // WPAD_ERR_NO_CONTROLLER (-1)
	wpadDevNotFound  = 253        // WPADExtensionType for "no device"
	wpadBatteryFull  = 4

	kpadNoController = 0xfffffffe // KPAD_ERROR_NO_CONTROLLER (-2)

	kpadUnifiedSize = 0x44 // KPADUnifiedWpadStatus
	kpadUnifiedErr  = 0x29 // its WPADStatus err byte

	kpadDefaultMax = 4
)

// wpadState holds what the program registered with padscore.
type wpadState struct {
	onConnect  [wpadChannels]uint32
	notified   [wpadChannels]bool // onConnect already heard "no controller"
	onSample   [wpadChannels]uint32
	onExtend   [wpadChannels]uint32
	maxPlayers uint32
}

// replace sets slot ch of list to fn and returns what was there (0 for a
// channel out of range).
func replace(list []uint32, ch, fn uint32) uint32 {
	if int(ch) >= len(list) {
		return 0
	}
	prev := list[ch]
	list[ch] = fn
	return prev
}

func init() {
	const p = "padscore"
	returns := func(v uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, v) }
	}
	putIf := func(m *Machine, addr, v uint32) {
		if addr != 0 {
			m.Mem.Write32(addr, v)
		}
	}

	// set-up and configuration calls with nothing to configure
	for _, name := range []string{
		"KPADInit", "KPADInitEx", "KPADShutdown", "WPADInit", "WPADShutdown",
		"WPADEnableURCC", "WPADEnableWiiRemote", "WPADSetAutoSleepTime",
		"KPADEnableDPD", "KPADDisableDPD", "WPADControlMotor", "WPADDisconnect",
		"WPADSetPowerSaveMode", "KPADSetBtnRepeat", "KPADSetPosParam",
		"KPADSetHoriParam", "KPADSetDistParam", "KPADSetAccParam",
		"WPADSetDataFormat", "WPADEnableMotor", "KPADEnableMpls",
		"KPADDisableMpls", "KPADResetMpls", "WPADRegisterAllocator",
		"KPADSetMplsWorkarea", "WPADGetDataFormat",
	} {
		reg(p, name, returns(0))
	}
	reg(p, "WPADGetStatus", returns(1))
	reg(p, "WPADIsMotorEnabled", returns(1))
	reg(p, "WPADGetBatteryLevel", returns(wpadBatteryFull))
	reg(p, "KPADGetMplsWorkSize", returns(0x5fe0))
	for _, name := range []string{"WPADRead", "WPADControlDpd", "WPADGetInfo"} {
		reg(p, name, returns(wpadNoController))
	}

	reg(p, "KPADSetMaxControllers", func(m *Machine, cpu *CPU, t *Thread) {
		m.pad.wpad.maxPlayers = arg(cpu, 0)
		ret(cpu, 0)
	})
	reg(p, "KPADGetMaxControllers", func(m *Machine, cpu *CPU, t *Thread) {
		n := m.pad.wpad.maxPlayers
		if n == 0 {
			n = kpadDefaultMax
		}
		ret(cpu, n)
	})

	// callback registration: (channel, fn) returning the old fn
	setConnect := func(m *Machine, cpu *CPU, t *Thread) {
		ch, fn := arg(cpu, 0), arg(cpu, 1)
		w := &m.pad.wpad
		ret(cpu, replace(w.onConnect[:], ch, fn))
		if fn == 0 || ch >= wpadChannels || w.notified[ch] {
			return
		}
		w.notified[ch] = true
		m.queueCallback(cpu.Core, fn, ch, wpadNoController)
	}
	reg(p, "KPADSetConnectCallback", setConnect)
	reg(p, "WPADSetConnectCallback", setConnect)
	setSampling := func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, replace(m.pad.wpad.onSample[:], arg(cpu, 0), arg(cpu, 1)))
	}
	reg(p, "KPADSetSamplingCallback", setSampling)
	reg(p, "WPADSetSamplingCallback", setSampling)
	reg(p, "WPADSetExtensionCallback", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, replace(m.pad.wpad.onExtend[:], arg(cpu, 0), arg(cpu, 1)))
	})

	// WPADProbe(channel, u32 *type)
	reg(p, "WPADProbe", func(m *Machine, cpu *CPU, t *Thread) {
		putIf(m, arg(cpu, 1), wpadDevNotFound)
		ret(cpu, wpadNoController)
	})
	// KPADRead(channel, buf, n) and KPADReadEx(channel, buf, n, s32 *err):
	// no samples
	reg(p, "KPADRead", returns(0))
	reg(p, "KPADReadEx", func(m *Machine, cpu *CPU, t *Thread) {
		putIf(m, arg(cpu, 3), kpadNoController)
		ret(cpu, 0)
	})
	// KPADGetUnifiedWpadStatus(channel, buf, n): n cleared records whose
	// first one says "no controller"
	reg(p, "KPADGetUnifiedWpadStatus", func(m *Machine, cpu *CPU, t *Thread) {
		ch, buf, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if ch < wpadChannels && buf != 0 && n > 0 {
			m.Mem.Fill(buf, 0, n*kpadUnifiedSize)
			m.Mem.Write8(buf+kpadUnifiedErr, 0xff)
		}
		ret(cpu, 0)
	})
	// WPADGetInfoAsync(channel, info, callback): callback(channel, error)
	reg(p, "WPADGetInfoAsync", func(m *Machine, cpu *CPU, t *Thread) {
		m.queueCallback(cpu.Core, arg(cpu, 2), arg(cpu, 0), kpadNoController)
		ret(cpu, wpadNoController)
	})
	// WPADIsMplsAttached(channel, u32 *attached, callback): channels 0..3
	// answer "not attached" and call back with the error
	reg(p, "WPADIsMplsAttached", func(m *Machine, cpu *CPU, t *Thread) {
		ch := arg(cpu, 0)
		if ch >= 4 {
			ret(cpu, wpadNoController)
			return
		}
		putIf(m, arg(cpu, 1), 0)
		m.queueCallback(cpu.Core, arg(cpu, 2), ch, wpadNoController)
		ret(cpu, wpadOK)
	})
	// WPADGetAccGravityUnit(channel, type, u32 unit[3])
	reg(p, "WPADGetAccGravityUnit", func(m *Machine, cpu *CPU, t *Thread) {
		unit := arg(cpu, 2)
		if unit == 0 || arg(cpu, 0) >= 4 {
			return
		}
		for i := uint32(0); i < 3; i++ {
			m.Mem.Write32(unit+i*4, 99)
		}
	})
}
