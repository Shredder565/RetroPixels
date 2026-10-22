// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_temp hands out temporary directories that a title can use as scratch
// space. Games call TEMPCreateAndInitTempDir(maxSize, devicePreference,
// outDirId) and only need a successful result plus a 64-bit directory id
// they can pass back later. We do not back the id with a real directory yet;
// every call simply produces a new, distinct id.

#include "Cafe/OS/common/OSCommon.h"
#include "nn_temp.h"

namespace nn::temp
{
	namespace
	{
		constexpr sint32 kTempStatusOk = 0;

		// SplitMix64 sequence: cheap, never repeats within 2^64 calls.
		uint64 g_nextDirSeed = 0x5EED0F7E3B9A4C21ULL;

		uint64 MakeDirId()
		{
			g_nextDirSeed += 0x9E3779B97F4A7C15ULL;
			uint64 z = g_nextDirSeed;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
			return z ^ (z >> 31);
		}

		sint32 CreateAndInitTempDir(uint32 maxSize, uint32 devicePreference, uint64be* dirIdOut)
		{
			if (dirIdOut)
				*dirIdOut = MakeDirId();
			return kTempStatusOk;
		}

		class TempLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_temp"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(CreateAndInitTempDir, "nn_temp", "TEMPCreateAndInitTempDir", LogType::Placeholder);
			}
		};

		TempLibrary g_tempLibrary;
	}

	COSModule* GetModule()
	{
		return &g_tempLibrary;
	}
}
