// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_aoc lets a game find and open its add-on content (DLC).
//
// When a game boots, PixelU mounts each installed DLC title of that game at
// /vol/aoc<16 hex digit title id> (see CafeSystem.cpp). AOC_ListTitle reports
// exactly those mounts, AOC_OpenTitle hands back the mount path, and
// AOC_GetPurchaseInfo says which DLC items are owned. Ownership comes from the
// DLC's ticket (title.tik) when one is installed; without a ticket every item
// counts as owned, so installed DLC always works.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "nn_aoc.h"

namespace nn::aoc
{
	namespace
	{
		constexpr uint32 kAocOk = 0;
		constexpr uint32 kMaxListed = 256;

		// AOC_TitleListType, one entry per DLC title
		struct AocTitle
		{
			uint64be titleId;
			uint32be groupId;
			uint16be version;
			char path[88];
			uint8 _pad[2];
		};
		static_assert(sizeof(AocTitle) == 0x68);

		std::vector<uint64> g_mountedAoc;
		bool g_scanned = false;

		std::string MountPathFor(uint64 titleId)
		{
			return fmt::format("/vol/aoc{:016x}", titleId);
		}

		// Looks at /vol once per game for directories named aoc<titleId>.
		void ScanMountedAoc()
		{
			if (g_scanned)
				return;
			g_scanned = true;
			g_mountedAoc.clear();

			sint32 status = 0;
			FSCVirtualFile* dir = fsc_openDirIterator("/vol", &status);
			if (!dir)
				return;
			constexpr std::string_view prefix = "aoc";
			FSCDirEntry entry;
			while (fsc_nextDir(dir, &entry))
			{
				if (!entry.isDirectory)
					continue;
				std::string_view name = entry.GetPath();
				if (name.size() != prefix.size() + 16 || !name.starts_with(prefix))
					continue;
				TitleId id;
				if (TitleIdParser::ParseFromStr(name.substr(prefix.size()), id))
					g_mountedAoc.push_back(id);
			}
			fsc_close(dir);
		}

		uint32 CalculateWorkBufferSize(uint32 maxTitles)
		{
			const uint32 n = std::min(maxTitles, kMaxListed);
			return 0x80 + n * 0x61;
		}

		uint32 ListTitle(uint32be* countOut, AocTitle* titles, uint32 maxTitles, void* workBuffer, uint32 workBufferSize)
		{
			ScanMountedAoc();
			const uint32 count = std::min<uint32>(maxTitles, (uint32)g_mountedAoc.size());
			for (uint32 i = 0; i < count; i++)
			{
				AocTitle& t = titles[i];
				t.titleId = g_mountedAoc[i];
				t.groupId = 0;
				t.version = 0;
				const std::string path = MountPathFor(g_mountedAoc[i]);
				memset(t.path, 0, sizeof(t.path));
				memcpy(t.path, path.data(), std::min(path.size(), sizeof(t.path) - 1));
			}
			*countOut = count;
			return kAocOk;
		}

		uint32 OpenTitle(char* mountPathOut, AocTitle* title, void* workBuffer, uint32 workBufferSize)
		{
			const size_t len = strnlen(title->path, sizeof(title->path));
			memcpy(mountPathOut, title->path, len);
			mountPathOut[len] = '\0';
			return kAocOk;
		}

		uint32 CloseTitle(void* title)
		{
			return kAocOk;
		}

		// Reads the installed DLC ticket of titleId, if there is one.
		bool LoadTicket(uint64 titleId, NCrypto::ETicketParser& ticket)
		{
			// tickets of DLC installed through PixelU's downloader sit next to its code
			const fs::path path = ActiveSettings::GetMlcPath("usr/title/{:08x}/{:08x}/code/title.tik", (uint32)(titleId >> 32), (uint32)titleId);
			std::unique_ptr<FileStream> file(FileStream::openFile2(path));
			if (!file)
				return false;
			std::vector<uint8> data;
			file->extract(data);
			if (data.empty())
				return false;
			if (!ticket.parse(data.data(), data.size()))
			{
				cemuLog_log(LogType::Force, "nn_aoc: could not parse DLC ticket {:016x}, treating all items as owned", titleId);
				return false;
			}
			return true;
		}

		uint32 GetPurchaseInfo(uint32be* ownedOut, uint64 titleId, uint16be* itemIds, uint32 itemCount, void* workBuffer, uint32 workBufferSize)
		{
			NCrypto::ETicketParser ticket;
			const bool haveTicket = LoadTicket(titleId, ticket);
			if (haveTicket)
				cemuLog_log(LogType::Force, "nn_aoc: DLC ownership taken from ticket of {:016x}", titleId);
			for (uint32 i = 0; i < itemCount; i++)
				ownedOut[i] = haveTicket ? (ticket.CheckRight((uint16)itemIds[i]) ? 1 : 0) : 1;
			return kAocOk;
		}

		class AocLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_aoc"; }

			void RPLMapped() override
			{
				g_scanned = false;
				g_mountedAoc.clear();
				cafeExportRegisterFunc(CalculateWorkBufferSize, "nn_aoc", "AOC_CalculateWorkBufferSize", LogType::NN_AOC);
				cafeExportRegisterFunc(ListTitle, "nn_aoc", "AOC_ListTitle", LogType::NN_AOC);
				cafeExportRegisterFunc(OpenTitle, "nn_aoc", "AOC_OpenTitle", LogType::NN_AOC);
				cafeExportRegisterFunc(CloseTitle, "nn_aoc", "AOC_CloseTitle", LogType::NN_AOC);
				cafeExportRegisterFunc(GetPurchaseInfo, "nn_aoc", "AOC_GetPurchaseInfo", LogType::NN_AOC);
			}

			void RPLUnmapped() override
			{
				g_scanned = false;
				g_mountedAoc.clear();
			}
		};

		AocLibrary g_aocLibrary;
	}

	COSModule* GetModule()
	{
		return &g_aocLibrary;
	}
}
