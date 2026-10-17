package wiiu

// vpad: the Wii U GamePad, and padscore: Wii Remotes and Pro Controllers
// (none are connected for now, so programs use the GamePad).

import "math"

// GamePad buttons (VPAD_BUTTON_*), as VPADRead reports them.
const (
	BtnSync   = 0x00000001
	BtnHome   = 0x00000002
	BtnMinus  = 0x00000004
	BtnPlus   = 0x00000008
	BtnR      = 0x00000010
	BtnL      = 0x00000020
	BtnZR     = 0x00000040
	BtnZL     = 0x00000080
	BtnDown   = 0x00000100
	BtnUp     = 0x00000200
	BtnRight  = 0x00000400
	BtnLeft   = 0x00000800
	BtnY      = 0x00001000
	BtnX      = 0x00002000
	BtnB      = 0x00004000
	BtnA      = 0x00008000
	BtnTV     = 0x00010000
	BtnStickR = 0x00020000
	BtnStickL = 0x00040000

	stickLLeft  = 0x40000000
	stickLRight = 0x20000000
	stickLUp    = 0x10000000
	stickLDown  = 0x08000000
	stickRLeft  = 0x04000000
	stickRRight = 0x02000000
	stickRUp    = 0x01000000
	stickRDown  = 0x00800000
)

// PadState is what the player is doing with the GamePad.
type PadState struct {
	Buttons uint32  // Btn* bits
	LX, LY  float32 // left stick, -1..1, up is +
	RX, RY  float32
	Touch   bool
	TX, TY  float32 // where the screen is touched, 0..1 across and down
}

// GamePad is the GamePad as the program reads it.
type GamePad struct {
	In        PadState // set by the host
	cur, prev uint32   // buttons, with the stick directions
	sampled   PadState
}

// SetGamePad sets the GamePad's state for the next frames.
func (m *Machine) SetGamePad(s PadState) { m.pad.In = s }

func stickBits(x, y float32, l, r, u, d uint32) uint32 {
	var b uint32
	const t = 0.5
	if x < -t {
		b |= l
	}
	if x > t {
		b |= r
	}
	if y > t {
		b |= u
	}
	if y < -t {
		b |= d
	}
	return b
}

func (p *GamePad) sample() {
	p.prev = p.cur
	s := p.In
	p.sampled = s
	p.cur = s.Buttons | stickBits(s.LX, s.LY, stickLLeft, stickLRight, stickLUp, stickLDown) |
		stickBits(s.RX, s.RY, stickRLeft, stickRRight, stickRUp, stickRDown)
}

func init() {
	v := "vpad"
	reg(v, "VPADInit", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(v, "VPADShutdown", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(v, "VPADRead", func(m *Machine, cpu *CPU, t *Thread) {
		ch, buf, n, errp := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		if ch != 0 || n == 0 {
			if errp != 0 {
				m.Mem.Write32(errp, 0xfffffffe) // VPAD_READ_INVALID_CONTROLLER
			}
			ret(cpu, 0)
			return
		}
		m.writeVPADStatus(buf)
		if errp != 0 {
			m.Mem.Write32(errp, 0)
		}
		ret(cpu, 1)
	})
	reg(v, "VPADGetTPCalibratedPoint", func(m *Machine, cpu *CPU, t *Thread) {
		m.calibrateTouch(arg(cpu, 1), arg(cpu, 2), DRCWidth, DRCHeight)
	})
	reg(v, "VPADGetTPCalibratedPointEx", func(m *Machine, cpu *CPU, t *Thread) {
		w, h := uint32(DRCWidth), uint32(DRCHeight)
		if arg(cpu, 1) == 1 { // 1280x720
			w, h = TVWidth, TVHeight
		}
		m.calibrateTouch(arg(cpu, 2), arg(cpu, 3), w, h)
	})
	for _, n := range []string{"VPADSetTPCalibrationParam", "VPADGetTPCalibrationParam", "VPADSetSamplingCallback",
		"VPADSetBtnRepeat", "VPADSetStickOrigin", "VPADEnableStickCrossClamp", "VPADDisableStickCrossClamp",
		"VPADSetCrossStickEmulationParamsL", "VPADSetCrossStickEmulationParamsR", "VPADSetLStickClampThreshold",
		"VPADSetRStickClampThreshold", "VPADSetAccParam", "VPADSetAccPlayMode", "VPADSetGyroAngle",
		"VPADSetGyroDirection", "VPADSetGyroDirectionMag", "VPADSetGyroMagnification", "VPADEnableGyroAccRevise",
		"VPADDisableGyroAccRevise", "VPADEnableGyroDirRevise", "VPADDisableGyroDirRevise", "VPADEnableGyroZeroPlay",
		"VPADDisableGyroZeroPlay", "VPADEnableGyroZeroDrift", "VPADDisableGyroZeroDrift", "VPADInitGyroAccReviseParam",
		"VPADInitGyroDirReviseParam", "VPADInitGyroZeroPlayParam", "VPADInitGyroZeroDriftMode", "VPADSetGyroZeroDriftMode",
		"VPADStopMotor", "VPADEnablePowerButton", "VPADDisablePowerButton", "VPADSetTVMenuInvalid",
		"VPADSetSensorBar", "VPADSetLcdMode", "VPADBASESetMotorOnRemainingCount"} {
		reg(v, n, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	reg(v, "VPADControlMotor", func(m *Machine, cpu *CPU, t *Thread) {
		m.pad.rumble(m, arg(cpu, 1), arg(cpu, 2))
		ret(cpu, 0)
	})
	reg(v, "VPADGetLcdMode", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 1), 1) // on
		ret(cpu, 0)
	})
	reg(v, "VPADGetGyroZeroDriftMode", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 1), 0) })
	reg(v, "VPADBASEGetMotorOnRemainingCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg("vpadbase", "VPADBASEGetMotorOnRemainingCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })

	// padscore: no Wii Remotes or Pro Controllers yet
	p := "padscore"
	for _, n := range []string{"KPADInit", "KPADInitEx", "KPADShutdown", "WPADInit", "WPADShutdown", "WPADEnableURCC",
		"WPADEnableWiiRemote", "WPADSetAutoSleepTime", "KPADSetConnectCallback", "WPADSetConnectCallback",
		"KPADEnableDPD", "KPADDisableDPD", "WPADControlMotor", "WPADDisconnect", "KPADSetMaxControllers",
		"WPADSetPowerSaveMode", "KPADSetBtnRepeat", "KPADSetPosParam", "KPADSetHoriParam", "KPADSetDistParam",
		"KPADSetAccParam", "WPADSetDataFormat", "WPADEnableMotor", "KPADEnableMpls", "KPADDisableMpls", "KPADResetMpls", "WPADRegisterAllocator", "KPADSetMplsWorkarea", "WPADSetSamplingCallback"} {
		reg(p, n, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	reg(p, "WPADProbe", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 1); a != 0 {
			m.Mem.Write32(a, 0xfd) // no controller
		}
		ret(cpu, 0xffffffff) // WPAD_ERROR_NO_CONTROLLER
	})
	reg(p, "KPADRead", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(p, "KPADReadEx", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 3); a != 0 {
			m.Mem.Write32(a, 0xffffffff) // KPAD_ERROR_NO_SAMPLES
		}
		ret(cpu, 0)
	})
	reg(p, "WPADGetStatus", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(p, "WPADGetBatteryLevel", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(p, "KPADGetMplsWorkSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x5fe0) })
	reg(p, "WPADGetInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xffffffff) })
	reg(p, "WPADGetInfoAsync", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xffffffff) })
	reg(p, "WPADIsMplsAttached", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xffffffff) })
	reg(p, "WPADRead", func(m *Machine, cpu *CPU, t *Thread) {})
}

