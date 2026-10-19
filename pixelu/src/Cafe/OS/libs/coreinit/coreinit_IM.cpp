// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"

// IM: idle management. Covers automatic power down (APD) and screen dimming. The emulated console
// never powers down or dims; these calls only report fixed settings.
namespace coreinit
{
	namespace
	{
		constexpr uint32 kIMSuccess = 0;
		constexpr uint32 kSecondsBeforeApd = 30 * 60;
		constexpr uint32 kSecondsBeforeDimming = 30 * 60;
		constexpr uint32 kApdPeriodSeconds = 600;

		bool s_dimmingEnabled = true;

		// IMIsAPDEnabledBySysSettings(uint32be* enabled)
		void Export_IMIsAPDEnabledBySysSettings(PPCInterpreter_t* hCPU)
		{
			ppcDefineParamTypePtr(enabledOut, uint32be, 0);
			*enabledOut = 0;
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IMGetTimeBeforeAPD(uint32be* seconds)
		void Export_IMGetTimeBeforeAPD(PPCInterpreter_t* hCPU)
		{
			memory_writeU32(hCPU->gpr[3], kSecondsBeforeApd);
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IMGetTimeBeforeDimming(uint32be* seconds)
		void Export_IMGetTimeBeforeDimming(PPCInterpreter_t* hCPU)
		{
			memory_writeU32(hCPU->gpr[3], kSecondsBeforeDimming);
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		void Export_IMEnableDim(PPCInterpreter_t* hCPU)
		{
			s_dimmingEnabled = true;
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IMIsDimEnabled(uint32be* enabled)
		void Export_IMIsDimEnabled(PPCInterpreter_t* hCPU)
		{
			memory_writeU32(hCPU->gpr[3], s_dimmingEnabled ? 1 : 0);
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IMGetAPDPeriod(uint32be* seconds)
		void Export_IMGetAPDPeriod(PPCInterpreter_t* hCPU)
		{
			cemuLog_logDebug(LogType::Force, "IMGetAPDPeriod({:08x})", hCPU->gpr[3]);
			memory_writeU32(hCPU->gpr[3], kApdPeriodSeconds);
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IM_GetParameter(handle, ?, parameterId, output, ?, ?)
		void Export_IM_GetParameter(PPCInterpreter_t* hCPU)
		{
			cemuLog_logDebug(LogType::Force, "IM_GetParameter()");
			ppcDefineParamS32(parameterId, 2);
			ppcDefineParamStructPtr(output, void, 3);
			if (parameterId == 0) // seconds of inactivity
				*(uint32be*)output = kApdPeriodSeconds;
			else
				cemu_assert_unimplemented();
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IM_GetRuntimeParameter(parameterId, output)
		void Export_IM_GetRuntimeParameter(PPCInterpreter_t* hCPU)
		{
			cemuLog_logDebug(LogType::Force, "IM_GetRuntimeParameter()");
			ppcDefineParamS32(parameterId, 0);
			ppcDefineParamStructPtr(output, void, 1);
			if (parameterId == 8) // whether the previous session ended by power down
				*(uint32be*)output = 0;
			else
				cemu_assert_unimplemented();
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}

		// IM_GetHomeButtonParams(handle, ipcBuffer, output, ?, ?)
		// The two output words are not understood. Zero for both keeps the browser (surf.rpx) from
		// panicking; the download manager (scope.rpx) would want 1 and 2 instead.
		void Export_IM_GetHomeButtonParams(PPCInterpreter_t* hCPU)
		{
			ppcDefineParamMPTR(output, 2);
			memory_writeU32(output + 0, 0);
			memory_writeU32(output + 4, 0);
			osLib_returnFromFunction(hCPU, kIMSuccess);
		}
	}

	void InitializeIM()
	{
		osLib_addFunction("coreinit", "IMIsAPDEnabledBySysSettings", Export_IMIsAPDEnabledBySysSettings);
		osLib_addFunction("coreinit", "IMGetTimeBeforeAPD", Export_IMGetTimeBeforeAPD);
		osLib_addFunction("coreinit", "IMGetTimeBeforeDimming", Export_IMGetTimeBeforeDimming);
		osLib_addFunction("coreinit", "IMEnableDim", Export_IMEnableDim);
		osLib_addFunction("coreinit", "IMIsDimEnabled", Export_IMIsDimEnabled);
		osLib_addFunction("coreinit", "IMGetAPDPeriod", Export_IMGetAPDPeriod);
		osLib_addFunction("coreinit", "IM_GetHomeButtonParams", Export_IM_GetHomeButtonParams);
		osLib_addFunction("coreinit", "IM_GetParameter", Export_IM_GetParameter);
		osLib_addFunction("coreinit", "IM_GetRuntimeParameter", Export_IM_GetRuntimeParameter);
	}
}
