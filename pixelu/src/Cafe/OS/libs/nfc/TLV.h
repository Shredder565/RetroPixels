// Original code for RetroPixels (PixelU). Not derived from Cemu.
// TLV blocks of an NFC Forum tag data area (type, length, value).
#pragma once

#include <span>
#include <vector>

namespace nfc::tlv
{
	constexpr uint8 kNull = 0x00;       // padding, no length
	constexpr uint8 kLockControl = 0x01;
	constexpr uint8 kMemoryControl = 0x02;
	constexpr uint8 kNdef = 0x03;
	constexpr uint8 kProprietary = 0xFD;
	constexpr uint8 kTerminator = 0xFE; // end of the list, no length

	struct Entry
	{
		uint8 type = kNull;
		std::vector<uint8> value;
	};

	// Splits a data area into its TLVs (NULL padding is skipped, parsing stops
	// after the terminator). Returns false if a length runs past the end.
	bool Parse(std::span<const uint8> area, std::vector<Entry>& entries);

	// Serialises one TLV (1-byte length, or 0xFF + 16-bit length from 0xFF up).
	void Append(const Entry& entry, std::vector<uint8>& out);
}
