// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_cmpt prepares hand-over to Wii mode (vWii). Titles that use it ask for
// two things: the parental-control settings to forward, and how large the
// buffer for the hand-over data must be.
//   CMPTAcctGetPcConf(conf)  -> three words of parental-control config;
//                               all zero = no restrictions.
//   CMPTGetDataSize(size)    -> required work buffer size, 0xC0000 bytes.

#include "Cafe/OS/common/OSCommon.h"
#include "nn_cmpt.h"

namespace nn::cmpt
{
	namespace
	{
		constexpr uint32 kResultOk = 0;
		constexpr uint32 kHandoverBufferSize = 0xC0000;

		struct ParentalConfig
		{
			uint32be words[3];
		};
		static_assert(sizeof(ParentalConfig) == 12);

		uint32 AcctGetPcConf(ParentalConfig* config)
		{
			*config = {};
			return kResultOk;
		}

		uint32 GetDataSize(uint32be* sizeOut)
		{
			*sizeOut = kHandoverBufferSize;
			return kResultOk;
		}

		class CmptLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_cmpt"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(AcctGetPcConf, "nn_cmpt", "CMPTAcctGetPcConf", LogType::Placeholder);
				cafeExportRegisterFunc(GetDataSize, "nn_cmpt", "CMPTGetDataSize", LogType::Placeholder);
			}
		};

		CmptLibrary g_cmptLibrary;
	}

	COSModule* GetModule()
	{
		return &g_cmptLibrary;
	}
}
