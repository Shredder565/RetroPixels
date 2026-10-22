// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Emulated Disney Infinity base, driven from the GUI through g_infinitybase.
#pragma once

#include <array>
#include <map>
#include <mutex>
#include <queue>

#include "nsyshid.h"
#include "Backend.h"
#include "BackendEmulated.h"

#include "Common/FileStream.h"

namespace nsyshid
{
	class InfinityBaseDevice final : public EmulatedHidDevice
	{
	public:
		InfinityBaseDevice();

		Status Read(uint8* buffer, uint32 length, sint32& bytesRead) override;
		Status Write(uint8* data, uint32 length, sint32& bytesWritten) override;
	};

	constexpr uint16 INF_BLOCK_COUNT = 0x14;
	constexpr uint16 INF_BLOCK_SIZE = 0x10;
	constexpr uint16 INF_FIGURE_SIZE = INF_BLOCK_COUNT * INF_BLOCK_SIZE;
	constexpr uint8 MAX_FIGURES = 9;

	class InfinityUSB
	{
	public:
		using Report = std::array<uint8, 32>;

		// a command written by the game, and the next report it reads
		void SendCommand(uint8* buf, uint32 length);
		Report GetStatus();

		// GUI: positions 0-2 are the play-set hexagon, 3-5 player one, 6-8 player two
		bool RemoveFigure(uint8 position);
		uint32 LoadFigure(const std::array<uint8, INF_FIGURE_SIZE>& buf, std::unique_ptr<FileStream> file, uint8 position);
		bool CreateFigure(fs::path pathName, uint32 figureNum, uint8 series);
		static std::map<const uint32, const std::pair<const uint8, const char*>> GetFigureList();
		std::pair<uint8, std::string> FindFigure(uint32 figNum);

	private:
		struct Figure
		{
			std::unique_ptr<FileStream> file;
			std::array<uint8, INF_FIGURE_SIZE> data{};
			bool present = false;
			uint8 order = 255; // order of first placement; the game addresses figures by it
			void Flush();
		};

		// pseudo random generator shared with the game for the challenge
		struct Rng
		{
			uint32 a = 0, b = 0, c = 0, d = 0;
			void Seed(uint32 seed);
			uint32 Next();
		};

		Figure& FigureByOrder(uint8 order);
		void ReadBlock(uint8 order, uint8 block, uint8 sequence, Report& reply);
		void WriteBlock(uint8 order, uint8 block, const uint8* data, uint8 sequence, Report& reply);
		void ReadIdentifier(uint8 order, uint8 sequence, Report& reply);
		void ListPresent(uint8 sequence, Report& reply);
		void QueueFigureEvent(uint8 position, uint8 order, bool removed);

		std::mutex m_lock;
		std::array<Figure, MAX_FIGURES> m_figures;
		uint8 m_nextOrder = 0;
		Rng m_rng;
		std::queue<Report> m_figureEvents;
		std::queue<Report> m_replies;
	};
	extern InfinityUSB g_infinitybase;
} // namespace nsyshid
