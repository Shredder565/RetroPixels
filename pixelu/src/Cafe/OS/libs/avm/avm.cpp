// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// avm manages the console's video outputs. Video players (and a few games)
// check that the TV link is copy-protected before showing protected content.
// We present the protection as available and switched on, and accept any
// attempt to change it.

#include "Cafe/OS/common/OSCommon.h"
#include "avm.h"

namespace avm
{
	namespace
	{
		bool IsHdcpAvailable() { return true; }
		bool IsHdcpOn() { return true; }
		bool IsAnalogProtectionOn() { return true; }
		bool SetAnalogProtectionEnable(sint32 /* enable */) { return true; }

		// Writes the current setting through the pointer. Returns false, which
		// callers treat as "no error".
		bool GetAnalogProtectionEnable(uint32be* enabledOut)
		{
			*enabledOut = 1;
			return false;
		}

		class AvmLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "avm"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(IsHdcpAvailable, "avm", "AVMIsHDCPAvailable", LogType::Placeholder);
				cafeExportRegisterFunc(IsHdcpOn, "avm", "AVMIsHDCPOn", LogType::Placeholder);
				cafeExportRegisterFunc(GetAnalogProtectionEnable, "avm", "AVMGetAnalogContentsProtectionEnable", LogType::Placeholder);
				cafeExportRegisterFunc(IsAnalogProtectionOn, "avm", "AVMIsAnalogContentsProtectionOn", LogType::Placeholder);
				cafeExportRegisterFunc(SetAnalogProtectionEnable, "avm", "AVMSetAnalogContentsProtectionEnable", LogType::Placeholder);
			}
		};

		AvmLibrary g_avmLibrary;
	}

	COSModule* GetModule()
	{
		return &g_avmLibrary;
	}
}
