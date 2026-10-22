// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Emulated Skylanders "Portal of Power". The figures placed on it are dumps
// managed from the GUI (EmulatedUSBDevices) through g_skyportal.
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
	class SkylanderPortalDevice final : public EmulatedHidDevice
	{
	public:
		SkylanderPortalDevice();

		Status Read(uint8* buffer, uint32 length, sint32& bytesRead) override;
		Status Write(uint8* data, uint32 length, sint32& bytesWritten) override;
		bool SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length) override;
	};

	constexpr uint16 SKY_BLOCK_COUNT = 0x40;
	constexpr uint16 SKY_BLOCK_SIZE = 0x10;
	constexpr uint16 SKY_FIGURE_SIZE = SKY_BLOCK_COUNT * SKY_BLOCK_SIZE;
	constexpr uint8 MAX_SKYLANDERS = 16;

	class SkylanderUSB
	{
	public:
		using Report = std::array<uint8, 64>;

		// a command sent by the game (HID SET_REPORT)
		void ControlTransfer(uint8* buf, uint32 length);
		// the next interrupt report for the game
		Report GetStatus();

		// GUI: place a figure dump on the portal; returns the portal slot or 0xFF
		uint8 LoadSkylander(uint8* buf, std::unique_ptr<FileStream> file);
		bool RemoveSkylander(uint8 skyNum);
		bool CreateSkylander(fs::path pathName, uint16 skyId, uint16 skyVar);
		uint16 SkylanderCRC16(uint16 initValue, const uint8* buffer, uint32 size);
		static std::map<const std::pair<const uint16, const uint16>, const char*> GetListSkylanders();
		std::string FindSkylander(uint16 skyId, uint16 skyVar);

	private:
		// presence states as reported in the status bitfield (2 bits per slot)
		enum : uint8
		{
			SLOT_EMPTY = 0,
			SLOT_PRESENT = 1,
			SLOT_LEAVING = 2,
			SLOT_ARRIVING = 3,
		};

		struct Slot
		{
			std::unique_ptr<FileStream> file;
			std::array<uint8, SKY_FIGURE_SIZE> data{};
			uint8 state = SLOT_EMPTY;
			std::queue<uint8> pendingStates; // reported one per status poll
			uint32 lastSerial = 0;           // a figure put back returns to its old slot

			bool Present() const { return (state & SLOT_PRESENT) != 0; }
			void Flush();
		};

		struct Color
		{
			uint8 r = 0, g = 0, b = 0;
		};

		void Activate();
		void SetLight(uint8 side, uint8 r, uint8 g, uint8 b);
		void ReadBlock(uint8 slot, uint8 block, Report& reply);
		void WriteBlock(uint8 slot, uint8 block, const uint8* data, Report& reply);

		std::mutex m_lock;
		std::array<Slot, MAX_SKYLANDERS> m_slots;
		std::queue<Report> m_replies;
		bool m_active = true;
		uint8 m_counter = 0;
		Color m_lightLeft, m_lightRight, m_lightTrap;
	};
	extern SkylanderUSB g_skyportal;
} // namespace nsyshid