func (p *GamePad) rumble(m *Machine, pattern, n uint32) {
	m.rumbleUntil = m.Frame + 6
}

// writeVPADStatus fills a VPADStatus (0xAC bytes).
func (m *Machine) writeVPADStatus(a uint32) {
	mem := m.Mem
	p := m.pad
	s := p.sampled
	mem.Fill(a, 0, 0xac)
	mem.Write32(a+0x00, p.cur)
	mem.Write32(a+0x04, p.cur&^p.prev)
	mem.Write32(a+0x08, p.prev&^p.cur)
	mem.WriteF32(a+0x0c, s.LX)
	mem.WriteF32(a+0x10, s.LY)
	mem.WriteF32(a+0x14, s.RX)
	mem.WriteF32(a+0x18, s.RY)
	// the GamePad lying still: gravity straight down its back
	mem.WriteF32(a+0x1c, 0)
	mem.WriteF32(a+0x20, -1)
	mem.WriteF32(a+0x24, 0)
	mem.WriteF32(a+0x28, 1) // magnitude
	mem.WriteF32(a+0x2c, 0) // variation
	mem.WriteF32(a+0x30, 0)
	mem.WriteF32(a+0x34, -1)
	// direction: the identity
	mem.WriteF32(a+0x6c, 1)
	mem.WriteF32(a+0x7c, 1)
	mem.WriteF32(a+0x8c, 1)
	mem.Write8(a+0x50, 0)
	// touch: raw coordinates 0..4095
	tx := uint16(math.Max(0, math.Min(4095, float64(s.TX)*4095)))
	ty := uint16(math.Max(0, math.Min(4095, float64(1-s.TY)*4095)))
	for _, o := range []uint32{0x52, 0x5a, 0x62} {
		if s.Touch {
			mem.Write16(a+o, tx)
			mem.Write16(a+o+2, ty)
			mem.Write16(a+o+4, 1)
		}
		mem.Write16(a+o+6, 0) // valid
	}
	mem.Write8(a+0xa0, 0xff) // volume slider
	mem.Write8(a+0xa1, 6)    // battery full
}

// calibrateTouch turns raw touch coordinates into screen pixels.
func (m *Machine) calibrateTouch(out, in, w, h uint32) {
	mem := m.Mem
	x, y := uint32(mem.Read16(in)), uint32(mem.Read16(in+2))
	cx := x * w / 4096
	cy := (4095 - y) * h / 4096
	mem.Write16(out, uint16(cx))
	mem.Write16(out+2, uint16(cy))
	mem.Write16(out+4, mem.Read16(in+4))
	mem.Write16(out+6, mem.Read16(in+6))
}

// Rumble reports whether the program has the GamePad rumbling.
func (m *Machine) Rumble() bool { return m.Frame < m.rumbleUntil }
