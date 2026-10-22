package wiiu

// vpad: the Wii U GamePad, and padscore: Wii Remotes and Pro Controllers
// (none are connected for now, so programs use the GamePad).

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
	wpad      wpadState
	lcdMode   uint32 // as set by VPADSetLcdMode
	lcdSet    bool   // until then the screen is on
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
		if n == 0 || buf == 0 {
			if errp != 0 {
				m.Mem.Write32(errp, 0xffffffff) // VPAD_READ_NO_SAMPLES
			}
			ret(cpu, 0)
			return
		}
		if ch != 0 {
			// no second GamePad: an empty sample saying so, as Cemu gives
			m.Mem.Fill(buf, 0, vpadStatusSize)
			m.Mem.Write8(buf+0x50, 0xff) // vpadErr -1
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
		m.calibrateTouch(arg(cpu, 1), arg(cpu, 2), 1280, 720)
		ret(cpu, 0)
	})
	reg(v, "VPADGetTPCalibratedPointEx", func(m *Machine, cpu *CPU, t *Thread) {
		w, h, _ := tpCalibrationResolution(arg(cpu, 1))
		m.calibrateTouch(arg(cpu, 2), arg(cpu, 3), w, h)
		ret(cpu, 0)
	})
	reg(v, "VPADGetTPCalibrationParam", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 1); a != 0 {
			m.Mem.Write16(a, tpOffsetX)
			m.Mem.Write16(a+2, tpOffsetY)
			m.Mem.WriteF32(a+4, 1280.0/tpScaleX)
			m.Mem.WriteF32(a+8, 720.0/tpScaleY)
		}
	})
	for _, n := range []string{"VPADSetTPCalibrationParam", "VPADSetSamplingCallback",
		"VPADSetBtnRepeat", "VPADSetStickOrigin", "VPADEnableStickCrossClamp", "VPADDisableStickCrossClamp",
		"VPADSetCrossStickEmulationParamsL", "VPADSetCrossStickEmulationParamsR", "VPADSetLStickClampThreshold",
		"VPADSetRStickClampThreshold", "VPADSetAccParam", "VPADSetAccPlayMode", "VPADSetGyroAngle",
		"VPADSetGyroDirection", "VPADSetGyroDirectionMag", "VPADSetGyroMagnification", "VPADEnableGyroAccRevise",
		"VPADDisableGyroAccRevise", "VPADEnableGyroDirRevise", "VPADDisableGyroDirRevise", "VPADEnableGyroZeroPlay",
		"VPADDisableGyroZeroPlay", "VPADEnableGyroZeroDrift", "VPADDisableGyroZeroDrift", "VPADInitGyroAccReviseParam",
		"VPADInitGyroDirReviseParam", "VPADInitGyroZeroPlayParam", "VPADInitGyroZeroDriftMode", "VPADSetGyroZeroDriftMode",
		"VPADStopMotor", "VPADEnablePowerButton", "VPADDisablePowerButton", "VPADSetTVMenuInvalid",
		"VPADSetSensorBar", "VPADBASESetMotorOnRemainingCount"} {
		reg(v, n, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	reg(v, "VPADControlMotor", func(m *Machine, cpu *CPU, t *Thread) {
		m.pad.rumble(m, arg(cpu, 1), arg(cpu, 2))
		ret(cpu, 0)
	})
	reg(v, "VPADSetLcdMode", func(m *Machine, cpu *CPU, t *Thread) {
		if arg(cpu, 0) == 0 {
			m.pad.lcdMode, m.pad.lcdSet = arg(cpu, 1), true
		}
		ret(cpu, 0)
	})
	reg(v, "VPADGetLcdMode", func(m *Machine, cpu *CPU, t *Thread) {
		mode := uint32(0xff) // VPAD_LCD_ON
		if m.pad.lcdSet {
			mode = m.pad.lcdMode
		}
		if a := arg(cpu, 1); a != 0 {
			m.Mem.Write32(a, mode)
		}
		ret(cpu, 0)
	})
	reg(v, "VPADGetGyroZeroDriftMode", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 1), 0) })
	reg(v, "VPADBASEGetMotorOnRemainingCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg("vpadbase", "VPADBASEGetMotorOnRemainingCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
}

func (p *GamePad) rumble(m *Machine, pattern, n uint32) {
	m.rumbleUntil = m.Frame + 6
}

// writeVPADStatus fills a VPADStatus as Cemu does for a GamePad lying still.
func (m *Machine) writeVPADStatus(a uint32) {
	mem := m.Mem
	p := m.pad
	s := p.sampled
	mem.Fill(a, 0, vpadStatusSize)
	mem.Write32(a+0x00, p.cur)
	mem.Write32(a+0x04, p.cur&^p.prev)
	mem.Write32(a+0x08, p.prev&^p.cur)
	mem.WriteF32(a+0x0c, s.LX)
	mem.WriteF32(a+0x10, s.LY)
	mem.WriteF32(a+0x14, s.RX)
	mem.WriteF32(a+0x18, s.RY)
	// gravity straight down its back
	mem.WriteF32(a+0x1c, 0)
	mem.WriteF32(a+0x20, -1)
	mem.WriteF32(a+0x24, 0)
	mem.WriteF32(a+0x28, 1) // magnitude
	mem.WriteF32(a+0x2c, 0) // variation
	mem.WriteF32(a+0x30, 1) // accXY
	mem.WriteF32(a+0x34, 0)
	// direction: the identity
	mem.WriteF32(a+0x6c, 1)
	mem.WriteF32(a+0x7c, 1)
	mem.WriteF32(a+0x8c, 1)
	mem.Write8(a+0x50, 0) // vpadErr
	// touch: raw panel coordinates, or "no x/y" when not touched
	tx, ty := tpRaw(s.TX, s.TY)
	for _, o := range []uint32{0x52, 0x5a, 0x62} {
		if s.Touch {
			mem.Write16(a+o, tx)
			mem.Write16(a+o+2, ty)
			mem.Write16(a+o+4, 1)
			mem.Write16(a+o+6, tpValid)
		} else {
			mem.Write16(a+o+6, tpInvalidXY)
		}
	}
	mem.Write8(a+0xa0, 0xff) // volume slider
	mem.Write8(a+0xa1, 0xc0) // battery full
	mem.Write8(a+0xa3, 0xff) // volume slider again
}

// Rumble reports whether the program has the GamePad rumbling.
func (m *Machine) Rumble() bool { return m.Frame < m.rumbleUntil }
