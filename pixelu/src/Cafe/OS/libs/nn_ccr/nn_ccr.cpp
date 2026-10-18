// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_ccr is the system library that talks to the GamePad (DRC) firmware.
// Games only touch one call of it: the "Caffeine" boot check, which asks
// whether the GamePad was started in the special Caffeine mode. An emulated
// GamePad never is, so the call always answers "no" (-1).

#include "Cafe/OS/common/OSCommon.h"
#include "nn_ccr.h"

namespace nn::ccr
{
	namespace
	{
		constexpr sint32 kNotCaffeineBoot = -1;

		sint32 SysCaffeineBootCheck()
		{
			return kNotCaffeineBoot;
		}

		class CcrLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_ccr"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(SysCaffeineBootCheck, "nn_ccr", "CCRSysCaffeineBootCheck", LogType::Placeholder);
			}
		};

		CcrLibrary g_ccrLibrary;
	}

	COSModule* GetModule()
	{
		return &g_ccrLibrary;
	}
}
