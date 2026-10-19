// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Play time tracking (PDM). Two files in the system save area of the mlc:
//   PlayStats.dat  be32 entry count + 256 fixed size per title records
//   PlayDiary.dat  be32 read/write index header + ring of diary records
// While a title runs a background thread adds the elapsed minutes to its
// PlayStats record.
#include <util/helpers/helpers.h>
#include "iosu_pdm.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "util/helpers/Semaphore.h"

#if BOOST_OS_LINUX
// older libstdc++ versions lack the C++20 calendar types, use the reference implementation
#include "Common/unix/date.h"
namespace chrono_d = date;
#else
namespace chrono_d = std::chrono;
#endif

namespace iosu
{
	namespace pdm
	{
		namespace
		{
			// all data is tracked for the first account only
			fs::path DataFilePath(const char* name)
			{
				return ActiveSettings::GetMlcPath(fmt::format("usr/save/system/pdm/80000001/{}", name));
			}

			void EnsureDataDirectory()
			{
				std::error_code ec;
				fs::create_directories(DataFilePath("."), ec);
			}

			// days are counted from 2000-01-01
			const chrono_d::sys_days kEpoch{ chrono_d::year(2000) / chrono_d::January / chrono_d::day(1) };

			uint16 TodayDayNumber()
			{
				const chrono_d::sys_days today = chrono_d::floor<chrono_d::days>(std::chrono::system_clock::now());
				return (uint16)(today - kEpoch).count();
			}

			chrono_d::year_month_day DayNumberToDate(uint16 day)
			{
				return chrono_d::year_month_day(kEpoch + chrono_d::days(day));
			}

			/* ---- PlayStats.dat ---- */

			struct TitleStats
			{
				uint32be titleIdHigh;
				uint32be titleIdLow;
				uint32be totalMinutesPlayed;
				uint16be numTimesLaunched;
				uint16be firstLaunchDayIndex;
				uint16be mostRecentDayIndex;
				uint16be ukn12;
			};
			static_assert(sizeof(TitleStats) == 0x14);

			constexpr uint32 kStatsTableBytes = NUM_PLAY_STATS_ENTRIES * sizeof(TitleStats);
			static_assert(kStatsTableBytes == 0x1400);
			constexpr uint64 kStatsFileSize = 4 + kStatsTableBytes;

			class PlayStatsStore
			{
			public:
				void Load()
				{
					std::lock_guard lock(m_mutex);
					ResetNoLock();
					cemu_assert_debug(!m_file);
					m_file = FileStream::openFile2(DataFilePath("PlayStats.dat"), true);
					if (!m_file)
					{
						CreateNoLock();
						return;
					}
					if (m_file->GetSize() != kStatsFileSize)
					{
						// keep the file untouched so it can be repaired by hand; tracking stays off
						CloseNoLock();
						cemuLog_log(LogType::Force, "PlayStats.dat malformed. Time tracking wont be used");
						return;
					}
					m_count = 0;
					m_file->readData(&m_count, sizeof(m_count));
					if (m_count > NUM_PLAY_STATS_ENTRIES)
						m_count = NUM_PLAY_STATS_ENTRIES;
					m_file->readData(m_records, kStatsTableBytes);
				}

				void Close()
				{
					std::lock_guard lock(m_mutex);
					CloseNoLock();
				}

				void Unload()
				{
					std::lock_guard lock(m_mutex);
					cemu_assert_debug(!m_file);
					m_count = 0;
					for (TitleStats& r : m_records)
						r = TitleStats{};
				}

				// returns false when the title has no record
				bool Lookup(uint64 titleId, uint32& minutes, uint16& lastDay)
				{
					std::lock_guard lock(m_mutex);
					const TitleStats* r = FindNoLock(titleId);
					if (!r)
						return false;
					minutes = r->totalMinutesPlayed;
					lastDay = r->mostRecentDayIndex;
					return true;
				}

				// marks a launch of the title and returns the slot used for its record
				size_t BeginSession(uint64 titleId)
				{
					std::lock_guard lock(m_mutex);
					if (TitleStats* r = FindNoLock(titleId))
					{
						r->mostRecentDayIndex = TodayDayNumber();
						r->numTimesLaunched += 1;
						StoreNoLock(*r, false);
						return r - m_records;
					}
					bool grew = false;
					TitleStats* r;
					if (m_count < NUM_PLAY_STATS_ENTRIES)
					{
						r = m_records + m_count;
						m_count += 1;
						grew = true;
					}
					else
					{
						// table is full: recycle the record with the least play time
						r = m_records;
						for (size_t i = 1; i < NUM_PLAY_STATS_ENTRIES; i++)
							if (m_records[i].totalMinutesPlayed < r->totalMinutesPlayed)
								r = m_records + i;
					}
					r->titleIdHigh = (uint32)(titleId >> 32);
					r->titleIdLow = (uint32)titleId;
					r->firstLaunchDayIndex = TodayDayNumber();
					r->mostRecentDayIndex = r->firstLaunchDayIndex;
					r->numTimesLaunched = 1;
					r->totalMinutesPlayed = 0;
					r->ukn12 = 0;
					StoreNoLock(*r, grew);
					return r - m_records;
				}

