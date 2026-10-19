// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/emulated/WPADController.h"

#include "input/api/Controller.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/WiimoteController.h"

namespace
{
	// which button layout a data format uses
	enum class ButtonLayout
	{
		None,
		Wiimote, // Wii remote, with the Nunchuk bits
		Classic,
		Pro,
	};

	ButtonLayout LayoutOf(WPADDataFormat format)
	{
		switch (format)
		{
		case kDataFormat_CORE:
		case kDataFormat_CORE_ACC:
		case kDataFormat_CORE_ACC_DPD:
		case kDataFormat_CORE_ACC_DPD_FULL:
		case kDataFormat_FREESTYLE:
		case kDataFormat_FREESTYLE_ACC:
		case kDataFormat_FREESTYLE_ACC_DPD:
		case kDataFormat_MPLS:
			return ButtonLayout::Wiimote;
		case kDataFormat_CLASSIC:
		case kDataFormat_CLASSIC_ACC:
		case kDataFormat_CLASSIC_ACC_DPD:
			return ButtonLayout::Classic;
		case kDataFormat_URCC:
			return ButtonLayout::Pro;
		default: // train, guitar, balance board, drums, taiko: no buttons emulated
			return ButtonLayout::None;
		}
	}

	// raw samples use 11 bit stick values around 0
	uint16 RawStick(float v)
	{
		return (uint16)(sint32)(v * 2048.0f);
	}

	sint8 RawNunchukStick(float v)
	{
		return (sint8)(v * 127.0f);
	}

	void SetVec(beVec2D_t& dst, const glm::vec2& v)
	{
		dst.x = v.x;
		dst.y = v.y;
	}

	void SetVec(beVec3D_t& dst, const glm::vec3& v)
	{
		dst.x = v.x;
		dst.y = v.y;
		dst.z = v.z;
	}

	void SetIdentity(KPADMPDir_t& dir)
	{
		SetVec(dir.X, { 1, 0, 0 });
		SetVec(dir.Y, { 0, 1, 0 });
		SetVec(dir.Z, { 0, 0, 1 });
	}
}

WPADController::WPADController(size_t player_index, WPADDataFormat data_format)
	: EmulatedController(player_index), m_data_format(data_format)
{
}

WPADDataFormat WPADController::get_default_data_format() const
{
	switch (get_device_type())
	{
	case kWAPDevCore: return kDataFormat_CORE_ACC_DPD;
	case kWAPDevFreestyle: return kDataFormat_FREESTYLE_ACC;
	case kWAPDevClassic: return kDataFormat_CLASSIC;
	case kWAPDevMPLS: return kDataFormat_MPLS;
	case kWAPDevMPLSFreeStyle: return kDataFormat_FREESTYLE_ACC_DPD;
	case kWAPDevMPLSClassic: return kDataFormat_CLASSIC_ACC_DPD;
	case kWAPDevURCC: return kDataFormat_URCC;
	default: return kDataFormat_CORE;
	}
}

uint32 WPADController::get_emulated_button_flag(WPADDataFormat format, uint32 id) const
{
	switch (LayoutOf(format))
	{
	case ButtonLayout::Wiimote: return WiimoteController::s_get_emulated_button_flag(id);
	case ButtonLayout::Classic: return ClassicController::s_get_emulated_button_flag(id);
	case ButtonLayout::Pro: return ProController::s_get_emulated_button_flag(id);
	default: return 0;
	}
}

uint32 WPADController::collect_buttons() const
{
	uint32 bits = 0;
	const uint32 count = (uint32)get_highest_mapping_id();
	for (uint32 id = 1; id < count; ++id)
	{
		if (is_mapping_down(id))
			bits |= get_emulated_button_flag(m_data_format, id);
	}
	return bits;
}

void WPADController::WPADRead(WPADStatus_t* status)
{
	controllers_update_states();
	const uint32 buttons = collect_buttons();
	if (is_home_down())
		m_homebutton_down = true;

	switch (m_data_format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
		std::memset(status, 0, sizeof(WPADStatus_t));
		status->button = (uint16)buttons;
		break;

	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	{
		auto& fs = *reinterpret_cast<WPADFSStatus_t*>(status);
		std::memset(&fs, 0, sizeof(fs));
		fs.button = (uint16)buttons;
		const glm::vec2 stick = get_axis();
		fs.fsStickX = RawNunchukStick(stick.x);
		fs.fsStickY = RawNunchukStick(stick.y);
		break;
	}

	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
	case kDataFormat_GUITAR:
	case kDataFormat_DRUM:
	case kDataFormat_TAIKO:
	{
		auto& cl = *reinterpret_cast<WPADCLStatus_t*>(status);
		std::memset(&cl, 0, sizeof(cl));
		cl.clButton = (uint16)buttons;
		const glm::vec2 left = get_axis();
		const glm::vec2 right = get_rotation();
		cl.clLStickX = RawStick(left.x);
		cl.clLStickY = RawStick(left.y);
		cl.clRStickX = RawStick(right.x);
		cl.clRStickY = RawStick(right.y);
		break;
	}

	case kDataFormat_URCC:
	{
		auto& uc = *reinterpret_cast<WPADUCStatus_t*>(status);
		std::memset(&uc, 0, sizeof(uc));
		uc.ucButton = buttons;
		uc.cable = TRUE;
		uc.charge = TRUE;
		const glm::vec2 left = get_axis();
		const glm::vec2 right = get_rotation();
		uc.ucLStickX = RawStick(left.x);
		uc.ucLStickY = RawStick(left.y);
		uc.ucRStickX = RawStick(right.x);
		uc.ucRStickY = RawStick(right.y);
		break;
	}

	case kDataFormat_MPLS:
		// only reports the MotionPlus as attached; the rest of the sample is left as the caller had it
		reinterpret_cast<WPADMPStatus_t*>(status)->stat = 1;
		break;

	case kDataFormat_TRAIN:
	case kDataFormat_BALANCE_CHECKER:
		// not emulated; the sample is left as the caller had it
		break;

	default:
		cemu_assert(false);
	}

	status->dev = get_device_type();
	status->err = WPAD_ERR_NONE;
}

