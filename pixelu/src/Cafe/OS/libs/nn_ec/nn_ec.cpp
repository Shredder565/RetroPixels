// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_ec is the in-game eShop (commerce) library. There is no shop to talk to,
// so only what games touch while starting up is provided: the constructor of
// nn::ec::Money(const char* value, const char* currency, const char* amount).
// It is a C++ constructor, so it must hand back the object pointer it was
// given. The object itself is left as the game allocated it.

#include "Cafe/OS/common/OSCommon.h"
#include "nn_ec.h"

namespace nn::ec
{
	namespace
	{
		void* MoneyConstruct(void* self, const char* value, const char* currency, const char* amount)
		{
			return self;
		}

		class EcLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_ec"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(MoneyConstruct, "nn_ec", "__ct__Q3_2nn2ec5MoneyFPCcN21", LogType::Placeholder);
			}
		};

		EcLibrary g_ecLibrary;
	}

	COSModule* GetModule()
	{
		return &g_ecLibrary;
	}
}