				void AddMinutes(size_t slot, uint32 minutes)
				{
					std::lock_guard lock(m_mutex);
					if (minutes == 0)
						return;
					TitleStats& r = m_records[slot];
					r.totalMinutesPlayed += minutes;
					r.mostRecentDayIndex = TodayDayNumber();
					StoreNoLock(r, false);
				}

			private:
				void ResetNoLock()
				{
					m_count = 0;
					memset(m_records, 0, sizeof(m_records));
				}

				void CreateNoLock()
				{
					m_file = FileStream::createFile2(DataFilePath("PlayStats.dat"));
					if (!m_file)
					{
						cemuLog_log(LogType::Force, "Unable to open or create PlayStats.dat");
						return;
					}
					const uint32be zero = 0;
					m_file->writeData(&zero, sizeof(zero));
					m_file->writeData(m_records, kStatsTableBytes);
				}

				void CloseNoLock()
				{
					delete m_file;
					m_file = nullptr;
				}

				TitleStats* FindNoLock(uint64 titleId)
				{
					const uint32 high = (uint32)(titleId >> 32);
					const uint32 low = (uint32)titleId;
					for (uint32 i = 0; i < (uint32)m_count; i++)
						if (m_records[i].titleIdHigh == high && m_records[i].titleIdLow == low)
							return m_records + i;
					return nullptr;
				}

				void StoreNoLock(const TitleStats& r, bool withCount)
				{
					if (!m_file)
						return;
					const size_t slot = &r - m_records;
					cemu_assert(slot < NUM_PLAY_STATS_ENTRIES);
					m_file->SetPosition(4 + slot * sizeof(TitleStats));
					if (m_file->writeData(&r, sizeof(TitleStats)) != sizeof(TitleStats))
					{
						cemuLog_log(LogType::Force, "Failed to write to PlayStats.dat");
						return;
					}
					if (withCount)
					{
						const uint32be count = m_count;
						m_file->SetPosition(0);
						m_file->writeData(&count, sizeof(count));
					}
				}

				std::mutex m_mutex;
				FileStream* m_file = nullptr;
				uint32be m_count = 0;
				TitleStats m_records[NUM_PLAY_STATS_ENTRIES]{};
			};

			PlayStatsStore g_stats;

			/* ---- PlayDiary.dat ---- */

			// ring buffer: entries [readIndex, writeIndex) are valid
			struct DiaryHeader
			{
				uint32be readIndex;
				uint32be writeIndex;
			};
			static_assert(sizeof(DiaryHeader) == 0x8);
			static_assert(sizeof(PlayDiaryEntry) == 0x10);

			class PlayDiaryStore
			{
			public:
				void Load()
				{
					std::lock_guard lock(m_mutex);
					cemu_assert_debug(!m_file);
					m_file = FileStream::openFile2(DataFilePath("PlayDiary.dat"), true);
					if (!m_file)
					{
						CreateNoLock();
						return;
					}
					if (m_file->readData(&m_header, sizeof(m_header)) != sizeof(m_header))
					{
						cemuLog_log(LogType::Force, "Failed to read valid PlayDiary header");
						CloseNoLock();
						CreateNoLock();
						return;
					}
					const uint32 readIndex = m_header.readIndex;
					const uint32 writeIndex = m_header.writeIndex;
					if (readIndex > NUM_PLAY_DIARY_ENTRIES_MAX || writeIndex > NUM_PLAY_DIARY_ENTRIES_MAX)
					{
						cemuLog_log(LogType::Force, "Bad value in play diary header (read={} write={})", readIndex, writeIndex);
						m_header.readIndex = readIndex % NUM_PLAY_DIARY_ENTRIES_MAX;
						m_header.writeIndex = writeIndex % NUM_PLAY_DIARY_ENTRIES_MAX;
					}
					const uint32 bytes = m_file->readData(m_entries, (uint32)sizeof(m_entries));
					for (size_t i = bytes / sizeof(PlayDiaryEntry); i < NUM_PLAY_DIARY_ENTRIES_MAX; i++)
						m_entries[i] = PlayDiaryEntry{};
				}

				void Close()
				{
					std::lock_guard lock(m_mutex);
					CloseNoLock();
				}

				void Unload()
				{
					std::lock_guard lock(m_mutex);
					cemu_assert_debug(!m_file);
					m_header.readIndex = 0;
					m_header.writeIndex = 0;
					for (PlayDiaryEntry& e : m_entries)
						e = PlayDiaryEntry{};
				}

				uint32 Read(PlayDiaryEntry* out, uint32 maxEntries)
				{
					std::lock_guard lock(m_mutex);
					uint32 n = 0;
					for (uint32 i = m_header.readIndex; i != (uint32)m_header.writeIndex && n < maxEntries; i = (i + 1) % NUM_PLAY_DIARY_ENTRIES_MAX)
						out[n++] = m_entries[i];
					return n;
				}

