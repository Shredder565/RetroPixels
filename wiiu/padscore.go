package wiiu

// padscore: Wii Remotes and Pro Controllers (the WPAD and KPAD calls).
// RetroPixels plays with the GamePad, so no Wii Remote is ever connected:
// each channel reports "no controller", a connect callback hears that once,
// reads return no samples, and the settings calls are accepted and ignored.

const (
	wpadChannels = 7

	wpadOK           = 0
	wpadNoController = 0xffffffff // WPAD_ERROR_NO_CONTROLLER (-1)
	kpadNoController = 0xfffffffe // KPAD_ERROR_NO_CONTROLLER (-2)

	wpadDevNotFound = 253 // WPADProbe's device type for an empty channel
	wpadBatteryFull = 4

	kpadStatusSize = 0x44 // KPADUnifiedWpadStatus
	kpadStatusErr  = 0x29 // its error byte
)

// wpadState holds what the program registered with padscore.
type wpadState struct {
	onConnect  [wpadChannels]uint32
	announced  [wpadChannels]bool // the connect callback has been told
	onSample   [wpadChannels]uint32
	onExtend   [wpadChannels]uint32
	controlers uint32 // KPADSetMaxControllers, 0 until set
}

// setCallback installs fn for a channel and gives back the one it replaces.
func setCallback(slots []uint32, ch, fn uint32) uint32 {
	if int(ch) >= len(slots) {
		return 0
	}
	old := slots[ch]
	slots[ch] = fn
	return old
}

func init() {
	const lib = "padscore"
	returns := func(v uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, v) }
	}
	for _, n := range []string{
		"KPADInit", "KPADInitEx", "KPADShutdown", "WPADInit", "WPADShutdown", "WPADEnableURCC",
		"WPADEnableWiiRemote", "WPADSetAutoSleepTime", "KPADEnableDPD", "KPADDisableDPD",
		"WPADControlMotor", "WPADDisconnect", "WPADSetPowerSaveMode", "KPADSetBtnRepeat",
		"KPADSetPosParam", "KPADSetHoriParam", "KPADSetDistParam", "KPADSetAccParam",
		"WPADSetDataFormat", "WPADEnableMotor", "KPADEnableMpls", "KPADDisableMpls",
		"KPADResetMpls", "WPADRegisterAllocator", "KPADSetMplsWorkarea", "WPADGetDataFormat",
	} {
		reg(lib, n, returns(0))
	}
	for _, n := range []string{"WPADRead", "WPADControlDpd", "WPADGetInfo"} {
		reg(lib, n, returns(wpadNoController))
	}
	reg(lib, "WPADGetStatus", returns(1))
	reg(lib, "WPADIsMotorEnabled", returns(1))
	reg(lib, "WPADGetBatteryLevel", returns(wpadBatteryFull))
	reg(lib, "KPADGetMplsWorkSize", returns(0x5fe0))

	reg(lib, "KPADSetMaxControllers", func(m *Machine, cpu *CPU, t *Thread) {
		m.pad.wpad.controlers = arg(cpu, 0)
		ret(cpu, 0)
	})
	reg(lib, "KPADGetMaxControllers", func(m *Machine, cpu *CPU, t *Thread) {
		if n := m.pad.wpad.controlers; n != 0 {
			ret(cpu, n)
		} else {
			ret(cpu, 4)
		}
	})

	// (channel, callback) -> the previous callback. A new connect callback
	// is told, once per channel, that nothing is there.
	onConnect := func(m *Machine, cpu *CPU, t *Thread) {
		ch, fn := arg(cpu, 0), arg(cpu, 1)
		w := &m.pad.wpad
		ret(cpu, setCallback(w.onConnect[:], ch, fn))
		if ch < wpadChannels && fn != 0 && !w.announced[ch] {
			w.announced[ch] = true
			m.queueCallback(cpu.Core, fn, ch, wpadNoController)
		}
	}
	reg(lib, "KPADSetConnectCallback", onConnect)
	reg(lib, "WPADSetConnectCallback", onConnect)
	onSample := func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, setCallback(m.pad.wpad.onSample[:], arg(cpu, 0), arg(cpu, 1)))
	}
	reg(lib, "KPADSetSamplingCallback", onSample)
	reg(lib, "WPADSetSamplingCallback", onSample)
	reg(lib, "WPADSetExtensionCallback", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, setCallback(m.pad.wpad.onExtend[:], arg(cpu, 0), arg(cpu, 1)))
	})

	// WPADProbe(channel, &type)
	reg(lib, "WPADProbe", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, wpadDevNotFound)
		}
		ret(cpu, wpadNoController)
	})
	// KPADRead(channel, buffer, count) and KPADReadEx(..., &error): no samples
	reg(lib, "KPADRead", returns(0))
	reg(lib, "KPADReadEx", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 3); p != 0 {
			m.Mem.Write32(p, kpadNoController)
		}
		ret(cpu, 0)
	})
	// KPADGetUnifiedWpadStatus(channel, buffer, count): blank records
	// marked "no controller"
	reg(lib, "KPADGetUnifiedWpadStatus", func(m *Machine, cpu *CPU, t *Thread) {
		ch, buf, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if ch < wpadChannels && buf != 0 && n != 0 {
			m.Mem.Fill(buf, 0, n*kpadStatusSize)
			m.Mem.Write8(buf+kpadStatusErr, 0xff)
		}
		ret(cpu, 0)
	})
	// WPADGetInfoAsync(channel, info, callback)
	reg(lib, "WPADGetInfoAsync", func(m *Machine, cpu *CPU, t *Thread) {
		m.queueCallback(cpu.Core, arg(cpu, 2), arg(cpu, 0), kpadNoController)
		ret(cpu, wpadNoController)
	})
	// WPADIsMplsAttached(channel, &attached, callback): Wii Remote channels
	// (0-3) answer "not attached", through the callback too
	reg(lib, "WPADIsMplsAttached", func(m *Machine, cpu *CPU, t *Thread) {
		ch := arg(cpu, 0)
		if ch >= 4 {
			ret(cpu, wpadNoController)
			return
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
		m.queueCallback(cpu.Core, arg(cpu, 2), ch, wpadNoController)
		ret(cpu, wpadOK)
	})
	// WPADGetAccGravityUnit(channel, type, &unit): 99 on each axis
	reg(lib, "WPADGetAccGravityUnit", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 2)
		if p == 0 || arg(cpu, 0) >= 4 {
			return
		}
		for i := uint32(0); i < 3; i++ {
			m.Mem.Write32(p+4*i, 99)
		}
	})
}
