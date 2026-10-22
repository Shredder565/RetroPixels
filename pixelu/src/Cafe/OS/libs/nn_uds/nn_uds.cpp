// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_uds is the local wireless (UDS) library. Local wireless play is not
// emulated. The one export games reach is the library's static initializer
// for uds_Api.cpp, which the RPL runs on load and which returns the address
// of the library's internal state block. We give it a small zeroed block in
// emulated system memory, created once per loaded game.

#include "Cafe/OS/common/OSCommon.h"
#include "nn_uds.h"

namespace nn::uds
{
	namespace
	{
		constexpr uint32 kStateBlockSize = 32;

		MPTR g_stateBlock = MPTR_NULL;

		MPTR ApiStaticInit()
		{
			if (g_stateBlock == MPTR_NULL)
			{
				g_stateBlock = coreinit_allocFromSysArea(kStateBlockSize, 32);
				memset(memory_getPointerFromVirtualOffset(g_stateBlock), 0, kStateBlockSize);
			}
			return g_stateBlock;
		}

		class UdsLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_uds"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(ApiStaticInit, "nn_uds", "__sti___11_uds_Api_cpp_f5d9abb2", LogType::Placeholder);
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
			{
				// system memory is recreated for every game, so forget the old block
				g_stateBlock = MPTR_NULL;
			}
		};

		UdsLibrary g_udsLibrary;
	}

	COSModule* GetModule()
	{
		return &g_udsLibrary;
	}
}
