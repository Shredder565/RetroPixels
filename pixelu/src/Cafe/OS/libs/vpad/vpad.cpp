// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// vpad is the GamePad library. VPADRead hands the game one sample per call;
// the emulated GamePad (input/emulated/VPADController) produces it, including
// the touch screen: a mouse click or touch on the emulator window becomes a
// GamePad touch in raw panel units. VPADGetTPCalibratedPoint(Ex) turns those
// raw units back into screen pixels, so the two conversions must stay mirror
// images of each other.
//
// Most of the remaining exports only store tuning values (stick clamping,
// accelerometer play mode, gyro drift handling) that games set and read back.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/RPL/rpl.h"
#include "audio/IAudioAPI.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "WindowSystem.h"

extern bool isLaunchTypeELF;

namespace vpad
{
	namespace
	{
		constexpr uint32 kChannels = VPAD_MAX_CONTROLLERS;

		// VPADRead error codes
		constexpr sint32 kReadOk = 0;
		constexpr sint32 kReadNoController = -2;

		constexpr uint8 kBatteryFull = 0xC0;
		constexpr uint32 kPlayModeTight = 1;
		constexpr uint8 kButtonProcTight = 1;
		constexpr uint32 kLcdOn = 0xFF;
		constexpr uint32 kZeroDriftStandard = 1;
		constexpr uint32 kZeroDriftNone = 3;
		constexpr sint32 kStickClampMin = 0x102;
		constexpr sint32 kStickClampMax = 0x397;
		constexpr uint8 kMotorPatternMax = 120;

		// Raw touch panel geometry, the same numbers VPADController uses when
		// it turns a window position into a raw sample.
		constexpr sint32 kTpRawMax = 4095;
		constexpr sint32 kTpOffsetX = 92;
		constexpr sint32 kTpOffsetY = 254;
		constexpr double kTpSpanX = 3883.0;
		constexpr double kTpSpanY = 3694.0;

		struct VPADTPCalibrationParam
		{
			uint16be offsetX;
			uint16be offsetY;
			float32be scaleX;
			float32be scaleY;
		};
		static_assert(sizeof(VPADTPCalibrationParam) == 0xC);

		struct Channel
		{
			MEMPTR<void> samplingCb;
			BtnRepeat repeat{};
			uint64 lastSampleTime = 0;

			float accRadius = 0.0f;
			float accSensitivity = 1.0f;
			uint32 accPlayMode = kPlayModeTight;

			bool stickCrossClamp = false;
			sint32 leftClampMax = kStickClampMax, leftClampMin = kStickClampMin;
			sint32 rightClampMax = kStickClampMax, rightClampMin = kStickClampMin;
			float crossL[3]{}; // rotation, input range, radius
			float crossR[3]{};

			uint8 buttonProcMode = kButtonProcTight;
			uint32 lcdMode = kLcdOn;

			uint16 tpOffsetX = kTpOffsetX, tpOffsetY = kTpOffsetY;
			float tpScaleX = 1280.0f / 3883.0f, tpScaleY = 720.0f / 3694.0f;

			VPADDir gyroDirection{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.1f}};
			uint32 gyroZeroDrift = kZeroDriftStandard;
			bool gyroReviseOn = false;
			VPADDir gyroReviseBase{};
			float gyroReviseWeight = 0.0f;
		};
		Channel s_ch[kChannels];

		SysAllocator<coreinit::OSAlarm_t> s_tickAlarm;

		Channel* Get(uint32 chan)
		{
			return chan < kChannels ? &s_ch[chan] : nullptr;
		}

		// raw panel units -> pixels of a width x height screen
		void TouchRawToScreen(uint16 rawX, uint16 rawY, sint32 width, sint32 height, sint32& outX, sint32& outY)
		{
			// the panel's y axis points up, the screen's down
			sint32 dx = std::max((sint32)(sint16)rawX - kTpOffsetX, 0);
			sint32 dy = std::max(kTpRawMax - kTpOffsetY - (sint32)(sint16)rawY, 0);
			outX = (sint32)((double)dx / kTpSpanX * (double)width);
			outY = (sint32)((double)dy / kTpSpanY * (double)height);
		}

