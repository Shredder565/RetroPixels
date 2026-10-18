// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Emulated LEGO Dimensions toy pad, driven from the GUI through g_dimensionstoypad.
#pragma once

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <span>

#include "nsyshid.h"
#include "Backend.h"
#include "BackendEmulated.h"

#include "Common/FileStream.h"

namespace nsyshid
{
	class DimensionsToypadDevice final : public EmulatedHidDevice
	{
	public:
		DimensionsToypadDevice();

		Status Read(uint8* buffer, uint32 length, sint32& bytesRead) override;
		Status Write(uint8* data, uint32 length, sint32& bytesWritten) override;
	};

	class DimensionsUSB
	{
	public:
		using Report = std::array<uint8, 32>;
		using TagData = std::array<uint8, 0x2D * 0x04>; // 45 NTAG213 pages

		// a command written by the game, and the next report it reads
		void SendCommand(std::span<const uint8, 32> buf);
		Report GetStatus();

		// GUI: a tag on one of the 7 slots (index 0-6) of a pad (1-3)
		bool RemoveFigure(uint8 pad, uint8 index, bool fullRemove);
		bool TempRemove(uint8 index);
		bool CancelRemove(uint8 index);
		uint32 LoadFigure(const TagData& buf, std::unique_ptr<FileStream> file, uint8 pad, uint8 index);
		bool CreateFigure(fs::path pathName, uint32 id);
		bool MoveFigure(uint8 pad, uint8 index, uint8 oldPad, uint8 oldIndex);
		static std::map<const uint32, const char*> GetListMinifigs();
		static std::map<const uint32, const char*> GetListTokens();
		std::string FindFigure(uint32 figNum);

	private:
		struct Tag
		{
			std::unique_ptr<FileStream> file;
			TagData data{};
			uint8 index = 255; // 1-based slot as reported to the game, 255 = empty
			uint8 pad = 255;
			uint32 id = 0;
			bool Present() const { return index != 255; }
			void Flush();
		};

		struct Rng
		{
			uint32 a = 0, b = 0, c = 0, d = 0;
			void Seed(uint32 seed);
			uint32 Next();
		};

		void Seed(std::span<const uint8, 8> payload, uint8 sequence, Report& reply);
		void Challenge(std::span<const uint8, 8> payload, uint8 sequence, Report& reply);
		void ReadPages(uint8 index, uint8 page, uint8 sequence, Report& reply);
		void WritePage(uint8 index, uint8 page, std::span<const uint8, 4> data, uint8 sequence, Report& reply);
		void Model(std::span<const uint8, 8> payload, uint8 sequence, Report& reply);

		uint32 PlaceLocked(const TagData& buf, std::unique_ptr<FileStream> file, uint8 pad, uint8 index);
		bool LiftLocked(uint8 index, bool announce);
		void QueueTagEvent(const Tag& tag, bool removed);

		std::mutex m_lock;
		std::array<Tag, 7> m_tags{};
		Rng m_rng;
		bool m_awake = false;
		std::queue<Report> m_tagEvents;
		std::queue<Report> m_replies;
	};
	extern DimensionsUSB g_dimensionstoypad;
} // namespace nsyshid
