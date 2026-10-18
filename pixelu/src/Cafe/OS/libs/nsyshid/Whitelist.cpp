// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Whitelist.h"

namespace nsyshid
{
	Whitelist& Whitelist::GetInstance()
	{
		static Whitelist instance;
		return instance;
	}

	Whitelist::Whitelist()
		: m_entries{
			  {0x0E6F, 0x0241}, // LEGO Dimensions toy pad
			  {0x1430, 0x0150}, // Skylanders portal
			  {0x1430, 0x1F17}, // Skylanders portal (Xbox 360)
			  {0x0E6F, 0x0129}, // Disney Infinity base
			  {0x0E6F, 0x200A}, // Kamen Rider ride gate
		  }
	{
	}

	bool Whitelist::IsDeviceWhitelisted(uint16 vendorId, uint16 productId)
	{
		std::unique_lock _l(m_lock);
		for (const auto& [vid, pid] : m_entries)
			if (vid == vendorId && pid == productId)
				return true;
		return false;
	}

	void Whitelist::AddDevice(uint16 vendorId, uint16 productId)
	{
		if (IsDeviceWhitelisted(vendorId, productId))
			return;
		std::unique_lock _l(m_lock);
		m_entries.emplace_back(vendorId, productId);
	}

	void Whitelist::RemoveDevice(uint16 vendorId, uint16 productId)
	{
		std::unique_lock _l(m_lock);
		m_entries.remove(std::make_tuple(vendorId, productId));
	}

	std::list<std::tuple<uint16, uint16>> Whitelist::GetDevices()
	{
		std::unique_lock _l(m_lock);
		return m_entries;
	}

	void Whitelist::RemoveAllDevices()
	{
		std::unique_lock _l(m_lock);
		m_entries.clear();
	}
} // namespace nsyshid
