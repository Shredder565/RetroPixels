// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/IOSU/iosu_types_common.h"

namespace iosu
{
	namespace pdm
	{
		inline constexpr size_t NUM_PLAY_STATS_ENTRIES = 256;
		inline constexpr size_t NUM_PLAY_DIARY_ENTRIES_MAX = 18250; // capacity of the diary ring

		// one record of PlayDiary.dat (big endian, as stored)
		struct PlayDiaryEntry
		{
			uint64be titleId;
			uint32be ukn08; // likely minutes played
			uint16be dayIndex; // days since 2000-01-01
			uint16be ukn0E;
		};

		uint32 GetDiaryEntries(uint8 accountSlot, PlayDiaryEntry* diaryEntries, uint32 maxEntries);

		// summary shown in the game list
		struct GameListStat
		{
			struct LastPlayDate
			{
				uint32 year; // 0 = never played
				uint32 month; // zero based
				uint32 day;

				bool operator<(const LastPlayDate& b) const;
				bool operator==(const LastPlayDate& b) const;
				std::weak_ordering operator<=>(const LastPlayDate& b) const;
			}last_played;
			uint32 numMinutesPlayed;
		};

		bool GetStatForGamelist(uint64 titleId, GameListStat& stat);

		IOSUModule* GetModule();
	};
};
