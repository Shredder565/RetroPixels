// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// padscore serves the Wii Remote family of controllers: WPAD* is the raw
// layer (one report per call, connection and extension callbacks), KPAD* the
// processed layer (buttons with trigger/release edges, pointer, motion) that
// most games read. Controllers come from the input manager's WPAD slots; the
// emulated controller classes (input/emulated/WPADController) know how to
// fill the guest structures, we only route the calls.
//
// Connection, extension and sampling callbacks are driven by a 5 ms alarm
// that coreinit arms at boot through padscore::start().

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/RPL/rpl.h"
#include "WindowSystem.h"
#include "input/InputManager.h"

namespace padscore
{
	namespace
	{
		constexpr uint32 kSlots = InputManager::kMaxWPADControllers;

		// KPADRead error codes
		constexpr sint32 kKpadOk = 0;
		constexpr sint32 kKpadNoSamples = -1;
		constexpr sint32 kKpadNoController = -2;

		constexpr uint32 kDevNotFound = 253;
		constexpr uint8 kBatteryFull = 4;
		constexpr uint8 kLedPlayer1 = 1;
		constexpr uint32 kMplsWorkSize = 0x5FE0;
		constexpr sint32 kGravityUnit = 99; // typical Wii Remote 1 g reading

		struct Slot
		{
			MEMPTR<void> connectCb;   // void(chan, result)
			MEMPTR<void> extensionCb; // void(chan, devType)
			MEMPTR<void> samplingCb;  // void(chan)
			MEMPTR<void> dpdCb;
			bool dpdOn = true;
			bool sentInitialDisconnect = false;
			BtnRepeat repeat{};
			uint64 lastKpadRead = 0;
		};

		struct State
		{
			Slot slot[kSlots];
			bool kpadInitialized = false;
			MEMPTR<KPADUnifiedWpadStatus_t> ringBuffer;
			uint32 ringLength = 0;
			bool callbackByKpad = false;
			uint32 maxControllers = kWPADMaxControllers;
		};
		State g;

		SysAllocator<coreinit::OSAlarm_t> s_tickAlarm;

		// WPADGetInfo result
		struct WPADInfo_t
		{
			uint32be dpd;
			uint32be speaker;
			uint32be attach;
			uint32be lowBat;
			uint32be nearempty;
			uint8 batteryLevel;
			uint8 led;
			uint8 protocol;
			uint8 firmware;
		};
		static_assert(sizeof(WPADInfo_t) == 0x18);

		std::shared_ptr<WPADController> Pad(uint32 chan)
		{
			return InputManager::instance().get_wpad_controller(chan);
		}

		void FillInfo(WPADInfo_t* info)
		{
			info->dpd = 0;
			info->speaker = 0;
			info->attach = 0;
			info->lowBat = 0;
			info->nearempty = 0;
			info->batteryLevel = kBatteryFull;
			info->led = kLedPlayer1;
		}

		MEMPTR<void> SwapCallback(MEMPTR<void> Slot::*which, uint32 chan, MEMPTR<void> cb)
		{
			if (chan >= kSlots)
				return nullptr;
			MEMPTR<void> previous = g.slot[chan].*which;
			g.slot[chan].*which = cb;
			return previous;
		}

		void Notify(MEMPTR<void> cb, uint32 chan, sint32 value)
		{
			PPCCoreCallback(cb, chan, value);
		}

