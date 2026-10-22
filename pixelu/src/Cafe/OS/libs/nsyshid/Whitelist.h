// Original code for RetroPixels (PixelU). Not derived from Cemu.
// USB devices (vendor id, product id) that may be passed through to games.
#pragma once

#include <list>
#include <mutex>
#include <tuple>

namespace nsyshid
{
	class Whitelist
	{
	public:
		static Whitelist& GetInstance();

		Whitelist(const Whitelist&) = delete;
		Whitelist& operator=(const Whitelist&) = delete;

		bool IsDeviceWhitelisted(uint16 vendorId, uint16 productId);
		void AddDevice(uint16 vendorId, uint16 productId);
		void RemoveDevice(uint16 vendorId, uint16 productId);
		std::list<std::tuple<uint16, uint16>> GetDevices();
		void RemoveAllDevices();

	private:
		Whitelist();

		std::mutex m_lock;
		std::list<std::tuple<uint16, uint16>> m_entries;
	};
} // namespace nsyshid
