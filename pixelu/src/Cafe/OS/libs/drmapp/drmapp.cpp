// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// drmapp runs background checks when a title starts: system update (NUP),
// title patch, add-on content and ticket checks. A game polls the matching
// *ChkIsFinished() call until it returns true. Nothing is checked here, so
// every check reports that it is already finished.

#include "Cafe/OS/common/OSCommon.h"
#include "drmapp.h"

namespace drmapp
{
	namespace
	{
		constexpr uint32 kFinished = 1;

		uint32 SystemUpdateCheckDone(uint32 /* unused */) { return kFinished; }
		uint32 PatchCheckDone() { return kFinished; }
		uint32 AddOnContentCheckDone() { return kFinished; }
		uint32 TicketCheckDone() { return kFinished; }

		class DrmAppLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "drmapp"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(SystemUpdateCheckDone, "drmapp", "NupChkIsFinished__3RplFv", LogType::Placeholder);
				cafeExportRegisterFunc(PatchCheckDone, "drmapp", "PatchChkIsFinished__3RplFv", LogType::Placeholder);
				cafeExportRegisterFunc(AddOnContentCheckDone, "drmapp", "AocChkIsFinished__3RplFv", LogType::Placeholder);
				cafeExportRegisterFunc(TicketCheckDone, "drmapp", "TicketChkIsFinished__3RplFv", LogType::Placeholder);
			}
		};

		DrmAppLibrary g_drmAppLibrary;
	}

	COSModule* GetModule()
	{
		return &g_drmAppLibrary;
	}
}
