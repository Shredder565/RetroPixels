// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_pdm is the user-side face of the play-data manager: it lets titles
// (mostly the Daily Log system app) read the per-account play diary. The
// diary itself lives in the IOSU PDM service (Cafe/IOSU/PDM), so these
// exports just forward to it. All calls return 0 (success).

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/IOSU/PDM/iosu_pdm.h"
#include "nn_pdm.h"

namespace nn::pdm
{
	namespace
	{
		constexpr uint32 kResultOk = 0;

		uint32 PlayDiaryMaxLength(uint32be* lengthOut)
		{
			*lengthOut = (uint32)iosu::pdm::NUM_PLAY_DIARY_ENTRIES_MAX;
			return kResultOk;
		}

		uint32 PlayStatsMaxLength(uint32be* lengthOut)
		{
			*lengthOut = (uint32)iosu::pdm::NUM_PLAY_STATS_ENTRIES;
			return kResultOk;
		}

		uint32 ReadPlayDiary(uint32be* entriesRead, iosu::pdm::PlayDiaryEntry* entries, uint32 accountSlot, uint32 capacity)
		{
			*entriesRead = iosu::pdm::GetDiaryEntries((uint8)accountSlot, entries, capacity);
			return kResultOk;
		}

		class PdmLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_pdm"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(PlayDiaryMaxLength, "nn_pdm", "GetPlayDiaryMaxLength__Q2_2nn3pdmFPi", LogType::NN_PDM);
				cafeExportRegisterFunc(PlayStatsMaxLength, "nn_pdm", "GetPlayStatsMaxLength__Q2_2nn3pdmFPi", LogType::NN_PDM);
				cafeExportRegisterFunc(ReadPlayDiary, "nn_pdm", "GetPlayDiary__Q2_2nn3pdmFPiPQ3_2nn3pdm9PlayDiaryiT3", LogType::NN_PDM);
			}
		};

		PdmLibrary g_pdmLibrary;
	}

	COSModule* GetModule()
	{
		return &g_pdmLibrary;
	}
}