		// runs every 5 ms on the alarm thread
		void Tick(coreinit::OSAlarm_t* alarm, void* context)
		{
			for (uint32 i = 0; i < kSlots; i++)
			{
				Slot& sl = g.slot[i];
				if (!sl.connectCb)
					continue;
				if (!sl.sentInitialDisconnect)
				{
					// the first report after a callback is installed is always "not connected"
					sl.sentInitialDisconnect = true;
					Notify(sl.connectCb, i, WPAD_ERR_NO_CONTROLLER);
					continue;
				}
				auto pad = Pad(i);
				if (!pad)
					continue;
				// pressing HOME on the controller is reported as a reconnect so
				// games show their controller check again
				if (pad->m_status == WPADController::ConnectCallbackStatus::ReportDisconnect || pad->was_home_button_down())
				{
					pad->m_status = WPADController::ConnectCallbackStatus::ReportConnect;
					Notify(sl.connectCb, i, WPAD_ERR_NO_CONTROLLER);
				}
				else if (pad->m_status == WPADController::ConnectCallbackStatus::ReportConnect)
				{
					pad->m_status = WPADController::ConnectCallbackStatus::None;
					Notify(sl.connectCb, i, WPAD_ERR_NONE);
				}
			}

			for (uint32 i = 0; i < kSlots; i++)
			{
				if (!g.slot[i].extensionCb)
					continue;
				auto pad = Pad(i);
				if (pad && pad->m_extension_status == WPADController::ConnectCallbackStatus::ReportConnect)
				{
					pad->m_extension_status = WPADController::ConnectCallbackStatus::None;
					Notify(g.slot[i].extensionCb, i, (sint32)pad->get_device_type());
				}
			}

			for (uint32 i = 0; i < kSlots; i++)
			{
				if (g.slot[i].samplingCb && Pad(i))
					PPCCoreCallback(g.slot[i].samplingCb, i);
			}
		}
	}

	// --- WPAD ----------------------------------------------------------------

	sint32 WPADGetStatus()
	{
		return 1; // library is up
	}

	sint32 WPADGetBatteryLevel(uint32 chan)
	{
		return kBatteryFull;
	}

	sint32 WPADProbe(uint32 chan, uint32be* devType)
	{
		auto pad = Pad(chan);
		if (devType)
			*devType = pad ? (uint32)pad->get_device_type() : kDevNotFound;
		return pad ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER;
	}

	sint32 WPADGetInfo(uint32 chan, WPADInfo_t* info)
	{
		if (chan >= kSlots || !Pad(chan))
			return WPAD_ERR_NO_CONTROLLER;
		FillInfo(info);
		return WPAD_ERR_NONE;
	}

	sint32 WPADGetInfoAsync(uint32 chan, WPADInfo_t* info, MPTR callback)
	{
		const sint32 result = WPADGetInfo(chan, info);
		if (callback != MPTR_NULL)
			coreinitAsyncCallback_add(callback, 2, chan, (uint32)result);
		return result;
	}

	sint32 WPADRead(uint32 chan, WPADStatus_t* status)
	{
		if (auto pad = Pad(chan))
			pad->WPADRead(status);
		return WPAD_ERR_NO_CONTROLLER; // WPADRead is void on the console; r3 is not used
	}

	sint32 WPADSetDataFormat(uint32 chan, uint32 format)
	{
		if (auto pad = Pad(chan))
			pad->set_data_format((WPADDataFormat)format);
		return 0;
	}

	sint32 WPADGetDataFormat(uint32 chan)
	{
		auto pad = Pad(chan);
		return pad ? (sint32)pad->get_data_format() : (sint32)kDataFormat_CORE;
	}

	sint32 WPADIsMotorEnabled()
	{
		return 1;
	}

	sint32 WPADControlMotor(uint32 chan, uint32 command)
	{
		if (auto pad = Pad(chan))
		{
			if (command == 1)
				pad->start_rumble();
			else
				pad->stop_rumble();
		}
		return 0;
	}

	sint32 WPADControlDpd(uint32 chan, uint32 command, MPTR callback)
	{
		if (chan >= kSlots || !Pad(chan))
			return WPAD_ERR_NO_CONTROLLER;
		g.slot[chan].dpdCb = MEMPTR<void>(callback);
		if (callback != MPTR_NULL)
			coreinitAsyncCallback_add(callback, 2, chan, WPAD_ERR_NONE);
		return WPAD_ERR_NONE;
	}

