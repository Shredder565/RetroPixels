// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_sl backs the Wii U Menu's quick-start list. Its interfaces are C++
// objects with GHS-style vtables: each vtable slot is 8 bytes (two 16-bit
// this-adjust offsets, then the function address), slot 0 is RTTI, slot 1
// the destructor.
//
// Only the default WhiteListAccessor is offered: an object whose Get(out)
// method returns an empty white list (no title types, no title ids).
//
// Note: like before, this module is compiled but not listed in
// GetCOSModules() (Cafe/OS/RPL/COSModule.cpp), so it is currently inactive.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "nn_sl.h"

namespace nn::sl
{
	namespace
	{
		struct GhsVTableSlot
		{
			uint16be thisAdjustA;
			uint16be thisAdjustB;
			uint32be function;
		};
		static_assert(sizeof(GhsVTableSlot) == 8);

#pragma pack(push, 1)
		struct WhiteList
		{
			uint32be titleTypes[50];
			uint32be titleTypeCount;
			uint32be _pad;
			uint64be titleIds[50];
			uint32be titleIdCount;
		};
#pragma pack(pop)
		static_assert(sizeof(WhiteList) == 0x264);

		struct WhiteListAccessorObject
		{
			uint32be vtable;
		};
		static_assert(sizeof(WhiteListAccessorObject) == 4);

		struct WhiteListAccessorVTable
		{
			GhsVTableSlot rtti;
			GhsVTableSlot destructor;
			GhsVTableSlot get;
		};

		SysAllocator<WhiteListAccessorVTable> g_whiteListVTable;
		SysAllocator<WhiteListAccessorObject> g_defaultWhiteListAccessor;

		// virtual ~WhiteListAccessor(); bit 0 of flags = also free the memory.
		// The only accessor that exists is the static default one, which is
		// never freed, so there is nothing to do.
		void WhiteListDestructor(WhiteListAccessorObject* self, uint32 flags)
		{
		}

		// virtual Result Get(WhiteList* out);
		uint32 WhiteListGet(WhiteListAccessorObject* self, WhiteList* out)
		{
			if (out)
				*out = WhiteList{};
			return 0;
		}

		WhiteListAccessorObject* GetDefaultWhiteListAccessor()
		{
			return g_defaultWhiteListAccessor.GetPtr();
		}

		void BuildDefaultAccessor()
		{
			WhiteListAccessorVTable& vt = *g_whiteListVTable.GetPtr();
			vt = {};
			vt.destructor.function = makeCallableExport<&WhiteListDestructor>();
			vt.get.function = makeCallableExport<&WhiteListGet>();
			g_defaultWhiteListAccessor->vtable = g_whiteListVTable.GetMPTR();
		}

		class SlLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_sl"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(GetDefaultWhiteListAccessor, "nn_sl", "GetDefaultWhiteListAccessor__Q2_2nn2slFv", LogType::NN_SL);
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
			{
				if (reason == coreinit::RplEntryReason::Loaded)
					BuildDefaultAccessor();
			}
		};

		SlLibrary g_slLibrary;
	}

	COSModule* GetModule()
	{
		return &g_slLibrary;
	}
}