			private:
				void CreateNoLock()
				{
					EnsureDataDirectory();
					cemu_assert_debug(!m_file);
					m_file = FileStream::createFile2(DataFilePath("PlayDiary.dat"));
					if (!m_file)
						cemuLog_log(LogType::Force, "Failed to read or write PlayDiary.dat, playtime tracking will not be possible");
					m_header.readIndex = 0;
					m_header.writeIndex = 0;
					if (m_file)
						m_file->writeData(&m_header, sizeof(m_header));
				}

				void CloseNoLock()
				{
					delete m_file;
					m_file = nullptr;
				}

				std::mutex m_mutex;
				FileStream* m_file = nullptr;
				DiaryHeader m_header{};
				PlayDiaryEntry m_entries[NUM_PLAY_DIARY_ENTRIES_MAX]{};
			};

			PlayDiaryStore g_diary;

			/* ---- session tracking thread ---- */

			std::thread g_trackerThread;
			CounterSemaphore g_trackerWake;
			std::atomic_bool g_trackerQuit{ false };

			void TrackerMain(uint64 titleId)
			{
				SetThreadName("PlayDiaryThread");
				const size_t slot = g_stats.BeginSession(titleId);
				const auto start = std::chrono::steady_clock::now();
				uint32 creditedMinutes = 0;
				while (true)
				{
					g_trackerWake.decrementWithWaitAndTimeout(15000);
					if (g_trackerQuit.load(std::memory_order::relaxed))
						break;
					const uint32 minutes = (uint32)std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - start).count();
					if (minutes > creditedMinutes)
					{
						g_stats.AddMinutes(slot, minutes - creditedMinutes);
						creditedMinutes = minutes;
					}
				}
			}

			void LoadAll()
			{
				g_stats.Load();
				g_diary.Load();
			}

			void CloseAll()
			{
				g_stats.Close();
				g_diary.Close();
			}

			void UnloadAll()
			{
				g_stats.Unload();
				g_diary.Unload();
			}

			class PdmModule : public ::IOSUModule
			{
			public:
				void SystemLaunch() override
				{
					// load once, then release the files so the mlc can be changed while idle
					LoadAll();
					CloseAll();
				}

				void SystemExit() override
				{
					CloseAll();
					UnloadAll();
				}

				void TitleStart() override
				{
					// reload from disk and keep the files open for the session
					UnloadAll();
					LoadAll();
					g_trackerQuit = false;
					g_trackerThread = std::thread(TrackerMain, CafeSystem::GetForegroundTitleId());
				}

				void TitleStop() override
				{
					g_trackerQuit.store(true);
					g_trackerWake.increment();
					if (g_trackerThread.joinable())
						g_trackerThread.join();
					CloseAll();
				}
			};

			PdmModule g_module;
		}

		uint32 GetDiaryEntries(uint8 accountSlot, PlayDiaryEntry* diaryEntries, uint32 maxEntries)
		{
			return g_diary.Read(diaryEntries, maxEntries);
		}

		bool GetStatForGamelist(uint64 titleId, GameListStat& stat)
		{
			stat.last_played = {};
			stat.numMinutesPlayed = 0;
			uint32 minutes;
			uint16 lastDay;
			if (g_stats.Lookup(titleId, minutes, lastDay))
			{
				stat.numMinutesPlayed = minutes;
				const chrono_d::year_month_day date = DayNumberToDate(lastDay);
				stat.last_played.year = (int)date.year();
				stat.last_played.month = (unsigned int)date.month() - 1; // zero based month
				stat.last_played.day = (unsigned int)date.day();
			}
			// merge in play time recorded by older versions in the settings file
			std::lock_guard lock(GetConfig().game_cache_entries_mutex);
			for (const auto& entry : GetConfig().game_cache_entries)
			{
				if (entry.title_id != titleId)
					continue;
				stat.numMinutesPlayed += (uint32)(entry.legacy_time_played / 60);
				if (entry.legacy_last_played == 0)
					continue;
				const time_t when = (time_t)entry.legacy_last_played;
				const tm* local = localtime(&when);
				GameListStat::LastPlayDate legacy{ (uint32)local->tm_year + 1900, (uint32)local->tm_mon, (uint32)local->tm_mday };
				if (stat.last_played.year == 0 || stat.last_played < legacy)
					stat.last_played = legacy;
			}
			return true;
		}

		IOSUModule* GetModule()
		{
			return &g_module;
		}

		bool GameListStat::LastPlayDate::operator<(const LastPlayDate& b) const
		{
			return std::tie(year, month, day) < std::tie(b.year, b.month, b.day);
		}

		bool GameListStat::LastPlayDate::operator==(const LastPlayDate& b) const
		{
			return year == b.year && month == b.month && day == b.day;
		}

		std::weak_ordering GameListStat::LastPlayDate::operator<=>(const LastPlayDate& b) const = default;
	}
}