	void* WPADSetExtensionCallback(uint32 chan, MEMPTR<void> callback)
	{
		return SwapCallback(&Slot::extensionCb, chan, callback).GetPtr();
	}

	void* KPADSetConnectCallback(uint32 chan, MEMPTR<void> callback)
	{
		return SwapCallback(&Slot::connectCb, chan, callback).GetPtr();
	}

	void* KPADSetSamplingCallback(uint32 chan, MEMPTR<void> callback)
	{
		return SwapCallback(&Slot::samplingCb, chan, callback).GetPtr();
	}

	sint32 WPADSetCallbackByKPAD(uint32 enable)
	{
		g.callbackByKpad = enable != 0;
		return 0;
	}

	sint32 WPADIsMplsAttached(sint32 chan, uint32be* attached, MEMPTR<void> callback)
	{
		if (chan >= kKPADMaxControllers)
			return WPAD_ERR_NO_CONTROLLER;
		auto pad = Pad((uint32)chan);
		*attached = (pad && pad->is_mpls_attached()) ? 1 : 0;
		if (callback)
			Notify(callback, (uint32)chan, pad ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER);
		return WPAD_ERR_NONE;
	}

	struct WPADAccGravity
	{
		sint32be x;
		sint32be y;
		sint32be z;
	};

	void WPADGetAccGravityUnit(sint32 chan, uint32 type, WPADAccGravity* acc)
	{
		if (chan >= kKPADMaxControllers)
			return;
		acc->x = kGravityUnit;
		acc->y = kGravityUnit;
		acc->z = kGravityUnit;
	}

	// --- KPAD ----------------------------------------------------------------

	sint32 KPADInitEx(KPADUnifiedWpadStatus_t* ringBuffer, uint32 length)
	{
		if (g.kpadInitialized)
			return 0;
		for (Slot& sl : g.slot)
			sl.dpdOn = true;
		g.ringBuffer = ringBuffer;
		g.ringLength = length;
		g.kpadInitialized = true;
		return 0;
	}

	sint32 KPADInit()
	{
		return KPADInitEx(nullptr, 0);
	}

	sint32 KPADSetMaxControllers(uint32 count)
	{
		if (count != (uint32)kWPADMaxControllers && count != (uint32)kKPADMaxControllers)
			return WPAD_ERR_INVALID;
		if (count == g.maxControllers)
			return 0;
		g.maxControllers = count;
		g.callbackByKpad = true;
		return (sint32)count;
	}

	sint32 KPADGetMaxControllers()
	{
		return (sint32)g.maxControllers;
	}

	sint32 KPADEnableDPD(sint32 chan)
	{
		if (auto pad = Pad((uint32)chan))
		{
			auto format = pad->get_data_format();
			if (format != kDataFormat_FREESTYLE && format != 0x1F)
				g.slot[chan].dpdOn = true;
		}
		return 0;
	}

	uint32 KPADGetMplsWorkSize()
	{
		return kMplsWorkSize;
	}

	sint32 KPADSetBtnRepeat(uint32 chan, float delaySec, float pulseSec)
	{
		// stored as whole numbers, which is what the emulated controllers expect today
		if (chan < kSlots)
			g.slot[chan].repeat = {(sint32)delaySec, (sint32)pulseSec};
		return 0;
	}

	// Fills one sample. Like the console, a read empties the queue, so reading
	// again within 1 ms gives nothing (Affordable Space Adventures relies on it).
	sint32 KPADReadEx(uint32 chan, KPADStatus_t* buffer, uint32 length, betype<sint32>* error)
	{
		auto setError = [&](sint32 e) { if (error) *error = e; };
		if (chan >= kSlots)
			return 0;
		auto pad = Pad(chan);
		if (!pad)
		{
			setError(kKpadNoController);
			return 0;
		}
		Slot& sl = g.slot[chan];
		const uint64 now = coreinit::OSGetTime();
		if (length == 0 || now - sl.lastKpadRead < (uint64)coreinit::EspressoTime::ConvertNsToTimerTicks(1000000))
		{
			setError(kKpadNoSamples);
			return 0;
		}
		sl.lastKpadRead = now;

		memset((void*)buffer, 0, sizeof(KPADStatus_t));
		buffer->wpadErr = WPAD_ERR_NONE;
		buffer->data_format = (uint8)pad->get_data_format();
		buffer->devType = (uint8)pad->get_device_type();
		// keep the game from reacting to input meant for the controller settings window
		if (!WindowSystem::InputConfigWindowHasFocus())
			pad->KPADRead(*buffer, sl.repeat);
		setError(kKpadOk);
		return 1;
	}