		void Tick(coreinit::OSAlarm_t* alarm, void* context)
		{
			auto& input = InputManager::instance();
			for (uint32 i = 0; i < kChannels; i++)
			{
				if (s_ch[i].samplingCb && input.get_vpad_controller(i))
					PPCCoreCallback(s_ch[i].samplingCb, i);
			}
		}
	}

	// --- reading -----------------------------------------------------------------

	sint32 VPADRead(uint32 chan, VPADStatus* status, uint32 length, sint32be* error)
	{
		auto report = [&](sint32 e) { if (error) *error = e; };

		memset((void*)status, 0, sizeof(VPADStatus));
		status->batteryLevel = kBatteryFull;
		status->slideVolume = (uint8)((g_padVolume * 0xFF) / 100);
		status->slideVolume2 = status->slideVolume;
		status->tpData.validity = kTpInvalid;
		status->tpProcessed1.validity = kTpInvalid;
		status->tpProcessed2.validity = kTpInvalid;

		auto pad = InputManager::instance().get_vpad_controller(chan);
		if (!pad)
		{
			// games expect a GamePad, so channel 0 always answers (with an idle sample)
			if (chan == 0)
			{
				report(kReadOk);
				return 1;
			}
			report(kReadNoController);
			if (length > 0)
				status->vpadErr = -1;
			return 0;
		}

		if (isLaunchTypeELF)
			PPCCore_switchToScheduler(); // homebrew often polls VPADRead in a tight loop

		if (WindowSystem::InputConfigWindowHasFocus())
		{
			// input goes to the controller settings window, the game sees an idle pad
			report(kReadOk);
			return 1;
		}

		if (ActiveSettings::VPADDelayEnabled())
		{
			// the real GamePad delivers a new sample every 1/60 s
			Channel& c = s_ch[chan];
			const uint64 frame = ESPRESSO_TIMER_CLOCK / 60ull;
			const uint64 now = coreinit::OSGetTime();
			const uint64 elapsed = now - c.lastSampleTime;
			if (elapsed <= frame)
			{
				report(kReadOk);
				return 0;
			}
			if (elapsed <= ESPRESSO_TIMER_CLOCK)
				c.lastSampleTime += frame; // keep a steady cadence
			else
				c.lastSampleTime = now;    // we were not polled for a while
		}

		pad->VPADRead(*status, s_ch[chan].repeat);
		report(kReadOk);
		return 1;
	}

	void VPADSetBtnRepeat(uint32 chan, float delaySec, float pulseSec)
	{
		Channel* c = Get(chan);
		if (!c)
			return;
		if (pulseSec == 0.0f)
		{
			c->repeat.delay = 40000; // repeat off
			c->repeat.pulse = 0;
			return;
		}
		// in 5 ms sampling periods, rounded
		c->repeat.delay = (sint32)(delaySec * 200.0f + 0.5f);
		c->repeat.pulse = (sint32)(pulseSec * 200.0f + 0.5f);
	}

	void* VPADSetSamplingCallback(uint32 chan, void* callback)
	{
		Channel* c = Get(chan);
		if (!c)
			return nullptr;
		void* previous = c->samplingCb.GetPtr();
		c->samplingCb = callback;
		return previous;
	}

	// --- accelerometer --------------------------------------------------------------

	void VPADGetAccParam(uint32 chan, float32be* playRadius, float32be* sensitivity)
	{
		if (Channel* c = Get(chan))
		{
			*playRadius = c->accRadius;
			*sensitivity = c->accSensitivity;
		}
	}

	void VPADSetAccParam(uint32 chan, float playRadius, float sensitivity)
	{
		if (Channel* c = Get(chan))
		{
			c->accRadius = playRadius;
			c->accSensitivity = sensitivity;
		}
	}

	uint32 VPADGetAccPlayMode(uint32 chan)
	{
		Channel* c = Get(chan);
		return c ? c->accPlayMode : kPlayModeTight;
	}

	void VPADSetAccPlayMode(uint32 chan, uint32 mode)
	{
		if (Channel* c = Get(chan))
			c->accPlayMode = mode;
	}

	// --- sticks -----------------------------------------------------------------

	void VPADEnableStickCrossClamp(uint32 chan)
	{
		if (Channel* c = Get(chan))
			c->stickCrossClamp = true;
	}

	void VPADDisableStickCrossClamp(uint32 chan)
	{
		if (Channel* c = Get(chan))
			c->stickCrossClamp = false;
	}

	void VPADSetLStickClampThreshold(uint32 chan, sint32 maxValue, sint32 minValue)
	{
		if (Channel* c = Get(chan))
		{
			c->leftClampMax = std::min(maxValue, kStickClampMax);
			c->leftClampMin = std::max(minValue, kStickClampMin);
		}
	}

	void VPADSetRStickClampThreshold(uint32 chan, sint32 maxValue, sint32 minValue)
	{
		if (Channel* c = Get(chan))
		{
			c->rightClampMax = std::min(maxValue, kStickClampMax);
			c->rightClampMin = std::max(minValue, kStickClampMin);
		}
	}

	void VPADGetLStickClampThreshold(uint32 chan, uint32be* maxValue, uint32be* minValue)
	{
		if (Channel* c = Get(chan))
		{
			*maxValue = (uint32)c->leftClampMax;
			*minValue = (uint32)c->leftClampMin;
		}
	}

	void VPADGetRStickClampThreshold(uint32 chan, uint32be* maxValue, uint32be* minValue)
	{
		if (Channel* c = Get(chan))
		{
			*maxValue = (uint32)c->rightClampMax;
			*minValue = (uint32)c->rightClampMin;
		}
	}

	void VPADSetCrossStickEmulationParamsL(uint32 chan, float rotation, float inputRange, float radius)
	{
		if (Channel* c = Get(chan))
		{
			c->crossL[0] = rotation;
			c->crossL[1] = inputRange;
			c->crossL[2] = radius;
		}
	}

	void VPADSetCrossStickEmulationParamsR(uint32 chan, float rotation, float inputRange, float radius)
	{
		if (Channel* c = Get(chan))
		{
			c->crossR[0] = rotation;
			c->crossR[1] = inputRange;
			c->crossR[2] = radius;
		}
	}

	void VPADGetCrossStickEmulationParamsL(uint32 chan, float32be* rotation, float32be* inputRange, float32be* radius)
	{
		if (Channel* c = Get(chan))
		{
			*rotation = c->crossL[0];
			*inputRange = c->crossL[1];
			*radius = c->crossL[2];
		}
	}

	void VPADGetCrossStickEmulationParamsR(uint32 chan, float32be* rotation, float32be* inputRange, float32be* radius)
	{
		if (Channel* c = Get(chan))
		{
			*rotation = c->crossR[0];
			*inputRange = c->crossR[1];
			*radius = c->crossR[2];
		}
	}

	// --- buttons, screen, rumble ------------------------------------------------------

	uint8 VPADGetButtonProcMode(uint32 chan)
	{
		Channel* c = Get(chan);
		return c ? c->buttonProcMode : kButtonProcTight;
	}

	void VPADSetButtonProcMode(uint32 chan, uint8 mode)
	{
		if (Channel* c = Get(chan))
			c->buttonProcMode = mode;
	}

	void VPADSetLcdMode(uint32 chan, uint32 mode)
	{
		if (Channel* c = Get(chan))
			c->lcdMode = mode;
	}

	void VPADGetLcdMode(uint32 chan, uint32be* mode)
	{
		if (Channel* c = Get(chan))
			*mode = c->lcdMode;
	}

	sint32 VPADControlMotor(uint32 chan, uint8* pattern, uint8 length)
	{
		if (length > kMotorPatternMax)
		{
			cemuLog_log(LogType::InputAPI, "VPADControlMotor: pattern of {} truncated to {}", length, kMotorPatternMax);
			length = kMotorPatternMax;
		}
		auto pad = InputManager::instance().get_vpad_controller(chan);
		if (!pad)
			return 0;
		if (length == 0)
		{
			pad->clear_rumble();
			return 0;
		}
		return pad->push_rumble(pattern, length) ? 0 : -1; // -1: pattern queue is full
	}

	void VPADStopMotor(uint32 chan)
	{
		if (auto pad = InputManager::instance().get_vpad_controller(chan))
			pad->clear_rumble();
	}

	// --- touch screen ---------------------------------------------------------

	void VPADGetTPCalibrationParam(uint32 chan, VPADTPCalibrationParam* param)
	{
		if (Channel* c = Get(chan))
		{
			param->offsetX = c->tpOffsetX;
			param->offsetY = c->tpOffsetY;
			param->scaleX = c->tpScaleX;
			param->scaleY = c->tpScaleY;
		}
	}

	void VPADSetTPCalibrationParam(uint32 chan, VPADTPCalibrationParam* param)
	{
		// stored for VPADGetTPCalibrationParam; our panel is always calibrated
		if (Channel* c = Get(chan))
		{
			c->tpOffsetX = param->offsetX;
			c->tpOffsetY = param->offsetY;
			c->tpScaleX = param->scaleX;
			c->tpScaleY = param->scaleY;
		}
	}

	void VPADGetTPCalibratedPointEx(uint32 chan, sint32 resolution, VPADTPData_t* out, VPADTPData_t* raw)
	{
		static constexpr sint32 kSizes[3][2] = {{1920, 1080}, {1280, 720}, {854, 480}};
		const uint16 rawX = raw->x, rawY = raw->y, touch = raw->touch, validity = raw->validity;
		sint32 x = 0, y = 0;
		if (resolution >= 0 && resolution < 3)
			TouchRawToScreen(rawX, rawY, kSizes[resolution][0], kSizes[resolution][1], x, y);
		else
			cemuLog_log(LogType::InputAPI, "VPADGetTPCalibratedPointEx: unknown resolution {}", resolution);
		out->x = (uint16)x;
		out->y = (uint16)y;
		out->touch = touch;
		out->validity = validity;
	}

	void VPADGetTPCalibratedPoint(uint32 chan, VPADTPData_t* out, VPADTPData_t* raw)
	{
		VPADGetTPCalibratedPointEx(chan, 1, out, raw); // 1280x720, the GamePad's own resolution
	}

	// --- gyro -----------------------------------------------------------------------

	void VPADSetGyroDirection(uint32 chan, VPADDir* dir)
	{
		if (Channel* c = Get(chan))
			c->gyroDirection = *dir;
	}

	void VPADGetGyroZeroDriftMode(uint32 chan, uint32be* mode)
	{
		Channel* c = Get(chan);
		*mode = c ? c->gyroZeroDrift : kZeroDriftNone;
	}

	void VPADSetGyroZeroDriftMode(uint32 chan, uint32 mode)
	{
		Channel* c = Get(chan);
		if (c && mode <= kZeroDriftNone)
			c->gyroZeroDrift = mode;
	}

	void VPADSetGyroDirReviseBase(uint32 chan, VPADDir* base)
	{
		if (Channel* c = Get(chan))
			c->gyroReviseBase = *base;
	}

	void VPADDisableGyroDirRevise(uint32 chan)
	{
		if (Channel* c = Get(chan))
			c->gyroReviseOn = false;
	}

	void VPADSetGyroDirReviseParam(uint32 chan, float weight)
	{
		if (Channel* c = Get(chan))
			c->gyroReviseWeight = weight;
	}

	// --- module -----------------------------------------------------------------

	void load()
	{
	}

	void start()
	{
		// a new title starts with default settings and no callbacks
		for (Channel& c : s_ch)
			c = Channel{};
		coreinit::OSCreateAlarm(s_tickAlarm.GetPtr());
		const uint64 period = coreinit::EspressoTime::GetTimerClock() * 5 / 1000; // 5 ms
		static const MPTR handler = makeCallableExport<&Tick>();
		coreinit::OSSetPeriodicAlarm(s_tickAlarm.GetPtr(), coreinit::OSGetTime(), period, handler);
	}

	class VPADModule : public COSModule
	{
	public:
		std::string_view GetName() override { return "vpad"; }

		void RPLMapped() override
		{
			cafeExportRegister("vpad", VPADRead, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetBtnRepeat, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetSamplingCallback, LogType::InputAPI);

			cafeExportRegister("vpad", VPADGetAccParam, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetAccParam, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetAccPlayMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetAccPlayMode, LogType::InputAPI);

			cafeExportRegister("vpad", VPADEnableStickCrossClamp, LogType::InputAPI);
			cafeExportRegister("vpad", VPADDisableStickCrossClamp, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetLStickClampThreshold, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetRStickClampThreshold, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetLStickClampThreshold, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetRStickClampThreshold, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetCrossStickEmulationParamsL, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetCrossStickEmulationParamsR, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetCrossStickEmulationParamsL, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetCrossStickEmulationParamsR, LogType::InputAPI);

			cafeExportRegister("vpad", VPADGetButtonProcMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetButtonProcMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetLcdMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetLcdMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADControlMotor, LogType::InputAPI);
			cafeExportRegister("vpad", VPADStopMotor, LogType::InputAPI);

			cafeExportRegister("vpad", VPADGetTPCalibrationParam, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetTPCalibrationParam, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetTPCalibratedPoint, LogType::InputAPI);
			cafeExportRegister("vpad", VPADGetTPCalibratedPointEx, LogType::InputAPI);

			cafeExportRegister("vpad", VPADGetGyroZeroDriftMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetGyroDirection, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetGyroZeroDriftMode, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetGyroDirReviseBase, LogType::InputAPI);
			cafeExportRegister("vpad", VPADDisableGyroDirRevise, LogType::InputAPI);
			cafeExportRegister("vpad", VPADSetGyroDirReviseParam, LogType::InputAPI);
		}
	};

	VPADModule s_vpadModule;

	COSModule* GetModule()
	{
		return &s_vpadModule;
	}
}