void WPADController::KPADRead(KPADStatus_t& status, const BtnRepeat& repeat)
{
	// the Pro and Classic Controller report their buttons in the extension block
	uint32be* hold = &status.hold;
	uint32be* trig = &status.trig;
	uint32be* release = &status.release;
	if (type() == Pro)
	{
		hold = &status.ex_status.uc.hold;
		trig = &status.ex_status.uc.trig;
		release = &status.ex_status.uc.release;
	}
	else if (type() == Classic)
	{
		hold = &status.ex_status.cl.hold;
		trig = &status.ex_status.cl.trig;
		release = &status.ex_status.cl.release;
	}

	controllers_update_states();
	uint32 held = (uint32)*hold | collect_buttons();
	if (is_home_down())
		m_homebutton_down = true;

	// button repeat. A pulse is added every `pulse` ms, but only while the buttons changed
	// less than `delay` ms ago; games have always seen it work this way round.
	const auto now = Clock::now();
	if (held != m_previous_hold)
	{
		m_hold_changed = now;
		m_last_repeat = now;
	}
	if (repeat.pulse > 0
		&& now <= m_hold_changed + std::chrono::milliseconds(repeat.delay)
		&& now > m_last_repeat + std::chrono::milliseconds(repeat.pulse))
	{
		m_last_repeat = now;
		held |= kWPADButtonRepeat;
	}

	*hold = held;
	*release = m_previous_hold & ~held;
	*trig = held & ~m_previous_hold;
	m_previous_hold = held;

	const bool mpls = is_mpls_attached();
	if (mpls)
		SetIdentity(status.mpls.dir);

	if (has_motion())
	{
		const MotionSample sample = get_motion_data();
		glm::vec3 acc;
		sample.getAccelerometer(&acc[0]);
		SetVec(status.acc, acc);
		status.acc_value = sample.getVPADAccMagnitude();
		status.acc_speed = sample.getVPADAccAcceleration();
		status.accVertical.x = std::min(1.0f, std::abs(acc.x + acc.y));
		status.accVertical.y = std::min(std::max(-1.0f, -acc.z), 1.0f);

		if (mpls)
		{
			glm::vec3 rate, angle;
			sample.getVPADGyroChange(&rate[0]);
			sample.getVPADOrientation(&angle[0]);
			SetVec(status.mpls.mpls, rate);
			SetVec(status.mpls.angle, angle);

			float m[9];
			sample.getVPADAttitudeMatrix(m);
			SetVec(status.mpls.dir.X, { m[0], m[1], m[2] });
			SetVec(status.mpls.dir.Y, { m[3], m[4], m[5] });
			SetVec(status.mpls.dir.Z, { m[6], m[7], m[8] });
		}
	}

	// pointer
	const PositionVisibility visibility = GetPositionVisibility();
	if (visibility != PositionVisibility::NONE && has_position())
	{
		status.dpd_valid_fg = visibility == PositionVisibility::FULL ? 2 : -1;
		const glm::vec2 position = get_position();
		const glm::vec2 delta = position - get_prev_position();
		SetVec(status.pos, position * 2.0f - 1.0f);
		SetVec(status.vec, delta);
		status.speed = glm::length(delta);
	}
	else
		status.dpd_valid_fg = 0;

	const glm::vec2 left = get_axis();
	switch (type())
	{
	case Wiimote:
		SetVec(status.ex_status.fs.stick, left);
		if (has_second_motion())
		{
			// the Nunchuk's own accelerometer
			const MotionSample sample = get_second_motion_data();
			glm::vec3 acc;
			sample.getAccelerometer(&acc[0]);
			SetVec(status.ex_status.fs.acc, acc);
			status.ex_status.fs.accValue = sample.getVPADAccMagnitude();
			status.ex_status.fs.accSpeed = sample.getVPADAccAcceleration();
		}
		break;
	case Pro:
		SetVec(status.ex_status.uc.lstick, left);
		SetVec(status.ex_status.uc.rstick, get_rotation());
		status.ex_status.uc.charge = FALSE;
		status.ex_status.uc.cable = TRUE;
		break;
	case Classic:
		SetVec(status.ex_status.cl.lstick, left);
		SetVec(status.ex_status.cl.rstick, get_rotation());
		// digital triggers
		if (held & kCLButton_ZL)
			status.ex_status.cl.ltrigger = 1.0f;
		if (held & kCLButton_ZR)
			status.ex_status.cl.rtrigger = 1.0f;
		break;
	default:
		cemu_assert(false);
	}
}