	sint32 KPADRead(uint32 chan, KPADStatus_t* buffer, uint32 length)
	{
		return KPADReadEx(chan, buffer, length, nullptr);
	}

	sint32 KPADGetUnifiedWpadStatus(uint32 chan, KPADUnifiedWpadStatus_t* status, uint32 count)
	{
		if (chan >= kSlots)
			return 0;
		memset((void*)status, 0, sizeof(KPADUnifiedWpadStatus_t) * count);
		if (count == 0)
			return 0;
		auto pad = Pad(chan);
		if (!pad)
		{
			status->u.core.err = WPAD_ERR_NO_CONTROLLER;
			return 0;
		}
		// the newest sample goes into the first entry; the format byte follows the
		// union, so write it after the report
		pad->WPADRead(&status->u.core);
		status->fmt = (uint8)pad->get_data_format();
		return 0;
	}

	// --- module ---------------------------------------------------------------

	void start()
	{
		// a new title starts: forget callbacks the previous one installed
		g = State{};
		coreinit::OSCreateAlarm(s_tickAlarm.GetPtr());
		const uint64 period = coreinit::EspressoTime::GetTimerClock() / 200; // 5 ms
		static const MPTR handler = makeCallableExport<&Tick>();
		coreinit::OSSetPeriodicAlarm(s_tickAlarm.GetPtr(), coreinit::OSGetTime(), period, handler);
	}

	class PadscoreModule : public COSModule
	{
	public:
		std::string_view GetName() override { return "padscore"; }

		void RPLMapped() override
		{
			cafeExportRegister("padscore", WPADGetStatus, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetBatteryLevel, LogType::InputAPI);
			cafeExportRegister("padscore", WPADProbe, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetInfo, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetInfoAsync, LogType::InputAPI);
			cafeExportRegister("padscore", WPADRead, LogType::InputAPI);
			cafeExportRegister("padscore", WPADSetDataFormat, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetDataFormat, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsMotorEnabled, LogType::InputAPI);
			cafeExportRegister("padscore", WPADControlMotor, LogType::InputAPI);
			cafeExportRegister("padscore", WPADControlDpd, LogType::InputAPI);
			cafeExportRegister("padscore", WPADSetExtensionCallback, LogType::InputAPI);
			cafeExportRegisterFunc(KPADSetConnectCallback, "padscore", "WPADSetConnectCallback", LogType::InputAPI);
			cafeExportRegisterFunc(KPADSetSamplingCallback, "padscore", "WPADSetSamplingCallback", LogType::InputAPI);
			cafeExportRegister("padscore", WPADSetCallbackByKPAD, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsMplsAttached, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetAccGravityUnit, LogType::InputAPI);

			cafeExportRegister("padscore", KPADInit, LogType::InputAPI);
			cafeExportRegister("padscore", KPADInitEx, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMaxControllers, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMaxControllers, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableDPD, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMplsWorkSize, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetConnectCallback, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetSamplingCallback, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetBtnRepeat, LogType::InputAPI);
			cafeExportRegister("padscore", KPADRead, LogType::InputAPI);
			cafeExportRegister("padscore", KPADReadEx, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetUnifiedWpadStatus, LogType::InputAPI);
		}
	};

	PadscoreModule s_padscoreModule;

	COSModule* GetModule()
	{
		return &s_padscoreModule;
	}
}
