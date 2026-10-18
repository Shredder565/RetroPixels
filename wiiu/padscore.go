// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/padscore/padscore.cpp
// and padscore.h, rewritten in Go for RetroPixels.

package wiiu

// padscore: Wii Remotes and Pro Controllers. None are connected, so every
// channel answers "no controller" the way Cemu does without one.

const (
	wpadMaxChannels = 7

	wpadErrNone         = 0
	wpadErrNoController = 0xffffffff // -1

	kpadErrNoSamples    = 0xffffffff // -1
	kpadErrNoController = 0xfffffffe // -2

	wpadBatteryFull = 4

	kpadUnifiedStatusSize = 0x44
	wpadStatusErr         = 0x29 // WPADStatus.err
)

// wpadState is what the program has told padscore.
type wpadState struct {
	connect   [wpadMaxChannels]uint32 // connect callbacks
	told      [wpadMaxChannels]bool   // connect callback told "no controller"
	sampling  [wpadMaxChannels]uint32
	extension [wpadMaxChannels]uint32
	maxCtl    uint32
}

// swapCallback stores fn in s[ch] and returns the previous one.
func swapCallback(s []uint32, ch, fn uint32) uint32 {
	if ch >= uint32(len(s)) {
		return 0
	}
	old := s[ch]
	s[ch] = fn
	return old
}

func init() {
	p := "padscore"
	for _, n := range []string{"KPADInit", "KPADInitEx", "KPADShutdown", "WPADInit", "WPADShutdown", "WPADEnableURCC",
		"WPADEnableWiiRemote", "WPADSetAutoSleepTime", "KPADEnableDPD", "KPADDisableDPD", "WPADControlMotor",
		"WPADDisconnect", "WPADSetPowerSaveMode", "KPADSetBtnRepeat", "KPADSetPosParam", "KPADSetHoriParam",
		"KPADSetDistParam", "KPADSetAccParam", "WPADSetDataFormat", "WPADEnableMotor", "KPADEnableMpls",
		"KPADDisableMpls", "KPADResetMpls", "WPADRegisterAllocator", "KPADSetMplsWorkarea"} {
		reg(p, n, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	reg(p, "KPADSetMaxControllers", func(m *Machine, cpu *CPU, t *Thread) {
		m.pad.wpad.maxCtl = arg(cpu, 0)
		ret(cpu, 0)
	})
	reg(p, "KPADGetMaxControllers", func(m *Machine, cpu *CPU, t *Thread) {
		n := m.pad.wpad.maxCtl
		if n == 0 {
			n = 4
		}
		ret(cpu, n)
	})
	// the connect callback hears "no controller" once per channel
	connect := func(m *Machine, cpu *CPU, t *Thread) {
		ch, fn := arg(cpu, 0), arg(cpu, 1)
		w := &m.pad.wpad
		ret(cpu, swapCallback(w.connect[:], ch, fn))
		if ch < wpadMaxChannels && fn != 0 && !w.told[ch] {
			w.told[ch] = true
			m.queueCallback(cpu.Core, fn, ch, wpadErrNoController)
		}
	}
	reg(p, "KPADSetConnectCallback", connect)
	reg(p, "WPADSetConnectCallback", connect)
	sampling := func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, swapCallback(m.pad.wpad.sampling[:], arg(cpu, 0), arg(cpu, 1)))
	}
	reg(p, "KPADSetSamplingCallback", sampling)
	reg(p, "WPADSetSamplingCallback", sampling)
	reg(p, "WPADSetExtensionCallback", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, swapCallback(m.pad.wpad.extension[:], arg(cpu, 0), arg(cpu, 1)))
	})
	reg(p, "WPADProbe", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 1); a != 0 {
			m.Mem.Write32(a, 253) // WPAD_DEV_NOT_FOUND
		}
		ret(cpu, wpadErrNoController)
	})
	kpadRead := func(m *Machine, cpu *CPU, errp uint32) {
		if errp != 0 {
			m.Mem.Write32(errp, kpadErrNoController)
		}
		ret(cpu, 0)
	}
	reg(p, "KPADRead", func(m *Machine, cpu *CPU, t *Thread) { kpadRead(m, cpu, 0) })
	reg(p, "KPADReadEx", func(m *Machine, cpu *CPU, t *Thread) { kpadRead(m, cpu, arg(cpu, 3)) })
	reg(p, "KPADGetUnifiedWpadStatus", func(m *Machine, cpu *CPU, t *Thread) {
		ch, buf, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if ch < wpadMaxChannels && buf != 0 && n != 0 {
			m.Mem.Fill(buf, 0, n*kpadUnifiedStatusSize)
			m.Mem.Write8(buf+wpadStatusErr, 0xff) // WPAD_ERR_NO_CONTROLLER
		}
		ret(cpu, 0)
	})
	reg(p, "WPADRead", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, wpadErrNoController) })
	reg(p, "WPADGetStatus", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(p, "WPADGetBatteryLevel", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, wpadBatteryFull) })
	reg(p, "WPADGetDataFormat", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(p, "WPADIsMotorEnabled", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(p, "WPADControlDpd", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, wpadErrNoController) })
	reg(p, "KPADGetMplsWorkSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x5fe0) })
	reg(p, "WPADGetInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, wpadErrNoController) })
	reg(p, "WPADGetInfoAsync", func(m *Machine, cpu *CPU, t *Thread) {
		m.queueCallback(cpu.Core, arg(cpu, 2), arg(cpu, 0), kpadErrNoController)
		ret(cpu, wpadErrNoController)
	})
	reg(p, "WPADIsMplsAttached", func(m *Machine, cpu *CPU, t *Thread) {
		ch, attached, cb := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if ch >= 4 {
			ret(cpu, wpadErrNoController)
			return
		}
		if attached != 0 {
			m.Mem.Write32(attached, 0)
		}
		m.queueCallback(cpu.Core, cb, ch, wpadErrNoController)
		ret(cpu, wpadErrNone)
	})
	reg(p, "WPADGetAccGravityUnit", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 2); a != 0 && arg(cpu, 0) < 4 {
			for i := uint32(0); i < 3; i++ {
				m.Mem.Write32(a+4*i, 99)
			}
		}
	})
}
